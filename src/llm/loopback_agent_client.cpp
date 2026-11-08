#include "loopback_agent_client.hpp"
#include "json/json_utils.hpp"
#include "logging/logger.h"

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace media::llm::agent::loopback_client {
namespace {

// ── Process-local script state ────────────────────────────────────────────────

struct ScriptState {
    std::string    source;           ///< resolved source string that produced script
    nlohmann::json script;           ///< parsed JSON array of response entries
    std::size_t    index  = 0;       ///< next entry to serve
    bool           loaded = false;   ///< true after first successful load
};

std::mutex& state_mutex()
{
    static std::mutex m;
    return m;
}

ScriptState& script_state()
{
    static ScriptState s;
    return s;
}

// ── Script loading ────────────────────────────────────────────────────────────

/// Read and JSON-parse a script from inline text or a '@<path>' file ref.
/// Returns a JSON array on success; null json on failure (err is populated).
nlohmann::json load_script_json(const std::string& src, std::string& err)
{
    std::string text;
    if (src.size() > 1 && src[0] == '@') {
        const std::string path = src.substr(1);
        std::ifstream f(path);
        if (!f) {
            err = "loopback: cannot open script file: " + path;
            return nullptr;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    } else {
        text = src;
    }

    try {
        auto j = nlohmann::json::parse(text);
        if (!j.is_array()) {
            err = "loopback: script root must be a JSON array";
            return nullptr;
        }
        return j;
    } catch (const std::exception& ex) {
        err = std::string("loopback: JSON parse error: ") + ex.what();
        return nullptr;
    }
}

/// Priority: `arg` (when it looks like a script) → PM_LOOPBACK_SCRIPT env var → empty.
/// The `arg` is the model string from ProviderSettings::model. Only treat it as a
/// script source when it starts with '[' (inline JSON array) or '@' (file reference).
/// Any other value (e.g. a model name like "openai/gpt-5.5" from app settings) is
/// ignored so that PM_LOOPBACK_SCRIPT can be the canonical env-var injection path.
std::string resolve_source(const std::string& arg)
{
    if (!arg.empty() && (arg[0] == '[' || arg[0] == '@'))
        return arg;
    if (const char* env = std::getenv("PM_LOOPBACK_SCRIPT"))
        if (*env) return env;
    return {};
}

// ── Response construction ─────────────────────────────────────────────────────

/// Parse one script entry into an LLMResponse.
/// Entry shape:
///   { "text": "...",
///     "tool_calls": [{"id":"...", "name":"...", "arguments": {...}}] }
polymech::kbot::LLMResponse entry_to_response(const nlohmann::json& entry,
                                              std::size_t round,
                                              std::size_t total)
{
    polymech::kbot::LLMResponse res;
    res.success = true;

    if (entry.contains("tool_calls") && entry["tool_calls"].is_array()) {
        for (const auto& tc : entry["tool_calls"]) {
            if (!tc.is_object()) continue;
            polymech::kbot::ToolCall call;
            if (tc.contains("id") && tc["id"].is_string())
                call.id = tc["id"].get<std::string>();
            // Synthesise a stable id when the script omits it.
            if (call.id.empty())
                call.id = "loop_" + std::to_string(round) + "_" + std::to_string(&tc - &entry["tool_calls"][0]);
            if (tc.contains("name") && tc["name"].is_string())
                call.name = tc["name"].get<std::string>();
            if (tc.contains("arguments")) {
                const auto& a = tc["arguments"];
                if (a.is_object()) {
                    call.arguments = a;
                } else if (a.is_string()) {
                    call.arguments = media::json::parse_or(a.get<std::string>(), nlohmann::json::object());
                } else {
                    call.arguments = nlohmann::json::object();
                }
            }
            res.tool_calls.push_back(std::move(call));
        }
    }

    // Text is surfaced when no tool calls are present (terminal response).
    if (res.tool_calls.empty() && entry.contains("text") && entry["text"].is_string())
        res.text = entry["text"].get<std::string>();

    // Synthetic provider metadata so the agent loop can stamp per-round tokens.
    res.provider_meta_json =
        nlohmann::json{{"model", "loopback"},
                       {"usage", {{"prompt_tokens", 0}, {"completion_tokens", 0},
                                  {"total_tokens", 0}}},
                       {"loopback_round", round},
                       {"loopback_total", total}}.dump();

    return res;
}

} // namespace

// ── Public API ────────────────────────────────────────────────────────────────

bool is_loopback_router(std::string_view router)
{
    return router == "loopback" || router == "mock";
}

polymech::kbot::LLMResponse execute_chat_messages(
    const std::vector<polymech::kbot::ChatMessage>& /*messages*/,
    const std::string& script_source,
    const nlohmann::json& /*tools*/,
    const nlohmann::json& /*tool_choice*/,
    int   timeout_ms,
    std::function<bool()> cancel_check)
{
    // ── Cancellation guard (fast path) ────────────────────────────────────────
    if (cancel_check && cancel_check()) {
        polymech::kbot::LLMResponse res;
        res.success   = false;
        res.cancelled = true;
        res.error     = "interrupted";
        return res;
    }

    const std::string src = resolve_source(script_source);

    std::lock_guard<std::mutex> lock(state_mutex());
    ScriptState& st = script_state();

    // ── Load / reload ─────────────────────────────────────────────────────────
    // Reload whenever the source string changes so that sequential test cases,
    // each supplying a different script, automatically get a fresh index.
    if (!st.loaded || st.source != src) {
        std::string err;
        nlohmann::json parsed;
        if (src.empty()) {
            parsed = nlohmann::json::array();
            logger::debug("loopback: no script source — all rounds return terminal response");
        } else {
            parsed = load_script_json(src, err);
            if (parsed.is_null()) {
                logger::warn(err);
                parsed = nlohmann::json::array();
            }
        }
        st.source = src;
        st.script  = std::move(parsed);
        st.index   = 0;
        st.loaded  = true;
        logger::info("loopback: script loaded, " + std::to_string(st.script.size()) + " entries");
    }

    // ── Honour timeout / cancel ───────────────────────────────────────────────
    // The loopback client itself is instantaneous; we only check cancel_check
    // once more after the lock (the busy path for a long-running test harness).
    if (cancel_check && cancel_check()) {
        polymech::kbot::LLMResponse res;
        res.success   = false;
        res.cancelled = true;
        res.error     = "interrupted";
        return res;
    }
    (void)timeout_ms;  // no actual blocking — see comment above

    // ── Script exhausted ──────────────────────────────────────────────────────
    if (st.index >= st.script.size()) {
        logger::debug("loopback: script exhausted at index " + std::to_string(st.index)
                      + " — returning terminal response");
        polymech::kbot::LLMResponse res;
        res.success = true;   // clean exit; loop will stop on empty tool_calls
        res.text    = "";
        res.provider_meta_json =
            nlohmann::json{{"model", "loopback"},
                           {"usage", {{"prompt_tokens", 0}, {"completion_tokens", 0},
                                      {"total_tokens", 0}}},
                           {"loopback_round",   st.index},
                           {"loopback_total",   st.script.size()},
                           {"loopback_exhausted", true}}.dump();
        return res;
    }

    // ── Serve next entry ──────────────────────────────────────────────────────
    const std::size_t round = st.index;
    const nlohmann::json& entry = st.script[round];
    ++st.index;

    logger::debug("loopback: serving entry " + std::to_string(round + 1)
                  + "/" + std::to_string(st.script.size()));

    return entry_to_response(entry, round, st.script.size());
}

void reset_script()
{
    std::lock_guard<std::mutex> lock(state_mutex());
    script_state() = ScriptState{};
    logger::debug("loopback: script state reset");
}

} // namespace media::llm::agent::loopback_client

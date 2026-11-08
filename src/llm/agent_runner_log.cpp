#include "agent_runner_log.hpp"

#include "agent_internal.hpp"
#include "agent_memory_artifacts.hpp"
#include "chat_session_store.hpp"
#include "string/string_sanitizers.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include "fs/path_sanitizer.hpp"

namespace media::llm::agent {
namespace {

constexpr std::size_t k_default_cli_tool_line_max = 160;

std::string collapse_cli_whitespace(std::string s)
{
    std::string out;
    out.reserve(s.size());
    bool space = false;
    for (char c : s) {
        if (c == '\r' || c == '\n' || c == '\t')
            c = ' ';
        if (c == ' ') {
            if (space) continue;
            space = true;
        } else {
            space = false;
        }
        out += c;
    }
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string truncate_cli_text(std::string s, std::size_t max_chars)
{
    s = collapse_cli_whitespace(std::move(s));
    if (s.size() <= max_chars)
        return s;
    const std::size_t keep = media::str_san::utf8_safe_prefix_len(
        s, max_chars > 3 ? max_chars - 3 : max_chars);
    return s.substr(0, keep) + "...";
}

std::string compact_json_line(const nlohmann::json& j)
{
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

const nlohmann::json* json_ptr_field(const nlohmann::json& obj, const char* key)
{
    if (!obj.is_object() || !obj.contains(key))
        return nullptr;
    return &obj[key];
}

std::string json_string_or_empty(const nlohmann::json& j)
{
    if (j.is_string())
        return j.get<std::string>();
    return {};
}

std::string tool_args_preview(const nlohmann::json& payload, std::size_t max_chars)
{
    nlohmann::json args = nlohmann::json::object();
    if (const nlohmann::json* a = json_ptr_field(payload, "arguments"))
        args = *a;
    else if (payload.is_object()) {
        args = payload;
        args.erase("id");
        args.erase("tool_use_id");
    } else {
        args = payload;
    }

    if (args.is_object()) {
        static const char* k_hint_keys[] = {
            "command", "file_path", "path", "query", "pattern", "description", "prompt", "url",
        };
        for (const char* key : k_hint_keys) {
            if (const nlohmann::json* v = json_ptr_field(args, key)) {
                const std::string val = json_string_or_empty(*v);
                if (!val.empty())
                    return truncate_cli_text(std::string(key) + "=" + val, max_chars);
            }
        }
        if (args.contains("paths") && args["paths"].is_array()) {
            const auto& pa = args["paths"];
            std::string line = std::to_string(pa.size()) + " path(s)";
            if (!pa.empty() && pa[0].is_string())
                line += ": " + pa[0].get<std::string>();
            return truncate_cli_text(line, max_chars);
        }
        if (args.contains("inputs") && args["inputs"].is_array()) {
            const auto& ia = args["inputs"];
            std::string line = std::to_string(ia.size()) + " input(s)";
            if (!ia.empty() && ia[0].is_string())
                line += ": " + ia[0].get<std::string>();
            return truncate_cli_text(line, max_chars);
        }
    }

    if (args.is_string())
        return truncate_cli_text(args.get<std::string>(), max_chars);
    if (args.is_null() || (args.is_object() && args.empty()))
        return {};
    return truncate_cli_text(compact_json_line(args), max_chars);
}

std::string tool_result_preview(const std::string& text,
                                const nlohmann::json& payload,
                                std::size_t max_chars)
{
    if (const nlohmann::json* env = json_ptr_field(payload, "envelope")) {
        std::ostringstream ss;
        if (env->is_object()) {
            if (env->contains("ok"))
                ss << (detail::json_child_bool(*env, "ok") ? "ok" : "fail");
            if (const std::string err = detail::json_child_string(*env, "error"); !err.empty()) {
                if (!ss.str().empty()) ss << ' ';
                ss << err;
            } else if (env->contains("results") && (*env)["results"].is_array()
                       && !(*env)["results"].empty()) {
                const auto& r0 = (*env)["results"][0];
                if (r0.is_object()) {
                    std::string path = detail::json_child_string(r0, "output_path");
                    if (path.empty()) path = detail::json_child_string(r0, "path");
                    if (!path.empty()) {
                        if (!ss.str().empty()) ss << ' ';
                        ss << path;
                    }
                }
            } else if (env->contains("summary") && (*env)["summary"].is_object()) {
                const auto& sum = (*env)["summary"];
                const int total = static_cast<int>(detail::json_child_int64(sum, "total"));
                const int ok_n  = static_cast<int>(detail::json_child_int64(sum, "succeeded"));
                if (total > 0) {
                    if (!ss.str().empty()) ss << ' ';
                    ss << ok_n << '/' << total;
                }
            }
        }
        std::string line = ss.str();
        if (!line.empty())
            return truncate_cli_text(line, max_chars);
    }

    if (!text.empty())
        return truncate_cli_text(text, max_chars);
    if (const nlohmann::json* content = json_ptr_field(payload, "content")) {
        if (content->is_string()) {
            const std::string s = content->get<std::string>();
            if (!s.empty())
                return truncate_cli_text(s, max_chars);
        }
    }

    if (const nlohmann::json* claude = json_ptr_field(payload, "claude")) {
        if (claude->is_object() && detail::json_child_bool(*claude, "is_error"))
            return "error";
    }

    nlohmann::json slim = payload.is_object() ? payload : nlohmann::json::object();
    if (slim.is_object()) {
        slim.erase("claude");
        slim.erase("tool_use_id");
        if (!slim.empty())
            return truncate_cli_text(compact_json_line(slim), max_chars);
    }
    return {};
}

std::string agent_log_utc_now()
{
    std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
#if defined(_WIN32)
    if (gmtime_s(&tm_buf, &t) != 0) return {};
#else
    if (gmtime_r(&t, &tm_buf) == nullptr) return {};
#endif
    char buf[40];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

std::string chat_api_mode_name(LlmApiMode mode)
{
    switch (mode) {
    case LlmApiMode::Responses: return "responses";
    case LlmApiMode::ChatCompletion: return "chat_completions";
    case LlmApiMode::Realtime: return "realtime";
    }
    return "responses";
}

std::string chat_streaming_mode_name(LlmStreamingMode mode)
{
    switch (mode) {
    case LlmStreamingMode::Off: return "off";
    case LlmStreamingMode::On: return "on";
    case LlmStreamingMode::Auto: return "auto";
    }
    return "auto";
}

} // namespace

std::string format_agent_tool_call_cli_line(const std::string& tool_name,
                                            const nlohmann::json& payload,
                                            std::size_t max_chars)
{
    const std::size_t cap = max_chars > 0 ? max_chars : k_default_cli_tool_line_max;
    const std::string args = tool_args_preview(payload, cap);
    if (args.empty())
        return std::string("[tool] ") + tool_name;
    return std::string("[tool] ") + tool_name + " " + args;
}

std::string format_agent_tool_result_cli_line(const std::string& tool_name,
                                              const std::string& text,
                                              const nlohmann::json& payload,
                                              std::size_t max_chars)
{
    const std::size_t cap = max_chars > 0 ? max_chars : k_default_cli_tool_line_max;
    std::ostringstream ss;
    ss << "[tool result] " << tool_name;
    const std::string preview = tool_result_preview(text, payload, cap);
    if (!preview.empty())
        ss << ' ' << preview;
    const long long ms = detail::json_child_int64(payload, "duration_ms");
    if (ms > 0)
        ss << " (" << ms << "ms)";
    return ss.str();
}

std::string format_agent_tool_result_status(const nlohmann::json& payload)
{
    const nlohmann::json* env = json_ptr_field(payload, "envelope");
    if (!env || !env->is_object())
        return " (done)";

    if (env->contains("exit_code") && (*env)["exit_code"].is_number_integer()) {
        const long long exit_code = detail::json_child_int64(*env, "exit_code");
        if (exit_code != 0)
            return " \xE2\x9C\x97 exit " + std::to_string(exit_code);
    }

    if (const nlohmann::json* summary = json_ptr_field(*env, "summary");
        summary && summary->is_object() && summary->contains("total")) {
        const long long total = detail::json_child_int64(*summary, "total");
        const long long ok = detail::json_child_int64(*summary, "succeeded");
        const long long failed = detail::json_child_int64(*summary, "failed");
        std::string status = " \xE2\x9C\x93 " + std::to_string(ok) + "/" + std::to_string(total);
        if (failed > 0)
            status += " (" + std::to_string(failed) + " failed)";
        return status;
    }

    if (const nlohmann::json* results = json_ptr_field(*env, "results");
        results && results->is_array() && !results->empty()) {
        long long ok = 0;
        for (const auto& row : *results)
            if (row.is_object() && detail::json_child_bool(row, "ok"))
                ++ok;
        return " \xE2\x9C\x93 " + std::to_string(ok) + "/" +
            std::to_string(results->size());
    }

    const bool has_count = env->contains("count") &&
        ((*env)["count"].is_number_integer() || (*env)["count"].is_number_unsigned());
    const long long count = has_count ? detail::json_child_int64(*env, "count") : 0;
    if (env->contains("body_found") && (*env)["body_found"].is_boolean()) {
        if ((*env)["body_found"].get<bool>())
            return " \xE2\x9C\x93 body recovered";
        if (count > 0)
            return " \xE2\x9C\x93 " + std::to_string(count) + " found (body unavailable)";
    }
    if (env->contains("payload_found") && (*env)["payload_found"].is_boolean()) {
        if ((*env)["payload_found"].get<bool>())
            return " \xE2\x9C\x93 payload recovered";
        if (env->value("payload_denied", false))
            return " \xE2\x9C\x97 payload denied";
        if (count > 0)
            return " \xE2\x9C\x93 " + std::to_string(count) + " found (payload unavailable)";
    }
    if (has_count)
        return " \xE2\x9C\x93 " + std::to_string(count) + " found";

    if (env->contains("ok") && (*env)["ok"].is_boolean())
        return (*env)["ok"].get<bool>() ? " \xE2\x9C\x93 done" : " \xE2\x9C\x97 failed";
    return " (done)";
}

void append_agent_log_event(nlohmann::json& events, const Event& e)
{
    nlohmann::json j;
    using EK = Event::Kind;
    switch (e.kind) {
    case EK::TurnStarted:      j["kind"] = "turn_started";       j["detail"] = e.payload; break;
    case EK::LlmRound:         j["kind"] = "llm_round";          j["detail"] = e.payload; break;
    case EK::ToolCall:         j["kind"] = "tool_call";          j["tool"]   = e.tool_name; j["detail"] = e.payload; break;
    case EK::ToolResult:       j["kind"] = "tool_result";        j["tool"]   = e.tool_name; j["detail"] = e.payload; break;
    case EK::ToolFileProgress: j["kind"] = "tool_file_progress"; j["tool"]   = e.tool_name; j["detail"] = e.payload; break;
    case EK::Thinking:         j["kind"] = "thinking";           j["text"]   = e.text;   break;
    case EK::TextDelta:        j["kind"] = "text_delta";         j["text"]   = e.text;   break;
    case EK::AssistantText:    j["kind"] = "assistant_text";     j["text"]   = e.text;   break;
    case EK::MemoryUpdate:
        j["kind"] = "memory_update";
        j["detail"] = truncate_memory_update_for_log(e.payload);
        break;
    case EK::ClarificationNeeded: j["kind"] = "clarification_needed"; j["detail"] = e.payload; break;
    case EK::ErrorReflexion:      j["kind"] = "error_reflexion";      j["detail"] = e.payload; break;
    case EK::SelfCheckResult:     j["kind"] = "self_check";            j["detail"] = e.payload; break;
    case EK::Error:               j["kind"] = "error";                 j["text"]   = e.text;    break;
    case EK::Done:                j["kind"] = "done";                  j["text"]   = e.text;    j["detail"] = e.payload; break;
    case EK::ExternalStream:      j["kind"] = "external_stream";       j["text"]   = e.text;    j["detail"] = e.payload; break;
    default:                      j["kind"] = "unknown";               break;
    }
    detail::sanitize_json_utf8_strings(j);
    events.push_back(std::move(j));
}

nlohmann::json build_agent_run_log(const AgentRunLogWriteOptions& opts,
                                   const Turn& turn,
                                   const ProviderSettings& prov,
                                   const nlohmann::json& log_events,
                                   const Result& res)
{
    nlohmann::json result_j{
        {"ok",         res.ok},
        {"final_text", res.final_text},
        {"iterations", res.iterations},
        {"transcript", res.transcript},
    };
    if (res.cancelled) result_j["cancelled"] = true;
    if (!res.error.empty()) result_j["error"] = res.error;
    if (res.llm_usage_aggregate.is_object()) {
        const int tt  = res.llm_usage_aggregate.value("total_tokens", 0);
        const int pr  = res.llm_usage_aggregate.value("prompt_tokens", 0);
        const int co  = res.llm_usage_aggregate.value("completion_tokens", 0);
        const auto rounds = res.llm_usage_aggregate.value("llm_rounds", nlohmann::json::array());
        const bool has_cost = res.llm_usage_aggregate.contains("cost")
                              && res.llm_usage_aggregate["cost"].is_number();
        if ((rounds.is_array() && !rounds.empty()) || tt > 0 || pr > 0 || co > 0 || has_cost)
            result_j["llm_usage"] = res.llm_usage_aggregate;
    }
    detail::sanitize_json_utf8_strings(result_j);

    nlohmann::json sel_j = nlohmann::json::array();
    for (const auto& p : turn.selection) sel_j.push_back(p);
    detail::sanitize_json_utf8_strings(sel_j);

    nlohmann::json open_j = nlohmann::json::array();
    for (const auto& p : turn.open_files) open_j.push_back(p);
    detail::sanitize_json_utf8_strings(open_j);

    nlohmann::json logj{
        {"schema_version", "1"},
        {"generated_at",   agent_log_utc_now()},
        {"source",         opts.source},
        {"agent", {
            {"id",           opts.run_id},
            {"display_name", opts.display_name},
            {"session_id",   opts.agent_session_id},
        }},
        {"provider", {
            {"router",            prov.router},
            {"model",             prov.model},
            {"runner",            opts.runner},
            {"base_url",          prov.base_url.empty()
                ? nlohmann::json(nullptr) : nlohmann::json(prov.base_url)},
            {"timeout_ms",        prov.timeout_ms},
            {"max_iterations",    prov.max_iterations},
            {"api_mode",          chat_api_mode_name(prov.api_mode)},
            {"streaming",         chat_streaming_mode_name(prov.streaming_mode)},
            {"planner_enabled",   turn.planner_enabled},
            {"api_key_configured", !prov.api_key.empty()},
        }},
        {"turn", {
            {"user_prompt",         turn.user_prompt},
            {"selection",           sel_j},
            {"open_files",          open_j},
            {"folder_hint",         turn.folder_hint},
            {"disabled_path_tools", nlohmann::json(turn.disabled_path_tools)},
        }},
        {"events", log_events},
        {"result", std::move(result_j)},
    };
    detail::sanitize_json_utf8_strings(logj);
    return logj;
}

void persist_agent_run_log_json(const AgentRunLogWriteOptions& opts, const nlohmann::json& logj)
{
    const std::string log_session_id = opts.chat_session_id.empty()
        ? opts.agent_session_id
        : opts.chat_session_id;
    const std::string log_id = opts.log_path.empty()
        ? opts.run_id
        : media::path::path_to_utf8(opts.log_path.stem());

    if (chat_sessions::using_secure_storage()) {
        std::string log_err;
        (void)chat_sessions::save_session_log_document(log_session_id, log_id, logj, log_err);
        return;
    }

    if (opts.log_path.empty())
        return;

    std::ofstream f(opts.log_path, std::ios::out | std::ios::trunc);
    if (f)
        f << detail::safe_json_dump(logj, 2) << "\n";
}

void persist_agent_run_log(const AgentRunLogWriteOptions& opts,
                           const Turn& turn,
                           const ProviderSettings& prov,
                           const nlohmann::json& log_events,
                           const Result& res)
{
    persist_agent_run_log_json(opts, build_agent_run_log(opts, turn, prov, log_events, res));
}

} // namespace media::llm::agent

#include "llama_agent_client.hpp"

#include "logging/logger.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include "fs/path_sanitizer.hpp"

#if defined(FEATURE_LLAMA) && FEATURE_LLAMA
#include "core/local_model_catalog.hpp"
#include "lib/llm/llama/TextChatModel.hpp"

namespace media::llm::agent::llama_client {
namespace {

struct CachedModel {
    std::string model_id;
    std::string model_path;
    int         n_gpu_layers = -1;
    int         n_ctx        = 0;
    int         active_calls = 0;
    nlohmann::json last_stats = nlohmann::json::object();
    media::llama::TextChatModel model;
};

std::mutex& cache_mutex()
{
    static std::mutex m;
    return m;
}

std::condition_variable& cache_cv()
{
    static std::condition_variable cv;
    return cv;
}

CachedModel& cache_slot()
{
    static CachedModel slot;
    return slot;
}

std::string dump_json_utf8_safe(const nlohmann::json& j)
{
    try {
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (...) {
        return j.is_array() ? "[]" : "{}";
    }
}

int n_gpu_layers_from_env()
{
    const char* raw = std::getenv("PM_AGENT_LOCAL_NGL");
    if (!raw || !*raw)
        return -1;
    try {
        return std::stoi(raw);
    } catch (...) {
        return -1;
    }
}

// KV window. 16k overflowed after file_read (~17–20k tokens). llama-server
// would reject the same prompt; we size the window instead of shifting KV.
constexpr int k_default_local_ctx = 32768;

int n_ctx_from_env()
{
    const char* raw = std::getenv("PM_AGENT_LOCAL_CTX");
    if (!raw || !*raw)
        return k_default_local_ctx;
    try {
        const int v = std::stoi(raw);
        if (v < 2048) return 2048;
        if (v > 131072) return 131072;
        return v;
    } catch (...) {
        return k_default_local_ctx;
    }
}

// Same idea as Unsloth's UNSLOTH_TOOL_RESULT_MAX_CHARS: cap the tool body
// before it is tokenised. Head+tail so HTML keep doctype and closing tags.
// ~3 chars/token; leave half the window for system prompt + generation.
void cap_tool_message_bodies(nlohmann::json& messages, int n_ctx)
{
    if (!messages.is_array() || n_ctx <= 0)
        return;
    const std::size_t max_chars = static_cast<std::size_t>(std::max(4096, (n_ctx / 2) * 3));
    for (auto& msg : messages) {
        if (!msg.is_object() || msg.value("role", "") != "tool")
            continue;
        if (!msg.contains("content") || !msg["content"].is_string())
            continue;
        std::string& c = msg["content"].get_ref<std::string&>();
        if (c.size() <= max_chars)
            continue;
        const std::size_t keep = max_chars / 2;
        const std::string note = "\n\n[... truncated "
            + std::to_string(c.size() - max_chars)
            + " chars so the local context (" + std::to_string(n_ctx)
            + " tokens) can continue; file_read succeeded ...]\n\n";
        c = c.substr(0, keep) + note + c.substr(c.size() - keep);
    }
}

void release_model_use()
{
    std::lock_guard<std::mutex> lock(cache_mutex());
    CachedModel& slot = cache_slot();
    if (slot.active_calls > 0)
        --slot.active_calls;
    if (slot.active_calls == 0)
        cache_cv().notify_all();
}

struct ModelUse {
    bool active = false;
    ~ModelUse()
    {
        if (active)
            release_model_use();
    }
};

bool model_for_path(const std::string& model_id,
                    const std::string& model_path,
                    media::llama::TextChatModel*& out,
                    int& n_gpu_layers_out,
                    int& n_ctx_out,
                    ModelUse& use,
                    std::function<bool()> cancel_check,
                    std::string& err)
{
    n_gpu_layers_out = n_gpu_layers_from_env();
    n_ctx_out = n_ctx_from_env();
    std::unique_lock<std::mutex> lock(cache_mutex());
    CachedModel& slot = cache_slot();

    // One in-process llama context. Concurrent complete_chat is unsafe, and a
    // hard "busy" error is what aborted the tool loop when AI consent review
    // (or a previous round) still held the cache. Wait it out instead.
    while (slot.active_calls > 0) {
        if (cancel_check && cancel_check()) {
            err = "cancelled";
            out = nullptr;
            return false;
        }
        cache_cv().wait_for(lock, std::chrono::milliseconds(50));
    }

    if (slot.model_path == model_path
        && slot.n_ctx == n_ctx_out
        && slot.n_gpu_layers == n_gpu_layers_out
        && slot.model.loaded()) {
        ++slot.active_calls;
        use.active = true;
        out = &slot.model;
        return true;
    }

    media::llama::TextChatModel fresh;
    if (!fresh.load(model_path, /*threads=*/4, n_ctx_out, n_gpu_layers_out)) {
        err = fresh.error().empty() ? "failed to load local llama model" : fresh.error();
        out = nullptr;
        return false;
    }
    slot.model_id   = model_id;
    slot.model_path = model_path;
    slot.n_gpu_layers = n_gpu_layers_out;
    slot.n_ctx      = n_ctx_out;
    slot.active_calls = 1;
    slot.model      = std::move(fresh);
    use.active = true;
    out = &slot.model;
    return true;
}

void update_model_stats(const std::string& model_path,
                        const media::llama::TextChatResult& out,
                        double completion_tokens_per_second,
                        double total_tokens_per_second)
{
    std::lock_guard<std::mutex> lock(cache_mutex());
    CachedModel& slot = cache_slot();
    if (slot.model_path != model_path || !slot.model.loaded())
        return;
    slot.last_stats = nlohmann::json{
        {"ok", out.ok},
        {"durationMs", out.elapsed_ms},
        {"promptEvalMs", out.prompt_eval_ms},
        {"generationMs", out.generation_ms},
        {"promptTokens", out.prompt_tokens},
        {"completionTokens", out.completion_tokens},
        {"tokensPerSecond", completion_tokens_per_second},
        {"totalTokensPerSecond", total_tokens_per_second},
    };
}

// kbot ChatMessage[] -> OpenAI-style JSON messages for common_chat_msgs_parse_oaicompat.
nlohmann::json messages_to_oai(const std::vector<polymech::kbot::ChatMessage>& messages)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : messages) {
        nlohmann::json msg{{"role", m.role}};
        if (m.role == "assistant" && m.tool_calls.is_array() && !m.tool_calls.empty()) {
            // tool_calls already carry OpenAI shape (function.arguments as string).
            msg["content"]    = nullptr;
            msg["tool_calls"] = m.tool_calls;
        } else if (m.role == "tool") {
            msg["content"]      = m.content;
            msg["tool_call_id"] = m.tool_call_id;
        } else {
            msg["content"] = m.content;
        }
        arr.push_back(std::move(msg));
    }
    return arr;
}

std::string tool_choice_to_string(const nlohmann::json& tool_choice)
{
    if (tool_choice.is_string())
        return tool_choice.get<std::string>();
    return "auto";
}

} // namespace

bool is_llama_router(std::string_view router)
{
    return router == "llama";
}

polymech::kbot::LLMResponse execute_chat_messages(
    const std::vector<polymech::kbot::ChatMessage>& messages,
    const std::string& model_id_or_path,
    const nlohmann::json& tools,
    const nlohmann::json& tool_choice,
    int timeout_ms,
    std::function<bool()> cancel_check)
{
    polymech::kbot::LLMResponse res;
    res.success = false;

    media::local_models::LocalModelEntry entry;
    std::string resolve_err;
    if (!media::local_models::resolve_llama_text_model(model_id_or_path, entry, resolve_err)) {
        res.error = resolve_err;
        return res;
    }

    const std::string model_path = media::path::path_to_utf8(entry.model_path);
    logger::debug("llama agent: local inference model=" + model_path);

    int n_gpu_layers = -1;
    int n_ctx = k_default_local_ctx;
    std::string load_err;
    media::llama::TextChatModel* model = nullptr;
    ModelUse model_use;
    if (!model_for_path(entry.id, model_path, model, n_gpu_layers, n_ctx, model_use,
                        cancel_check, load_err) || !model) {
        res.error = load_err.empty() ? "failed to load local llama model" : load_err;
        if (load_err == "cancelled" || (cancel_check && cancel_check()))
            res.cancelled = true;
        return res;
    }

    nlohmann::json messages_oai = messages_to_oai(messages);
    cap_tool_message_bodies(messages_oai, n_ctx);
    const std::string choice = tool_choice_to_string(tool_choice);
    const std::string tools_json = (tools.is_array() && !tools.empty())
        ? dump_json_utf8_safe(tools)
        : std::string();

    // Compose the caller's cancel predicate with an optional timeout deadline.
    using Clock    = std::chrono::steady_clock;
    using OptTP    = std::optional<Clock::time_point>;
    const OptTP deadline = (timeout_ms > 0)
        ? OptTP(Clock::now() + std::chrono::milliseconds(timeout_ms))
        : OptTP{};

    auto effective_cancel = [&cancel_check, &deadline]() -> bool {
        if (cancel_check && cancel_check()) return true;
        if (deadline && Clock::now() >= *deadline) return true;
        return false;
    };

    const auto out = model->complete_chat(
        dump_json_utf8_safe(messages_oai), tools_json, choice,
                                          /*n_predict=*/1024, effective_cancel);
    const double generation_seconds = out.generation_ms > 0.f
        ? static_cast<double>(out.generation_ms) / 1000.0
        : 0.0;
    const double completion_tokens_per_second = generation_seconds > 0.0
        ? static_cast<double>(out.completion_tokens) / generation_seconds
        : 0.0;
    const double total_seconds = out.elapsed_ms > 0.f
        ? static_cast<double>(out.elapsed_ms) / 1000.0
        : 0.0;
    const double total_tokens_per_second = total_seconds > 0.0
        ? static_cast<double>(out.prompt_tokens + out.completion_tokens) / total_seconds
        : 0.0;
    update_model_stats(model_path, out, completion_tokens_per_second, total_tokens_per_second);

    if (!out.ok) {
        res.error = out.error.empty() ? "local llama inference failed" : out.error;
        if (res.error.find("exceeds context") != std::string::npos)
            res.error += ". File tools may have succeeded; raise PM_AGENT_LOCAL_CTX "
                         "or read a smaller slice.";
        return res;
    }

    res.text           = out.text;
    res.reasoning_text = out.reasoning;
    int call_index = 0;
    for (const auto& tc : out.tool_calls) {
        polymech::kbot::ToolCall call;
        call.id   = tc.id.empty() ? ("call_" + std::to_string(call_index)) : tc.id;
        call.name = tc.name;
        try {
            call.arguments = tc.arguments.empty()
                ? nlohmann::json::object()
                : nlohmann::json::parse(tc.arguments);
        } catch (...) {
            call.arguments = nlohmann::json::object();
        }
        res.tool_calls.push_back(std::move(call));
        ++call_index;
    }

    res.success = true;

    res.provider_meta_json = nlohmann::json{
        {"provider", "llama.cpp"},
        {"model", entry.id},
        {"model_path", model_path},
        {"duration_ms", out.elapsed_ms},
        {"prompt_eval_ms", out.prompt_eval_ms},
        {"generation_ms", out.generation_ms},
        {"usage", {
            {"prompt_tokens", out.prompt_tokens},
            {"completion_tokens", out.completion_tokens},
            {"total_tokens", out.prompt_tokens + out.completion_tokens},
            {"tokens_per_second", completion_tokens_per_second},
        }},
        {"prompt_tokens", out.prompt_tokens},
        {"completion_tokens", out.completion_tokens},
        {"tokens_per_second", completion_tokens_per_second},
        {"total_tokens_per_second", total_tokens_per_second},
        {"n_ctx", n_ctx},
        {"n_gpu_layers", n_gpu_layers},
        {"gpu_mode", n_gpu_layers == 0 ? "cpu" : (n_gpu_layers < 0 ? "gpu-all" : "gpu-partial")},
    }.dump();
    return res;
}

nlohmann::json loaded_models_json()
{
    std::lock_guard<std::mutex> lock(cache_mutex());
    const CachedModel& slot = cache_slot();
    if (!slot.model.loaded())
        return nlohmann::json::array();
    nlohmann::json row = {
        {"key", "llama:text"},
        {"kind", "llama-text"},
        {"name", slot.model_id.empty() ? media::path::path_to_utf8(media::path::path_from_utf8(slot.model_path).filename()) : slot.model_id},
        {"path", slot.model_path},
        {"status", slot.active_calls > 0 ? "busy" : "loaded"},
        {"activeCalls", slot.active_calls},
        {"nCtx", slot.n_ctx},
        {"nGpuLayers", slot.n_gpu_layers},
        {"stopSupported", true},
    };
    if (!slot.last_stats.empty()) {
        for (auto it = slot.last_stats.begin(); it != slot.last_stats.end(); ++it)
            row[it.key()] = it.value();
    }
    return nlohmann::json::array({std::move(row)});
}

bool unload_loaded_model(std::string_view key, std::string& err)
{
    if (!key.empty() && key != "llama:text") {
        err = "not a llama text model key";
        return false;
    }
    std::lock_guard<std::mutex> lock(cache_mutex());
    CachedModel& slot = cache_slot();
    if (!slot.model.loaded()) {
        err = "no cached llama text model loaded";
        return false;
    }
    if (slot.active_calls > 0) {
        err = "cached llama text model is busy";
        return false;
    }
    slot = CachedModel{};
    return true;
}

} // namespace media::llm::agent::llama_client

#else

namespace media::llm::agent::llama_client {

bool is_llama_router(std::string_view router)
{
    return router == "llama";
}

polymech::kbot::LLMResponse execute_chat_messages(
    const std::vector<polymech::kbot::ChatMessage>&,
    const std::string&,
    const nlohmann::json&,
    const nlohmann::json&,
    int,
    std::function<bool()>)
{
    polymech::kbot::LLMResponse res;
    res.success = false;
    res.error = "llama router requires FEATURE_LLAMA in this build";
    return res;
}

nlohmann::json loaded_models_json()
{
    return nlohmann::json::array();
}

bool unload_loaded_model(std::string_view, std::string& err)
{
    err = "llama router requires FEATURE_LLAMA in this build";
    return false;
}

} // namespace media::llm::agent::llama_client

#endif

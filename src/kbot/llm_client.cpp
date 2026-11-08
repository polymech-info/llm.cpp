#include "llm_client.h"
#include "logging/logger.h"
#include <curl/curl.h>
#include <curl/websockets.h>
#include "curl_ca_android.hpp"
#include <liboai.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#if defined(__ANDROID__)
#include <android/log.h>
#endif
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_set>

namespace polymech {
namespace kbot {

namespace {

thread_local const std::function<bool()>* g_active_llm_cancel_check = nullptr;

void android_llm_line(const std::string& line)
{
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_ERROR, "pm-chat", "%s", line.c_str());
#else
    (void)line;
#endif
}

std::string one_line_preview(std::string s, std::size_t cap)
{
    for (char& c : s) {
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    }
    if (s.size() > cap)
        s.resize(cap);
    return s;
}

std::string dump_json_for_diagnostics(const nlohmann::json& j, int indent);
std::string dump_json_utf8_safe(nlohmann::json& j, const char* tag);
bool looks_cancelled_by_http_callback(const std::string& what);

struct ScopedLlmCancelCheck {
    explicit ScopedLlmCancelCheck(const std::function<bool()>* fn)
        : prev(g_active_llm_cancel_check)
    {
        g_active_llm_cancel_check = fn;
    }
    ~ScopedLlmCancelCheck()
    {
        g_active_llm_cancel_check = prev;
    }
    const std::function<bool()>* prev = nullptr;
};

bool active_llm_cancel_requested()
{
    if (!g_active_llm_cancel_check || !*g_active_llm_cancel_check)
        return false;
    try {
        return (*g_active_llm_cancel_check)();
    } catch (...) {
        return false;
    }
}

// liboai::Authorization is a process singleton (Authorizer()). Concurrent
// ChatCompletion / netimpl::Get from llm agent each --concurrency N heap-corrupts
// on Windows (STATUS_HEAP_CORRUPTION 0xC0000374 / 3221226356).
std::mutex& liboai_http_mu()
{
    static std::mutex m;
    return m;
}

int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

std::string b64url_decode(std::string in)
{
    while (!in.empty() && in.back() == '=') in.pop_back();
    std::string out;
    out.reserve(in.size() * 3 / 4);
    int val = 0;
    int valb = -8;
    for (unsigned char c : in) {
        const int d = b64_value(static_cast<char>(c));
        if (d < 0) continue;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::string chatgpt_account_id_from_jwt(const std::string& jwt)
{
    const size_t dot1 = jwt.find('.');
    if (dot1 == std::string::npos) return {};
    const size_t dot2 = jwt.find('.', dot1 + 1);
    if (dot2 == std::string::npos || dot2 <= dot1 + 1) return {};
    const std::string payload = b64url_decode(jwt.substr(dot1 + 1, dot2 - dot1 - 1));
    if (payload.empty()) return {};
    const nlohmann::json j = nlohmann::json::parse(payload, nullptr, false);
    if (!j.is_object()) return {};
    const auto auth = j.find("https://api.openai.com/auth");
    if (auth != j.end() && auth->is_object()) {
        const auto id = auth->find("chatgpt_account_id");
        if (id != auth->end() && id->is_string()) return id->get<std::string>();
    }
    const auto top = j.find("chatgpt_account_id");
    if (top != j.end() && top->is_string()) return top->get<std::string>();
    return {};
}

std::string openai_auth_trace(const std::string& api_key)
{
    const char* kind = "other";
    if (api_key.rfind("sk-", 0) == 0) kind = "sk";
    else if (api_key.rfind("eyJ", 0) == 0) kind = "jwt";
    else if (api_key.empty()) kind = "empty";
    const bool account = std::string(kind) == "jwt" && !chatgpt_account_id_from_jwt(api_key).empty();
    return std::string(" auth=") + kind
        + " len=" + std::to_string(api_key.size())
        + " account=" + (account ? "set" : "absent");
}

bool apply_openai_auth(liboai::OpenAI& oai, const std::string& api_key)
{
    if (!oai.auth.SetKey(api_key))
        return false;
    if (api_key.rfind("eyJ", 0) != 0)
        return true;
    const std::string id = chatgpt_account_id_from_jwt(api_key);
    if (!id.empty())
        oai.auth.SetHeader("ChatGPT-Account-ID", id);
    logger::debug(std::string("LLMClient: ") + openai_auth_trace(api_key));
    return true;
}

// ChatGPT plan sign-in (scope chatgpt.tokens.use.direct). Not an sk- key, and
// not every JWT: Zitadel access tokens also start with "eyJ". OpenAI admits a
// plan token only on POST /v1/responses with store=false and stream=true.
bool chatgpt_plan_token(const std::string& api_key)
{
    return api_key.rfind("eyJ", 0) == 0 && !chatgpt_account_id_from_jwt(api_key).empty();
}

// Sign-in-with-ChatGPT rejects chat completions and a stored Responses
// conversation. Tools go on an additional_tools input item so function names
// stay bare; a namespace would prefix them and break local dispatch.
void shape_chatgpt_plan_request(nlohmann::json& request)
{
    request["store"] = false;
    static const char* kDrop[] = {
        "background", "conversation", "max_output_tokens", "max_tool_calls",
        "metadata", "moderation", "multi_agent", "prompt", "prompt_cache_retention",
        "safety_identifier", "temperature", "top_logprobs", "top_p", "truncation",
        "user", "previous_response_id", "include",
    };
    for (const char* key : kDrop)
        request.erase(key);
    if (request.contains("tools") && request["tools"].is_array() && !request["tools"].empty()) {
        nlohmann::json extra = {
            {"type", "additional_tools"},
            {"role", "developer"},
            {"tools", std::move(request["tools"])},
        };
        request.erase("tools");
        if (!request["input"].is_array())
            request["input"] = nlohmann::json::array();
        request["input"].insert(request["input"].begin(), std::move(extra));
    }
    logger::debug("LLMClient: ChatGPT plan token -> /v1/responses store=false stream=true");
}

} // namespace

extern "C" int polymech_kbot_llm_cancel_check()
{
    return active_llm_cancel_requested() ? 1 : 0;
}

std::string compose_user_llm_error(const std::string& what,
                                   long http_status,
                                   const std::string& body,
                                   const std::string& model);

/** OpenRouter bills against a completion ceiling; omitting `max_tokens` lets many models default to ~64k,
 *  which can exceed the balance check ("need more credits or fewer max_tokens") even with credits left.
 *  For chat we still keep a sane ceiling; for tool-agent loops we use the max OpenRouter accepts so that
 *  large `app_batch` payloads (20-30+ keystroke steps ≈ ~1500-2500 output tokens of JSON) are never
 *  truncated mid-arguments — truncation would arrive as `{}` and cause the agent to retry the same plan
 *  in a loop. uint16_t maxes at 65535 which is well over what any model emits in one response. */
static std::optional<uint16_t> max_tokens_cap_for_router(std::string_view router, bool tool_agent_loop)
{
    if (router != "openrouter")
        return std::nullopt;
    if (tool_agent_loop)
        return static_cast<uint16_t>(65535);
    return static_cast<uint16_t>(16384);
}

/** Rough size of what OpenRouter counts as input (not just the user's last sentence): messages plus the
 *  entire `tools` JSON. MCP + path tools often contribute hundreds of KB of schema text. */
static size_t approximate_openrouter_input_chars(
    const std::vector<ChatMessage>& messages,
    const nlohmann::json& tools)
{
    size_t n = 0;
    for (const auto& m : messages) {
        n += m.role.size() + m.content.size() + m.tool_call_id.size();
        if (!m.tool_calls.empty())
            n += m.tool_calls.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).size();
    }
    if (tools.is_array() && !tools.empty())
        n += tools.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).size();
    return n;
}

/** OpenRouter preflight: reserved completion tokens must fit under a balance-derived ceiling once input
 *  (including tool definitions) is accounted for. Shrink max_tokens when the serialized payload is large.
 *
 *  Tool-agent loops do NOT shrink — they need full headroom for multi-step batch payloads. Any cap below
 *  the actual response size truncates assistant arguments to `{}` and breaks tool calls. The chat path
 *  (no tools) keeps the bucket table since most replies are short and balance checks still apply. */
static uint16_t openrouter_shrink_max_tokens_for_payload(uint16_t base_cap,
                                                         size_t   approx_chars,
                                                         bool     tool_agent_loop)
{
    if (tool_agent_loop)
        return base_cap;                      // no shrinking for tool loops; let the model emit freely
    uint16_t cap = base_cap;
    /* Parenthesize std::min / std::max so Windows headers cannot break them with min/max macros. */
    if (approx_chars > 180000)
        cap = (std::min)(cap, static_cast<uint16_t>(384));
    else if (approx_chars > 110000)
        cap = (std::min)(cap, static_cast<uint16_t>(512));
    else if (approx_chars > 70000)
        cap = (std::min)(cap, static_cast<uint16_t>(768));
    else if (approx_chars > 42000)
        cap = (std::min)(cap, static_cast<uint16_t>(1024));
    else if (approx_chars > 24000)
        cap = (std::min)(cap, static_cast<uint16_t>(2048));
    else if (approx_chars > 12000)
        cap = (std::min)(cap, static_cast<uint16_t>(4096));
    return (std::max)(static_cast<uint16_t>(256), cap);
}

/** Backwards-compatible overload for non-tool-loop callers. */
static uint16_t openrouter_shrink_max_tokens_for_payload(uint16_t base_cap, size_t approx_chars)
{
    return openrouter_shrink_max_tokens_for_payload(base_cap, approx_chars, false);
}

static std::string realtime_base_url_for(const std::string& base_url) {
    if (base_url.empty())
        return "wss://api.openai.com/v1/realtime";
    std::string u = base_url;
    // Convert http(s) scheme to ws(s).
    const auto scheme_pos = u.find("://");
    if (scheme_pos != std::string::npos) {
        const std::string scheme = u.substr(0, scheme_pos);
        if (scheme == "https") u.replace(0, scheme_pos, "wss");
        else if (scheme == "http") u.replace(0, scheme_pos, "ws");
    }
    while (!u.empty() && u.back() == '/') u.pop_back();
    // OpenAI native: wss://api.openai.com/v1 → .../v1/realtime.
    // LiteLLM proxy:  wss://host:4000/v1    → .../realtime  (strip /v1 first).
    // Bare host:      wss://host:4000        → .../realtime.
    if (u.size() >= 3 && u.substr(u.size() - 3) == "/v1") {
        // Check if this looks like an OpenAI-owned host; if not, strip /v1.
        const bool is_openai_host = (u.find("api.openai.com") != std::string::npos);
        if (!is_openai_host)
            u.resize(u.size() - 3);
    }
    if (u.size() < 9 || u.substr(u.size() - 9) != "/realtime")
        u += "/realtime";
    return u;
}

struct RealtimePromptParts {
    std::string system;
    std::string user;
};

static RealtimePromptParts derive_realtime_prompt_parts(const std::vector<ChatMessage>& messages) {
    RealtimePromptParts out;
    std::string latest_user;
    std::string latest_system;
    for (const auto& m : messages) {
        if (m.role == "system" && !m.content.empty())
            latest_system = m.content;
        else if (m.role == "user" && !m.content.empty())
            latest_user = m.content;
    }
    out.system = std::move(latest_system);
    out.user = std::move(latest_user);
    return out;
}

void warmup_llm_http_async()
{
    // Detached GET /models used to race ChatCompletion on liboai's process-wide
    // Authorization singleton (Windows heap corruption) and, once mutexed, could
    // stall the first execute_chat for the whole Timeout if those URLs hang.
    // First chat pays DNS/TLS; do not background-GET.
}

LLMClient::LLMClient(const KBotOptions& opts)
    : api_key_(opts.api_key),
      model_(opts.model),
      router_(opts.router),
      llm_timeout_ms_(opts.llm_timeout_ms),
      response_format_json_(opts.response_format_json),
      cancel_check_(opts.cancel_check) {
    
    // Set default base_url_ according to client.ts mappings
    if (opts.base_url.empty()) {
        if (router_ == "openrouter")      base_url_ = "https://openrouter.ai/api/v1";
        else if (router_ == "openai")     base_url_ = ""; // liboai uses the default URL automatically
        else if (router_ == "deepseek")   base_url_ = "https://api.deepseek.com/v1";
        else if (router_ == "huggingface")base_url_ = "https://api-inference.huggingface.co/v1";
        else if (router_ == "ollama")     base_url_ = "http://localhost:11434/v1";
        else if (router_ == "fireworks")  base_url_ = "https://api.fireworks.ai/v1";
        else if (router_ == "gemini")     base_url_ = "https://generativelanguage.googleapis.com/v1beta"; // or gemini openai compat endpt
        else if (router_ == "xai")        base_url_ = "https://api.x.ai/v1";
        else                              base_url_ = "https://api.openai.com/v1"; // Fallback to openai API
    } else {
        base_url_ = opts.base_url;
    }

    // Default models based on router (from client.ts)
    if (model_.empty()) {
        if (router_ == "openrouter")       model_ = "anthropic/claude-sonnet-4";
        else if (router_ == "openai")      model_ = "gpt-4o";
        else if (router_ == "deepseek")    model_ = "deepseek-chat";
        else if (router_ == "huggingface") model_ = "meta-llama/2";
        else if (router_ == "ollama")      model_ = "llama3.2";
        else if (router_ == "fireworks")   model_ = "llama-v2-70b-chat";
        else if (router_ == "gemini")      model_ = "gemini-1.5-pro";
        else if (router_ == "xai")         model_ = "grok-1";
        else if (router_ == "custom")      model_ = "";
        else                               model_ = "gpt-4o";
    }

    warmup_llm_http_async();
}

LLMClient::~LLMClient() = default;

LLMResponse LLMClient::execute_chat(const std::string& prompt) {
    LLMResponse res;
    
    logger::debug("LLMClient::execute_chat: Starting. api_key length: " + std::to_string(api_key_.length()));
    if (api_key_.empty()) {
        res.success = false;
        res.error = "API Key is empty.";
        return res;
    }
    if (chatgpt_plan_token(api_key_)) {
        std::vector<ChatMessage> messages;
        messages.push_back({"user", prompt, {}, nlohmann::json()});
        return execute_chat_messages(messages);
    }

    std::lock_guard<std::mutex> http_lk(liboai_http_mu());
    logger::debug("LLMClient::execute_chat: base_url_: " + base_url_);
    liboai::OpenAI oai_impl(base_url_.empty() ? "https://api.openai.com/v1" : base_url_);

    logger::debug("LLMClient::execute_chat: Setting API Key");
    bool success = apply_openai_auth(oai_impl, api_key_);
    if (!success) {
        res.success = false;
        res.error = "Failed to set API Key in liboai.";
        return res;
    }

    if (llm_timeout_ms_ > 0) {
        oai_impl.auth.SetMaxTimeout(llm_timeout_ms_);
        logger::trace("LLMClient: HTTP timeout set to " + std::to_string(llm_timeout_ms_) + " ms");
    }

    std::string target_model = model_.empty() ? "gpt-4o" : model_;
    logger::debug("LLMClient::execute_chat: Target model: " + target_model);

    logger::trace("LLMClient: calling ChatCompletion (prompt chars=" + std::to_string(prompt.size()) + ")");
    logger::debug("LLMClient::execute_chat: Init Conversation");
    liboai::Conversation convo;
    if (!convo.AddUserData(prompt)) {
        res.success = false;
        res.error = "Failed to add user prompt to conversation.";
        return res;
    }

    std::optional<nlohmann::json> response_format;
    if (!response_format_json_.empty()) {
        try {
            response_format = nlohmann::json::parse(response_format_json_);
        } catch (const std::exception& e) {
            logger::warn("LLMClient: invalid --response-format / response_format_json, ignoring: " +
                         std::string(e.what()));
        }
    }

    logger::debug("LLMClient::execute_chat: Calling create()");
    const std::optional<uint16_t> max_out = max_tokens_cap_for_router(router_, false);
    try {
        liboai::Response response = oai_impl.ChatCompletion->create(
            target_model,
            convo,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            max_out,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            response_format);
        logger::trace("LLMClient: ChatCompletion returned (HTTP " + std::to_string(response.status_code) + ")");
        logger::debug("LLMClient::execute_chat: Got response with status: " + std::to_string(response.status_code));

        // liboai may not populate raw_json for custom base URLs — parse content directly.
        nlohmann::json j;
        bool json_ok = false;
        if (!response.raw_json.empty() && response.raw_json.contains("choices")) {
            j = response.raw_json;
            json_ok = true;
        } else if (!response.content.empty()) {
            try {
                j = nlohmann::json::parse(response.content);
                json_ok = j.contains("choices");
            } catch (...) {}
        }

        if (!json_ok || j["choices"].empty()) {
            res.success = false;
            if (json_ok && j.contains("error")) {
                res.error = "API Error: " + j["error"].dump();
            } else {
                res.error = "Invalid response format: no choices found. Raw: " + response.content;
            }
            return res;
        }

        res.success = true;
        res.text = j["choices"][0]["message"]["content"].get<std::string>();

        /* Usage, model, cost (OpenRouter), etc. — everything except message bodies in choices. */
        try {
            nlohmann::json meta = nlohmann::json::object();
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.key() == "choices") continue;
                meta[it.key()] = it.value();
            }
            if (!meta.empty()) res.provider_meta_json = meta.dump();
        } catch (...) {
            /* keep text; omit provider_meta_json */
        }

    } catch (std::exception& e) {
        logger::error("LLMClient::execute_chat: Exception caught: " + std::string(e.what()));
        res.success = false;
        res.error = e.what();
    } catch (...) {
        logger::error("LLMClient::execute_chat: Unknown exception caught");
        res.success = false;
        res.error = "Unknown error occurred inside LLMClient execute_chat.";
    }

    return res;
}

// ── chat-completions SSE (stream=true) ─────────────────────────────────────

struct ChatSseAcc {
    std::string buf;
    std::string raw;
    std::string text;
    std::string reasoning;
    std::string finish_reason;
    std::string id;
    std::string model;
    std::string error;
    bool cancelled = false;
    bool saw_sse = false;
    nlohmann::json usage;
    struct Tool {
        std::string id;
        std::string name;
        std::string arguments;
    };
    std::map<int, Tool> tools;
};

bool chat_sse_apply_json(const nlohmann::json& j,
                         ChatSseAcc& acc,
                         const std::function<bool(const std::string&)>& on_text_delta)
{
    if (!j.is_object()) return true;
    if (j.contains("error") && !j["error"].is_null()) {
        const auto& err = j["error"];
        if (err.is_string())
            acc.error = err.get<std::string>();
        else if (err.is_object() && err.contains("message") && err["message"].is_string())
            acc.error = err["message"].get<std::string>();
        else
            acc.error = err.dump();
        return false;
    }
    if (acc.id.empty() && j.contains("id") && j["id"].is_string())
        acc.id = j["id"].get<std::string>();
    if (j.contains("model") && j["model"].is_string())
        acc.model = j["model"].get<std::string>();
    if (j.contains("usage") && j["usage"].is_object() && !j["usage"].empty())
        acc.usage = j["usage"];
    if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()
            || !j["choices"][0].is_object())
        return true;
    const auto& choice = j["choices"][0];
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string())
        acc.finish_reason = choice["finish_reason"].get<std::string>();
    if (!choice.contains("delta") || !choice["delta"].is_object())
        return true;
    const auto& delta = choice["delta"];

    auto take_string = [](const nlohmann::json& node, const char* key) -> std::string {
        if (!node.contains(key) || !node[key].is_string()) return {};
        return node[key].get<std::string>();
    };
    const std::string reasoning = [&]() {
        const std::string a = take_string(delta, "reasoning_content");
        if (!a.empty()) return a;
        return take_string(delta, "reasoning");
    }();
    if (!reasoning.empty())
        acc.reasoning += reasoning;

    const std::string content = take_string(delta, "content");
    if (!content.empty()) {
        acc.text += content;
        if (on_text_delta && !on_text_delta(content)) {
            acc.cancelled = true;
            return false;
        }
    }

    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
        int fallback = 0;
        for (const auto& tc : delta["tool_calls"]) {
            if (!tc.is_object()) { ++fallback; continue; }
            int index = fallback;
            if (tc.contains("index") && tc["index"].is_number_integer())
                index = tc["index"].get<int>();
            ++fallback;
            auto& slot = acc.tools[index];
            if (tc.contains("id") && tc["id"].is_string()) {
                const std::string id = tc["id"].get<std::string>();
                if (!id.empty()) slot.id = id;
            }
            if (!tc.contains("function") || !tc["function"].is_object())
                continue;
            const auto& fn = tc["function"];
            if (fn.contains("name") && fn["name"].is_string()) {
                const std::string name = fn["name"].get<std::string>();
                if (!name.empty()) {
                    if (slot.name.empty()) slot.name = name;
                    else if (name != slot.name) slot.name += name;
                }
            }
            if (fn.contains("arguments")) {
                if (fn["arguments"].is_string())
                    slot.arguments += fn["arguments"].get<std::string>();
                else if (slot.arguments.empty() && !fn["arguments"].is_null())
                    slot.arguments = fn["arguments"].dump();
            }
        }
    }
    return true;
}

bool chat_sse_drain(ChatSseAcc& acc,
                    const std::function<bool(const std::string&)>& on_text_delta)
{
    for (;;) {
        const size_t sep = acc.buf.find("\n\n");
        if (sep == std::string::npos) break;
        const std::string evt = acc.buf.substr(0, sep);
        acc.buf.erase(0, sep + 2);
        std::string data;
        size_t line_start = 0;
        while (line_start <= evt.size()) {
            size_t line_end = evt.find('\n', line_start);
            if (line_end == std::string::npos) line_end = evt.size();
            const std::string line = evt.substr(line_start, line_end - line_start);
            if (line.rfind("data:", 0) == 0) {
                acc.saw_sse = true;
                std::string payload = line.size() > 5 ? line.substr(5) : std::string();
                if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
                if (!data.empty()) data.push_back('\n');
                data += payload;
            }
            if (line_end == evt.size()) break;
            line_start = line_end + 1;
        }
        if (data.empty() || data == "[DONE]") continue;
        const auto parsed = nlohmann::json::parse(data, nullptr, false);
        if (parsed.is_discarded()) continue;
        if (!chat_sse_apply_json(parsed, acc, on_text_delta))
            return false;
    }
    return true;
}

bool chat_sse_ingest(const std::string& chunk,
                     ChatSseAcc& acc,
                     const std::function<bool(const std::string&)>& on_text_delta)
{
    acc.raw.append(chunk);
    for (char ch : chunk) {
        if (ch != '\r') acc.buf.push_back(ch);
    }
    if (!chat_sse_drain(acc, on_text_delta))
        return false;
    // A streaming write callback replaces the normal response body. Proxies
    // answer HTTP 400 with a JSON document, not `data:` frames, so the SSE
    // drain never sees it and CheckResponse reports an empty Bad Request.
    if (acc.error.empty() && acc.raw.find("data:") == std::string::npos) {
        const auto parsed = nlohmann::json::parse(acc.raw, nullptr, false);
        if (!parsed.is_discarded())
            (void)chat_sse_apply_json(parsed, acc, on_text_delta);
    }
    return acc.error.empty() && !acc.cancelled;
}

void chat_sse_finish(ChatSseAcc& acc,
                     const std::function<bool(const std::string&)>& on_text_delta)
{
    if (acc.buf.find("data:") == std::string::npos) return;
    acc.buf.append("\n\n");
    (void)chat_sse_drain(acc, on_text_delta);
}

// ── execute_chat_messages — multi-turn / multi-tool ────────────────────────

LLMResponse LLMClient::execute_chat_messages(
    const std::vector<ChatMessage>& messages,
    const nlohmann::json& tools,
    const nlohmann::json& tool_choice,
    const std::optional<std::string>& reasoning_effort,
    std::function<bool(const std::string&)> on_text_delta)
{
    LLMResponse res;

    if (router_ == "llama") {
        res.success = false;
        res.error = "router=llama must use built-in llama.cpp inference (not kbot HTTP client)";
        return res;
    }
    if (api_key_.empty()) {
        res.success = false;
        res.error = "API Key is empty.";
        return res;
    }
    if (messages.empty()) {
        res.success = false;
        res.error = "messages is empty (need at least a user message).";
        return res;
    }
    {
        const int n_tools = tools.is_array() ? static_cast<int>(tools.size()) : 0;
        const bool plan = chatgpt_plan_token(api_key_);
        android_llm_line(std::string("chat request model=") + (model_.empty() ? "gpt-4o" : model_)
                         + " tools=" + std::to_string(n_tools)
                         + " plan=" + (plan ? "1" : "0")
                         + " stream=" + (on_text_delta ? "1" : "0"));
        if (plan)
            return execute_responses_stream(messages, tools, tool_choice, {});
    }

    std::lock_guard<std::mutex> http_lk(liboai_http_mu());
    liboai::OpenAI oai_impl(base_url_.empty() ? "https://api.openai.com/v1" : base_url_);
    if (!apply_openai_auth(oai_impl, api_key_)) {
        res.success = false;
        res.error = "Failed to set API Key in liboai.";
        return res;
    }
    if (llm_timeout_ms_ > 0) {
        oai_impl.auth.SetMaxTimeout(llm_timeout_ms_);
        logger::trace("LLMClient: HTTP timeout set to " + std::to_string(llm_timeout_ms_) + " ms");
    }

    const std::string target_model = model_.empty() ? "gpt-4o" : model_;
    const std::string effective_url = (base_url_.empty() ? std::string("https://api.openai.com/v1") : base_url_)
                                       + "/chat/completions";
    const int n_tools = tools.is_array() ? (int)tools.size() : 0;
    logger::trace("LLMClient[chat]: POST " + effective_url
                 + " model=" + target_model
                 + " msgs=" + std::to_string(messages.size())
                 + " tools=" + std::to_string(n_tools)
                 + " router=" + (router_.empty() ? "?" : router_)
                 + " timeout_ms=" + std::to_string(llm_timeout_ms_)
                 + openai_auth_trace(api_key_)
                 + (on_text_delta ? " stream=true" : " stream=false")
                 + (reasoning_effort ? " reasoning_effort=" + *reasoning_effort : ""));
    // Debug: log tool names so schema errors are traceable.
    if (n_tools > 0) {
        std::string tool_names;
        for (const auto& t : tools) {
            if (!tool_names.empty()) tool_names += ", ";
            if (t.contains("function") && t["function"].is_object())
                tool_names += t["function"].value("name", std::string("?"));
            else
                tool_names += t.value("name", std::string("?"));
        }
        logger::debug("LLMClient[chat]: tools=[" + tool_names + "]");
        android_llm_line("chat tools [" + tool_names + "]");
    }

    // Build a Conversation by hand so we can express the tool-role messages
    // and the assistant tool_calls turns that liboai's higher-level
    // SetSystemData / AddUserData helpers don't cover.
    liboai::Conversation convo;
    {
        nlohmann::json msgs = nlohmann::json::array();
        for (const auto& m : messages) {
            nlohmann::json j;
            j["role"] = m.role;
            // OpenAI spec: assistant turns with tool_calls may carry null content.
            if (m.content.empty() && !m.tool_calls.empty() && m.role == "assistant") {
                j["content"] = nullptr;
            } else {
                j["content"] = m.content;
            }
            if (!m.tool_call_id.empty()) j["tool_call_id"] = m.tool_call_id;
            if (m.tool_calls.is_array() && !m.tool_calls.empty()) j["tool_calls"] = m.tool_calls;
            msgs.push_back(std::move(j));
        }
        // Import the constructed conversation. Conversation::Import expects the
        // Export() format: { "messages": [...] }.
        nlohmann::json packed; packed["messages"] = std::move(msgs);
        if (!convo.Import(dump_json_utf8_safe(packed, "chat"))) {
            res.success = false;
            res.error = "LLMClient: Conversation::Import failed for the supplied messages.";
            return res;
        }
    }

    std::optional<nlohmann::json> tools_opt;
    if (tools.is_array() && !tools.empty()) tools_opt = tools;
    std::optional<nlohmann::json> tool_choice_opt;
    if (!tool_choice.is_null()) tool_choice_opt = tool_choice;

    const auto t_start      = std::chrono::steady_clock::now();
    const bool is_tool_loop = tools.is_array() && !tools.empty();
    std::optional<uint16_t> max_out = max_tokens_cap_for_router(router_, is_tool_loop);
    if (max_out && router_ == "openrouter") {
        const size_t approx_in = approximate_openrouter_input_chars(messages, tools);
        *max_out = openrouter_shrink_max_tokens_for_payload(*max_out, approx_in, is_tool_loop);
        logger::trace("LLMClient[chat]: openrouter approx_input_chars=" + std::to_string(approx_in)
                     + " max_tokens=" + std::to_string(static_cast<unsigned>(*max_out))
                     + (is_tool_loop ? " (tool-loop)" : ""));
    }

    ChatSseAcc stream_acc;
    ScopedLlmCancelCheck cancel_scope(&cancel_check_);
    try {
        std::optional<liboai::ChatCompletion::ChatStreamCallback> stream_cb;
        if (on_text_delta) {
            stream_cb = [&](std::string data, intptr_t, liboai::Conversation&) -> bool {
                return chat_sse_ingest(data, stream_acc, on_text_delta);
            };
        }
        liboai::Response response = oai_impl.ChatCompletion->create(
            target_model,
            convo,
            std::nullopt,    // function_call (legacy) — leave unset
            std::nullopt,    // temperature
            std::nullopt,    // top_p
            std::nullopt,    // n
            stream_cb,       // stream — set only when on_text_delta is provided
            std::nullopt,    // stop
            max_out,         // max_tokens — lower on OpenRouter for agent/tool loops (large prompts)
            std::nullopt,    // presence_penalty
            std::nullopt,    // frequency_penalty
            std::nullopt,    // logit_bias
            std::nullopt,    // user
            std::nullopt,    // response_format
            tools_opt,       // tools (NEW)
            tool_choice_opt, // tool_choice (NEW)
            reasoning_effort // reasoning_effort (OpenAI o-series / gpt-5.x)
        );

        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t_start).count();
        logger::trace("LLMClient[chat]: HTTP " + std::to_string(response.status_code)
                     + " in " + std::to_string(elapsed_ms) + " ms"
                     + " resp_bytes=" + std::to_string(response.content.size()));

        if (on_text_delta) {
            chat_sse_finish(stream_acc, on_text_delta);
            if (stream_acc.cancelled) {
                res.success = false;
                res.cancelled = true;
                res.error = "Chat Completions stream cancelled";
                return res;
            }
            if (!stream_acc.error.empty()) {
                res.success = false;
                res.error = stream_acc.error;
                logger::error("LLMClient[chat-stream]: " + res.error);
                return res;
            }
            if (stream_acc.saw_sse) {
                res.text = std::move(stream_acc.text);
                res.reasoning_text = std::move(stream_acc.reasoning);
                res.finish_reason = stream_acc.finish_reason;
                const bool truncated = res.finish_reason == "length";
                if (truncated)
                    logger::warn("LLMClient[chat-stream]: finish_reason=length — output was truncated by max_tokens");
                for (const auto& kv : stream_acc.tools) {
                    const auto& slot = kv.second;
                    if (slot.name.empty() && slot.id.empty() && slot.arguments.empty())
                        continue;
                    ToolCall call;
                    call.id = slot.id;
                    call.name = slot.name;
                    const std::string raw_args = slot.arguments.empty() ? std::string("{}") : slot.arguments;
                    auto parsed = nlohmann::json::parse(raw_args, nullptr, false);
                    call.arguments = parsed.is_discarded() ? nlohmann::json::object() : std::move(parsed);
                    if (truncated && call.arguments.is_object() && call.arguments.empty()) {
                        logger::warn("LLMClient[chat-stream]: dropping truncated tool_call name="
                                     + call.name + " (no arguments due to max_tokens)");
                        continue;
                    }
                    res.tool_calls.push_back(std::move(call));
                }
                res.success = true;
                nlohmann::json meta = nlohmann::json::object();
                if (!stream_acc.id.empty()) meta["id"] = stream_acc.id;
                if (!stream_acc.model.empty()) meta["model"] = stream_acc.model;
                if (stream_acc.usage.is_object() && !stream_acc.usage.empty())
                    meta["usage"] = stream_acc.usage;
                if (!meta.empty()) res.provider_meta_json = meta.dump();
                logger::trace("LLMClient[chat-stream]: ok in " + std::to_string(elapsed_ms)
                             + " ms text_chars=" + std::to_string(res.text.size())
                             + " tool_calls=" + std::to_string(res.tool_calls.size())
                             + " finish_reason=" + (res.finish_reason.empty() ? "?" : res.finish_reason)
                             + (res.reasoning_text.empty() ? ""
                                : " reasoning_chars=" + std::to_string(res.reasoning_text.size())));
                return res;
            }
            if (response.content.empty() && !stream_acc.raw.empty())
                response.content = stream_acc.raw;
        }

        // Parse the top-level JSON the same way execute_chat does — liboai may
        // not populate raw_json for custom base URLs.
        nlohmann::json j;
        bool json_ok = false;
        if (!response.raw_json.empty() && response.raw_json.contains("choices")) {
            j = response.raw_json;
            json_ok = true;
        } else if (!response.content.empty()) {
            try {
                j = nlohmann::json::parse(response.content);
                json_ok = j.contains("choices");
            } catch (...) {}
        }

        if (!json_ok || j["choices"].empty()) {
            res.success = false;
            if (json_ok && j.contains("error")) {
                // Surface the OpenRouter / OpenAI-style {"error":{"message":...,"code":...,"type":...}}
                // shape directly so the user sees the model id / quota / auth issue verbatim.
                std::string detail;
                if (j["error"].is_object()) {
                    if (j["error"].contains("message") && j["error"]["message"].is_string())
                        detail = j["error"]["message"].get<std::string>();
                    else
                        detail = j["error"].dump();
                } else {
                    detail = j["error"].dump();
                }
                res.error = "API Error (HTTP " + std::to_string(response.status_code) + "): " + detail;
            } else {
                std::string raw_preview = response.content;
                if (raw_preview.size() > 400) raw_preview = raw_preview.substr(0, 400) + "\xE2\x80\xA6";
                res.error = "Invalid response format (HTTP " + std::to_string(response.status_code)
                             + "): no choices found. Raw: " + raw_preview;
            }
            logger::error("LLMClient[chat]: " + res.error);
            return res;
        }

        const auto& msg = j["choices"][0]["message"];

        // Detect output truncation: finish_reason="length" means max_tokens was hit.
        // Tool calls generated under truncation often have missing/empty arguments.
        const std::string finish_reason =
            (j["choices"][0].contains("finish_reason") && j["choices"][0]["finish_reason"].is_string())
            ? j["choices"][0]["finish_reason"].get<std::string>() : std::string{};
        res.finish_reason = finish_reason;
        const bool truncated = (finish_reason == "length");
        if (truncated)
            logger::warn("LLMClient[chat]: finish_reason=length — output was truncated by max_tokens");
        else if (!finish_reason.empty())
            logger::trace("LLMClient[chat]: finish_reason=" + finish_reason);

        // Either text content or tool_calls — both are success.
        if (msg.contains("content") && msg["content"].is_string()) {
            res.text = msg["content"].get<std::string>();
        }
        if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
            for (const auto& tc : msg["tool_calls"]) {
                ToolCall call;
                if (tc.contains("id")    && tc["id"].is_string())   call.id   = tc["id"].get<std::string>();
                if (tc.contains("function") && tc["function"].is_object()) {
                    const auto& fn = tc["function"];
                    if (fn.contains("name") && fn["name"].is_string()) call.name = fn["name"].get<std::string>();
                    if (fn.contains("arguments")) {
                        // OpenAI spec: arguments is a JSON string. Parse it.
                        if (fn["arguments"].is_string()) {
                            const std::string raw = fn["arguments"].get<std::string>();
                            auto parsed = nlohmann::json::parse(raw, nullptr, /*allow_exceptions*/ false);
                            call.arguments = parsed.is_discarded() ? nlohmann::json::object() : std::move(parsed);
                        } else if (fn["arguments"].is_object()) {
                            call.arguments = fn["arguments"];
                        } else {
                            call.arguments = nlohmann::json::object();
                        }
                    }
                }
                // Drop tool calls with empty arguments when output was truncated —
                // the model did not finish generating them.
                if (truncated && call.arguments.is_object() && call.arguments.empty()) {
                    logger::warn("LLMClient[chat]: dropping truncated tool_call name="
                                 + call.name + " (no arguments due to max_tokens)");
                    continue;
                }
                res.tool_calls.push_back(std::move(call));
            }
        }
        res.success = true;
        logger::trace("LLMClient[chat]: ok \xE2\x80\x94 text_chars=" + std::to_string(res.text.size())
                     + " tool_calls=" + std::to_string(res.tool_calls.size())
                     + " finish_reason=" + (res.finish_reason.empty() ? "?" : res.finish_reason));

        // Stash everything except `choices` for diagnostics (cost / model id / OpenRouter extras).
        try {
            nlohmann::json meta = nlohmann::json::object();
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.key() == "choices") continue;
                meta[it.key()] = it.value();
            }
            if (!meta.empty()) res.provider_meta_json = meta.dump();
        } catch (...) {}
    } catch (const std::exception& e) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t_start).count();
        const std::string what = e.what();
        logger::error(std::string("LLMClient[chat]: exception after ") + std::to_string(elapsed_ms)
                      + " ms: " + what);
        // Schema-related errors: dump the full tools JSON so the bad definition is identifiable.
        if (tools.is_array() && !tools.empty()
                && (what.find("schema") != std::string::npos
                    || what.find("function") != std::string::npos
                    || what.find("properties") != std::string::npos)) {
            logger::error("LLMClient[chat]: tools JSON that caused the error:\n"
                          + dump_json_for_diagnostics(tools, 2));
        }
        res.success = false;
        if (!stream_acc.error.empty()) {
            android_llm_line("chat stream error " + one_line_preview(stream_acc.error, 500)
                             + " raw=" + one_line_preview(stream_acc.raw, 500));
            res.error = stream_acc.error;
            logger::error("LLMClient[chat-stream]: " + res.error);
        } else if (stream_acc.cancelled
                || looks_cancelled_by_http_callback(what)
                || active_llm_cancel_requested()) {
            res.cancelled = true;
            res.error = "Chat Completions request cancelled";
        } else if (const auto* api = dynamic_cast<const liboai::exception::OpenAIException*>(&e)) {
            std::string body = api->ResponseBody();
            if (body.empty())
                body = stream_acc.raw;
            android_llm_line("chat http=" + std::to_string(api->HttpStatusCode())
                             + " tools=" + std::to_string(n_tools)
                             + " url=" + (api->ResponseUrl().empty() ? effective_url : api->ResponseUrl())
                             + " reason=" + api->ResponseReason()
                             + " body=" + one_line_preview(body, 700));
            logger::error("LLMClient[chat]: http=" + std::to_string(api->HttpStatusCode())
                + " reason=" + api->ResponseReason()
                + " body=" + one_line_preview(body, 500));
            res.error = compose_user_llm_error(what, api->HttpStatusCode(), body, target_model);
        } else {
            res.error = what;
        }
    } catch (...) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t_start).count();
        logger::error("LLMClient[chat]: unknown exception after " + std::to_string(elapsed_ms) + " ms");
        res.success = false;
        if (!stream_acc.error.empty()) {
            res.error = stream_acc.error;
        } else if (stream_acc.cancelled || active_llm_cancel_requested()) {
            res.cancelled = true;
            res.error = "Chat Completions request cancelled";
        } else {
            res.error = "Unknown error inside LLMClient::execute_chat_messages.";
        }
    }

    return res;
}

// ── execute_responses — POST /responses (OpenAI Responses API) ────────────

namespace {

/// Convert tools from Chat Completions format to Responses API format.
/// Chat:      {"type":"function","function":{"name":"...","description":"...","parameters":{...}}}
/// Responses: {"type":"function","name":"...","description":"...","parameters":{...}}
///
/// OpenAI /responses defaults `strict` to true when omitted. Strict mode then
/// rewrites the schema (every property required, additionalProperties:false,
/// empty-string enums dropped) so optional fields like image_transform's
/// aspect_ratio / image_size become mandatory and the model invents 16:9 / 2K.
/// Our catalog is written with optional fields; keep that contract unless a
/// tool already set `strict` itself.
void ensure_responses_function_strict_default(nlohmann::json& tool)
{
    if (!tool.is_object()) return;
    if (tool.value("type", std::string()) != "function") return;
    if (!tool.contains("strict"))
        tool["strict"] = false;
}

nlohmann::json convert_tools_to_responses_format(const nlohmann::json& tools)
{
    if (!tools.is_array()) return nlohmann::json::array();
    nlohmann::json out = nlohmann::json::array();
    for (const auto& t : tools) {
        if (!t.is_object()) continue;
        if (t.value("type", std::string()) == "function"
                && t.contains("function") && t["function"].is_object()) {
            nlohmann::json flat = t["function"];
            flat["type"] = "function";
            ensure_responses_function_strict_default(flat);
            out.push_back(std::move(flat));
        } else {
            nlohmann::json copy = t;
            ensure_responses_function_strict_default(copy);
            out.push_back(std::move(copy));
        }
    }
    return out;
}

void strip_empty_enum_values_in_schema(nlohmann::json& node, int& removed_count)
{
    if (node.is_object()) {
        if (node.contains("enum") && node["enum"].is_array()) {
            nlohmann::json cleaned = nlohmann::json::array();
            for (const auto& v : node["enum"]) {
                if (v.is_string() && v.get<std::string>().empty()) {
                    ++removed_count;
                    continue;
                }
                cleaned.push_back(v);
            }
            node["enum"] = std::move(cleaned);
        }
        for (auto it = node.begin(); it != node.end(); ++it) {
            strip_empty_enum_values_in_schema(it.value(), removed_count);
        }
    } else if (node.is_array()) {
        for (auto& v : node) {
            strip_empty_enum_values_in_schema(v, removed_count);
        }
    }
}

std::string repair_utf8_for_json(const std::string& in,
                                 bool* changed = nullptr,
                                 size_t* first_bad_index = nullptr,
                                 unsigned* first_bad_byte = nullptr)
{
    if (changed) *changed = false;
    if (first_bad_index) *first_bad_index = std::string::npos;
    if (first_bad_byte) *first_bad_byte = 0;
    std::string out;
    out.reserve(in.size());

    auto replace_one = [&](size_t i, unsigned char byte) {
        out += "\xEF\xBF\xBD"; // U+FFFD replacement character
        if (changed) *changed = true;
        if (first_bad_index && *first_bad_index == std::string::npos) {
            *first_bad_index = i;
            if (first_bad_byte) *first_bad_byte = byte;
        }
    };

    for (size_t i = 0; i < in.size();) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c <= 0x7F) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        auto cont = [&](size_t off) -> bool {
            if (i + off >= in.size()) return false;
            const unsigned char x = static_cast<unsigned char>(in[i + off]);
            return (x & 0xC0) == 0x80;
        };
        if (c >= 0xC2 && c <= 0xDF && cont(1)) {
            out.append(in, i, 2);
            i += 2;
            continue;
        }
        if (c >= 0xE0 && c <= 0xEF && cont(1) && cont(2)) {
            const unsigned char c1 = static_cast<unsigned char>(in[i + 1]);
            if ((c != 0xE0 || c1 >= 0xA0)      // reject overlong 3-byte forms
                    && (c != 0xED || c1 <= 0x9F)) { // reject UTF-16 surrogates
                out.append(in, i, 3);
                i += 3;
                continue;
            }
        }
        if (c >= 0xF0 && c <= 0xF4 && cont(1) && cont(2) && cont(3)) {
            const unsigned char c1 = static_cast<unsigned char>(in[i + 1]);
            if ((c != 0xF0 || c1 >= 0x90)      // reject overlong 4-byte forms
                    && (c != 0xF4 || c1 <= 0x8F)) { // reject > U+10FFFF
                out.append(in, i, 4);
                i += 4;
                continue;
            }
        }
        replace_one(i, c);
        ++i;
    }
    return out;
}

std::string json_path_join(const std::string& base, const std::string& part)
{
    std::string escaped;
    escaped.reserve(part.size());
    for (char c : part) {
        if (c == '~') escaped += "~0";
        else if (c == '/') escaped += "~1";
        else escaped.push_back(c);
    }
    return base + "/" + escaped;
}

std::string hex_byte(unsigned byte)
{
    char buf[8]{};
    std::snprintf(buf, sizeof(buf), "0x%02X", byte & 0xFFu);
    return buf;
}

std::string byte_window_preview(const std::string& s, size_t at)
{
    if (s.empty() || at == std::string::npos) return {};
    const size_t begin = at > 24 ? at - 24 : 0;
    const size_t end = (std::min)(s.size(), at + 25);
    std::string out;
    if (begin > 0) out += "...";
    for (size_t i = begin; i < end; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (i == at) out += "[";
        if (c >= 0x20 && c <= 0x7E && c != '\\' && c != '"') {
            out.push_back(static_cast<char>(c));
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c == '"') {
            out += "\\\"";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out += "\\x";
            out += hex_byte(c).substr(2);
        }
        if (i == at) out += "]";
    }
    if (end < s.size()) out += "...";
    return out;
}

void sanitize_json_utf8(nlohmann::json& node,
                        int& repaired_strings,
                        const char* tag = nullptr,
                        const std::string& path = "")
{
    if (node.is_string()) {
        bool changed = false;
        size_t first_bad = std::string::npos;
        unsigned first_byte = 0;
        const std::string original = node.get<std::string>();
        std::string s = repair_utf8_for_json(original, &changed, &first_bad, &first_byte);
        if (changed) {
            if (tag && first_bad != std::string::npos) {
                logger::trace(std::string("LLMClient[") + tag + "]: invalid UTF-8 at "
                              + (path.empty() ? std::string("/") : path)
                              + " byte=" + std::to_string(first_bad)
                              + " value=" + hex_byte(first_byte)
                              + " len=" + std::to_string(original.size())
                              + " preview=\"" + byte_window_preview(original, first_bad) + "\"");
            }
            node = std::move(s);
            ++repaired_strings;
        }
    } else if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it)
            sanitize_json_utf8(it.value(), repaired_strings, tag,
                               json_path_join(path, it.key()));
    } else if (node.is_array()) {
        for (size_t i = 0; i < node.size(); ++i)
            sanitize_json_utf8(node[i], repaired_strings, tag,
                               json_path_join(path, std::to_string(i)));
    }
}

void sanitize_responses_request_utf8(nlohmann::json& request, const char* tag)
{
    int repaired = 0;
    sanitize_json_utf8(request, repaired, tag);
    if (repaired > 0) {
        logger::warn(std::string("LLMClient[") + tag + "]: repaired "
                     + std::to_string(repaired)
                     + " invalid UTF-8 string(s) before serializing request");
    }
}

std::string dump_json_utf8_safe(nlohmann::json& j, const char* tag)
{
    sanitize_responses_request_utf8(j, tag);
    try {
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (...) {
        return "{}";
    }
}

std::string dump_json_for_diagnostics(const nlohmann::json& j, int indent = 2)
{
    try {
        return j.dump(indent);
    } catch (const nlohmann::json::type_error&) {
        nlohmann::json repaired = j;
        int repaired_strings = 0;
        sanitize_json_utf8(repaired, repaired_strings);
        try {
            return repaired.dump(indent)
                + "\n[diagnostic: repaired invalid UTF-8 strings="
                + std::to_string(repaired_strings) + "]";
        } catch (const std::exception& e) {
            return std::string("[diagnostic dump unavailable after UTF-8 repair: ") + e.what() + "]";
        }
    } catch (const std::exception& e) {
        return std::string("[diagnostic dump unavailable: ") + e.what() + "]";
    }
}

bool looks_cancelled_by_http_callback(const std::string& what)
{
    return what.find("CURLE_ABORTED_BY_CALLBACK") != std::string::npos
        || what.find("aborted by callback") != std::string::npos
        || what.find("Callback aborted") != std::string::npos
        || what.find("operation was aborted") != std::string::npos;
}

} // namespace

std::string preview_json_or_text(const std::string& content, std::size_t max_len = 2000) {
    std::string out = content;
    if (out.size() > max_len)
        out = out.substr(0, max_len) + "...";
    return out;
}

std::string extract_provider_error_detail(const std::string& body);

std::string strip_liboai_exception_tail(std::string s)
{
    const auto paren = s.rfind('(');
    if (paren != std::string::npos && paren + 1 < s.size() && s.back() == ')') {
        const std::string tail = s.substr(paren + 1, s.size() - paren - 2);
        if (tail.size() >= 3 && tail[0] == 'E' && tail[1] == '_')
            s.erase(paren);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

std::string strip_liboai_check_response_prefix(std::string s)
{
    const std::string prefix = "liboai::Response::CheckResponse(): ";
    if (s.rfind(prefix, 0) == 0)
        s.erase(0, prefix.size());
    return strip_liboai_exception_tail(std::move(s));
}

bool is_generic_liboai_http_reason(std::string_view text)
{
    std::string s = strip_liboai_check_response_prefix(std::string(text));
    const std::string lower = [&]() {
        std::string out = s;
        for (char& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }();
    return lower == "bad request"
        || lower == "an unknown error occurred"
        || lower.find("bad request ") == 0;
}

bool looks_like_model_access_error(std::string_view text)
{
    const std::string lower = [&]() {
        std::string out(text);
        for (char& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }();
    return lower.find("the model ") != std::string::npos
        || lower.find("does not exist or you do not have access") != std::string::npos
        || lower.find("model_not_found") != std::string::npos;
}

std::string compose_user_llm_error(const std::string& what,
                                   long http_status,
                                   const std::string& body,
                                   const std::string& model)
{
    if (const std::string from_body = extract_provider_error_detail(body); !from_body.empty())
        return from_body;

    std::string stripped = strip_liboai_check_response_prefix(what);
    if (looks_like_model_access_error(stripped))
        return stripped;
    if (!is_generic_liboai_http_reason(stripped) && !stripped.empty())
        return stripped;

    if (http_status == 400 && !model.empty()) {
        std::string msg = "The model `" + model
            + "` does not exist or you do not have access to it.";
        if (!body.empty())
            msg += " HTTP body: " + one_line_preview(body, 480);
        return msg;
    }

    if (!stripped.empty())
        return stripped;
    return what;
}

std::string extract_provider_error_detail(const std::string& body) {
    if (body.empty()) return {};
    try {
        const auto j = nlohmann::json::parse(body, nullptr, false);
        if (!j.is_object() || !j.contains("error") || !j["error"].is_object())
            return {};
        const auto& err = j["error"];
        if (err.contains("message") && err["message"].is_string()) {
            const std::string msg = err["message"].get<std::string>();
            if (!msg.empty())
                return msg;
        }
        std::string provider_name;
        std::string provider_status;
        std::string provider_message;
        if (err.contains("metadata") && err["metadata"].is_object()) {
            const auto& m = err["metadata"];
            if (m.contains("provider_name") && m["provider_name"].is_string())
                provider_name = m["provider_name"].get<std::string>();
            if (m.contains("raw") && m["raw"].is_string()) {
                const std::string raw = m["raw"].get<std::string>();
                const auto jr = nlohmann::json::parse(raw, nullptr, false);
                if (jr.is_object() && jr.contains("error") && jr["error"].is_object()) {
                    const auto& pe = jr["error"];
                    if (pe.contains("status") && pe["status"].is_string())
                        provider_status = pe["status"].get<std::string>();
                    if (pe.contains("message") && pe["message"].is_string())
                        provider_message = pe["message"].get<std::string>();
                }
            }
        }
        std::string out;
        if (!provider_name.empty())
            out += "provider=" + provider_name;
        if (!provider_status.empty()) {
            if (!out.empty()) out += " ";
            out += "status=" + provider_status;
        }
        if (!provider_message.empty()) {
            if (!out.empty()) out += " ";
            out += "message=" + provider_message;
        }
        return out;
    } catch (...) {
        return {};
    }
}

LLMResponse LLMClient::execute_responses(
    const std::vector<ChatMessage>& messages,
    const nlohmann::json& tools,
    const nlohmann::json& tool_choice,
    const std::string& previous_response_id)
{
    LLMResponse res;

    if (api_key_.empty()) {
        res.success = false;
        res.error = "API Key is empty.";
        return res;
    }
    if (messages.empty()) {
        res.success = false;
        res.error = "messages is empty.";
        return res;
    }
    // Plan tokens are rejected unless the body is streamed.
    if (chatgpt_plan_token(api_key_))
        return execute_responses_stream(messages, tools, tool_choice, {});

    std::lock_guard<std::mutex> http_lk(liboai_http_mu());
    const std::string eff_base = base_url_.empty() ? "https://api.openai.com/v1" : base_url_;
    liboai::OpenAI oai_impl(eff_base);
    if (!apply_openai_auth(oai_impl, api_key_)) {
        res.success = false;
        res.error = "Failed to set API Key in liboai.";
        return res;
    }
    if (llm_timeout_ms_ > 0) {
        oai_impl.auth.SetMaxTimeout(llm_timeout_ms_);
        logger::trace("LLMClient[responses]: HTTP timeout set to "
                      + std::to_string(llm_timeout_ms_) + " ms");
    }

    const std::string target_model = model_.empty() ? "gpt-4o" : model_;
    const int n_tools = tools.is_array() ? static_cast<int>(tools.size()) : 0;
    logger::trace("LLMClient[responses]: POST " + eff_base + "/responses"
                 + " model=" + target_model
                 + (previous_response_id.empty()
                    ? " msgs=" + std::to_string(messages.size()) + " [stateless]"
                    : " [stateful prev=" + previous_response_id.substr(0, 16) + "…]")
                 + " tools=" + std::to_string(n_tools)
                 + " timeout_ms=" + std::to_string(llm_timeout_ms_));
    if (n_tools > 0) {
        std::string tool_names;
        for (const auto& t : tools) {
            if (!tool_names.empty()) tool_names += ", ";
            if (t.contains("function") && t["function"].is_object())
                tool_names += t["function"].value("name", std::string("?"));
            else
                tool_names += t.value("name", std::string("?"));
        }
        logger::debug("LLMClient[responses]: tools=[" + tool_names + "]");
    }

    // ── Build input[] and request body ────────────────────────────────────
    nlohmann::json request;
    request["model"] = target_model;
    // /responses server-side storage (store=true + previous_response_id chaining)
    // is only honored by OpenAI-family models. OpenRouter — and the pixlwiz
    // umbrella that proxies to it — reject store=true for non-OpenAI backends
    // (e.g. Anthropic: "Invalid input: expected false"). Detect the OpenAI case
    // from the slug; otherwise fall back to stateless replay with store=false.
    const bool model_is_openai_family =
        target_model.find("openai/") != std::string::npos
        || target_model.rfind("gpt", 0) == 0
        || target_model.find("/gpt") != std::string::npos;
    const bool no_responses_storage =
        router_ == "openrouter"
        || (router_ == "tanit" && !model_is_openai_family);
    request["store"] = no_responses_storage ? false : true;
    // OpenRouter may reject `include` on /responses for some routed models;
    // encrypted reasoning content is optional for our loop, so only request it
    // on native OpenAI endpoints for now.
    if (router_ == "openai")
        request["include"] = nlohmann::json::array({"reasoning.encrypted_content"});

    nlohmann::json responses_tools;
    if (n_tools > 0) {
        responses_tools = convert_tools_to_responses_format(tools);
        int removed_empty_enums = 0;
        strip_empty_enum_values_in_schema(responses_tools, removed_empty_enums);
        if (removed_empty_enums > 0) {
            logger::warn("LLMClient[responses]: sanitized tools schema, removed "
                         + std::to_string(removed_empty_enums)
                         + " empty enum entries for provider compatibility");
        }
    }

    bool use_stateful = !previous_response_id.empty();
    const bool plan_token = chatgpt_plan_token(api_key_);
    if (plan_token)
        use_stateful = false;
    if (use_stateful && no_responses_storage) {
        logger::debug("LLMClient[responses]: disabling previous_response_id chaining "
                     "(OpenRouter/pixlwiz non-OpenAI backend has no /responses storage); "
                     "using stateless replay");
        use_stateful = false;
    }
    if (use_stateful) {
        // ── Stateful (chained) mode ────────────────────────────────────────
        // Only send the tool results from the most recent round; the full
        // conversation history, system prompt, and tool schemas are cached
        // server-side under previous_response_id. This avoids re-sending
        // the entire context (tool schemas + file contents + reasoning blobs)
        // on every round, which would otherwise blow past 40k tokens on
        // multi-step agent tasks.
        nlohmann::json input = nlohmann::json::array();
        // Walk messages from the end, collecting "tool" role messages until
        // we hit the preceding "assistant" turn (which is already cached).
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (it->role == "assistant") break;
            if (it->role == "tool") {
                input.insert(input.begin(), nlohmann::json{
                    {"type",    "function_call_output"},
                    {"call_id", it->tool_call_id},
                    {"output",  it->content}
                });
            }
        }
        if (input.empty()) {
            logger::warn("LLMClient[responses]: stateful round had no tool outputs; "
                         "falling back to stateless request to avoid empty-input provider error");
            use_stateful = false;
        } else {
            request["previous_response_id"] = previous_response_id;
            request["input"] = std::move(input);
            // Tools are re-sent each round so the model can use them from the new output.
            if (n_tools > 0) {
                request["tools"] = responses_tools;
                if (!tool_choice.is_null() && !tool_choice.is_discarded())
                    request["tool_choice"] = tool_choice;
                else
                    request["tool_choice"] = "auto";
            }
            logger::debug("LLMClient[responses]: stateful chain — prev=" + previous_response_id);
        }
    }
    if (!use_stateful) {
        // ── Stateless (first round) mode ───────────────────────────────────
        // Translate the full message history into input[] + instructions.
        std::string instructions;
        nlohmann::json input = nlohmann::json::array();

        for (const auto& m : messages) {
            if (m.role == "system") {
                instructions = m.content;
                continue;
            }
            if (m.role == "user") {
                input.push_back(nlohmann::json{
                    {"type",    "message"},
                    {"role",    "user"},
                    {"content", nlohmann::json::array({
                        nlohmann::json{{"type","input_text"},{"text", m.content}}
                    })}
                });
            } else if (m.role == "assistant") {
                if (m.tool_calls.is_array() && !m.tool_calls.empty()) {
                    if (m.responses_output_items.is_array() && !m.responses_output_items.empty()) {
                        // Replay verbatim: reasoning items carry summary + encrypted_content;
                        // function_call items carry all original fields. Different gpt-5 versions
                        // require different fields (gpt-5.2 needs encrypted_content, gpt-5.5
                        // needs summary), so we store and replay the complete item.
                        for (const auto& raw : m.responses_output_items)
                            input.push_back(raw);
                    } else {
                        // Fallback: reconstruct function_call items from tool_calls[] array.
                        // Used for /chat/completions history or pre-stateful conversations.
                        for (const auto& tc : m.tool_calls) {
                            const std::string call_id = tc.value("id", std::string());
                            std::string name, arguments;
                            if (tc.contains("function") && tc["function"].is_object()) {
                                name = tc["function"].value("name", std::string());
                                if (tc["function"].contains("arguments")) {
                                    const auto& args = tc["function"]["arguments"];
                                    if (args.is_string())
                                        arguments = args.get<std::string>();
                                    else
                                        arguments = args.dump();
                                } else {
                                    arguments = "{}";
                                }
                            }
                            const std::string item_id = tc.value("item_id", std::string());
                            nlohmann::json fc_item{
                                {"type",      "function_call"},
                                {"call_id",   call_id},
                                {"name",      std::move(name)},
                                {"arguments", std::move(arguments)}
                            };
                            if (!item_id.empty())
                                fc_item["id"] = item_id;
                            input.push_back(std::move(fc_item));
                        }
                    }
                } else {
                    input.push_back(nlohmann::json{
                        {"type",    "message"},
                        {"role",    "assistant"},
                        {"content", nlohmann::json::array({
                            nlohmann::json{{"type","output_text"},{"text", m.content}}
                        })}
                    });
                }
            } else if (m.role == "tool") {
                input.push_back(nlohmann::json{
                    {"type",    "function_call_output"},
                    {"call_id", m.tool_call_id},
                    {"output",  m.content}
                });
            }
        }
        request["input"] = std::move(input);
        if (!instructions.empty())
            request["instructions"] = instructions;
        if (n_tools > 0) {
            request["tools"] = responses_tools;
            if (!tool_choice.is_null() && !tool_choice.is_discarded())
                request["tool_choice"] = tool_choice;
            else
                request["tool_choice"] = "auto";
        }
    }

    if (router_ == "openrouter") {
        const bool   is_tool_loop = (n_tools > 0);
        const size_t approx_in    = approximate_openrouter_input_chars(messages, tools);
        uint16_t     max_out      = max_tokens_cap_for_router(router_, is_tool_loop)
                                      .value_or(static_cast<uint16_t>(1024));
        max_out = openrouter_shrink_max_tokens_for_payload(max_out, approx_in, is_tool_loop);
        request["max_output_tokens"] = static_cast<unsigned>(max_out);
        logger::trace("LLMClient[responses]: openrouter approx_input_chars=" + std::to_string(approx_in)
                     + " max_output_tokens=" + std::to_string(static_cast<unsigned>(max_out))
                     + (is_tool_loop ? " (tool-loop)" : ""));
    }

    if (plan_token)
        shape_chatgpt_plan_request(request);
    sanitize_responses_request_utf8(request, "responses");

    // ── HTTP call ──────────────────────────────────────────────────────────
    const auto t_start = std::chrono::steady_clock::now();
    ScopedLlmCancelCheck cancel_scope(&cancel_check_);
    try {
        liboai::Response response = oai_impl.Responses->create(request);

        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t_start).count();
        logger::trace("LLMClient[responses]: HTTP " + std::to_string(response.status_code)
                     + " in " + std::to_string(elapsed_ms) + " ms"
                     + " resp_bytes=" + std::to_string(response.content.size()));

        // ── Parse response ─────────────────────────────────────────────────
        nlohmann::json j;
        bool json_ok = false;
        if (!response.raw_json.empty() && response.raw_json.is_object()) {
            j = response.raw_json;
            json_ok = true;
        } else if (!response.content.empty()) {
            try {
                j = nlohmann::json::parse(response.content);
                json_ok = j.is_object();
            } catch (...) {}
        }

        if (!json_ok) {
            res.success = false;
            std::string preview = response.content;
            if (preview.size() > 400) preview = preview.substr(0, 400) + "\xe2\x80\xa6";
            res.error = "Responses API: invalid JSON (HTTP "
                        + std::to_string(response.status_code) + "): " + preview;
            logger::error("LLMClient[responses]: " + res.error);
            return res;
        }

        // API-level error envelope
        if (j.contains("error") && j["error"].is_object()) {
            const auto& e = j["error"];
            const std::string detail = e.contains("message") && e["message"].is_string()
                ? e["message"].get<std::string>() : e.dump();
            res.success = false;
            res.error = "Responses API error (HTTP "
                        + std::to_string(response.status_code) + "): " + detail;
            logger::error("LLMClient[responses]: " + res.error);
            return res;
        }

        // status == "failed" | "cancelled"
        const std::string status = j.value("status", std::string());
        if (status == "failed" || status == "cancelled") {
            std::string detail;
            if (j.contains("error") && j["error"].is_object())
                detail = j["error"].value("message", j["error"].dump());
            res.success = false;
            res.error = "Responses API: status=" + status + (detail.empty() ? "" : ": " + detail);
            logger::error("LLMClient[responses]: " + res.error);
            return res;
        }

        // ── Extract output items ───────────────────────────────────────────
        std::string text_accum;
        std::string reasoning_accum;
        if (j.contains("output") && j["output"].is_array()) {
            for (const auto& item : j["output"]) {
                const std::string itype = item.value("type", std::string());
                if (itype == "reasoning") {
                    // Store the complete item for verbatim replay. Different model versions
                    // have different requirements: gpt-5.2 needs encrypted_content, gpt-5.5
                    // needs summary. Storing everything keeps us compatible with both.
                    res.responses_output_items.push_back(item);
                    // Also extract human-readable summary text for Event::Thinking display.
                    if (item.contains("summary") && item["summary"].is_array()) {
                        for (const auto& s : item["summary"]) {
                            if (s.value("type", std::string()) == "summary_text"
                                    && s.contains("text") && s["text"].is_string()) {
                                if (!reasoning_accum.empty()) reasoning_accum += '\n';
                                reasoning_accum += s["text"].get<std::string>();
                            }
                        }
                    }
                } else if (itype == "message") {
                    if (item.contains("content") && item["content"].is_array()) {
                        for (const auto& c : item["content"]) {
                            const std::string ctype = c.value("type", std::string());
                            if ((ctype == "output_text" || ctype == "text")
                                    && c.contains("text") && c["text"].is_string())
                                text_accum += c["text"].get<std::string>();
                        }
                    }
                } else if (itype == "function_call") {
                    // Store verbatim for replay (must follow its reasoning item in input[]).
                    res.responses_output_items.push_back(item);
                    ToolCall call;
                    // item_id: the "fc_…" item identifier — must be replayed as
                    // input[].function_call.id in the next round.
                    call.item_id = item.value("id", std::string());
                    // call_id: used in function_call_output.call_id for result correlation.
                    call.id = item.contains("call_id") && item["call_id"].is_string()
                              ? item["call_id"].get<std::string>()
                              : call.item_id;
                    call.name = item.value("name", std::string());
                    const std::string args_str = item.value("arguments", std::string("{}"));
                    auto parsed = nlohmann::json::parse(args_str, nullptr, /*exceptions*/ false);
                    call.arguments = parsed.is_discarded() ? nlohmann::json::object() : std::move(parsed);
                    res.tool_calls.push_back(std::move(call));
                } else {
                    // Forward-compatible: preserve any other non-message output items.
                    res.responses_output_items.push_back(item);
                }
            }
        }

        res.text           = std::move(text_accum);
        res.reasoning_text = std::move(reasoning_accum);
        // Capture the response ID for stateful chaining in the next round.
        if (j.contains("id") && j["id"].is_string())
            res.response_id = j["id"].get<std::string>();
        res.success = true;
        logger::trace("LLMClient[responses]: ok \xe2\x80\x94 text_chars=" + std::to_string(res.text.size())
                     + " tool_calls=" + std::to_string(res.tool_calls.size())
                     + (res.reasoning_text.empty() ? "" :
                        " reasoning_chars=" + std::to_string(res.reasoning_text.size())));

        // Stash top-level meta for usage aggregation (omit "output")
        try {
            nlohmann::json meta = nlohmann::json::object();
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.key() == "output") continue;
                meta[it.key()] = it.value();
            }
            if (!meta.empty()) res.provider_meta_json = meta.dump();
        } catch (...) {}

    } catch (const liboai::exception::OpenAIException& e) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t_start).count();
        const std::string what = e.what();
        if (looks_cancelled_by_http_callback(what)) {
            res.success = false;
            res.cancelled = true;
            res.error = "Responses API request cancelled";
            logger::trace("LLMClient[responses]: cancelled after "
                          + std::to_string(elapsed_ms) + " ms");
            return res;
        }
        std::string detail_for_error;
        logger::error("LLMClient[responses]: exception after " + std::to_string(elapsed_ms)
                      + " ms: " + what);
        logger::error("LLMClient[responses]: request payload preview:\n"
                      + preview_json_or_text(dump_json_for_diagnostics(request)));
        logger::error("LLMClient[responses]: request base_url=" + eff_base
                      + " model=" + target_model
                      + " tool_count=" + std::to_string(n_tools)
                      + (previous_response_id.empty()
                         ? " state=stateless"
                         : " state=stateful"));

        const long http_status = e.HttpStatusCode();
        const std::string& body = e.ResponseBody();
        if (http_status > 0 || !body.empty()) {
            logger::error("LLMClient[responses]: liboai error HTTP " + std::to_string(http_status)
                          + " body:\n" + preview_json_or_text(body, 6000));
            detail_for_error = " [responses_http="
                + std::to_string(http_status)
                + " body=" + preview_json_or_text(body, 1200) + "]";
            const std::string extracted = extract_provider_error_detail(body);
            if (!extracted.empty()) {
                logger::error("LLMClient[responses]: provider detail: " + extracted);
                detail_for_error += " [responses_provider_detail=" + extracted + "]";
            }
        }
        (void)detail_for_error;

        if (tools.is_array() && !tools.empty()
                && (what.find("schema") != std::string::npos
                    || what.find("function") != std::string::npos
                    || what.find("properties") != std::string::npos)) {
            logger::error("LLMClient[responses]: tools JSON that caused the error:\n"
                          + dump_json_for_diagnostics(tools));
        }
        res.success = false;
        res.error = compose_user_llm_error(what, http_status, body, target_model);
    } catch (const std::exception& e) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t_start).count();
        const std::string what = e.what();
        if (looks_cancelled_by_http_callback(what)) {
            res.success = false;
            res.cancelled = true;
            res.error = "Responses API request cancelled";
            logger::trace("LLMClient[responses]: cancelled after "
                          + std::to_string(elapsed_ms) + " ms");
            return res;
        }
        logger::error("LLMClient[responses]: exception after " + std::to_string(elapsed_ms)
                      + " ms: " + what);
        logger::error("LLMClient[responses]: request payload preview:\n"
                      + preview_json_or_text(dump_json_for_diagnostics(request)));
        logger::error("LLMClient[responses]: request base_url=" + eff_base
                      + " model=" + target_model
                      + " tool_count=" + std::to_string(n_tools)
                      + (previous_response_id.empty()
                         ? " state=stateless"
                         : " state=stateful"));
        if (tools.is_array() && !tools.empty()
                && (what.find("schema") != std::string::npos
                    || what.find("function") != std::string::npos
                    || what.find("properties") != std::string::npos)) {
            logger::error("LLMClient[responses]: tools JSON that caused the error:\n"
                          + dump_json_for_diagnostics(tools));
        }
        res.success = false;
        res.error = compose_user_llm_error(what, 0, std::string{}, target_model);
    } catch (...) {
        res.success = false;
        res.error = "Unknown error inside LLMClient::execute_responses.";
    }

    return res;
}

LLMResponse LLMClient::execute_responses_stream(
    const std::vector<ChatMessage>& messages,
    const nlohmann::json& tools,
    const nlohmann::json& tool_choice,
    const std::string& previous_response_id,
    std::function<bool(const std::string&)> on_text_delta)
{
    LLMResponse res;

    if (api_key_.empty()) {
        res.success = false;
        res.error = "API Key is empty.";
        return res;
    }
    if (messages.empty()) {
        res.success = false;
        res.error = "messages is empty.";
        return res;
    }

    std::lock_guard<std::mutex> http_lk(liboai_http_mu());
    const std::string eff_base = base_url_.empty() ? "https://api.openai.com/v1" : base_url_;
    liboai::OpenAI oai_impl(eff_base);
    if (!apply_openai_auth(oai_impl, api_key_)) {
        res.success = false;
        res.error = "Failed to set API Key in liboai.";
        return res;
    }
    if (llm_timeout_ms_ > 0) {
        oai_impl.auth.SetMaxTimeout(llm_timeout_ms_);
        logger::trace("LLMClient[responses-stream]: HTTP timeout set to "
                      + std::to_string(llm_timeout_ms_) + " ms");
    }

    const std::string target_model = model_.empty() ? "gpt-4o" : model_;
    const int n_tools = tools.is_array() ? static_cast<int>(tools.size()) : 0;
    logger::trace("LLMClient[responses-stream]: POST " + eff_base + "/responses"
                 + " model=" + target_model
                 + (previous_response_id.empty()
                    ? " msgs=" + std::to_string(messages.size()) + " [stateless]"
                    : " [stateful prev=" + previous_response_id.substr(0, 16) + "…]")
                 + " tools=" + std::to_string(n_tools)
                 + " timeout_ms=" + std::to_string(llm_timeout_ms_));

    nlohmann::json request;
    request["model"] = target_model;
    // See execute_responses(): only OpenAI-family backends honor /responses
    // storage; OpenRouter and pixlwiz→non-OpenAI reject store=true.
    const bool model_is_openai_family =
        target_model.find("openai/") != std::string::npos
        || target_model.rfind("gpt", 0) == 0
        || target_model.find("/gpt") != std::string::npos;
    const bool no_responses_storage =
        router_ == "openrouter"
        || (router_ == "tanit" && !model_is_openai_family);
    request["store"] = no_responses_storage ? false : true;
    if (router_ == "openai")
        request["include"] = nlohmann::json::array({"reasoning.encrypted_content"});

    nlohmann::json responses_tools;
    if (n_tools > 0) {
        responses_tools = convert_tools_to_responses_format(tools);
        int removed_empty_enums = 0;
        strip_empty_enum_values_in_schema(responses_tools, removed_empty_enums);
        if (removed_empty_enums > 0) {
            logger::warn("LLMClient[responses-stream]: sanitized tools schema, removed "
                         + std::to_string(removed_empty_enums)
                         + " empty enum entries for provider compatibility");
        }
    }

    bool use_stateful = !previous_response_id.empty();
    const bool plan_token = chatgpt_plan_token(api_key_);
    if (plan_token)
        use_stateful = false;
    if (use_stateful && no_responses_storage) {
        logger::warn("LLMClient[responses-stream]: disabling previous_response_id chaining "
                     "(OpenRouter/pixlwiz non-OpenAI backend has no /responses storage); "
                     "using stateless replay");
        use_stateful = false;
    }
    if (use_stateful) {
        nlohmann::json input = nlohmann::json::array();
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (it->role == "assistant") break;
            if (it->role == "tool") {
                input.insert(input.begin(), nlohmann::json{
                    {"type",    "function_call_output"},
                    {"call_id", it->tool_call_id},
                    {"output",  it->content}
                });
            }
        }
        if (input.empty()) {
            logger::warn("LLMClient[responses-stream]: stateful round had no tool outputs; "
                         "falling back to stateless request to avoid empty-input provider error");
            use_stateful = false;
        } else {
            request["previous_response_id"] = previous_response_id;
            request["input"] = std::move(input);
            if (n_tools > 0) {
                request["tools"] = responses_tools;
                if (!tool_choice.is_null() && !tool_choice.is_discarded())
                    request["tool_choice"] = tool_choice;
                else
                    request["tool_choice"] = "auto";
            }
        }
    }
    if (!use_stateful) {
        std::string instructions;
        nlohmann::json input = nlohmann::json::array();
        for (const auto& m : messages) {
            if (m.role == "system") {
                instructions = m.content;
                continue;
            }
            if (m.role == "user") {
                input.push_back(nlohmann::json{
                    {"type",    "message"},
                    {"role",    "user"},
                    {"content", nlohmann::json::array({
                        nlohmann::json{{"type","input_text"},{"text", m.content}}
                    })}
                });
            } else if (m.role == "assistant") {
                if (m.tool_calls.is_array() && !m.tool_calls.empty()) {
                    if (m.responses_output_items.is_array() && !m.responses_output_items.empty()) {
                        for (const auto& raw : m.responses_output_items)
                            input.push_back(raw);
                    } else {
                        for (const auto& tc : m.tool_calls) {
                            const std::string call_id = tc.value("id", std::string());
                            std::string name, arguments;
                            if (tc.contains("function") && tc["function"].is_object()) {
                                name = tc["function"].value("name", std::string());
                                if (tc["function"].contains("arguments")) {
                                    const auto& args = tc["function"]["arguments"];
                                    if (args.is_string()) arguments = args.get<std::string>();
                                    else                  arguments = args.dump();
                                } else {
                                    arguments = "{}";
                                }
                            }
                            const std::string item_id = tc.value("item_id", std::string());
                            nlohmann::json fc_item{
                                {"type",      "function_call"},
                                {"call_id",   call_id},
                                {"name",      std::move(name)},
                                {"arguments", std::move(arguments)}
                            };
                            if (!item_id.empty())
                                fc_item["id"] = item_id;
                            input.push_back(std::move(fc_item));
                        }
                    }
                } else {
                    input.push_back(nlohmann::json{
                        {"type",    "message"},
                        {"role",    "assistant"},
                        {"content", nlohmann::json::array({
                            nlohmann::json{{"type","output_text"},{"text", m.content}}
                        })}
                    });
                }
            } else if (m.role == "tool") {
                input.push_back(nlohmann::json{
                    {"type",    "function_call_output"},
                    {"call_id", m.tool_call_id},
                    {"output",  m.content}
                });
            }
        }
        request["input"] = std::move(input);
        if (!instructions.empty())
            request["instructions"] = instructions;
        if (n_tools > 0) {
            request["tools"] = responses_tools;
            if (!tool_choice.is_null() && !tool_choice.is_discarded())
                request["tool_choice"] = tool_choice;
            else
                request["tool_choice"] = "auto";
        }
    }

    if (plan_token)
        shape_chatgpt_plan_request(request);
    sanitize_responses_request_utf8(request, "responses-stream");

    request["stream"] = true;
    logger::trace("LLMClient[responses-stream]: request stream=true");

    std::string text_accum;
    std::string reasoning_accum;
    std::size_t reasoning_delta_count = 0;
    bool saw_output_text_delta = false;
    bool saw_reasoning_stream_delta = false;
    std::string sse_buf;
    nlohmann::json completed_response;
    std::string stream_error;
    // output_item.done already ingested these; response.completed.output[] must
    // not push the same function_call / reasoning item again (that doubled
    // tool_calls and caused every tool to run twice in the agent loop).
    std::unordered_set<std::string> seen_output_item_keys;

    auto append_reasoning_delta = [&](const std::string& d) {
        if (d.empty()) return;
        reasoning_accum += d;
        ++reasoning_delta_count;
        saw_reasoning_stream_delta = true;
    };

    auto output_item_dedupe_key = [](const nlohmann::json& item) -> std::string {
        const std::string itype = item.value("type", std::string());
        if (itype == "function_call") {
            if (item.contains("call_id") && item["call_id"].is_string())
                return "fc:" + item["call_id"].get<std::string>();
            if (item.contains("id") && item["id"].is_string())
                return "fc:" + item["id"].get<std::string>();
            return "fc:" + item.value("name", std::string());
        }
        if (item.contains("id") && item["id"].is_string())
            return itype + ":" + item["id"].get<std::string>();
        return {};
    };

    auto process_output_item = [&](const nlohmann::json& item) {
        const std::string itype = item.value("type", std::string());
        const std::string dedupe_key = output_item_dedupe_key(item);
        if (!dedupe_key.empty() && !seen_output_item_keys.insert(dedupe_key).second)
            return;

        if (itype == "reasoning") {
            res.responses_output_items.push_back(item);
            if (!saw_reasoning_stream_delta
                    && item.contains("summary") && item["summary"].is_array()) {
                for (const auto& s : item["summary"]) {
                    if (s.value("type", std::string()) == "summary_text"
                            && s.contains("text") && s["text"].is_string()) {
                        if (!reasoning_accum.empty()) reasoning_accum += '\n';
                        reasoning_accum += s["text"].get<std::string>();
                    }
                }
            }
        } else if (itype == "message") {
            // Deltas already populated text_accum; replaying output[].message
            // would duplicate (or triplicate) the final assistant string.
            if (saw_output_text_delta) return;
            if (item.contains("content") && item["content"].is_array()) {
                for (const auto& c : item["content"]) {
                    const std::string ctype = c.value("type", std::string());
                    if ((ctype == "output_text" || ctype == "text")
                            && c.contains("text") && c["text"].is_string()) {
                        text_accum += c["text"].get<std::string>();
                    }
                }
            }
        } else if (itype == "function_call") {
            res.responses_output_items.push_back(item);
            ToolCall call;
            call.item_id = item.value("id", std::string());
            call.id = item.contains("call_id") && item["call_id"].is_string()
                      ? item["call_id"].get<std::string>()
                      : call.item_id;
            call.name = item.value("name", std::string());
            const std::string args_str = item.value("arguments", std::string("{}"));
            auto parsed = nlohmann::json::parse(args_str, nullptr, false);
            call.arguments = parsed.is_discarded() ? nlohmann::json::object() : std::move(parsed);
            res.tool_calls.push_back(std::move(call));
        } else {
            res.responses_output_items.push_back(item);
        }
    };

    const auto t_start = std::chrono::steady_clock::now();
    ScopedLlmCancelCheck cancel_scope(&cancel_check_);
    try {
        liboai::Response response = oai_impl.Responses->create(
            request,
            std::optional<liboai::Responses::StreamCallback>(
                [&](std::string chunk, intptr_t) -> bool {
                    // Normalize CRLF framing to LF so SSE block splitting works
                    // across providers/proxies (OpenRouter, LiteLLM, etc).
                    for (char ch : chunk) {
                        if (ch != '\r') sse_buf.push_back(ch);
                    }
                    for (;;) {
                        const size_t sep = sse_buf.find("\n\n");
                        if (sep == std::string::npos) break;
                        std::string evt = sse_buf.substr(0, sep);
                        sse_buf.erase(0, sep + 2);
                        std::string data;
                        std::string event_name;
                        size_t line_start = 0;
                        while (line_start <= evt.size()) {
                            size_t line_end = evt.find('\n', line_start);
                            if (line_end == std::string::npos) line_end = evt.size();
                            std::string line = evt.substr(line_start, line_end - line_start);
                            if (line.rfind("data:", 0) == 0) {
                                std::string payload = line.size() > 5 ? line.substr(5) : std::string();
                                if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
                                if (!data.empty()) data.push_back('\n');
                                data += payload;
                            } else if (line.rfind("event:", 0) == 0) {
                                event_name = line.size() > 6 ? line.substr(6) : std::string();
                                if (!event_name.empty() && event_name[0] == ' ')
                                    event_name.erase(0, 1);
                            }
                            if (line_end == evt.size()) break;
                            line_start = line_end + 1;
                        }
                        if (data.empty() || data == "[DONE]") continue;
                        const auto j = nlohmann::json::parse(data, nullptr, false);
                        if (j.is_discarded() || !j.is_object()) continue;
                        std::string typ = j.value("type", std::string());
                        if (typ.empty()) typ = event_name;
                        if (typ == "error") {
                            std::string msg;
                            if (j.contains("error") && j["error"].is_object())
                                msg = j["error"].value("message", std::string());
                            if (msg.empty())
                                msg = j.value("message", std::string());
                            if (msg.empty())
                                msg = "Responses stream error";
                            stream_error = std::move(msg);
                            logger::error("LLMClient[responses-stream]: " + stream_error);
                            // Keep reading so curl finishes and the caller reports
                            // stream_error. Returning false here is CURLE_WRITE_ERROR
                            // ("Failed writing received data to disk/application").
                            return true;
                        }
                        if (typ == "response.output_text.delta"
                                && j.contains("delta") && j["delta"].is_string()) {
                            const std::string d = j["delta"].get<std::string>();
                            saw_output_text_delta = true;
                            text_accum += d;
                            if (on_text_delta && !d.empty()) {
                                if (!on_text_delta(d)) {
                                    stream_error = "Responses stream cancelled by callback";
                                    logger::error("LLMClient[responses-stream]: " + stream_error);
                                    return false;
                                }
                            }
                        } else if ((typ == "response.reasoning_summary_text.delta"
                                    || typ == "response.reasoning_text.delta")
                                && j.contains("delta") && j["delta"].is_string()) {
                            append_reasoning_delta(j["delta"].get<std::string>());
                        } else if (typ == "response.reasoning_summary_text.done"
                                && j.contains("text") && j["text"].is_string()) {
                            // Some proxies emit only .done (no prior deltas).
                            if (reasoning_accum.empty())
                                reasoning_accum = j["text"].get<std::string>();
                        } else if (typ == "response.reasoning_text.done"
                                && j.contains("text") && j["text"].is_string()) {
                            if (reasoning_accum.empty())
                                reasoning_accum = j["text"].get<std::string>();
                        } else if (typ == "response.output_item.done"
                                   && j.contains("item") && j["item"].is_object()) {
                            process_output_item(j["item"]);
                        } else if (typ == "response.completed" || typ == "response.done") {
                            if (j.contains("response") && j["response"].is_object())
                                completed_response = j["response"];
                        } else if (typ == "response.failed") {
                            std::string msg;
                            if (j.contains("response") && j["response"].is_object()) {
                                const auto& resp = j["response"];
                                if (resp.contains("error") && resp["error"].is_object())
                                    msg = resp["error"].value("message", std::string());
                            }
                            if (stream_error.empty()) {
                                stream_error = msg.empty() ? std::string("Responses stream failed") : std::move(msg);
                                logger::error("LLMClient[responses-stream]: " + stream_error);
                            }
                            return true;
                        }
                    }
                    return true;
                }));

        if (!stream_error.empty()) {
            res.success = false;
            res.error = stream_error;
            return res;
        }

        nlohmann::json j = completed_response;
        if ((j.is_null() || j.empty()) && !response.content.empty()) {
            const auto parsed = nlohmann::json::parse(response.content, nullptr, false);
            if (parsed.is_object()) j = parsed;
        }

        if (j.is_object()) {
            if (j.contains("id") && j["id"].is_string())
                res.response_id = j["id"].get<std::string>();
            if (j.contains("output") && j["output"].is_array()) {
                for (const auto& item : j["output"])
                    process_output_item(item);
            }
            try {
                nlohmann::json meta = nlohmann::json::object();
                for (auto it = j.begin(); it != j.end(); ++it) {
                    if (it.key() == "output") continue;
                    meta[it.key()] = it.value();
                }
                if (!meta.empty()) res.provider_meta_json = meta.dump();
            } catch (...) {}
        } else {
            try {
                const auto m = nlohmann::json::parse(response.content, nullptr, false);
                if (m.is_object()) res.provider_meta_json = m.dump();
            } catch (...) {}
        }

        res.text = std::move(text_accum);
        res.reasoning_text = std::move(reasoning_accum);
        res.success = true;

        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t_start).count();
        logger::trace("LLMClient[responses-stream]: ok in " + std::to_string(elapsed_ms)
                     + " ms text_chars=" + std::to_string(res.text.size())
                     + " tool_calls=" + std::to_string(res.tool_calls.size())
                     + (res.reasoning_text.empty() ? " reasoning=none"
                        : " reasoning_chars=" + std::to_string(res.reasoning_text.size())
                          + " reasoning_deltas=" + std::to_string(reasoning_delta_count)));
    } catch (const liboai::exception::OpenAIException& e) {
        const std::string what = e.what();
        if (!stream_error.empty()) {
            res.success = false;
            res.cancelled = stream_error.find("cancelled by callback") != std::string::npos;
            res.error = stream_error;
            logger::error("LLMClient[responses-stream]: " + what);
            return res;
        }
        if (looks_cancelled_by_http_callback(what)
                || what.find("Failed writing received data") != std::string::npos) {
            res.success = false;
            res.cancelled = true;
            res.error = "Responses stream request cancelled";
            return res;
        }
        std::string detail_for_error;
        const long http_status = e.HttpStatusCode();
        const std::string& body = e.ResponseBody();
        if (http_status > 0 || !body.empty()) {
            detail_for_error = " [responses_http="
                + std::to_string(http_status)
                + " body=" + preview_json_or_text(body, 1200) + "]";
            const std::string extracted = extract_provider_error_detail(body);
            if (!extracted.empty())
                detail_for_error += " [responses_provider_detail=" + extracted + "]";
        }
        (void)detail_for_error;
        res.success = false;
        res.error = compose_user_llm_error(what, http_status, body, target_model);
    } catch (const std::exception& e) {
        const std::string what = e.what();
        if (looks_cancelled_by_http_callback(what)) {
            res.success = false;
            res.cancelled = true;
            res.error = "Responses stream request cancelled";
            return res;
        }
        res.success = false;
        res.error = compose_user_llm_error(what, 0, std::string{}, target_model);
    } catch (...) {
        res.success = false;
        res.error = "Unknown error inside LLMClient::execute_responses_stream.";
    }

    return res;
}

LLMResponse LLMClient::execute_realtime_text(
    const std::vector<ChatMessage>& messages,
    std::function<bool(const std::string&)> on_text_delta)
{
    LLMResponse res;
#if !(defined(FEATURE_REALTIME) && FEATURE_REALTIME)
    res.success = false;
    res.error = "execute_realtime_text: FEATURE_REALTIME is disabled in this build";
    return res;
#else
    if (api_key_.empty()) {
        res.success = false;
        res.error = "API Key is empty.";
        return res;
    }
    const RealtimePromptParts parts = derive_realtime_prompt_parts(messages);
    if (parts.user.empty()) {
        res.success = false;
        res.error = "messages is empty (no user content to send).";
        return res;
    }

    const std::string target_model = model_.empty() ? "gpt-realtime" : model_;
    const std::string ws_url = realtime_base_url_for(base_url_) + "?model=" + target_model;
    logger::trace("LLMClient[realtime]: WS connect " + ws_url);

    CURL* c = curl_easy_init();
    if (!c) {
        res.success = false;
        res.error = "curl_easy_init failed";
        return res;
    }

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, ("Authorization: Bearer " + api_key_).c_str());
    // LiteLLM also accepts x-litellm-api-key (alias), but Bearer works for both.
    curl_easy_setopt(c, CURLOPT_URL, ws_url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    apply_android_curl_ca(c);
    if (llm_timeout_ms_ > 0)
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, static_cast<long>(llm_timeout_ms_));

    const auto t_start = std::chrono::steady_clock::now();
    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        res.success = false;
        res.error = std::string("Realtime connect failed: ") + curl_easy_strerror(rc);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(c);
        return res;
    }

    auto send_event = [&](const nlohmann::json& j) -> bool {
        const std::string wire = j.dump();
        size_t sent = 0;
        const CURLcode s = curl_ws_send(c, wire.data(), wire.size(), &sent, 0, CURLWS_TEXT);
        return s == CURLE_OK;
    };

    // session.update — GA /v1/realtime requires session.type; set the (text-only)
    // output modality and instructions here too. Always sent: the GA endpoint rejects
    // a response without a prior session.type.
    {
        nlohmann::json session{
            {"type", "realtime"},
            {"model", target_model},
            {"output_modalities", nlohmann::json::array({"text"})},
        };
        if (!parts.system.empty()) session["instructions"] = parts.system;
        nlohmann::json upd{{"type", "session.update"}, {"session", session}};
        (void)send_event(upd); // best-effort; ignored if the endpoint doesn't support it
    }

    nlohmann::json add_user{
        {"type", "conversation.item.create"},
        {"item", {
            {"type", "message"},
            {"role", "user"},
            {"content", nlohmann::json::array({
                nlohmann::json{{"type","input_text"}, {"text", parts.user}}
            })}
        }}
    };
    if (!send_event(add_user)) {
        res.success = false;
        res.error = "Realtime send failed: conversation.item.create";
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(c);
        return res;
    }

    nlohmann::json create{
        {"type", "response.create"},
        {"response", {{"output_modalities", nlohmann::json::array({"text"})}}}
    };
    if (!send_event(create)) {
        res.success = false;
        res.error = "Realtime send failed: response.create";
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(c);
        return res;
    }

    char tmp[16384];
    std::string frame_buf;
    std::string out_text;
    bool saw_text_delta = false;
    bool done = false;
    while (!done) {
        size_t nrecvd = 0;
        const curl_ws_frame* meta = nullptr;
        rc = curl_ws_recv(c, tmp, sizeof(tmp), &nrecvd, &meta);
        if (rc == CURLE_AGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (rc != CURLE_OK) {
            res.success = false;
            res.error = std::string("Realtime recv failed: ") + curl_easy_strerror(rc);
            break;
        }
        if (meta && (meta->flags & CURLWS_CLOSE)) {
            done = true;
            break;
        }

        if (nrecvd > 0) frame_buf.append(tmp, nrecvd);
        if (meta && meta->bytesleft == 0 && !frame_buf.empty()) {
            try {
                const auto j = nlohmann::json::parse(frame_buf, nullptr, false);
                if (j.is_object()) {
                    const std::string typ = j.value("type", std::string());

                    if (typ == "error") {
                        res.success = false;
                        res.error = "Realtime API error: " + j.dump();
                        done = true;

                    } else if ((typ == "response.text.delta"
                                || typ == "response.output_text.delta")
                               && j.contains("delta") && j["delta"].is_string()) {
                        saw_text_delta = true;
                        const std::string delta = j["delta"].get<std::string>();
                        out_text += delta;
                        // Stream delta to caller (e.g. chat UI TextDelta events).
                        if (on_text_delta && !delta.empty()) {
                            if (!on_text_delta(delta)) {
                                res.cancelled = true;
                                done = true;
                            }
                        }

                    } else if ((typ == "response.text.done"
                                || typ == "response.output_text.done")
                               && j.contains("text") && j["text"].is_string()) {
                        if (!saw_text_delta)
                            out_text = j["text"].get<std::string>();

                    } else if (typ == "response.output_item.done"
                               && j.contains("item") && j["item"].is_object()) {
                        if (!saw_text_delta) {
                            const auto& item = j["item"];
                            if (item.value("type", std::string()) == "message"
                                    && item.contains("content") && item["content"].is_array()) {
                                for (const auto& c : item["content"]) {
                                    if ((c.value("type", std::string()) == "output_text"
                                         || c.value("type", std::string()) == "text")
                                            && c.contains("text") && c["text"].is_string()) {
                                        out_text += c["text"].get<std::string>();
                                    }
                                }
                            }
                        }

                    } else if (typ == "response.done") {
                        // Fallback text extraction when no deltas were received.
                        if (!saw_text_delta && out_text.empty()
                                && j.contains("response") && j["response"].is_object()) {
                            const auto& r = j["response"];
                            if (r.contains("output") && r["output"].is_array()) {
                                for (const auto& item : r["output"]) {
                                    if (!item.is_object()) continue;
                                    if (item.value("type", std::string()) != "message") continue;
                                    if (!item.contains("content") || !item["content"].is_array()) continue;
                                    for (const auto& c2 : item["content"]) {
                                        if ((c2.value("type", std::string()) == "output_text"
                                             || c2.value("type", std::string()) == "text")
                                                && c2.contains("text") && c2["text"].is_string()) {
                                            out_text += c2["text"].get<std::string>();
                                        }
                                    }
                                }
                            }
                        }
                        // Capture token usage → provider_meta_json for the agent loop.
                        try {
                            if (j.contains("response") && j["response"].is_object()
                                    && j["response"].contains("usage")
                                    && j["response"]["usage"].is_object()) {
                                nlohmann::json meta_out = nlohmann::json::object();
                                meta_out["usage"] = j["response"]["usage"];
                                if (j["response"].contains("model"))
                                    meta_out["model"] = j["response"]["model"];
                                res.provider_meta_json = meta_out.dump();
                            }
                        } catch (...) {}
                        done = true;
                    }
                }
            } catch (...) {}
            frame_buf.clear();
        }
    }

    if (!res.cancelled && (res.success || res.error.empty())) {
        res.success = true;
        res.text = out_text;
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t_start).count();
        logger::trace("LLMClient[realtime]: done in " + std::to_string(elapsed_ms)
                     + " ms text_chars=" + std::to_string(out_text.size()));
    }

    size_t sent = 0;
    curl_ws_send(c, "", 0, &sent, 0, CURLWS_CLOSE);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return res;
#endif // FEATURE_REALTIME
}

} // namespace kbot
} // namespace polymech

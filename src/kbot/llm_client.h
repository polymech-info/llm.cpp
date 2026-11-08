#pragma once

#include <string>
#include <vector>
#include <functional>
#include <optional>

#include <nlohmann/json.hpp>

#include "kbot.h"
#include "polymech_export.h"

namespace polymech {
namespace kbot {

/**
 * One entry in a tool-aware chat conversation. Mirrors the OpenAI Chat
 * Completions message shape so that `execute_chat_messages` can hand the
 * vector to `liboai::ChatCompletion::create` without any further
 * translation.
 *
 * Roles:
 *   "system"     — instructions only; `content` is the system prompt.
 *   "user"       — user input; `content` is the prompt text.
 *   "assistant"  — model output. Either `content` (text) or `tool_calls`
 *                  (an array of function-call requests) is set, not both.
 *                  When `tool_calls` is non-empty, `content` is null/empty.
 *   "tool"       — result of a previously-requested tool call. Set
 *                  `tool_call_id` to the matching id from a prior
 *                  assistant `tool_calls[i].id`; `content` is the tool's
 *                  textual result (typically a JSON envelope serialised
 *                  to a string).
 */
struct ChatMessage {
    std::string    role;
    std::string    content;
    std::string    tool_call_id;   // when role == "tool"
    nlohmann::json tool_calls;     // when role == "assistant" with calls
    /**
     * Responses API only: raw output[] items (reasoning + function_call, in order)
     * from the round that produced this assistant message. When non-empty,
     * execute_responses() replays these verbatim as input[] items instead of
     * re-translating tool_calls[] — required so reasoning items precede the
     * function_call items they generated (enforced by reasoning models).
     */
    nlohmann::json responses_output_items = nlohmann::json::array();
};

/**
 * One parsed entry from `LLMResponse::tool_calls`. The arguments string is
 * already-parsed JSON (so callers don't have to nlohmann-parse twice).
 */
struct ToolCall {
    std::string    id;       // call_id — used in function_call_output.call_id (both APIs)
    std::string    item_id;  // Responses API only: output[].id ("fc_…"); replayed as input[].function_call.id
    std::string    name;
    nlohmann::json arguments = nlohmann::json::object();  // parsed; empty object on parse failure or missing field
};

struct LLMResponse {
    std::string text;
    /** Chat Completions `choices[0].finish_reason` when captured (e.g. "stop", "length"). */
    std::string finish_reason;
    bool success = false;
    /** Set when a streaming callback asked to abort (e.g. realtime on_text_delta returned false). */
    bool cancelled = false;
    std::string error;
    /** Top-level chat completion JSON minus `choices` (usage, model, id, OpenRouter extras). Empty if not captured. */
    std::string provider_meta_json;
    /**
     * Modern multi-tool calling (Chat Completions, post-2023-11). Empty for
     * text-only responses. When non-empty, `text` will typically be empty —
     * the agent loop should dispatch each entry, append the assistant turn
     * (via `Conversation::AddAssistantToolCalls`) plus a `tool` message per
     * call (via `Conversation::AddToolResult`), and call back in.
     */
    std::vector<ToolCall> tool_calls;
    /**
     * Reasoning / thinking text from the Responses API (output[].type="reasoning",
     * summary[].text concatenated). Empty for chat/completions or when the model
     * does not emit reasoning. Never sent back to the model.
     */
    std::string reasoning_text;
    /**
     * Responses API only: the raw non-message output[] items (type="reasoning",
     * type="function_call", …) in their original order. Must be replayed verbatim as
     * input[] items in the next round so the API can link reasoning items to the
     * function_call items they generated (required by reasoning models like gpt-5).
     * Used only in stateless mode; ignored when previous_response_id is provided.
     */
    nlohmann::json responses_output_items = nlohmann::json::array();
    /**
     * Responses API only: the id of this response (e.g. "resp_abc123").
     * Pass as previous_response_id in the next execute_responses() call to
     * enable stateful (chained) mode — subsequent rounds only send the new
     * tool results; the full history and tool schemas are cached server-side.
     */
    std::string response_id;
};

/** Process-once DNS/TLS warmup so the first chat POST can reuse libcurl's share. */
POLYMECH_API void warmup_llm_http_async();

class POLYMECH_API LLMClient {
public:
    // Initialize the client with the options (api_key, model, router).
    explicit LLMClient(const KBotOptions& opts);
    ~LLMClient();

    // Execute a basic chat completion using the provided prompt.
    LLMResponse execute_chat(const std::string& prompt);

    /**
     * Execute a multi-turn chat completion with optional tool support.
     * Mirrors OpenAI Chat Completions: pass the full message history each
     * call (kbot does not maintain conversation state across calls).
     *
     * @param messages       Conversation so far (system + user + assistant +
     *                        tool messages, in order).
     * @param tools          Optional `tools` array — each entry must follow
     *                        the OpenAI shape:
     *                        `{"type":"function","function":{"name":"...",
     *                          "description":"...","parameters":{<JSON-Schema>}}}`.
     *                        Pass `nullptr` (default) for text-only mode.
     * @param tool_choice    Optional `tool_choice` selector. Common values:
     *                        `"auto"` (default), `"none"`, or
     *                        `{"type":"function","function":{"name":"X"}}`.
     *
     * @param on_text_delta  When set, POST with stream=true and invoke this
     *                       for each assistant content delta as it arrives.
     *                       Return false to abort. Tool-call argument
     *                       fragments are accumulated and not forwarded.
     *                       Empty (default) keeps the buffered request.
     *
     * @return LLMResponse with either `text` populated, `tool_calls`
     *         populated, or both empty (model failure / API error).
     */
    LLMResponse execute_chat_messages(
        const std::vector<ChatMessage>& messages,
        const nlohmann::json& tools = nlohmann::json(),
        const nlohmann::json& tool_choice = nlohmann::json(),
        const std::optional<std::string>& reasoning_effort = std::nullopt,
        std::function<bool(const std::string&)> on_text_delta = {}
    );

    /**
     * Execute a turn using the OpenAI Responses API (POST /responses).
     * Accepts the same message history as execute_chat_messages and returns the
     * same LLMResponse, so the agent loop in agent.cpp is unchanged.
     *
     * Translations applied internally:
     *   - system message     → "instructions" field
     *   - user/assistant/tool messages → "input[]" array items
     *   - tool catalog       → flat Responses API tool format (no nested "function" key)
     *   - output[].type=message       → res.text
     *   - output[].type=function_call → res.tool_calls (id = call_id for feedback)
     *
     * Supported providers: OpenAI, OpenRouter, LiteLLM proxy (bridges non-native models).
     */
    /**
     * @param previous_response_id  When non-empty, activates stateful (chained) mode:
     *   only tool results at the tail of `messages` are sent; full conversation history
     *   and tool schemas are cached server-side. Pass resp.response_id from the
     *   previous round. Leave empty for the first round of every turn.
     */
    LLMResponse execute_responses(
        const std::vector<ChatMessage>& messages,
        const nlohmann::json& tools = nlohmann::json(),
        const nlohmann::json& tool_choice = nlohmann::json(),
        const std::string& previous_response_id = {}
    );

    /**
     * Execute /responses with SSE streaming enabled and parse incremental text/tool events.
     * The callback receives incremental assistant text deltas.
     */
    LLMResponse execute_responses_stream(
        const std::vector<ChatMessage>& messages,
        const nlohmann::json& tools = nlohmann::json(),
        const nlohmann::json& tool_choice = nlohmann::json(),
        const std::string& previous_response_id = {},
        std::function<bool(const std::string&)> on_text_delta = {}
    );

    /**
     * Experimental: execute a text-only prompt through OpenAI Realtime API over WebSocket.
     * Supports direct OpenAI endpoint and LiteLLM /realtime proxy.
     * Current constraints: no tool calls; single response per connection.
     * Requires FEATURE_REALTIME=1.
     *
     * @param on_text_delta  Optional streaming callback invoked for each text delta.
     *                       Return false to abort (sets res.cancelled = true).
     */
    LLMResponse execute_realtime_text(
        const std::vector<ChatMessage>& messages,
        std::function<bool(const std::string&)> on_text_delta = {}
    );

private:
    std::string api_key_;
    std::string model_;
    std::string router_;
    std::string base_url_;
    int llm_timeout_ms_ = 0;
    /** Parsed in execute_chat; raw JSON from KBotOptions::response_format_json */
    std::string response_format_json_;
    std::function<bool()> cancel_check_;
};

} // namespace kbot
} // namespace polymech

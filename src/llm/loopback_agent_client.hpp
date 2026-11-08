#pragma once

#include "kbot.h"
#include "llm_client.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace media::llm::agent::loopback_client {

/// Returns true when `router` is "loopback" or "mock".
/// Gate for agent_factory: checked before the llama path.
bool is_loopback_router(std::string_view router);

/// Execute one agent round from a scripted JSON array.
///
/// `script_source` resolved in priority order:
///   1. Value passed here (inline JSON array or '@<path>' file reference).
///   2. PM_LOOPBACK_SCRIPT environment variable (same syntax).
///
/// Script format — a JSON array; each element is one "LLM response":
///   [
///     { "tool_calls": [{"id":"c1","name":"write_file","arguments":{"path":"...","content":"..."}}] },
///     { "text": "Done." }
///   ]
///
/// - `tool_calls[].arguments` may be an object or a JSON-encoded string.
/// - When a round has both "text" and "tool_calls", tool_calls wins (loop continues).
/// - Round index advances per call (process-local, mutex-protected).
/// - When the script changes (source string differs from last call), the index
///   resets to 0 so sequential test cases can each supply a fresh script.
/// - When exhausted, returns a success response with empty tool_calls so the
///   loop terminates cleanly.
/// - `cancel_check` / `timeout_ms` honoured for loop parity.
///
/// Mirror signature of llama_client::execute_chat_messages so agent_factory
/// can dispatch uniformly.
polymech::kbot::LLMResponse execute_chat_messages(
    const std::vector<polymech::kbot::ChatMessage>& messages,
    const std::string& script_source,
    const nlohmann::json& tools        = nlohmann::json(),
    const nlohmann::json& tool_choice  = nlohmann::json(),
    int timeout_ms                     = 0,
    std::function<bool()> cancel_check = {});

/// Reset the process-local round index and clear the cached script.
/// Call between test cases when running multiple scenarios in one process.
void reset_script();

} // namespace media::llm::agent::loopback_client

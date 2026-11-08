#pragma once

#include "agent.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace media::llm::agent {

/// Append one agent Event to a JSON log array (agent.json `events` field).
PM_CHAT_API void append_agent_log_event(nlohmann::json& events, const Event& e);

/// Human-readable one-line stderr preview for a tool call (name + truncated args).
PM_CHAT_API std::string format_agent_tool_call_cli_line(const std::string& tool_name,
                                                         const nlohmann::json& payload,
                                                         std::size_t max_chars = 160);

/// Human-readable one-line stderr preview for a tool result (name + truncated summary/content).
PM_CHAT_API std::string format_agent_tool_result_cli_line(const std::string& tool_name,
                                                            const std::string& text,
                                                            const nlohmann::json& payload,
                                                            std::size_t max_chars = 160);

/// Compact status suffix for chat UI tool rows. Supports batch summaries,
/// result arrays, count-style search tools, and plain {ok} envelopes.
PM_CHAT_API std::string format_agent_tool_result_status(const nlohmann::json& payload);

struct AgentRunLogWriteOptions {
    std::string source;          // e.g. "chat_workbench", "realtime_voice"
    std::string run_id;
    std::string display_name;
    std::string agent_session_id;
    std::string chat_session_id; // secure log doc owner; falls back to agent_session_id
    std::string runner;          // native | claude-code | …
    std::filesystem::path log_path; // plaintext fallback when secure storage off
};

/// Build the agent.json document (does not write to disk).
PM_CHAT_API nlohmann::json build_agent_run_log(const AgentRunLogWriteOptions& opts,
                                                const Turn& turn,
                                                const ProviderSettings& prov,
                                                const nlohmann::json& log_events,
                                                const Result& res);

/// Persist a pre-built agent.json document.
PM_CHAT_API void persist_agent_run_log_json(const AgentRunLogWriteOptions& opts,
                                             const nlohmann::json& logj);

/// Build and persist a completed agent run log.
PM_CHAT_API void persist_agent_run_log(const AgentRunLogWriteOptions& opts,
                                        const Turn& turn,
                                        const ProviderSettings& prov,
                                        const nlohmann::json& log_events,
                                        const Result& res);

} // namespace media::llm::agent

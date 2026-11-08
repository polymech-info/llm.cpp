#pragma once

#include "agent_runner.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace media::llm::agent {

/// Parse CLI/preset runner names: native | claude | claude-code | codex.
std::optional<AgentRunnerKind> parse_agent_runner_kind(std::string_view name);

/// Claude external session id stored under session memory_state.external_agents.
std::string external_session_lookup(const std::string& pixlwiz_session_id,
                                    AgentRunnerKind runner);
void external_session_store(const std::string& pixlwiz_session_id,
                            AgentRunnerKind runner,
                            const std::string& external_session_id);

std::string extract_external_session_id(const Result& result);

} // namespace media::llm::agent

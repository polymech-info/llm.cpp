#pragma once

#include "agent_runner.hpp"

namespace media::llm::agent {

Result run_codex_agent_turn(const Turn& turn, const AgentRunnerOptions& opt, EventCallback on_event);

} // namespace media::llm::agent

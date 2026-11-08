#pragma once

#include "agent.hpp"

#include <nlohmann/json.hpp>

namespace media::llm::agent {

/// Stamp chat-web `workflow` (or top-level aliases) onto an existing Turn.
/// Call after launch / preset policy so the recipe can override those knobs.
void apply_workflow_send_json(const nlohmann::json& send, Turn& turn);

} // namespace media::llm::agent

#pragma once

#include "llm/agent_feature.hpp"

#include <memory>

namespace media::llm::agent::AgentFactory {

/// Context-phase feature: publish Turn.workflow_inject into system_extra.
std::shared_ptr<IAgentFeature> feature_workflow();

} // namespace media::llm::agent::AgentFactory

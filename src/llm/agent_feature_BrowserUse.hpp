#pragma once

#include "llm/agent_feature.hpp"

#include <memory>

namespace media::llm::agent::AgentFactory {

/// Plan-phase feature: snapshot active WebView context and retain browser tools.
std::shared_ptr<IAgentFeature> feature_browser_use();

} // namespace media::llm::agent::AgentFactory

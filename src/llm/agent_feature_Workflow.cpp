#include "llm/agent_feature_Workflow.hpp"

#include "logging/logger.h"

namespace media::llm::agent {
namespace {

class WorkflowFeature final : public IAgentFeature {
public:
    FeaturePhase phase() const noexcept override { return FeaturePhase::Context; }
    const char* name() const noexcept override { return "workflow"; }

    void on_prepare(Turn& turn, const ProviderSettings&, PreparedRun&) override
    {
        if (turn.workflow_inject.empty())
            return;
        if (turn.system_extra.find(turn.workflow_inject) != std::string::npos)
            return;
        if (turn.system_extra.empty())
            turn.system_extra = turn.workflow_inject;
        else
            turn.system_extra += "\n" + turn.workflow_inject;
        logger::debug(std::string("[workflow] context injected id=")
                      + (turn.workflow_id.empty() ? "-" : turn.workflow_id));
    }
};

} // namespace

namespace AgentFactory {

std::shared_ptr<IAgentFeature> feature_workflow()
{
    return std::make_shared<WorkflowFeature>();
}

} // namespace AgentFactory
} // namespace media::llm::agent

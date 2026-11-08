#include "agent.hpp"
#include "agent_factory.hpp"

namespace media::llm::agent {

Result run_turn(const Turn&             turn,
                const ProviderSettings& provider,
                EventCallback           on_event)
{
    return AgentFactory::default_agent().run(turn, provider, std::move(on_event));
}

} // namespace media::llm::agent

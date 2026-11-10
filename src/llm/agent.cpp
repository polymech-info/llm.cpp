#include "agent.hpp"
#include "agent_factory.hpp"

#include "core/cli_cancel.hpp"

namespace media::llm::agent {

Result run_turn(const Turn&             turn,
                const ProviderSettings& provider,
                EventCallback           on_event)
{
    return AgentFactory::default_agent().run(turn, provider, std::move(on_event));
}

Result run_spoken_page(const std::string& page_text,
                       const std::string& prompt,
                       const std::string& language,
                       const ProviderSettings& provider)
{
    ProviderSettings text = provider;
    text.max_iterations = 0;

    Turn turn;
    turn.planner_enabled = false;
    turn.planner_strategy = PlannerStrategy::Disabled;
    turn.skills_enabled = false;
    turn.load_agent_md = false;
    turn.self_check_enabled = false;
    turn.structured_memory_features = 0;
    turn.system_prompt_override_set = true;
    turn.system_prompt_override =
        "Rewrite this page into text that will be spoken. Follow the prompt. "
        "Return only that spoken text.";
    turn.user_prompt = page_text + "\n\nprompt: " + prompt;
    if (!language.empty())
        turn.user_prompt += "\nlanguage: " + language;

    AgentOptions options;
    options.features.push_back(AgentFactory::feature_system_prompt());
    return Agent(std::move(options)).run(turn, text, [](const Event&) {
        return !media::cli::cancel_requested();
    });
}

} // namespace media::llm::agent

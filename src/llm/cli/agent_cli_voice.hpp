#pragma once

#include "pm_image_cli_state.hpp"
#include "llm/agent.hpp"
#include "core/settings_runtime.hpp"

namespace pm::cli { class CliActivityScope; }

namespace pm_llm_agent_cli {

#if defined(FEATURE_STT) && FEATURE_STT
int cmd_llm_agent_mic(const PmImageCliState& st,
                      media::llm::agent::Turn base_turn,
                      const media::llm::agent::ProviderSettings& prov,
                      const media::runtime_settings::ChatProviderSettings& chat);
#if defined(FEATURE_REALTIME) && FEATURE_REALTIME
int cmd_llm_agent_realtime(PmImageCliState& st,
                           const media::runtime_settings::ChatProviderSettings& chat,
                           pm::cli::CliActivityScope* cli_activity);
#endif
#endif

} // namespace pm_llm_agent_cli

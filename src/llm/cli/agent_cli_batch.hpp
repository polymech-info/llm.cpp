#pragma once

#include "pm_image_cli_state.hpp"

namespace pm_llm_agent_cli {

#if defined(FEATURE_JQ) && FEATURE_JQ
int cmd_llm_agent_decide(PmImageCliState& st);
int cmd_llm_agent_each(PmImageCliState& st);
#if defined(FEATURE_LLAMA) && FEATURE_LLAMA
int cmd_llm_agent_dedupe(PmImageCliState& st);
#endif
#endif

} // namespace pm_llm_agent_cli

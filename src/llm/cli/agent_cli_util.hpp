#pragma once

#include "llm/cli/agent_cli_hud.hpp"
#include "llm/agent_cli.hpp"
#include "llm/agent_memory.hpp"

#include <nlohmann/json.hpp>

#ifndef FEATURE_HOME_LLM_SKILLS
#define FEATURE_HOME_LLM_SKILLS 1
#endif
#if FEATURE_HOME_LLM_SKILLS
#include "llm/agent_skills.hpp"
#endif

#include <string>

namespace pm_llm_agent_cli {

pm::llm::StructuredMemoryFeatures resolve_structured_memory_features(
    pm::llm::StructuredMemoryFeatures fallback, const PmImageCliState& st);

#if FEATURE_HOME_LLM_SKILLS
media::llm::skills::SkillSnapshot load_skill_snapshot_for_cwd(
    const std::string& cwd_hint,
    const media::llm::agent::Turn* turn_override = nullptr);
#endif

void log_llm_usage_aggregate_debug(const nlohmann::json& agg, long long wall_ms = -1);

bool apply_agent_system_prompt_option(const std::string& raw,
                                      media::llm::agent::Turn& turn,
                                      std::string& err);
bool apply_agent_planner_prompt_option(const std::string& raw,
                                       media::llm::agent::Turn& turn,
                                       std::string& err);

media::llm::agent::cliopt::AgentCliOptions agent_cli_options_from(const PmImageCliState& st);
void apply_agent_capability_opt_outs(media::llm::agent::Turn& turn, const PmImageCliState& st);

void append_session_turn_summary_if_enabled(const PmImageCliState& st,
                                            const std::string& session_task_id,
                                            const std::string& user_prompt,
                                            const media::llm::agent::Result& result,
                                            pm::llm::StructuredMemoryFeatures features);

int run_scheduler_blocking(const media::llm::agent::ProviderSettings& prov,
                           int timeout_s,
                           bool exit_when_idle);

} // namespace pm_llm_agent_cli

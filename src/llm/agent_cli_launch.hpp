#pragma once

#include "llm/agent_cli.hpp"
#include "llm/agent_launch.hpp"
#include "llm/agent_turn_options.hpp"

namespace media::llm::agent::cliopt {

inline launch::AgentLaunchSpec launch_spec_from(const AgentCliOptions& o)
{
    launch::AgentLaunchSpec spec;
    spec.preset = o.preset;
    spec.router = o.router;
    spec.model = o.model;
    spec.api_key = o.api_key;
    spec.base_url = o.base_url;
    spec.ai_consent_review = o.ai_consent_review;
    spec.ai_consent_router = o.ai_consent_router;
    spec.ai_consent_model = o.ai_consent_model;
    spec.timeout_ms = o.timeout_ms;
    spec.max_iterations = o.max_iterations;
    spec.no_tools = o.no_tools;
    spec.api_mode = o.api_mode;
    spec.streaming = o.streaming;
    spec.planner_enabled = o.planner;
    spec.planner_schema_budget = o.planner_budget;
    spec.ocr_provider = o.ocr_provider;
    spec.ocr_model = o.ocr_model;
    spec.recognition_provider = o.recognition_provider;
    spec.recognition_model = o.recognition_model;
    spec.image_provider = o.image_provider;
    spec.image_model = o.image_model;
    spec.video_provider = o.video_provider;
    spec.video_model = o.video_model;
    spec.stt_provider = o.stt_provider;
    spec.stt_model = o.stt_model;
    spec.tts_provider = o.tts_provider;
    spec.tts_model = o.tts_model;
    spec.tts_voice_id = o.tts_voice_id;
    spec.runner = o.runner;
    return spec;
}

inline void apply_capability_opt_outs(Turn& turn, const AgentCliOptions& o)
{
    if (o.no_mcp) {
        turn.mcp_tools_enabled = false;
        turn.tool_policy_override_set = true;
    }
    if (o.skills) {
        turn.skills_enabled = true;
        turn.skills_roaming_enabled = true;
        turn.skills_workspace_enabled = true;
        turn.skill_policy_override_set = true;
    } else if (o.no_skills) {
        turn.skills_enabled = false;
        turn.skill_policy_override_set = true;
    }
    if (o.no_planner) {
        turn.planner_enabled = false;
        turn.planner_strategy = PlannerStrategy::Disabled;
    }
    if (!o.context_reduction.empty()) {
        const auto mode = options::parse_context_reduction_mode(o.context_reduction);
        turn.context_reduction_mode = mode;
        if (mode != pm::llm::ContextReductionMode::Off) {
            turn.context_reduction_ratio = o.context_reduction_ratio;
            turn.context_reduction_min_bytes =
                static_cast<std::size_t>(o.context_reduction_min_bytes > 0
                    ? o.context_reduction_min_bytes : 0);
            turn.context_reduction_objective = o.context_reduction_objective;
        }
    }
}

} // namespace media::llm::agent::cliopt

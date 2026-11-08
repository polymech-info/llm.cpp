#pragma once

#include "llm/agent.hpp"
#include "core/settings_runtime.hpp"

#include <string>

namespace media::llm::agent::launch {

struct AgentLaunchSpec {
    std::string preset;
    std::string router;
    std::string model;
    std::string api_key;
    std::string base_url;
    bool ai_consent_review = false;
    std::string ai_consent_router;
    std::string ai_consent_model;
    int timeout_ms = 0;
    int max_iterations = 0;
    bool no_tools = false;
    std::string api_mode;
    std::string streaming = "on";
    bool planner_enabled = false;
    bool planner_override_set = false;
    bool use_chat_planner_default = false;
    int planner_schema_budget = 8;
    bool dry_run_credentials = false;

    /// Per-run tool provider/model overrides. Non-empty values win over the
    /// preset/global chat settings and are visible to all tool dispatch paths
    /// (ocr_text, image_understand, image_create, create_video) via
    /// push_chat_settings_override() in llm_image_tool_defaults.
    std::string ocr_provider;
    std::string ocr_model;
    std::string recognition_provider;
    std::string recognition_model;
    std::string image_provider;
    std::string image_model;
    std::string video_provider;
    std::string video_model;

    /// Per-run STT / TTS overrides. Applied to chat_settings so --mic mode and
    /// audio_transcribe tool picks them up without touching settings.json.
    /// Use stt_provider="whisper" + stt_model="base.en" for fully local STT.
    std::string stt_provider;
    std::string stt_model;
    std::string tts_provider;
    std::string tts_model;
    std::string tts_voice_id;

    /// External agent backend for delegated runs: native (default) |
    /// claude-code | codex. Also honored via PM_LLM_AGENT_RUNNER.
    std::string runner;
};

struct AgentRunName {
    std::string display_name;
    std::string safe_id;
};

struct AgentLaunchResolved {
    media::runtime_settings::ChatProviderSettings chat_settings;
    media::runtime_settings::ChatProviderPreset selected_preset;
    bool has_preset = false;
    ProviderSettings provider;
    /// Effective agent backend after merging explicit `spec.runner`, preset `runner`,
    /// and the default (`native`). Used by CLI, voice bridge, and (future) chat send.
    std::string effective_runner = "native";
};

bool resolve_agent_launch(const AgentLaunchSpec& spec,
                          AgentLaunchResolved& out,
                          std::string& err);

void apply_launch_policy_to_turn(const AgentLaunchSpec& spec,
                                 const AgentLaunchResolved& resolved,
                                 Turn& turn);

AgentRunName make_timestamp_agent_run_name();
std::string make_safe_agent_run_id(const std::string& display_name);

} // namespace media::llm::agent::launch

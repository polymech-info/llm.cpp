#pragma once

// Portable `llm agent` flags. Tanit's CLI and pm-llm both fill this record.
// The launch-spec and turn helpers live in agent_cli_launch.hpp so this header
// stays usable without the host agent loop.

#include <string>
#include <vector>

namespace media::llm::agent::cliopt {

struct AgentCliOptions {
    std::string prompt;
    std::string system_prompt;
    std::string planner_prompt;
    std::vector<std::string> include_paths;
    std::vector<std::string> embed_paths;
    std::string preset;
    std::string runner;
    std::string save_preset;
    std::string router;
    std::string model;
    std::string api_key;
    std::string base_url;
    std::string api_mode;
    std::string streaming = "auto";
    int timeout_ms = 0;
    int max_iterations = 0;
    bool no_tools = false;
    bool no_mcp = false;
    bool no_skills = false;
    bool skills = false;
    bool no_planner = false;
    bool planner = false;
    int planner_budget = 8;
    bool no_parallel_tools = false;
    bool ai_consent_review = false;
    std::string ai_consent_router;
    std::string ai_consent_model;
    std::string consent_ui;
    std::string ocr_provider;
    std::string ocr_model;
    std::string recognition_provider;
    std::string recognition_model;
    std::string image_provider;
    std::string image_model;
    std::string video_provider;
    std::string video_model;
    std::string stt_provider;
    std::string stt_model;
    std::string tts_provider;
    std::string tts_model;
    std::string tts_voice_id;
    std::string context_reduction;
    double context_reduction_ratio = 0.5;
    int context_reduction_min_bytes = 2048;
    std::string context_reduction_objective;
    std::string session_id;
    std::string load_snapshot;
    std::string save_snapshot;
    bool multi_turn = true;
};

} // namespace media::llm::agent::cliopt

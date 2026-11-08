#pragma once

#include "polymech_export.h"
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <functional>

namespace polymech {
namespace kbot {

struct KBotOptions {
    std::string path = ".";
    std::string prompt;
    std::string output;
    std::string dst;
    std::string append = "concat";
    std::string wrap = "none";
    std::string each;
    std::vector<std::string> disable;
    std::vector<std::string> disable_tools;
    std::vector<std::string> tools;
    std::vector<std::string> include_globs;
    std::vector<std::string> exclude_globs;
    std::string glob_extension;
    std::string api_key;
    std::string model;
    std::string router = "openrouter";
    std::string mode = "tools";
    int log_level = 4;
    std::string profile;
    std::string base_url;
    std::string config_path;
    std::string dump;
    std::string preferences;
    std::string logs;
    bool stream = false;
    bool alt = false;
    std::string env = "default";
    std::string filters;
    std::string query;
    bool dry_run = false;
    std::string format;
    /** liboai HTTP timeout (ms). 0 = library default (~30s). IPC may set for long prompts. */
    int llm_timeout_ms = 0;
    /**
     * Optional chat completion `response_format` JSON (OpenAI structured outputs).
     * Example: {"type":"json_object"} or {"type":"json_schema","json_schema":{...}}.
     * Empty = omit (default text completion).
     */
    std::string response_format_json;
    /** Optional per-call cancellation predicate polled by blocking LLM HTTP calls. */
    std::function<bool()> cancel_check;

    // Internal 
    std::string job_id;
    std::shared_ptr<std::atomic<bool>> cancel_token;
};

struct KBotRunOptions {
    std::string config = "default";
    bool dry = false;
    bool list = false;
    std::string project_path;
    std::string log_file_path;
    
    // Internal 
    std::string job_id;
    std::shared_ptr<std::atomic<bool>> cancel_token;
};

struct KBotCallbacks {
    std::function<void(const std::string& type, const std::string& json)> onEvent;
};

POLYMECH_API int run_kbot_ai_pipeline(const KBotOptions& opts, const KBotCallbacks& cb);
POLYMECH_API int run_kbot_run_pipeline(const KBotRunOptions& opts, const KBotCallbacks& cb);

} // namespace kbot
} // namespace polymech

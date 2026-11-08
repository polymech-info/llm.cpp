#pragma once

// Portable `llm agent --serve` flags. Tanit's server command and pm-llm both
// fill this record. The HTTP daemon stays in the host translation unit.

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace media::llm::agent::cliopt {

struct AgentServerOptions {
    std::string host = "127.0.0.1";
    int port = 8090;
    int concurrency = 2;
    int http_workers = 0;
    std::string serve_api_key;
    std::string preset;
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
    std::string disable_tools;
    std::string enable_tools;
    std::string system_prompt;
    std::string planner_prompt;
    std::string consent_ui = "auto-deny";
    std::string consent_owner;
    std::string cwd;
    bool fs_jail = false;
    std::string runner;
    std::vector<std::string> host_tool_providers;
    bool lan = false;
    int lan_port = 0;
    std::string lan_name;
    bool pair_on_start = false;
    std::string pair_scopes;
};

inline bool server_env_truthy(const char* value)
{
    if (!value || !*value)
        return false;
    const std::string v(value);
    return v == "1" || v == "true" || v == "TRUE" || v == "yes";
}

inline void apply_server_process_env(AgentServerOptions& o)
{
    if (o.serve_api_key.empty()) {
        if (const char* env = std::getenv("TANIT_SERVE_API_KEY")) {
            if (*env)
                o.serve_api_key = env;
        }
    }
    if (!o.fs_jail)
        o.fs_jail = server_env_truthy(std::getenv("TANIT_SERVE_FS_JAIL"));
    if (!o.lan)
        o.lan = server_env_truthy(std::getenv("TANIT_SERVE_LAN"));
    if (const char* env = std::getenv("TANIT_HOST_TOOL_PROVIDER")) {
        if (*env) {
            std::string raw = env;
            std::size_t start = 0;
            while (start < raw.size()) {
                const std::size_t sep = raw.find(';', start);
                const std::string piece = sep == std::string::npos
                    ? raw.substr(start)
                    : raw.substr(start, sep - start);
                if (!piece.empty())
                    o.host_tool_providers.push_back(piece);
                if (sep == std::string::npos)
                    break;
                start = sep + 1;
            }
        }
    }
}

inline bool is_loopback_host(std::string_view addr)
{
    if (addr.empty())
        return false;
    if (addr == "127.0.0.1" || addr == "::1" || addr == "localhost" || addr == "[::1]")
        return true;
    if (addr.size() >= 4 && addr.substr(0, 4) == "127.")
        return true;
    return addr.find("::ffff:127.") != std::string_view::npos;
}

inline int resolved_http_workers(const AgentServerOptions& o)
{
    int workers = o.http_workers;
    if (workers < 1) {
        const int conc = o.concurrency < 1 ? 1 : o.concurrency;
        workers = conc * 2;
        if (workers < 32)
            workers = 32;
    }
    if (workers > 512)
        workers = 512;
    return workers;
}

} // namespace media::llm::agent::cliopt

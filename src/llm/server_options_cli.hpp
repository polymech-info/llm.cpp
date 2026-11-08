#pragma once

// Portable `llm agent --serve` options and request converters.
// Tanit's HTTP daemon and pm-llm both fill AgentServerOptions and call these
// helpers. Route registration, the turn loop, video, LAN, and IPC stay on the host.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
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

/// Trim host, fill defaults, reject a bad port or an open non-loopback bind.
/// Empty means the record is ready to dispatch.
std::string validate_server_options(AgentServerOptions& o);

/// Copy chat.router/model/base_url/api_key into empty option fields.
void overlay_server_chat(const nlohmann::json& chat, AgentServerOptions& o);

std::string trim_ascii(std::string s);

std::string openai_completion_id();
long openai_created_unix();
nlohmann::json openai_error(const std::string& message,
                            const std::string& type = "server_error",
                            const std::string& code = "agent_error");

struct ParsedChatRequest {
    std::string user_prompt;
    std::string system_prompt;
    nlohmann::json seed_chat_messages = nlohmann::json::array();
};

bool parse_chat_request(const nlohmann::json& body, ParsedChatRequest& out, std::string& err);
bool looks_like_loopback_script(const std::string& model);

struct TurnOutcome {
    bool ok = false;
    std::string model;
    std::string final_text;
    std::string error;
    nlohmann::json usage;
    long long queue_wait_ms = 0;
    long long exec_ms = 0;
    std::string stream_id;
    long stream_created = 0;
    std::string session_id;
    std::string finish_reason;
    std::size_t streamed_chars = 0;
    std::size_t stream_deltas = 0;
};

using StreamEmitFn = std::function<bool(std::string_view)>;

std::string resolve_body_cwd(const nlohmann::json& body,
                             std::string_view cwd_header,
                             const AgentServerOptions& opt);
std::string resolve_body_session(const nlohmann::json& body, std::string_view session_header);
void apply_body_provider_overrides(const nlohmann::json& body,
                                   std::string_view provider_key_header,
                                   std::string_view provider_base_header,
                                   std::string& api_key,
                                   std::string& base_url);
std::vector<std::string> resolve_request_selection(const nlohmann::json& body);

std::string stage_message_images(const nlohmann::json& body,
                                 const std::string& folder,
                                 std::vector<std::string>& paths);
std::string stage_mobile_attachments(const nlohmann::json& body,
                                     const std::string& folder,
                                     std::vector<std::string>& paths);

/// Capability decisions from serve flags. The host copies these onto Turn.
struct ServerTurnCaps {
    bool clear_mcp = false;
    bool skills_on = false;
    bool skills_off = false;
    bool planner_off = false;
    bool fs_jail = false;
};

inline ServerTurnCaps server_turn_caps(const AgentServerOptions& opt)
{
    ServerTurnCaps caps;
    caps.clear_mcp = opt.no_mcp;
    caps.skills_on = opt.skills;
    caps.skills_off = !opt.skills && opt.no_skills;
    caps.planner_off = opt.no_planner;
    caps.fs_jail = opt.fs_jail;
    return caps;
}

inline std::string sse_data_line(const nlohmann::json& j)
{
    return "data: " + j.dump() + "\n\n";
}

nlohmann::json usage_fields(const nlohmann::json& raw);
nlohmann::json completion_success_json(const TurnOutcome& out);

bool bearer_token_authorized(std::string_view authorization,
                             std::string_view api_key_a,
                             std::string_view api_key_b,
                             std::string_view api_key_c,
                             const std::string& expected_key);

/// Stamps TANIT_CURRENT_URL* for the duration of one turn.
class BrowserEnvGuard {
public:
    explicit BrowserEnvGuard(const nlohmann::json& body);
    ~BrowserEnvGuard();
    BrowserEnvGuard(const BrowserEnvGuard&) = delete;
    BrowserEnvGuard& operator=(const BrowserEnvGuard&) = delete;

private:
    std::unique_lock<std::mutex> lock_;
    std::string prev_url_;
    std::string prev_title_;
    std::string prev_last_;
    std::string prev_tabs_;
};

void apply_provider_defaults(std::string& api_key,
                             std::string& base_url,
                             const std::string& server_key,
                             const std::string& server_base);

struct VictoriaTarget {
    bool enabled = true;
    std::string host = "127.0.0.1";
    int port = 9428;
};

VictoriaTarget victoria_target();
std::string utc_now_iso();
std::string clip_text(std::string s, std::size_t max);
std::string log_level_name(std::string level);

bool parse_host_tool_provider_spec(const std::string& spec, std::string& id, std::string& url);
bool is_video_route(const std::string& path);

} // namespace media::llm::agent::cliopt

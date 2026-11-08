#pragma once

#include "polymech_export.h"
#include "mcp_session.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifndef FEATURE_MCP_CLIENT
#define FEATURE_MCP_CLIENT 1
#endif

namespace media::process {
struct StdioLaunchOptions;
}

namespace media::llm::mcp {

constexpr const char* kMcpConfigUri = "pm://config/mcp.json";

#if FEATURE_MCP_CLIENT

struct McpSessionCreateOptions {
    bool apply_persisted_sandbox = false;
    int timeout_ms = 0;
    std::function<bool()> cancel_check;
};

struct McpServerEndpoint {
    std::string type;
    std::string url;
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    std::map<std::string, std::string> headers;
};

POLYMECH_API std::filesystem::path resolve_mcp_config_dir(const std::filesystem::path& dir = {});

POLYMECH_API std::filesystem::path mcp_json_path(const std::filesystem::path& dir = {});

POLYMECH_API bool load_mcp_config(const std::filesystem::path& dir,
                                  nlohmann::json& out,
                                  std::string& err);

POLYMECH_API bool save_mcp_config(const std::filesystem::path& dir,
                                  const nlohmann::json& cfg,
                                  std::string& err);

POLYMECH_API std::vector<std::string> list_mcp_servers(const nlohmann::json& cfg);

POLYMECH_API std::map<std::string, std::string> parse_header_map(const nlohmann::json& server_cfg);

POLYMECH_API media::process::StdioLaunchOptions parse_stdio_launch_options(const nlohmann::json& server_cfg,
                                                                           const std::string& server_name);

POLYMECH_API void apply_persisted_sandbox_routing(media::process::StdioLaunchOptions& opts,
                                                  const std::string& server_name);

POLYMECH_API bool parse_mcp_server_endpoint(const nlohmann::json& server_cfg, McpServerEndpoint& out);

POLYMECH_API std::unique_ptr<IMcpSession> create_mcp_session(const nlohmann::json& server_cfg,
                                                             const std::string& server_name,
                                                             std::string& err,
                                                             const McpSessionCreateOptions& opts = {});

POLYMECH_API nlohmann::json parameters_from_mcp_tool(const nlohmann::json& tool_json);

POLYMECH_API bool require_registered_mcp(const nlohmann::json& root);

POLYMECH_API bool mcp_server_policy_allows(const nlohmann::json& server,
                                           const std::string& server_key,
                                           const std::string& command,
                                           bool require_registered,
                                           std::string& warning);

POLYMECH_API int mcp_default_timeout_ms();

POLYMECH_API std::function<bool()> mcp_cancel_after_ms_from_env();

POLYMECH_API std::filesystem::path mcp_tools_cache_path();

/// Drop `mcp-tools-cache.json`. Call after MCP settings are saved, and after
/// login/logout, so the next agent/planner turn re-probes tools/list instead
/// of serving a stale catalog (router MCP skip-on-missing-auth is cached).
POLYMECH_API void invalidate_mcp_tools_cache();

/// Drop the in-process tools/list cache. Called from `invalidate_mcp_tools_cache`.
POLYMECH_API void clear_mcp_tools_memory_cache();

POLYMECH_API nlohmann::json load_or_probe_mcp_tools(bool allow_probe,
                                                    std::string& note,
                                                    const std::filesystem::path& config_dir = {},
                                                    bool allow_stale = false);

POLYMECH_API nlohmann::json mcp_probe_cached(bool allow_probe,
                                             std::string* note = nullptr,
                                             const std::filesystem::path& config_dir = {});

// ── Inline / custom server probe cache ──────────────────────────────────────

/// Compute a stable 16-char hex slug (first 16 chars of SHA-256) for an inline
/// server config object using canonical (key-sorted) JSON serialisation.
/// Always uses the raw, pre-resolved config — never the resolved form — so that
/// secrets passed via ${USER:…} do not leak into cache filenames.
POLYMECH_API std::string mcp_inline_server_slug(const nlohmann::json& server_cfg);

/// Full path to the per-inline-config cache file:
///   <config_dir>/mcp_server-<slug>_cache.json
/// Returns an empty path when the config directory cannot be resolved.
POLYMECH_API std::filesystem::path mcp_inline_server_cache_path(const std::string& slug);

/// Returns false when the cache file is missing or stale.
POLYMECH_API bool load_mcp_inline_probe_cache(const std::string& slug, nlohmann::json& probe);

/// Probe a single inline MCP server with a slug-keyed disk cache (same 8 h TTL
/// as the main probe cache).  Reads from cache when fresh; runs a live handshake
/// + tools/list otherwise and saves the result.
///
/// force_refresh=true bypasses the disk cache and always does a live probe.
///
/// @return  Probe-shape JSON { "servers": [{name:"__inline__", tools:[…], …}] }
///
/// IMPORTANT: call this only from an explicit user action ("Load tools" button).
/// Never invoke from a panel render / mount path — it spawns a subprocess for
/// stdio servers and can be slow.
/// @param force_refresh  When true, bypass the disk cache and always do a live probe.
/// @param timeout_ms     Per-operation timeout forwarded to probe_inline_mcp_server.
///                       0 → use mcp_default_timeout_ms() (60 s in production).
POLYMECH_API nlohmann::json mcp_probe_inline_server(const nlohmann::json& server_cfg,
                                                     bool force_refresh = false,
                                                     int  timeout_ms    = 0);

#endif

} // namespace media::llm::mcp

#pragma once

#include "polymech_export.h"

#include <filesystem>
#include <nlohmann/json.hpp>

#ifndef FEATURE_MCP_CLIENT
#define FEATURE_MCP_CLIENT 1
#endif

namespace media::llm::mcp {

#if FEATURE_MCP_CLIENT
/**
 * Inspect profile `mcp.json`: path, parse status, and per-server MCP handshake (`initialize`) +
 * `tools/list` (names + descriptions). Used by `pm-image llm info` for diagnostics.
 * @param profile_config_dir Same root as portable `settings.json` / agent MCP (empty → default profile dir).
 */
POLYMECH_API nlohmann::json probe_mcp_config(const std::filesystem::path& profile_config_dir);

/**
 * Probe a single MCP server described by an inline config object — no mcp.json lookup.
 * Performs the same handshake + tools/list as probe_mcp_config but for one server only.
 * Returns a probe-shape JSON { "servers": [{...}] } where the single entry carries
 * name == server_key, plus handshake_ok / tools_list_ok / tools / etc.
 *
 * Only call this from an explicit user action (e.g. "Load tools" button).
 * Never call from a UI hot path — it spawns a subprocess for stdio servers.
 *
 * @param server_cfg  Raw (unresolved) MCP server config object, same shape as a
 *                    mcpServers entry in mcp.json.
 * @param server_key  Name to use in the result's servers[].name field (default "__inline__").
 */
/// @param timeout_ms  Per-operation timeout (initialize + tools/list combined).
///                    Defaults to mcp_default_timeout_ms() (60 s in production).
///                    Pass 0 to use the session layer's own default.
POLYMECH_API nlohmann::json probe_inline_mcp_server(const nlohmann::json& server_cfg,
                                                     const std::string& server_key = "__inline__",
                                                     int timeout_ms = 0);
#else
inline nlohmann::json probe_mcp_config(const std::filesystem::path&)
{
    return nlohmann::json{
        {"skipped", true},
        {"disabled", true},
        {"note", "MCP client support is disabled in this build (FEATURE_MCP_CLIENT=OFF)."},
        {"servers", nlohmann::json::array()}};
}
#endif

} // namespace media::llm::mcp

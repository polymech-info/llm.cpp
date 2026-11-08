#pragma once

#include "agent.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace media::llm::agent {

/// Extra CLI flags loaded from claude_defaults.json (config dir + optional cwd overlay).
/// Each key is a Claude flag name (without `--`); value is a string path/value, `true` for
/// boolean flags, or a string array for repeatable flags (e.g. `add-dir`).
struct ClaudeDefaults {
    nlohmann::json flags   = nlohmann::json::object();
    nlohmann::json sources = nlohmann::json::array(); // loaded file paths (for logs)
};

/// Load and merge `<config_dir>/claude_defaults.json` then `./claude_defaults.json`
/// (relative to `working_dir`, or the process cwd when empty).
PM_CHAT_API ClaudeDefaults load_claude_defaults(const std::string& working_dir = {});

/// Flatten `flags` to `--key value` argv and expand with `deep_resolve_variables`
/// using the standard command-variable map (${CONFIG_DIR}, ${TEMP_DIR}, ${MCP_CONFIG}, …)
/// plus lowercase ${mcp_config}/${config_dir}/${cwd} aliases.
/// Writes the Claude-shaped export to ${TEMP_DIR}/claude-mcp-config.json.
/// When `tanit_mcp_url` is set, that file's `mcpServers.tanit` is the local embed.
PM_CHAT_API std::vector<std::string> resolve_claude_default_argv(const ClaudeDefaults& defs,
                                                                    const Turn& turn,
                                                                    const std::string& tanit_mcp_url = {});

/// Log-safe single-line argv (prompt value replaced by a size tag).
PM_CHAT_API std::string format_claude_argv_for_log(const std::vector<std::string>& argv,
                                                    std::size_t max_len = 4000);

} // namespace media::llm::agent

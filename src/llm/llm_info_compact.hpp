#pragma once

#include "polymech_export.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace media::llm::info {

struct CompressedMarkdownOptions {
    bool include_commands = true;
    bool include_presets = true;
    bool include_tools = true;
    bool include_mcp = true;
    bool include_skills = true;
    bool include_xblox = false;
    bool allow_mcp_probe = true;
    /// Chat preset name/id for effective tool/MCP/skill policy. Empty selects Default, or the lone preset.
    std::string preset;
};

struct McpToolInventoryEntry {
    std::string function_name;
    std::string server_name;
    std::string remote_name;
    std::string description;
    std::string input_schema_json;
    std::vector<std::string> policy_labels;
};

/// Compact, deterministic Markdown for router/planner context injection.
PM_CHAT_API std::string build_compressed_markdown(const CompressedMarkdownOptions& options = {});

/// Cached MCP tools visible under global or saved preset policy, using chat-agent function names.
PM_CHAT_API std::vector<McpToolInventoryEntry> mcp_tool_inventory(bool allow_probe = true);

/// Cached/live MCP probe used by `llm info`; disk cache TTL is 8h unless mcp.json changes.
PM_CHAT_API nlohmann::json mcp_probe_cached(bool allow_probe = true, std::string* note = nullptr);

} // namespace media::llm::info

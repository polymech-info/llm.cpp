#pragma once

#include "agent.hpp"
#include "llm_constants.hpp"

#include <string>
#include <vector>

namespace media::llm::agent::options {

/// Full system-prompt override (--system-prompt grammar): literal, null/none,
/// @path, file:path, or an existing absolute/relative file path.
bool apply_system_prompt_option(const std::string& raw, Turn& turn, std::string& err);

/// Planner system-prompt override (--planner-prompt grammar).
bool apply_planner_prompt_option(const std::string& raw, Turn& turn, std::string& err);

pm::llm::ContextReductionMode parse_context_reduction_mode(const std::string& raw);

/// Populate disabled path/MCP tool names from a string list (one name per row).
void apply_disable_tools_list(const std::vector<std::string>& tools,
                              bool no_tools,
                              std::vector<std::string>& path_out,
                              std::vector<std::string>* mcp_out = nullptr);

/// Populate allowed path tools from a string list; sets allowlist_set_out when non-empty.
void apply_enable_tools_list(const std::vector<std::string>& tools,
                             bool no_tools,
                             std::vector<std::string>& allowed_out,
                             bool& allowlist_set_out);

} // namespace media::llm::agent::options

#pragma once
//
// media::llm::computer_use — app inspection / desktop interaction tools for LLM agents.
//
// This module owns both the path-tool schemas and the execution handlers for:
//   app_inspect_dump, app_inspect_find, app_screenshot, app_click
//
#include "polymech_export.h"
#include "llm/path_tool_catalog.hpp"
#include "llm/path_tool_executor.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace media::llm::computer_use {

POLYMECH_API std::vector<media::llm::path::ToolDef> tool_defs();

POLYMECH_API bool is_computer_use_tool(const std::string& name);

POLYMECH_API media::llm::path::ExecuteResult execute(const std::string& name,
                                                     const nlohmann::json& args,
                                                     const std::string& agent_path_base);

} // namespace media::llm::computer_use

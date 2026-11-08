#pragma once

#include "polymech_export.h"
#include "llm/path_tool_catalog.hpp"
#include "llm/path_tool_executor.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace media::llm::app_command {

PM_CHAT_API std::vector<media::llm::path::ToolDef> tool_defs();
PM_CHAT_API bool is_app_command_tool(const std::string& name);
PM_CHAT_API media::llm::path::ExecuteResult execute(
    const std::string& name,
    const nlohmann::json& args);

} // namespace media::llm::app_command

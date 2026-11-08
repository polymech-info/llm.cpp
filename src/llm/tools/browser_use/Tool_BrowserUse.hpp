#pragma once

#include "polymech_export.h"
#include "llm/path_tool_catalog.hpp"
#include "llm/path_tool_executor.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace media::llm::browser_use {

POLYMECH_API std::vector<media::llm::path::ToolDef> tool_defs();
POLYMECH_API bool is_browser_use_tool(const std::string& name);
POLYMECH_API media::llm::path::ExecuteResult execute(
    const std::string& name,
    const nlohmann::json& args);

} // namespace media::llm::browser_use

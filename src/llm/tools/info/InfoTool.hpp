#pragma once
//
// media::llm::info_tool — lightweight introspection tool for agent planning.
//
// Lets the agent fetch exact details for commands, tools, and other PM-Image
// capability surfaces without bloating the default system prompt.
//
#include "polymech_export.h"
#include "llm/path_tool_executor.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace media::llm::info_tool {

PM_CHAT_API media::llm::path::ExecuteResult execute(
    const nlohmann::json& args,
    const std::string& cwd_hint = {});

} // namespace media::llm::info_tool

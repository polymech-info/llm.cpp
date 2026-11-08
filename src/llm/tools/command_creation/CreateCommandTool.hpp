#pragma once
//
// media::llm::create_command — Create a new custom command entry in commands.json.
//
// Writes a fully-formed button entry (with a generated ID) into the user's
// commands.json so it appears in the ribbon or Windows Explorer context menu.
// Handles ID generation, field validation, and atomic file save.
//
// Gated by FEATURE_TOOLS_COMMAND_CREATION.
//
#include "polymech_export.h"
#include "llm/path_tool_executor.hpp"

#include <nlohmann/json.hpp>

namespace media::llm::create_command {

PM_CHAT_API media::llm::path::ExecuteResult execute(const nlohmann::json& args);

} // namespace media::llm::create_command

// Copyright PolyMech / pm-image. Internal use only.
//
// Network agent tools: ssh_exec, mqtt, modbus_read, modbus_write.
// One connection per call; see docs/llm/network-tools.md.
#pragma once

#include "llm/tools/commons/PathToolCommons.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace media::llm::path {
struct ToolDef;
}

namespace media::llm::path::detail {

bool is_network_tool(std::string_view name) noexcept;

ExecuteResult do_network_tool(const std::string& name, const nlohmann::json& args);

/// Tools whose transport is compiled in (FEATURE_SSH / FEATURE_MQTT / FEATURE_MODBUS).
std::vector<ToolDef> network_tool_catalog();

} // namespace media::llm::path::detail

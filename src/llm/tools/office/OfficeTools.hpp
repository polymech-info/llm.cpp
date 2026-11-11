// Copyright PolyMech / pm-image. Internal use only.
//
// office_* path tools. The bodies call packages/office/cpp (pm-office).
// Gated by FEATURE_OFFICE_LLM_TOOLS. Charts, pivots, raw XML, animations,
// and HTML preview are not in this surface.
#pragma once

#include "llm/tools/commons/PathToolCommons.hpp"

#include <string>
#include <vector>

namespace media::llm::path {
struct ToolDef;
}

namespace media::llm::path::detail {

#if defined(FEATURE_OFFICE_LLM_TOOLS) && FEATURE_OFFICE_LLM_TOOLS

bool is_office_tool(std::string_view name) noexcept;

ExecuteResult do_office_tool(const std::string& name, const nlohmann::json& args);

std::vector<ToolDef> office_tool_catalog();

#endif

} // namespace media::llm::path::detail

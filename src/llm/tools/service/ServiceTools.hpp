// Copyright PolyMech / pm-image. Internal use only.
//
// Tanit service API agent tools (`service_*`). Auth uses zitadel-oauth.json
// (run `tanit-cli login` first). Requires FEATURE_INSTALLER_VFS / CMS in pm-media.
#pragma once

#include "llm/tools/commons/PathToolCommons.hpp"

#include <string>
#include <vector>

namespace media::llm::path {
struct ToolDef;
}

namespace media::llm::path::detail {

#if defined(FEATURE_INSTALLER_VFS) && FEATURE_INSTALLER_VFS

bool is_service_tool(std::string_view name) noexcept;

ExecuteResult do_service_tool(const std::string& name, const nlohmann::json& args);

std::vector<ToolDef> service_tool_catalog();

#endif

} // namespace media::llm::path::detail

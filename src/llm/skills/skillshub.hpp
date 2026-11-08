#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace media::llm::skills::hub {

struct ToolReply {
    bool ok = false;
    std::string error;
    /// Unwrapped tool payload. A JSON text body is parsed; other text is `{"text": ...}`.
    nlohmann::json data = nlohmann::json::object();
};

/// Call one SkillHub MCP tool (`search_skills`, `get_skill_detail`, ...).
/// Resolves the server from the tools cache: exact name, or a `skillhub-` prefix
/// (the Tanit proxy publishes them as `skillhub-search_skills`).
ToolReply call_tool(const std::string& tool, const nlohmann::json& args, std::uint64_t search_id = 0);

/// `search_id` lets the settings UI abort this call. `0` is not cancellable.
ToolReply search_skills(const std::string& query, int limit, const std::string& category, std::uint64_t search_id = 0);
ToolReply browse_catalog(int limit, int offset, const std::string& sort, const std::string& category);
ToolReply get_skill_detail(const std::string& skill_id, bool include_content);
ToolReply recommend_skills(const std::string& context, int limit, const std::vector<std::string>& current_skills);

/// Abort the SkillHub search started with this `search_id`.
void cancel_inflight(std::uint64_t search_id);

/// Skill rows from a hub payload. A structured array is used when present.
/// Otherwise the markdown `text` body is parsed (`1. **name** (slug) [category]`).
nlohmann::json catalog_entries(const nlohmann::json& data);

/// Human markdown for a hub payload. SkillHub's own `text` is kept when set.
std::string catalog_markdown(const nlohmann::json& data);

struct InstallReply {
    bool ok = false;
    bool replaced = false;
    /// A workspace skill with the same name hides the roaming copy.
    bool shadowed_by_workspace = false;
    std::string error;
    std::string name;
    std::string path;
};

/// Fetch SKILL.md via `get_skill_detail` and write it under `config_dir()/skills/<slug>`.
/// Does not call remote `install_skill` (that targets the detected agent, usually Claude).
/// Refuses when the roaming folder already has SKILL.md unless `force` is set.
InstallReply install_roaming(const std::string& skill_id, bool force);

struct RemoveReply {
    bool ok = false;
    bool workspace_remains = false;
    std::string error;
    std::string name;
    std::string path;
};

/// Delete one direct child of the roaming skills root. Workspace copies are left in place.
RemoveReply remove_roaming(const std::string& name);

} // namespace media::llm::skills::hub

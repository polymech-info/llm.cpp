#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace media::llm::skills {

enum class SkillSource {
    Roaming,
    Workspace,
};

struct SkillEntry {
    std::string name;
    std::string description;
    std::filesystem::path skill_md_path;
    std::filesystem::path manifest_path;
    std::string media_type = "text/markdown";
    std::vector<std::string> declared_tools;
    SkillSource source = SkillSource::Workspace;
    bool available = true;
    bool always = false;
    bool pinned = false;
    bool disabled = false;
    bool active = false;
    std::vector<std::string> missing_requirements;
};

struct SkillPolicy {
    bool enabled = true;
    bool roaming_enabled = true;
    bool workspace_enabled = true;
    std::vector<std::string> pinned;
    std::vector<std::string> disabled;
};

struct SkillSnapshot {
    std::filesystem::path roaming_root;
    std::filesystem::path workspace_root;
    SkillPolicy policy;
    std::vector<SkillEntry> entries;
};

std::filesystem::path default_roaming_skills_root();
std::filesystem::path default_workspace_skills_root(const std::string& cwd_hint);
SkillSnapshot discover_skills(const SkillPolicy& policy, const std::string& cwd_hint);

/// Load `custom.agent_skills` into a SkillPolicy (enable flags + pin/disable lists).
SkillPolicy load_policy_from_settings();

/// One skill row for settings / chat slash typeahead (name, description, source, flags).
nlohmann::json skill_entry_json(const SkillEntry& entry);

/// Full list payload: policy flags, roots, and `skills` array from a snapshot.
nlohmann::json skills_list_data_json(const SkillSnapshot& snap);

/// Discover with settings policy (or override) and return `skills_list_data_json`.
nlohmann::json list_skills_data_json(
    const std::string& cwd_hint,
    const SkillPolicy* policy_override = nullptr);

} // namespace media::llm::skills


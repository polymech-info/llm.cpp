#include "llm/agent_skills.hpp"

#include "core/settings_store.hpp"
#include "core/settings_runtime.hpp"
#include "ui_defaults.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <unordered_map>
#include <vector>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

namespace media::llm::skills {

namespace {


static std::string trim(std::string s) {
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

static void dedupe_lower(std::vector<std::string>& values) {
    std::unordered_map<std::string, bool> seen;
    std::vector<std::string> out;
    out.reserve(values.size());
    for (auto v : values) {
        v = media::strutil::lower_ascii(trim(std::move(v)));
        if (v.empty() || seen.count(v))
            continue;
        seen[v] = true;
        out.push_back(std::move(v));
    }
    values = std::move(out);
}

static std::string current_os_tag() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

struct ParsedMeta {
    std::string description;
    bool always = false;
    std::vector<std::string> os;
    std::vector<std::string> bins;
    std::vector<std::string> env;
    std::vector<std::string> declared_tools;
};

static ParsedMeta parse_skill_metadata(const std::filesystem::path& skill_md) {
    ParsedMeta meta;
    std::ifstream in(skill_md, std::ios::binary);
    if (!in)
        return meta;
    // Frontmatter lives at the top. Don't slurp a multi-MB SKILL.md on prepare.
    constexpr std::size_t kFrontmatterCap = 8192;
    std::string content(kFrontmatterCap, '\0');
    in.read(content.data(), static_cast<std::streamsize>(kFrontmatterCap));
    content.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
    if (content.empty())
        return meta;

    auto parse_csv_array = [](std::string raw, bool lower) -> std::vector<std::string> {
        std::vector<std::string> out;
        raw = trim(raw);
        if (raw.empty())
            return out;
        if (raw.front() == '[' && raw.back() == ']' && raw.size() >= 2)
            raw = raw.substr(1, raw.size() - 2);
        std::size_t pos = 0;
        while (pos < raw.size()) {
            const std::size_t comma = raw.find(',', pos);
            std::string one = (comma == std::string::npos)
                ? raw.substr(pos)
                : raw.substr(pos, comma - pos);
            one = trim(one);
            if (!one.empty() && one.front() == '"' && one.back() == '"' && one.size() >= 2)
                one = one.substr(1, one.size() - 2);
            if (!one.empty() && one.front() == '\'' && one.back() == '\'' && one.size() >= 2)
                one = one.substr(1, one.size() - 2);
            one = trim(one);
            if (!one.empty())
                out.push_back(lower ? media::strutil::lower_ascii(one) : one);
            if (comma == std::string::npos)
                break;
            pos = comma + 1;
        }
        return out;
    };

    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#')
            continue;
        const std::size_t colon = t.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string key = media::strutil::lower_ascii(trim(t.substr(0, colon)));
        const std::string val = trim(t.substr(colon + 1));
        if (key == "description" && meta.description.empty()) {
            meta.description = val;
        } else if (key == "always") {
            meta.always = (media::strutil::lower_ascii(val) == "true");
        } else if (key == "os") {
            meta.os = parse_csv_array(val, true);
        } else if (key == "bins") {
            meta.bins = parse_csv_array(val, true);
        } else if (key == "env") {
            meta.env = parse_csv_array(val, false);
        }
        // allowed-tools / declared_tools / tools: ignored (not policy, not grants).
    }

    dedupe_lower(meta.os);
    dedupe_lower(meta.bins);
    dedupe_lower(meta.env);
    return meta;
}

static bool find_skill_body(const std::filesystem::path& dir,
                            std::filesystem::path& out_body,
                            std::filesystem::path& out_manifest) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec))
        return false;
    const auto f = dir / "SKILL.md";
    if (std::filesystem::exists(f, ec) && std::filesystem::is_regular_file(f, ec)) {
        out_body = f;
        return true;
    }
    for (const char* name : {"skill.json", "resource.json"}) {
        const auto manifest = dir / name;
        if (!std::filesystem::exists(manifest, ec) || !std::filesystem::is_regular_file(manifest, ec))
            continue;
        try {
            std::ifstream in(manifest, std::ios::binary);
            const auto doc = nlohmann::json::parse(in);
            if (!doc.is_object() || media::strutil::lower_ascii(doc.value("type", std::string{})) != "skill")
                continue;
            const std::string relative = doc.value("body", doc.value("path", std::string{}));
            if (relative.empty()) continue;
            const auto body = (dir / media::path::path_from_utf8(relative)).lexically_normal();
            const auto base = std::filesystem::weakly_canonical(dir, ec);
            const auto canonical_body = std::filesystem::weakly_canonical(body, ec);
            const auto relative_body = canonical_body.lexically_relative(base);
            if (ec || relative_body.empty() || *relative_body.begin() == ".."
                || !std::filesystem::is_regular_file(canonical_body, ec))
                continue;
            out_body = canonical_body;
            out_manifest = manifest;
            return true;
        } catch (...) {
            continue;
        }
    }
    return false;
}

static void apply_explicit_manifest(const std::filesystem::path& manifest,
                                    SkillEntry& entry,
                                    ParsedMeta& meta) {
    if (manifest.empty()) return;
    try {
        std::ifstream in(manifest, std::ios::binary);
        const auto doc = nlohmann::json::parse(in);
        if (!doc.is_object()) return;
        entry.name = doc.value("name", entry.name);
        meta.description = doc.value("description", meta.description);
        meta.always = doc.value("always", meta.always);
        auto load_array = [&](const char* key, std::vector<std::string>& out) {
            if (!doc.contains(key) || !doc[key].is_array()) return;
            out.clear();
            for (const auto& value : doc[key])
                if (value.is_string()) out.push_back(value.get<std::string>());
        };
        load_array("os", meta.os);
        load_array("bins", meta.bins);
        load_array("env", meta.env);
        // declared_tools / tools: ignored (request_tools owns grants).
        entry.media_type = doc.value("media_type", entry.media_type);
        entry.manifest_path = manifest;
        dedupe_lower(meta.os);
        dedupe_lower(meta.bins);
        dedupe_lower(meta.env);
    } catch (...) {
    }
}

std::mutex g_bin_mu;
std::unordered_map<std::string, bool> g_bin_cache;

static bool binary_available(const std::string& name) {
    if (name.empty())
        return false;
    {
        std::lock_guard<std::mutex> lock(g_bin_mu);
        const auto it = g_bin_cache.find(name);
        if (it != g_bin_cache.end())
            return it->second;
    }
#if defined(_WIN32)
    const std::string cmd = "where " + name + " >nul 2>nul";
#else
    const std::string cmd = "command -v " + name + " >/dev/null 2>&1";
#endif
    const int rc = std::system(cmd.c_str());
    const bool ok = (rc == 0);
    std::lock_guard<std::mutex> lock(g_bin_mu);
    g_bin_cache[name] = ok;
    return ok;
}

} // namespace

SkillPolicy load_policy_from_settings() {
    SkillPolicy p;
    std::string err;
    nlohmann::json custom;
    if (!media::settings::load_subtree(media::settings::SettingsSubtreeKeys::custom, custom, err))
        return p;
    if (!custom.is_object())
        return p;
    const auto& node = custom.contains("agent_skills") ? custom["agent_skills"] : nlohmann::json();
    if (!node.is_object())
        return p;
    if (node.contains("enabled") && node["enabled"].is_boolean())
        p.enabled = node["enabled"].get<bool>();
    if (node.contains("roaming_enabled") && node["roaming_enabled"].is_boolean())
        p.roaming_enabled = node["roaming_enabled"].get<bool>();
    if (node.contains("workspace_enabled") && node["workspace_enabled"].is_boolean())
        p.workspace_enabled = node["workspace_enabled"].get<bool>();
    if (node.contains("pinned") && node["pinned"].is_array()) {
        for (const auto& it : node["pinned"])
            if (it.is_string())
                p.pinned.push_back(it.get<std::string>());
    }
    if (node.contains("disabled") && node["disabled"].is_array()) {
        for (const auto& it : node["disabled"])
            if (it.is_string())
                p.disabled.push_back(it.get<std::string>());
    }
    dedupe_lower(p.pinned);
    dedupe_lower(p.disabled);
    return p;
}

std::filesystem::path default_roaming_skills_root() {
    return media::settings::get_config_dir() / "skills";
}

std::filesystem::path default_workspace_skills_root(const std::string& cwd_hint) {
    const std::filesystem::path base = cwd_hint.empty()
        ? media::ui_defaults::fallback_work_folder()
        : media::path::path_from_utf8(cwd_hint);
    return base / "skills";
}

namespace {

std::string skill_discover_cache_key(const std::string& cwd, const SkillPolicy& p)
{
    std::ostringstream o;
    o << cwd << '\n'
      << (p.enabled ? '1' : '0')
      << (p.roaming_enabled ? '1' : '0')
      << (p.workspace_enabled ? '1' : '0') << '\n';
    for (const auto& n : p.pinned)
        o << n << ',';
    o << '\n';
    for (const auto& n : p.disabled)
        o << n << ',';
    return o.str();
}

void append_skill_root_fp(std::string& out, const std::filesystem::path& root)
{
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec))
        return;
    std::vector<std::filesystem::path> dirs;
    for (auto it = std::filesystem::directory_iterator(root, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec))
            dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end());
    for (const auto& dir : dirs) {
        out += media::path::path_to_utf8(dir.filename());
        out += '\n';
        for (const char* name : {"SKILL.md", "skill.json", "resource.json"}) {
            const auto f = dir / name;
            if (!std::filesystem::is_regular_file(f, ec))
                continue;
            const auto sz = std::filesystem::file_size(f, ec);
            const auto mt = std::filesystem::last_write_time(f, ec);
            out += name;
            out += ' ';
            out += std::to_string(sz);
            out += ' ';
            out += std::to_string(mt.time_since_epoch().count());
            out += '\n';
        }
    }
}

std::string skill_roots_fingerprint(const SkillSnapshot& snap)
{
    std::string fp;
    if (snap.policy.roaming_enabled)
        append_skill_root_fp(fp, snap.roaming_root);
    if (snap.policy.workspace_enabled)
        append_skill_root_fp(fp, snap.workspace_root);
    return fp;
}

struct SkillDiscoverCache {
    std::string key;
    std::string fp;
    SkillSnapshot snap;
};
std::mutex g_discover_mu;
SkillDiscoverCache g_discover;

} // namespace

SkillSnapshot discover_skills(const SkillPolicy& policy_in, const std::string& cwd_hint) {
    SkillSnapshot snap;
    snap.roaming_root = default_roaming_skills_root();
    snap.workspace_root = default_workspace_skills_root(cwd_hint);
    snap.policy = policy_in;

    if (snap.policy.pinned.empty() && snap.policy.disabled.empty()) {
        // Fill pin/disable lists from settings when the caller left them empty.
        // Keep the caller's enablement bits so CLI overrides like `--skills`
        // are not clobbered by a disabled global/preset policy.
        const SkillPolicy from_settings = load_policy_from_settings();
        snap.policy.pinned = from_settings.pinned;
        snap.policy.disabled = from_settings.disabled;
    }

    dedupe_lower(snap.policy.pinned);
    dedupe_lower(snap.policy.disabled);

    const std::string cache_key = skill_discover_cache_key(cwd_hint, snap.policy);
    const std::string files_fp = skill_roots_fingerprint(snap);
    {
        std::lock_guard<std::mutex> lock(g_discover_mu);
        if (g_discover.key == cache_key && g_discover.fp == files_fp)
            return g_discover.snap;
    }

    std::unordered_map<std::string, bool> pinned_set;
    std::unordered_map<std::string, bool> disabled_set;
    for (const auto& n : snap.policy.pinned)
        pinned_set[n] = true;
    for (const auto& n : snap.policy.disabled)
        disabled_set[n] = true;

    auto add_from_root = [&](const std::filesystem::path& root, SkillSource src) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec))
            return;
        for (auto it = std::filesystem::directory_iterator(root, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            if (ec)
                break;
            const auto& dir = it->path();
            if (!it->is_directory(ec))
                continue;
            std::filesystem::path skill_md;
            std::filesystem::path manifest;
            if (!find_skill_body(dir, skill_md, manifest))
                continue;
            SkillEntry e;
            e.name = media::path::path_to_utf8(dir.filename());
            e.skill_md_path = skill_md;
            e.source = src;
            auto meta = parse_skill_metadata(skill_md);
            apply_explicit_manifest(manifest, e, meta);
            e.description = meta.description.empty() ? e.name : meta.description;
            e.always = meta.always;
            e.declared_tools = meta.declared_tools;

            // availability
            e.available = true;
            if (!meta.os.empty()) {
                const std::string cur = current_os_tag();
                bool match = false;
                for (const auto& one : meta.os)
                    if (one == cur) {
                        match = true;
                        break;
                    }
                if (!match) {
                    e.available = false;
                    e.missing_requirements.push_back("os:" + cur);
                }
            }
            for (const auto& b : meta.bins) {
                if (!binary_available(b)) {
                    e.available = false;
                    e.missing_requirements.push_back("bin:" + b);
                }
            }
            for (const auto& env : meta.env) {
                const char* v = std::getenv(env.c_str());
                if (!v || !*v) {
                    e.available = false;
                    e.missing_requirements.push_back("env:" + env);
                }
            }

            const std::string key = media::strutil::lower_ascii(e.name);
            e.pinned = pinned_set.count(key) > 0;
            e.disabled = disabled_set.count(key) > 0;
            e.active = snap.policy.enabled && e.available && !e.disabled && (e.always || e.pinned);
            snap.entries.push_back(std::move(e));
        }
    };

    // Roaming first, then workspace overlay with name replacement.
    if (snap.policy.roaming_enabled)
        add_from_root(snap.roaming_root, SkillSource::Roaming);
    if (snap.policy.workspace_enabled) {
        SkillSnapshot ws;
        std::vector<SkillEntry> ws_entries;
        {
            std::error_code ec;
            if (std::filesystem::exists(snap.workspace_root, ec) && std::filesystem::is_directory(snap.workspace_root, ec)) {
                for (auto it = std::filesystem::directory_iterator(snap.workspace_root, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
                    if (ec)
                        break;
                    const auto& dir = it->path();
                    if (!it->is_directory(ec))
                        continue;
                    std::filesystem::path skill_md;
                    std::filesystem::path manifest;
                    if (!find_skill_body(dir, skill_md, manifest))
                        continue;
                    SkillEntry e;
                    e.name = media::path::path_to_utf8(dir.filename());
                    e.skill_md_path = skill_md;
                    e.source = SkillSource::Workspace;
                    auto meta = parse_skill_metadata(skill_md);
                    apply_explicit_manifest(manifest, e, meta);
                    e.description = meta.description.empty() ? e.name : meta.description;
                    e.always = meta.always;
                    e.declared_tools = meta.declared_tools;
                    e.available = true;
                    if (!meta.os.empty()) {
                        const std::string cur = current_os_tag();
                        bool match = false;
                        for (const auto& one : meta.os)
                            if (one == cur) {
                                match = true;
                                break;
                            }
                        if (!match) {
                            e.available = false;
                            e.missing_requirements.push_back("os:" + cur);
                        }
                    }
                    for (const auto& b : meta.bins) {
                        if (!binary_available(b)) {
                            e.available = false;
                            e.missing_requirements.push_back("bin:" + b);
                        }
                    }
                    for (const auto& env : meta.env) {
                        const char* v = std::getenv(env.c_str());
                        if (!v || !*v) {
                            e.available = false;
                            e.missing_requirements.push_back("env:" + env);
                        }
                    }
                    const std::string key = media::strutil::lower_ascii(e.name);
                    e.pinned = pinned_set.count(key) > 0;
                    e.disabled = disabled_set.count(key) > 0;
                    e.active = snap.policy.enabled && e.available && !e.disabled && (e.always || e.pinned);
                    ws_entries.push_back(std::move(e));
                }
            }
        }
        for (auto& ws_e : ws_entries) {
            const std::string ws_key = media::strutil::lower_ascii(ws_e.name);
            snap.entries.erase(
                std::remove_if(snap.entries.begin(), snap.entries.end(), [&](const SkillEntry& cur) {
                    return media::strutil::lower_ascii(cur.name) == ws_key;
                }),
                snap.entries.end());
            snap.entries.push_back(std::move(ws_e));
        }
    }

    std::sort(snap.entries.begin(), snap.entries.end(), [](const SkillEntry& a, const SkillEntry& b) {
        return media::strutil::lower_ascii(a.name) < media::strutil::lower_ascii(b.name);
    });
    {
        std::lock_guard<std::mutex> lock(g_discover_mu);
        g_discover.key = cache_key;
        g_discover.fp = files_fp;
        g_discover.snap = snap;
    }
    return snap;
}

nlohmann::json skill_entry_json(const SkillEntry& entry) {
    return {
        {"name", entry.name},
        {"description", entry.description},
        {"source", entry.source == SkillSource::Roaming ? "roaming" : "workspace"},
        {"available", entry.available},
        {"active", entry.active},
        {"always", entry.always},
        {"pinned", entry.pinned},
        {"disabled", entry.disabled},
        {"missing_requirements", entry.missing_requirements},
        {"path", media::path::path_to_utf8(entry.skill_md_path)},
    };
}

nlohmann::json skills_list_data_json(const SkillSnapshot& snap) {
    nlohmann::json skills = nlohmann::json::array();
    for (const auto& e : snap.entries)
        skills.push_back(skill_entry_json(e));
    return {
        {"enabled", snap.policy.enabled},
        {"roaming_enabled", snap.policy.roaming_enabled},
        {"workspace_enabled", snap.policy.workspace_enabled},
        {"pinned", snap.policy.pinned},
        {"disabled", snap.policy.disabled},
        {"roots", nlohmann::json{
            {"roaming", media::path::path_to_utf8(snap.roaming_root)},
            {"workspace", media::path::path_to_utf8(snap.workspace_root)},
        }},
        {"skills", std::move(skills)},
    };
}

nlohmann::json list_skills_data_json(
    const std::string& cwd_hint,
    const SkillPolicy* policy_override)
{
    SkillPolicy policy = policy_override ? *policy_override : load_policy_from_settings();
    return skills_list_data_json(discover_skills(policy, cwd_hint));
}

} // namespace media::llm::skills


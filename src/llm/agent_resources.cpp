#include "agent_resources.hpp"

#include "agent_memory_recall.hpp"
#include "agent_retrieval.hpp"
#include "string/string_sanitizers.hpp"
#include "core/settings_store.hpp"
#include "net/uri.hpp"
#include "pm_branding_config.hpp"
#include "ui_defaults.hpp"

#include <picosha2.h>

#if defined(FEATURE_INSTALLER_VFS) && FEATURE_INSTALLER_VFS
#include "cms/pm_cms_detail.hpp"
#include "cms/pm_cms_session.hpp"
#include "service_defaults.hpp"
#endif

#if FEATURE_LLAMA
#include "lib/llm/local_vector_store/VectorStore.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <unordered_map>
#include <unordered_set>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

namespace media::llm::resources {
namespace {


std::string encode_path(std::string_view value)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/')
            out.push_back(static_cast<char>(std::tolower(c)));
        else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xf]);
            out.push_back(hex[c & 0xf]);
        }
    }
    return out;
}

std::string read_file(const std::filesystem::path& path, std::size_t limit, bool& truncated)
{
    truncated = false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string out;
    out.resize(limit + 1);
    in.read(out.data(), static_cast<std::streamsize>(out.size()));
    const std::size_t got = static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount()));
    truncated = got > limit;
    out.resize(std::min(got, limit));
    while (!out.empty()
           && (static_cast<unsigned char>(out.back()) & 0xc0) == 0x80)
        out.pop_back();
    return out;
}

std::string content_hash(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string bytes(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    // picosha2 — do not pull pm::storage::sha256_hex (FEATURE_SECURE_STORAGE).
    std::string hex;
    picosha2::hash256_hex_string(bytes.begin(), bytes.end(), hex);
    return hex;
}

std::string text_hash(const std::string& text)
{
    std::string hex;
    picosha2::hash256_hex_string(text.begin(), text.end(), hex);
    return hex;
}

bool safe_vfs_component(std::string_view value)
{
    if (value.empty() || value.front() == '/' || value.front() == '\\')
        return false;
    std::filesystem::path path(value);
    for (const auto& component : path)
        if (component == "..") return false;
    return value.find('\\') == std::string_view::npos
        && value.find('?') == std::string_view::npos
        && value.find('#') == std::string_view::npos;
}

std::string join_vfs_path(std::string base, std::string relative)
{
    std::replace(base.begin(), base.end(), '\\', '/');
    std::replace(relative.begin(), relative.end(), '\\', '/');
    const auto slash = base.find_last_of('/');
    base = slash == std::string::npos ? std::string{} : base.substr(0, slash + 1);
    return base + relative;
}

bool read_vfs_text(const std::string& server_url,
                   bool public_only,
                   const std::string& mount,
                   const std::string& path,
                   std::size_t hard_limit,
                   std::string& out)
{
    if (server_url == "mock://local") {
        const char* test_root = std::getenv("PM_LLM_TEST_VFS_ROOT");
        if (!test_root || !*test_root) return false;
        std::error_code ec;
        const auto root = std::filesystem::weakly_canonical(test_root, ec);
        const auto candidate = std::filesystem::weakly_canonical(
            root / mount / media::path::path_from_utf8(path), ec);
        const auto relative = candidate.lexically_relative(root);
        if (ec || relative.empty() || *relative.begin() == ".."
            || !std::filesystem::is_regular_file(candidate, ec))
            return false;
        bool truncated = false;
        out = read_file(candidate, hard_limit, truncated);
        return !truncated;
    }
#if defined(FEATURE_INSTALLER_VFS) && FEATURE_INSTALLER_VFS
    std::string base;
    std::string bearer;
    if (public_only) {
        base = pm::services::resolve_cms_server_base(server_url);
    } else {
        pm::cms::CmsSession session;
        std::string err;
        if (!pm::cms::resolve_cms_session(server_url, session, err))
            return false;
        base = std::move(session.base);
        bearer = std::move(session.bearer);
    }
    if (base.empty()) return false;
    const auto raw = pm::cms::read_vfs_file_raw(base, bearer, mount, path, {});
    if (!raw.ok || raw.bytes.size() > hard_limit) return false;
    out.assign(reinterpret_cast<const char*>(raw.bytes.data()), raw.bytes.size());
    return true;
#else
    (void)public_only;
    (void)mount;
    (void)path;
    (void)hard_limit;
    (void)out;
    return false;
#endif
}

std::vector<nlohmann::json> resolve_links(const RetrievalResource& resource,
                                          const std::string& body)
{
    std::vector<nlohmann::json> out;
    static const std::regex markdown_link(R"(\[[^\]]*\]\(([^)\s]+)(?:\s+["'][^"']*["'])?\))");
    for (std::sregex_iterator it(body.begin(), body.end(), markdown_link), end;
         it != end; ++it) {
        const std::string target = (*it)[1].str();
        nlohmann::json link = {{"target", target}};
        const auto parsed_target = pm::uri::parse_uri(target);
        if (parsed_target && !parsed_target->scheme.empty()) {
            link["uri"] = target;
            link["kind"] = "uri";
        } else if (!resource.vfs_mount.empty()) {
            link["kind"] = "relative";
            if (!safe_vfs_component(target)) {
                link["available"] = false;
                link["rejected"] = "path_escape";
            } else {
                const std::string remote = join_vfs_path(resource.vfs_path, target);
                link["available"] = true;
                link["uri"] = canonical_uri(
                    ResourceKind::Text, "vfs/" + resource.vfs_mount, remote);
            }
        } else {
            std::error_code ec;
            const auto candidate = (resource.base_path / target).lexically_normal();
            const auto base = std::filesystem::weakly_canonical(resource.base_path, ec);
            const auto resolved = std::filesystem::weakly_canonical(candidate, ec);
            const auto relative = resolved.lexically_relative(base);
            const bool escapes = relative.empty()
                || (!relative.empty() && *relative.begin() == "..");
            link["kind"] = "relative";
            link["available"] = !ec && !escapes
                && std::filesystem::is_regular_file(resolved, ec);
            if (!ec && !escapes) link["path"] = media::path::path_to_utf8(resolved);
            if (escapes) link["rejected"] = "path_escape";
        }
        out.push_back(std::move(link));
    }
    return out;
}

void strip_sensitive_fields(nlohmann::json& value)
{
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end();) {
            const std::string key = media::strutil::lower_ascii(it.key());
            if (key.find("token") != std::string::npos
                || key.find("secret") != std::string::npos
                || key == "authorization" || key == "bearer") {
                it = value.erase(it);
            } else {
                strip_sensitive_fields(it.value());
                ++it;
            }
        }
    } else if (value.is_array()) {
        for (auto& item : value) strip_sensitive_fields(item);
    }
}

void append_vfs_resources(std::vector<RetrievalResource>& out)
{
    nlohmann::json custom;
    std::string settings_error;
    nlohmann::json roots = nlohmann::json::array();
    if (media::settings::load_subtree(
            media::settings::SettingsSubtreeKeys::custom, custom, settings_error)
        && custom.is_object()) {
        const auto& skill_settings = custom.contains("agent_skills")
            ? custom["agent_skills"] : nlohmann::json();
        if (skill_settings.is_object()
            && skill_settings.contains("vfs_roots")
            && skill_settings["vfs_roots"].is_array())
            roots = skill_settings["vfs_roots"];
    }
    // Deterministic local mock injection for isolated tests only. Production
    // discovery remains settings-based and this is inert without the mock root.
    const char* test_root = std::getenv("PM_LLM_TEST_VFS_ROOT");
    const char* test_config = std::getenv("PM_LLM_TEST_VFS_CONFIG");
    if (test_root && *test_root && test_config && *test_config) {
        try {
            auto test_roots = nlohmann::json::parse(test_config);
            if (test_roots.is_array())
                for (auto& root : test_roots) roots.push_back(std::move(root));
        } catch (...) {
        }
    }

    for (const auto& root : roots) {
        if (!root.is_object()) continue;
        const std::string mount = root.value("mount", std::string{});
        const std::string manifest_path = root.value("manifest", std::string{});
        const std::string server_url = root.value("server_url", std::string{});
        const bool public_only = root.value("public_only", false);
        const bool trusted = root.value("trusted", false)
            || root.value("signature_verified", false);
        if (!safe_vfs_component(mount) || !safe_vfs_component(manifest_path))
            continue;

        const std::string cache_key = text_hash(
            mount + "\n" + manifest_path + "\n" + server_url);
        const auto cache_path = media::settings::get_config_dir()
            / "agent-resource-cache" / ("vfs-" + cache_key + ".json");
        nlohmann::json manifest;
        std::string body;
        if (read_vfs_text(server_url, public_only, mount, manifest_path,
                          256 * 1024, body)) {
            try {
                manifest = nlohmann::json::parse(body);
                strip_sensitive_fields(manifest);
                std::error_code ec;
                std::filesystem::create_directories(cache_path.parent_path(), ec);
                if (!ec) {
                    std::ofstream cache(cache_path, std::ios::binary | std::ios::trunc);
                    if (cache) cache << manifest.dump(2);
                }
            } catch (...) {
                manifest = {};
            }
        }
        if (manifest.is_null() || manifest.empty()) {
            try {
                std::ifstream cache(cache_path, std::ios::binary);
                if (cache) manifest = nlohmann::json::parse(cache);
            } catch (...) {
                manifest = {};
            }
        }

        const nlohmann::json* rows = nullptr;
        if (manifest.is_array()) rows = &manifest;
        else if (manifest.is_object() && manifest.contains("resources")
                 && manifest["resources"].is_array())
            rows = &manifest["resources"];
        else if (manifest.is_object() && manifest.contains("skills")
                 && manifest["skills"].is_array())
            rows = &manifest["skills"];
        if (!rows) continue;

        for (const auto& row : *rows) {
            if (!row.is_object()
                || media::strutil::lower_ascii(row.value("type", std::string{"skill"})) != "skill")
                continue;
            const std::string id = row.value("id", row.value("name", std::string{}));
            const std::string remote_body = row.value("body", row.value("path", std::string{}));
            if (id.empty() || !safe_vfs_component(id)
                || !safe_vfs_component(remote_body))
                continue;
            RetrievalResource resource;
            resource.kind = ResourceKind::Skill;
            resource.scope = "vfs/" + mount;
            resource.uri = canonical_uri(resource.kind, resource.scope, id);
            resource.qualified_name = "vfs:" + media::strutil::lower_ascii(mount) + ":" + media::strutil::lower_ascii(id);
            resource.title = row.value("title", row.value("name", id));
            resource.description = row.value("description", resource.title);
            resource.search_text = resource.title + " " + resource.description;
            resource.vfs_mount = mount;
            resource.vfs_path = remote_body;
            resource.vfs_server_url = server_url;
            resource.vfs_public_only = public_only;
            resource.media_type = row.value("media_type", std::string{"text/markdown"});
            resource.content_hash = row.value("sha256", row.value("content_hash", std::string{}));
            resource.trust = trusted ? "signed-vendor" : "untrusted";
            resource.available = trusted;
            resource.disabled = !trusted;
            // declared_tools / allowed-tools: ignored (request_tools owns grants).
            if (!trusted) resource.missing_requirements.push_back("trust:unverified");
            out.push_back(std::move(resource));
        }
    }
}

} // namespace

void index_resources(const std::vector<RetrievalResource>& entries,
                     const std::string& embed_model_hint)
{
#if FEATURE_LLAMA
    VectorStore store;
    if (!store.open(agent::memory_recall_index_dir(), embed_model_hint))
        return;
    for (const auto& resource : entries) {
        if (!resource.available || resource.disabled || resource.search_text.empty())
            continue;
        VStoreDoc doc;
        doc.source = "resource";
        doc.path = resource.uri;
        doc.title = resource.title;
        doc.chunk = resource.description;
        doc.locator = {
            {"uri", resource.uri},
            {"kind", resource.kind == ResourceKind::Skill ? "skill" : "resource"},
            {"hash", resource.content_hash},
        };
        (void)store.upsert(std::move(doc), resource.search_text);
    }
#else
    (void)entries;
    (void)embed_model_hint;
#endif
}

std::string canonical_uri(ResourceKind kind, std::string_view scope, std::string_view id)
{
    const std::string uri = std::string(pm::brand::k_app_id_u8)
        + "://" + (kind == ResourceKind::Skill ? "skill/" : "resource/")
        + encode_path(scope) + "/" + encode_path(id);
    return pm::uri::parse_uri(uri) ? uri : std::string{};
}

bool is_canonical_resource_uri(std::string_view uri, ResourceKind kind)
{
    const auto parsed = pm::uri::parse_uri(uri, {
        .reject_userinfo = true,
        .reject_percent_encoded_authority = true,
        .require_authority_host = true,
    });
    return parsed && parsed->scheme == media::strutil::lower_ascii(pm::brand::k_app_id_u8)
        && parsed->host == (kind == ResourceKind::Skill ? "skill" : "resource")
        && !parsed->path.empty();
}

ResourceSnapshot discover(const skills::SkillPolicy& policy,
                          const std::string& cwd_hint,
                          const std::string& embed_model_hint)
{
    ResourceSnapshot snapshot;
    snapshot.skills = skills::discover_skills(policy, cwd_hint);
    snapshot.entries.reserve(snapshot.skills.entries.size());
    for (const auto& skill : snapshot.skills.entries) {
        RetrievalResource resource;
        resource.kind = ResourceKind::Skill;
        resource.scope = skill.source == skills::SkillSource::Workspace
            ? "workspace" : "config";
        resource.uri = canonical_uri(resource.kind, resource.scope, skill.name);
        resource.qualified_name = resource.scope + ":" + media::strutil::lower_ascii(skill.name);
        resource.title = skill.name;
        resource.description = skill.description;
        resource.search_text = skill.name + " " + skill.description;
        resource.body_path = skill.skill_md_path;
        resource.base_path = skill.skill_md_path.parent_path();
        resource.media_type = skill.media_type;
        resource.content_hash = content_hash(skill.skill_md_path);
        resource.trust = skill.source == skills::SkillSource::Workspace
            ? "workspace" : "user-local";
        resource.declared_tools = skill.declared_tools;
        resource.missing_requirements = skill.missing_requirements;
        resource.available = skill.available;
        resource.disabled = skill.disabled || !snapshot.skills.policy.enabled;
        resource.pinned = skill.pinned;
        resource.always = skill.always;
        snapshot.entries.push_back(std::move(resource));
    }
    append_vfs_resources(snapshot.entries);
    std::sort(snapshot.entries.begin(), snapshot.entries.end(),
              [](const RetrievalResource& a, const RetrievalResource& b) {
                  return a.uri < b.uri;
              });
    snapshot.entries.erase(
        std::unique(snapshot.entries.begin(), snapshot.entries.end(),
                    [](const RetrievalResource& a, const RetrievalResource& b) {
                        return a.uri == b.uri;
                    }),
        snapshot.entries.end());
    (void)embed_model_hint;
    return snapshot;
}

std::vector<std::size_t> rank(std::string_view query,
                              const std::vector<RetrievalResource>& resources,
                              std::size_t top_k,
                              const std::string& embed_model_hint)
{
    std::vector<std::string> texts;
    texts.reserve(resources.size());
    for (const auto& resource : resources)
        texts.push_back(resource.search_text);
    std::vector<std::vector<std::size_t>> rank_lists;
    rank_lists.push_back(retrieval::ranks_from_scores(
        retrieval::bm25_rank(query, texts), resources.size()));
#if FEATURE_LLAMA
    VectorStore store;
    if (store.open(agent::memory_recall_index_dir(), embed_model_hint)) {
        const auto hits = store.search(query, resources.size(), "resource", {});
        std::unordered_map<std::string, std::size_t> by_uri;
        for (std::size_t i = 0; i < resources.size(); ++i)
            by_uri[resources[i].uri] = i;
        std::vector<std::size_t> dense;
        for (const auto& hit : hits) {
            const auto found = by_uri.find(hit.doc.path);
            if (found != by_uri.end()) dense.push_back(found->second);
        }
        if (!dense.empty()) rank_lists.push_back(std::move(dense));
    }
#else
    (void)embed_model_hint;
#endif
    auto ranked = retrieval::rrf_fuse(rank_lists, resources.size(), top_k, 60);
    ranked.erase(std::remove_if(ranked.begin(), ranked.end(), [&](std::size_t index) {
        return index >= resources.size()
            || !resources[index].available || resources[index].disabled;
    }), ranked.end());
    return ranked;
}

std::optional<std::size_t> resolve(std::string_view uri_or_name,
                                   const std::vector<RetrievalResource>& resources)
{
    const std::string query = media::strutil::lower_ascii(std::string(uri_or_name));
    std::optional<std::size_t> plain;
    for (std::size_t i = 0; i < resources.size(); ++i) {
        const auto& resource = resources[i];
        if (media::strutil::lower_ascii(resource.uri) == query || media::strutil::lower_ascii(resource.qualified_name) == query)
            return i;
        if (media::strutil::lower_ascii(resource.title) == query) {
            if (plain) return std::nullopt;
            plain = i;
        }
    }
    return plain;
}

nlohmann::json metadata_json(const RetrievalResource& resource)
{
    return {
        {"uri", resource.uri},
        {"qualified_name", resource.qualified_name},
        {"type", resource.kind == ResourceKind::Skill ? "skill" : "resource"},
        {"scope", resource.scope},
        {"title", resource.title},
        {"description", resource.description},
        {"media_type", resource.media_type},
        {"content_hash", resource.content_hash},
        {"trust", resource.trust},
        {"available", resource.available},
        {"disabled", resource.disabled},
        {"pinned", resource.pinned},
        {"always", resource.always},
        {"missing_requirements", resource.missing_requirements},
    };
}

nlohmann::json materialize(const RetrievalResource& resource,
                           std::size_t byte_limit,
                           int traversal_depth,
                           const std::string& session_id)
{
    if (resource.disabled)
        return {{"ok", false}, {"error", "resource is disabled"}, {"uri", resource.uri}};
    if (!resource.available)
        return {{"ok", false}, {"error", "resource requirements are unavailable"},
                {"uri", resource.uri}, {"missing_requirements", resource.missing_requirements}};
    if (traversal_depth < 0 || traversal_depth > 4)
        return {{"ok", false}, {"error", "resource traversal depth exceeds limit"}};

    static thread_local std::unordered_map<std::string, std::unordered_set<std::string>> loaded;
    if (!session_id.empty() && !resource.content_hash.empty()
        && loaded[session_id].count(resource.content_hash)) {
        auto result = metadata_json(resource);
        result["ok"] = true;
        result["duplicate"] = true;
        result["body"] = "";
        result["policy_note"] =
            "skill allowed-tools / declared_tools are ignored; use request_tools for schema grants";
        return result;
    }

    bool truncated = false;
    std::string body;
    if (!resource.vfs_mount.empty()) {
        if (!safe_vfs_component(resource.vfs_mount)
            || !safe_vfs_component(resource.vfs_path)
            || !read_vfs_text(resource.vfs_server_url, resource.vfs_public_only,
                              resource.vfs_mount, resource.vfs_path,
                              96 * 1024 + 1, body))
            return {{"ok", false}, {"error", "remote resource body could not be fetched"},
                    {"uri", resource.uri}};
        if (body.size() > byte_limit) {
            body.resize(byte_limit);
            truncated = true;
        }
    } else {
        body = read_file(resource.body_path, std::max<std::size_t>(1, byte_limit), truncated);
        std::error_code ec;
        if (body.empty() && (!std::filesystem::is_empty(resource.body_path, ec) || ec))
            return {{"ok", false}, {"error", "resource body could not be read"}, {"uri", resource.uri}};
    }
    const std::string actual_hash = text_hash(body);
    if (!resource.content_hash.empty() && !truncated
        && media::strutil::lower_ascii(actual_hash) != media::strutil::lower_ascii(resource.content_hash))
        return {{"ok", false}, {"error", "resource content hash mismatch"}, {"uri", resource.uri}};
    if (!session_id.empty() && loaded[session_id].count(actual_hash)) {
        auto result = metadata_json(resource);
        result["ok"] = true;
        result["duplicate"] = true;
        result["body"] = "";
        result["policy_note"] =
            "skill allowed-tools / declared_tools are ignored; use request_tools for schema grants";
        return result;
    }
    body = media::str_san::apply_text_sanitizers(
        body, media::str_san::k_default_skill_prompt_sanitizers);
    if (!session_id.empty())
        loaded[session_id].insert(actual_hash);

    auto result = metadata_json(resource);
    result["ok"] = true;
    result["body"] = std::move(body);
    result["truncated"] = truncated;
    result["links"] = resolve_links(resource, result["body"].get_ref<const std::string&>());
    result["policy_note"] =
        "skill allowed-tools / declared_tools are ignored; use request_tools for schema grants";
    return result;
}

} // namespace media::llm::resources

#include "llm/chat_session_store.hpp"
#include "json/json_utils.hpp"

#include "constants.hpp"
#include "core/settings_store.hpp"
#include "llm/agent_internal.hpp"
#include "llm/agent_memory.hpp"
#include "pm_branding_config.hpp"
#include "policy/app_policy.hpp"
#include "security/security_defaults.hpp"

#if defined(FEATURE_SECURE_STORAGE) && FEATURE_SECURE_STORAGE
#include "model/resource_schema.hpp"
#include "storage/secure_storage.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <system_error>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

namespace media::llm::chat_sessions {
namespace {

constexpr int k_max_sessions = 50;
constexpr const char* k_env_backend = "PM_CHAT_SESSION_STORE";
constexpr const char* k_env_root = "PM_CHAT_SESSION_STORE_ROOT";
constexpr const char* k_env_protection = "PM_CHAT_SESSION_STORE_PROTECTION";
constexpr const char* k_env_max_sessions = "PM_CHAT_SESSION_STORE_MAX";

long long now_unix_ms()
{
    namespace chr = std::chrono;
    return chr::duration_cast<chr::milliseconds>(
        chr::system_clock::now().time_since_epoch()).count();
}

long long json_i64(const nlohmann::json& j, const char* key)
{
    if (!j.is_object() || !j.contains(key)) return 0;
    const auto& v = j[key];
    if (v.is_number_integer()) return v.get<long long>();
    if (v.is_number_unsigned()) return static_cast<long long>(v.get<unsigned long long>());
    if (v.is_number_float()) return static_cast<long long>(v.get<double>());
    if (v.is_string()) {
        try { return std::stoll(v.get<std::string>()); } catch (...) {}
    }
    return 0;
}

bool atomic_write_json(const std::filesystem::path& path, const nlohmann::json& j, std::string& err)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec && !std::filesystem::is_directory(path.parent_path())) {
        err = ec.message();
        return false;
    }
    const std::filesystem::path tmp = media::llm::agent::detail::unique_sibling_temp(path);
    {
        std::ofstream f(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "open tmp failed";
            return false;
        }
        f << media::llm::agent::detail::safe_json_dump(j, 2);
        f.flush();
        if (!f.good()) {
            err = "write tmp failed";
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    if (!media::llm::agent::detail::replace_file_preserving_destination(tmp, path)) {
        err = "replace failed";
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool read_json_file(const std::filesystem::path& path, nlohmann::json& out)
{
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f) return false;
    out = media::json::try_parse(f);
    return !out.is_discarded();
}

void append_unique_string(nlohmann::json& arr, const std::string& value)
{
    if (!arr.is_array()) arr = nlohmann::json::array();
    for (const auto& el : arr) {
        if (el.is_string() && el.get<std::string>() == value)
            return;
    }
    arr.push_back(value);
}

std::vector<std::filesystem::path> collect_agent_log_paths(const nlohmann::json& session)
{
    std::vector<std::filesystem::path> out;
    if (!session.is_object()) return out;
    const std::string single = session.value("agent_log_path", std::string{});
    if (!single.empty()) out.emplace_back(single);
    const auto it = session.find("agent_log_paths");
    if (it != session.end() && it->is_array()) {
        for (const auto& el : *it) {
            if (el.is_string()) out.emplace_back(el.get<std::string>());
        }
    }
    return out;
}

using media::strutil::lower_ascii;

bool env_is(const char* name, const char* expected)
{
    const char* value = std::getenv(name);
    return value && lower_ascii(value) == expected;
}

int env_int(const char* name, int fallback)
{
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    try {
        const int parsed = std::stoi(value);
        return parsed > 0 ? parsed : fallback;
    } catch (...) {
        return fallback;
    }
}

std::string safe_uri_segment(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '-' || c == '_' || c == '.')
            out.push_back(c);
        else
            out.push_back('_');
    }
    return out.empty() ? "session" : out;
}

std::string session_uri(const std::string& id)
{
    return "pm://session/" + safe_uri_segment(agent::sanitize_session_id(id));
}

std::string session_memory_uri(const std::string& id)
{
    return "pm://session-memory/" + safe_uri_segment(agent::sanitize_session_id(id));
}

std::string session_log_uri(const std::string& session_id, const std::string& log_id)
{
    return "pm://session-log/" + safe_uri_segment(agent::sanitize_session_id(session_id))
        + "__" + safe_uri_segment(agent::sanitize_session_id(log_id));
}

#if defined(FEATURE_SECURE_STORAGE) && FEATURE_SECURE_STORAGE
pm::storage::StoragePaths secure_paths(const StoreOptions& options)
{
    const std::filesystem::path root = options.root_override.empty()
        ? agent::session_store_dir()
        : options.root_override;
    return pm::storage::resolve_storage_paths(root);
}

bool secure_plaintext_allowed(const StoreOptions& options)
{
    return options.protection == ProtectionMode::Plain ||
           options.protection == ProtectionMode::Signed;
}

bool secure_load_session(const StoreOptions& options,
                         const std::string& id,
                         nlohmann::json& out_session,
                         std::string& err)
{
    out_session = nullptr;
    pm::storage::DocumentLoadPolicy policy;
    policy.allowed_trust = {pm::model::TrustLevel::UserLocal};
    policy.require_encryption =
        options.protection == ProtectionMode::Encrypted ||
        options.protection == ProtectionMode::EncryptedWithUserPresence;
    policy.require_signature = options.protection != ProtectionMode::Plain;
    policy.allow_vendor_seed_plaintext = secure_plaintext_allowed(options);
    const auto r = pm::storage::load_document(secure_paths(options), session_uri(id), policy);
    if (!r.ok) {
        if (r.error.rfind("document not found:", 0) == 0)
            return true;
        err = r.error;
        return false;
    }
    try {
        out_session = media::json::try_parse(std::string_view(
            reinterpret_cast<const char*>(r.document.data.data()),
            r.document.data.size()));
    } catch (...) {
        out_session = nullptr;
    }
    if (!out_session.is_object()) {
        err = "invalid secure chat session json";
        out_session = nullptr;
        return false;
    }
    return true;
}

bool secure_load_json_document(const StoreOptions& options,
                               const std::string& uri,
                               nlohmann::json& out_document,
                               bool& found,
                               std::string& err)
{
    found = false;
    out_document = nullptr;
    pm::storage::DocumentLoadPolicy policy;
    policy.allowed_trust = {pm::model::TrustLevel::UserLocal};
    policy.require_encryption =
        options.protection == ProtectionMode::Encrypted ||
        options.protection == ProtectionMode::EncryptedWithUserPresence;
    policy.require_signature = options.protection != ProtectionMode::Plain;
    policy.allow_vendor_seed_plaintext = secure_plaintext_allowed(options);
    const auto r = pm::storage::load_document(secure_paths(options), uri, policy);
    if (!r.ok) {
        if (r.error.rfind("document not found:", 0) == 0)
            return true;
        err = r.error;
        return false;
    }
    found = true;
    try {
        out_document = media::json::try_parse(std::string_view(
            reinterpret_cast<const char*>(r.document.data.data()),
            r.document.data.size()));
    } catch (...) {
        out_document = nullptr;
    }
    if (!out_document.is_object() && !out_document.is_array()) {
        err = "invalid secure session sidecar json";
        out_document = nullptr;
        return false;
    }
    return true;
}

bool secure_save_json_document(const StoreOptions& options,
                               const std::string& uri,
                               pm::model::ResourceType type,
                               const nlohmann::json& document,
                               std::string& err)
{
    const std::string text = media::llm::agent::detail::safe_json_dump(document, 2);
    pm::storage::PutDocumentRequest req;
    req.uri = uri;
    req.type = type;
    req.trust = pm::model::TrustLevel::UserLocal;
    req.version = "1";
    req.provider = pm::brand::k_app_id_u8;
    req.encrypt = !secure_plaintext_allowed(options);
    req.allow_plaintext_seed = secure_plaintext_allowed(options);
    req.data.assign(text.begin(), text.end());
    const auto r = pm::storage::put_document(secure_paths(options), req);
    if (!r.ok) {
        err = r.error;
        return false;
    }
    if (options.protection != ProtectionMode::Plain) {
        const auto signed_doc = pm::storage::sign_document(secure_paths(options), req.uri);
        if (!signed_doc.ok) {
            err = signed_doc.error;
            return false;
        }
    }
    return true;
}

bool secure_save_session(const StoreOptions& options,
                         const nlohmann::json& session,
                         std::string& err)
{
    const std::string id = session.value("id", std::string{});
    if (id.empty()) {
        err = "missing session.id";
        return false;
    }
    return secure_save_json_document(options, session_uri(id), pm::model::ResourceType::SessionTranscript, session, err);
}
#endif

bool secure_backend_requested(const StoreOptions& options)
{
    return options.backend == StoreBackend::SecureStorage;
}

void apply_backend_token(StoreOptions& options, const std::string& token)
{
    const std::string value = lower_ascii(token);
    if (value == "secure-storage" || value == "secure")
        options.backend = StoreBackend::SecureStorage;
#if defined(FEATURE_COMMAND_TEST) && FEATURE_COMMAND_TEST
    else if (value == "legacy-files" || value == "legacy" || value == "files")
        options.backend = StoreBackend::LegacyFiles;
#endif
}

void apply_settings_backend_token(StoreOptions& options, const std::string& token)
{
    const std::string value = lower_ascii(token);
    if (value == "secure-storage" || value == "secure")
        options.backend = StoreBackend::SecureStorage;
}

void apply_protection_token(StoreOptions& options, const std::string& token)
{
    const std::string value = lower_ascii(token);
    if (value == "plain") {
        options.protection = ProtectionMode::Plain;
    } else if (value == "signed" || value == "signed-only") {
        options.protection = ProtectionMode::Signed;
    } else if (value == "encrypted-up" || value == "encrypted+up") {
        options.protection = ProtectionMode::EncryptedWithUserPresence;
    } else if (value == "encrypted") {
        options.protection = ProtectionMode::Encrypted;
    }
}

void apply_settings_protection_token(StoreOptions& options, const std::string& token)
{
    const std::string value = lower_ascii(token);
#if defined(FEATURE_COMMAND_TEST) && FEATURE_COMMAND_TEST
    apply_protection_token(options, value);
#else
    if (value != "plain")
        apply_protection_token(options, value);
#endif
}

void apply_settings_options(StoreOptions& options)
{
    nlohmann::json security;
    std::string err;
    if (!media::settings::load_subtree(media::settings::SettingsSubtreeKeys::security, security, err) ||
        !security.is_object())
        return;
    const auto ss = security.find("secureStorage");
    if (ss == security.end() || !ss->is_object())
        return;
    const auto sessions = ss->find("sessions");
    if (sessions == ss->end() || !sessions->is_object())
        return;
    if (auto it = sessions->find("backend"); it != sessions->end() && it->is_string())
        apply_settings_backend_token(options, it->get<std::string>());
    if (auto it = sessions->find("root"); it != sessions->end() && it->is_string() && !it->get<std::string>().empty())
        options.root_override = media::path::path_from_utf8(it->get<std::string>());
    if (auto it = sessions->find("protection"); it != sessions->end() && it->is_string())
        apply_settings_protection_token(options, it->get<std::string>());
    if (auto it = sessions->find("maxSessions"); it != sessions->end() && it->is_number_integer()) {
        const int n = it->get<int>();
        if (n > 0) options.max_sessions = n;
    }
}

void apply_managed_policy_options(StoreOptions& options)
{
#if defined(FEATURE_WINDOWS_GPO) && FEATURE_WINDOWS_GPO
    // Managed policy only narrows/redirects the secure store. It never enables
    // legacy files or plaintext session storage.
    options.backend = StoreBackend::SecureStorage;

    const std::string root = pm::policy::policy_session_storage_root();
    if (!root.empty())
        options.root_override = media::path::path_from_utf8(root);

    const std::string protection = lower_ascii(pm::policy::policy_session_storage_protection());
    if (protection == "signed") {
        options.protection = ProtectionMode::Signed;
    } else if (protection == "encrypted") {
        if (options.protection == ProtectionMode::Plain || options.protection == ProtectionMode::Signed)
            options.protection = ProtectionMode::Encrypted;
    } else if (protection == "encrypted+up" || protection == "encrypted-up") {
        options.protection = ProtectionMode::EncryptedWithUserPresence;
    }

    if (pm::policy::policy_session_storage_require_encryption()) {
        if (options.protection == ProtectionMode::Plain || options.protection == ProtectionMode::Signed)
            options.protection = ProtectionMode::Encrypted;
    }
    if (pm::policy::policy_session_storage_require_signing() && options.protection == ProtectionMode::Plain)
        options.protection = ProtectionMode::Signed;
#endif
}

bool path_is_under_agent_sessions(const std::filesystem::path& path)
{
    try {
        std::error_code ec;
        const std::filesystem::path base =
            std::filesystem::absolute(agent::session_store_dir(), ec).lexically_normal();
        if (ec) return false;
        const std::filesystem::path p = std::filesystem::absolute(path, ec).lexically_normal();
        if (ec) return false;
        const std::string base_s = lower_ascii(media::path::path_to_utf8(base));
        const std::string p_s = lower_ascii(media::path::path_to_utf8(p));
        return p_s == base_s
            || (p_s.size() > base_s.size()
                && p_s.compare(0, base_s.size(), base_s) == 0
                && (p_s[base_s.size()] == '\\' || p_s[base_s.size()] == '/'));
    } catch (...) {
        return false;
    }
}

bool remove_agent_log_file(const std::filesystem::path& path, bool prune_empty_parent = true)
{
    if (path.empty() || path.extension() != ".json" || !path_is_under_agent_sessions(path))
        return false;
    std::error_code ec;
    const bool removed = std::filesystem::remove(path, ec);
    if (removed && prune_empty_parent) {
        ec.clear();
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty() && path_is_under_agent_sessions(parent))
            std::filesystem::remove(parent, ec); // Only succeeds when the directory is empty.
    }
    return removed;
}

int remove_agent_logs_for_task_id(const std::string& agent_task_id)
{
    if (agent_task_id.empty()) return 0;
    int removed = 0;
    std::error_code ec;
    const auto base = agent::session_store_dir();
    if (!std::filesystem::is_directory(base, ec) || ec) return 0;
    for (std::filesystem::recursive_directory_iterator it(base, ec), end; it != end && !ec; it.increment(ec)) {
        const std::filesystem::path path = it->path();
        if (!it->is_regular_file(ec) || path.extension() != ".json")
            continue;
        const std::string name = media::path::path_to_utf8(path.filename());
        if (name.rfind("agent-", 0) != 0)
            continue;
        try {
            std::ifstream f(path, std::ios::in | std::ios::binary);
            if (!f) continue;
            nlohmann::json j = media::json::try_parse(f);
            if (j.is_discarded()) continue;
            const std::string sid = j.value("agent", nlohmann::json::object())
                                      .value("session_id", std::string{});
            if (sid == agent_task_id && remove_agent_log_file(path, false))
                ++removed;
        } catch (...) {
        }
    }
    return removed;
}

nlohmann::json entries_from_agent_session_file(const std::string& session_id,
                                               const std::string& source_session_dir,
                                               long long now_ms)
{
    nlohmann::json entries = nlohmann::json::array();
    if (session_id.empty() || source_session_dir.empty())
        return entries;
    try {
        const std::filesystem::path path =
            media::path::path_from_utf8(source_session_dir) / (session_id + ".json");
        nlohmann::json root;
        if (!read_json_file(path, root) || !root.is_object() ||
            !root.contains("events") || !root["events"].is_array())
            return entries;
        int id = 1;
        for (const auto& ev : root["events"]) {
            if (!ev.is_object()) continue;
            const std::string user = ev.value("user", std::string{});
            const std::string assistant = ev.value("assistant", std::string{});
            if (!user.empty()) {
                entries.push_back({{"id", id++}, {"role", "user"}, {"text", user}, {"ts", now_ms + id}});
            }
            if (!assistant.empty()) {
                entries.push_back({{"id", id++}, {"role", "assistant"}, {"text", assistant}, {"ts", now_ms + id}});
            }
        }
    } catch (...) {
        entries = nlohmann::json::array();
    }
    return entries;
}

void copy_agent_session_into_default_store(const std::string& session_id,
                                           const std::string& source_session_dir)
{
    if (session_id.empty() || source_session_dir.empty())
        return;
    try {
        const std::filesystem::path src =
            media::path::path_from_utf8(source_session_dir) / (session_id + ".json");
        const std::filesystem::path dst = agent::session_store_dir() / (session_id + ".json");
        std::error_code ec;
        if (std::filesystem::equivalent(src, dst, ec) && !ec)
            return;
        ec.clear();
        if (!std::filesystem::exists(src, ec) || ec)
            return;
        ec.clear();
        std::filesystem::create_directories(dst.parent_path(), ec);
        if (ec) return;
        ec.clear();
        if (!std::filesystem::exists(dst, ec))
            std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
    } catch (...) {
    }
}

} // namespace

StoreOptions active_options()
{
    StoreOptions options;
    options.max_sessions = pm::security::k_session_storage_max_sessions_default;
    apply_settings_options(options);
    const char* backend = std::getenv(k_env_backend);
    if (backend && *backend)
        apply_backend_token(options, backend);
    const char* root = std::getenv(k_env_root);
    if (root && *root)
        options.root_override = media::path::path_from_utf8(root);
    const char* protection = std::getenv(k_env_protection);
    if (protection && *protection) {
#if defined(FEATURE_COMMAND_TEST) && FEATURE_COMMAND_TEST
        apply_protection_token(options, protection);
#else
        const std::string value = lower_ascii(protection);
        if (value != "plain")
            apply_protection_token(options, value);
#endif
    }
    options.max_sessions = env_int(k_env_max_sessions, options.max_sessions);
    apply_managed_policy_options(options);
    return options;
}

bool using_secure_storage()
{
    return active_options().backend == StoreBackend::SecureStorage;
}

bool try_list_secure_memory_ids(std::vector<std::string>& out, std::string& err)
{
    out.clear();
    err.clear();
    if (!using_secure_storage())
        return false;
#if defined(FEATURE_SECURE_STORAGE) && FEATURE_SECURE_STORAGE
    const StoreOptions options = active_options();
    const auto docs = pm::storage::list_documents(secure_paths(options));
    if (!docs.ok) {
        err = docs.error;
        return true;
    }
    const std::string prefix = "pm://session-memory/";
    for (const auto& meta : docs.items) {
        if (meta.type != pm::model::ResourceType::SessionMemory)
            continue;
        if (meta.uri.rfind(prefix, 0) != 0)
            continue;
        const std::string id = meta.uri.substr(prefix.size());
        if (!id.empty())
            out.push_back(id);
    }
    return true;
#else
    err = "secure storage feature is disabled";
    return true;
#endif
}

std::filesystem::path store_dir()
{
    const StoreOptions options = active_options();
    if (secure_backend_requested(options))
        return options.root_override.empty() ? agent::session_store_dir() : options.root_override;
    auto dir = agent::session_store_dir() / "chat-web";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path session_path(const std::string& id)
{
    return store_dir() / (agent::sanitize_session_id(id) + ".json");
}

nlohmann::json meta_row(const nlohmann::json& session)
{
    return nlohmann::json{{"id", session.value("id", std::string{})},
                          {"title", session.value("title", std::string{"Chat"})},
                          {"createdAt", json_i64(session, "createdAt")},
                          {"updatedAt", json_i64(session, "updatedAt")},
                          {"entryCount", session.contains("entries") && session["entries"].is_array()
                              ? session["entries"].size() : 0u}};
}

std::vector<nlohmann::json> list_meta(std::string& err)
{
    err.clear();
    std::vector<nlohmann::json> out;
    const StoreOptions options = active_options();
    if (secure_backend_requested(options)) {
#if defined(FEATURE_SECURE_STORAGE) && FEATURE_SECURE_STORAGE
        const auto r = pm::storage::list_documents(secure_paths(options));
        if (!r.ok) {
            err = r.error;
            return out;
        }
        for (const auto& meta : r.items) {
            if (meta.type != pm::model::ResourceType::SessionTranscript)
                continue;
            nlohmann::json session;
            std::string load_err;
            const std::string id = meta.uri.rfind("pm://session/", 0) == 0
                ? meta.uri.substr(std::string("pm://session/").size())
                : std::string{};
            if (!id.empty() && secure_load_session(options, id, session, load_err) && session.is_object())
                out.push_back(meta_row(session));
        }
        std::sort(out.begin(), out.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
            return json_i64(a, "updatedAt") > json_i64(b, "updatedAt");
        });
        if (static_cast<int>(out.size()) > options.max_sessions)
            out.resize(options.max_sessions);
        return out;
#else
        err = "secure session storage is not compiled in";
        return out;
#endif
    }
    std::error_code ec;
    const auto dir = store_dir();
    if (!std::filesystem::is_directory(dir, ec) || ec)
        return out;
    for (std::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".json")
            continue;
        nlohmann::json session;
        if (read_json_file(it->path(), session) && session.is_object())
            out.push_back(meta_row(session));
    }
    std::sort(out.begin(), out.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return json_i64(a, "updatedAt") > json_i64(b, "updatedAt");
    });
    if (static_cast<int>(out.size()) > options.max_sessions)
        out.resize(options.max_sessions);
    return out;
}

bool load(const std::string& id, nlohmann::json& out_session, std::string& err)
{
    err.clear();
    out_session = nullptr;
    if (id.empty()) {
        err = "missing id";
        return false;
    }
    const StoreOptions options = active_options();
    if (secure_backend_requested(options)) {
#if defined(FEATURE_SECURE_STORAGE) && FEATURE_SECURE_STORAGE
        return secure_load_session(options, id, out_session, err);
#else
        err = "secure session storage is not compiled in";
        return false;
#endif
    }
    const auto path = session_path(id);
    if (!std::filesystem::exists(path))
        return true;
    if (!read_json_file(path, out_session) || !out_session.is_object()) {
        err = "invalid chat session json";
        out_session = nullptr;
        return false;
    }
    return true;
}

bool save(const nlohmann::json& session, std::string& err)
{
    err.clear();
    if (!session.is_object()) {
        err = "missing session";
        return false;
    }
    const std::string id = session.value("id", std::string{});
    if (id.empty()) {
        err = "missing session.id";
        return false;
    }
    nlohmann::json merged = session;
    nlohmann::json existing;
    std::string load_err;
    if (load(id, existing, load_err) && existing.is_object()) {
        if (!merged.contains("agent_task_id") && existing.contains("agent_task_id"))
            merged["agent_task_id"] = existing["agent_task_id"];
        if (!merged.contains("agent_log_paths") && existing.contains("agent_log_paths"))
            merged["agent_log_paths"] = existing["agent_log_paths"];
    }
    const StoreOptions options = active_options();
    if (secure_backend_requested(options)) {
#if defined(FEATURE_SECURE_STORAGE) && FEATURE_SECURE_STORAGE
        return secure_save_session(options, merged, err);
#else
        err = "secure session storage is not compiled in";
        return false;
#endif
    }
    return atomic_write_json(session_path(id), merged, err);
}

bool merge_host_fields(const std::string& id, const nlohmann::json& patch, std::string& err)
{
    err.clear();
    if (id.empty()) {
        err = "missing id";
        return false;
    }
    nlohmann::json session;
    if (!load(id, session, err)) return false;
    const long long now = now_unix_ms();
    if (!session.is_object()) {
        session = {{"id", id}, {"title", "Chat"}, {"createdAt", now}, {"updatedAt", now},
                   {"entries", nlohmann::json::array()}};
    }
    for (auto it = patch.begin(); it != patch.end(); ++it)
        session[it.key()] = it.value();
    if (!session.contains("updatedAt"))
        session["updatedAt"] = now;
    return save(session, err);
}

std::string lookup_agent_task_id(const std::string& chat_session_id)
{
    nlohmann::json session;
    std::string err;
    if (!load(chat_session_id, session, err) || !session.is_object())
        return {};
    return session.value("agent_task_id", std::string{});
}

bool store_agent_task_id(const std::string& chat_session_id, const std::string& agent_task_id, std::string& err)
{
    if (chat_session_id.empty() || agent_task_id.empty()) return true;
    return merge_host_fields(chat_session_id, nlohmann::json{{"agent_task_id", agent_task_id}}, err);
}

bool store_agent_log_path(const std::string& chat_session_id, const std::string& agent_log_path, std::string& err)
{
    if (chat_session_id.empty() || agent_log_path.empty()) return true;
    nlohmann::json session;
    if (!load(chat_session_id, session, err)) return false;
    if (!session.is_object()) {
        const long long now = now_unix_ms();
        session = {{"id", chat_session_id}, {"title", "Chat"}, {"createdAt", now},
                   {"updatedAt", now}, {"entries", nlohmann::json::array()}};
    }
    append_unique_string(session["agent_log_paths"], agent_log_path);
    return save(session, err);
}

DeleteResult remove(const std::string& id, std::string& err)
{
    err.clear();
    DeleteResult result;
    nlohmann::json session;
    if (!load(id, session, err))
        return result;
    std::vector<std::filesystem::path> agent_log_paths;
    if (session.is_object()) {
        result.agent_task_id = session.value("agent_task_id", std::string{});
        agent_log_paths = collect_agent_log_paths(session);
    }
    std::error_code ec;
    const StoreOptions options = active_options();
    if (secure_backend_requested(options)) {
#if defined(FEATURE_SECURE_STORAGE) && FEATURE_SECURE_STORAGE
        const auto deleted = pm::storage::delete_document(secure_paths(options), session_uri(id));
        result.removed = deleted.ok;
        if (!deleted.ok && deleted.error.rfind("document not found:", 0) != 0)
            err = deleted.error;
#else
        err = "secure session storage is not compiled in";
#endif
    } else {
        result.removed = std::filesystem::remove(session_path(id), ec);
        if (ec) err = ec.message();
    }
    if (!err.empty() || !result.removed)
        return result;

    for (const auto& path : agent_log_paths) {
        std::string log_err;
        (void)delete_session_log_document(id, media::path::path_to_utf8(path.stem()), log_err);
        if (remove_agent_log_file(path))
            ++result.agent_logs_removed;
    }
    if (!result.agent_task_id.empty()) {
        result.agent_session_removed = agent::session_delete(result.agent_task_id);
        result.agent_logs_removed += remove_agent_logs_for_task_id(result.agent_task_id);
    }
    return result;
}

bool load_session_memory_document(const std::string& session_id,
                                  nlohmann::json& out_document,
                                  bool& found,
                                  std::string& err)
{
    err.clear();
    found = false;
    out_document = nullptr;
    if (session_id.empty()) return true;
    const StoreOptions options = active_options();
    if (!secure_backend_requested(options)) {
        err = "secure session storage is not active";
        return false;
    }
#if defined(FEATURE_SECURE_STORAGE) && FEATURE_SECURE_STORAGE
    return secure_load_json_document(options, session_memory_uri(session_id), out_document, found, err);
#else
    err = "secure storage feature is disabled";
    return false;
#endif
}

bool save_session_memory_document(const std::string& session_id,
                                  const nlohmann::json& document,
                                  std::string& err)
{
    err.clear();
    if (session_id.empty()) {
        err = "missing session id";
        return false;
    }
    const StoreOptions options = active_options();
    if (!secure_backend_requested(options)) {
        err = "secure session storage is not active";
        return false;
    }
#if defined(FEATURE_SECURE_STORAGE) && FEATURE_SECURE_STORAGE
    return secure_save_json_document(options,
                                     session_memory_uri(session_id),
                                     pm::model::ResourceType::SessionMemory,
                                     document,
                                     err);
#else
    err = "secure storage feature is disabled";
    return false;
#endif
}

bool delete_session_memory_document(const std::string& session_id, std::string& err)
{
    err.clear();
    if (session_id.empty()) return true;
    const StoreOptions options = active_options();
    if (!secure_backend_requested(options)) return true;
#if defined(FEATURE_SECURE_STORAGE) && FEATURE_SECURE_STORAGE
    const auto deleted = pm::storage::delete_document(secure_paths(options), session_memory_uri(session_id));
    if (!deleted.ok && deleted.error.rfind("document not found:", 0) != 0) {
        err = deleted.error;
        return false;
    }
    return true;
#else
    err = "secure storage feature is disabled";
    return false;
#endif
}

bool load_session_log_document(const std::string& session_id,
                               const std::string& log_id,
                               nlohmann::json& out_document,
                               bool& found,
                               std::string& err)
{
    err.clear();
    found = false;
    out_document = nullptr;
    if (session_id.empty() || log_id.empty()) return true;
    const StoreOptions options = active_options();
    if (!secure_backend_requested(options)) {
        err = "secure session storage is not active";
        return false;
    }
#if defined(FEATURE_SECURE_STORAGE) && FEATURE_SECURE_STORAGE
    return secure_load_json_document(options, session_log_uri(session_id, log_id), out_document, found, err);
#else
    err = "secure storage feature is disabled";
    return false;
#endif
}

bool load_agent_final_text(const std::string& session_id,
                           const std::string& log_id,
                           const std::string& fallback_path,
                           std::size_t offset,
                           std::size_t max_bytes,
                           AgentFinalTextSlice& out,
                           bool& found,
                           std::string& err)
{
    out = {};
    found = false;
    err.clear();

    nlohmann::json document;
    bool secure_found = false;
    std::string secure_err;
    if (!session_id.empty() && !log_id.empty() &&
        load_session_log_document(session_id, log_id, document, secure_found, secure_err) &&
        secure_found) {
        found = true;
    } else if (!fallback_path.empty()) {
        const std::filesystem::path path(fallback_path);
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec) && !ec) {
            if (!read_json_file(path, document) || !document.is_object()) {
                err = "agent log is not valid JSON";
                return false;
            }
            found = true;
        }
    }

    if (!found)
        return true;

    std::string final_text;
    if (document.contains("result") && document["result"].is_object() &&
        document["result"].contains("final_text") &&
        document["result"]["final_text"].is_string()) {
        final_text = document["result"]["final_text"].get<std::string>();
    } else if (document.contains("final_text") && document["final_text"].is_string()) {
        final_text = document["final_text"].get<std::string>();
    } else {
        err = "agent log does not contain result.final_text";
        return false;
    }

    out.total_bytes = final_text.size();
    out.offset = std::min(offset, out.total_bytes);
    while (out.offset < out.total_bytes &&
           (static_cast<unsigned char>(final_text[out.offset]) & 0xC0u) == 0x80u) {
        ++out.offset;
    }

    const std::size_t available = out.total_bytes - out.offset;
    std::size_t take = std::min(max_bytes, available);
    while (take > 0 && out.offset + take < out.total_bytes &&
           (static_cast<unsigned char>(final_text[out.offset + take]) & 0xC0u) == 0x80u) {
        --take;
    }
    out.text = final_text.substr(out.offset, take);
    out.truncated = out.offset + take < out.total_bytes;
    return true;
}

bool load_agent_tool_result(const std::string& session_id,
                            const std::string& log_id,
                            const std::string& fallback_path,
                            const std::string& tool_call_id,
                            std::size_t offset,
                            std::size_t max_bytes,
                            AgentToolResultSlice& out,
                            bool& found,
                            std::string& err)
{
    out = {};
    found = false;
    err.clear();
    if (tool_call_id.empty()) {
        err = "missing tool call id";
        return false;
    }

    nlohmann::json document;
    bool secure_found = false;
    std::string secure_err;
    if (!session_id.empty() && !log_id.empty() &&
        load_session_log_document(session_id, log_id, document, secure_found, secure_err) &&
        secure_found) {
        // Secure canonical document wins over a filesystem fallback.
    } else if (!fallback_path.empty()) {
        const std::filesystem::path path(fallback_path);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec)
            return true;
        if (!read_json_file(path, document) || !document.is_object()) {
            err = "agent log is not valid JSON";
            return false;
        }
    } else {
        return true;
    }

    nlohmann::json payload;
    if (document.contains("events") && document["events"].is_array()) {
        for (const auto& event : document["events"]) {
            if (!event.is_object() ||
                event.value("kind", std::string{}) != "tool_result")
                continue;
            nlohmann::json detail = event.value("detail", nlohmann::json::object());
            if (detail.is_string()) {
                try { detail = nlohmann::json::parse(detail.get<std::string>()); }
                catch (...) { continue; }
            }
            if (!detail.is_object() ||
                detail.value("id", std::string{}) != tool_call_id)
                continue;
            out.tool_name = event.value("tool", std::string{});
            payload = detail.contains("envelope") ? detail["envelope"] : detail;
            found = true;
            break;
        }
    }

    // Older logs may have the transcript but no event stream.
    if (!found && document.contains("result") && document["result"].is_object()) {
        const auto& transcript = document["result"].value(
            "transcript", nlohmann::json::object());
        const auto& messages = transcript.value("messages", nlohmann::json::array());
        if (messages.is_array()) {
            for (const auto& message : messages) {
                if (!message.is_object()) continue;
                if (message.value("role", std::string{}) == "assistant" &&
                    message.contains("tool_calls") && message["tool_calls"].is_array()) {
                    for (const auto& call : message["tool_calls"]) {
                        if (!call.is_object() ||
                            call.value("id", std::string{}) != tool_call_id)
                            continue;
                        if (call.contains("function") && call["function"].is_object())
                            out.tool_name = call["function"].value("name", std::string{});
                    }
                }
                if (message.value("role", std::string{}) == "tool" &&
                    message.value("tool_call_id", std::string{}) == tool_call_id) {
                    payload = message.value("content", nlohmann::json{});
                    found = true;
                }
            }
        }
    }
    if (!found)
        return true;

    const std::string serialized = media::llm::agent::detail::safe_json_dump(payload);
    out.total_bytes = serialized.size();
    out.offset = std::min(offset, out.total_bytes);
    while (out.offset < out.total_bytes &&
           (static_cast<unsigned char>(serialized[out.offset]) & 0xC0u) == 0x80u) {
        ++out.offset;
    }
    const std::size_t available = out.total_bytes - out.offset;
    std::size_t take = std::min(max_bytes, available);
    while (take > 0 && out.offset + take < out.total_bytes &&
           (static_cast<unsigned char>(serialized[out.offset + take]) & 0xC0u) == 0x80u) {
        --take;
    }
    out.text = serialized.substr(out.offset, take);
    out.truncated = out.offset + take < out.total_bytes;
    return true;
}

bool save_session_log_document(const std::string& session_id,
                               const std::string& log_id,
                               const nlohmann::json& document,
                               std::string& err)
{
    err.clear();
    if (session_id.empty() || log_id.empty()) {
        err = "missing session/log id";
        return false;
    }
    const StoreOptions options = active_options();
    if (!secure_backend_requested(options)) {
        err = "secure session storage is not active";
        return false;
    }
#if defined(FEATURE_SECURE_STORAGE) && FEATURE_SECURE_STORAGE
    return secure_save_json_document(options,
                                     session_log_uri(session_id, log_id),
                                     pm::model::ResourceType::SessionLog,
                                     document,
                                     err);
#else
    err = "secure storage feature is disabled";
    return false;
#endif
}

bool delete_session_log_document(const std::string& session_id,
                                 const std::string& log_id,
                                 std::string& err)
{
    err.clear();
    if (session_id.empty() || log_id.empty()) return true;
    const StoreOptions options = active_options();
    if (!secure_backend_requested(options)) return true;
#if defined(FEATURE_SECURE_STORAGE) && FEATURE_SECURE_STORAGE
    const auto deleted = pm::storage::delete_document(secure_paths(options), session_log_uri(session_id, log_id));
    if (!deleted.ok && deleted.error.rfind("document not found:", 0) != 0) {
        err = deleted.error;
        return false;
    }
    return true;
#else
    err = "secure storage feature is disabled";
    return false;
#endif
}

bool seed_from_agent_session(const std::string& session_id,
                             const std::string& source_session_dir,
                             std::string& err)
{
    err.clear();
    if (session_id.empty()) return true;
    const long long now = now_unix_ms();
    nlohmann::json entries = entries_from_agent_session_file(session_id, source_session_dir, now);
    copy_agent_session_into_default_store(session_id, source_session_dir);
    if (entries.empty()) {
        entries = nlohmann::json::array({{{"id", 1},
                                          {"role", "system"},
                                          {"text", "Realtime voice assistant session."},
                                          {"ts", now}}});
    }
    nlohmann::json session;
    if (!load(session_id, session, err))
        session = nlohmann::json::object();
    if (!session.is_object()) {
        session = {{"id", session_id}, {"title", "Realtime Assistant"}, {"createdAt", now}};
    }
    session["id"] = session_id;
    if (!session.contains("title") || !session["title"].is_string() ||
        session["title"].get<std::string>().empty())
        session["title"] = "Realtime Assistant";
    if (!session.contains("createdAt"))
        session["createdAt"] = now;
    session["updatedAt"] = now;
    session["entries"] = std::move(entries);
    return save(session, err);
}

nlohmann::json query_store(std::string& err)
{
    err.clear();
    nlohmann::json out{
        {"backend", using_secure_storage() ? "secure-storage" : "legacy-files"},
        {"root", media::path::path_to_generic_utf8(store_dir())},
        {"sessions", nlohmann::json::array()},
        {"sidecars", nlohmann::json::array()},
        {"memoryItems", nlohmann::json::array()},
        {"artifacts", nlohmann::json::array()},
        {"securityItems", nlohmann::json::array()},
    };
    const auto collect_item = [&](const std::string& id, const nlohmann::json& item) {
        if (!item.is_object()) return;
        nlohmann::json row = item;
        row["sessionId"] = id;
        out["memoryItems"].push_back(row);
        const std::string role = item.value("role", std::string{});
        const std::string kind = item.value("kind", std::string{});
        if (role == "security_review" || role == "security_decision" ||
            kind == "security_review" || kind == "security_consent")
            out["securityItems"].push_back(row);
    };
    const auto collect_memory_state = [&](const std::string& id, const nlohmann::json& memory_state) {
        if (!memory_state.is_object()) return;
        const auto items = memory_state.find("items");
        if (items == memory_state.end()) return;
        if (items->is_array()) {
            for (const auto& item : *items)
                collect_item(id, item);
        } else if (items->is_object()) {
            for (const auto& item : items->items())
                collect_item(id, item.value());
        }
    };

#if defined(FEATURE_SECURE_STORAGE) && FEATURE_SECURE_STORAGE
    if (using_secure_storage()) {
        const StoreOptions options = active_options();
        const auto docs = pm::storage::list_documents(secure_paths(options));
        if (docs.ok) {
            for (const auto& meta : docs.items) {
                if (meta.type != pm::model::ResourceType::SessionMemory &&
                    meta.type != pm::model::ResourceType::SessionLog &&
                    meta.type != pm::model::ResourceType::SessionArtifact)
                    continue;
                nlohmann::json row = pm::model::resource_meta_to_json(meta);
                out["sidecars"].push_back(row);
                if (meta.type == pm::model::ResourceType::SessionMemory) {
                    nlohmann::json doc;
                    bool found = false;
                    std::string load_err;
                    const std::string id = meta.uri.rfind("pm://session-memory/", 0) == 0
                        ? meta.uri.substr(std::string("pm://session-memory/").size())
                        : std::string{};
                    if (!id.empty() &&
                        secure_load_json_document(options, meta.uri, doc, found, load_err) &&
                        found && doc.is_object()) {
                        collect_memory_state(id, doc.value("memory_state", nlohmann::json::object()));
                    }
                }
            }
        }
    }
#endif

    const auto metas = list_meta(err);
    if (!err.empty())
        return out;
    for (const auto& meta : metas) {
        const std::string id = meta.value("id", std::string{});
        if (id.empty()) continue;
        nlohmann::json session;
        std::string load_err;
        if (!load(id, session, load_err) || !session.is_object())
            continue;
        out["sessions"].push_back(meta);
        if (auto it = session.find("memory_state"); it != session.end() && it->is_object()) {
            collect_memory_state(id, *it);
        }
        if (auto items = session.find("items"); items != session.end() && items->is_array()) {
            for (const auto& item : *items)
                collect_item(id, item);
        }
        if (auto artifacts = session.find("artifacts"); artifacts != session.end() && artifacts->is_array()) {
            for (const auto& artifact : *artifacts) {
                nlohmann::json row = artifact;
                if (row.is_object())
                    row["sessionId"] = id;
                out["artifacts"].push_back(std::move(row));
            }
        }
    }
    out["sessionCount"] = out["sessions"].size();
    out["sidecarCount"] = out["sidecars"].size();
    out["memoryItemCount"] = out["memoryItems"].size();
    out["artifactCount"] = out["artifacts"].size();
    out["securityItemCount"] = out["securityItems"].size();
    return out;
}

PruneResult prune(const PruneOptions& options, std::string& err)
{
    err.clear();
    PruneResult result;
    if (options.keep_latest <= 0)
        return result;
    auto metas = list_meta(err);
    if (!err.empty())
        return result;
    std::sort(metas.begin(), metas.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return json_i64(a, "updatedAt") > json_i64(b, "updatedAt");
    });
    for (std::size_t i = static_cast<std::size_t>(options.keep_latest); i < metas.size(); ++i) {
        const std::string id = metas[i].value("id", std::string{});
        if (id.empty()) continue;
        std::string rm_err;
        DeleteResult deleted = remove(id, rm_err);
        if (deleted.removed) {
            ++result.sessions_removed;
            ++result.documents_removed;
        } else if (err.empty() && !rm_err.empty()) {
            err = rm_err;
        }
    }
    return result;
}

namespace {

void collect_session_artifact_paths(const nlohmann::json& session,
                                    std::vector<std::string>& out,
                                    std::set<std::string>& seen_lower)
{
    const auto entries = session.find("entries");
    if (entries == session.end() || !entries->is_array())
        return;

    const auto push_path = [&](std::string raw) {
        while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t'))
            raw.erase(raw.begin());
        while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t'))
            raw.pop_back();
        if (raw.empty())
            return;

        std::string key = raw;
        key = media::strutil::lower_ascii(std::move(key));
        if (!seen_lower.insert(key).second)
            return;
        out.push_back(std::move(raw));
    };

    for (const auto& entry : *entries) {
        if (!entry.is_object())
            continue;
        const std::string role = entry.value("role", std::string{});
        if (role == "file" || role == "image")
            push_path(entry.value("text", std::string{}));
        const auto rp = entry.find("resultPaths");
        if (rp == entry.end() || !rp->is_array())
            continue;
        for (const auto& value : *rp) {
            if (value.is_string())
                push_path(value.get<std::string>());
        }
    }
}

} // namespace

std::vector<std::string> recent_session_artifact_paths(int max_sessions,
                                                       int max_paths,
                                                       std::string& err)
{
    err.clear();
    std::vector<std::string> out;
    if (max_sessions <= 0 || max_paths <= 0)
        return out;

#if defined(FEATURE_SECURE_STORAGE) && FEATURE_SECURE_STORAGE
    {
        const StoreOptions options = active_options();
        if (secure_backend_requested(options)) {
            // REVERT startup-perf: this used to call list_meta(), which decrypts
            // every SessionTranscript just to sort on updatedAt, then load()
            // decrypts the newest ones again. The document index already stores
            // updated_at as UTC ISO-8601, so only max_sessions bodies are read.
            const auto listed = pm::storage::list_documents(secure_paths(options));
            if (!listed.ok) {
                err = listed.error;
                return out;
            }
            struct Row {
                std::string id;
                std::string updated;
            };
            std::vector<Row> rows;
            const std::string prefix = "pm://session/";
            rows.reserve(listed.items.size());
            for (const auto& meta : listed.items) {
                if (meta.type != pm::model::ResourceType::SessionTranscript)
                    continue;
                if (meta.uri.rfind(prefix, 0) != 0)
                    continue;
                std::string id = meta.uri.substr(prefix.size());
                if (id.empty())
                    continue;
                rows.push_back(Row{std::move(id), meta.updated_at});
            }
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
                return a.updated > b.updated;
            });
            std::set<std::string> seen_lower;
            int loaded = 0;
            for (const auto& row : rows) {
                if (loaded >= max_sessions)
                    break;
                nlohmann::json session;
                std::string load_err;
                if (!load(row.id, session, load_err))
                    continue;
                ++loaded;
                collect_session_artifact_paths(session, out, seen_lower);
                if (static_cast<int>(out.size()) >= max_paths)
                    break;
            }
            if (static_cast<int>(out.size()) > max_paths)
                out.resize(static_cast<std::size_t>(max_paths));
            return out;
        }
    }
#endif

    const auto metas = list_meta(err);
    if (!err.empty())
        return out;

    std::set<std::string> seen_lower;
    int loaded = 0;
    for (const auto& meta : metas) {
        if (loaded >= max_sessions)
            break;
        const std::string id = meta.value("id", std::string{});
        if (id.empty())
            continue;

        nlohmann::json session;
        std::string load_err;
        if (!load(id, session, load_err))
            continue;
        ++loaded;
        collect_session_artifact_paths(session, out, seen_lower);
        if (static_cast<int>(out.size()) >= max_paths)
            break;
    }

    if (static_cast<int>(out.size()) > max_paths)
        out.resize(static_cast<std::size_t>(max_paths));
    return out;
}

} // namespace media::llm::chat_sessions

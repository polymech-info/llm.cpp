#include "mcp_config.hpp"

#include "mcp_probe.hpp"
#include "mcp_stdio_client.hpp"
#include "mcp_stream_client.hpp"
#if defined(FEATURE_SECURITY_TOOL_IDENTITY) && FEATURE_SECURITY_TOOL_IDENTITY
#  include "security/ToolIdentity.hpp"
#endif
#include "storage/secure_storage.hpp"

#include "core/stdio_child_runner.hpp"
#include "core/app_exe_directory.hpp"
#include "core/settings_store.hpp"
#include "lib/pm_zitadel_oauth.hpp"
#include "service_defaults.hpp"
#if defined(__ANDROID__)
#include "core/settings_portable.hpp"
#include "pm_android_oauth_store.hpp"
#include <picosha2.h>
#else
#include <sodium.h>
#endif

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include "fs/path_sanitizer.hpp"

namespace media::llm::mcp {
namespace {

#ifndef FEATURE_SECURE_STORAGE
#define FEATURE_SECURE_STORAGE 0
#endif

#ifndef FEATURE_SECURITY_TOOL_IDENTITY
#define FEATURE_SECURITY_TOOL_IDENTITY 0
#endif

#ifndef FEATURE_SECURITY_MCP_POLICY
#define FEATURE_SECURITY_MCP_POLICY 0
#endif

std::int64_t now_epoch_seconds()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(now).count();
}

std::int64_t file_mtime_epoch(const std::filesystem::path& p)
{
    std::error_code ec;
    if (!std::filesystem::exists(p, ec))
        return 0;
    const auto ft = std::filesystem::last_write_time(p, ec);
    if (ec)
        return 0;
    const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count();
}

std::optional<std::int64_t> json_epoch_seconds(const nlohmann::json& j, const char* key)
{
    if (!j.is_object() || !j.contains(key))
        return std::nullopt;
    const auto& v = j[key];
    if (v.is_number_integer())
        return v.get<std::int64_t>();
    if (v.is_number_unsigned())
        return static_cast<std::int64_t>(v.get<std::uint64_t>());
    if (v.is_number_float())
        return static_cast<std::int64_t>(v.get<double>());
    if (v.is_string()) {
        try {
            return static_cast<std::int64_t>(std::stoll(v.get_ref<const std::string&>()));
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

constexpr std::int64_t k_mcp_tools_cache_ttl_seconds = 8 * 60 * 60;

std::string openai_slug(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if (std::isalnum(c))
            out += static_cast<char>(std::tolower(c));
        else if (c == '_' || c == '-')
            out += static_cast<char>(c);
        else
            out += '_';
    }
    if (out.empty())
        return "srv";
    return out;
}

nlohmann::json mcp_manifest_json(const nlohmann::json& server)
{
    nlohmann::json manifest = server;
    if (manifest.is_object())
        manifest.erase("security");
    return manifest;
}

std::string hash_text_sha256(const std::string& text)
{
#if defined(__ANDROID__)
    return picosha2::hash256_hex_string(text);
#else
    if (sodium_init() < 0) return {};
    unsigned char digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(
        digest,
        reinterpret_cast<const unsigned char*>(text.data()),
        static_cast<unsigned long long>(text.size()));
    char hex[crypto_hash_sha256_BYTES * 2 + 1];
    sodium_bin2hex(hex, sizeof(hex), digest, sizeof(digest));
    return std::string(hex);
#endif
}

std::string mcp_server_manifest_hash(const nlohmann::json& server)
{
    try {
        return hash_text_sha256(mcp_manifest_json(server).dump());
    } catch (...) {
        return {};
    }
}

bool load_plain_mcp_json_file(const std::filesystem::path& path,
                              nlohmann::json& out,
                              std::string& err)
{
    err.clear();
    out = nlohmann::json::object();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        err = "mcp.json not found";
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "could not open mcp.json for reading";
        return false;
    }

    try {
        in >> out;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

std::string resolve_stdio_executable(const std::string& command)
{
    if (command.empty())
        return command;

    const std::filesystem::path req(command);
    std::error_code ec;
    if (req.is_absolute() && std::filesystem::is_regular_file(req, ec) && !ec)
        return media::path::path_to_utf8(std::filesystem::absolute(req, ec));
    if (req.has_parent_path()) {
        if (std::filesystem::is_regular_file(req, ec) && !ec)
            return media::path::path_to_utf8(std::filesystem::absolute(req, ec));
        return command;
    }

    const std::filesystem::path resolved = media::app::resolve_third_party_executable(req);
    if (!resolved.empty())
        return media::path::path_to_utf8(resolved);
    return command;
}

std::string trim_trailing_slashes(std::string s)
{
    while (s.size() > 1 && s.back() == '/')
        s.pop_back();
    return s;
}

bool mcp_url_matches_tanit_router(const std::string& url)
{
    if (url.empty())
        return false;

    std::string base = pm::services::resolve_llm_router_base();
    if (base.empty())
        return false;
    base = trim_trailing_slashes(std::move(base));

    std::string norm_url = trim_trailing_slashes(url);
    const std::string expected = base + "/mcp";
    if (norm_url == expected)
        return true;
    if (norm_url.size() > expected.size() &&
        norm_url.compare(0, expected.size(), expected) == 0 &&
        (norm_url[expected.size()] == '/' || norm_url[expected.size()] == '?')) {
        return true;
    }
    return false;
}

std::string bearer_header_value(std::string token)
{
    if (token.empty())
        return {};
    if (token.rfind("Bearer ", 0) == 0)
        return token;
    return "Bearer " + token;
}

bool tanit_router_bearer_available(std::string& bearer, std::string& err)
{
    bearer.clear();
    err.clear();
#if defined(__ANDROID__)
    // Same Zitadel session chat already stored for this profile. Desktop reads
    // zitadel-oauth.json; the phone keeps the token in the profile oauth store.
    const std::string profile =
        media::path::path_to_utf8(media::portable_settings::settings_pixelwiz_profile_dir());
    if (pm::android::oauth_api_bearer(profile, bearer, err) && !bearer.empty())
        return true;
    if (err.empty())
        err = "Tanit MCP requires the same sign-in as chat";
    return false;
#else
    if (pm_zitadel_oauth_read_access_token(bearer, err) && !bearer.empty())
        return true;
    bearer.clear();
    if (err.empty())
        err = "pixlwiz router MCP requires authentication (run `tanit-cli login` first)";
    return false;
#endif
}

bool apply_tanit_router_auth_headers(std::map<std::string, std::string>& headers,
                                       const std::string& mcp_url,
                                       std::string& err)
{
    err.clear();
    if (!mcp_url_matches_tanit_router(mcp_url))
        return true;

    std::string bearer;
    if (!tanit_router_bearer_available(bearer, err) || bearer.empty())
        return false;

    headers["Authorization"] = bearer_header_value(std::move(bearer));
    return true;
}

bool probe_skipped_for_missing_auth(const nlohmann::json& probe)
{
    if (!probe.is_object() || !probe.contains("servers") || !probe["servers"].is_array())
        return false;
    for (const auto& server : probe["servers"]) {
        if (!server.is_object() || !server.value("skipped", false))
            continue;
        const std::string why = server.value("skip_reason", std::string{});
        if (why.find("authentication") != std::string::npos ||
            why.find("tanit-cli login") != std::string::npos ||
            why.find("access token") != std::string::npos)
            return true;
    }
    return false;
}

std::vector<std::string> split_command_line_args(const std::string& line)
{
    std::vector<std::string> out;
    std::string cur;
    bool in_quote = false;
    char quote = '\0';
    for (unsigned char uc : line) {
        const char c = static_cast<char>(uc);
        if (in_quote) {
            if (c == quote) {
                in_quote = false;
                continue;
            }
            cur += c;
            continue;
        }
        if (c == '"' || c == '\'') {
            in_quote = true;
            quote = c;
            continue;
        }
        if (std::isspace(uc)) {
            if (!cur.empty()) {
                out.push_back(std::move(cur));
                cur.clear();
            }
            continue;
        }
        cur += c;
    }
    if (!cur.empty())
        out.push_back(std::move(cur));
    return out;
}

std::string filename_token(const std::string& path)
{
    const auto slash = path.find_last_of("\\/");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name.size() > 4) {
        const std::string ext = name.substr(name.size() - 4);
        if (ext == ".exe" || ext == ".cmd" || ext == ".bat")
            name.resize(name.size() - 4);
    }
    return name;
}

std::string extract_mcp_remote_http_url(const McpServerEndpoint& ep)
{
    std::vector<std::string> tokens;
    if (!ep.command.empty())
        tokens.push_back(ep.command);
    tokens.insert(tokens.end(), ep.args.begin(), ep.args.end());
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (filename_token(tokens[i]) != "mcp-remote")
            continue;
        for (std::size_t j = i + 1; j < tokens.size(); ++j) {
            if (tokens[j].rfind("http://", 0) == 0 || tokens[j].rfind("https://", 0) == 0)
                return tokens[j];
        }
    }
    return {};
}

std::string persisted_sandbox_backend_for_server(const std::string& server_key)
{
    nlohmann::json permissions;
    std::string err;
    if (!media::settings::load_subtree(media::settings::SettingsSubtreeKeys::permissions,
                                       permissions, err) ||
        !permissions.is_array()) {
        return {};
    }

    const std::string prefix = "mcp_" + openai_slug(server_key) + "__";
    for (const auto& row : permissions) {
        if (!row.is_object())
            continue;
        const std::string sandbox = row.value("sandboxBackend", std::string{});
        if (sandbox.empty())
            continue;
        const std::string action = row.value("action", std::string{});
        if (action != "execute")
            continue;
        const auto& tool = row.contains("tool") ? row["tool"] : nlohmann::json::object();
        if (!tool.is_object() || tool.value("provider", std::string{}) != "mcp")
            continue;
        const std::string tool_id = tool.value("toolId", std::string{});
        if (tool_id.rfind(prefix, 0) == 0)
            return sandbox;
        if (row.contains("resourceScopes") && row["resourceScopes"].is_array()) {
            for (const auto& scope : row["resourceScopes"]) {
                if (scope.is_string()) {
                    const std::string s = scope.get<std::string>();
                    if (s.rfind("mcp://" + prefix, 0) == 0)
                        return sandbox;
                }
            }
        }
    }
    return {};
}

#if FEATURE_SECURE_STORAGE
bool load_secure_mcp_config(const std::filesystem::path& dir,
                            nlohmann::json& out,
                            std::string& err)
{
    pm::storage::DocumentLoadPolicy policy;
    policy.require_signature = true;
    policy.require_encryption = true;
    policy.allowed_trust = {pm::model::TrustLevel::UserLocal};
    const auto loaded = pm::storage::load_or_import_seed_document(pm::storage::resolve_storage_paths(dir), kMcpConfigUri, policy);
    if (!loaded.ok) {
        err = loaded.error;
        return false;
    }
    try {
        out = nlohmann::json::parse(std::string(reinterpret_cast<const char*>(loaded.document.data.data()),
                                                loaded.document.data.size()));
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}
#endif

nlohmann::json canonical_json(const nlohmann::json& v); // defined below

// Fingerprint of the *loaded* MCP config (secure doc or plain file). Plain
// mcp.json mtime is not enough: settings UI writes pm://config/mcp.json via
// secure storage and often never touches the plaintext file.
std::string mcp_config_fingerprint(const nlohmann::json& cfg)
{
    try {
        const std::string full = hash_text_sha256(canonical_json(cfg).dump());
        return full.size() >= 32 ? full.substr(0, 32) : full;
    } catch (...) {
        return {};
    }
}

bool mcp_cache_is_fresh(const nlohmann::json& cache, const std::string& config_fp,
                        bool allow_stale)
{
    if (!cache.is_object() || !cache.contains("probe") || !cache["probe"].is_object())
        return false;
    // v2+ keys off config fingerprint (secure-storage safe). v1 used plain-file
    // mtime and is always treated as stale.
    if (cache.value("version", 0) < 2)
        return false;
    if (config_fp.empty())
        return false;
    if (cache.value("mcp_config_fingerprint", std::string{}) != config_fp)
        return false;

    // Login writes tokens without changing mcp.json. A cached "run login first"
    // skip must not survive for the 8h TTL after the user signs in.
    if (probe_skipped_for_missing_auth(cache["probe"])) {
        std::string bearer;
        std::string token_err;
        if (tanit_router_bearer_available(bearer, token_err))
            return false;
    }

    if (allow_stale)
        return true;

    const auto fetched_at = json_epoch_seconds(cache, "fetched_at");
    if (!fetched_at)
        return false;

    std::int64_t age = now_epoch_seconds() - *fetched_at;
    if (age < 0) {
        if (age < -3600)
            return false;
        age = 0;
    }
    return age <= k_mcp_tools_cache_ttl_seconds;
}

bool load_mcp_tools_cache(const std::string& config_fp, nlohmann::json& probe,
                          bool allow_stale)
{
    probe = nlohmann::json::object();
    std::ifstream in(mcp_tools_cache_path(), std::ios::binary);
    if (!in)
        return false;
    nlohmann::json cache;
    try {
        in >> cache;
    } catch (...) {
        return false;
    }
    if (!mcp_cache_is_fresh(cache, config_fp, allow_stale))
        return false;
    probe = cache["probe"];
    probe["cache"] = nlohmann::json{
        {"source", allow_stale ? "disk-stale-ok" : "disk"},
        {"path", media::path::path_to_generic_utf8(mcp_tools_cache_path())},
        {"ttl_seconds", k_mcp_tools_cache_ttl_seconds},
        {"fetched_at", cache.value("fetched_at", 0)},
        {"mcp_config_fingerprint", config_fp},
    };
    return true;
}

void save_mcp_tools_cache(const std::string& config_fp,
                          const std::filesystem::path& mcp_path,
                          const nlohmann::json& probe)
{
    nlohmann::json cache{
        {"version", 2},
        {"fetched_at", now_epoch_seconds()},
        {"mcp_config_fingerprint", config_fp},
        {"mcp_json_path", media::path::path_to_generic_utf8(mcp_path)},
        {"mcp_json_mtime", file_mtime_epoch(mcp_path)},
        {"probe", probe},
    };
    std::error_code ec;
    const auto path = mcp_tools_cache_path();
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out)
        out << cache.dump();
}

// ── Inline / custom server probe cache (private helpers) ────────────────────

// Recursively sort JSON object keys so that dump() is stable across insertion
// orders.  Needed to produce a deterministic slug regardless of how the caller
// constructed the serverConfig object.
nlohmann::json canonical_json(const nlohmann::json& v)
{
    if (v.is_object()) {
        std::map<std::string, nlohmann::json> m;
        for (const auto& [k, val] : v.items())
            m[k] = canonical_json(val);
        nlohmann::json r = nlohmann::json::object();
        for (auto& [k, val] : m)
            r[k] = std::move(val);
        return r;
    }
    if (v.is_array()) {
        nlohmann::json r = nlohmann::json::array();
        for (const auto& el : v)
            r.push_back(canonical_json(el));
        return r;
    }
    return v;
}

bool mcp_inline_cache_is_fresh(const nlohmann::json& cache, const std::string& slug)
{
    if (!cache.is_object() || !cache.contains("probe") || !cache["probe"].is_object())
        return false;
    if (cache.value("version", 0) != 1)
        return false;
    // Slug mismatch means the config changed — treat as stale (caller uses different slug).
    if (cache.value("slug", std::string{}) != slug)
        return false;
    const auto fetched_at = json_epoch_seconds(cache, "fetched_at");
    if (!fetched_at)
        return false;
    std::int64_t age = now_epoch_seconds() - *fetched_at;
    if (age < 0) {
        if (age < -3600) return false; // clock skew > 1 h → invalidate
        age = 0;
    }
    return age <= k_mcp_tools_cache_ttl_seconds;
}

std::filesystem::path inline_cache_path_for(const std::string& slug)
{
    const auto dir = resolve_mcp_config_dir({});
    if (dir.empty()) return {};
    return dir / ("mcp_server-" + slug + "_cache.json");
}

bool load_mcp_inline_cache(const std::string& slug, nlohmann::json& probe)
{
    probe = nlohmann::json::object();
    const auto path = inline_cache_path_for(slug);
    if (path.empty()) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    nlohmann::json cache;
    try {
        in >> cache;
    } catch (...) {
        return false;
    }
    if (!mcp_inline_cache_is_fresh(cache, slug))
        return false;
    probe = cache["probe"];
    return true;
}

void save_mcp_inline_cache(const std::string& slug, const nlohmann::json& probe)
{
    const auto path = inline_cache_path_for(slug);
    if (path.empty()) return;
    nlohmann::json cache{
        {"version",    1},
        {"slug",       slug},
        {"fetched_at", now_epoch_seconds()},
        {"probe",      probe},
    };
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out)
        out << cache.dump();
}

struct McpCfgMem {
    std::filesystem::path dir;
    std::optional<std::filesystem::file_time_type> wt;
    nlohmann::json cfg;
    bool ok = false;
};
std::mutex g_mcp_cfg_mu;
McpCfgMem g_mcp_cfg;

std::optional<std::filesystem::file_time_type>
mcp_config_write_time(const std::filesystem::path& resolved)
{
    if (resolved.empty())
        return std::nullopt;
#if FEATURE_SECURE_STORAGE
    try {
        const auto paths = pm::storage::resolve_storage_paths(resolved);
        if (auto wt = pm::storage::document_write_time(paths, kMcpConfigUri))
            return wt;
    } catch (...) {}
#endif
    std::error_code ec;
    const auto ft = std::filesystem::last_write_time(resolved / "mcp.json", ec);
    if (ec)
        return std::nullopt;
    return ft;
}

struct McpToolsMem {
    std::filesystem::path dir;
    std::optional<std::filesystem::file_time_type> wt;
    std::string config_fp;
    nlohmann::json probe;
};
std::mutex g_mcp_tools_mem_mu;
McpToolsMem g_mcp_tools_mem;

bool recall_mcp_tools_mem_fresh(const std::filesystem::path& dir,
                                 const std::optional<std::filesystem::file_time_type>& wt,
                                 nlohmann::json& probe)
{
    if (!wt)
        return false;
    std::lock_guard<std::mutex> lock(g_mcp_tools_mem_mu);
    if (g_mcp_tools_mem.dir != dir || !g_mcp_tools_mem.wt || *g_mcp_tools_mem.wt != *wt)
        return false;
    if (!g_mcp_tools_mem.probe.is_object())
        return false;
    probe = g_mcp_tools_mem.probe;
    return true;
}

bool recall_mcp_tools_mem(const std::string& config_fp, nlohmann::json& probe)
{
    std::lock_guard<std::mutex> lock(g_mcp_tools_mem_mu);
    if (config_fp.empty() || g_mcp_tools_mem.config_fp != config_fp)
        return false;
    if (!g_mcp_tools_mem.probe.is_object())
        return false;
    probe = g_mcp_tools_mem.probe;
    return true;
}

void remember_mcp_tools_mem(const std::string& config_fp,
                           const nlohmann::json& probe,
                           const std::filesystem::path& dir,
                           const std::optional<std::filesystem::file_time_type>& wt)
{
    if (config_fp.empty() || !probe.is_object())
        return;
    std::lock_guard<std::mutex> lock(g_mcp_tools_mem_mu);
    g_mcp_tools_mem.dir = dir;
    g_mcp_tools_mem.wt = wt;
    g_mcp_tools_mem.config_fp = config_fp;
    g_mcp_tools_mem.probe = probe;
}

} // namespace

std::filesystem::path resolve_mcp_config_dir(const std::filesystem::path& dir)
{
    if (!dir.empty())
        return dir;
    try {
        return media::settings::get_config_dir();
    } catch (...) {
        return {};
    }
}

std::filesystem::path mcp_json_path(const std::filesystem::path& dir)
{
    const auto resolved = resolve_mcp_config_dir(dir);
    return resolved.empty() ? media::path::path_from_utf8("mcp.json") : (resolved / "mcp.json");
}

bool load_mcp_config(const std::filesystem::path& dir, nlohmann::json& out, std::string& err)
{
    err.clear();
    out = nlohmann::json::object();
    const auto resolved = resolve_mcp_config_dir(dir);
    if (resolved.empty()) {
        err = "profile config directory unknown";
        return false;
    }

    const auto wt = mcp_config_write_time(resolved);
    if (wt) {
        std::lock_guard<std::mutex> lock(g_mcp_cfg_mu);
        if (g_mcp_cfg.ok && g_mcp_cfg.dir == resolved && g_mcp_cfg.wt && *g_mcp_cfg.wt == *wt) {
            out = g_mcp_cfg.cfg;
            return true;
        }
    }

    bool loaded = false;
#if FEATURE_SECURE_STORAGE
    loaded = load_secure_mcp_config(resolved, out, err);
    if (!loaded) {
        const auto plain_path = resolved / "mcp.json";
        std::string plain_err;
        if (load_plain_mcp_json_file(plain_path, out, plain_err)) {
            loaded = true;
            err.clear();
        }
    }
#else
    loaded = load_plain_mcp_json_file(resolved / "mcp.json", out, err);
#endif

    if (!loaded)
        return false;

    if (!out.is_object()) {
        err = "mcp.json root must be a JSON object";
        out = nlohmann::json::object();
        return false;
    }
    if (!out.contains("mcpServers") || !out["mcpServers"].is_object()) {
        err = R"(expected "mcpServers": { "<name>": { ... } })";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_mcp_cfg_mu);
        g_mcp_cfg.dir = resolved;
        g_mcp_cfg.wt = mcp_config_write_time(resolved);
        g_mcp_cfg.cfg = out;
        g_mcp_cfg.ok = true;
    }
    return true;
}

bool save_mcp_config(const std::filesystem::path& dir, const nlohmann::json& cfg, std::string& err)
{
    err.clear();
    if (!cfg.is_object()) {
        err = "mcp.json root must be a JSON object";
        return false;
    }
    if (!cfg.contains("mcpServers") || !cfg["mcpServers"].is_object()) {
        err = R"(expected "mcpServers": { "<name>": { ... } })";
        return false;
    }

    const auto resolved = resolve_mcp_config_dir(dir);
    if (resolved.empty()) {
        err = "profile config directory unknown";
        return false;
    }

#if FEATURE_SECURE_STORAGE
    pm::storage::PutDocumentRequest req;
    req.uri = kMcpConfigUri;
    req.type = pm::model::ResourceType::McpConfig;
    req.trust = pm::model::TrustLevel::UserLocal;
    req.version = "1";
    req.provider = "user";
    req.signer_id = "local-mcp";
    req.encrypt = true;
    req.replace_vendor_seed = true;
    const std::string text = cfg.dump(2) + "\n";
    req.data.assign(text.begin(), text.end());

    const auto paths = pm::storage::resolve_storage_paths(resolved);
    const auto stored = pm::storage::put_document(paths, req);
    if (!stored.ok) {
        err = stored.error;
        return false;
    }
    const auto signed_doc = pm::storage::sign_document(paths, kMcpConfigUri);
    if (!signed_doc.ok) {
        err = signed_doc.error;
        return false;
    }
    invalidate_mcp_tools_cache();
    return true;
#else
    const auto plain_path = resolved / "mcp.json";
    std::error_code ec;
    std::filesystem::create_directories(plain_path.parent_path(), ec);
    std::ofstream out(plain_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "could not open mcp.json for writing";
        return false;
    }
    out << cfg.dump(2) << "\n";
    if (!out.good()) {
        err = "write failed: " + media::path::path_to_utf8(plain_path);
        return false;
    }
    invalidate_mcp_tools_cache();
    return true;
#endif
}

std::vector<std::string> list_mcp_servers(const nlohmann::json& cfg)
{
    std::vector<std::string> names;
    if (!cfg.is_object() || !cfg.contains("mcpServers") || !cfg["mcpServers"].is_object())
        return names;
    for (auto it = cfg["mcpServers"].begin(); it != cfg["mcpServers"].end(); ++it) {
        if (it.value().is_object() &&
            it.value().contains("enabled") &&
            it.value()["enabled"].is_boolean() &&
            !it.value()["enabled"].get<bool>()) {
            continue;
        }
        names.push_back(it.key());
    }
    return names;
}

std::map<std::string, std::string> parse_header_map(const nlohmann::json& server_cfg)
{
    std::map<std::string, std::string> headers;
    if (!server_cfg.contains("headers") || !server_cfg["headers"].is_object())
        return headers;
    for (auto hit = server_cfg["headers"].begin(); hit != server_cfg["headers"].end(); ++hit) {
        if (!hit.key().empty() && hit.value().is_string())
            headers[hit.key()] = hit.value().get<std::string>();
    }
    return headers;
}

media::process::StdioLaunchOptions parse_stdio_launch_options(const nlohmann::json& server_cfg,
                                                              const std::string& server_name)
{
    media::process::StdioLaunchOptions opts;
    opts.sandbox_backend = server_cfg.value("sandboxBackend", std::string{});
    if (opts.sandbox_backend.empty())
        opts.sandbox_backend = server_cfg.value("sandbox_backend", std::string{});
    if (opts.sandbox_backend.empty() && server_cfg.value("sandbox", false))
        opts.sandbox_backend = "appcontainer";
    opts.cwd = server_cfg.value("cwd", std::string{});
    opts.sandbox_moniker = server_cfg.value("sandboxMoniker", std::string{});
    if (opts.sandbox_moniker.empty())
        opts.sandbox_moniker = server_cfg.value("sandbox_moniker", std::string{});
    if (opts.sandbox_moniker.empty() && !opts.sandbox_backend.empty())
        opts.sandbox_moniker = "pm-mcp-" + server_name;
    opts.lpac = server_cfg.value("lpac", false);
    opts.retain_sandbox_profile = server_cfg.value("retainSandboxProfile", false) ||
                                  server_cfg.value("retain_sandbox_profile", false);

    const nlohmann::json* paths = nullptr;
    if (server_cfg.contains("paths") && server_cfg["paths"].is_array())
        paths = &server_cfg["paths"];
    else if (server_cfg.contains("sandboxPaths") && server_cfg["sandboxPaths"].is_array())
        paths = &server_cfg["sandboxPaths"];
    if (paths) {
        for (const auto& item : *paths) {
            if (!item.is_object()) continue;
            media::process::StdioPathGrant pg;
            pg.path = item.value("path", std::string{});
            pg.access = item.value("access", std::string{"read"});
            if (!pg.path.empty())
                opts.paths.push_back(std::move(pg));
        }
    }
    return opts;
}

void apply_persisted_sandbox_routing(media::process::StdioLaunchOptions& opts,
                                     const std::string& server_name)
{
    if (opts.sandbox_backend.empty())
        opts.sandbox_backend = persisted_sandbox_backend_for_server(server_name);
    if (!opts.sandbox_backend.empty() && opts.sandbox_moniker.empty())
        opts.sandbox_moniker = "pm-mcp-" + server_name;
}

bool parse_mcp_server_endpoint(const nlohmann::json& server_cfg, McpServerEndpoint& out)
{
    out = {};
    if (!server_cfg.is_object())
        return false;

    if (server_cfg.contains("type") && server_cfg["type"].is_string())
        out.type = server_cfg["type"].get<std::string>();
    if (server_cfg.contains("url") && server_cfg["url"].is_string())
        out.url = server_cfg["url"].get<std::string>();
    if (server_cfg.contains("command") && server_cfg["command"].is_string())
        out.command = server_cfg["command"].get<std::string>();
    if (server_cfg.contains("args") && server_cfg["args"].is_array()) {
        for (const auto& a : server_cfg["args"]) {
            if (a.is_string())
                out.args.push_back(a.get<std::string>());
        }
    }
    if (server_cfg.contains("env") && server_cfg["env"].is_object()) {
        for (auto eit = server_cfg["env"].begin(); eit != server_cfg["env"].end(); ++eit) {
            if (eit.value().is_string())
                out.env[eit.key()] = eit.value().get<std::string>();
        }
    }
    out.headers = parse_header_map(server_cfg);

    // Settings UIs often store a whole shell line in `command` with empty args.
    // CreateProcessW treats that as one filename → Win32 123 (ERROR_INVALID_NAME).
    if (out.args.empty() && out.command.find(' ') != std::string::npos) {
        const auto parts = split_command_line_args(out.command);
        if (parts.size() >= 2) {
            out.command = parts.front();
            out.args.assign(parts.begin() + 1, parts.end());
        }
    }

    if (out.url.empty() && !out.command.empty() &&
        (out.command.compare(0, 7, "http://") == 0 || out.command.compare(0, 8, "https://") == 0)) {
        out.url = out.command;
        out.command.clear();
    }

    // `npx -y mcp-remote https://host/mcp` is a stdio shim. Tanit already has a
    // native Streamable HTTP client — use the URL directly unless type=stdio.
    if (out.url.empty() && out.type != "stdio") {
        const std::string remote_url = extract_mcp_remote_http_url(out);
        if (!remote_url.empty()) {
            out.url = remote_url;
            out.command.clear();
            out.args.clear();
        }
    }
    return true;
}

std::unique_ptr<IMcpSession> create_mcp_session(const nlohmann::json& server_cfg,
                                                const std::string& server_name,
                                                std::string& err,
                                                const McpSessionCreateOptions& opts)
{
    err.clear();
    if (!server_cfg.is_object()) {
        err = "server config must be a JSON object";
        return nullptr;
    }

    McpServerEndpoint endpoint;
    parse_mcp_server_endpoint(server_cfg, endpoint);
    const std::string resolved_command = resolve_stdio_executable(endpoint.command);

    std::unique_ptr<IMcpSession> client;
    if (endpoint.type == "stdio") {
        if (resolved_command.empty()) {
            err = "type stdio but command missing or empty";
            return nullptr;
        }
        auto launch_opts = parse_stdio_launch_options(server_cfg, server_name);
        if (opts.apply_persisted_sandbox)
            apply_persisted_sandbox_routing(launch_opts, server_name);
        client = std::make_unique<StdioMcpClient>(
            resolved_command, endpoint.args, std::move(endpoint.env), std::move(launch_opts));
    } else if (!endpoint.url.empty()) {
        if (!apply_tanit_router_auth_headers(endpoint.headers, endpoint.url, err))
            return nullptr;
        client = std::make_unique<StreamHttpClient>(endpoint.url, std::move(endpoint.headers));
    } else if (!resolved_command.empty()) {
        auto launch_opts = parse_stdio_launch_options(server_cfg, server_name);
        if (opts.apply_persisted_sandbox)
            apply_persisted_sandbox_routing(launch_opts, server_name);
        client = std::make_unique<StdioMcpClient>(
            resolved_command, endpoint.args, std::move(endpoint.env), std::move(launch_opts));
    } else {
        err = "need non-empty url or command";
        return nullptr;
    }

    const int timeout_ms = opts.timeout_ms > 0 ? opts.timeout_ms : mcp_default_timeout_ms();
    client->set_request_options(timeout_ms, opts.cancel_check);
    return client;
}

nlohmann::json parameters_from_mcp_tool(const nlohmann::json& tool_json)
{
    if (tool_json.contains("inputSchema") && tool_json["inputSchema"].is_object())
        return tool_json["inputSchema"];
    return nlohmann::json{{"type", "object"}, {"additionalProperties", true}};
}

bool require_registered_mcp(const nlohmann::json& root)
{
    if (root.value("requireRegisteredMcp", false))
        return true;
    if (root.contains("security") && root["security"].is_object())
        return root["security"].value("requireRegisteredMcp", false);
    return false;
}

bool mcp_server_policy_allows(const nlohmann::json& server,
                              const std::string& server_key,
                              const std::string& command,
                              bool require_registered,
                              std::string& warning)
{
    warning.clear();
    const nlohmann::json* sec = nullptr;
    if (server.contains("security") && server["security"].is_object())
        sec = &server["security"];

    const bool registered = sec && sec->value("registered", false);
    if (require_registered && !registered) {
        warning = "MCP: server \"" + server_key + "\" skipped (not registered by policy)\n";
        return false;
    }
    if (!sec)
        return true;

#if defined(FEATURE_SECURITY_TOOL_IDENTITY) && FEATURE_SECURITY_TOOL_IDENTITY
    pm::security::ToolIdentityResolver resolver;
    const std::string expected_exe = sec->value("executableHash", std::string{});
    if (!expected_exe.empty()) {
        const std::string actual = resolver.hashFile(media::path::path_from_utf8(command));
        if (actual.empty() || actual != expected_exe) {
            warning = "MCP: server \"" + server_key + "\" skipped (executable hash mismatch)\n";
            return false;
        }
    }
#endif

#if defined(FEATURE_SECURITY_MCP_POLICY) && FEATURE_SECURITY_MCP_POLICY
    const std::string expected_manifest = sec->value("manifestHash", std::string{});
    if (!expected_manifest.empty()) {
        const std::string actual = mcp_server_manifest_hash(server);
        if (actual.empty() || actual != expected_manifest) {
            warning = "MCP: server \"" + server_key + "\" skipped (manifest hash mismatch)\n";
            return false;
        }
    }
#endif
    return true;
}

int mcp_default_timeout_ms()
{
    const char* env = std::getenv("PM_MCP_TIMEOUT_MS");
    if (!env || !*env)
        return 60000;
    try {
        const int v = std::stoi(env);
        return v > 0 ? v : 60000;
    } catch (...) {
        return 60000;
    }
}

std::function<bool()> mcp_cancel_after_ms_from_env()
{
    const char* env = std::getenv("PM_MCP_CANCEL_AFTER_MS");
    if (!env || !*env)
        return {};
    int ms = 0;
    try {
        ms = std::stoi(env);
    } catch (...) {
        return {};
    }
    if (ms <= 0)
        return {};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    return [deadline] {
        return std::chrono::steady_clock::now() >= deadline;
    };
}

// ── Inline / custom server probe cache (public API) ─────────────────────────

std::string mcp_inline_server_slug(const nlohmann::json& server_cfg)
{
    try {
        const std::string text = canonical_json(server_cfg).dump();
        const std::string full = hash_text_sha256(text);
        // First 16 hex chars (64 bits) is sufficient to distinguish configs.
        return full.size() >= 16 ? full.substr(0, 16) : full;
    } catch (...) {
        return "unknown";
    }
}

std::filesystem::path mcp_inline_server_cache_path(const std::string& slug)
{
    const auto dir = resolve_mcp_config_dir({});
    if (dir.empty())
        return {};
    return dir / ("mcp_server-" + slug + "_cache.json");
}

nlohmann::json mcp_probe_inline_server(const nlohmann::json& server_cfg,
                                        bool force_refresh,
                                        int  timeout_ms)
{
    // Slug is computed from the raw (unresolved) config so that ${USER:…}
    // substitutions never appear in the cache filename.
    const std::string slug = mcp_inline_server_slug(server_cfg);

    nlohmann::json probe;
    if (!force_refresh && load_mcp_inline_cache(slug, probe))
        return probe;

    probe = probe_inline_mcp_server(server_cfg, "__inline__", timeout_ms);
    save_mcp_inline_cache(slug, probe);
    return probe;
}

bool load_mcp_inline_probe_cache(const std::string& slug, nlohmann::json& probe)
{
    return load_mcp_inline_cache(slug, probe);
}

nlohmann::json load_or_probe_mcp_tools(bool allow_probe,
                                       std::string& note,
                                       const std::filesystem::path& config_dir,
                                       bool allow_stale)
{
    note.clear();
    std::filesystem::path mcp_path;
    try {
        mcp_path = mcp_json_path(config_dir);
    } catch (...) {
        note = "config dir unavailable";
        return nlohmann::json{{"skipped", true}, {"servers", nlohmann::json::array()}};
    }

    const auto resolved = resolve_mcp_config_dir(config_dir);
    const auto wt = mcp_config_write_time(resolved);
    nlohmann::json probe;
    if (recall_mcp_tools_mem_fresh(resolved, wt, probe)) {
        note = "tools/list from process cache";
        return probe;
    }

    // Fingerprint the loaded config (secure storage or plain file). This is the
    // invalidation key — not plaintext mcp.json mtime.
    nlohmann::json cfg;
    std::string cfg_err;
    std::string config_fp;
    if (load_mcp_config(config_dir, cfg, cfg_err))
        config_fp = mcp_config_fingerprint(cfg);

    if (!config_fp.empty() && recall_mcp_tools_mem(config_fp, probe)) {
        remember_mcp_tools_mem(config_fp, probe, resolved, wt);
        note = "tools/list from process cache";
        return probe;
    }

    if (!config_fp.empty() && load_mcp_tools_cache(config_fp, probe, allow_stale)) {
        remember_mcp_tools_mem(config_fp, probe, resolved, wt);
        note = allow_stale
            ? "tools/list from disk cache (stale ok; fingerprint match)"
            : "tools/list from cache (ttl 8h; invalidated when MCP config content changes)";
        return probe;
    }

    if (!allow_probe) {
        note = "tools/list cache missing or stale; live probe disabled";
        return nlohmann::json{{"skipped", true}, {"servers", nlohmann::json::array()}};
    }

    try {
        probe = probe_mcp_config(config_dir);
        if (config_fp.empty() && load_mcp_config(config_dir, cfg, cfg_err))
            config_fp = mcp_config_fingerprint(cfg);
        if (!config_fp.empty()) {
            save_mcp_tools_cache(config_fp, mcp_path, probe);
            remember_mcp_tools_mem(config_fp, probe, resolved, mcp_config_write_time(resolved));
        }
        note = "tools/list probed live and cached (ttl 8h)";
        return probe;
    } catch (const std::exception& e) {
        note = std::string("MCP probe failed: ") + e.what();
    } catch (...) {
        note = "MCP probe failed: unknown exception";
    }
    return nlohmann::json{{"skipped", true}, {"servers", nlohmann::json::array()}};
}

namespace detail {

void clear_mcp_tools_probe_memory()
{
    std::lock_guard<std::mutex> lock(g_mcp_tools_mem_mu);
    g_mcp_tools_mem = {};
}

void clear_mcp_config_memory()
{
    std::lock_guard<std::mutex> lock(g_mcp_cfg_mu);
    g_mcp_cfg = {};
}

} // namespace detail

nlohmann::json mcp_probe_cached(bool allow_probe,
                                std::string* note,
                                const std::filesystem::path& config_dir)
{
    std::string local_note;
    nlohmann::json probe = load_or_probe_mcp_tools(allow_probe, local_note, config_dir);
    if (note)
        *note = local_note;
    return probe;
}

} // namespace media::llm::mcp

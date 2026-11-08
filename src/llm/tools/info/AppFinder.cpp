#include "AppFinder.hpp"
#include "AppFinder_Internal.hpp"

#include "core/app_resolver.hpp"
#include "logging/pm_log.hpp"
#include "string/string_utils.hpp"   // has_embed_backend, embed_match_score (FEATURE_COMMAND_LLAMA_INFERENCE)

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>
#include <rapidfuzz/fuzz.hpp>
#include "fs/path_sanitizer.hpp"

namespace media::llm::info {
namespace {

namespace fs = std::filesystem;
constexpr const char k_ch[] = "app_finder";
constexpr int64_t k_cache_ttl_seconds = 2 * 24 * 3600; // 2 days

// ─── Alias table ─────────────────────────────────────────────────────────

using AliasMap = std::unordered_map<std::string, std::vector<std::pair<std::string, int>>>;

static const AliasMap& alias_table()
{
    static const AliasMap m = {
        {"googlechrome",       {{"chrome", 100}}},
        {"chrome",             {{"googlechrome", 80}}},
        {"mozillafirefox",     {{"firefox", 100}}},
        {"firefox",            {{"mozillafirefox", 80}}},
        {"visualstudiocode",   {{"vscode", 100}, {"code", 90}}},
        {"code",               {{"vscode", 100}, {"visualstudiocode", 80}}},
        {"vscode",             {{"visualstudiocode", 80}, {"code", 90}}},
        {"devenv",             {{"vs", 100}, {"visualstudio", 80}}},
        {"visualstudio",       {{"vs", 100}, {"devenv", 80}}},
        {"notepadplusplus",    {{"notepad++", 100}}},
        {"notepad++",          {{"notepadplusplus", 80}}},
        {"vlcmediaplayer",     {{"vlc", 100}}},
        {"vlc",                {{"vlcmediaplayer", 80}}},
        {"gimp",               {{"gimpgnu", 80}}},
        {"libreoffice",        {{"soffice", 90}}},
        {"soffice",            {{"libreoffice", 100}}},
        {"winword",            {{"word", 100}, {"microsoftword", 80}}},
        {"microsoftword",      {{"word", 100}, {"winword", 80}}},
        {"excel",              {{"microsoftexcel", 80}}},
        {"microsoftexcel",     {{"excel", 100}}},
        {"powerpnt",           {{"powerpoint", 100}, {"microsoftpowerpoint", 80}}},
        {"microsoftpowerpoint",{{"powerpoint", 100}, {"powerpnt", 80}}},
        {"outlook",            {{"microsoftoutlook", 80}}},
        {"microsoftoutlook",   {{"outlook", 100}}},
        {"microsoftteams",     {{"teams", 100}}},
        {"teams",              {{"microsoftteams", 80}}},
        {"obsstudio",          {{"obs", 100}, {"obs64", 90}}},
        {"obs64",              {{"obs", 100}, {"obsstudio", 80}}},
        {"imagemagick",        {{"magick", 90}, {"convert", 80}}},
        {"magick",             {{"imagemagick", 80}}},
        {"7z",                 {{"7zip", 100}, {"7zfm", 90}}},
        {"7zfm",               {{"7zip", 100}, {"7z", 90}}},
        {"python3",            {{"python", 100}}},
        {"python",             {{"python3", 90}}},
        {"nodejs",             {{"node", 100}}},
        {"node",               {{"nodejs", 90}}},
        {"gitforwindows",      {{"git", 100}}},
        {"git",                {{"gitforwindows", 80}}},
    };
    return m;
}

// ─── Key builder ─────────────────────────────────────────────────────────

} // namespace (anon) — build_keys must be non-static for AppFinder_Win.cpp

void build_keys(AppEntry& e)
{
    std::unordered_set<std::string> seen;
    auto add = [&](const std::string& v, int w) {
        if (v.empty() || seen.count(v)) return;
        seen.insert(v);
        e.keys.push_back({v, w});
    };

    const std::string norm_name = af_normalize(e.display_name);
    const std::string norm_exe  = af_exe_basename_no_ext(e.exe_path);

    add(norm_name, 100);
    add(norm_exe,  90);

    const auto& at = alias_table();
    auto inject = [&](const std::string& key) {
        auto it = at.find(key);
        if (it == at.end()) return;
        for (const auto& [alias, w] : it->second)
            add(alias, w);
    };
    inject(norm_name);
    inject(norm_exe);
}

namespace {

// ─── Platform stubs (non-Win32) ───────────────────────────────────────────

#if !defined(_WIN32)
std::filesystem::path af_platform_config_dir() { return {}; }
void af_scan_platform(std::vector<AppEntry>&) {}
#endif

// ─── Config dir / cache path ──────────────────────────────────────────────

static fs::path cache_file_path()
{
    auto d = af_platform_config_dir();
    if (d.empty()) return {};
    return d / "app_cache.json";
}

// ─── Dedup helper ─────────────────────────────────────────────────────────

static void dedup(std::vector<AppEntry>& entries)
{
    std::unordered_set<std::string> seen;
    entries.erase(
        std::remove_if(entries.begin(), entries.end(), [&](const AppEntry& e) {
            if (e.exe_path.empty()) return true;
            std::string key = media::strutil::lower_ascii(e.exe_path);
            std::replace(key.begin(), key.end(), '/', '\\');
            if (seen.count(key)) return true;
            seen.insert(key);
            return false;
        }),
        entries.end());
}

// ─── Build full cache ─────────────────────────────────────────────────────

static std::vector<AppEntry> build_cache()
{
    std::vector<AppEntry> entries;
    af_scan_platform(entries);
    dedup(entries);

    pm::log::info_lazy(k_ch, [&] {
        return "cache built: " + std::to_string(entries.size()) + " apps";
    });
    return entries;
}

// ─── Disk persistence ─────────────────────────────────────────────────────

static nlohmann::json entries_to_json(const std::vector<AppEntry>& entries)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : entries) {
        arr.push_back({
            {"n", e.display_name},
            {"e", e.exe_path},
            {"r", e.install_root},
            {"w", e.working_directory},
            {"p", e.app_paths_env},
            {"s", e.source},
        });
    }
    auto now     = std::chrono::system_clock::now();
    auto epoch_s = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    return {{"v", 4}, {"ts", epoch_s}, {"apps", std::move(arr)}};
}

static bool load_cache_from_disk(std::vector<AppEntry>& out)
{
    const auto path = cache_file_path();
    if (path.empty()) return false;

    std::error_code ec;
    if (!fs::exists(path, ec)) return false;

    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    try {
        auto j = nlohmann::json::parse(f);
        if (j.value("v", 0) != 4 || !j.contains("ts") || !j.contains("apps") || !j["apps"].is_array())
            return false;

        auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        int64_t cache_ts = j["ts"].get<int64_t>();
        if (now_s - cache_ts > k_cache_ttl_seconds)
            return false;

        out.clear();
        for (const auto& item : j["apps"]) {
            AppEntry e;
            e.display_name       = item.value("n", "");
            e.exe_path           = item.value("e", "");
            e.install_root       = item.value("r", "");
            e.working_directory  = item.value("w", "");
            e.app_paths_env      = item.value("p", "");
            e.source             = item.value("s", "");
            if (e.exe_path.empty() || af_is_installer_cache_path(e.exe_path)) continue;
            if (af_is_installer_cache_path(e.install_root))
                e.install_root.clear();
            build_keys(e);
            out.push_back(std::move(e));
        }

        pm::log::info_lazy(k_ch, [&] {
            return "cache loaded from disk: " + std::to_string(out.size()) + " apps";
        });
        return !out.empty();
    } catch (...) {
        return false;
    }
}

static void save_cache_to_disk(const std::vector<AppEntry>& entries)
{
    const auto path = cache_file_path();
    if (path.empty()) return;
    try {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << entries_to_json(entries).dump();
    } catch (...) {}
}

// ─── Global cache ─────────────────────────────────────────────────────────

static std::mutex            g_cache_mu;
static bool                  g_cache_ready      = false;
static bool                  g_cache_invalidated = false;
static std::vector<AppEntry> g_cache;

static const std::vector<AppEntry>& ensure_cache()
{
    std::lock_guard<std::mutex> lk(g_cache_mu);
    if (g_cache_ready && !g_cache_invalidated)
        return g_cache;

    g_cache_invalidated = false;

    if (!g_cache_ready && load_cache_from_disk(g_cache)) {
        g_cache_ready = true;
        return g_cache;
    }

    g_cache       = build_cache();
    g_cache_ready = true;
    save_cache_to_disk(g_cache);
    return g_cache;
}

// ─── Scoring (weighted accumulator + rapidfuzz) ───────────────────────────

static constexpr double k_source_boost_app_paths  = 50.0;
static constexpr double k_source_boost_start_menu = 20.0;
static constexpr double k_source_boost_app_package = 20.0;
static constexpr double k_source_boost_known_dir  = 10.0;

static double source_boost(const std::string& source)
{
    if (source == "app_paths")   return k_source_boost_app_paths;
    if (source == "start_menu")  return k_source_boost_start_menu;
    if (source == "app_package") return k_source_boost_app_package;
    if (source == "known_dir")   return k_source_boost_known_dir;
    return 0.0;
}

static double score_entry(const AppEntry& e, const std::string& query_norm, bool lexical_only)
{
    if (query_norm.empty() || e.keys.empty()) return 0.0;
    if (af_is_installer_cache_path(e.exe_path))
        return 0.0;
#if !defined(FEATURE_COMMAND_LLAMA_INFERENCE) || !FEATURE_COMMAND_LLAMA_INFERENCE
    (void)lexical_only;
#endif

    double score   = 0.0;
    bool   any_hit = false;

    for (const auto& key : e.keys) {
        const double w = key.weight / 100.0;

        // §1. Exact match
        if (key.value == query_norm) {
            score += 1000.0 * w;
            any_hit = true;
            continue;
        }

        // §2. Starts-with (either direction)
        if (key.value.size() >= 2 && query_norm.size() >= 2) {
            if (key.value.find(query_norm) == 0) {
                score += 500.0 * w;
                any_hit = true;
            } else if (query_norm.find(key.value) == 0) {
                score += 450.0 * w;
                any_hit = true;
            }
        }

        // §3. Contains (either direction)
        if (key.value.size() >= 3 && query_norm.size() >= 3) {
            if (key.value.find(query_norm) != std::string::npos) {
                score += 250.0 * w;
                any_hit = true;
            } else if (query_norm.find(key.value) != std::string::npos) {
                score += 200.0 * w;
                any_hit = true;
            }
        }

        // §4. Fuzzy match
        if (key.value.size() >= 3 && query_norm.size() >= 3) {
            double ratio = rapidfuzz::fuzz::ratio(query_norm, key.value);
            if (ratio >= 80.0) {
                score += ratio * 2.0 * w;
                any_hit = true;
            }
        }
    }

#if defined(FEATURE_COMMAND_LLAMA_INFERENCE) && FEATURE_COMMAND_LLAMA_INFERENCE
    // §5. Semantic embed fallback — only when all lexical tiers missed.
    // Catches "photo editor" → GIMP, "spreadsheet" → Excel, "browser" → Firefox,
    // "video editor" → DaVinci Resolve, etc. The query and every display_name
    // are cached after first encoding (EmbedCache), so repeated searches for
    // the same user query across a 300-entry app catalog cost 1 encode (query)
    // on subsequent calls once the catalog names are warm.
    // Score range: 75–150 — below fuzzy §4 (~200) so lexical matches always win.
    if (!lexical_only && !any_hit && media::strutil::has_embed_backend()) {
        const float embed = media::strutil::embed_match_score(query_norm, e.display_name);
        if (embed >= 0.50f) {
            score   = static_cast<double>(embed) * 150.0;
            any_hit = true;
        }
    }
#endif

    if (!any_hit) return 0.0;

    // §6. Source boost
    score += source_boost(e.source);
    if (af_is_satellite_name(media::strutil::lower_ascii(e.display_name))
        || af_is_satellite_name(af_exe_basename_no_ext(e.exe_path)))
        score *= 0.2;
    return score;
}

} // namespace

// ─── Public API ───────────────────────────────────────────────────────────

bool name_fits_query(const std::string& qnorm, const AppMatch& m)
{
    const std::string n = af_normalize(m.name);
    const std::string e = af_exe_basename_no_ext(m.exe);
    if (n == qnorm || e == qnorm)
        return true;
    if (qnorm.size() >= 2 && (n.find(qnorm) == 0 || e.find(qnorm) == 0))
        return true;
    if (qnorm.size() >= 3 && (n.find(qnorm) != std::string::npos || e.find(qnorm) != std::string::npos))
        return true;
    if (qnorm.size() >= 4 && n.size() >= 4 && rapidfuzz::fuzz::ratio(qnorm, n) >= 85.0)
        return true;
    if (qnorm.size() >= 4 && e.size() >= 4 && rapidfuzz::fuzz::ratio(qnorm, e) >= 85.0)
        return true;
    if (af_abbrev_match(qnorm, n) || af_abbrev_match(qnorm, e))
        return true;
    auto alias_hit = [&](const std::string& key) {
        const auto it = alias_table().find(key);
        if (it == alias_table().end())
            return false;
        for (const auto& [alias, w] : it->second) {
            (void)w;
            if (alias == qnorm)
                return true;
            if (qnorm.size() >= 3 && alias.find(qnorm) != std::string::npos)
                return true;
        }
        return false;
    };
    return alias_hit(n) || alias_hit(e) || alias_hit(qnorm);
}

std::vector<AppMatch> find_installed_apps(const std::string& query, int max_results, bool lexical_only)
{
    if (query.empty()) return {};

    const auto& cache      = ensure_cache();
    const std::string qnorm = af_normalize(query);

    std::vector<std::pair<double, const AppEntry*>> scored;
    scored.reserve(cache.size());
    for (const auto& e : cache) {
        double s = score_entry(e, qnorm, lexical_only);
        if (s > 0.0)
            scored.push_back({s, &e});
    }

    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        return a.first > b.first;
    });

    const double top = scored.empty() ? 1.0 : scored.front().first;

    std::unordered_set<std::string> seen_exe;
    std::vector<AppMatch> results;
    for (const auto& [raw_score, ep] : scored) {
        if (static_cast<int>(results.size()) >= max_results) break;
        std::string key = media::strutil::lower_ascii(ep->exe_path);
        if (seen_exe.count(key)) continue;
        seen_exe.insert(key);

        AppMatch m;
        m.name        = ep->display_name;
        m.exe         = ep->exe_path;
        m.install_dir = ep->install_root;
        m.source      = ep->source;
        m.confidence  = raw_score / top;
        if (lexical_only && !name_fits_query(qnorm, m))
            continue;
        results.push_back(std::move(m));
    }
    return results;
}

void invalidate_app_cache()
{
    std::lock_guard<std::mutex> lk(g_cache_mu);
    g_cache_invalidated = true;
}

std::string find_path_executable(const std::string& name)
{
    if (name.empty() || name.find_first_of("\\/:") != std::string::npos)
        return {};
    for (unsigned char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok)
            return {};
    }
    const char* path_env = std::getenv("PATH");
    if (!path_env || !*path_env)
        return {};
    const std::string_view pv(path_env);
#if defined(_WIN32)
    const char sep = ';';
    const char* exts[] = {".exe", ".cmd", ".bat", ".com"};
#else
    const char sep = ':';
    const char* exts[] = {""};
#endif
    size_t start = 0;
    while (start <= pv.size()) {
        const auto end = pv.find(sep, start);
        const std::string_view dir = pv.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!dir.empty()) {
            for (const char* ext : exts) {
                std::error_code ec;
                const fs::path p = media::path::path_from_utf8(std::string(dir)) / (name + ext);
                if (fs::is_regular_file(p, ec) && !ec)
                    return media::path::path_to_utf8(p);
            }
        }
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return {};
}

namespace {

media::app::ResolvedApplication resolved_from_match(const AppMatch& m, std::string_view query)
{
    media::app::ResolvedApplication app;
    app.query = std::string(query);
    app.executable = media::path::path_from_utf8(m.exe);
    if (!m.install_dir.empty())
        app.install_root = media::path::path_from_utf8(m.install_dir);
    app.display_name = m.name;
    if (m.source == "app_package") {
        app.source = media::app::ApplicationSource::WindowsPackage;
        app.packaged = true;
        app.match = media::app::MatchQuality::Registered;
        const auto slash = m.exe.find_last_of("\\/");
        if (slash != std::string::npos && slash + 1 < m.exe.size())
            app.launch_name = m.exe.substr(slash + 1);
        else if (!m.exe.empty())
            app.launch_name = m.exe;
    } else {
        app.source = media::app::ApplicationSource::ExistingAppDiscovery;
        app.match = (m.source == "app_paths")
            ? media::app::MatchQuality::Registered
            : media::app::MatchQuality::Heuristic;
    }
    app.exists = !m.exe.empty();
    app.directly_executable = false;
    app.metadata["discovery_source"] = m.source;
    app.metadata["confidence"] = std::to_string(m.confidence);
    return app;
}

struct AppResolverHook {
    AppResolverHook()
    {
        media::app::set_existing_app_discovery_fn(
            [](std::string_view query, const media::app::ResolveRequest& request) {
                (void)request;
                std::vector<media::app::ResolvedApplication> out;
                for (const auto& match : find_installed_apps(std::string(query), 8, true))
                    out.push_back(resolved_from_match(match, query));
                return out;
            });
    }
};

const AppResolverHook g_app_resolver_hook;

} // namespace

} // namespace media::llm::info

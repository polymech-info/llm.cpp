#pragma once
//
// AppFinder_Internal.hpp — shared types and inline helpers.
// Included only by AppFinder.cpp and AppFinder_Win.cpp.
//
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

namespace media::llm::info {

// ─── Data model ────────────────────────────────────────────────────────────

struct SearchKey {
    std::string value;  // normalized: lowered, separators stripped
    int         weight; // 100 = primary name, 90 = exe stem, 80 = alias, …
};

struct AppEntry {
    std::string display_name;
    std::string exe_path;
    std::string install_root;        // only when actually known
    std::string working_directory;   // shortcut Start-in
    std::string app_paths_env;       // App Paths "Path" value
    std::string source; // "app_paths"|"registry_uninstall"|"start_menu"|"known_dir"|"app_package"
    std::vector<SearchKey> keys;
};

// ─── Inline string helpers ──────────────────────────────────────────────────


inline std::string af_normalize(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == ' ' || c == '-' || c == '_' || c == '.')
            continue;
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

// Vowel-stripped form: display name vs shortened exe stem, no product list.
inline std::string af_consonant_skeleton(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u')
            continue;
        out.push_back(static_cast<char>(c));
    }
    return out;
}

inline bool af_abbrev_match(const std::string& a, const std::string& b)
{
    if (a.size() < 4 || b.size() < 4)
        return false;
    const std::string sa = af_consonant_skeleton(a);
    const std::string sb = af_consonant_skeleton(b);
    return sa.size() >= 4 && sa == sb;
}

inline std::string af_parent_dir(const std::string& exe)
{
    return media::path::path_to_utf8(media::path::path_from_utf8(exe).parent_path());
}

inline std::string af_exe_basename_no_ext(const std::string& exe)
{
    return media::strutil::lower_ascii(media::path::path_to_utf8(media::path::path_from_utf8(exe).stem()));
}

inline bool af_exe_exists(const std::string& path)
{
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(media::path::path_from_utf8(path), ec);
}

inline bool af_is_uninstaller_name(const std::string& name_lower)
{
    return name_lower.find("unins")  != std::string::npos
        || name_lower.find("setup")  != std::string::npos
        || name_lower.find("update") != std::string::npos;
}

inline bool af_is_installer_cache_path(std::string path)
{
    path = media::strutil::lower_ascii(std::move(path));
    for (char& c : path) {
        if (c == '/')
            c = '\\';
    }
    return path.find("\\windows\\installer") != std::string::npos
        || path.find("\\config.msi") != std::string::npos;
}

inline bool af_is_satellite_name(const std::string& name_lower)
{
    return name_lower.find("background") != std::string::npos
        || name_lower.find("downloader") != std::string::npos
        || name_lower.find("installation manager") != std::string::npos
        || name_lower.find("install manager") != std::string::npos
        || name_lower.find("settings wizard") != std::string::npos
        || name_lower.find("task scheduler") != std::string::npos
        || name_lower.find("copyopt") != std::string::npos
        || name_lower.find("copy options") != std::string::npos;
}

inline bool af_is_native_ext(std::string ext)
{
    ext = media::strutil::lower_ascii(std::move(ext));
    return ext == ".exe" || ext == ".com";
}

// ─── Platform hooks — defined in AppFinder_Win.cpp (Win32) ─────────────────
//     or stubbed in AppFinder.cpp for non-Win32.

#ifdef _WIN32
/// Returns the PolyMech/pm-image config directory, or an empty path on
/// platforms where it is not applicable.
std::filesystem::path af_platform_config_dir();

/// Scans the platform's installed-application sources (registry, Start Menu,
/// known install directories) and appends discovered entries to `out`.
/// No-op on non-Windows platforms.
void af_scan_platform(std::vector<AppEntry>& out);
#endif

} // namespace media::llm::info

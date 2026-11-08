#pragma once
//
// media::llm::info — discover installed Windows applications.
//
// Scans registry (Uninstall, App Paths), Start Menu shortcuts, known
// install directories, and user Store/MSIX aliases (skipping inbox/system
// packages) once, persists the cache to app_cache.json with a configurable
// TTL, and provides fuzzy name matching.
//
#include "polymech_export.h"

#include <string>
#include <vector>

namespace media::llm::info {

struct AppMatch {
    std::string name;
    std::string exe;
    std::string install_dir; // install root only when known; else empty
    std::string source;      // "app_paths" | "registry_uninstall" | "start_menu" | "known_dir" | "app_package"
    double      confidence;  // inventory rank, not "will run"
};

/// Search the cached installed-app index. First miss may rebuild the cache.
/// Used by info_lookup and Application/InstallRoot resolve — never by Command.
POLYMECH_API std::vector<AppMatch> find_installed_apps(const std::string& query,
                                                       int max_results = 8,
                                                       bool lexical_only = false);

/// Force a cache rebuild (e.g. after installing new software).
POLYMECH_API void invalidate_app_cache();

/// Exact basename on PATH (.exe, then .cmd/.bat/.com on Windows). Empty if none.
/// Does not fuzzy-match and does not search the installed-app index.
POLYMECH_API std::string find_path_executable(const std::string& name);

} // namespace media::llm::info

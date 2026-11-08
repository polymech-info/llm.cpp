#pragma once

#include "kbot.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace polymech {
namespace kbot {

/** True if we treat this path as a text source (UTF-8). Images/PDF reserved for future. */
bool is_text_source_file(const std::string& path_generic);

/**
 * Resolve --include / IPC `include` patterns against `opts.path` (project root).
 * Skips non-text files (e.g. images) with a debug log. Applies `exclude_globs` to relative paths.
 */
std::vector<std::string> collect_source_rel_paths(const KBotOptions& opts);

/**
 * Build user prompt: optional file blocks (`--- file: rel ---` + contents) then `opts.prompt`.
 * If `out_rel_paths` is set, filled with forward-slash relative paths in read order (deduped).
 */
std::string build_prompt_with_sources(const KBotOptions& opts,
                                      std::vector<std::string>* out_rel_paths = nullptr);

/** JSON body for dry-run job_result when includes are used (sources + preview). */
nlohmann::json make_dry_run_ai_result(const KBotOptions& opts, const std::string& augmented_prompt,
                                      const std::vector<std::string>& rel_paths);

} // namespace kbot
} // namespace polymech

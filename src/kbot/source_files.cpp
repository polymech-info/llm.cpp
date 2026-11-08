#include "source_files.h"
#include "logging/logger.h"
#include <glob/glob.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace polymech {
namespace kbot {

namespace {

constexpr std::size_t kMaxBytesPerFile = 4 * 1024 * 1024;

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string ext_of(const fs::path& p) {
  std::string e = p.extension().string();
  return to_lower(e);
}

/** Extensions handled as binary / non-text in this slice (expand later for vision). */
bool is_image_ext(const std::string& ext) {
  static const char* kImg[] = {".jpg",  ".jpeg", ".png",  ".gif", ".webp",
                                 ".bmp", ".tiff", ".tif", ".ico", ".heic", ".avif"};
  for (auto* x : kImg) {
    if (ext == x) return true;
  }
  return false;
}

bool is_pdf_ext(const std::string& ext) { return ext == ".pdf"; }

/** Filename / relative path glob with * and ? only (no **). */
bool glob_match_segment(const std::string& text, const std::string& pat) {
  const size_t n = text.size(), m = pat.size();
  std::vector<std::vector<bool>> dp(n + 1, std::vector<bool>(m + 1, false));
  dp[0][0] = true;
  for (size_t j = 1; j <= m; ++j) {
    if (pat[j - 1] == '*') dp[0][j] = dp[0][j - 1];
  }
  for (size_t i = 1; i <= n; ++i) {
    for (size_t j = 1; j <= m; ++j) {
      if (pat[j - 1] == '*') {
        dp[i][j] = dp[i][j - 1] || dp[i - 1][j];
      } else if (pat[j - 1] == '?' || text[i - 1] == pat[j - 1]) {
        dp[i][j] = dp[i - 1][j - 1];
      }
    }
  }
  return dp[n][m];
}

fs::path absolute_root(const std::string& path_opt) {
  if (path_opt.empty()) return fs::absolute(fs::path("."));
  return fs::absolute(fs::path(path_opt));
}

bool excluded(const std::string& rel_fwd, const std::vector<std::string>& exclude_globs) {
  for (const auto& pat : exclude_globs) {
    if (pat.empty()) continue;
    if (glob_match_segment(rel_fwd, pat)) return true;
    fs::path p(rel_fwd);
    if (glob_match_segment(p.filename().string(), pat)) return true;
  }
  return false;
}

void push_unique(std::vector<fs::path>& out, std::unordered_set<std::string>& seen, const fs::path& file) {
  std::string key = file.generic_string();
  if (seen.insert(key).second) out.push_back(file);
}

void expand_one_pattern(const fs::path& root, const std::string& pattern_str,
                        std::vector<fs::path>& out, std::unordered_set<std::string>& seen) {
  fs::path pat_path = pattern_str.empty() ? fs::path() : fs::path(pattern_str);
  fs::path resolved = pat_path.is_absolute() ? pat_path : (root / pat_path);
  resolved = resolved.lexically_normal();

  const std::string pat = resolved.string();

  std::vector<fs::path> matched;
  try {
    if (pattern_str.find("**") != std::string::npos) {
      matched = glob::rglob(pat);
    } else {
      matched = glob::glob(pat);
    }
  } catch (const std::exception& e) {
    logger::warn(std::string("source_files: glob failed: ") + e.what());
    return;
  }

  for (auto& p : matched) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) continue;
    fs::path canon = fs::weakly_canonical(p, ec);
    if (!ec) push_unique(out, seen, canon);
  }
}

std::string read_file_limited(const fs::path& p, std::size_t max_bytes) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  std::string buf;
  buf.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (buf.size() > max_bytes) {
    logger::warn("source_files: truncating large file " + p.generic_string());
    buf.resize(max_bytes);
  }
  return buf;
}

} // namespace

bool is_text_source_file(const std::string& path_generic) {
  fs::path p(path_generic);
  std::string ext = ext_of(p);
  if (is_image_ext(ext)) return false;
  if (is_pdf_ext(ext)) return false;
  if (ext.empty()) return true;
  /* Code / text-like extensions (aligned with TS text/* + common sources) */
  static const char* kText[] = {
      ".txt",  ".md",   ".json", ".js",   ".mjs",  ".cjs",  ".ts",   ".tsx", ".jsx",  ".css",
      ".html", ".htm",  ".xml",  ".csv",  ".yaml", ".yml",  ".toml", ".sh",  ".py",
      ".rs",   ".go",   ".java", ".cpp",  ".cc",   ".cxx",  ".h",    ".hpp", ".c",
      ".cs",   ".rb",   ".php",  ".swift", ".kt",  ".vue",  ".svelte", ".scss", ".less",
      ".ini",  ".cfg",  ".properties", ".gradle", ".cmake", ".mdx", ".log", ".sql",
  };
  for (auto* x : kText) {
    if (ext == x) return true;
  }
  return false;
}

std::vector<std::string> collect_source_rel_paths(const KBotOptions& opts) {
  std::vector<std::string> rel;
  build_prompt_with_sources(opts, &rel);
  return rel;
}

std::string build_prompt_with_sources(const KBotOptions& opts, std::vector<std::string>* out_rel_paths) {
  if (opts.include_globs.empty()) {
    return opts.prompt;
  }

  const fs::path root = absolute_root(opts.path);
  std::vector<fs::path> files;
  std::unordered_set<std::string> seen;

  for (const auto& inc : opts.include_globs) {
    if (inc.empty()) continue;
    expand_one_pattern(root, inc, files, seen);
  }

  std::ostringstream body;
  for (const auto& abs : files) {
    std::error_code ec;
    if (!fs::is_regular_file(abs, ec) || ec) continue;

    std::string abs_gen = abs.generic_string();
    if (!is_text_source_file(abs_gen)) {
      logger::info("source_files: skip non-text (e.g. image): " + abs_gen);
      continue;
    }

    fs::path rel = fs::relative(abs, root, ec);
    if (ec) rel = abs.filename();
    std::string rel_fwd = rel.generic_string();

    if (excluded(rel_fwd, opts.exclude_globs)) {
      logger::debug("source_files: excluded: " + rel_fwd);
      continue;
    }

    std::string content = read_file_limited(abs, kMaxBytesPerFile);
    if (out_rel_paths) out_rel_paths->push_back(rel_fwd);

    body << "--- file: " << rel_fwd << " ---\n";
    body << content;
    if (!content.empty() && content.back() != '\n') body << '\n';
    body << '\n';
  }

  if (!opts.prompt.empty()) {
    body << opts.prompt;
  }

  return body.str();
}

nlohmann::json make_dry_run_ai_result(const KBotOptions& opts, const std::string& augmented_prompt,
                                      const std::vector<std::string>& rel_paths) {
  nlohmann::json o;
  o["status"] = "success";
  o["mode"] = "ai";
  o["text"] = "[dry-run] no LLM call";
  o["dry_run"] = true;
  o["path"] = opts.path.empty() ? std::string(".") : opts.path;
  o["sources"] = rel_paths;
  o["prompt_char_count"] = augmented_prompt.size();
  const std::size_t cap = 2000;
  if (augmented_prompt.size() <= cap) {
    o["prompt_preview"] = augmented_prompt;
  } else {
    o["prompt_preview"] = augmented_prompt.substr(0, cap);
    o["prompt_preview_truncated"] = true;
  }
  return o;
}

} // namespace kbot
} // namespace polymech

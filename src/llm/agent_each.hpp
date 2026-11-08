#pragma once

/// Shared options + runner for `llm agent each` and `POST /v1/llm/each`.
/// CLI flags and the HTTP JSON body map onto `EachOptions` via
/// `parse_each_options` (camelCase and snake_case keys both accepted).

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace media::llm::agent::each {

struct EachOptions {
    /// Inline document. When null/absent, loaded from `source`.
    nlohmann::json document = nullptr;
    std::string source;
    std::string selector;
    std::string target;
    std::string dst;
    std::string prompt;
    std::string system_prompt;
    std::string transform = "upper"; ///< dry-run: upper|prefix|identity|llm-stub|json-stub
    std::string prefix = "x";
    int concurrency = 0; ///< 0 = auto (1 dry-run, 4 live)
    int throttle_ms = 0;
    int max_retries = 0; ///< 0 = auto
    bool dry_run = false;
    bool no_cache = false;
    bool merge_json = false;
    std::vector<std::string> merge_keys;
    std::string api_mode = "completion";
    int abort_consecutive = 0;
    bool partial_commit = false;

    std::string preset;
    std::string router;
    std::string model;
    std::string api_key;
    std::string base_url;
    int timeout_ms = 0;

    /// Base directory for relative `source` / `dst` paths.
    std::string cwd;
};

struct EachResult {
    bool ok = false;
    std::string error;
    nlohmann::json document = nullptr;

    std::string source;
    std::string selector;
    std::string target;
    std::string dst;
    bool dry_run = false;
    bool merge_json = false;
    std::vector<std::string> merge_keys;
    std::string router;
    std::string model;
    int concurrency = 0;
    int matched = 0;
    int transformed = 0;
    int skipped = 0;
    int errors = 0;
    int cache_hits = 0;
    bool aborted = false;
    int aborted_after_errors = 0;
    bool partial_commit = false;
    std::int64_t collect_ms = 0;
    std::int64_t transform_ms = 0;
    std::int64_t apply_ms = 0;
    bool wrote_dst = false;

    nlohmann::json to_json() const;
};

using ProgressCallback = std::function<void(int done, int total, const std::string& path)>;
using ErrorCallback = std::function<void(const std::string& path,
                                         const std::string& value,
                                         const std::string& error)>;

/// Fill `out` from an HTTP / tool JSON object. Does not load files.
bool parse_each_options(const nlohmann::json& j, EachOptions& out, std::string& err);

/// Validate required fields (selector; prompt unless dry-run; document or source).
bool validate_each_options(const EachOptions& opt, std::string& err);

/// Run the iterator. Mutates a copy of the document (or loads `source`).
/// When `dst` is set, writes on success (or partial commit when enabled).
bool run_each(const EachOptions& opt,
              EachResult& out,
              ProgressCallback progress = {},
              ErrorCallback on_error = {});

} // namespace media::llm::agent::each

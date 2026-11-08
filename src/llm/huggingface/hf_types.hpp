#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace pm::hf {

struct DownloadProgress {
    std::uint64_t bytes_done    = 0;
    std::uint64_t bytes_total   = 0; ///< 0 when unknown
    double        bytes_per_sec = 0.0;
    std::string   current_file;
    int           file_index = 0;
    int           file_count = 0;
    bool          paused     = false;
};

using ProgressFn = std::function<void(const DownloadProgress&)>;
using CancelFn   = std::function<bool()>;
using PauseFn    = std::function<bool()>; ///< return true while paused

struct StreamDownloadOptions {
    std::string   bearer_token;
    bool          resume         = true;
    bool          keep_partial   = true;
    int           retries        = 3;
    int           retry_delay_ms = 500;
    long          timeout_ms     = 0;
    long          connect_timeout_ms = 30'000;
    std::int64_t  expected_size  = -1;
    bool          skip_existing  = true;
    bool          overwrite      = false;
    ProgressFn    on_progress;
    CancelFn      cancel_check;
    PauseFn       pause_check;
};

struct StreamDownloadResult {
    bool                    ok = false;
    bool                    skipped = false;
    bool                    cancelled = false;
    std::string             err;
    long                    http_status = 0;
    std::uint64_t           bytes_written = 0;
    std::uint64_t           resumed_from  = 0;
    std::filesystem::path   local_path;
    std::string             effective_url;
};

struct ModelEntry {
    std::string alias;
    std::string repo_id;
    std::string store_name;
    std::string format = "gguf";
    std::string default_variant;
    std::string description;
    /// Optional direct file URL (GitHub raw, etc.) for non-GGUF catalog assets.
    std::string source_url;
    /// Optional destination basename when the upstream filename is non-canonical.
    std::string local_filename;
};

struct RepoFileInfo {
    std::string path;
    std::string name;
    std::uint64_t size_bytes = 0;
    bool lfs = false;
    std::string xet_hash; ///< Xet file id from HF tree API (when on Xet storage)
};

struct VariantInfo {
    std::string label;
    std::vector<RepoFileInfo> files;
    std::uint64_t total_bytes = 0;
    int quant_bits = 0;
    double size_gb = 0.0;
    bool sharded = false;
    std::uint64_t kv_cache_bytes_est = 0;
    std::uint64_t total_vram_bytes_est = 0;
    double total_vram_gb_est = 0.0;
    std::uint32_t vram_context_assumed = 8192;
    bool vram_heuristic = false;
};

struct RepoCapabilities {
    bool has_vision = false;
    std::vector<RepoFileInfo> mmproj_files;
    std::optional<RepoFileInfo> recommended_mmproj;
};

struct ProbeResult {
    bool ok = false;
    std::string err;
    ModelEntry entry;
    std::vector<std::string> variants;
    std::vector<VariantInfo> variant_infos;
    nlohmann::json model_info = nlohmann::json::object();
    RepoCapabilities repo;
    nlohmann::json summary = nlohmann::json::object();
};

} // namespace pm::hf

#pragma once

#include "llm/huggingface/hf_types.hpp"

namespace pm::hf {

struct DownloadRequest {
    ModelEntry entry;
    std::string variant;
    std::filesystem::path output_root;
    std::string token;
    bool remanifest_only = false;
    bool skip_existing   = true;
    bool overwrite       = false;
    bool write_manifest  = true;
    bool write_sidecar   = true;
    ProgressFn on_progress;
    CancelFn   cancel_check;
    PauseFn    pause_check;
};

struct DownloadResult {
    bool ok = false;
    bool cancelled = false;
    bool skipped = false;
    std::string err;
    std::filesystem::path output_dir;
    std::vector<std::filesystem::path> files;
    nlohmann::json manifest = nlohmann::json::object();
    nlohmann::json sidecar  = nlohmann::json::object();
};

struct OrgProbeResult {
    bool ok = false;
    std::string err;
    std::string org;
    nlohmann::json models = nlohmann::json::array();
};

ProbeResult probe_model(const std::string& alias_or_repo, const std::string& token = {},
                        const std::string& variant = {});

OrgProbeResult probe_org(const std::string& org_or_url, const std::string& token = {}, int limit = 100);

struct SearchProbeResult {
    bool ok = false;
    std::string err;
    std::string query;
    bool gguf_only = true;
    nlohmann::json models = nlohmann::json::array();
};

SearchProbeResult search_models(const std::string& query, const std::string& token = {},
                                int limit = 20, bool gguf_only = true);

/// Hub model info + README. No file-tree listing (unlike probe_model).
struct ModelCardResult {
    bool ok = false;
    std::string err;
    ModelEntry entry;
    nlohmann::json model_info = nlohmann::json::object();
    std::string readme;
    std::string description;
};

ModelCardResult fetch_model_card(const std::string& alias_or_repo, const std::string& token = {});

DownloadResult download_gguf_variant(DownloadRequest request);

/// Single-file catalog download (onnx-tar, etc.). Uses `source_url` when set,
/// otherwise Hugging Face resolve for `default_variant` / `--variant`.
DownloadResult download_named_file(DownloadRequest request);

} // namespace pm::hf

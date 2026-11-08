#pragma once

#include "llm/huggingface/hf_types.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace pm::hf {

/// Token order: override → user variable HF_TOKEN → providers.huggingface.api_key → HF_TOKEN env.
std::string resolve_hf_token(const std::string& override_token = {});

std::string hf_api_base();

std::string build_resolve_url(const std::string& repo_id, const std::string& filename);

struct HubRequestResult {
    bool        ok = false;
    long        http_status = 0;
    std::string err;
    std::string body;
    nlohmann::json json;
};

HubRequestResult hub_get_json(const std::string& path, const std::string& token = {});

HubRequestResult fetch_model_info(const std::string& repo_id, const std::string& token = {});

/// GET /{repo}/raw/{main|master}/README.md (model card markdown, not JSON).
HubRequestResult fetch_model_readme(const std::string& repo_id, const std::string& token = {});

/// GET /api/models?author={org}
HubRequestResult fetch_org_models(const std::string& org, const std::string& token = {}, int limit = 100);

/// GET /api/models?search={query}&sort=downloads (optional filter=gguf)
HubRequestResult fetch_search_models(const std::string& query, const std::string& token = {},
                                     int limit = 20, bool gguf_only = true);

/// GET /api/models?filter=gguf&pipeline_tag=text-generation&sort=downloads
HubRequestResult fetch_popular_gguf_models(int limit = 40, const std::string& token = {});

std::vector<RepoFileInfo> list_repo_files(const std::string& repo_id, const std::string& token = {},
                                          std::string* err_out = nullptr);

HubRequestResult probe_remote_file(const std::string& url, const std::string& token = {});

/// Binary GET (no JSON parse). Optional `Range: bytes=a-b` header (end inclusive per Xet spec).
HubRequestResult hub_http_get_bytes(const std::string& url, const std::string& bearer_token = {},
                                    const std::string& range_header = {});

struct XetReadToken {
    std::string cas_url;
    std::string access_token;
    std::int64_t exp_unix = 0;
};

/// GET /api/models/{repo}/xet-read-token/{revision} using HF bearer token.
std::optional<XetReadToken> fetch_xet_read_token(const std::string& repo_id, const std::string& revision,
                                                 const std::string& hf_token);

/// Model commit sha from HF model info (`sha` field).
std::string fetch_model_revision(const std::string& repo_id, const std::string& hf_token = {});

} // namespace pm::hf

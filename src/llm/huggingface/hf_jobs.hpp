#pragma once

#include "llm/huggingface/hf_download.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>

namespace pm::hf {

using HostEventPoster = std::function<void(const std::string& target_id,
                                           const std::string& event_type,
                                           nlohmann::json payload)>;

struct JobRpcResult {
    bool              ok = false;
    nlohmann::json    data = nlohmann::json::object();
    std::string       error;
};

JobRpcResult jobs_start(const std::string& from_id, const nlohmann::json& req, HostEventPoster post);
JobRpcResult jobs_cancel(const nlohmann::json& req);
JobRpcResult jobs_pause(const nlohmann::json& req);
JobRpcResult jobs_tasks_get();

JobRpcResult jobs_variants_list(const nlohmann::json& req);
JobRpcResult jobs_meta_get(const nlohmann::json& req);
JobRpcResult jobs_model_info(const nlohmann::json& req);
JobRpcResult jobs_probe(const nlohmann::json& req);
JobRpcResult jobs_org_list(const nlohmann::json& req);
JobRpcResult jobs_search(const nlohmann::json& req);
JobRpcResult jobs_discover(const nlohmann::json& req);

} // namespace pm::hf

#include "llm/huggingface/hf_jobs.hpp"

#include "llm/huggingface/hf_catalog.hpp"
#include "llm/huggingface/hf_discover.hpp"
#include "llm/huggingface/hf_hub_client.hpp"
#include "llm/huggingface/hf_sidecar.hpp"
#include "llm/huggingface/hf_ref.hpp"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include "fs/path_sanitizer.hpp"

namespace fs = std::filesystem;

namespace pm::hf {
namespace {

std::string json_string_or(const nlohmann::json& j, const char* key, const std::string& fallback = {})
{
    if (j.contains(key) && j[key].is_string())
        return j[key].get<std::string>();
    return fallback;
}

bool json_bool_or(const nlohmann::json& j, const char* key, bool fallback)
{
    if (j.contains(key) && j[key].is_boolean())
        return j[key].get<bool>();
    return fallback;
}

int json_int_or(const nlohmann::json& j, const char* key, int fallback)
{
    if (j.contains(key) && j[key].is_number_integer())
        return static_cast<int>(j[key].get<long long>());
    return fallback;
}

double json_double_or(const nlohmann::json& j, const char* key, double fallback)
{
    if (j.contains(key) && j[key].is_number())
        return j[key].get<double>();
    return fallback;
}

std::string make_task_id()
{
    static std::atomic<unsigned> seq{0};
    return "hg-" + std::to_string(++seq);
}

struct Job {
    std::string              task_id;
    std::string              from_id;
    std::string              alias_or_repo;
    std::string              repo_id;
    std::string              variant;
    std::string              local_root;
    std::string              phase = "running";
    std::atomic<bool>        cancel_requested{false};
    std::atomic<bool>        pause_requested{false};
    std::atomic<bool>        running{true};
    std::uint64_t            bytes_written = 0;
    std::string              error;
};

std::mutex g_jobs_mutex;
std::unordered_map<std::string, std::shared_ptr<Job>> g_jobs;

void set_job_phase(const std::shared_ptr<Job>& job, std::string phase, std::string err = {})
{
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    job->phase = std::move(phase);
    if (!err.empty())
        job->error = std::move(err);
    job->running.store(job->phase == "running" || job->phase == "paused");
}

nlohmann::json job_snapshot_unlocked(const std::shared_ptr<Job>& job)
{
    const bool paused = job->pause_requested.load();
    std::string phase = job->phase;
    if (job->running.load() && paused)
        phase = "paused";
    return nlohmann::json{
        {"taskId", job->task_id},
        {"phase", phase},
        {"repoId", job->repo_id},
        {"aliasOrRepo", job->alias_or_repo},
        {"variant", job->variant},
        {"localRoot", job->local_root},
        {"modelsDir", media::path::path_to_utf8(default_models_root())},
        {"running", job->running.load()},
        {"paused", paused},
        {"bytesWritten", job->bytes_written},
        // Avoid nullptr : std::string ternary — MSVC picks const char* as the common type,
        // which passes nullptr to std::string(const char*) → strlen(nullptr) → AV.
        {"error", job->error.empty() ? nlohmann::json(nullptr) : nlohmann::json(job->error)},
    };
}

std::shared_ptr<Job> find_job(const std::string& task_id)
{
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    const auto it = g_jobs.find(task_id);
    return it == g_jobs.end() ? nullptr : it->second;
}

fs::path resolve_local_root(const std::string& local_dir)
{
    return resolve_models_root(local_dir);
}

} // namespace

JobRpcResult jobs_start(const std::string& from_id, const nlohmann::json& req, HostEventPoster post)
{
    JobRpcResult out;
    const std::string alias_or_repo = json_string_or(req, "aliasOrRepo",
        json_string_or(req, "repoId", json_string_or(req, "model")));
    if (alias_or_repo.empty()) {
        out.error = "aliasOrRepo (or repoId) is required";
        return out;
    }

    const auto entry = resolve_model_ref(alias_or_repo);
    if (!entry) {
        out.error = "unknown model alias or repo: " + alias_or_repo;
        return out;
    }

    const std::string variant   = json_string_or(req, "variant", entry->default_variant);
    const std::string local_dir = json_string_or(req, "localDir", json_string_or(req, "local_dir"));
    const bool overwrite        = json_bool_or(req, "overwrite", false);
    const bool remanifest_only  = json_bool_or(req, "remanifestOnly", false);
    const std::string token     = json_string_or(req, "token");

    fs::path local_root;
    try {
        local_root = resolve_local_root(local_dir);
        std::error_code ec;
        fs::create_directories(local_root, ec);
    } catch (const std::exception& e) {
        out.error = e.what();
        return out;
    }

    auto job               = std::make_shared<Job>();
    job->task_id           = make_task_id();
    job->from_id           = from_id;
    job->alias_or_repo     = alias_or_repo;
    job->repo_id           = entry->repo_id;
    job->variant           = variant;
    job->local_root        = media::path::path_to_utf8(local_root);

    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        g_jobs[job->task_id] = job;
    }

    std::thread([job, post, entry = *entry, local_root, overwrite, remanifest_only, token]() {
        post(job->from_id, "hg.download.started",
             {{"taskId", job->task_id},
              {"repoId", job->repo_id},
              {"variant", job->variant},
              {"localRoot", job->local_root}});

        DownloadRequest dreq;
        dreq.entry          = entry;
        dreq.variant        = job->variant;
        dreq.output_root    = local_root;
        dreq.token          = token;
        dreq.remanifest_only = remanifest_only;
        dreq.skip_existing  = !overwrite;
        dreq.overwrite      = overwrite;
        dreq.cancel_check   = [job] { return job->cancel_requested.load(); };
        dreq.pause_check    = [job] { return job->pause_requested.load(); };
        dreq.on_progress    = [job, post](const DownloadProgress& p) {
            {
                std::lock_guard<std::mutex> lock(g_jobs_mutex);
                job->bytes_written = p.bytes_done;
            }
            post(job->from_id, "hg.download.progress",
                 {{"taskId", job->task_id},
                  {"repoId", job->repo_id},
                  {"variant", job->variant},
                  {"localRoot", job->local_root},
                  {"currentFile", p.current_file},
                  {"fileIndex", p.file_index},
                  {"fileCount", p.file_count},
                  {"bytesDone", p.bytes_done},
                  {"bytesTotal", p.bytes_total},
                  {"bytesPerSec", p.bytes_per_sec},
                  {"paused", p.paused}});
        };

        const DownloadResult result = download_gguf_variant(std::move(dreq));

        if (job->cancel_requested.load()) {
            set_job_phase(job, "cancelled");
            post(job->from_id, "hg.download.cancelled", {{"taskId", job->task_id}});
            return;
        }

        if (!result.ok) {
            const std::string err = result.err.empty() ? "hg download failed" : result.err;
            set_job_phase(job, "failed", err);
            post(job->from_id, "hg.download.failed", {{"taskId", job->task_id}, {"error", err}});
            return;
        }

        set_job_phase(job, "completed");
        nlohmann::json files = nlohmann::json::array();
        for (const auto& f : result.files)
            files.push_back(media::path::path_to_utf8(f));
        post(job->from_id, "hg.download.complete",
             {{"taskId", job->task_id},
              {"repoId", job->repo_id},
              {"variant", job->variant},
              {"localRoot", media::path::path_to_utf8(result.output_dir)},
              {"modelsDir", media::path::path_to_utf8(default_models_root())},
              {"skipped", result.skipped},
              {"bytesWritten", job->bytes_written},
              {"files", files},
              {"manifest", result.manifest},
              {"sidecar", result.sidecar},
              {"sidecarFile", k_local_sidecar_filename}});
    }).detach();

    out.ok   = true;
    out.data = {{"taskId", job->task_id},
                {"repoId", entry->repo_id},
                {"aliasOrRepo", alias_or_repo},
                {"variant", variant},
                {"localRoot", media::path::path_to_utf8(local_root)},
                {"modelsDir", media::path::path_to_utf8(default_models_root())}};
    return out;
}

JobRpcResult jobs_cancel(const nlohmann::json& req)
{
    JobRpcResult out;
    const std::string task_id = json_string_or(req, "taskId", json_string_or(req, "task_id"));
    if (task_id.empty()) {
        out.error = "taskId is required";
        return out;
    }
    const auto job = find_job(task_id);
    if (!job) {
        out.error = "task not found";
        return out;
    }
    job->cancel_requested.store(true);
    job->pause_requested.store(false);
    out.ok   = true;
    out.data = {{"taskId", task_id}, {"cancelRequested", true}};
    return out;
}

JobRpcResult jobs_pause(const nlohmann::json& req)
{
    JobRpcResult out;
    const std::string task_id = json_string_or(req, "taskId", json_string_or(req, "task_id"));
    if (task_id.empty()) {
        out.error = "taskId is required";
        return out;
    }
    const auto job = find_job(task_id);
    if (!job) {
        out.error = "task not found";
        return out;
    }
    const bool paused = json_bool_or(req, "paused", !job->pause_requested.load());
    job->pause_requested.store(paused);
    out.ok   = true;
    out.data = {{"taskId", task_id}, {"paused", paused}};
    return out;
}

JobRpcResult jobs_tasks_get()
{
    JobRpcResult out;
    nlohmann::json tasks = nlohmann::json::array();
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    for (const auto& kv : g_jobs)
        tasks.push_back(job_snapshot_unlocked(kv.second));
    out.ok   = true;
    out.data = {{"tasks", tasks}};
    return out;
}

JobRpcResult jobs_variants_list(const nlohmann::json& req)
{
    JobRpcResult out;
    const std::string alias_or_repo = json_string_or(req, "aliasOrRepo",
        json_string_or(req, "repoId", json_string_or(req, "model")));
    if (alias_or_repo.empty()) {
        out.error = "aliasOrRepo is required";
        return out;
    }
    const std::string token = json_string_or(req, "token");
    const ProbeResult probe = probe_model(alias_or_repo, token);
    if (!probe.ok) {
        out.error = probe.err.empty() ? "variants list failed" : probe.err;
        return out;
    }
    nlohmann::json variants = nlohmann::json::array();
    for (const auto& v : probe.variant_infos)
        variants.push_back(pm::hf::variant_info_to_json(v));
    out.ok   = true;
    out.data = {{"aliasOrRepo", alias_or_repo},
                {"repoId", probe.entry.repo_id},
                {"storeName", probe.entry.store_name},
                {"repo", repo_capabilities_to_json(probe.repo)},
                {"summary", probe.summary},
                {"variants", variants}};
    return out;
}

JobRpcResult jobs_meta_get(const nlohmann::json& req)
{
    JobRpcResult out;
    const std::string alias_or_repo = json_string_or(req, "aliasOrRepo",
        json_string_or(req, "repoId", json_string_or(req, "model")));
    if (alias_or_repo.empty()) {
        out.error = "aliasOrRepo is required";
        return out;
    }
    const std::string token = json_string_or(req, "token");
    const ModelCardResult card = fetch_model_card(alias_or_repo, token);
    if (!card.ok) {
        out.error = card.err.empty() ? "meta fetch failed" : card.err;
        return out;
    }
    out.ok   = true;
    out.data = {{"aliasOrRepo", alias_or_repo},
                {"repoId", card.entry.repo_id},
                {"entry", {{"alias", card.entry.alias},
                           {"repoId", card.entry.repo_id},
                           {"storeName", card.entry.store_name},
                           {"format", card.entry.format},
                           {"defaultVariant", card.entry.default_variant},
                           {"description", card.description}}},
                {"readme", card.readme},
                {"modelInfo", card.model_info}};
    return out;
}

JobRpcResult jobs_model_info(const nlohmann::json& req)
{
    return jobs_meta_get(req);
}

JobRpcResult jobs_probe(const nlohmann::json& req)
{
    JobRpcResult out;
    const std::string alias_or_repo = json_string_or(req, "aliasOrRepo",
        json_string_or(req, "repoId", json_string_or(req, "model")));
    if (alias_or_repo.empty()) {
        out.error = "aliasOrRepo is required";
        return out;
    }
    const std::string variant = json_string_or(req, "variant");
    const std::string token   = json_string_or(req, "token");
    const ProbeResult probe   = probe_model(alias_or_repo, token, variant);
    if (!probe.ok) {
        out.error = probe.err.empty() ? "probe failed" : probe.err;
        return out;
    }
    nlohmann::json variants = nlohmann::json::array();
    for (const auto& v : probe.variant_infos)
        variants.push_back(pm::hf::variant_info_to_json(v));
    out.ok   = true;
    out.data = {{"aliasOrRepo", alias_or_repo},
                {"repoId", probe.entry.repo_id},
                {"storeName", probe.entry.store_name},
                {"variant", variant},
                {"variantLabels", probe.variants},
                {"repo", repo_capabilities_to_json(probe.repo)},
                {"summary", probe.summary},
                {"variants", variants},
                {"modelInfo", probe.model_info}};
    return out;
}

JobRpcResult jobs_org_list(const nlohmann::json& req)
{
    JobRpcResult out;
    const std::string org_ref = json_string_or(req, "orgOrUrl",
        json_string_or(req, "org", json_string_or(req, "aliasOrRepo")));
    if (org_ref.empty()) {
        out.error = "orgOrUrl is required";
        return out;
    }
    const int limit = json_int_or(req, "limit", 100);
    const std::string token = json_string_or(req, "token");
    const OrgProbeResult org = probe_org(org_ref, token, limit);
    if (!org.ok) {
        out.error = org.err.empty() ? "org list failed" : org.err;
        return out;
    }
    out.ok   = true;
    out.data = {{"org", org.org}, {"count", org.models.size()}, {"models", org.models}};
    return out;
}

JobRpcResult jobs_search(const nlohmann::json& req)
{
    JobRpcResult out;
    const std::string query = json_string_or(req, "query",
        json_string_or(req, "search", json_string_or(req, "q")));
    if (query.empty()) {
        out.error = "query is required";
        return out;
    }
    const int limit = json_int_or(req, "limit", 20);
    const bool gguf_only = json_bool_or(req, "ggufOnly", json_bool_or(req, "gguf_only", true));
    const std::string token = json_string_or(req, "token");
    const SearchProbeResult found = search_models(query, token, limit, gguf_only);
    if (!found.ok) {
        out.error = found.err.empty() ? "search failed" : found.err;
        return out;
    }
    out.ok   = true;
    out.data = {{"query", found.query},
                {"ggufOnly", found.gguf_only},
                {"count", found.models.size()},
                {"models", found.models}};
    return out;
}

JobRpcResult jobs_discover(const nlohmann::json& req)
{
    JobRpcResult out;
    try {
        const double gpu_gb = json_double_or(req, "gpuTotalGb", json_double_or(req, "gpu_total_gb", 0.0));
        const double sys_gb = json_double_or(req, "sysMemGb", json_double_or(req, "sys_mem_gb", 0.0));
        const bool metal = json_bool_or(req, "metal", false);
        const int ctx = json_int_or(req, "contextTokens", json_int_or(req, "context_tokens", 8192));
        const int scan = json_int_or(req, "scan", 40);
        const std::string token = json_string_or(req, "token");
        out.ok = true;
        out.data = discover_popular_gguf(gpu_gb, metal, sys_gb, ctx, scan, token);
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

} // namespace pm::hf

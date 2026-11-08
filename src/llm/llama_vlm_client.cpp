#include "llama_vlm_client.hpp"

#include "core/local_model_catalog.hpp"
#include "llm/ocr_grounding.hpp"
#include "logging/logger.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include "fs/path_sanitizer.hpp"

#if defined(FEATURE_VLM) && FEATURE_VLM
#  include "lib/llm/llama/VlmModel.hpp"
#endif

namespace media::llm::vlm_client {
namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Timing helper
// ─────────────────────────────────────────────────────────────────────────────
using Clock = std::chrono::steady_clock;
static float elapsed_ms_since(Clock::time_point t0)
{
    return static_cast<float>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count()
    ) / 1000.f;
}

// ─────────────────────────────────────────────────────────────────────────────
// GPU layer resolution.
// Reads PM_VLM_NGL first, then PM_AGENT_LOCAL_NGL, then defaults to -1 (all).
// ─────────────────────────────────────────────────────────────────────────────
int n_gpu_layers_from_env()
{
    for (const char* var : { "PM_VLM_NGL", "PM_AGENT_LOCAL_NGL" }) {
        const char* raw = std::getenv(var);
        if (!raw || !*raw) continue;
        try { return std::stoi(raw); } catch (...) {}
    }
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Single-slot VlmModel cache (keyed by model_path + mmproj_path + n_gpu_layers).
// ─────────────────────────────────────────────────────────────────────────────
#if defined(FEATURE_VLM) && FEATURE_VLM

struct CachedVlm {
    std::string model_id;
    std::string model_path;
    std::string mmproj_path;
    int         n_gpu_layers = INT_MIN;
    int         active_calls = 0;
    media::llama::VlmModel model;
};

std::mutex& cache_mutex()
{
    static std::mutex m;
    return m;
}

// Heap, not a function-local static. ~VlmModel calls llama_free / mtmd_free,
// which touch CUDA. A static destructor runs after the CUDA runtime has
// started unloading, and ggml then aborts (ggml-cuda.cu CUDA error,
// process status 0xC0000409) even though inference already returned 0.
// Callers release the model while the runtime is still up
// (unload_loaded_model). The empty shell is leaked on purpose.
CachedVlm*& cache_ptr()
{
    static CachedVlm* slot = new CachedVlm;
    return slot;
}

CachedVlm& cache_slot()
{
    return *cache_ptr();
}

media::llama::VlmModel* model_for_paths(const std::string& model_path,
                                         const std::string& model_id,
                                         const std::string& mmproj_path,
                                         int                n_gpu_layers,
                                         int                n_threads,
                                         int                n_ctx,
                                         bool&              use_active,
                                         std::string&       err)
{
    std::lock_guard<std::mutex> lock(cache_mutex());
    CachedVlm& slot = cache_slot();

    const bool cache_hit = (slot.model_path   == model_path   &&
                            slot.mmproj_path  == mmproj_path  &&
                            slot.n_gpu_layers == n_gpu_layers &&
                            slot.model.loaded());
    if (cache_hit) {
        logger::trace("[vlm_client] model cache hit: " + slot.model_path);
        ++slot.active_calls;
        use_active = true;
        return &slot.model;
    }

    if (slot.active_calls > 0) {
        err = "cached VLM model is busy";
        return nullptr;
    }

    if (slot.model.loaded())
        logger::trace("[vlm_client] model cache miss — reloading (was: " + slot.model_path + ")");
    else
        logger::trace("[vlm_client] model cache cold — first load");

    logger::trace("[vlm_client] loading model:  " + model_path);
    logger::trace("[vlm_client] loading mmproj: " + mmproj_path);
    logger::trace("[vlm_client] n_gpu_layers="   + std::to_string(n_gpu_layers)
                  + " n_threads=" + std::to_string(n_threads)
                  + " n_ctx="     + std::to_string(n_ctx));

    const auto t0 = Clock::now();
    media::llama::VlmModel fresh;
    if (!fresh.load(model_path, mmproj_path, n_threads, n_ctx, n_gpu_layers)) {
        err = fresh.error().empty() ? "failed to load VLM" : fresh.error();
        logger::trace("[vlm_client] load FAILED: " + err);
        return nullptr;
    }
    const float load_ms = elapsed_ms_since(t0);
    logger::trace("[vlm_client] loaded in " + std::to_string(static_cast<int>(load_ms)) + " ms");

    slot.model_id     = model_id;
    slot.model_path   = model_path;
    slot.mmproj_path  = mmproj_path;
    slot.n_gpu_layers = n_gpu_layers;
    slot.active_calls = 1;
    slot.model        = std::move(fresh);
    use_active = true;
    return &slot.model;
}

void release_model_use()
{
    std::lock_guard<std::mutex> lock(cache_mutex());
    CachedVlm& slot = cache_slot();
    if (slot.active_calls > 0)
        --slot.active_calls;
}

struct ModelUse {
    bool active = false;
    ~ModelUse()
    {
        if (active)
            release_model_use();
    }
};

#endif // FEATURE_VLM

// ─────────────────────────────────────────────────────────────────────────────
// Catalog resolution
// ─────────────────────────────────────────────────────────────────────────────
bool resolve_model(const std::string& model_id,
                   const std::string& slot,
                   media::local_models::LocalModelEntry& out,
                   std::string& err)
{
    const std::string effective_slot = slot.empty() ? "vision" : slot;

    if (!model_id.empty()) {
        logger::trace("[vlm_client] resolve: explicit id=" + model_id);
        return media::local_models::resolve_llama_vlm_model(model_id, out, err);
    }

    logger::trace("[vlm_client] resolve: auto-pick slot=" + effective_slot);
    const auto models = media::local_models::discover_llama_vlm_models(false);
    logger::trace("[vlm_client] catalog: " + std::to_string(models.size()) + " VLM model(s) found");

    for (const auto& m : models) {
        if (!m.valid) continue;
        for (const auto& s : m.slots)
            if (s == effective_slot) {
                logger::trace("[vlm_client] selected: " + m.id + " (slot=" + s + ")");
                out = m;
                return true;
            }
    }
    // Widen to "vision" when requested slot had no match.
    if (effective_slot != "vision") {
        logger::trace("[vlm_client] no '" + effective_slot + "' slot — widening to 'vision'");
        for (const auto& m : models) {
            if (!m.valid) continue;
            for (const auto& s : m.slots)
                if (s == "vision") {
                    logger::trace("[vlm_client] selected (fallback): " + m.id);
                    out = m;
                    return true;
                }
        }
    }
    err = "no local VLM model found for slot '" + effective_slot + "' — "
          "add a VLM GGUF pair under models/ or create a model.json manifest";
    logger::trace("[vlm_client] resolve failed: " + err);
    return false;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

VlmResponse execute(const VlmRequest& req)
{
    VlmResponse res;

    if (req.image_paths.empty()) {
        res.error = "vlm_client::execute: image_paths is empty";
        return res;
    }

#if !defined(FEATURE_VLM) || !FEATURE_VLM
    res.error = "vlm_client::execute: this build was compiled without FEATURE_VLM";
    return res;
#else
    logger::trace("[vlm_client] execute: slot=" + (req.slot.empty() ? "vision" : req.slot)
                  + " images=" + std::to_string(req.image_paths.size())
                  + " model_id=" + (req.model_id.empty() ? "(auto)" : req.model_id));
    for (std::size_t i = 0; i < req.image_paths.size(); ++i)
        logger::trace("[vlm_client]   [" + std::to_string(i) + "] " + req.image_paths[i]);

    // Truncate prompt for trace (first 120 chars).
    const std::string prompt_preview = req.prompt.size() > 120
        ? req.prompt.substr(0, 120) + "…"
        : req.prompt;
    logger::trace("[vlm_client] prompt: " + prompt_preview);

    media::local_models::LocalModelEntry entry;
    std::string resolve_err;
    if (!resolve_model(req.model_id, req.slot, entry, resolve_err)) {
        res.error = resolve_err;
        return res;
    }

    res.model_id    = entry.id;
    res.model_path  = media::path::path_to_utf8(entry.model_path);
    res.mmproj_path = media::path::path_to_utf8(entry.mmproj_path);

    const int ngl      = req.n_gpu_layers < 0 ? n_gpu_layers_from_env() : req.n_gpu_layers;
    const std::string gpu_mode = (ngl == 0) ? "cpu" : (ngl < 0 ? "gpu-all" : "gpu-partial");
    logger::debug("[vlm_client] model=" + entry.id + " gpu_mode=" + gpu_mode
                  + " images=" + std::to_string(req.image_paths.size()));

    std::string load_err;
    ModelUse model_use;
    media::llama::VlmModel* model = model_for_paths(
        res.model_path, entry.id, res.mmproj_path, ngl, req.n_threads, req.n_ctx,
        model_use.active, load_err);
    if (!model) {
        res.error = "vlm_client: " + load_err;
        return res;
    }

    logger::trace("[vlm_client] starting inference"
                  + std::string(req.image_paths.size() > 1 ? " (multi-frame)" : "")
                  + " n_predict=" + std::to_string(req.n_predict));

    const auto t_infer = Clock::now();
    media::llama::VlmResult r;
    if (req.image_paths.size() == 1)
        r = model->generate(req.image_paths.front(), req.prompt, req.n_predict, req.stream_tokens);
    else
        r = model->generate_frames(req.image_paths, req.prompt, req.n_predict, req.stream_tokens);

    const float wall_ms = elapsed_ms_since(t_infer);

    if (r.ok) {
        logger::trace("[vlm_client] inference ok: elapsed_ms=" + std::to_string(static_cast<int>(r.elapsed_ms))
                      + " wall_ms=" + std::to_string(static_cast<int>(wall_ms))
                      + " text_chars=" + std::to_string(r.text.size()));
    } else {
        logger::trace("[vlm_client] inference FAILED: " + r.error);
    }

    if (r.ok) {
        std::string cleaned = clean_ocr_markup(r.text);
        if (cleaned != r.text) {
            logger::debug("[vlm_client] stripped grounding markup, chars "
                          + std::to_string(r.text.size()) + " -> "
                          + std::to_string(cleaned.size()));
            r.text = std::move(cleaned);
        }
    }

    res.ok           = r.ok;
    res.text         = std::move(r.text);
    res.error        = std::move(r.error);
    res.elapsed_ms   = r.elapsed_ms;
    res.n_gpu_layers = ngl;
    return res;
#endif
}

nlohmann::json loaded_models_json()
{
#if !defined(FEATURE_VLM) || !FEATURE_VLM
    return nlohmann::json::array();
#else
    std::lock_guard<std::mutex> lock(cache_mutex());
    const CachedVlm& slot = cache_slot();
    if (!slot.model.loaded())
        return nlohmann::json::array();
    return nlohmann::json::array({
        {
            {"key", "llama:vlm"},
            {"kind", "llama-vlm"},
            {"name", slot.model_id.empty() ? media::path::path_to_utf8(media::path::path_from_utf8(slot.model_path).filename()) : slot.model_id},
            {"path", slot.model_path},
            {"mmprojPath", slot.mmproj_path},
            {"status", slot.active_calls > 0 ? "busy" : "loaded"},
            {"activeCalls", slot.active_calls},
            {"nGpuLayers", slot.n_gpu_layers},
            {"stopSupported", true},
        }
    });
#endif
}

bool unload_loaded_model(std::string_view key, std::string& err)
{
#if !defined(FEATURE_VLM) || !FEATURE_VLM
    err = "VLM support is disabled in this build";
    return false;
#else
    if (!key.empty() && key != "llama:vlm") {
        err = "not a llama VLM model key";
        return false;
    }
    std::lock_guard<std::mutex> lock(cache_mutex());
    CachedVlm& slot = cache_slot();
    if (!slot.model.loaded()) {
        err = "no cached VLM model loaded";
        return false;
    }
    if (slot.active_calls > 0) {
        err = "cached VLM model is busy";
        return false;
    }
    slot = CachedVlm{};
    return true;
#endif
}

bool is_vlm_available(const std::string& slot)
{
#if !defined(FEATURE_VLM) || !FEATURE_VLM
    (void)slot;
    return false;
#else
    const std::string effective_slot = slot.empty() ? "vision" : slot;
    const auto models = media::local_models::discover_llama_vlm_models(false);
    for (const auto& m : models) {
        if (!m.valid) continue;
        for (const auto& s : m.slots)
            if (s == effective_slot) return true;
    }
    return false;
#endif
}

} // namespace media::llm::vlm_client

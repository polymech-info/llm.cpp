#pragma once
//
// Library function for local VLM inference via llama.cpp (FEATURE_VLM).
// Mirrors the role llama_agent_client plays for text: a single call that
// resolves a model-catalog entry and runs VlmModel under the hood, keeping
// all llama/mtmd symbols out of callers.
//
// Typical use — image understanding:
//   VlmRequest req;
//   req.image_paths = { abs_path };
//   req.prompt      = "Describe this image.";
//   req.model_id    = "minicpm-v-4-5-q4-k-m";   // catalog id, or empty to auto-pick
//   auto res = vlm_client::execute(req);
//   if (res.ok) use(res.text);
//
// Typical use — OCR:
//   VlmRequest req;
//   req.image_paths = { document_path };
//   req.prompt      = "OCR:";
//   req.slot        = "ocr";                      // prefer OCR-slot model when model_id empty
//   auto res = vlm_client::execute(req);
//

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace media::llm::vlm_client {

// ─────────────────────────────────────────────────────────────────────────────

struct VlmRequest {
    /// Catalog id or absolute .gguf path for the language-model weights.
    /// When empty the first valid model whose slots include `slot` is used.
    std::string model_id;

    /// Preferred slot for auto-selection when model_id is empty.
    /// Typical values: "vision" (image understanding), "ocr" (text extraction),
    /// "video" (multi-frame / temporal). Falls back to "vision" when empty.
    std::string slot = "vision";

    /// One image path for single-image inference.
    /// Two or more paths for multi-frame (video) inference — VlmModel assigns
    /// M-RoPE positions automatically per frame for temporal models.
    std::vector<std::string> image_paths;

    /// Text prompt. For OCR use "OCR:" or "Extract all text verbatim.".
    std::string prompt = "Describe this image.";

    // ── Inference parameters ─────────────────────────────────────────────────
    int n_predict    = 2048;   ///< Maximum tokens to generate.
    int n_threads    = 4;      ///< CPU threads.
    int n_ctx        = 8192;   ///< KV context size.
    int n_gpu_layers = -1;     ///< GPU layers (-1 = all available).

    /// Stream generated tokens to stderr (for live progress). Off when silent runs are needed.
    bool stream_tokens = true;

    /// Optional cancellation predicate polled between tokens.
    std::function<bool()> cancel_check;
};

// ─────────────────────────────────────────────────────────────────────────────

struct VlmResponse {
    bool        ok         = false;
    std::string text;          ///< Generated text on success.
    std::string error;         ///< Diagnostic string on failure.
    float       elapsed_ms = 0.f;

    /// Resolved paths for diagnostics / caching.
    std::string model_id;
    std::string model_path;
    std::string mmproj_path;

    /// Effective GPU layers used (-1 = all, 0 = CPU-only, N = partial).
    int n_gpu_layers = -1;
};

// ─────────────────────────────────────────────────────────────────────────────

/// Run local VLM inference synchronously.
///
/// Resolution order for the model:
///   1. req.model_id (if non-empty) → looked up in the local VLM catalog.
///   2. First valid catalog entry whose slots include req.slot.
///   3. Error if nothing is found.
///
/// Returns VlmResponse::ok=false with a diagnostic in VlmResponse::error when:
///   - FEATURE_VLM is disabled at compile time.
///   - No suitable model is found in the catalog.
///   - VlmModel::load() fails.
///   - Inference fails.
///   - req.image_paths is empty.
VlmResponse execute(const VlmRequest& req);

/// Returns true if a local VLM is available for the given slot without running
/// inference (fast catalog probe — no model loading).
bool is_vlm_available(const std::string& slot = "vision");

/// Process-local diagnostics for the cached VLM/OCR GGUF model.
nlohmann::json loaded_models_json();

/// Unload the cached VLM/OCR GGUF model. Returns false when it is busy or absent.
bool unload_loaded_model(std::string_view key, std::string& err);

} // namespace media::llm::vlm_client

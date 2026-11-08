#pragma once
//
// Universal context compressor (ported from headroom's compression/universal.py).
//
// Pipeline:
//   1. detect content type (or honor an override)
//   2. build a structural preservation mask via the matching handler
//      (JSON today; text/code/etc. fall back to an all-compressible mask)
//   3. optionally union a high-entropy-word preservation mask
//   4. reduce only the non-structural spans with a deterministic, model-free
//      compress function (whitespace collapse + line dedup, then optional
//      head/tail truncation to hit a target ratio)
//
// Public entry: media::llm::compress::compress(content, opts).
// This is the surface the agent pre-send boundary (agent_factory.cpp) will call
// in a later slice; it has no OS / model / network dependencies.
//
// Requires: FEATURE_LLM_COMPRESSION=1.
//
#if !defined(FEATURE_LLM_COMPRESSION) || !FEATURE_LLM_COMPRESSION
#  error "llm/compressors/universal_compressor.hpp requires FEATURE_LLM_COMPRESSION=1"
#endif

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "content_detector.hpp"
#include "json_handler.hpp"
#include "text_handler.hpp"

namespace media::llm::compress {

struct CompressOptions {
    ContentType type                     = ContentType::Auto;  ///< Auto = detect; else force
    std::size_t min_content_length       = 100;                ///< below this, pass through untouched
    bool        use_entropy_preservation = true;
    double      entropy_threshold        = 0.85;
    std::size_t min_span_to_compress     = 50;                 ///< only reduce non-structural spans larger than this
    bool        enable_truncation        = true;               ///< apply head/tail truncation after lossless reduction
    double      compression_ratio_target = 0.3;                ///< truncation target (fraction of span length)

    JsonHandlerOptions json{};
    TextReduceOptions  text{};
};

struct CompressionResult {
    std::string compressed;
    double      compression_ratio    = 1.0;   ///< output_bytes / input_bytes
    std::size_t input_bytes          = 0;
    std::size_t output_bytes         = 0;
    std::size_t tokens_before        = 0;     ///< ~bytes/4 estimate
    std::size_t tokens_after         = 0;
    ContentType content_type         = ContentType::Unknown;
    double      detection_confidence = 0.0;
    std::string handler_used         = "none";
    double      preservation_ratio   = 1.0;
    bool        skipped              = false;
    std::string note;
};

/// Compress `content` with structure preservation. Deterministic and
/// thread-safe (no shared state) UNLESS a text-model reducer is registered
/// (see set_text_model_reducer), in which case text-like content is delegated
/// to that model with a deterministic fallback.
CompressionResult compress(std::string_view content, const CompressOptions& opts = {});

/// ~bytes/4 token estimate (matches headroom's heuristic).
std::size_t estimate_tokens(std::string_view text) noexcept;

/// Optional model-backed reducer for prose-like content (Text/Markdown/Log).
/// Given the full content and a keep-ratio target (<=0 → "model decides", e.g.
/// Kompress's score-threshold mode), returns reduced text or std::nullopt to
/// fall back to the deterministic structural pipeline. Registered once at
/// startup from the llama/ONNX layer; reads in compress() are serialized.
using TextModelReducer = std::function<std::optional<std::string>(std::string_view content, double target_ratio)>;

/// Install (or clear, with nullptr) the process-wide text-model reducer.
void set_text_model_reducer(TextModelReducer reducer);

/// True if a text-model reducer is currently installed.
bool has_text_model_reducer();

/// Copy of the installed reducer (empty if none).
TextModelReducer text_model_reducer();

} // namespace media::llm::compress

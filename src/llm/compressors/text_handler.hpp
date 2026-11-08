#pragma once
//
// Deterministic, model-free text reduction used as the default compress
// function for non-structural spans.
//
// The headroom text path uses the ML "Kompress" ModernBERT scorer; wiring that
// ONNX model requires a ModernBERT byte-level BPE tokenizer that does not yet
// exist in this codebase, so it is deferred to a later slice. Until then this
// provides a fast, dependency-free reduction:
//   - reduce_text():     lossless-ish passes (collapse whitespace runs, drop
//                        blank-line runs, dedup consecutive identical lines).
//                        High-entropy identifiers contain no spaces and survive
//                        these passes unchanged.
//   - simple_truncate(): headroom's _simple_compress fallback (keep head + tail
//                        with a marker) to hit a target ratio when reduction
//                        alone is not enough.
//
// Requires: FEATURE_LLM_COMPRESSION=1.
//
#include <cstddef>
#include <string>
#include <string_view>

namespace media::llm::compress {

struct TextReduceOptions {
    bool collapse_whitespace      = true;   ///< collapse runs of spaces/tabs to one; drop blank-line runs
    bool dedup_consecutive_lines  = true;   ///< drop a line identical to the previous one
};

/// Lossless-ish reduction: whitespace collapse + consecutive-line dedup.
std::string reduce_text(std::string_view text, const TextReduceOptions& opts = {});

/// headroom-style truncation: keep the first ~2/3 and last ~1/3 of
/// `ratio_target * len` with a "...[compressed]..." marker. Returns the text
/// unchanged when it is already at or below the target length.
std::string simple_truncate(std::string_view text, double ratio_target);

} // namespace media::llm::compress

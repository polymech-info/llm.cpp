#pragma once
//
// Attention reduction layer (POC) — a general "scoring + selection" primitive.
//
//   Segmenter (content -> immutable spans)
//     -> Score stages   (entropy, embed, kompress)  write scores[channel]
//     -> Drop stages    (dedup, levels)             set keep=false / flags
//     -> Protect stage  (paths/URLs/errors/hashes)  set protected=true
//     -> Blend          (weighted sum of channels)  -> score_final
//     -> Budget select  (topk / threshold / ratio)  protected never dropped
//     -> Restore original order (stable_order)
//     -> Render + stats (+ reason per unit)
//
// This module is intentionally dependency-light: it lives in pm-media under
// FEATURE_LLM_COMPRESSION and has NO llama.cpp / ONNX dependency. Model-backed
// score stages (embed, kompress) read callbacks from StageContext that the
// caller supplies (the CLI links llama+ONNX and provides them); when a callback
// is absent the stage is a no-op, so the deterministic path always runs.
//
// JSON / code do NOT go through attention stages — reduce() routes them to the
// existing structural compressor (media::llm::compress) as a terminal path.
//
// Requires: FEATURE_LLM_COMPRESSION=1.
//
#if !defined(FEATURE_LLM_COMPRESSION) || !FEATURE_LLM_COMPRESSION
#  error "llm/reduce/reduce.hpp requires FEATURE_LLM_COMPRESSION=1"
#endif

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "llm/compressors/content_detector.hpp"

namespace media::llm::reduce {

// ── Units ────────────────────────────────────────────────────────────────────

/// An immutable span of the source content plus per-stage scratch. Stages may
/// write scores / score_final / keep / protected_ / flags / reason — never
/// start/end (those are owned by the Segmenter).
struct Unit {
    std::size_t start = 0;
    std::size_t end   = 0;   ///< exclusive

    std::unordered_map<std::string, float> scores;  ///< channel -> score (embed/entropy/kompress)
    float         score_final = 0.0f;
    bool          keep        = true;   ///< selection decision
    bool          protected_  = false;  ///< never dropped by selection
    std::uint32_t flags       = 0;      ///< reserved (dup/low_level/…)
    std::string   reason;               ///< "embed:objective_match", "drop:dup", "protect:url", …

    std::size_t length() const noexcept { return end - start; }
    std::string_view text(std::string_view content) const { return content.substr(start, length()); }
};

enum class Granularity { Auto = 0, Whole, Lines, Paragraphs };

/// Segment content into immutable spans at the requested granularity.
std::vector<Unit> segment(std::string_view content, Granularity g);

// ── Budget ───────────────────────────────────────────────────────────────────

enum class BudgetKind { None = 0, Ratio, MaxTokens, Threshold };

struct Budget {
    BudgetKind kind  = BudgetKind::None;
    double     value = 0.0;   ///< ratio (0..1) | max tokens | score threshold
};

// ── Stage context + registry ───────────────────────────────────────────────

/// Optional model-backed scorers, supplied by the caller. Both return a vector
/// aligned 1:1 with `unit_texts`. Absent (null) => the corresponding stage is a
/// no-op.
using EmbedScoreFn =
    std::function<std::vector<float>(std::string_view objective,
                                     const std::vector<std::string_view>& unit_texts)>;
using UnitScoreFn =
    std::function<std::vector<float>(const std::vector<std::string_view>& unit_texts)>;

struct StageContext {
    std::string_view                content;
    std::optional<std::string_view> objective;
    Budget                          budget;
    std::vector<Unit>&              units;

    EmbedScoreFn embed_scores;    ///< for the "embed" stage (objective-conditioned)
    UnitScoreFn  kompress_scores; ///< for the "kompress" stage
};

/// One pipeline entry. `nums` carries numeric params (blend weights keyed by
/// channel; select budget keyed by "ratio"/"tokens"/"threshold"/"keep").
struct StageSpec {
    std::string                            name;
    std::unordered_map<std::string, double> nums;
};

using StageFn = std::function<void(StageContext&, const StageSpec&)>;

/// Look up a registered stage by name; returns nullptr if unknown.
const StageFn* find_stage(const std::string& name);

/// Expand a preset name ("logs"/"docs"/"prose"/"tool"/"memory") to its stage
/// list (with "protect" implicitly prepended). Returns empty if unknown.
std::vector<StageSpec> preset_stages(const std::string& preset);

// ── Request / result / entry ─────────────────────────────────────────────────

struct UnitReport {
    std::string                            text;
    std::unordered_map<std::string, float> scores;
    float                                  score_final = 0.0f;
    bool                                   keep        = true;
    bool                                   protected_  = false;
    std::string                            reason;
};

struct StageTiming {
    std::string name;
    double      ms = 0.0;
};

struct ReduceRequest {
    std::string_view                content;
    std::optional<std::string_view> objective;
    compress::ContentType           kind   = compress::ContentType::Auto;  ///< Auto = detect
    Granularity                     unit   = Granularity::Auto;
    Budget                          budget;
    std::vector<StageSpec>          stages;
    bool                            stable_order = true;   ///< render kept units in original order
    bool                            drop_markers = false;  ///< emit "…[dropped N]…" between gaps

    EmbedScoreFn embed_scores;
    UnitScoreFn  kompress_scores;
};

struct ReduceResult {
    std::string compressed;
    std::size_t input_bytes  = 0;
    std::size_t output_bytes = 0;
    std::size_t units_in     = 0;
    std::size_t units_out    = 0;
    double      ratio        = 1.0;          ///< output_bytes / input_bytes
    bool        structural   = false;        ///< took the JSON/code terminal path
    compress::ContentType kind = compress::ContentType::Unknown;
    std::string note;
    std::vector<UnitReport>  units;
    std::vector<StageTiming> timings;
    bool        ok = true;
};

/// Run the reduction pipeline. JSON/code route to the structural terminal path.
ReduceResult reduce(const ReduceRequest& req);

/// Convenience: objective-conditioned reduction with a default docs pipeline.
ReduceResult attention_reduce(std::string_view content,
                              std::optional<std::string_view> objective,
                              Budget budget,
                              EmbedScoreFn embed_scores = {});

} // namespace media::llm::reduce

#pragma once
//
// Structure mask system for context compression (ported from headroom's
// compression/masks.py).
//
// A StructureMask is a byte-aligned boolean array over the original content.
// mask[i] == true  => "preserve" byte i (structural / navigational / high-info)
// mask[i] == false => "compressible" byte i (may be reduced by the compress fn)
//
// The mask is content-agnostic: handlers (JSON, text, …) decide which bytes are
// structural; the universal compressor turns the mask into contiguous spans and
// only reduces the non-structural ones.
//
// Requires: FEATURE_LLM_COMPRESSION=1.
//
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace media::llm::compress {

/// A contiguous run of equal mask values.
struct MaskSpan {
    std::size_t start          = 0;
    std::size_t end            = 0;      ///< exclusive
    bool        is_structural  = false;

    std::size_t length() const noexcept { return end - start; }
};

/// Byte-aligned preserve/compressible mask.
class StructureMask {
public:
    StructureMask() = default;
    explicit StructureMask(std::vector<bool> mask) : mask_(std::move(mask)) {}

    std::size_t size()  const noexcept { return mask_.size(); }
    bool        empty() const noexcept { return mask_.empty(); }

    bool at(std::size_t i)        const { return mask_[i]; }
    void set(std::size_t i, bool v)     { mask_[i] = v; }

    const std::vector<bool>& bits() const noexcept { return mask_; }

    std::size_t structural_count()   const noexcept;
    std::size_t compressible_count() const noexcept;
    double      preservation_ratio() const noexcept;

    /// Preserve if EITHER mask preserves. Sizes must match; on mismatch the
    /// shorter length wins (defensive — callers align to the same content).
    StructureMask union_with(const StructureMask& other) const;

    static StructureMask empty_mask(std::size_t n) { return StructureMask(std::vector<bool>(n, false)); }
    static StructureMask full_mask(std::size_t n)  { return StructureMask(std::vector<bool>(n, true)); }

private:
    std::vector<bool> mask_;
};

/// Decompose a mask into contiguous structural / non-structural spans.
std::vector<MaskSpan> mask_to_spans(const StructureMask& mask);

/// Entropy-based preservation signal. High normalized Shannon entropy
/// (UUIDs, hashes, API keys) => preserve, because they are information-dense
/// identifiers that a token compressor could mangle.
struct EntropyScore {
    double value          = 0.0;    ///< normalized 0..1
    bool   should_preserve = false;

    static EntropyScore compute(std::string_view text, double threshold = 0.85);
};

/// Build a mask that preserves the byte spans of high-entropy, whitespace-
/// delimited words.
///
/// NOTE (deviation from headroom): headroom's compute_entropy_mask runs over
/// character-level tokens, which makes every token shorter than
/// `min_token_length` and the resulting mask an effective no-op in the
/// universal pipeline. We instead score whitespace-delimited words and map the
/// preserved words back to their byte spans, which matches the documented
/// intent ("UUIDs / hashes preserved") and makes entropy preservation actually
/// take effect for plain-text content.
StructureMask compute_entropy_mask(std::string_view content,
                                   double           threshold        = 0.85,
                                   std::size_t      min_token_length = 8);

} // namespace media::llm::compress

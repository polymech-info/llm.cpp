#include "structure_mask.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace media::llm::compress {

std::size_t StructureMask::structural_count() const noexcept
{
    std::size_t n = 0;
    for (bool b : mask_) {
        if (b) ++n;
    }
    return n;
}

std::size_t StructureMask::compressible_count() const noexcept
{
    return mask_.size() - structural_count();
}

double StructureMask::preservation_ratio() const noexcept
{
    if (mask_.empty()) return 0.0;
    return static_cast<double>(structural_count()) / static_cast<double>(mask_.size());
}

StructureMask StructureMask::union_with(const StructureMask& other) const
{
    const std::size_t n = std::min(mask_.size(), other.mask_.size());
    std::vector<bool> out(mask_.size(), false);
    for (std::size_t i = 0; i < mask_.size(); ++i) {
        out[i] = mask_[i] || (i < n && other.mask_[i]);
    }
    return StructureMask(std::move(out));
}

std::vector<MaskSpan> mask_to_spans(const StructureMask& mask)
{
    std::vector<MaskSpan> spans;
    if (mask.empty()) return spans;

    std::size_t current_start      = 0;
    bool        current_structural = mask.at(0);

    for (std::size_t i = 1; i < mask.size(); ++i) {
        const bool is_structural = mask.at(i);
        if (is_structural != current_structural) {
            spans.push_back(MaskSpan{current_start, i, current_structural});
            current_start      = i;
            current_structural = is_structural;
        }
    }
    spans.push_back(MaskSpan{current_start, mask.size(), current_structural});
    return spans;
}

EntropyScore EntropyScore::compute(std::string_view text, double threshold)
{
    if (text.empty()) {
        return EntropyScore{0.0, false};
    }

    // Byte-frequency histogram (matches Python char counting for ASCII; for
    // multibyte UTF-8 it counts bytes, which is acceptable for the identifier
    // heuristic this drives).
    std::array<std::size_t, 256> counts{};
    counts.fill(0);
    for (unsigned char c : text) {
        ++counts[c];
    }

    const double total = static_cast<double>(text.size());
    double       entropy = 0.0;
    std::size_t  distinct = 0;
    for (std::size_t count : counts) {
        if (count > 0) {
            ++distinct;
            const double p = static_cast<double>(count) / total;
            entropy -= p * std::log2(p);
        }
    }

    const double max_entropy = distinct > 1 ? std::log2(static_cast<double>(distinct)) : 1.0;
    const double normalized  = max_entropy > 0.0 ? entropy / max_entropy : 0.0;

    return EntropyScore{normalized, normalized >= threshold};
}

namespace {

constexpr bool is_ws(unsigned char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

} // namespace

StructureMask compute_entropy_mask(std::string_view content,
                                   double           threshold,
                                   std::size_t      min_token_length)
{
    std::vector<bool> mask(content.size(), false);

    std::size_t i = 0;
    const std::size_t n = content.size();
    while (i < n) {
        // skip whitespace
        if (is_ws(static_cast<unsigned char>(content[i]))) {
            ++i;
            continue;
        }
        // word run [start, j)
        const std::size_t start = i;
        while (i < n && !is_ws(static_cast<unsigned char>(content[i]))) {
            ++i;
        }
        const std::size_t end = i;
        const std::size_t len = end - start;
        if (len < min_token_length) {
            continue;
        }
        const std::string_view word = content.substr(start, len);
        if (EntropyScore::compute(word, threshold).should_preserve) {
            for (std::size_t k = start; k < end; ++k) {
                mask[k] = true;
            }
        }
    }

    return StructureMask(std::move(mask));
}

} // namespace media::llm::compress

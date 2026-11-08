#pragma once

#include "llm/huggingface/hf_gguf_header.hpp"

#include <cstdint>

namespace pm::hf {

struct VramEstimate {
    std::uint64_t weights_bytes     = 0;
    std::uint64_t kv_cache_bytes_est = 0;
    std::uint64_t total_bytes_est   = 0;
    double        total_gb_est      = 0.0;
    std::uint32_t context_assumed   = 8192;
    bool          heuristic         = false;
};

VramEstimate estimate_vram(const GgufHeaderMeta& header, std::uint64_t weights_bytes, std::uint32_t context = 8192);

VramEstimate estimate_vram_heuristic(std::uint64_t weights_bytes, std::uint32_t context = 8192);

} // namespace pm::hf

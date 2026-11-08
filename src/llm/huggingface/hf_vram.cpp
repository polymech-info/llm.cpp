#include "llm/huggingface/hf_vram.hpp"

#include "llm/huggingface/hf_quant.hpp"

namespace pm::hf {

VramEstimate estimate_vram_heuristic(std::uint64_t weights_bytes, std::uint32_t context)
{
    VramEstimate out;
    out.weights_bytes   = weights_bytes;
    out.context_assumed = context;
    out.heuristic       = true;
    const double ctx_scale = static_cast<double>(context) / 8192.0;
    out.kv_cache_bytes_est = static_cast<std::uint64_t>(weights_bytes * 0.18 * ctx_scale);
    out.total_bytes_est    = out.weights_bytes + out.kv_cache_bytes_est;
    out.total_gb_est       = bytes_to_decimal_gb(out.total_bytes_est);
    return out;
}

VramEstimate estimate_vram(const GgufHeaderMeta& header, std::uint64_t weights_bytes, std::uint32_t context)
{
    VramEstimate out;
    out.weights_bytes   = weights_bytes;
    out.context_assumed = context;

    if (header.block_count > 0 && header.n_kv_heads > 0 && header.kv_key_length > 0 && header.kv_value_length > 0) {
        const std::uint64_t per_token =
            2ULL * header.block_count * header.n_kv_heads * (header.kv_key_length + header.kv_value_length);
        out.kv_cache_bytes_est = per_token * context;
    } else if (header.block_count > 0 && header.n_kv_heads > 0 && header.embedding_length > 0 && header.n_heads > 0) {
        const std::uint64_t head_dim = header.embedding_length / header.n_heads;
        out.kv_cache_bytes_est = 4ULL * header.block_count * context * header.n_kv_heads * head_dim;
    } else {
        return estimate_vram_heuristic(weights_bytes, context);
    }

    out.total_bytes_est = out.weights_bytes + out.kv_cache_bytes_est;
    out.total_gb_est    = bytes_to_decimal_gb(out.total_bytes_est);
    return out;
}

} // namespace pm::hf

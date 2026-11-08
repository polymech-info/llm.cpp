#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace pm::hf {

/// Parsed GGUF header KV metadata (no tensor load). Mirrors Studio header reads.
struct GgufHeaderMeta {
    bool valid = false;

    std::string architecture;
    std::string general_type;
    std::string basename;
    std::string organization;
    std::string base_model_repo_url;

    std::uint64_t context_length   = 0;
    std::uint32_t block_count      = 0;
    std::uint32_t n_kv_heads       = 0;
    std::uint32_t n_heads          = 0;
    std::uint32_t embedding_length = 0;
    std::uint32_t kv_key_length    = 0;
    std::uint32_t kv_value_length  = 0;
    std::uint64_t kv_lora_rank     = 0;
    std::uint32_t sliding_window   = 0;

    bool          has_chat_template    = false;
    std::size_t   chat_template_chars  = 0;

    bool        is_mmproj = false;
    std::string error;
};

GgufHeaderMeta read_gguf_header(const std::filesystem::path& path);

} // namespace pm::hf

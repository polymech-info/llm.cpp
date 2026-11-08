#pragma once

#include "llm/huggingface/hf_gguf_header.hpp"
#include "llm/huggingface/hf_types.hpp"

namespace pm::hf {

void enrich_variant_info(VariantInfo& variant, const GgufHeaderMeta* local_header = nullptr);

void enrich_probe_result(ProbeResult& probe, const std::vector<RepoFileInfo>& repo_files);

GgufHeaderMeta read_header_for_variant_dir(const std::filesystem::path& variant_dir);

} // namespace pm::hf

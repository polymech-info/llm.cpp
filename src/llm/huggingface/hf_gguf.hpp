#pragma once

#include "llm/huggingface/hf_types.hpp"

#include <string>
#include <vector>

namespace pm::hf {

std::vector<std::string> list_gguf_variants(const std::vector<RepoFileInfo>& repo_files);

std::vector<std::string> pick_gguf_filenames(const std::vector<RepoFileInfo>& repo_files,
                                             const std::string& variant,
                                             std::string* err_out = nullptr);

std::optional<VariantInfo> build_variant_info(const std::vector<RepoFileInfo>& repo_files,
                                              const std::string& variant,
                                              std::string* err_out = nullptr);

std::vector<VariantInfo> all_variant_infos(const std::vector<RepoFileInfo>& repo_files);

} // namespace pm::hf

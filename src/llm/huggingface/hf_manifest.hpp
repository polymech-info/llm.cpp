#pragma once

#include "llm/huggingface/hf_types.hpp"

#include <nlohmann/json.hpp>

namespace pm::hf {

nlohmann::json build_manifest(const ModelEntry& entry,
                              const std::filesystem::path& output_dir,
                              const std::string& variant,
                              bool hash_files = true);

std::filesystem::path write_manifest(const ModelEntry& entry,
                                     const std::filesystem::path& output_dir,
                                     const std::string& variant,
                                     bool hash_files = true);

} // namespace pm::hf

#pragma once

#include "llm/huggingface/hf_types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace pm::hf {

/// Built-in GGUF catalog (mirrors unsloth/tanit/catalog/models.yaml).
std::vector<ModelEntry> catalog_entries();

/// Same root as VFS model downloads and ${MODELS_DIR} (@ref media::settings::get_models_dir).
std::filesystem::path default_models_root();

/// Explicit --output / localDir override, else @ref default_models_root.
std::filesystem::path resolve_models_root(const std::string& override_dir = {});

std::filesystem::path resolve_output_dir(const std::filesystem::path& output_root,
                                         const ModelEntry& entry,
                                         const std::string& variant);

} // namespace pm::hf

#pragma once

#include "llm/huggingface/hf_types.hpp"

#include <vector>

namespace pm::hf {

RepoCapabilities detect_repo_capabilities(const std::vector<RepoFileInfo>& repo_files);

} // namespace pm::hf

#pragma once

#include "llm/huggingface/hf_types.hpp"

#include <optional>
#include <string>

namespace pm::hf {

enum class RefKind {
    Invalid,
    Repo,
    Org,
};

struct ParsedRef {
    RefKind     kind  = RefKind::Invalid;
    std::string repo_id; ///< org/model
    std::string org;     ///< organization/user slug
    std::string raw;
};

/// Accept aliases, org/model, HF URLs, and bare org slugs (for `hg org`).
ParsedRef parse_hf_ref(std::string input);

bool looks_like_repo_id(const std::string& s);

ModelEntry entry_from_repo_id(const std::string& repo_id);

/// Catalog alias, repo id, or HF model URL → ModelEntry. Orgs return nullopt.
std::optional<ModelEntry> resolve_model_ref(const std::string& input);

} // namespace pm::hf

#pragma once

#include "polymech_export.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace media::llm::retrieval {

using ScoredIndex = std::pair<std::size_t, float>;

/// BM25 ranks normalized text rows. Rows with no query overlap are omitted.
POLYMECH_API std::vector<ScoredIndex> bm25_rank(
    std::string_view query,
    const std::vector<std::string>& documents);

/// Convert descending scored rows to a bounded rank list.
POLYMECH_API std::vector<std::size_t> ranks_from_scores(
    const std::vector<ScoredIndex>& scored,
    std::size_t pool_limit);

/// Reciprocal-rank fusion over arbitrary retriever rank lists.
POLYMECH_API std::vector<std::size_t> rrf_fuse(
    const std::vector<std::vector<std::size_t>>& rank_lists,
    std::size_t candidate_count,
    std::size_t top_k,
    int rrf_k);

} // namespace media::llm::retrieval

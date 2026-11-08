#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace media::llm::browser_use {

#if defined(FEATURE_BROWSER_PAGE_MODEL) && FEATURE_BROWSER_PAGE_MODEL

/// Rank model.zones[] with BM25 against `query` (V4). Writes model.rankedZones
/// (top hits with zoneId/score/liveRef/name). Exact name/zoneId matches boosted.
void rank_page_model_zones(nlohmann::json& model, std::string_view query, std::size_t top_k = 5);

/// If envelope has data.model (or top-level model), rank it in place.
void enrich_envelope_page_model(nlohmann::json& envelope, std::string_view query);

#endif

} // namespace media::llm::browser_use

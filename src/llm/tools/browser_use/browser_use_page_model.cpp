#include "llm/tools/browser_use/browser_use_page_model.hpp"

#if defined(FEATURE_BROWSER_PAGE_MODEL) && FEATURE_BROWSER_PAGE_MODEL

#include "llm/agent_retrieval.hpp"
#include "string/string_utils.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace media::llm::browser_use {
namespace {

bool exact_boost(std::string_view query_l, const nlohmann::json& zone)
{
    const std::string name = media::strutil::lower_ascii(zone.value("name", std::string{}));
    const std::string zone_id = media::strutil::lower_ascii(zone.value("zoneId", std::string{}));
    const std::string test_id = zone.contains("durable") && zone["durable"].is_object()
        ? media::strutil::lower_ascii(zone["durable"].value("testId", std::string{}))
        : std::string{};
    if (!name.empty() && (query_l.find(name) != std::string::npos || name.find(query_l) != std::string::npos))
        return true;
    if (!zone_id.empty() && query_l.find(zone_id) != std::string::npos)
        return true;
    if (!test_id.empty() && query_l.find(test_id) != std::string::npos)
        return true;
    return false;
}

} // namespace

void rank_page_model_zones(nlohmann::json& model, std::string_view query, std::size_t top_k)
{
    if (!model.is_object() || query.empty())
        return;
    if (!model.contains("zones") || !model["zones"].is_array() || model["zones"].empty())
        return;

    const auto& zones = model["zones"];
    std::vector<std::string> docs;
    docs.reserve(zones.size());
    for (const auto& z : zones) {
        if (!z.is_object()) {
            docs.emplace_back();
            continue;
        }
        std::string doc = z.value("doc", std::string{});
        if (doc.empty()) {
            doc = z.value("name", std::string{}) + " "
                + z.value("kind", std::string{}) + " "
                + z.value("purpose", std::string{}) + " "
                + z.value("headingPath", std::string{});
        }
        docs.push_back(std::move(doc));
    }

    auto scored = retrieval::bm25_rank(query, docs);
    const std::string query_l = media::strutil::lower_ascii(std::string(query));

    // Exact / substring boosts for name / zoneId / testId.
    for (std::size_t i = 0; i < zones.size(); ++i) {
        if (!zones[i].is_object())
            continue;
        if (!exact_boost(query_l, zones[i]))
            continue;
        bool found = false;
        for (auto& s : scored) {
            if (s.first == i) {
                s.second += 5.f;
                found = true;
                break;
            }
        }
        if (!found)
            scored.emplace_back(i, 5.f);
    }
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    nlohmann::json ranked = nlohmann::json::array();
    const std::size_t limit = std::min(top_k, scored.size());
    for (std::size_t i = 0; i < limit; ++i) {
        const auto idx = scored[i].first;
        if (idx >= zones.size() || !zones[idx].is_object())
            continue;
        const auto& z = zones[idx];
        ranked.push_back({
            {"zoneId", z.value("zoneId", std::string{})},
            {"score", scored[i].second},
            {"liveRef", z.value("liveRef", std::string{})},
            {"name", z.value("name", std::string{})},
            {"kind", z.value("kind", std::string{})},
        });
    }
    if (!ranked.empty())
        model["rankedZones"] = std::move(ranked);
}

void enrich_envelope_page_model(nlohmann::json& envelope, std::string_view query)
{
    if (query.empty() || !envelope.is_object())
        return;
    nlohmann::json* model = nullptr;
    if (envelope.contains("data") && envelope["data"].is_object()
        && envelope["data"].contains("model") && envelope["data"]["model"].is_object()) {
        model = &envelope["data"]["model"];
    } else if (envelope.contains("model") && envelope["model"].is_object()) {
        model = &envelope["model"];
    }
    if (model)
        rank_page_model_zones(*model, query, 5);
}

} // namespace media::llm::browser_use

#endif // FEATURE_BROWSER_PAGE_MODEL

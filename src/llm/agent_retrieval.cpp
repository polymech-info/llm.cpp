#include "agent_retrieval.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace media::llm::retrieval {
namespace {

std::vector<std::string> tokenize(std::string_view text)
{
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        if (cur.size() >= 2) out.push_back(cur);
        cur.clear();
    };
    for (const unsigned char c : text) {
        if (std::isalnum(c))
            cur.push_back(static_cast<char>(std::tolower(c)));
        else
            flush();
    }
    flush();
    return out;
}

} // namespace

std::vector<ScoredIndex> bm25_rank(
    std::string_view query,
    const std::vector<std::string>& documents)
{
    const auto query_tokens = tokenize(query);
    if (query_tokens.empty() || documents.empty()) return {};

    std::vector<std::vector<std::string>> doc_tokens;
    std::unordered_map<std::string, int> df;
    doc_tokens.reserve(documents.size());
    std::size_t total_len = 0;
    for (const auto& document : documents) {
        auto tokens = tokenize(document);
        total_len += tokens.size();
        std::unordered_set<std::string> unique(tokens.begin(), tokens.end());
        for (const auto& token : unique) ++df[token];
        doc_tokens.push_back(std::move(tokens));
    }

    constexpr float k1 = 1.2f;
    constexpr float b = 0.75f;
    const float avgdl = static_cast<float>(total_len)
        / static_cast<float>(documents.size());
    const int count = static_cast<int>(documents.size());

    std::vector<ScoredIndex> out;
    for (std::size_t i = 0; i < doc_tokens.size(); ++i) {
        const auto& tokens = doc_tokens[i];
        std::unordered_map<std::string, int> tf;
        for (const auto& token : tokens) ++tf[token];
        float score = 0.f;
        for (const auto& query_token : query_tokens) {
            const auto dit = df.find(query_token);
            const auto tit = tf.find(query_token);
            if (dit == df.end() || tit == tf.end()) continue;
            const float f = static_cast<float>(tit->second);
            const float idf = std::log(
                1.f + (static_cast<float>(count - dit->second) + 0.5f)
                    / (static_cast<float>(dit->second) + 0.5f));
            const float dl = static_cast<float>(tokens.size());
            const float denom = f + k1 * (
                1.f - b + b * (dl / (avgdl <= 0.f ? 1.f : avgdl)));
            score += idf * (f * (k1 + 1.f) / (denom <= 0.f ? 1.f : denom));
        }
        if (score > 0.f) out.emplace_back(i, score);
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

std::vector<std::size_t> ranks_from_scores(
    const std::vector<ScoredIndex>& scored,
    std::size_t pool_limit)
{
    std::vector<std::size_t> out;
    out.reserve(std::min(pool_limit, scored.size()));
    for (std::size_t i = 0; i < scored.size() && i < pool_limit; ++i)
        out.push_back(scored[i].first);
    return out;
}

std::vector<std::size_t> rrf_fuse(
    const std::vector<std::vector<std::size_t>>& rank_lists,
    std::size_t candidate_count,
    std::size_t top_k,
    int rrf_k)
{
    std::unordered_map<std::size_t, float> fused;
    const float k = static_cast<float>(std::max(1, rrf_k));
    for (const auto& ranks : rank_lists) {
        for (std::size_t rank = 0; rank < ranks.size(); ++rank) {
            const std::size_t index = ranks[rank];
            if (index >= candidate_count) continue;
            fused[index] += 1.f / (k + static_cast<float>(rank + 1));
        }
    }
    std::vector<ScoredIndex> sorted;
    sorted.reserve(fused.size());
    for (const auto& [index, score] : fused)
        sorted.emplace_back(index, score);
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<std::size_t> out;
    for (const auto& [index, score] : sorted) {
        (void)score;
        if (out.size() >= top_k) break;
        out.push_back(index);
    }
    return out;
}

} // namespace media::llm::retrieval

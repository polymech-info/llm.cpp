#include "agent_memory_recall.hpp"

#include "agent.hpp"
#include "agent_memory.hpp"
#include "agent_retrieval.hpp"
#include "chat_session_store.hpp"
#include "core/settings_runtime.hpp"
#include "core/settings_store.hpp"
#include "llm/tools/memory/MemoryTools.hpp"

#include "logging/logger.h"

#include <fstream>

#if !defined(FEATURE_LLAMA)
#  define FEATURE_LLAMA 0
#endif
#if FEATURE_LLAMA
#  include "lib/llm/local_vector_store/VectorStore.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include "string/string_utils.hpp"

namespace media::llm::agent {
namespace {

using SMF  = pm::llm::StructuredMemoryFeature;
using SMFs = pm::llm::StructuredMemoryFeatures;

std::mutex& pending_mutex()
{
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, std::string>& pending_by_session()
{
    static std::unordered_map<std::string, std::string> map;
    return map;
}

std::filesystem::path pending_recall_path(const std::string& session_id)
{
    namespace fs = std::filesystem;
    return session_store_dir() / (sanitize_session_id(session_id) + ".pending_recall.txt");
}

bool persist_pending_recall(const std::string& session_id, const std::string& block)
{
    namespace fs = std::filesystem;
    const fs::path path = pending_recall_path(session_id);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out << block;
    return out.good();
}

void clear_pending_recall_file(const std::string& session_id)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove(pending_recall_path(session_id), ec);
}

std::optional<std::string> load_pending_recall_file(const std::string& session_id)
{
    namespace fs = std::filesystem;
    const fs::path path = pending_recall_path(session_id);
    std::error_code ec;
    if (!fs::exists(path, ec) || ec)
        return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string block = ss.str();
    if (block.empty())
        return std::nullopt;
    return block;
}

using media::strutil::lower_ascii;

void append_field(std::ostringstream& ss, std::string_view value)
{
    if (value.empty()) return;
    if (ss.tellp() > 0) ss << ' ';
    ss << value;
}

std::string json_keys_flat(const nlohmann::json& meta)
{
    if (!meta.is_object() || !meta.contains("json_keys") || !meta["json_keys"].is_array())
        return {};
    std::ostringstream ss;
    for (const auto& k : meta["json_keys"]) {
        if (k.is_string()) append_field(ss, k.get<std::string>());
    }
    return ss.str();
}

std::string candidate_inject_line(const MemoryRecallCandidate& c)
{
    if (!c.summary.empty())
        return c.item_id + ": " + c.summary;
    return c.item_id;
}

/// Item IDs already shown live: last-N turn_pairs (## Recent turns) plus the
/// Session Items window (artifacts only — turn_pairs no longer share that list).
std::unordered_set<std::string> recent_session_item_ids(const nlohmann::json& memory_state,
                                                        std::size_t max_items)
{
    std::unordered_set<std::string> out;
    if (!memory_state.is_object() || !memory_state.contains("items")
        || !memory_state["items"].is_object() || max_items == 0)
        return out;

    const auto& rows = memory_state["items"];
    std::vector<std::pair<std::string, const nlohmann::json*>> sorted;
    sorted.reserve(rows.size());
    for (auto it = rows.begin(); it != rows.end(); ++it) {
        if (!it.value().is_object()) continue;
        const std::string kind = it.value().value("kind", std::string{});
        const std::string role = it.value().value("role", std::string{});
        if (kind == "self_check" || role == "security_review" || role == "security_decision")
            continue;
        sorted.emplace_back(it.key(), &it.value());
    }
    auto ts = [](const nlohmann::json& j, const char* key) -> std::string {
        auto it = j.find(key);
        return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string{};
    };
    std::sort(sorted.begin(), sorted.end(), [&](const auto& a, const auto& b) {
        const std::string la = ts(*a.second, "last_used_at");
        const std::string lb = ts(*b.second, "last_used_at");
        const std::string ca = ts(*a.second, "created_at");
        const std::string cb = ts(*b.second, "created_at");
        const std::string ka = la.empty() ? ca : la;
        const std::string kb = lb.empty() ? cb : lb;
        if (ka.empty() != kb.empty()) return !ka.empty();
        return ka > kb;
    });
    std::size_t turns = 0;
    std::size_t items = 0;
    for (const auto& row : sorted) {
        const bool is_turn = row.second->value("role", std::string{}) == "turn_pair";
        if (is_turn) {
            if (turns >= pm::llm::k_recent_turns_inject_max)
                continue;
            ++turns;
        } else {
            if (items >= max_items)
                continue;
            ++items;
        }
        out.insert(row.first);
        if (turns >= pm::llm::k_recent_turns_inject_max && items >= max_items)
            break;
    }
    return out;
}

std::vector<MemoryRecallCandidate> suppress_session_item_overlap(
    const std::string& session_id,
    const nlohmann::json& memory_state,
    std::vector<MemoryRecallCandidate> hits,
    std::size_t& suppressed_out)
{
    suppressed_out = 0;
    const auto recent = recent_session_item_ids(memory_state, pm::llm::k_session_items_inject_max);
    if (recent.empty())
        return hits;

    std::vector<MemoryRecallCandidate> kept;
    kept.reserve(hits.size());
    for (auto& h : hits) {
        // Only suppress current-session rows that are already in the live window.
        // Cross-session / global hits are the point of recall.
        if (h.session_id == session_id && recent.count(h.item_id)) {
            ++suppressed_out;
            continue;
        }
        kept.push_back(std::move(h));
    }
    return kept;
}

void push_candidate(const std::string& session_id,
                  const std::string& item_id,
                  const nlohmann::json& item,
                  std::vector<MemoryRecallCandidate>& out)
{
    if (!item.is_object()) return;
    const std::string role = item.value("role", std::string{});
    if (role == "security_review" || role == "security_decision")
        return;

    MemoryRecallCandidate c;
    c.session_id  = session_id;
    c.item_id     = item_id;
    c.item        = item;
    // Resolve summary before search_text so default_memory `value` fields
    // (Buddy, codewords, …) participate in BM25 / dense indexing.
    c.summary     = item.value("summary", std::string{});
    if (c.summary.empty() && item.contains("metadata") && item["metadata"].is_object())
        c.summary = item["metadata"].value("user", std::string{});
    if (c.summary.empty() && item.contains("value")) {
        if (item["value"].is_string())
            c.summary = item["value"].get<std::string>();
        else
            c.summary = item["value"].dump();
    }
    nlohmann::json for_search = item;
    if (!c.summary.empty())
        for_search["summary"] = c.summary;
    c.search_text = memory_recall_normalize_search_text(for_search, session_id);
    c.inject_line = candidate_inject_line(c);
    if (c.search_text.empty()) return;
    out.push_back(std::move(c));
}

bool read_default_memory_document(nlohmann::json& out_doc)
{
    out_doc = nlohmann::json::object();
    return media::llm::path::detail::load_global_memory_document(out_doc);
}

void collect_default_memory_candidates(std::vector<MemoryRecallCandidate>& out,
                                       std::unordered_set<std::string>& seen)
{
    nlohmann::json doc;
    if (!read_default_memory_document(doc))
        return;

    const char* global = "global";
    if (doc.contains("memory_items") && doc["memory_items"].is_object()) {
        for (const auto& [key, item] : doc["memory_items"].items()) {
            if (!item.is_object()) continue;
            const std::string dedupe = std::string(global) + '\0' + key;
            if (!seen.insert(dedupe).second) continue;
            nlohmann::json row = item;
            if (!row.contains("id")) row["id"] = key;
            row["kind"] = row.value("kind", std::string{"preference"});
            row["role"] = row.value("role", std::string{"user_fact"});
            push_candidate(global, key, row, out);
        }
    } else if (doc.contains("memory_state") && doc["memory_state"].is_object()) {
        for (const auto& [key, value] : doc["memory_state"].items()) {
            const std::string dedupe = std::string(global) + '\0' + key;
            if (!seen.insert(dedupe).second) continue;
            nlohmann::json row = {
                {"id", key},
                {"kind", "preference"},
                {"role", "user_fact"},
                {"value", value},
                {"summary", value.is_string() ? value.get<std::string>() : value.dump()},
            };
            push_candidate(global, key, row, out);
        }
    }
}

#if FEATURE_LLAMA
void index_default_memory_entries(media::llm::VectorStore& store,
                                const std::string& embed_model_hint)
{
    nlohmann::json doc;
    if (!read_default_memory_document(doc))
        return;

    const char* global = "global";
    auto index_row = [&](const std::string& key, const nlohmann::json& item) {
        nlohmann::json row = item;
        if (!row.contains("id")) row["id"] = key;
        row["kind"] = row.value("kind", std::string{"preference"});
        row["role"] = row.value("role", std::string{"user_fact"});
        if (row.value("summary", std::string{}).empty() && row.contains("value")) {
            if (row["value"].is_string()) row["summary"] = row["value"].get<std::string>();
            else row["summary"] = row["value"].dump();
        }
        const std::string search_text = memory_recall_normalize_search_text(row, global);
        if (search_text.empty()) return;

        const std::string path_key = memory_recall_stable_path_key(global, key);
        media::llm::VStoreDoc vdoc;
        vdoc.source = "memory";
        vdoc.path   = path_key;
        vdoc.title  = key;
        vdoc.chunk  = row.value("summary", std::string{});
        if (vdoc.chunk.empty())
            vdoc.chunk = search_text.substr(0, std::min<std::size_t>(400, search_text.size()));
        vdoc.locator = {{"scope", "global"}, {"key", key}};
        (void)store.upsert(std::move(vdoc), search_text);
    };

    if (doc.contains("memory_items") && doc["memory_items"].is_object()) {
        for (const auto& [key, item] : doc["memory_items"].items())
            if (item.is_object()) index_row(key, item);
    } else if (doc.contains("memory_state") && doc["memory_state"].is_object()) {
        for (const auto& [key, value] : doc["memory_state"].items()) {
            nlohmann::json row = {
                {"id", key},
                {"value", value},
                {"summary", value.is_string() ? value.get<std::string>() : value.dump()},
            };
            index_row(key, row);
        }
    }
}
#endif

#if FEATURE_LLAMA
std::vector<std::pair<std::size_t, float>> dense_rank(
    std::string_view query,
    const std::vector<MemoryRecallCandidate>& candidates,
    const std::string& embed_model_hint,
    std::size_t top_n)
{
    std::vector<std::pair<std::size_t, float>> out;
    const auto dir = memory_recall_index_dir();
    std::string err;
    (void)media::llm::vector_store_with(dir, embed_model_hint,
        [&](media::llm::VectorStore& store) {
            const std::size_t k = top_n == 0 ? candidates.size() : top_n;
            const auto hits = store.search(query, k, "memory", {});
            if (hits.empty()) return;

            std::unordered_map<std::string, std::size_t> path_to_idx;
            path_to_idx.reserve(candidates.size());
            for (std::size_t i = 0; i < candidates.size(); ++i)
                path_to_idx[memory_recall_stable_path_key(candidates[i].session_id, candidates[i].item_id)] = i;

            for (const auto& hit : hits) {
                const auto it = path_to_idx.find(hit.doc.path);
                if (it == path_to_idx.end()) continue;
                out.emplace_back(it->second, hit.score);
            }
            std::sort(out.begin(), out.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
        }, &err);
    (void)err;
    return out;
}
#endif

std::size_t env_size_t(const char* name, std::size_t fallback)
{
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return fallback;
    try {
        const long long v = std::stoll(raw);
        if (v < 0) return fallback;
        return static_cast<std::size_t>(v);
    } catch (...) {
        return fallback;
    }
}

std::int64_t env_int64(const char* name, std::int64_t fallback)
{
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return fallback;
    try {
        return std::stoll(raw);
    } catch (...) {
        return fallback;
    }
}

} // namespace

void apply_richness(MemoryRecallSettings& s, std::string_view richness)
{
    if (richness == "concise") {
        s.top_k = 3;
        s.inject_chars = 800;
    } else if (richness == "rich") {
        s.top_k = 8;
        s.inject_chars = 2500;
    } else {
        s.top_k = pm::llm::k_recall_top_k;
        s.inject_chars = pm::llm::k_recall_inject_chars;
    }
}

MemoryRecallSettings MemoryRecallSettings::resolve()
{
    MemoryRecallSettings s;
    media::runtime_settings::AgentMemorySettings mem;
    std::string err;
    if (media::runtime_settings::load_agent_memory_settings(mem, err)) {
        apply_richness(s, mem.richness);
        s.dense_budget_ms = mem.dense_budget_ms;
    }
    // Env overrides settings (harness presets).
    if (std::getenv("PM_LLM_RECALL_TOP_K"))
        s.top_k = std::max<std::size_t>(1, env_size_t("PM_LLM_RECALL_TOP_K", s.top_k));
    if (std::getenv("PM_LLM_RECALL_INJECT_CHARS"))
        s.inject_chars = std::max<std::size_t>(200, env_size_t("PM_LLM_RECALL_INJECT_CHARS", s.inject_chars));
    if (std::getenv("PM_LLM_RECALL_POOL"))
        s.pool_override = env_size_t("PM_LLM_RECALL_POOL", 0);
    if (std::getenv("PM_LLM_RECALL_DENSE_BUDGET_MS"))
        s.dense_budget_ms = env_int64("PM_LLM_RECALL_DENSE_BUDGET_MS", s.dense_budget_ms);
    return s;
}

void apply_memory_recall_profile(pm::llm::StructuredMemoryFeatures& features,
                                 std::string_view profile)
{
    using SMF = pm::llm::StructuredMemoryFeature;
    constexpr auto k_recall_bits =
        static_cast<pm::llm::StructuredMemoryFeatures>(SMF::IndexMemoryEntries)
        | static_cast<pm::llm::StructuredMemoryFeatures>(SMF::HybridRecallSearch)
        | static_cast<pm::llm::StructuredMemoryFeatures>(SMF::InjectPendingRecall);
    features &= ~k_recall_bits;

    std::string p(profile);
    p = media::strutil::lower_ascii(std::move(p));
    if (p == "off")
        return;
    if (p == "lexical") {
        features |= static_cast<pm::llm::StructuredMemoryFeatures>(SMF::HybridRecallSearch)
                 |  static_cast<pm::llm::StructuredMemoryFeatures>(SMF::InjectPendingRecall);
        return;
    }
    if (p == "shadow") {
        features |= static_cast<pm::llm::StructuredMemoryFeatures>(SMF::IndexMemoryEntries)
                 |  static_cast<pm::llm::StructuredMemoryFeatures>(SMF::HybridRecallSearch);
        return;
    }
    // hybrid (default)
    features |= static_cast<pm::llm::StructuredMemoryFeatures>(SMF::IndexMemoryEntries)
             |  static_cast<pm::llm::StructuredMemoryFeatures>(SMF::HybridRecallSearch)
             |  static_cast<pm::llm::StructuredMemoryFeatures>(SMF::InjectPendingRecall);
}

void apply_agent_memory_settings_to_turn(Turn& turn)
{
    media::runtime_settings::AgentMemorySettings mem;
    std::string err;
    if (media::runtime_settings::load_agent_memory_settings(mem, err))
        apply_memory_recall_profile(turn.structured_memory_features, mem.recall_profile);
}

std::size_t MemoryRecallSettings::shortlist_for(std::size_t candidate_count) const
{
    if (candidate_count == 0) return 0;
    if (pool_override > 0)
        return std::min(candidate_count, pool_override);

    // Adaptive: keep all small corpora; otherwise floor at max(10*top_k, 100),
    // grow gently with sqrt(N), hard-cap at k_recall_shortlist_ceiling.
    const std::size_t floor_n = std::max(top_k * 10, pm::llm::k_recall_candidate_pool);
    if (candidate_count <= floor_n)
        return candidate_count;
    const auto grow = static_cast<std::size_t>(std::sqrt(static_cast<double>(candidate_count)));
    const std::size_t adaptive = floor_n + grow;
    return std::min(candidate_count,
                    std::min(pm::llm::k_recall_shortlist_ceiling, adaptive));
}

std::string memory_recall_normalize_search_text(const nlohmann::json& item,
                                                std::string_view session_id)
{
    std::ostringstream ss;
    if (!session_id.empty())
        append_field(ss, session_id);
    append_field(ss, item.value("id", std::string{}));
    append_field(ss, item.value("kind", std::string{}));
    append_field(ss, item.value("role", std::string{}));
    append_field(ss, item.value("summary", std::string{}));
    append_field(ss, item.value("content_type", std::string{}));

    if (item.contains("locator") && item["locator"].is_object()) {
        const auto& loc = item["locator"];
        append_field(ss, loc.value("path", std::string{}));
        append_field(ss, loc.value("url", std::string{}));
    }
    if (item.contains("origin") && item["origin"].is_object())
        append_field(ss, item["origin"].value("tool_name", std::string{}));
    if (item.contains("metadata") && item["metadata"].is_object()) {
        const auto& meta = item["metadata"];
        append_field(ss, json_keys_flat(meta));
        append_field(ss, meta.value("source_pattern", std::string{}));
    }
    return lower_ascii(ss.str());
}

std::string memory_recall_stable_path_key(std::string_view session_id,
                                          std::string_view item_id)
{
    return std::string("memory://") + std::string(session_id) + "/" + std::string(item_id);
}

std::filesystem::path memory_recall_index_dir()
{
    return media::settings::get_config_dir() / "agent-memory-index";
}

std::vector<MemoryRecallCandidate> memory_recall_collect_candidates(
    const std::string& session_id,
    const nlohmann::json& memory_state,
    bool include_cross_session)
{
    std::vector<MemoryRecallCandidate> out;
    std::unordered_set<std::string> seen;

    if (memory_state.is_object() && memory_state.contains("items") &&
        memory_state["items"].is_object()) {
        for (const auto& [id, item] : memory_state["items"].items()) {
            const std::string key = session_id + '\0' + id;
            if (!seen.insert(key).second) continue;
            push_candidate(session_id, id, item, out);
        }
    }

    collect_default_memory_candidates(out, seen);

    if (!include_cross_session)
        return out;

    std::string err;
    const nlohmann::json store = chat_sessions::query_store(err);
    if (!store.is_object() || !store.contains("memoryItems") || !store["memoryItems"].is_array())
        return out;

    for (const auto& row : store["memoryItems"]) {
        if (!row.is_object()) continue;
        const std::string sid = row.value("sessionId", std::string{});
        const std::string iid = row.value("id", std::string{});
        if (sid.empty() || iid.empty() || sid == session_id) continue;
        const std::string key = sid + '\0' + iid;
        if (!seen.insert(key).second) continue;
        push_candidate(sid, iid, row, out);
    }
    return out;
}

std::vector<MemoryRecallCandidate> memory_recall_rank(
    std::string_view query,
    const std::vector<MemoryRecallCandidate>& candidates,
    const MemoryRecallSettings& settings,
    SMFs features,
    const std::string& embed_model_hint,
    MemoryRecallTelemetry* telemetry)
{
    if (query.empty() || candidates.empty())
        return {};

    const std::size_t pool = settings.shortlist_for(candidates.size());
    if (telemetry) {
        telemetry->candidate_count = candidates.size();
        telemetry->pool_size = pool;
        telemetry->pool_policy = settings.pool_override > 0 ? "fixed" : "adaptive";
        telemetry->dense_budget_ms = settings.dense_budget_ms;
    }

    std::vector<std::string> search_documents;
    search_documents.reserve(candidates.size());
    for (const auto& candidate : candidates)
        search_documents.push_back(candidate.search_text);

    const auto t_bm25 = std::chrono::steady_clock::now();
    const auto bm25_scored = retrieval::bm25_rank(query, search_documents);
    if (telemetry) {
        telemetry->bm25_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_bm25).count();
    }

    std::vector<std::vector<std::size_t>> rank_lists;
    rank_lists.push_back(retrieval::ranks_from_scores(bm25_scored, pool));

#if FEATURE_LLAMA
    // Skip dense for one turn after an over-budget run (lexical fallback).
    static bool skip_dense_next = false;
    const bool want_dense = pm::llm::has_feature(features, SMF::IndexMemoryEntries)
        || pm::llm::has_feature(features, SMF::HybridRecallSearch);
    if (want_dense && skip_dense_next) {
        skip_dense_next = false;
        if (telemetry) {
            telemetry->dense_ran = false;
            telemetry->dense_over_budget = true;
        }
    } else if (want_dense) {
        const auto t_dense = std::chrono::steady_clock::now();
        const auto dense_scored = dense_rank(query, candidates, embed_model_hint, pool);
        const auto dense_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_dense).count();
        const bool over = settings.dense_budget_ms > 0 && dense_ms > settings.dense_budget_ms;
        if (telemetry) {
            telemetry->dense_ran = true;
            telemetry->dense_ms = dense_ms;
            telemetry->dense_over_budget = over;
        }
        if (over)
            skip_dense_next = true;
        rank_lists.push_back(retrieval::ranks_from_scores(dense_scored, pool));
    }
#else
    (void)embed_model_hint;
#endif

    const auto fused = retrieval::rrf_fuse(
        rank_lists, candidates.size(), settings.top_k, settings.rrf_k);
    std::vector<MemoryRecallCandidate> hits;
    hits.reserve(fused.size());
    for (std::size_t idx : fused)
        hits.push_back(candidates[idx]);
    if (telemetry)
        telemetry->ranked_hits = hits.size();
    return hits;
}

void memory_recall_index_session_items(
    const std::string& session_id,
    const nlohmann::json& memory_state,
    SMFs features,
    const std::string& embed_model_hint)
{
    if (!pm::llm::has_feature(features, SMF::IndexMemoryEntries))
        return;
#if !FEATURE_LLAMA
    (void)session_id;
    (void)memory_state;
    (void)embed_model_hint;
    return;
#else
    if (session_id.empty() || !memory_state.is_object() || !memory_state.contains("items"))
        return;
    if (!memory_state["items"].is_object())
        return;

    const auto dir = memory_recall_index_dir();
    std::string err;
    const bool opened = media::llm::vector_store_with(dir, embed_model_hint,
        [&](media::llm::VectorStore& store) {
            int indexed = 0;
            for (const auto& [item_id, item] : memory_state["items"].items()) {
                if (!item.is_object()) continue;
                const std::string search_text = memory_recall_normalize_search_text(item, session_id);
                if (search_text.empty()) continue;

                const std::string path_key = memory_recall_stable_path_key(session_id, item_id);

                media::llm::VStoreDoc doc;
                doc.source = "memory";
                doc.path   = path_key;
                doc.title  = item_id;
                doc.chunk  = item.value("summary", std::string{});
                if (doc.chunk.empty())
                    doc.chunk = search_text.substr(0, std::min<std::size_t>(400, search_text.size()));
                doc.locator = nlohmann::json{
                    {"session_id", session_id},
                    {"item_id", item_id},
                };
                if (item.contains("locator"))
                    doc.locator["item_locator"] = item["locator"];

                const uint64_t added = store.upsert(std::move(doc), search_text);
                if (!added) {
                    logger::warn(std::string("memory_recall_index: upsert failed for ") + path_key
                                 + ": " + store.error());
                    continue;
                }
                ++indexed;
            }

            if (indexed > 0 && !store.save())
                logger::warn(std::string("memory_recall_index: save failed: ") + store.error());

            index_default_memory_entries(store, embed_model_hint);
            if (!store.save())
                logger::warn(std::string("memory_recall_index: save after global index failed: ")
                             + store.error());
        }, &err);
    if (!opened)
        logger::warn(std::string("memory_recall_index: open failed: ") + err);
#endif
}

void memory_recall_remove_session_items(
    const std::string& session_id,
    const std::vector<std::string>& item_ids,
    const std::string& embed_model_hint)
{
#if !FEATURE_LLAMA
    (void)session_id;
    (void)item_ids;
    (void)embed_model_hint;
#else
    if (session_id.empty() || item_ids.empty())
        return;
    const auto dir = memory_recall_index_dir();
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || ec)
        return;
    std::string err;
    const bool opened = media::llm::vector_store_with(dir, embed_model_hint,
        [&](media::llm::VectorStore& store) {
            std::size_t removed = 0;
            for (const std::string& item_id : item_ids) {
                if (item_id.empty()) continue;
                removed += store.remove_path(
                    memory_recall_stable_path_key(session_id, item_id));
            }
            if (removed > 0 && !store.save())
                logger::warn(std::string("memory_recall_remove: save failed: ") + store.error());
        }, &err);
    if (!opened)
        logger::warn(std::string("memory_recall_remove: open failed: ") + err);
#endif
}

nlohmann::json MemoryRecallTelemetry::to_json() const
{
    return nlohmann::json{
        {"candidates", candidate_count},
        {"pool_size", pool_size},
        {"pool_policy", pool_policy},
        {"ranked_hits", ranked_hits},
        {"overlap_suppressed", overlap_suppressed},
        {"injected_hits", injected_hits},
        {"injected_bytes", injected_bytes},
        {"injected_item_ids", injected_item_ids},
        {"bm25_ms", bm25_ms},
        {"dense_ms", dense_ms},
        {"dense_budget_ms", dense_budget_ms},
        {"dense_ran", dense_ran},
        {"dense_over_budget", dense_over_budget},
        {"total_ms", total_ms},
        {"pending_enqueued", pending_enqueued},
    };
}

MemoryRecallTelemetry memory_recall_enqueue_pending(
    const std::string& session_id,
    std::string_view query,
    const nlohmann::json& memory_state,
    SMFs features,
    const std::string& embed_model_hint)
{
    MemoryRecallTelemetry tel;
    if (!pm::llm::has_feature(features, SMF::HybridRecallSearch) || session_id.empty())
        return tel;

    const auto settings = MemoryRecallSettings::resolve();
    const auto t0 = std::chrono::steady_clock::now();

    auto candidates = memory_recall_collect_candidates(session_id, memory_state, true);
    tel.candidate_count = candidates.size();
    if (candidates.empty()) {
        tel.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        logger::debug(std::string("memory_recall: session=") + session_id
                     + " candidates=0 (skip)");
        return tel;
    }

    auto hits = memory_recall_rank(query, candidates, settings,
                                   features, embed_model_hint, &tel);

    std::size_t suppressed = 0;
    hits = suppress_session_item_overlap(session_id, memory_state, std::move(hits), suppressed);
    tel.overlap_suppressed = suppressed;
    tel.injected_hits = hits.size();
    tel.injected_item_ids.clear();
    tel.injected_item_ids.reserve(hits.size());
    for (const auto& h : hits)
        tel.injected_item_ids.push_back(h.session_id + "/" + h.item_id);

    const std::string block = hits.empty()
        ? std::string{}
        : memory_recall_format_inject_block(hits, settings.inject_chars);
    tel.injected_bytes = block.size();

    if (!block.empty()) {
        if (!persist_pending_recall(session_id, block)) {
            logger::warn("memory_recall_enqueue_pending: failed to persist pending block for "
                         + session_id);
        } else {
            std::lock_guard<std::mutex> lk(pending_mutex());
            pending_by_session()[session_id] = block;
            tel.pending_enqueued = true;
        }
    }

    tel.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    logger::debug(std::string("memory_recall: session=") + session_id
                 + " candidates=" + std::to_string(tel.candidate_count)
                 + " pool=" + std::to_string(tel.pool_size)
                 + " policy=" + tel.pool_policy
                 + " ranked=" + std::to_string(tel.ranked_hits)
                 + " overlap_suppressed=" + std::to_string(tel.overlap_suppressed)
                 + " injected_hits=" + std::to_string(tel.injected_hits)
                 + " injected_bytes=" + std::to_string(tel.injected_bytes)
                 + " bm25_ms=" + std::to_string(tel.bm25_ms)
                 + " dense_ms=" + std::to_string(tel.dense_ms)
                 + " dense_over_budget=" + (tel.dense_over_budget ? "1" : "0")
                 + " dense_ran=" + (tel.dense_ran ? "1" : "0")
                 + " total_ms=" + std::to_string(tel.total_ms)
                 + " pending=" + (tel.pending_enqueued ? "1" : "0"));
    return tel;
}

std::optional<std::string> memory_recall_take_pending(const std::string& session_id)
{
    if (session_id.empty())
        return std::nullopt;

    {
        std::lock_guard<std::mutex> lk(pending_mutex());
        auto& map = pending_by_session();
        auto it = map.find(session_id);
        if (it != map.end() && !it->second.empty()) {
            std::string out = std::move(it->second);
            map.erase(it);
            clear_pending_recall_file(session_id);
            return out;
        }
    }

    auto disk = load_pending_recall_file(session_id);
    if (!disk || disk->empty())
        return std::nullopt;
    clear_pending_recall_file(session_id);
    return disk;
}

std::string memory_recall_format_inject_block(
    const std::vector<MemoryRecallCandidate>& hits,
    std::size_t chars_budget)
{
    if (hits.empty()) return {};

    std::ostringstream s;
    s << "\n## Recalled memory\n"
         "Query-conditioned snippets from prior session context (injected once):\n";

    std::size_t used = s.str().size();
    for (const auto& h : hits) {
        std::string line = "- ";
        if (!h.session_id.empty())
            line += "[" + h.session_id + "] ";
        line += h.inject_line;
        line += "\n";
        if (used + line.size() > chars_budget)
            break;
        s << line;
        used += line.size();
    }
    return s.str();
}

} // namespace media::llm::agent

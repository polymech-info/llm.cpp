#pragma once
//
// Hybrid memory recall — BM25 (+ optional dense via VectorStore) with RRF fusion,
// background indexing of session memory items, and pending inject for turn N+1.
//

#include "llm_constants.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace media::llm::agent {

/// Runtime recall budgets (settings + env overrides). Not a corpus-size cap.
struct MemoryRecallSettings {
    std::size_t  top_k             = pm::llm::k_recall_top_k;
    std::size_t  inject_chars      = pm::llm::k_recall_inject_chars;
    /// 0 = adaptive shortlist; >0 forces fixed pool (harness presets).
    std::size_t  pool_override     = 0;
    std::int64_t dense_budget_ms   = pm::llm::k_recall_dense_budget_ms;
    int          rrf_k             = pm::llm::k_recall_rrf_k;

    /// settings.json custom.agent_memory then PM_LLM_RECALL_* env.
    static MemoryRecallSettings resolve();
    /// Alias for resolve() (harness / legacy call sites).
    static MemoryRecallSettings from_env() { return resolve(); }

    /// BM25/dense shortlist size entering RRF.
    std::size_t shortlist_for(std::size_t candidate_count) const;
};

/// Apply recall profile bits (off|lexical|hybrid|shadow) onto a feature mask.
void apply_memory_recall_profile(pm::llm::StructuredMemoryFeatures& features,
                                 std::string_view profile);

/// Load custom.agent_memory.recall_profile into Turn feature bits.
/// Callers should apply PM_LLM_STRUCTURED_MEMORY_FEATURES / CLI masks afterward,
/// then re-apply PM_LLM_MEMORY_RECALL_PROFILE so env wins.
void apply_agent_memory_settings_to_turn(struct Turn& turn);

/// One retrievable memory row (session item or cross-session pool entry).
struct MemoryRecallCandidate {
    std::string session_id;
    std::string item_id;
    std::string search_text;
    std::string summary;
    std::string inject_line;
    nlohmann::json item = nlohmann::json::object();
};

/// Per-enqueue retrieval stats (logged + emitted as MemoryUpdate.recall).
struct MemoryRecallTelemetry {
    std::size_t candidate_count    = 0;
    std::size_t pool_size          = 0;
    std::size_t ranked_hits        = 0;
    std::size_t overlap_suppressed = 0;
    std::size_t injected_hits      = 0;
    std::size_t injected_bytes     = 0;
    std::int64_t bm25_ms           = 0;
    std::int64_t dense_ms          = 0;
    std::int64_t dense_budget_ms   = 0;
    std::int64_t total_ms          = 0;
    bool dense_ran                 = false;
    bool dense_over_budget         = false;
    bool pending_enqueued          = false;
    std::string pool_policy; // "adaptive" | "fixed"
    std::vector<std::string> injected_item_ids;

    nlohmann::json to_json() const;
};

/// Normalized lexical field for BM25 / indexing (path + role + summary + metadata).
std::string memory_recall_normalize_search_text(const nlohmann::json& item,
                                                std::string_view session_id = {});

/// Stable VectorStore path key for a session memory item.
std::string memory_recall_stable_path_key(std::string_view session_id,
                                          std::string_view item_id);

/// Directory for the agent-memory vector index ({config_dir}/agent-memory-index).
std::filesystem::path memory_recall_index_dir();

/// Collect candidate pool from current session memory_state + optional cross-session store.
std::vector<MemoryRecallCandidate> memory_recall_collect_candidates(
    const std::string& session_id,
    const nlohmann::json& memory_state,
    bool include_cross_session);

/// Rank + fuse; returns top hits (may be empty). Fills optional timing fields.
std::vector<MemoryRecallCandidate> memory_recall_rank(
    std::string_view query,
    const std::vector<MemoryRecallCandidate>& candidates,
    const MemoryRecallSettings& settings,
    pm::llm::StructuredMemoryFeatures features,
    const std::string& embed_model_hint = {},
    MemoryRecallTelemetry* telemetry = nullptr);

/// Upsert session items into the local memory index (no-op when IndexMemoryEntries off).
void memory_recall_index_session_items(
    const std::string& session_id,
    const nlohmann::json& memory_state,
    pm::llm::StructuredMemoryFeatures features,
    const std::string& embed_model_hint = {});

/// Remove evicted session-item rows from the local vector index. Canonical
/// session logs are untouched. No-op when local vector indexing is unavailable.
void memory_recall_remove_session_items(
    const std::string& session_id,
    const std::vector<std::string>& item_ids,
    const std::string& embed_model_hint = {});

/// Score query, suppress hits already in the recent Session Items window, and
/// store a pending inject block for the next turn (HybridRecallSearch).
MemoryRecallTelemetry memory_recall_enqueue_pending(
    const std::string& session_id,
    std::string_view query,
    const nlohmann::json& memory_state,
    pm::llm::StructuredMemoryFeatures features,
    const std::string& embed_model_hint = {});

/// Take and clear pending recall for session_id (InjectPendingRecall / on_prepare).
std::optional<std::string> memory_recall_take_pending(const std::string& session_id);

/// Render ## Recalled memory block (UTF-8 capped).
std::string memory_recall_format_inject_block(
    const std::vector<MemoryRecallCandidate>& hits,
    std::size_t chars_budget = pm::llm::k_recall_inject_chars);

} // namespace media::llm::agent

#pragma once

#include "agent.hpp"
#include "llm_constants.hpp"
#include "security/types.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace media::llm::agent {

/// Parse a `--memory-features` / PM_LLM_STRUCTURED_MEMORY_FEATURES value into
/// a feature mask. The input is a comma-separated list of tokens; each token
/// is one of:
///
///   * a decimal or `0xHEX` literal (bits ORed in as-is)
///   * a flag name (case-insensitive, underscores optional), e.g.
///     `CaptureWriteArtifacts`, `inject_items`, `emitmemoryupdate`
///   * `all` (= every bit set)
///   * `default` (= pm::llm::k_default_structured_memory_features)
///   * `none` (= 0)
///   * a `+name` / `-name` prefix to add or clear that flag relative to the
///     mask built so far (without a prefix, names are OR'd in).
///
/// Examples:
///   "0x7FF"
///   "CaptureWriteArtifacts,InjectItems"
///   "default,-ReplayTranscript"
///   "none,+InjectItems,+InjectRules"
///
/// Returns `std::nullopt` when the input is empty or no token parsed; callers
/// fall back to their own default (typically the Turn's existing mask).
std::optional<pm::llm::StructuredMemoryFeatures>
parse_structured_memory_features(std::string_view spec);

/// Apply the `PM_LLM_STRUCTURED_MEMORY_FEATURES` environment variable on top of
/// a fallback mask. When the variable is unset or unparseable, the fallback is
/// returned unchanged. Logs the resolved mask on apply / the offending spec on
/// failure. Shared by every Turn entry point (CLI, chat UI, scheduler) so the
/// same env override works regardless of how the agent was launched.
pm::llm::StructuredMemoryFeatures apply_structured_memory_features_env(
    pm::llm::StructuredMemoryFeatures fallback, const char* origin = "env");

/// Return the canonical name of a feature bit (matches the enum identifier).
/// Empty for unknown / multi-bit masks.
std::string_view structured_memory_feature_name(pm::llm::StructuredMemoryFeature f);

/// Infer compact session items/artifacts from a completed agent turn and merge
/// them into the persisted session memory. The full transcript remains the raw
/// evidence; memory receives summaries, locators, roles, and origin metadata.
///
/// `features` is a bitmask of pm::llm::StructuredMemoryFeature flags. When 0
/// the function returns an empty object without touching the store.
/// `context_files` is unused (explorer selection is not the conversation
/// subject; files the turn touched live on tool_calls). Kept for ABI.
/// `planner_meta`, when it carries intent/action/subject, is copied onto the
/// turn pair as `metadata.proposed` so the next prompt can show them.
nlohmann::json session_persist_artifacts_from_result(
    const std::string&              session_id,
    const std::string&              user_prompt,
    const Result&                   result,
    pm::llm::StructuredMemoryFeatures features    = pm::llm::k_default_structured_memory_features,
    const std::vector<std::string>& context_files = {},
    const std::string&              source_log_session_id = {},
    const std::string&              source_log_id = {},
    const std::string&              source_log_path = {},
    const nlohmann::json&           planner_meta = nlohmann::json::object());

/// Persist replaceable, bounded observations for high-value tool outcomes
/// (currently validation/build/install run commands). Stable IDs supersede
/// older outcomes instead of accumulating one item per call.
nlohmann::json session_persist_tool_checkpoints_from_result(
    const std::string& session_id,
    const Result& result,
    const std::string& source_log_session_id = {},
    const std::string& source_log_id = {},
    const std::string& source_log_path = {});

/// When a turn ends in failure or cancellation and at least one tool call
/// returned ok=false, write a kind=error_reflexion item to session memory so
/// future planner calls can see "previous attempt with X failed".
/// Returns the memory-update patch (same shape as session_persist_artifacts_from_result)
/// or an empty object when there is nothing to record.
/// Only runs when CaptureErrorReflexions is set in features.
nlohmann::json session_persist_error_reflexion(
    const std::string& session_id,
    const std::string& user_prompt,
    const Result&      result,
    pm::llm::StructuredMemoryFeatures features = pm::llm::k_default_structured_memory_features);

/// Persist a compact, harvestable security-review item into session memory.
/// Review payloads are sidecar facts for future embeddings, not file artifacts.
nlohmann::json session_persist_security_review(
    const std::string&  session_id,
    const std::string&  req_id,
    const nlohmann::json& consent_request,
    const nlohmann::json& review,
    const std::string&  router = {},
    const std::string&  model = {});

/// Persist the final user/security decision for a tool request into session
/// memory. This records the outcome even when no file artifact was produced.
nlohmann::json session_persist_security_decision(
    const std::string& session_id,
    const std::string& req_id,
    const pm::security::ToolRequest& request,
    const pm::security::PolicyDecision& decision);

/// Strip the raw `patch` field from a MemoryUpdate event payload when its JSON
/// serialization exceeds pm::llm::k_memory_update_log_chars. Counts and ids
/// remain; downstream UIs/logs still get a useful summary without ballooning.
nlohmann::json truncate_memory_update_for_log(nlohmann::json payload);

/// Reconcile mid-turn journal entries into session memory after a crash or
/// kill. Idempotent: skips tool_call_ids already listed under
/// memory_state.journal_recovery.recovered_tool_call_ids and paths that
/// already exist as session items. Returns a summary object with at least
/// { "recovered": N } or an empty object when nothing was promoted.
nlohmann::json session_recover_from_journal(
    const std::string& session_id,
    pm::llm::StructuredMemoryFeatures features = pm::llm::k_default_structured_memory_features);

} // namespace media::llm::agent

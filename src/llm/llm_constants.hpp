#pragma once
//
// Constants for the agent / LLM library — defaults and feature toggles that
// callers (CLI, UI, scheduler, library embedders) may override per Turn.
//
// Lives in src/llm because it is library-internal LLM policy, not app-wide.
// Pair with src/constants.hpp which holds product-wide defaults.
//

#include <cstddef>
#include <cstdint>

#ifndef FEATURE_LLM_FS_STRICT
#define FEATURE_LLM_FS_STRICT 0
#endif
#ifndef FEATURE_LLM_RUN_STRICT
#define FEATURE_LLM_RUN_STRICT 1
#endif

namespace pm::llm {

// ── Filesystem guard policy ───────────────────────────────────────────────────

/// When enabled, LLM filesystem guard hits are hard tool failures. When disabled,
/// batch/listing tools may skip policy-blocked paths and report skipped counts.
inline constexpr bool LLM_FS_GUARD_STRICT = FEATURE_LLM_FS_STRICT != 0;
inline constexpr bool k_fs_guard_strict = LLM_FS_GUARD_STRICT;

// ── Shell run policy ──────────────────────────────────────────────────────────

/// When enabled, the run tool treats validator warnings as hard failures.
/// Deny-tier validators and cwd filesystem guards are always enforced.
inline constexpr bool LLM_RUN_STRICT = FEATURE_LLM_RUN_STRICT != 0;

// ── Tanit service tools (service_* path-mode tools) ───────────────────────────

/// Max inline markdown for service_page_create / service_page_update before the
/// tool writes a temp file and calls the CMS page API.
inline constexpr std::size_t k_service_page_markdown_max_bytes = 2u * 1024u * 1024u;

/// Max VFS file body returned inline from service_files_get (text only).
inline constexpr std::size_t k_service_files_get_max_inline_bytes = 4u * 1024u * 1024u;

// ── Tool result budgets for frontier-model context ────────────────────────────

/// Max lines returned by one file_read window. Secondary structural bound.
/// Unwindowed reads of files longer than this (or heavier than
/// k_file_read_large_bytes) fail so the agent must locate + window.
inline constexpr int k_file_read_window_lines = 400;

/// Max bytes returned by one file_read window. An explicit source read gets
/// more room than a listing or process noise. 96 KiB is about 24k tokens,
/// roughly $0.24 at $10 / 1M input tokens.
inline constexpr std::size_t k_file_read_window_bytes = 96u * 1024u;

/// Byte threshold at which an unwindowed file_read is "large" and must be windowed.
inline constexpr std::size_t k_file_read_large_bytes = 48u * 1024u;

/// Inline budget for structured enumeration (file_glob, list_images).
/// Paths and repetitive metadata are low density.
inline constexpr std::size_t k_structured_list_inline_max_bytes = 24u * 1024u;

// ── file_compose ──────────────────────────────────────────────────────────────
// Large outputs are large because the filesystem copied existing bytes, not
// because the model embedded them in the tool call.

/// Maximum generated literal payload in one compose part.
inline constexpr std::size_t k_file_compose_literal_max_bytes = 64u * 1024u;

/// Maximum total model-generated literal data per compose call.
inline constexpr std::size_t k_file_compose_literal_total_max_bytes = 256u * 1024u;

/// Streaming copy buffer. This is not an LLM-context limit.
inline constexpr std::size_t k_file_compose_copy_buffer_bytes = 64u * 1024u;

/// Defensive maximum number of recipe parts.
inline constexpr std::size_t k_file_compose_max_parts = 256;

/// Defensive ceiling on the composed file. Not the write_file 16 MiB cap.
inline constexpr std::uintmax_t k_file_compose_max_output_bytes =
    1024ull * 1024ull * 1024ull;

static_assert(k_file_compose_literal_total_max_bytes >= k_file_compose_literal_max_bytes);
static_assert(k_file_compose_max_parts >= 1);
static_assert(k_file_compose_max_output_bytes > k_file_compose_literal_total_max_bytes);
static_assert(k_file_compose_copy_buffer_bytes >= 4096);

// ── Structured session memory — bitmask feature toggles ──────────────────────
//
// A Turn carries a bitmask of these flags so each caller can decide which
// structured-memory sub-features run. The default mask enables the full set.
//
//   StructuredMemoryFeature::CaptureWriteArtifacts
//       Promote successful write_file outputs to session items / artifacts.
//   StructuredMemoryFeature::CaptureMcpObservations
//       Record MCP tool results as `kind: tool_observation` items.
//   StructuredMemoryFeature::SynthesizeRules
//       Emit scoped instruction rules (use prior items, exact reply, …).
//   StructuredMemoryFeature::DedupOnWrite
//       Collapse duplicate items by stable id / locator before writing.
//   StructuredMemoryFeature::TruncateSummaries
//       Clamp item summaries to k_session_item_summary_chars.
//   StructuredMemoryFeature::EvictOldest
//       Drop oldest items once k_session_items_max is exceeded.
//   StructuredMemoryFeature::InjectItems
//       Render "## Session Items" into the agent system prompt.
//   StructuredMemoryFeature::InjectRules
//       Render "## Relevant Session Rules" into the agent system prompt.
//   StructuredMemoryFeature::InjectInPlanner
//       Append the items / rules block to the planner user message too.
//   StructuredMemoryFeature::EmitMemoryUpdate
//       Fire an Event::MemoryUpdate after a successful turn writes memory.
//   StructuredMemoryFeature::CaptureTurnPairs
//       After each successful turn, record a paired conversation item
//       (kind=conversation, role=turn_pair) carrying the user prompt, final
//       assistant text, and tool names. The last k_recent_turns_inject_max
//       pairs are rendered as "## Recent turns" (not as explorer-selection
//       context). Bridges chat continuity without the legacy transcript replay.
//   StructuredMemoryFeature::ReplayTranscript
//       Inject the "## Conversation History" block built from
//       turn.recent_events into the agent system prompt. Clearing this bit is
//       how callers retire raw transcript replay once structured memory is
//       judged rich enough on its own.
//   StructuredMemoryFeature::CaptureErrorReflexions
//       When a tool call returns ok=false AND the whole turn ends in failure or
//       cancellation, write a kind=error_reflexion item to session memory.
//       Fields: error_tool, error_message, user_prompt_summary, iterations.
//       Appears in the planner's session-items block on the next turn as a
//       routing hint ("previous attempt with X failed — prefer Y").
//   StructuredMemoryFeature::CaptureSelfCheck
//       After each turn, SelfCheckFeature writes a kind=self_check item with
//       structural quality signals: tool ok-rate, deflection detection,
//       completion_confidence. Recorded regardless of pass/fail so future
//       turns can see recent quality trends.
//   StructuredMemoryFeature::IndexMemoryEntries
//       Upsert session memory items into {config}/agent-memory-index (VectorStore).
//   StructuredMemoryFeature::HybridRecallSearch
//       BM25 (+ dense when indexed) + RRF after each turn; suppress hits already in
//       the recent Session Items window; write pending for next turn; emit
//       MemoryUpdate.recall telemetry (candidates, latency, overlap, bytes).
//   StructuredMemoryFeature::InjectPendingRecall
//       Inject the pending "## Recalled memory" block once on the following turn.
//
enum class StructuredMemoryFeature : std::uint32_t {
    None                   = 0,
    CaptureWriteArtifacts  = 1u << 0,
    CaptureMcpObservations = 1u << 1,
    SynthesizeRules        = 1u << 2,
    DedupOnWrite           = 1u << 3,
    TruncateSummaries      = 1u << 4,
    EvictOldest            = 1u << 5,
    InjectItems            = 1u << 6,
    InjectRules            = 1u << 7,
    InjectInPlanner        = 1u << 8,
    EmitMemoryUpdate       = 1u << 9,
    CaptureTurnPairs       = 1u << 10,
    ReplayTranscript       = 1u << 11,
    CaptureErrorReflexions = 1u << 12,
    /// Persist a kind=self_check item to session memory after SelfCheckFeature
    /// runs its structural quality gate. Recorded even when the check fails so
    /// future planner/agent turns can see recent quality trends.
    CaptureSelfCheck       = 1u << 13,
    /// Background upsert of session memory items into {config}/agent-memory-index.
    IndexMemoryEntries     = 1u << 14,
    /// After each turn, BM25 (+ dense when available) + RRF; overlap-suppress vs
    /// recent Session Items; write pending + emit MemoryUpdate.recall telemetry.
    HybridRecallSearch     = 1u << 15,
    /// on_prepare: inject pending recall block once (turn N scores → turn N+1 inject).
    InjectPendingRecall    = 1u << 16,
    All                    = 0xFFFFFFFFu,
};

/// Mask type used on Turn and at call sites.
using StructuredMemoryFeatures = std::uint32_t;

constexpr StructuredMemoryFeatures operator|(StructuredMemoryFeature a, StructuredMemoryFeature b) {
    return static_cast<StructuredMemoryFeatures>(a) | static_cast<StructuredMemoryFeatures>(b);
}
constexpr StructuredMemoryFeatures operator|(StructuredMemoryFeatures a, StructuredMemoryFeature b) {
    return a | static_cast<StructuredMemoryFeatures>(b);
}

constexpr bool has_feature(StructuredMemoryFeatures mask, StructuredMemoryFeature f) {
    return (mask & static_cast<StructuredMemoryFeatures>(f)) != 0;
}

/// Default mask: structured memory + hybrid recall on. `ReplayTranscript`
/// is intentionally *off* — turn_pair items, recent artifact locators, and
/// query-conditioned recall provide the useful context at lower prompt cost.
inline constexpr StructuredMemoryFeatures k_default_structured_memory_features =
      StructuredMemoryFeature::CaptureWriteArtifacts
    | StructuredMemoryFeature::CaptureMcpObservations
    | StructuredMemoryFeature::SynthesizeRules
    | StructuredMemoryFeature::DedupOnWrite
    | StructuredMemoryFeature::TruncateSummaries
    | StructuredMemoryFeature::EvictOldest
    | StructuredMemoryFeature::InjectItems
    | StructuredMemoryFeature::InjectRules
    | StructuredMemoryFeature::InjectInPlanner
    | StructuredMemoryFeature::EmitMemoryUpdate
    | StructuredMemoryFeature::CaptureTurnPairs
    | StructuredMemoryFeature::CaptureErrorReflexions
    | StructuredMemoryFeature::CaptureSelfCheck
    | StructuredMemoryFeature::IndexMemoryEntries
    | StructuredMemoryFeature::HybridRecallSearch
    | StructuredMemoryFeature::InjectPendingRecall;

// ── Structured session memory — size caps ────────────────────────────────────

/// Max session items (artifacts + tool observations) kept per session.
/// Eviction is oldest-first (by created_at) once this is exceeded.
inline constexpr std::size_t k_session_items_max          = 50;

/// Max active instruction rules per session. Retired rules (active=false) are
/// still allowed but never injected; they count toward this budget.
inline constexpr std::size_t k_session_rules_max          = 20;

/// Per-item summary truncation (UTF-8 chars).
inline constexpr std::size_t k_session_item_summary_chars = 400;

/// Total UTF-8 chars injected into the agent system prompt for "Session Items".
inline constexpr std::size_t k_session_items_inject_chars = 4000;

/// Maximum recent, user-relevant items injected into both planner and executor.
/// Security bookkeeping, self-check rows, and turn_pairs do not consume this
/// budget — turn_pairs go through ## Recent turns instead.
inline constexpr std::size_t k_session_items_inject_max   = 10;

/// Last N conversation turn_pairs injected as "## Recent turns" (oldest first).
/// The user line and the assistant line are each two visual lines. Tool calls
/// stay hints (name, status, one locator, recall handle). Bodies stay in the
/// agent log and come back through memory_find.
inline constexpr std::size_t k_recent_turns_inject_max    = 4;
inline constexpr std::size_t k_recent_turns_line_chars    = 96;
inline constexpr std::size_t k_recent_turns_user_chars    = k_recent_turns_line_chars * 2;
inline constexpr std::size_t k_recent_turns_asst_chars    = k_recent_turns_line_chars * 2;
inline constexpr std::size_t k_recent_turns_inject_chars  = 2200;
inline constexpr std::size_t k_recent_turns_hint_calls    = 2;

/// Total UTF-8 chars injected into the agent system prompt for "Session Rules".
inline constexpr std::size_t k_session_rules_inject_chars = 1500;

/// Soft cap for the MemoryUpdate event payload `patch` field in agent logs.
/// When exceeded, the log serializer keeps the counts + id digests and drops
/// the full patch body.
inline constexpr std::size_t k_memory_update_log_chars    = 8000;

/// #9 — Max entries kept in the per-session tool-call journal
/// (<session-id>.journal.jsonl). Acts as a ring buffer: once we cross this
/// threshold the file is compacted on next append and the oldest entries are
/// dropped. 200 covers ~20 turns of normal tool use without unbounded growth.
inline constexpr std::size_t k_session_journal_max_entries = 200;

// ── Hybrid memory recall ─────────────────────────────────────────────────────
//
// Defaults are inject/latency budgets, not corpus-size ceilings. Ranking uses an
// adaptive shortlist (see MemoryRecallSettings::shortlist_for). Env overrides for
// harness presets:
//   PM_LLM_RECALL_TOP_K, PM_LLM_RECALL_INJECT_CHARS,
//   PM_LLM_RECALL_POOL (0/unset = adaptive), PM_LLM_RECALL_DENSE_BUDGET_MS

inline constexpr std::size_t k_recall_top_k              = 5;
inline constexpr std::size_t k_recall_inject_chars       = 1500;
/// Adaptive shortlist floor hint when PM_LLM_RECALL_POOL is unset.
inline constexpr std::size_t k_recall_candidate_pool     = 100;
inline constexpr std::size_t k_recall_shortlist_ceiling  = 2000;
inline constexpr std::int64_t k_recall_dense_budget_ms   = 150;
inline constexpr int         k_recall_rrf_k              = 60;

// ── Pre-API context reduction (attention reduction layer) ────────────────────
//
// Opt-in shrinking of the message history sent to the model, applied by the
// agent's pre-API filter immediately before each LLM call. The canonical
// transcript keeps full fidelity — only the wire payload is reduced. JSON/code
// tool envelopes route to the structural compressor; free text routes through
// the deterministic attention pipeline (protect → dedup → levels → entropy →
// lexical), conditioned on the user prompt as objective. Model-backed score
// stages (embed/kompress) are NOT wired here yet — this slice lands the option
// + the boundary; the score callbacks are a follow-up.
//
//   ContextReductionMode::Off          no reduction (default — fully opt-in)
//   ContextReductionMode::ToolResults  reduce large `tool` result messages only
//   ContextReductionMode::All          also reduce older assistant/user context
//                                       (never the system message or the live
//                                       user turn)
//
enum class ContextReductionMode : std::uint8_t {
    Off         = 0,
    ToolResults = 1,
    All         = 2,
};

/// Default reduction mode — Off. Reduction is strictly opt-in per Turn so the
/// agent's wire payload is byte-identical to today unless a caller asks for it.
inline constexpr ContextReductionMode k_default_context_reduction_mode =
    ContextReductionMode::Off;

/// Minimum message size (bytes) before the pre-API filter considers reducing it.
/// Small payloads are not worth the structural / attention pass.
inline constexpr std::size_t k_context_reduction_min_bytes = 2048;

/// Target output/input byte ratio for reduced messages (0..1).
inline constexpr double k_context_reduction_ratio = 0.5;

// ── run tool captured output ──────────────────────────────────────────────────
//
// Three different sizes, kept separate so a later spooling capture can add
// total-produced bytes without renaming these fields:
//
//   inline limit   what one stdout or stderr stream may place in the tool
//                  result (model context). Always applied.
//   capture limit  how much of one stream may stay resident before capture
//                  must spool to disk. Phase 1 still receives the full
//                  ProcessRunner string; this constant is the phase-2 threshold.
//   overflow file  the complete captured stream, under {config}/agent-temp/run,
//                  readable with file_read.
//
// 24 KiB is about 6k tokens at 4 bytes/token. At $10 / 1M input tokens that is
// roughly $0.06 per clipped stream, or $0.12 when both streams clip. A later
// cached replay of the same result is about $0.006 per stream at $1 / 1M.
// Head keeps the opening of the command; tail keeps the failure. The clipping
// marker is a few hundred bytes on top of this budget.
// Context reduction is a separate, optional pass and is not this limit.

/// Maximum bytes of one captured stream copied into the tool result.
inline constexpr std::size_t k_run_output_inline_max_bytes = 24u * 1024u;

/// Bytes retained from the beginning when a stream is clipped.
inline constexpr std::size_t k_run_output_head_bytes = 16u * 1024u;

/// Remaining inline budget, retained from the end.
inline constexpr std::size_t k_run_output_tail_bytes =
    k_run_output_inline_max_bytes - k_run_output_head_bytes;

/// Resident-memory ceiling for one stream once capture itself spools.
/// Not enforced while ProcessRunner still returns the full string.
inline constexpr std::size_t k_run_output_memory_capture_max_bytes =
    4u * 1024u * 1024u;

/// Age after which a saved run capture may be deleted. Long enough that the
/// next agent turn, including one the following day, can still file_read it.
inline constexpr std::int64_t k_run_output_capture_max_age_hours = 72;

static_assert(k_run_output_head_bytes < k_run_output_inline_max_bytes);
static_assert(k_run_output_tail_bytes > 0);
static_assert(k_run_output_head_bytes + k_run_output_tail_bytes
              == k_run_output_inline_max_bytes);
static_assert(k_run_output_memory_capture_max_bytes
              >= k_run_output_inline_max_bytes);

} // namespace pm::llm

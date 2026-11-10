#pragma once
#include "constants.hpp"
#include "llm_constants.hpp"
#include <string_view>
//
// media::llm::agent — synchronous chat-agent loop on top of polymech::kbot
// and media::llm::path. Used by both the in-app chat dock (Win32 UI) and
// the `pm-image llm agent` CLI subcommand.
//
// Flow per turn:
//   1. Build a system prompt: optional `system-prompt.md` (trimmed) next to the executable,
//      else under the app config dir (Windows: get_config_dir(); macOS/Linux/Unix: same folder as portable
//      settings.json, see settings_portable) — replaces the built-in **intro** only. A fixed **runtime context**
//      block (OS, product, chat UI) plus tool catalog + Explorer selection/folder + guidelines
//      are always appended (see build_system_prompt).
//   2. Send {system, user} to kbot with tools = path::tool_catalog_openai() plus optional
//      MCP tools from profile mcp.json (Streamable HTTP; see ref/agent.cpp src/mcp/).
//   3. While the response carries tool_calls[]: dispatch each via
//      media::llm::path::execute(...), append results back as `tool` messages,
//      call kbot again. Capped by AgentOptions::max_iterations.
//   4. Return the final assistant text + a structured transcript.
//
// Cancellation: (1) the EventCallback may return false to abort between
// iterations; (2) global Ctrl+C / SIGINT (see `core/cli_cancel.hpp`) stops
// the loop before the next LLM or tool call and between files inside path
// tools; returns Result with `cancelled=true` (exit code 130 in CLI).
//
// See docs/chat.md §6 for the design rationale.
//
#include "polymech_export.h"
#include "llm/agent_tool_provider.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace media::llm::agent {

// ── P4: PlannerStrategy — adaptive planner execution mode ────────────────────
//
// Controls how the planner pre-pass runs for a given turn.
// When set to Auto (default), PlannerFeature falls back to `planner_enabled`
// for backward compatibility with existing callers.
//
enum class PlannerStrategy : uint8_t {
    Auto,         // default — fall back to planner_enabled bool
    Disabled,     // skip planner entirely (overrides planner_enabled=true)
    IntentOnly,   // run planner LLM call; extract intent/action/subject/confidence;
                  // skip tool-schema expansion and allowed_path_tools assignment
    Full,         // full planner pass: schema selection + allowlist expansion
};

// ── P3: TurnMode — planner-derived behavioral mode ────────────────────────────
//
// Derived automatically from the planner's `intent` field (via PlannerFeature)
// or set explicitly by the caller before calling Agent::run().
// When non-Auto, SystemPromptFeature loads `mode-<name>.md` from the prompts
// directory and injects a ## Behavioral Mode block into the system prompt.
//
enum class TurnMode : uint8_t {
    Auto,         // default — no overlay injected
    Lookup,       // read-only; info-first; prefer retrieval tools
    Execute,      // tool-first; terse; prefer action tools
    Research,     // multi-hop; evidence-focused; cite sources
    Orchestrate,  // parallel-first; delegation-aware; spawn sub-tasks
    Create,       // write/generate; consider constraints
    FollowUp,     // continuation; context-first; terse; references prior turn
};

// One human chat turn: a prompt, plus the user's current selection /
// open viewer files / folder context that the agent should treat as
// implicit input when the prompt says "these files" / "this folder" / etc.
struct Turn {
    std::string              user_prompt;
    /// Optional prior OpenAI-style chat turns (`[{role,content},…]`) inserted
    /// after the system prompt and before `user_prompt`. Used by the
    /// OpenAI-compat daemon (`llm agent --serve`) for multi-turn clients that
    /// resend full `messages[]` history. Roles other than user/assistant are
    /// skipped here (system is handled separately).
    nlohmann::json           seed_chat_messages = nlohmann::json::array();
    std::vector<std::string> selection;     // explorer-selected files (may be empty)
    std::vector<std::string> open_files;    // files open in viewer tabs (may be empty)
    std::string              folder_hint;   // current Explorer folder ("" when N/A)
    /// Rendered as ${CONNECTION_CONTEXT} in host-specific prompts. Describes
    /// where tools execute (for example phone-local vs paired desktop LAN).
    std::string              connection_context;
    /// When true with a non-empty folder_hint, path tools (FS / search / pictures /
    /// service) hard-deny any path outside that folder (`--fs-jail` / serve hosts).
    bool                     fs_jail = false;
    /// Monaco caret / inner selection for the active editor file (1-based).
    /// Empty path means no live editor caret was reported this turn.
    std::string editor_path;
    int         editor_line = 0;
    int         editor_column = 0;
    int         editor_sel_start_line = 0;
    int         editor_sel_start_col = 0;
    int         editor_sel_end_line = 0;
    int         editor_sel_end_col = 0;
    std::string editor_selected_text;
    std::string              system_extra;  // optional caller-supplied extra system text
    /// Explicit full system prompt override. When set, replaces the canonical
    /// built-in/file prompt. When system_prompt_is_null is true, no system
    /// message is sent at all.
    bool                     system_prompt_override_set = false;
    bool                     system_prompt_is_null      = false;
    std::string              system_prompt_override;
    std::string              system_prompt_override_source; // override, override-file, override-null
    std::string              system_prompt_override_path;
    /// Planner system-prompt override. Mirrors the system-prompt knobs above
    /// but targets the planner's first LLM call. Overrides are always additive:
    /// the rendered `planner-prompt.md` base is injected as `${builtin}` (and
    /// forced-wrapped when the override omits that token), so a user inject
    /// cannot wipe the JSON-selection contract. `planner_prompt_is_null`
    /// clears only the inject — the base file still applies when present.
    bool                     planner_prompt_override_set = false;
    bool                     planner_prompt_is_null      = false;
    std::string              planner_prompt_override;
    std::string              planner_prompt_override_source; // override, override-file, override-null
    std::string              planner_prompt_override_path;
    /// Path tool names to omit (case-insensitive), e.g. from `llm agent --disable-tools=image_resize,list_images`.
    /// Empty = full catalog. Invalid names are ignored (with a warning on the CLI).
    std::vector<std::string> disabled_path_tools;
    /// MCP OpenAI function names to omit, e.g. `mcp_server__tool`.
    std::vector<std::string> disabled_mcp_tools;
    /// When false, MCP-backed OpenAI tools (`mcp_*` function names) are removed after `mcp.json` merge
    /// (Web chat sidebar master toggle). Default true — same behavior as CLI / native when unset.
    bool mcp_tools_enabled = true;
    /// MCP server names (mcp.json keys) to suppress for this turn. Empty = all servers active.
    /// Applied by McpFeature after AgentMcpBridge::try_create merges all servers.
    std::vector<std::string> disabled_mcp_servers;
    /// When true, disabled_path_tools / mcp_tools_enabled / disabled_mcp_servers are
    /// already the complete policy for this turn; skip the persisted global tools policy.
    bool tool_policy_override_set = false;
    /// Quick-tools Replicate video parameters (chat-next): merged into each `create_video` call so UI
    /// overrides model defaults / LLM-chosen enum values for overlapping keys (non-empty values only).
    nlohmann::json create_video_ui_overrides = nlohmann::json::object();
    /// Chat-web workflow id (`general` / `pictures` / `video`). Empty = none.
    std::string workflow_id;
    /// Pre-rendered filmstrip / workflow context. WorkflowFeature appends this
    /// to system_extra; PlannerFeature also copies it into the planner user message.
    std::string workflow_inject;
    /// When false, skip agent skill discovery/injection for this turn.
    bool skills_enabled = true;
    /// When true, inject `Agent.md` / `AGENTS.md` from the current folder.
    /// Default on; chat presets can turn it off.
    bool load_agent_md = true;
    bool skill_policy_override_set = false;
    bool skills_roaming_enabled = true;
    bool skills_workspace_enabled = true;
    std::vector<std::string> pinned_skills;
    std::vector<std::string> disabled_skills;
    /// Opt-in composite mode: run a small planner pass before schema assembly
    /// to choose a budgeted subset of path-tool schemas for the normal agent.
    bool planner_enabled = false;
    /// Maximum path-tool schemas the planner may pre-expand before falling back
    /// to the full catalog. Zero/negative means the default planner budget.
    int planner_schema_budget = 8;
    /// Host / app tool providers to attach this turn (ids from the process
    /// registry or AgentOptions). Empty = all registered providers.
    std::vector<std::string> tool_provider_ids;
    /// Providers attached for this turn only. They are not added to the
    /// process registry, so other turns do not see them. Same merge as the
    /// agent server's host providers: schemas are appended after the path
    /// catalog allowlist, and invoke() runs before path::execute.
    std::vector<std::shared_ptr<IAgentToolProvider>> extra_tool_providers;
    /// Planner-selected path tool allowlist. Empty means use the normal catalog.
    std::vector<std::string> allowed_path_tools;
    /// True when the planner made an explicit path-tool decision. This lets an
    /// empty allowlist mean "no path tools needed" instead of "fall back to all".
    bool planner_path_tool_allowlist_set = false;
    /// Debug/log metadata from the planner pass.
    nlohmann::json planner_meta = nlohmann::json::object();

    /// Set by BrowserUseFeature when a browser observation contains classic
    /// prompt-injection bait (exfil / download-and-run / instruction override).
    /// Agent::run aborts the turn at the next boundary.
    bool        browser_prompt_injection_abort = false;
    std::string browser_prompt_injection_reason;

    // ── Embedding inference (FEATURE_COMMAND_LLAMA_INFERENCE) ──────────────────
    /// Optional path to a GGUF embedding model for semantic tool/command matching.
    /// Empty = auto-discover via @ref media::local_models::resolve_embed_model_path
    /// (${MODELS_DIR}, install exe/../models, or PM_EMBED_MODEL). Only read on the first agent turn (call_once).
    std::string embed_model_path;

    // ── Scheduled-task context (set by AgentScheduler; empty for normal turns) ─
    /// Non-empty when this turn belongs to a scheduled task. Scopes memory_write.
    std::string    task_id;
    /// Compact JSON state from the task's last memory_write (injected into system prompt).
    nlohmann::json memory_state    = nlohmann::json::object();
    /// Last N events from the task event log (injected into system prompt).
    nlohmann::json recent_events   = nlohmann::json::array();
    /// True when this turn was fired by the scheduler timer (vs a user-initiated normal turn).
    bool           is_scheduled_tick = false;

    /// Canonical completed-run log locator. Runners set these before run_turn so
    /// CaptureTurnPair can retain a compact, trusted pointer to the unabridged
    /// assistant response without copying it into structured memory.
    std::string source_log_session_id;
    std::string source_log_id;
    std::string source_log_path;

    /// Optional per-turn cancel signal. When set and flipped to true, the agent
    /// loop aborts at the next iteration boundary (same effect as the global
    /// Ctrl+C flag) WITHOUT touching the process-global cancel state — so a single
    /// turn can be stopped in isolation (e.g. a voice-delegated run) while other
    /// subsystems (the live realtime session) keep running. nullptr = no signal.
    std::shared_ptr<std::atomic<bool>> cancel_signal;

    /// Optional extra cancel predicate checked alongside cancel_signal and the
    /// global Ctrl+C flag. Polled at every interrupt boundary — including inside
    /// llama.cpp token generation — so it fires immediately rather than waiting
    /// for the next event emission. Use when the cancel source is a host
    /// executor callback (e.g. xblox io.cancelled()) that cannot write to a
    /// shared atomic. Takes precedence over cancel_signal for sub-turn latency.
    std::function<bool()> cancel_check;

    /// Bitmask of pm::llm::StructuredMemoryFeature flags controlling capture,
    /// dedup/truncate/evict, and prompt-injection of the structured session
    /// memory lanes (items, artifacts, instructions). Pass 0 to fully disable.
    pm::llm::StructuredMemoryFeatures structured_memory_features =
        pm::llm::k_default_structured_memory_features;

    /// One-shot hybrid recall block consumed by SystemPromptFeature (InjectPendingRecall).
    std::string pending_recall_inject;

    // ── P3: behavioral mode ────────────────────────────────────────────────────
    /// Behavioral mode for this turn. PlannerFeature sets this automatically
    /// from the planner's intent field; callers may also set it directly.
    /// When non-Auto, SystemPromptFeature loads `mode-<name>.md` from the
    /// prompts directory and injects it as a ## Behavioral Mode block.
    TurnMode mode = TurnMode::Auto;

    // ── P4: planner strategy ───────────────────────────────────────────────────
    /// Controls how the planner pre-pass runs. When Auto, falls back to
    /// `planner_enabled`. IntentOnly extracts intent without tool-schema
    /// expansion — useful to derive TurnMode on text-only or low-cost turns.
    PlannerStrategy planner_strategy = PlannerStrategy::Auto;

    // ── P6: parallel tool dispatch ─────────────────────────────────────────────
    /// When true and the model emits > 1 tool call in one response, all calls
    /// are dispatched concurrently via std::async(launch::async). Results are
    /// collected and emitted in original model order.
    /// Limitations in this mode:
    ///   • Per-file progress streaming (image_transform, run chunk hooks) is
    ///     suppressed — hooks are single-threaded state.
    ///   • path::execute and mcp_bridge::call_tool must be re-entrant.
    /// Falls back to serial dispatch when there is only one tool call.
    bool parallel_tool_dispatch = true;

    // ── P9: post-turn self-check ───────────────────────────────────────────────
    /// When true, SelfCheckFeature runs on_finish and emits Event::SelfCheckResult
    /// with structural quality signals (tool ok-rate, deflection detection).
    /// No LLM call involved. Disable to suppress the extra event.
    bool self_check_enabled = true;

    // ── P12: named session snapshots ──────────────────────────────────────────
    /// Non-empty: SnapshotFeature will load this snapshot before on_prepare
    /// completes and merge it into turn.memory_state / turn.recent_events so
    /// the system prompt sees the snapshot context.
    /// CLI: --load-snapshot <name>
    std::string load_snapshot_name;
    /// Non-empty: SnapshotFeature will save the current session state to a named
    /// snapshot file after on_finish (on success or on any outcome when forced).
    /// CLI: --save-snapshot <name>
    std::string save_snapshot_name;

    // ── Pre-API context reduction (attention reduction layer) ──────────────────
    /// Opt-in shrinking of the wire message history sent to the model, applied by
    /// the agent's pre-API filter right before each LLM call. The canonical
    /// transcript keeps full fidelity; only the bytes sent to the provider are
    /// reduced. JSON/code tool envelopes route to the structural compressor; free
    /// text routes through the deterministic attention pipeline. Default Off, so
    /// the wire payload is byte-identical to today unless a caller opts in.
    /// Requires the FEATURE_LLM_COMPRESSION build; otherwise the filter is a
    /// no-op even when the mode is non-Off.
    pm::llm::ContextReductionMode context_reduction_mode =
        pm::llm::k_default_context_reduction_mode;
    /// Messages at least this many bytes are reduction candidates.
    std::size_t context_reduction_min_bytes = pm::llm::k_context_reduction_min_bytes;
    /// Target output/input byte ratio for reduced messages (0..1).
    double      context_reduction_ratio     = pm::llm::k_context_reduction_ratio;
    /// Relevance objective for text reduction. Empty = fall back to user_prompt.
    std::string context_reduction_objective;
};

/// Which API endpoint/transport the agent uses to talk to the LLM.
/// ChatCompletion: POST /chat/completions — OpenAI-compatible, broad router support.
/// Responses: POST /responses — OpenAI Responses API; supported natively by OpenAI /
///   OpenRouter and via bridge by LiteLLM proxy (auto-bridges non-native models).
/// Realtime: WebSocket /realtime PoC (text-only, no tools), currently OpenAI-focused.
enum class LlmApiMode { ChatCompletion, Responses, Realtime };

/// Streaming preference for LLM calls.
/// On: stream when the selected endpoint supports it (default).
/// Auto: stream only when model catalog meta allows it (`tanit_streaming`).
/// Off: disable streaming even if supported.
enum class LlmStreamingMode { Auto, On, Off };

// Provider settings — all fields can fall back to env vars at the kbot layer.
struct ProviderSettings {
    std::string router;  // matches polymech::kbot::KBotOptions::router
    std::string base_url;                       // empty = router default
    std::string custom_base_url;                // preset `chat.base_url` for router=custom slots
    std::string api_key;                        // empty = take from env (caller decides which)
    std::string model;
    int         timeout_ms    = 180'000;
    int         max_iterations = 30;             // tool-call loop cap (0 = no tools)
    LlmApiMode  api_mode      = (std::string_view(pm::llm::k_default_api_mode) == "responses"
                                    ? LlmApiMode::Responses : LlmApiMode::ChatCompletion);
    LlmStreamingMode streaming_mode = LlmStreamingMode::On;
    /// Optional planner override. Empty = inherit `router` / `model` (legacy behaviour). When set,
    /// the planner pre-selection LLM call uses these instead of the main agent's router/model so
    /// the cheap call can hit a faster/cheaper router (eg. `composer-2-fast`) while the main agent
    /// runs on a stronger model. Credentials for `planner_router` are re-resolved in
    /// `apply_planner_tool_selection` via `merge_provider_credentials` when it differs from
    /// `router`.
    std::string planner_router;
    std::string planner_model;
    /// Optional advisory SecurityAgent reviewer used by CLI/Web consent surfaces.
    /// Empty/disabled means no AI review is run before human consent.
    bool        ai_consent_review = false;
    std::string ai_consent_router;
    std::string ai_consent_model;
};

struct Event {
    enum Kind {
        TurnStarted,        // accepted; before host prepare / planner. payload = {selection, folder}
        /// Fired after every successful LLM HTTP response. payload shape:
        ///   { "round": N, "duration_ms": N, "stateful": bool,
        ///     "response_id": "resp_…",         // Responses API only
        ///     "tokens": { "input": N, "output": N, "reasoning": N? },
        ///     "model": "gpt-5.5-…" }
        /// Appears in the agent JSON between tool_result and the next tool_call
        /// so you can see exactly what each round cost.
        LlmRound,
        ToolCall,           // model requested a tool; tool_name + payload (arguments)
        ToolResult,         // we ran the tool; tool_name + payload (envelope)
        /// One file finished during `image_transform` (multi-file). payload = {id, envelope} — same
        /// envelope shape as ToolResult but `results` has a single entry. UIs can update the
        /// file queue before the full tool returns.
        ToolFileProgress,
        /// Model emitted reasoning / thinking text (Responses API only; output[].type="reasoning").
        /// text = concatenated summary. Fired before AssistantText. Never fed back to the model.
        Thinking,
        /// Incremental assistant text chunk from a streaming endpoint.
        /// text = delta chunk (may be empty for keepalive/control events).
        TextDelta,
        AssistantText,      // model produced final text; text = the response
        MemoryUpdate,       // session memory changed; payload = compact patch / counts
        /// Planner confidence fell below the clarification threshold (< 0.4).
        /// payload = { "confidence": float, "intent": str, "reason": str }
        /// The turn still proceeds — callers may use this to surface an inline
        /// hint or ask the user to confirm before committing to a tool call.
        ClarificationNeeded,
        /// A tool call returned ok=false and the whole turn failed / was
        /// cancelled without a successful recovery. Written to session memory
        /// as a kind=error_reflexion item so future planner calls can avoid
        /// repeating the same failure pattern.
        /// payload = { "error_tool": str, "error_message": str,
        ///             "user_prompt_summary": str, "iterations": int }
        ErrorReflexion,
        /// Post-turn structural quality gate (P9). Fired once at the end of on_finish
        /// before Done. No LLM call — pure analysis of the turn's transcript.
        /// payload = { "passed": bool, "tool_ok_rate": float,
        ///             "deflection_detected": bool, "final_text_chars": int,
        ///             "total_tools": int, "failed_tools": int,
        ///             "completion_confidence": float, "issues": [str]? }
        SelfCheckResult,
        Error,              // text = error message
        Done,               // turn complete; text = final assistant text or empty
        /// Raw NDJSON line from an external CLI runner (Claude stream-json, etc.).
        /// payload = the parsed object; text = optional short type/subtype label.
        ExternalStream,
    };
    Kind           kind;
    std::string    text;
    std::string    tool_name;
    nlohmann::json payload;
};

/// Returning `false` from the callback signals the loop to abort cleanly
/// at the next iteration boundary.
using EventCallback = std::function<bool(const Event&)>;

struct Result {
    bool           ok = false;
    std::string    error;
    std::string    final_text;       // last assistant message content
    /** Last LLM round finish_reason when available (e.g. "length" = max_tokens hit). */
    std::string    llm_finish_reason;
    bool           cancelled = false;
    int            iterations = 0;
    nlohmann::json transcript;       // full {messages:[…], summary} for debug/UI
    /**
     * Aggregated LLM telemetry from each successful chat completion in this turn
     * (one HTTP response per agent iteration). OpenAI-style `usage` fields plus optional
     * `usage.cost` (OpenRouter). Shape:
     *   { "prompt_tokens", "completion_tokens", "total_tokens", "cost"?, "llm_rounds": [ { "usage", "model"?, "id"? } ] }
     */
    nlohmann::json llm_usage_aggregate;
};

/// Run a synchronous turn. Blocks until the model stops calling tools, hits
/// max_iterations, or the callback returns false.
PM_CHAT_API Result run_turn(const Turn& turn,
                              const ProviderSettings& provider,
                              EventCallback on_event = nullptr);

/// One rewrite of a document page. Skills and memory stay off. The original
/// page stays with the caller. `plan` false forces max_iterations to 0.
/// `plan` true runs the planner and allows up to 30 tool iterations.
Result run_spoken_page(const std::string& page_text,
                       const std::string& prompt,
                       const std::string& language,
                       const ProviderSettings& provider,
                       const std::string& system = {},
                       bool plan = false);

/// Process-once, non-blocking host I/O warmup (prompt decrypt, MCP tools
/// disk/stale cache, skills listing, embed GGUF). Call from chat HWND create
/// so the first turn does not pay that work on the silent prepare path.
PM_CHAT_API void warmup_host_io_async();

/// Build the canonical system prompt using the pre-assembled OpenAI tools JSON
/// (path tools + MCP tools, already filtered). Preferred inside Agent::run().
PM_CHAT_API std::string build_system_prompt(const Turn& turn, const nlohmann::json& tools);
PM_CHAT_API std::string build_system_prompt(const Turn& turn,
                                             const nlohmann::json& tools,
                                             nlohmann::json* prompt_meta);

/// Backward-compat overload: rebuilds the tools JSON from the path catalog
/// using turn.disabled_path_tools. Used by CLI --dry-run and tests.
PM_CHAT_API std::string build_system_prompt(const Turn& turn);

} // namespace media::llm::agent

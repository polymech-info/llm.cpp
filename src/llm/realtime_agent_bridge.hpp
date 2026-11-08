#pragma once
//
// RealtimeAgentBridge — wraps the full PM-Image agent (media::llm::agent::run_turn)
// as a SINGLE function tool that a realtime voice/chat model can call.
//
// The realtime model stays a thin conversational shell: it never sees the 40+
// path/MCP tools. Instead it calls one tool — `run_pm_agent(goal, mode)` — and
// this bridge runs the real agent on a worker thread, then hands back a short
// summary the voice model reads aloud (see docs/a2a-realtime.md, chat-supervisor
// pattern). No audio / websocket detail crosses into here; the surface forwards
// tool calls in and delivers results out via the SendResult callback.
//
// Multi-turn continuity is provided by the agent's session store, keyed by a
// stable `session_id` (the live voice session) — exactly like the normal chat /
// CLI agent. Each call: session_load → run_turn → append turn summary.
//
// Cross-platform: pure agent orchestration, reusable by the Win32 chat panel and
// the CLI realtime path alike.

#include "agent.hpp"
#include "llm/agent_launch.hpp"
#include "polymech_export.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace media::llm::agent {

/// How overlapping realtime tool calls map onto agent runs.
enum class RealtimeAgentMode : uint8_t {
    /// One agent turn in flight per session. Overlapping calls are rejected with
    /// a {"status":"busy"} result the model reads back ("still working on …").
    /// Continuity across spoken turns comes from the shared session task_id
    /// (structured memory) — behaves like the normal, non-realtime agent.
    Singleton,
    /// Overlapping calls spin up a parallel agent run under a derived child
    /// session id so they don't race the primary session's memory. (Future-leaning;
    /// the primary turn still owns the main session id.)
    MultiAgent,
};

class PM_CHAT_API RealtimeAgentBridge {
public:
    /// Deliver a tool result back to the realtime model. `call_id` matches the
    /// originating tool call; `output_json` is a JSON string (function_call_output).
    using SendResult = std::function<void(const std::string& call_id,
                                          const std::string& output_json)>;

    /// Optional progress sink: every run_turn Event from the delegated agent is
    /// forwarded here so the surface can render the agent's full activity (tool
    /// calls, created files, run output, assistant text) into the chat transcript
    /// — exactly like a normal, non-realtime agent run. Pure side-effect sink;
    /// cancellation stays owned by the bridge.
    using ForwardEvent = std::function<void(const Event&)>;

    /// Optional: fetch the caller's live context (Explorer-selected files +
    /// current folder + open viewer tabs) right before each delegated run, so
    /// the agent sees the same context a typed turn would. Called on the
    /// worker thread; populate the out-params in place.
    using ContextProvider = std::function<void(std::vector<std::string>& selection,
                                               std::string& folder_hint,
                                               std::vector<std::string>& open_files)>;

    /// Per-run cooperative cancel flag (shared with the delegated worker thread).
    using DelegatedRunCancel = std::shared_ptr<std::atomic<bool>>;

    /// Optional: fired when a delegated run actually begins (after the
    /// concurrency gate — NOT for rejected/busy calls), so the surface can show
    /// the same "agent pill" a normal agent run does. `call_id` is the stable id
    /// to match against SendResult for the pill's done/error state. `cancel` is
    /// the same flag the bridge honours — register it in the host so stop works
    /// after the voice session tears down.
    using OnStarted = std::function<void(const std::string& call_id,
                                         const std::string& goal,
                                         DelegatedRunCancel cancel)>;

    struct Config {
        /// Shared agent session/task id (= the live voice session) for multi-turn
        /// continuity. Empty → a fresh session is created on first call.
        std::string       session_id;
        /// Resolved text agent provider (/responses or /chat completions) — NOT
        /// the realtime provider. This is the "brain" with the full tool catalog.
        ProviderSettings  provider;
        /// Launch spec + resolved settings for the brain. When
        /// `launch_resolved_ok`, apply_launch_policy_to_turn() is called on each
        /// delegated run so the brain gets the same planner / tool policy as a
        /// typed chat turn (instead of shipping the full catalog).
        launch::AgentLaunchSpec     launch_spec;
        launch::AgentLaunchResolved launch_resolved;
        bool                          launch_resolved_ok = false;
        RealtimeAgentMode mode = RealtimeAgentMode::Singleton;
        /// Optional: forwarded run_turn events for transcript rendering.
        ForwardEvent      on_event;
        /// Optional: live selection / folder context fetched per delegated run.
        ContextProvider   context_provider;
        /// Optional: fired when a delegated run actually starts (agent pill).
        OnStarted         on_started;
        /// External agent backend for delegated runs (native | claude-code | codex).
        /// Empty → native run_turn. Also falls back to PM_LLM_AGENT_RUNNER.
        std::string       agent_runner;
        /// Pre-built ## Embedded Content block appended to each delegated goal
        /// (CLI `llm agent --embed`, built once at session start).
        std::string       embedded_prompt_suffix;
        /// Chat session id for secure agent.json log documents (optional).
        std::string       chat_session_id;
        /// Optional: last word on each delegated Turn, after launch policy and
        /// context (e.g. a host without a launch spec sets planner / memory here).
        std::function<void(Turn&)> prepare_turn;
        /// End the live voice session on this host. Called on the realtime
        /// tool thread for `suspend_voice`. Return false and fill `err` when
        /// no session is running. Must not block on the voice socket.
        std::function<bool(std::string& err)> suspend_voice;
    };

    RealtimeAgentBridge(Config cfg, SendResult send);
    ~RealtimeAgentBridge();
    RealtimeAgentBridge(const RealtimeAgentBridge&) = delete;
    RealtimeAgentBridge& operator=(const RealtimeAgentBridge&) = delete;

    /// Handle one realtime tool call. Returns immediately; the agent runs on a
    /// worker thread and the result is delivered asynchronously via SendResult.
    void handle_tool_call(const std::string& name,
                          const std::string& call_id,
                          const nlohmann::json& args);

    /// Cancel an in-flight delegated run. `run_call_id` is the originating
    /// run_pm_agent call id (= the agent pill id). Empty cancels ALL active runs.
    /// Cooperative (aborts at the next iteration boundary) plus an immediate
    /// interrupt of any in-progress tool / TTS. Safe from any thread.
    /// Returns true if a matching run was found and signalled (lets callers
    /// distinguish a voice-delegated run from a normal agent id).
    bool cancel(const std::string& run_call_id);

    /// Cooperatively abort in-flight runs and wait for workers to finish.
    /// Idempotent; safe from any thread. Call this BEFORE destroying whatever
    /// the SendResult callback captures (e.g. the voice session).
    void stop();

    /// True while at least one delegated agent run is in flight (Singleton guard,
    /// MultiAgent runs, or a worker still touching `this`). Lets a host keep an
    /// idle-timeout from killing the session mid-task. Safe from any thread.
    [[nodiscard]] bool is_busy() const noexcept;

    /// OpenAI function schema for the exposed tools. Add these to the realtime
    /// session's `tools` array so the model can delegate and abort.
    static nlohmann::json run_pm_agent_tool_schema();
    static nlohmann::json stop_pm_agent_tool_schema();
    static nlohmann::json suspend_voice_tool_schema();

private:
    using CancelFlag = DelegatedRunCancel;
    void run_agent_worker(std::string call_id, std::string goal,
                          std::string mode_hint, std::string session_id,
                          CancelFlag cancel_flag);

    Config     cfg_;
    SendResult send_;

    std::atomic<bool> busy_{false};      // Singleton in-flight guard
    std::atomic<int>  active_count_{0};  // MultiAgent in-flight runs (child-id naming)
    std::atomic<int>  child_counter_{0}; // derived child session-id suffix
    std::atomic<bool> stopping_{false};

    std::mutex                                   runs_mu_;  // guards active_cancels_, quiet_cancels_
    std::unordered_map<std::string, CancelFlag>  active_cancels_; // call_id → per-run cancel
    std::unordered_set<std::string>              quiet_cancels_;  // stopped via stop_pm_agent (already acknowledged)

    std::atomic<int>        outstanding_{0}; // detached workers still touching `this`
    std::mutex              done_mu_;
    std::condition_variable done_cv_;
};

/// Shared spoken-turn persistence for realtime voice sessions.
///
/// Accumulates one spoken turn (final user input transcription + streamed
/// assistant audio transcript) and appends it to the agent session store as a
/// single `{ts, user, assistant, mode:"realtime"}` event on commit() — the
/// exact event shape every surface uses. Wire it from the voice-session
/// callbacks so the chat UI and the headless CLI persist identically:
///
///   cb.on_user_transcript → if (final) rec.set_user(t)
///   cb.on_transcript      → rec.add_assistant(delta)
///   cb.on_response_done   → rec.commit()
///
/// Thread-safe; commit() with an empty turn is a no-op.
class PM_CHAT_API RealtimeTurnRecorder {
public:
    explicit RealtimeTurnRecorder(std::string session_id);

    void set_user(const std::string& final_transcript);
    void add_assistant(const std::string& delta);
    void commit();

private:
    std::mutex  mu_;
    std::string session_id_;
    std::string user_;
    std::string asst_;
};

} // namespace media::llm::agent

#pragma once
//
// VoiceAgent — the realtime counterpart of AgentFactory::default_agent().
//
// One shared composite that owns everything a live speech-to-speech agent
// session consists of, so surfaces stop re-building the same scaffolding:
//
//   • provider resolution : ONE launch spec, resolved twice through the shared
//     resolver — api_mode=realtime for the WS voice model (honors router/model
//     overrides), api_mode=responses for the delegated agent "brain" (router/
//     model/api_key overrides are deliberately NOT threaded: those target the
//     WS model; the brain always follows the chat preset's text router/model)
//   • session config      : apply_session_defaults (tools, realtime-prompt.md
//     persona + live-context note, voice, mic/speaker devices)
//   • the voice session   : pm::audio::RealtimeVoiceSession lifecycle
//   • the agent bridge    : RealtimeAgentBridge (run_pm_agent / stop_pm_agent)
//   • turn persistence    : RealtimeTurnRecorder ({user,assistant,mode:"realtime"})
//   • the interconnect    : tool calls → bridge, results → voice, transcripts
//     → recorder, and the teardown ordering (bridge joins workers BEFORE the
//     voice session it calls back into is stopped)
//
// Surfaces provide only sinks (Hooks: chat panel → JS, CLI → stdout) and keep
// surface policy at the call site: session-id mapping/adoption, push-to-talk,
// rt_ipc singleton ownership, idle auto-stop.

#if defined(FEATURE_STT) && FEATURE_STT && defined(FEATURE_REALTIME) && FEATURE_REALTIME

#include "core/audio_realtime.hpp"
#include "core/settings_runtime.hpp"
#include "llm/agent_launch.hpp"
#include "llm/realtime_agent_bridge.hpp"
#include "llm/realtime_session_config.hpp"
#include "polymech_export.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace pm::audio::rt_ipc { class Owner; }

namespace media::llm::agent::realtime {

class PM_CHAT_API VoiceAgent {
public:
    struct Options {
        /// Surface-built launch spec (preset, overrides, timeout, …). api_mode
        /// is stamped internally for the two resolutions — see header note.
        launch::AgentLaunchSpec spec;
        /// Selection/folder + persona/voice/mic overrides → apply_session_defaults.
        SetupOptions setup;
        /// Agent session id for continuity + persistence. Empty → a fresh
        /// session is created. Surfaces own the mapping (chat adoption,
        /// --session-id / multi-turn).
        std::string session_id;
        /// Appended verbatim after the rendered persona (e.g. the CLI --prompt
        /// "Additional context").
        std::string extra_instructions;
        /// Optional push-to-talk gate (null → open mic).
        std::shared_ptr<std::atomic<bool>> mic_send_enabled;
        /// Optional playback gate (null → audible speaker).
        std::shared_ptr<std::atomic<bool>> speaker_send_enabled;
        /// Acquire the cross-process rt_ipc singleton (one live voice session
        /// per machine; makes the session visible to `realtime-status`, the
        /// assistant toggle, and stoppable via `--realtime-stop`). A stop
        /// watcher thread honors cross-process stop requests for ANY host —
        /// UI-owned sessions included.
        bool acquire_realtime_singleton = true;
        /// >0 → retry the singleton acquire for this long (tolerates a
        /// predecessor still tearing down, e.g. the assistant→chat handoff).
        int acquire_timeout_ms = 0;
        /// Pre-built embed block appended to each delegated run_pm_agent goal
        /// (from CLI `--embed`; empty when unset).
        std::string delegated_embed_suffix;
        /// UI chat session id (for secure agent.json log documents).
        std::string chat_session_id;
    };

    /// Surface sinks; every member is optional. Shared behavior (recorder,
    /// bridge dispatch, result delivery to the voice model) happens regardless.
    struct Hooks {
        std::function<void()>                          on_ready;
        std::function<void(const std::string&)>        on_text;            // assistant text delta
        std::function<void(const std::string&)>        on_transcript;      // spoken transcript delta
        std::function<void(const std::string&, bool)>  on_user_transcript; // (text, final)
        std::function<void()>                          on_response_done;
        std::function<void(double)>                    on_level;
        /// Voice model delegated a tool call (before the bridge runs it).
        std::function<void(const std::string& name, const nlohmann::json& args)> on_handover;
        std::function<void(const std::string&)>        on_error;
        // Fired in addition to on_error when the failure is an authentication
        // error (HTTP 401/403, expired token, invalid API key). Use this to show
        // a provider-specific "sign in" or "check API key" prompt.
        std::function<void(const std::string&)>        on_auth_error;
        // Fired in addition to on_error when the failure is a network connectivity
        // problem (DNS, TCP, SSL, timeout, mid-session drop). Use this to show
        // a "check your internet connection" prompt.
        std::function<void(const std::string&)>        on_network_error;
        std::function<void()>                          on_closed;
        // Delegated-agent activity (forwarded from the bridge).
        RealtimeAgentBridge::ForwardEvent              on_agent_event;
        RealtimeAgentBridge::OnStarted                 on_agent_started;
        RealtimeAgentBridge::ContextProvider           context_provider;
        /// Raw run_pm_agent result JSON, after it was handed to the voice model.
        std::function<void(const std::string& call_id, const std::string& output_json)> on_agent_result;
    };

    /// Blocking-run policy for headless hosts (see run()).
    struct RunPolicy {
        /// Injected once as a spoken-turn seed after the session goes live.
        std::string seed_prompt;
        /// >0 → self-stop after this much inactivity (no speech / reply /
        /// delegation) while no delegated agent run is in flight.
        std::int64_t idle_auto_stop_ms = 0;
        /// External cancel (e.g. Ctrl+C flag). Polled every loop tick.
        std::function<bool()> cancel;
    };

    enum class RunResult : uint8_t { Ok = 0, StartError = 1, Cancelled = 2 };

    VoiceAgent();
    ~VoiceAgent();
    VoiceAgent(const VoiceAgent&) = delete;
    VoiceAgent& operator=(const VoiceAgent&) = delete;

    /// Resolve providers, build config + bridge, acquire the rt_ipc singleton
    /// (when enabled). Fails (fills err) when the realtime provider cannot be
    /// resolved, has no API key, or another live session holds the singleton.
    bool init(Options opt, Hooks hooks, std::string& err);

    /// Open the WS session (blocks on connect ~1s — call off the UI thread).
    /// On failure the bridge is already stopped; the instance can be discarded.
    bool start(std::string& err);

    /// Headless convenience: start() + seed prompt + block until the session
    /// ends (cancel predicate, cross-process stop, voice teardown, or idle
    /// auto-stop), then ordered shutdown. The library-function form of the
    /// whole CLI realtime command.
    RunResult run(const RunPolicy& policy, std::string& err);

    /// Idempotent: flip the IPC state to Stopping, join in-flight delegated
    /// agent workers, then stop the voice session. Safe from any thread.
    void stop();

    [[nodiscard]] bool is_active() const;
    /// True when a cross-process stop was requested via the rt_ipc singleton.
    [[nodiscard]] bool stop_requested() const;
    /// True while a delegated run_pm_agent is in flight (idle-stop guard).
    [[nodiscard]] bool agent_busy() const;
    /// Cancel a delegated run (empty id → all). Returns false for unknown ids.
    bool cancel_agent(const std::string& run_call_id = {});
    /// Inject a manual text turn (e.g. the CLI --prompt seed).
    void send_text(const std::string& text);
    /// Finalize the input buffer (push-to-talk release).
    void finish_input_turn();

    [[nodiscard]] const std::string& session_id() const { return session_id_; }
    [[nodiscard]] const pm::audio::RealtimeVoiceSession::Config& config() const { return cfg_; }
    [[nodiscard]] const ProviderSettings& realtime_provider() const { return rt_prov_; }
    [[nodiscard]] const ProviderSettings& brain_provider() const { return brain_prov_; }
    /// Effective delegated backend for run_pm_agent (native | claude-code | …).
    [[nodiscard]] const std::string& delegated_agent_runner() const { return delegated_agent_runner_; }

private:
    void bump_activity();
    void join_watcher_if_foreign();

    pm::audio::RealtimeVoiceSession::Config cfg_;
    ProviderSettings rt_prov_;
    ProviderSettings brain_prov_;
    launch::AgentLaunchSpec     launch_spec_;
    launch::AgentLaunchResolved launch_resolved_;
    bool                        brain_launch_ok_ = false;
    std::string                 delegated_agent_runner_ = "native";
    std::string      session_id_;
    Hooks            hooks_;

    // Declaration order = teardown contract: bridge_ destructs (joins workers)
    // before the voice session it sends tool results into.
    std::unique_ptr<pm::audio::RealtimeVoiceSession> voice_;
    std::unique_ptr<RealtimeAgentBridge>             bridge_;
    std::shared_ptr<RealtimeTurnRecorder>            recorder_;

    std::unique_ptr<pm::audio::rt_ipc::Owner> ipc_owner_;
    std::thread                               stop_watcher_;
    std::atomic<bool>                         stopping_{false};
    std::atomic<std::int64_t>                 last_activity_ms_{0};
};

/// One-call form: construct, init, and run() a voice agent. The whole headless
/// realtime session as a library function fed with options.
PM_CHAT_API VoiceAgent::RunResult run_voice_agent(VoiceAgent::Options opt,
                                                   VoiceAgent::Hooks hooks,
                                                   const VoiceAgent::RunPolicy& policy,
                                                   std::string& err);

} // namespace media::llm::agent::realtime

#endif // FEATURE_STT && FEATURE_REALTIME

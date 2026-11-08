#include "llm/realtime_agent_bridge.hpp"

#include "llm/agent_launch.hpp"
#include "llm/agent_memory.hpp"        // session_load / session_append_turn / session_get_or_create / now_iso8601
#include "llm/agent_runner.hpp"
#include "llm/agent_runner_session.hpp"
#include "llm/agent_runner_log.hpp"
#include "llm/path_tool_executor.hpp"  // abort_active_run / abort_active_speak
#include "core/cli_cancel.hpp"         // ScopedIgnoreGlobalCancel (isolate delegated run from global latch)
#include "string/string_sanitizers.hpp"
#include "logging/logger.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace media::llm::agent {

nlohmann::json RealtimeAgentBridge::run_pm_agent_tool_schema() {
    // GA realtime tool shape: flat {type:"function", name, description, parameters}.
    return nlohmann::json{
        {"type", "function"},
        {"name", "run_pm_agent"},
        {"description",
         "Delegate any non-trivial request to the agent, which has this device's full "
         "tool set (see your instructions for what it can do), a planner, and persistent "
         "session memory. Use this for anything beyond basic chit-chat, and whenever you "
         "are unsure what the user wants or whether it can be done. Say a brief filler "
         "phrase first (e.g. \"one moment\"), then call this. Returns a short summary to read "
         "back to the user verbatim. status \"cancelled\" means the user stopped it."},
        {"parameters", {
            {"type", "object"},
            {"properties", {
                {"goal", {
                    {"type", "string"},
                    {"description", "What the user wants done, in one clear sentence."}
                }},
                {"mode", {
                    {"type", "string"},
                    {"enum", nlohmann::json::array({"lookup", "execute", "research", "create"})},
                    {"description", "Optional hint about the kind of task."}
                }}
            }},
            {"required", nlohmann::json::array({"goal"})}
        }}
    };
}

nlohmann::json RealtimeAgentBridge::stop_pm_agent_tool_schema() {
    return nlohmann::json{
        {"type", "function"},
        {"name", "stop_pm_agent"},
        {"description",
         "Abort the currently running run_pm_agent task. Call this when the user asks to "
         "stop, cancel, abort, or never mind the task in progress. No-op if nothing is running."},
        {"parameters", {
            {"type", "object"},
            {"properties", nlohmann::json::object()},
            {"required", nlohmann::json::array()}
        }}
    };
}

nlohmann::json RealtimeAgentBridge::suspend_voice_tool_schema() {
    return nlohmann::json{
        {"type", "function"},
        {"name", "suspend_voice"},
        {"description",
         "End this voice session and return to standby. Call this directly when the user "
         "is done with voice, says stop listening, suspend, or hang up. Do not use "
         "run_pm_agent for that. Does not cancel a task — that is stop_pm_agent. "
         "Say nothing after this returns."},
        {"parameters", {
            {"type", "object"},
            {"properties", nlohmann::json::object()},
            {"required", nlohmann::json::array()}
        }}
    };
}

RealtimeAgentBridge::RealtimeAgentBridge(Config cfg, SendResult send)
    : cfg_(std::move(cfg)), send_(std::move(send)) {}

RealtimeAgentBridge::~RealtimeAgentBridge() { stop(); }

void RealtimeAgentBridge::handle_tool_call(const std::string& name,
                                           const std::string& call_id,
                                           const nlohmann::json& args) {
    if (stopping_.load()) return;

    if (name == "stop_pm_agent") {
        {
            std::lock_guard<std::mutex> lk(runs_mu_);
            for (const auto& kv : active_cancels_) quiet_cancels_.insert(kv.first);
        }
        const bool any = cancel(std::string{}); // abort all active runs
        if (send_)
            send_(call_id, any
                ? nlohmann::json{{"status", "stopped"}}.dump()
                : nlohmann::json{{"status", "idle"},
                                 {"note", "No task was running."}}.dump());
        return;
    }

    if (name == "suspend_voice") {
        std::string err;
        const bool ok = cfg_.suspend_voice && cfg_.suspend_voice(err);
        if (ok)
            cancel(std::string{});
        if (send_)
            send_(call_id, ok
                ? nlohmann::json{{"status", "suspended"},
                                 {"note", "Voice session is ending. Say nothing further."}}.dump()
                : nlohmann::json{{"status", "idle"},
                                 {"note", err.empty() ? "No live voice session." : err}}.dump());
        return;
    }

    if (name != "run_pm_agent") {
        if (send_)
            send_(call_id, nlohmann::json{{"status", "error"},
                                          {"error", "unknown tool: " + name}}.dump());
        return;
    }

    const std::string goal      = args.is_object() ? args.value("goal", std::string{}) : std::string{};
    const std::string mode_hint = args.is_object() ? args.value("mode", std::string{}) : std::string{};
    if (goal.empty()) {
        if (send_)
            send_(call_id, nlohmann::json{{"status", "error"},
                                          {"error", "missing required parameter: goal"}}.dump());
        return;
    }

    std::string session_id = cfg_.session_id;

    if (cfg_.mode == RealtimeAgentMode::Singleton) {
        bool expected = false;
        if (!busy_.compare_exchange_strong(expected, true)) {
            logger::info("RealtimeAgentBridge: busy — rejecting overlapping run_pm_agent");
            if (send_)
                send_(call_id, nlohmann::json{
                    {"status", "busy"},
                    {"note", "Still working on the previous request — let me finish that first."}
                }.dump());
            return;
        }
    } else {
        // MultiAgent: never block; if a run is already active, derive a child
        // session id so the parallel run doesn't race the primary's memory.
        if (active_count_.fetch_add(1) > 0) {
            session_id = cfg_.session_id + "#rt" + std::to_string(++child_counter_);
            logger::info("RealtimeAgentBridge: multi-agent overlap → child session " + session_id);
        }
    }

    // Register a per-run cancel flag so the pill's stop button / stop_pm_agent
    // can abort this specific run by call_id.
    auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
    // Count ownership before exposing the run through active_cancels_/on_started;
    // stop() must never observe a registered run while outstanding_ is still 0.
    outstanding_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(runs_mu_);
        active_cancels_[call_id] = cancel_flag;
    }

    // The run is committed past the concurrency gate — surface the agent pill.
    if (cfg_.on_started) {
        try { cfg_.on_started(call_id, goal, cancel_flag); } catch (...) {}
    }

    try {
        std::thread(&RealtimeAgentBridge::run_agent_worker, this,
                    call_id, goal, mode_hint, session_id, cancel_flag)
            .detach();
    } catch (...) {
        {
            std::lock_guard<std::mutex> lk(runs_mu_);
            active_cancels_.erase(call_id);
        }
        if (cfg_.mode == RealtimeAgentMode::Singleton) busy_.store(false);
        else                                           active_count_.fetch_sub(1);
        outstanding_.fetch_sub(1);
        done_cv_.notify_all();
        if (send_) {
            try {
                send_(call_id, nlohmann::json{
                    {"status", "error"},
                    {"error", "failed to start delegated agent worker"},
                }.dump());
            } catch (...) {}
        }
    }
}

void RealtimeAgentBridge::run_agent_worker(std::string call_id, std::string goal,
                                           std::string mode_hint, std::string session_id,
                                           CancelFlag cancel_flag) {
    nlohmann::json out;
    try {
        const std::string sid = session_get_or_create(session_id);

        Turn turn;
        turn.user_prompt   = goal;
        if (!cfg_.embedded_prompt_suffix.empty())
            turn.user_prompt += cfg_.embedded_prompt_suffix;
        turn.task_id       = sid;
        turn.cancel_signal = cancel_flag; // honored by the agent loop (per-turn, not global)
        if      (mode_hint == "lookup")   turn.mode = TurnMode::Lookup;
        else if (mode_hint == "execute")  turn.mode = TurnMode::Execute;
        else if (mode_hint == "research") turn.mode = TurnMode::Research;
        else if (mode_hint == "create")   turn.mode = TurnMode::Create;

        // Same launch policy as typed chat: planner seed, preset tool toggles,
        // MCP enablement. Without this the brain ships the full path catalog.
        if (cfg_.launch_resolved_ok)
            launch::apply_launch_policy_to_turn(cfg_.launch_spec, cfg_.launch_resolved, turn);
        {
            const auto launch_overrides = media::runtime_settings::chat_launch_overrides();
            if (launch_overrides.no_mcp_tools)
                turn.mcp_tools_enabled = false;
            if (!turn.tool_policy_override_set) {
                std::string gerr;
                media::runtime_settings::apply_global_tool_policy_to_values(
                    turn.disabled_path_tools, turn.mcp_tools_enabled, turn.disabled_mcp_servers, gerr);
            }
        }

        // Hand the agent the same live context a typed turn gets: selected files
        // + open viewer tabs + current folder.
        if (cfg_.context_provider) {
            try { cfg_.context_provider(turn.selection, turn.folder_hint, turn.open_files); } catch (...) {}
        }
        if (cfg_.prepare_turn) {
            try { cfg_.prepare_turn(turn); } catch (...) {}
        }

        // Hydrate prior turns/structured memory for multi-turn continuity.
        session_load(sid, turn.memory_state, turn.recent_events, 20);

        logger::info("RealtimeAgentBridge: run_pm_agent session=" + sid
                     + " goal=" + goal.substr(0, 120));

        nlohmann::json log_events = nlohmann::json::array();
        // Forward each event to the surface (transcript rendering) and bail at the
        // next iteration boundary on a global stop or this run's cancel.
        EventCallback on_event = [this, cancel_flag, &log_events](const Event& e) {
            if (stopping_.load() || (cancel_flag && cancel_flag->load()))
                return false;
            append_agent_log_event(log_events, e);
            if (cfg_.on_event) { try { cfg_.on_event(e); } catch (...) {} }
            return !(stopping_.load() || (cancel_flag && cancel_flag->load()));
        };
        // This delegated run is isolated from the process-global cancel latch:
        // it has its own cancel channel (cancel_flag → cancel_signal + on_event,
        // propagated through ScopedCancelPredicate). Opting this worker thread
        // out of the process-global latch ensures an unrelated CLI Ctrl+C cannot
        // kill this run through the loop, curl hook, or a polling tool.
        media::cli::ScopedIgnoreGlobalCancel ignore_global_cancel;

        std::optional<AgentRunnerKind> external_runner;
        if (!cfg_.agent_runner.empty())
            external_runner = parse_agent_runner_kind(cfg_.agent_runner);
        else if (const char* env = std::getenv("PM_LLM_AGENT_RUNNER")) {
            if (*env)
                external_runner = parse_agent_runner_kind(env);
        }

        Result res;
        if (external_runner
            && *external_runner != AgentRunnerKind::Native
            && agent_runner_kind_enabled(*external_runner)) {
            AgentRunnerOptions ropt;
            ropt.kind = *external_runner;
            if (ropt.resume_id.empty())
                ropt.resume_id = external_session_lookup(sid, *external_runner);
            res = run_agent_turn(turn, cfg_.provider, ropt, on_event);
            const std::string ext_sess = extract_external_session_id(res);
            if (!ext_sess.empty())
                external_session_store(sid, *external_runner, ext_sess);
        } else {
            res = run_turn(turn, cfg_.provider, on_event);
        }

        {
            const std::string runner_name =
                (external_runner && agent_runner_kind_enabled(*external_runner))
                    ? agent_runner_kind_name(*external_runner)
                    : "native";
            AgentRunLogWriteOptions log_opts;
            log_opts.source           = "realtime_voice";
            log_opts.run_id           = call_id;
            log_opts.agent_session_id = sid;
            log_opts.chat_session_id  = cfg_.chat_session_id;
            log_opts.runner           = runner_name;
            log_opts.display_name     = goal;
            log_opts.display_name = media::str_san::utf8_safe_ellipsize(
                log_opts.display_name, 64, "\u2026");
            if (log_opts.display_name.empty())
                log_opts.display_name = "voice task";
            persist_agent_run_log(log_opts, turn, cfg_.provider, log_events, res);
        }

        const bool cancelled = res.cancelled || (cancel_flag && cancel_flag->load());
        // Persist a compact turn summary so the next spoken turn sees it.
        if (!sid.empty() && (res.ok || cancelled || !res.final_text.empty())) {
            try {
                nlohmann::json entry = nlohmann::json::object();
                entry["ts"] = now_iso8601();
                std::string u = media::str_san::utf8_safe_ellipsize(
                    goal, pm::llm::k_session_memory_user_chars, "\u2026");
                entry["user"] = std::move(u);
                std::string a = media::str_san::utf8_safe_ellipsize(
                    res.final_text, pm::llm::k_session_memory_asst_chars, "\u2026");
                if (cancelled && !res.ok)
                    a = a.empty() ? std::string("[stopped by the user]") : a + " [stopped by the user]";
                if (!a.empty()) entry["assistant"] = std::move(a);
                entry["mode"] = "realtime_agent";
                session_append_turn(sid, entry);
            } catch (...) {}
        }

        if (cancelled && !res.ok) {
            bool quiet = false;
            {
                std::lock_guard<std::mutex> lk(runs_mu_);
                quiet = quiet_cancels_.count(call_id) > 0;
            }
            out["status"]  = "cancelled";
            out["summary"] = res.final_text;
            out["note"] = quiet
                ? "Stopped at the user's spoken request. You already said so; say nothing more."
                : "The user stopped this task on screen. Tell them in one short sentence that "
                  "it was stopped. Do not retry it unless they ask.";
        } else {
            out["status"]  = res.ok ? "completed" : "error";
            out["summary"] = res.final_text;
            if (!res.ok && !res.error.empty()) out["error"] = res.error;
        }
    } catch (const std::exception& e) {
        out = nlohmann::json{{"status", "error"}, {"error", std::string("bridge exception: ") + e.what()}};
    } catch (...) {
        out = nlohmann::json{{"status", "error"}, {"error", "bridge exception"}};
    }

    // Release the in-flight slot + deregister the cancel flag.
    {
        std::lock_guard<std::mutex> lk(runs_mu_);
        active_cancels_.erase(call_id);
        quiet_cancels_.erase(call_id);
    }
    if (cfg_.mode == RealtimeAgentMode::Singleton) busy_.store(false);
    else                                           active_count_.fetch_sub(1);

    // Deliver the result, then drop our reference to `this` LAST so stop() can
    // safely wait for outstanding workers before the bridge is destroyed.
    if (send_ && !stopping_.load()) {
        try { send_(call_id, out.dump()); } catch (...) {}
    }
    outstanding_.fetch_sub(1);
    done_cv_.notify_all();
}

bool RealtimeAgentBridge::cancel(const std::string& run_call_id) {
    std::vector<CancelFlag> targets;
    {
        std::lock_guard<std::mutex> lk(runs_mu_);
        if (run_call_id.empty()) {
            for (auto& kv : active_cancels_) targets.push_back(kv.second);
        } else {
            auto it = active_cancels_.find(run_call_id);
            if (it != active_cancels_.end()) targets.push_back(it->second);
        }
    }
    if (targets.empty()) return false; // unknown id (e.g. a normal active_runs id) → no-op
    for (auto& f : targets) if (f) f->store(true);
    logger::info("RealtimeAgentBridge: cancel "
                 + (run_call_id.empty() ? std::string("ALL") : run_call_id));
    // A targeted run observes its per-turn predicate in HTTP, tools, MCP and
    // ProcessRunner. Do not use the process-global abort hooks here: concurrent
    // agents may own other shell/TTS work. Cancel-all may interrupt everything.
    if (run_call_id.empty()) {
        media::llm::path::abort_active_run();
        media::llm::path::abort_active_speak();
    }
    return true;
}

bool RealtimeAgentBridge::is_busy() const noexcept {
    return busy_.load() || active_count_.load() > 0 || outstanding_.load() > 0;
}

void RealtimeAgentBridge::stop() {
    stopping_.store(true);
    cancel(std::string{}); // flip per-turn signals so in-flight run_turn aborts promptly
    std::unique_lock<std::mutex> lk(done_mu_);
    // Never destroy the bridge while detached workers still dereference `this`.
    // The old 30-second timeout converted a slow cancellation into a use-after-
    // free and left child processes/threads behind. Per-turn cancellation now
    // reaches HTTP, MCP, consent/ask waits and ProcessRunner, so workers unwind
    // promptly and teardown can safely wait for the actual terminal state.
    done_cv_.wait(lk, [this] { return outstanding_.load() == 0; });
}

// ── RealtimeTurnRecorder ─────────────────────────────────────────────────────

RealtimeTurnRecorder::RealtimeTurnRecorder(std::string session_id)
    : session_id_(std::move(session_id)) {}

void RealtimeTurnRecorder::set_user(const std::string& final_transcript) {
    std::lock_guard<std::mutex> lk(mu_);
    user_ = final_transcript;
}

void RealtimeTurnRecorder::add_assistant(const std::string& delta) {
    std::lock_guard<std::mutex> lk(mu_);
    asst_ += delta;
}

void RealtimeTurnRecorder::commit() {
    std::string user, asst;
    {
        std::lock_guard<std::mutex> lk(mu_);
        user = std::move(user_); asst = std::move(asst_);
        user_.clear(); asst_.clear();
    }
    if (session_id_.empty() || (user.empty() && asst.empty())) return;
    try {
        nlohmann::json entry = nlohmann::json::object();
        entry["ts"] = now_iso8601();
        user = media::str_san::utf8_safe_ellipsize(
            user, pm::llm::k_session_memory_user_chars, "\u2026");
        asst = media::str_san::utf8_safe_ellipsize(
            asst, pm::llm::k_session_memory_asst_chars, "\u2026");
        if (!user.empty()) entry["user"] = std::move(user);
        if (!asst.empty()) entry["assistant"] = std::move(asst);
        entry["mode"] = "realtime";
        session_append_turn(session_id_, entry);
    } catch (...) {}
}

} // namespace media::llm::agent

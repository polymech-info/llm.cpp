#include "llm/realtime_voice_agent.hpp"

#if defined(FEATURE_STT) && FEATURE_STT && defined(FEATURE_REALTIME) && FEATURE_REALTIME

#include "core/realtime_voice_session_ipc.hpp"
#include "core/chat_settings_schema.hpp"
#include "llm/agent_memory.hpp"
#include "logging/logger.h"

#include <chrono>
#include "fs/path_sanitizer.hpp"

namespace media::llm::agent::realtime {

namespace {
std::int64_t mono_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

VoiceAgent::VoiceAgent() = default;

VoiceAgent::~VoiceAgent()
{
    stop();
    join_watcher_if_foreign();
}

void VoiceAgent::bump_activity() { last_activity_ms_.store(mono_ms()); }

void VoiceAgent::join_watcher_if_foreign()
{
    if (stop_watcher_.joinable()
        && stop_watcher_.get_id() != std::this_thread::get_id())
        stop_watcher_.join();
}

bool VoiceAgent::init(Options opt, Hooks hooks, std::string& err)
{
    hooks_ = std::move(hooks);
    bump_activity();

    // ── Realtime (WS voice model) provider — honors router/model overrides ──
    {
        launch::AgentLaunchSpec rt_spec = opt.spec;
        rt_spec.api_mode = "realtime";
        launch::AgentLaunchResolved resolved;
        std::string rerr;
        if (!launch::resolve_agent_launch(rt_spec, resolved, rerr)) {
            err = "realtime provider resolve failed: " + rerr;
            return false;
        }
        rt_prov_ = resolved.provider;
    }
    if (rt_prov_.api_key.empty()) {
        err = "no API key for the realtime provider. Configure a chat preset / "
              "provider (api_key is resolved there).";
        return false;
    }
    cfg_.base_url = rt_prov_.base_url;
    cfg_.api_key  = rt_prov_.api_key;
    cfg_.model    = rt_prov_.model;
    cfg_.mic_send_enabled = opt.mic_send_enabled;
    cfg_.speaker_send_enabled = opt.speaker_send_enabled;

    // ── Shared session defaults: tools / persona / voice / devices ──────────
    apply_session_defaults(cfg_, opt.setup);
    if (!opt.extra_instructions.empty())
        cfg_.instructions += opt.extra_instructions;

    // ── Agent session (continuity + persistence) ─────────────────────────────
    session_id_ = opt.session_id.empty() ? session_create() : opt.session_id;
    recorder_   = std::make_shared<RealtimeTurnRecorder>(session_id_);

    // ── Brain provider: the text agent (/responses), NOT the realtime model.
    // router/model/api_key/base_url overrides target the WS model and are
    // deliberately NOT threaded here; the brain follows the chat preset. ─────
    brain_prov_ = rt_prov_;
    launch_spec_ = opt.spec;
    {
        launch::AgentLaunchSpec brain_spec = opt.spec;
        brain_spec.router.clear();
        brain_spec.model.clear();
        brain_spec.api_key.clear();
        brain_spec.base_url.clear();
        brain_spec.api_mode = "responses";
        launch::AgentLaunchResolved resolved;
        std::string berr;
        if (launch::resolve_agent_launch(brain_spec, resolved, berr)) {
            brain_prov_       = resolved.provider;
            launch_resolved_  = std::move(resolved);
            brain_launch_ok_  = true;
        } else {
            logger::error("VoiceAgent: brain provider resolve failed: " + berr);
        }
    }

    // ── Cross-process singleton: one live voice session per machine ──────────
    // Held for the lifetime of this VoiceAgent regardless of host (UI or
    // headless), so realtime-status / the assistant toggle / --realtime-stop
    // see and control every session uniformly.
    if (opt.acquire_realtime_singleton) {
        ipc_owner_ = std::make_unique<pm::audio::rt_ipc::Owner>();
        pm::audio::rt_ipc::StartInfo sinfo;
        sinfo.model       = cfg_.model;
        sinfo.voice       = cfg_.voice;
        sinfo.session_id  = session_id_;
        sinfo.session_dir = media::path::path_to_utf8(session_store_dir());
        const std::int64_t deadline = mono_ms() + opt.acquire_timeout_ms;
        std::string oerr;
        while (!ipc_owner_->acquire(sinfo, oerr)) {
            if (mono_ms() >= deadline) {
                ipc_owner_.reset();
                err = oerr;
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // ── Voice session + agent bridge ─────────────────────────────────────────
    voice_ = std::make_unique<pm::audio::RealtimeVoiceSession>();

    RealtimeAgentBridge::Config bridge_cfg;
    bridge_cfg.session_id          = session_id_;
    bridge_cfg.provider            = brain_prov_;
    bridge_cfg.launch_spec         = launch_spec_;
    bridge_cfg.launch_resolved     = launch_resolved_;
    bridge_cfg.launch_resolved_ok  = brain_launch_ok_;
    bridge_cfg.mode                = RealtimeAgentMode::Singleton;
    if (launch_spec_.runner.empty()) {
        if (const char* env = std::getenv("PM_LLM_AGENT_RUNNER")) {
            if (*env)
                launch_spec_.runner = env;
        }
    }
    if (launch_spec_.runner.empty() && brain_launch_ok_)
        launch_spec_.runner = launch_resolved_.effective_runner;
    if (launch_spec_.runner.empty())
        launch_spec_.runner = "native";
    launch_spec_.runner =
        media::settings_schema::normalize_chat_preset_runner(launch_spec_.runner);
    bridge_cfg.agent_runner        = launch_spec_.runner;
    delegated_agent_runner_        = launch_spec_.runner;
    bridge_cfg.embedded_prompt_suffix = opt.delegated_embed_suffix;
    bridge_cfg.chat_session_id        = opt.chat_session_id;
    bridge_cfg.on_event         = hooks_.on_agent_event;
    bridge_cfg.context_provider = hooks_.context_provider;
    bridge_cfg.on_started       = hooks_.on_agent_started;
    bridge_cfg.suspend_voice    = [this](std::string& serr) {
        if (!ipc_owner_) {
            serr = "no active live voice session";
            return false;
        }
        return pm::audio::rt_ipc::request_stop(serr);
    };

    bridge_ = std::make_unique<RealtimeAgentBridge>(
        std::move(bridge_cfg),
        [this](const std::string& call_id, const std::string& output_json) {
            if (voice_) voice_->send_tool_result(call_id, output_json);
            if (hooks_.on_agent_result) {
                try { hooks_.on_agent_result(call_id, output_json); } catch (...) {}
            }
        });

    logger::info("VoiceAgent: ready (session=" + session_id_
                 + " realtime=" + rt_prov_.router + "/" + rt_prov_.model
                 + " brain=" + brain_prov_.router + "/" + brain_prov_.model
                 + " runner=" + delegated_agent_runner_ + ")");
    return true;
}

bool VoiceAgent::start(std::string& err)
{
    if (!voice_ || !bridge_) { err = "VoiceAgent: start() before init()"; return false; }

    auto recorder = recorder_;
    pm::audio::RealtimeVoiceSession::Callbacks cb;
    cb.on_ready = [this] {
        bump_activity();
        if (hooks_.on_ready) hooks_.on_ready();
    };
    cb.on_text = [this](const std::string& d) {
        if (hooks_.on_text) hooks_.on_text(d);
    };
    cb.on_transcript = [this, recorder](const std::string& d) {
        recorder->add_assistant(d);
        if (hooks_.on_transcript) hooks_.on_transcript(d);
    };
    cb.on_user_transcript = [this, recorder](const std::string& t, bool fin) {
        bump_activity();
        if (fin && !t.empty()) recorder->set_user(t);
        if (hooks_.on_user_transcript) hooks_.on_user_transcript(t, fin);
    };
    cb.on_response_done = [this, recorder] {
        bump_activity();
        recorder->commit();
        if (hooks_.on_response_done) hooks_.on_response_done();
    };
    cb.on_level = [this](double level) {
        if (hooks_.on_level) hooks_.on_level(level);
    };
    cb.on_tool_call = [this](const std::string& name, const std::string& call_id,
                             const nlohmann::json& args) {
        bump_activity();
        if (hooks_.on_handover) {
            try { hooks_.on_handover(name, args); } catch (...) {}
        }
        if (bridge_) bridge_->handle_tool_call(name, call_id, args);
    };
    cb.on_error = [this](const std::string& e) {
        if (hooks_.on_error) hooks_.on_error(e);
    };
    cb.on_auth_error = [this](const std::string& e) {
        if (hooks_.on_auth_error) hooks_.on_auth_error(e);
    };
    cb.on_network_error = [this](const std::string& e) {
        if (hooks_.on_network_error) hooks_.on_network_error(e);
    };
    cb.on_closed = [this] {
        stopping_.store(true);
        if (bridge_) bridge_->stop();
        if (ipc_owner_) ipc_owner_->set_state(pm::audio::rt_ipc::State::Done);
        join_watcher_if_foreign();
        ipc_owner_.reset();
        if (hooks_.on_closed) hooks_.on_closed();
    };

    if (!voice_->start(cfg_, std::move(cb), err)) {
        bridge_->stop(); // no session — join any (impossible-yet) workers and bail
        return false;
    }

    // Honor cross-process stop requests (assistant toggle / --realtime-stop)
    // for any host: UI sessions have no wait loop, so the owner is watched here.
    if (ipc_owner_ && !stop_watcher_.joinable()) {
        stop_watcher_ = std::thread([this] {
            while (!stopping_.load()) {
                if (ipc_owner_->stop_requested()) {
                    logger::info("VoiceAgent: cross-process stop requested");
                    stop(); // join skipped for self (join_watcher_if_foreign)
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        });
    }
    return true;
}

VoiceAgent::RunResult VoiceAgent::run(const RunPolicy& policy, std::string& err)
{
    if (!start(err)) return RunResult::StartError;

    if (!policy.seed_prompt.empty())
        send_text(policy.seed_prompt);

    // Idle auto-stop guards headless launches that have no window to close:
    // once nothing has happened for idle_auto_stop_ms AND no delegated
    // run_pm_agent is in flight, the session self-stops. A busy agent keeps it
    // alive — and keeps bumping the clock so the full idle window restarts
    // cleanly after a long task finishes.
    bool idle_stop = false;
    while (!(policy.cancel && policy.cancel())
           && !stop_requested()
           && is_active()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (policy.idle_auto_stop_ms > 0) {
            if (agent_busy()) {
                bump_activity();
            } else if (mono_ms() - last_activity_ms_.load() >= policy.idle_auto_stop_ms) {
                idle_stop = true;
                break;
            }
        }
    }
    if (idle_stop)
        logger::info("VoiceAgent: no activity for "
                     + std::to_string(policy.idle_auto_stop_ms)
                     + " ms — auto-stopping idle live voice session");

    stop();
    return (policy.cancel && policy.cancel()) ? RunResult::Cancelled : RunResult::Ok;
}

void VoiceAgent::stop()
{
    stopping_.store(true);
    // Flip the IPC state first so status readers see the teardown, then join
    // delegated agent workers BEFORE closing the voice session they send tool
    // results into.
    if (ipc_owner_) ipc_owner_->set_state(pm::audio::rt_ipc::State::Stopping);
    if (bridge_) bridge_->stop();
    if (voice_)  voice_->stop();
    join_watcher_if_foreign();
    ipc_owner_.reset();
}

bool VoiceAgent::is_active() const { return voice_ && voice_->is_active(); }

bool VoiceAgent::stop_requested() const
{
    return ipc_owner_ && ipc_owner_->stop_requested();
}

bool VoiceAgent::agent_busy() const { return bridge_ && bridge_->is_busy(); }

bool VoiceAgent::cancel_agent(const std::string& run_call_id)
{
    return bridge_ && bridge_->cancel(run_call_id);
}

void VoiceAgent::send_text(const std::string& text)
{
    if (voice_) voice_->send_text(text);
}

void VoiceAgent::finish_input_turn()
{
    if (voice_) voice_->finish_input_turn();
}

VoiceAgent::RunResult run_voice_agent(VoiceAgent::Options opt,
                                      VoiceAgent::Hooks hooks,
                                      const VoiceAgent::RunPolicy& policy,
                                      std::string& err)
{
    VoiceAgent agent;
    if (!agent.init(std::move(opt), std::move(hooks), err))
        return VoiceAgent::RunResult::StartError;
    return agent.run(policy, err);
}

} // namespace media::llm::agent::realtime

#endif // FEATURE_STT && FEATURE_REALTIME

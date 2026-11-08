#include "agent_scheduler.hpp"
#include "path_tool_executor.hpp"

#include "string/string_sanitizers.hpp"
#include "logging/logger.h"

#include <chrono>
#include <sstream>
#include <thread>

namespace media::llm::agent {

namespace {
constexpr size_t k_recent_events_cap = 10;
}

AgentScheduler::AgentScheduler()  = default;
AgentScheduler::~AgentScheduler() { stop(); }

void AgentScheduler::configure(ProviderFactory factory, EventCallback on_tick_event) {
    std::unique_lock<std::mutex> lk(config_mutex_);
    factory_  = std::move(factory);
    on_event_ = std::move(on_tick_event);
}

void AgentScheduler::start() {
    if (running_.exchange(true)) return;
    stop_requested_ = false;
    thread_ = std::thread([this] { loop(); });
    logger::info("[agent_scheduler] started");
}

void AgentScheduler::stop() {
    stop_requested_ = true;
    // Wake the inter-tick sleep so the loop exits immediately.
    {
        std::lock_guard<std::mutex> lk(sleep_mu_);
        sleep_cv_.notify_all();
    }
    media::llm::path::abort_active_speak(); // unblock any in-progress TTS play_sync
    media::llm::path::abort_active_run();   // kill any in-progress shell child process
    if (thread_.joinable()) {
        thread_.join();
    }
    running_ = false;
}

void AgentScheduler::loop() {
    while (!stop_requested_) {
        try {
            const auto now   = std::chrono::system_clock::now();
            auto       tasks = task_store_list();

            for (auto& task : tasks) {
                if (!task.enabled)           continue;
                if (!task.next_run_at)       continue;
                if (now < *task.next_run_at) continue;
                try {
                    run_due_task(std::move(task));
                } catch (const std::exception& ex) {
                    logger::warn(std::string("[scheduler] run_due_task threw: ") + ex.what());
                } catch (...) {
                    logger::warn("[scheduler] run_due_task threw unknown exception");
                }
            }
        } catch (const std::exception& ex) {
            logger::warn(std::string("[scheduler] loop iteration threw: ") + ex.what());
        } catch (...) {
            logger::warn("[scheduler] loop iteration threw unknown exception");
        }

        // Sleep until next tick OR stop() wakes us via sleep_cv_.
        {
            std::unique_lock<std::mutex> lk(sleep_mu_);
            sleep_cv_.wait_for(lk, std::chrono::seconds(1), [this] {
                return stop_requested_.load();
            });
        }
    }
    logger::info("[scheduler] loop exited");
    running_ = false;
}

void AgentScheduler::run_due_task(ScheduledTask task) {
    // Re-read provider settings fresh every tick so settings changes take effect.
    ProviderFactory  factory;
    EventCallback    ev_cb;
    {
        std::unique_lock<std::mutex> lk(config_mutex_);
        factory = factory_;
        ev_cb   = on_event_;
    }

    if (!factory) {
        logger::warn("[scheduler] no provider factory configured — skipping task " + task.id);
        return;
    }

    ProviderSettings prov = factory();
    const std::string task_id = task.id;

    {
        std::ostringstream s;
        s << "[scheduler] tick  task=" << task_id
          << "  title=\"" << task.title << "\""
          << "  run#" << (task.run_count + 1);
        if (task.schedule.kind == ScheduleKind::Every && task.schedule.max_runs)
            s << "/" << *task.schedule.max_runs;
        s << "  router=" << prov.router << "  model=" << prov.model;
        logger::info(s.str());
    }

    // Prompt preview (first 120 chars)
    {
        std::string preview = task.prompt;
        preview = media::str_san::utf8_safe_ellipsize(preview, 120);
        logger::info("[scheduler] prompt: " + preview);
    }

    // Recent events for context
    const size_t ev_start =
        task.events.size() > k_recent_events_cap
            ? task.events.size() - k_recent_events_cap
            : 0;
    nlohmann::json recent = nlohmann::json::array();
    for (size_t i = ev_start; i < task.events.size(); ++i)
        recent.push_back(task.events[i]);

    Turn t;
    t.user_prompt       = task.prompt;
    t.task_id           = task_id;
    t.memory_state      = task.memory_state;
    t.recent_events     = std::move(recent);
    t.is_scheduled_tick = true;
    t.folder_hint       = task.folder_hint;

    // run_turn sets/clears current_task_id via TaskIdScope.
    Result result = run_turn(t, prov, ev_cb);

    const auto now = std::chrono::system_clock::now();

    // Log result
    {
        std::ostringstream s;
        s << "[scheduler] tick done  task=" << task_id
          << "  ok=" << (result.ok ? "yes" : "no")
          << "  iterations=" << result.iterations;
        if (!result.ok && !result.error.empty())
            s << "  error=" << result.error;
        logger::info(s.str());
    }
    if (result.ok && !result.final_text.empty()) {
        std::string preview = result.final_text;
        preview = media::str_san::utf8_safe_ellipsize(preview, 120);
        logger::info("[scheduler] assistant: " + preview);
    }

    nlohmann::json ev;
    ev["time"] = now_iso8601();
    ev["type"] = "tick_result";
    ev["ok"]   = result.ok;
    if (!result.final_text.empty()) ev["final_text"] = result.final_text;
    if (!result.error.empty())      ev["error"]      = result.error;
    task_store_append_event(task_id, ev);

    // Reload — memory_write during the turn may have mutated memory_state.
    auto reloaded = task_store_get(task_id);
    if (!reloaded) return;

    ScheduledTask& rt = *reloaded;
    rt.run_count++;
    rt.last_run_at = now;

    if (result.ok) {
        rt.consecutive_failures = 0;
        rt.last_error.clear();
    } else {
        rt.consecutive_failures++;
        rt.last_error = result.error;
    }

    const auto& spec = rt.schedule;
    if (spec.kind == ScheduleKind::Every && spec.interval) {
        rt.next_run_at = now + *spec.interval;
        if (spec.max_runs && rt.run_count >= *spec.max_runs) {
            rt.enabled = false;
            logger::info("[scheduler] task " + task_id + " reached max_runs — disabled");
        } else {
            logger::info("[scheduler] task " + task_id + " next run in "
                         + std::to_string(spec.interval->count()) + "s");
        }
    } else {
        // One-shot: At or In
        rt.enabled     = false;
        rt.next_run_at = std::nullopt;
        logger::info("[scheduler] one-shot task " + task_id + " complete — disabled");
    }

    task_store_update(rt);
}

AgentScheduler& global_scheduler() {
    static AgentScheduler s;
    return s;
}

} // namespace media::llm::agent

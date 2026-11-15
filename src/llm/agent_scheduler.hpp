#pragma once
//
// media::llm::agent::AgentScheduler — background thread that fires due tasks.
//
// Usage:
//   auto& sched = global_scheduler();
//   sched.configure(provider);
//   sched.start();
//   // …
//   sched.stop();
//
// The scheduler wakes every second and calls run_due. That claims each due
// task, runs an agent turn or posts a notification, then arms the next time.
// A second caller for the same claim does not run it again.
//
#include "agent.hpp"
#include "agent_memory.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

namespace media::llm::agent {

struct AgentTickResult {
    bool        ran = false;
    bool        ok = false;
    std::string text;
    std::string error;
};

class AgentScheduler {
public:
    AgentScheduler();
    ~AgentScheduler();

    /// Factory called once per tick to get fresh provider settings (reads live settings.json).
    /// Set before calling start(); may be re-set at any time (mutex-protected).
    using ProviderFactory = std::function<ProviderSettings()>;
    void configure(ProviderFactory   factory,
                   EventCallback     on_tick_event = nullptr);

    void start();
    /// Ask the loop to leave. Does not abort process-global TTS or shell.
    /// The in-flight scheduled turn sees this through `Turn::cancel_check`.
    /// A second `stop()` waits until the thread that owns the join has finished.
    void stop();

    bool is_running() const noexcept { return running_.load(); }

    AgentScheduler(const AgentScheduler&)            = delete;
    AgentScheduler& operator=(const AgentScheduler&) = delete;

private:
    void loop();
    AgentTickResult run_agent_tick(const ScheduledTask& task);

    mutable std::mutex config_mutex_;
    ProviderFactory    factory_;
    EventCallback      on_event_;

    enum class Life { Stopped, Running, Stopping };

    std::atomic<bool>       stop_requested_{false};
    std::atomic<bool>       running_{false};
    Life                    state_ = Life::Stopped;
    std::mutex              life_mu_;
    std::condition_variable life_cv_;
    std::thread             thread_;
    std::thread::id         worker_id_{};
    // Sleep gate: stop() wakes the inter-tick wait immediately instead of
    // letting the join() block on the 1 s sleep_for fallback.
    std::mutex              sleep_mu_;
    std::condition_variable sleep_cv_;
};

/// Process-wide singleton. Configure + start once at app startup.
AgentScheduler& global_scheduler();

/// Run every task that is due at `now`. `run_agents` false leaves agent tasks
/// unclaimed so a background wakeup cannot start a model turn.
/// Finish bookkeeping uses the later of `now` and `now_source` (the wall
/// clock when `now_source` is empty), so a long turn arms its next interval
/// from completion. `should_stop` is checked before each new claim. `posted`
/// lists reminders still waiting for handoff, each with its own `delivery_id`.
/// `wakeup.work` and `wakeup.alarm` are independent.
nlohmann::json run_due(std::chrono::system_clock::time_point now,
                       bool run_agents,
                       std::function<AgentTickResult(const ScheduledTask&)> run_agent,
                       std::function<bool()> should_stop = {},
                       std::function<std::chrono::system_clock::time_point()> now_source = {});

/// Next best-effort reminder and next time-sensitive reminder, separately.
nlohmann::json schedule_next_wakeup();

/// Reminder payloads still waiting on disk. Taking them does not acknowledge.
void retain_schedule_notes(const nlohmann::json& report);
nlohmann::json take_schedule_notes();

/// Store-level checks for claim, cancel, notification, and a missed interval.
/// Uses the current schedule directory. Erases the tasks it creates.
nlohmann::json scheduler_run_due_selftest();

} // namespace media::llm::agent

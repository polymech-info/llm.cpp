#include "agent_scheduler.hpp"

#include "logging/logger.h"

#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>
#include <vector>

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
    std::unique_lock<std::mutex> life(life_mu_);
    life_cv_.wait(life, [this] { return state_ != Life::Stopping; });
    if (state_ == Life::Running)
        return;
    stop_requested_ = false;
    state_ = Life::Running;
    running_ = true;
    try {
        thread_ = std::thread([this] { loop(); });
        worker_id_ = thread_.get_id();
    } catch (...) {
        state_ = Life::Stopped;
        running_ = false;
        worker_id_ = {};
        throw;
    }
    logger::info("[agent_scheduler] started");
}

void AgentScheduler::stop() {
    std::unique_lock<std::mutex> life(life_mu_);
    if (worker_id_ == std::this_thread::get_id()) {
        stop_requested_ = true;
        std::lock_guard<std::mutex> sleep(sleep_mu_);
        sleep_cv_.notify_all();
        return;
    }
    if (state_ == Life::Stopping) {
        life_cv_.wait(life, [this] { return state_ != Life::Stopping; });
        return;
    }
    if (state_ == Life::Stopped)
        return;
    stop_requested_ = true;
    {
        std::lock_guard<std::mutex> sleep(sleep_mu_);
        sleep_cv_.notify_all();
    }
    std::thread worker = std::move(thread_);
    state_ = Life::Stopping;
    running_ = false;
    life.unlock();
    if (worker.joinable())
        worker.join();
    life.lock();
    if (state_ == Life::Stopping) {
        state_ = Life::Stopped;
        worker_id_ = {};
    }
    life_cv_.notify_all();
}

void AgentScheduler::loop() {
    while (!stop_requested_) {
        try {
            bool run_agents = false;
            {
                std::unique_lock<std::mutex> lk(config_mutex_);
                run_agents = static_cast<bool>(factory_);
            }
            run_due(std::chrono::system_clock::now(), run_agents,
                    [this](const ScheduledTask& task) { return run_agent_tick(task); },
                    [this] { return stop_requested_.load(); });
        } catch (const std::exception& ex) {
            logger::warn(std::string("[scheduler] loop iteration threw: ") + ex.what());
        } catch (...) {
            logger::warn("[scheduler] loop iteration threw unknown exception");
        }

        std::unique_lock<std::mutex> lk(sleep_mu_);
        sleep_cv_.wait_for(lk, std::chrono::seconds(1), [this] {
            return stop_requested_.load();
        });
    }
    logger::info("[scheduler] loop exited");
}

AgentTickResult AgentScheduler::run_agent_tick(const ScheduledTask& task) {
    ProviderFactory factory;
    EventCallback ev_cb;
    {
        std::unique_lock<std::mutex> lk(config_mutex_);
        factory = factory_;
        ev_cb = on_event_;
    }
    if (!factory)
        return {};

    ProviderSettings prov = factory();
    const std::string task_id = task.id;
    {
        std::ostringstream s;
        s << "[scheduler] tick  task=" << task_id
          << "  kind=" << execution_kind_id(task.execution)
          << "  prompt_chars=" << task.prompt.size()
          << "  run#" << (task.run_count + 1);
        if (task.schedule.kind == ScheduleKind::Every && task.schedule.max_runs)
            s << "/" << *task.schedule.max_runs;
        s << "  router=" << prov.router << "  model=" << prov.model;
        logger::info(s.str());
    }

    nlohmann::json recent = nlohmann::json::array();
    const size_t ev_start = task.events.size() > k_recent_events_cap
                                ? task.events.size() - k_recent_events_cap
                                : 0;
    for (size_t i = ev_start; i < task.events.size(); ++i)
        recent.push_back(task.events[i]);

    Turn turn;
    turn.user_prompt = task.prompt;
    turn.task_id = task_id;
    turn.memory_state = task.memory_state;
    turn.recent_events = std::move(recent);
    turn.is_scheduled_tick = true;
    turn.folder_hint = task.folder_hint;
    turn.cancel_check = [this] { return stop_requested_.load(); };

    Result result = run_turn(turn, prov, ev_cb);
    {
        std::ostringstream s;
        s << "[scheduler] tick done  task=" << task_id
          << "  ok=" << (result.ok ? "yes" : "no")
          << "  iterations=" << result.iterations;
        if (!result.ok && !result.error.empty())
            s << "  error=" << result.error;
        logger::info(s.str());
    }

    AgentTickResult out;
    out.ran = true;
    out.ok = result.ok;
    out.text = result.final_text;
    out.error = result.error;
    return out;
}

namespace {

nlohmann::json wakeup_slot(const ScheduledTask* task) {
    if (!task || !task->next_run_at)
        return nullptr;
    return {{"task_id", task->id}, {"at", format_iso8601(*task->next_run_at)}};
}

nlohmann::json wakeup_for(std::chrono::system_clock::time_point /*now*/) {
    const auto tasks = task_store_list();
    const ScheduledTask* work = nullptr;
    const ScheduledTask* alarm = nullptr;
    for (const auto& task : tasks) {
        const char* kind = schedule_wakeup_kind(task);
        if (std::strcmp(kind, "work") == 0) {
            if (!work || *task.next_run_at < *work->next_run_at)
                work = &task;
        } else if (std::strcmp(kind, "alarm") == 0) {
            if (!alarm || *task.next_run_at < *alarm->next_run_at)
                alarm = &task;
        }
    }
    return {{"work", wakeup_slot(work)}, {"alarm", wakeup_slot(alarm)}};
}

std::chrono::system_clock::time_point finished_at(
    std::chrono::system_clock::time_point dispatched,
    const std::function<std::chrono::system_clock::time_point()>& now_source) {
    const auto wall = now_source ? now_source() : std::chrono::system_clock::now();
    return wall > dispatched ? wall : dispatched;
}

} // namespace

void retain_schedule_notes(const nlohmann::json& /*report*/) {}

nlohmann::json take_schedule_notes() {
    nlohmann::json posted = nlohmann::json::array();
    for (const auto& task : task_store_list()) {
        for (const auto& row : task.pending_deliveries) {
            posted.push_back({
                {"delivery_id", row.delivery_id},
                {"task_id", task.id},
                {"occurrence_id", row.occurrence_id},
                {"title", row.title},
                {"text", row.text},
                {"sound", row.sound},
                {"status", "pending"},
            });
        }
    }
    return {{"posted", std::move(posted)}};
}

nlohmann::json schedule_next_wakeup() {
    return wakeup_for(std::chrono::system_clock::now());
}

nlohmann::json run_due(std::chrono::system_clock::time_point now,
                       bool run_agents,
                       std::function<AgentTickResult(const ScheduledTask&)> run_agent,
                       std::function<bool()> should_stop,
                       std::function<std::chrono::system_clock::time_point()> now_source) {
    int agent_runs = 0;
    int notification_runs = 0;
    int interrupted = 0;
    int blocked = 0;
    nlohmann::json posted = nlohmann::json::array();
    const auto tasks = task_store_list();
    for (const auto& listed : tasks) {
        if (should_stop && should_stop())
            break;
        if (!listed.enabled || !listed.next_run_at || now < *listed.next_run_at)
            continue;
        const DueBeginResult began = task_store_begin_due(listed.id, now, run_agents);
        if (began.action == DueBeginAction::Skip)
            continue;
        if (began.action == DueBeginAction::Blocked) {
            ++blocked;
            continue;
        }
        if (began.action == DueBeginAction::Interrupted) {
            ++interrupted;
            continue;
        }

        const auto done_at = finished_at(now, now_source);
        switch (began.task.execution) {
        case ExecutionKind::Notification: {
            if (!task_store_finish_due(began.task.id, began.generation, done_at, true,
                                       began.task.prompt, "", true))
                break;
            posted.push_back({
                {"delivery_id", began.task.occurrence_id},
                {"task_id", began.task.id},
                {"occurrence_id", began.task.occurrence_id},
                {"title", began.task.title},
                {"text", began.task.prompt},
                {"sound", began.task.sound},
                {"status", "pending"},
            });
            ++notification_runs;
            break;
        }
        case ExecutionKind::Agent: {
            AgentTickResult tick;
            try {
                if (run_agent)
                    tick = run_agent(began.task);
            } catch (const std::exception& ex) {
                tick.ran = true;
                tick.ok = false;
                tick.error = ex.what();
            } catch (...) {
                tick.ran = true;
                tick.ok = false;
                tick.error = "run_due agent threw";
            }
            if (!tick.ran) {
                task_store_release_unrun(began.task.id, began.generation);
                break;
            }
            if (task_store_finish_due(began.task.id, began.generation, finished_at(now, now_source),
                                      tick.ok, tick.text, tick.error, false))
                ++agent_runs;
            break;
        }
        default: {
            logger::warn("[scheduler] unsupported execution task=" + began.task.id);
            task_store_finish_due(began.task.id, began.generation, done_at, false, "",
                                  "unsupported execution", false);
            break;
        }
        }
    }
    return {
        {"agent_runs", agent_runs},
        {"notification_runs", notification_runs},
        {"interrupted", interrupted},
        {"blocked", blocked},
        {"posted", std::move(posted)},
        {"wakeup", wakeup_for(now)},
    };
}

namespace {

const auto k_selftest_now = std::chrono::system_clock::from_time_t(1'800'000'000);

AgentTickResult ok_tick(int& calls) {
    ++calls;
    AgentTickResult result;
    result.ran = true;
    result.ok = true;
    result.text = "done";
    return result;
}

nlohmann::json due_at(bool run_agents,
                      std::function<AgentTickResult(const ScheduledTask&)> run_agent,
                      std::function<bool()> should_stop = {}) {
    return run_due(k_selftest_now, run_agents, std::move(run_agent), std::move(should_stop),
                   [] { return k_selftest_now; });
}

ScheduledTask make_due(const char* title, ScheduleKind kind) {
    ScheduledTask task;
    task.title = title;
    task.prompt = "probe";
    task.schedule.kind = kind;
    if (kind == ScheduleKind::Every)
        task.schedule.interval = std::chrono::seconds(10);
    else
        task.schedule.delay = std::chrono::seconds(10);
    task.next_run_at = k_selftest_now - std::chrono::hours(1);
    return task;
}

} // namespace

nlohmann::json scheduler_run_due_selftest() {
    nlohmann::json out{{"ok", false}};
    std::vector<std::string> ids;
    const bool was_running = global_scheduler().is_running();
    global_scheduler().stop();
    const auto previous_sessions = session_store_dir();
    set_session_store_dir(previous_sessions / "run-due-selftest");
    const auto erase_all = [&] {
        for (const auto& id : ids)
            task_store_erase(id);
    };
    try {
        task_store_clear_all();

        {
            AgentScheduler local;
            std::atomic<bool> open{false};
            auto hammer = [&] {
                while (!open.load())
                    std::this_thread::yield();
                for (int i = 0; i < 25; ++i) {
                    local.start();
                    local.stop();
                }
            };
            std::thread first(hammer);
            std::thread second(hammer);
            open = true;
            first.join();
            second.join();
            local.stop();
            if (local.is_running())
                out["error"] = "concurrent start/stop left the scheduler running";
        }

        int calls = 0;
        const auto agent = [&](const ScheduledTask&) { return ok_tick(calls); };

        if (!out.contains("error")) {
            const std::string id = task_store_create(make_due("due-once", ScheduleKind::In));
            ids.push_back(id);
            const auto first = due_at(true, agent);
            const auto loaded = task_store_get(id);
            const int calls_after = calls;
            due_at(true, agent);
            if (first.value("agent_runs", 0) != 1 || calls_after != 1 || calls != 1 || !loaded
                || loaded->enabled || loaded->occurrence_status != OccurrenceStatus::Completed)
                out["error"] = "one-shot agent did not run once and disable";
        }

        if (!out.contains("error")) {
            ScheduledTask every = make_due("due-every", ScheduleKind::Every);
            every.schedule.max_runs = 3;
            const std::string id = task_store_create(std::move(every));
            ids.push_back(id);
            calls = 0;
            due_at(true, agent);
            auto loaded = task_store_get(id);
            const auto expected_next = k_selftest_now + std::chrono::seconds(10);
            if (calls != 1 || !loaded || loaded->run_count != 1 || !loaded->enabled
                || !loaded->next_run_at || *loaded->next_run_at != expected_next)
                out["error"] = "every did not arm one interval after completion";
            else {
                due_at(true, agent);
                if (calls != 1)
                    out["error"] = "every ran a second time before it was due";
            }
            if (!out.contains("error")) {
                loaded = task_store_get(id);
                loaded->next_run_at = k_selftest_now;
                task_store_update(*loaded);
                const auto fail = [&](const ScheduledTask&) {
                    ++calls;
                    AgentTickResult result;
                    result.ran = true;
                    result.ok = false;
                    result.error = "nope";
                    return result;
                };
                due_at(true, fail);
                loaded = task_store_get(id);
                if (!loaded || loaded->run_count != 2 || loaded->consecutive_failures != 1
                    || !loaded->enabled)
                    out["error"] = "failed every tick did not count and stay enabled";
            }
            if (!out.contains("error")) {
                loaded = task_store_get(id);
                loaded->next_run_at = k_selftest_now;
                task_store_update(*loaded);
                due_at(true, agent);
                loaded = task_store_get(id);
                if (!loaded || loaded->run_count != 3 || loaded->enabled)
                    out["error"] = "every did not disable at max_runs";
            }
        }

        if (!out.contains("error")) {
            ScheduledTask note = make_due("due-note", ScheduleKind::In);
            note.execution = ExecutionKind::Notification;
            note.delivery = DeliveryKind::Notification;
            note.sound = true;
            const std::string id = task_store_create(std::move(note));
            ids.push_back(id);
            calls = 0;
            due_at(true, agent);
            const auto loaded = task_store_get(id);
            bool note_event = false;
            if (loaded) {
                for (const auto& ev : loaded->events) {
                    if (ev.value("type", std::string{}) == "notification" && ev.value("sound", false))
                        note_event = true;
                }
            }
            const auto pending = take_schedule_notes();
            std::string delivery;
            if (pending.contains("posted") && pending["posted"].is_array()) {
                for (const auto& row : pending["posted"]) {
                    if (row.value("task_id", std::string{}) != id)
                        continue;
                    if (row.value("status", std::string{}) == "pending")
                        delivery = row.value("delivery_id", std::string{});
                }
            }
            const bool acked_task = task_store_ack_handoff(id);
            const auto still = task_store_get(id);
            const bool acked = !delivery.empty() && task_store_ack_handoff(delivery);
            const auto after = task_store_get(id);
            if (calls != 0 || !loaded || loaded->enabled || !note_event
                || loaded->occurrence_status != OccurrenceStatus::Completed
                || loaded->pending_deliveries.size() != 1 || delivery.empty() || acked_task
                || !still || still->pending_deliveries.size() != 1 || !acked || !after
                || after->handoff_pending || !after->pending_deliveries.empty())
                out["error"] = "notification handoff was not acknowledged by delivery";
        }

        if (!out.contains("error")) {
            ScheduledTask note = make_due("due-two", ScheduleKind::Every);
            note.execution = ExecutionKind::Notification;
            note.delivery = DeliveryKind::Notification;
            note.schedule.interval = std::chrono::seconds(10);
            const std::string id = task_store_create(std::move(note));
            ids.push_back(id);
            due_at(true, agent);
            auto loaded = task_store_get(id);
            if (!loaded || loaded->pending_deliveries.size() != 1)
                out["error"] = "first reminder delivery was not kept";
            else {
                loaded->next_run_at = k_selftest_now;
                task_store_update(*loaded);
                due_at(true, agent);
                loaded = task_store_get(id);
                if (!loaded || loaded->pending_deliveries.size() != 2)
                    out["error"] = "second reminder replaced the first delivery";
                else {
                    const auto first_delivery = loaded->pending_deliveries[0].delivery_id;
                    const auto second_delivery = loaded->pending_deliveries[1].delivery_id;
                    if (!task_store_ack_handoff(first_delivery))
                        out["error"] = "first delivery ack failed";
                    else {
                        loaded = task_store_get(id);
                        if (!loaded || !loaded->handoff_pending || loaded->pending_deliveries.size() != 1
                            || loaded->pending_deliveries[0].delivery_id != second_delivery
                            || !task_store_ack_handoff(second_delivery))
                            out["error"] = "acking one delivery cleared the other";
                        else {
                            loaded = task_store_get(id);
                            if (!loaded || loaded->handoff_pending || !loaded->pending_deliveries.empty())
                                out["error"] = "acking one delivery cleared the other";
                        }
                    }
                }
            }
        }

        if (!out.contains("error")) {
            ScheduledTask bad = make_due("due-kind", ScheduleKind::In);
            bad.execution = static_cast<ExecutionKind>(99);
            const std::string id = task_store_create(std::move(bad));
            ids.push_back(id);
            calls = 0;
            due_at(true, agent);
            const auto loaded = task_store_get(id);
            if (calls != 0 || !loaded || loaded->enabled
                || loaded->occurrence_status != OccurrenceStatus::Failed
                || loaded->last_error != "unsupported execution")
                out["error"] = "unsupported execution stayed claimed";
        }

        if (!out.contains("error")) {
            ScheduledTask stale = make_due("due-stale", ScheduleKind::In);
            stale.occurrence_status = OccurrenceStatus::Running;
            stale.occurrence_planned_at = stale.next_run_at;
            stale.occurrence_id = "stale";
            const std::string id = task_store_create(std::move(stale));
            ids.push_back(id);
            calls = 0;
            const auto report = due_at(true, agent);
            const auto loaded = task_store_get(id);
            if (calls != 0 || report.value("interrupted", 0) < 1 || !loaded || loaded->enabled
                || loaded->occurrence_status != OccurrenceStatus::Interrupted)
                out["error"] = "stale running claim was executed";
        }

        if (!out.contains("error")) {
            const std::string id = task_store_create(make_due("due-cancel", ScheduleKind::In));
            ids.push_back(id);
            task_store_cancel(id);
            calls = 0;
            due_at(true, agent);
            const auto loaded = task_store_get(id);
            if (calls != 0 || !loaded || loaded->enabled)
                out["error"] = "cancelled task ran";
        }

        if (!out.contains("error")) {
            const std::string id = task_store_create(make_due("due-race", ScheduleKind::In));
            ids.push_back(id);
            calls = 0;
            due_at(true, [&](const ScheduledTask& task) {
                ++calls;
                task_store_cancel(task.id);
                AgentTickResult result;
                result.ran = true;
                result.ok = true;
                result.text = "late";
                return result;
            });
            const auto loaded = task_store_get(id);
            if (calls != 1 || !loaded || loaded->enabled)
                out["error"] = "cancel during the tick was overwritten";
        }

        if (!out.contains("error")) {
            const std::string id = task_store_create(make_due("due-back", ScheduleKind::In));
            ids.push_back(id);
            const auto before = task_store_get(id);
            calls = 0;
            const auto report = due_at(false, agent);
            const auto loaded = task_store_get(id);
            if (calls != 0 || report.value("blocked", 0) < 1 || !loaded || !loaded->enabled
                || !before || loaded->next_run_at != before->next_run_at
                || loaded->occurrence_status == OccurrenceStatus::Running)
                out["error"] = "background agent run was claimed";
        }

        if (!out.contains("error")) {
            ScheduledTask note = make_due("due-back-note", ScheduleKind::In);
            note.execution = ExecutionKind::Notification;
            note.delivery = DeliveryKind::Notification;
            const std::string id = task_store_create(std::move(note));
            ids.push_back(id);
            calls = 0;
            due_at(false, agent);
            const auto loaded = task_store_get(id);
            if (calls != 0 || !loaded || loaded->enabled)
                out["error"] = "background notification did not complete";
        }

        erase_all();
        ids.clear();
        if (!out.contains("error")) {
            int nested = 0;
            const std::string id = task_store_create(make_due("due-nest", ScheduleKind::In));
            ids.push_back(id);
            due_at(true, [&](const ScheduledTask&) {
                ++nested;
                due_at(true, [&](const ScheduledTask&) {
                    ++nested;
                    return ok_tick(nested);
                });
                AgentTickResult result;
                result.ran = true;
                result.ok = true;
                return result;
            });
            const auto loaded = task_store_get(id);
            if (nested != 1 || !loaded || loaded->enabled
                || loaded->occurrence_status != OccurrenceStatus::Completed)
                out["error"] = "overlapping run_due executed the same claim";
        }

        erase_all();
        ids.clear();
        if (!out.contains("error")) {
            const std::string id = task_store_create(make_due("due-threads", ScheduleKind::In));
            ids.push_back(id);
            std::atomic<int> raced{0};
            auto runner = [&](const ScheduledTask&) {
                ++raced;
                for (int i = 0; i < 20; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                AgentTickResult result;
                result.ran = true;
                result.ok = true;
                result.text = "once";
                return result;
            };
            std::thread left([&] { due_at(true, runner); });
            std::thread right([&] { due_at(true, runner); });
            left.join();
            right.join();
            const auto loaded = task_store_get(id);
            if (raced.load() != 1 || !loaded || loaded->enabled
                || loaded->occurrence_status != OccurrenceStatus::Completed)
                out["error"] = "two threads both finished the same claim";
        }

        ScheduledTask alarm_task;
        alarm_task.execution = ExecutionKind::Notification;
        alarm_task.precision = SchedulePrecision::TimeSensitive;
        alarm_task.enabled = true;
        alarm_task.next_run_at = k_selftest_now + std::chrono::hours(2);
        ScheduledTask work_task;
        work_task.execution = ExecutionKind::Notification;
        work_task.enabled = true;
        work_task.next_run_at = k_selftest_now + std::chrono::hours(1);
        ScheduledTask agent_task;
        agent_task.execution = ExecutionKind::Agent;
        agent_task.enabled = true;
        agent_task.next_run_at = k_selftest_now - std::chrono::hours(1);
        if (!out.contains("error")
            && (std::strcmp(schedule_wakeup_kind(alarm_task), "alarm") != 0
                || std::strcmp(schedule_wakeup_kind(work_task), "work") != 0
                || std::strcmp(schedule_wakeup_kind(agent_task), "none") != 0))
            out["error"] = "wakeup kind mismatched the executor";
        if (!out.contains("error")) {
            ScheduledTask blocked = alarm_task;
            blocked.precision = SchedulePrecision::BestEffort;
            blocked.occurrence_status = OccurrenceStatus::Blocked;
            blocked.next_run_at = k_selftest_now - std::chrono::hours(1);
            alarm_task.id = task_store_create(alarm_task);
            work_task.id = task_store_create(work_task);
            agent_task.id = task_store_create(agent_task);
            blocked.id = task_store_create(blocked);
            ids.push_back(alarm_task.id);
            ids.push_back(work_task.id);
            ids.push_back(agent_task.id);
            ids.push_back(blocked.id);
            const auto wake = schedule_next_wakeup();
            const auto work = wake.value("work", nlohmann::json{});
            const auto alarm = wake.value("alarm", nlohmann::json{});
            const auto work_id = work.value("task_id", std::string{});
            const auto alarm_id = alarm.value("task_id", std::string{});
            if (!work.is_object() || work_id != work_task.id
                || !alarm.is_object() || alarm_id != alarm_task.id
                || work_id == agent_task.id || alarm_id == agent_task.id
                || work_id == blocked.id || alarm_id == blocked.id)
                out["error"] = "work and alarm wakeups were not independent";
        }

        erase_all();
        out["ok"] = !out.contains("error");
        if (out["ok"].get<bool>())
            out.erase("error");
    } catch (const std::exception& ex) {
        erase_all();
        set_session_store_dir(previous_sessions);
        if (was_running)
            global_scheduler().start();
        out["ok"] = false;
        out["error"] = ex.what();
        return out;
    } catch (...) {
        erase_all();
        set_session_store_dir(previous_sessions);
        if (was_running)
            global_scheduler().start();
        out["ok"] = false;
        out["error"] = "run_due selftest failed";
        return out;
    }
    set_session_store_dir(previous_sessions);
    if (was_running)
        global_scheduler().start();
    return out;
}

AgentScheduler& global_scheduler() {
    static AgentScheduler s;
    return s;
}

namespace {
struct StopSchedulerOnProfileSwap {
    StopSchedulerOnProfileSwap() {
        set_session_dir_hook([] { global_scheduler().stop(); });
    }
};
const StopSchedulerOnProfileSwap k_stop_scheduler_on_profile_swap;
}

} // namespace media::llm::agent

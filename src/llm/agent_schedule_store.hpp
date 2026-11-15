#pragma once
// Scheduled-task persistence. Session memory lives in agent_memory.hpp.
#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace media::llm::agent {

enum class ScheduleKind {
    At,     // absolute one-shot   — run_at
    In,     // relative one-shot   — delay
    Every,  // repeating interval  — interval [+ start_at] [+ max_runs]
    Daily,  // wall clock in a timezone — hour, minute, timezone
};

/// What a due task runs. Missing JSON means Agent, so older files stay agent turns.
enum class ExecutionKind {
    Agent,
    Notification,
    Tool,
};

enum class DeliveryKind {
    Inbox,
    Notification,
    Silent,
};

enum class SchedulePrecision {
    BestEffort,
    TimeSensitive,
};

enum class OccurrenceStatus {
    None,
    Pending,
    Running,
    Completed,
    Failed,
    Cancelled,
    Interrupted,
    Blocked,
    Deferred,
};

struct ScheduleSpec {
    ScheduleKind kind = ScheduleKind::Every;

    // At
    std::optional<std::chrono::system_clock::time_point> run_at;

    // In
    std::optional<std::chrono::seconds> delay;

    // Every
    std::optional<std::chrono::seconds> interval;
    std::optional<std::chrono::system_clock::time_point> start_at;
    std::optional<int> max_runs;

    // Daily. timezone is an IANA name. Empty means UTC.
    int hour = 0;
    int minute = 0;
    std::string timezone;
};

struct ScheduledTask {
    // ── definition ────────────────────────────────────────────────────────────
    std::string  id;
    std::string  title;
    std::string  prompt;
    ScheduleSpec schedule;
    bool         enabled    = true;
    std::string  created_at;
    std::string  updated_at;
    std::string  folder_hint;   // optional — injected into scheduled Turn
    ExecutionKind execution = ExecutionKind::Agent;
    DeliveryKind  delivery  = DeliveryKind::Inbox;
    SchedulePrecision precision = SchedulePrecision::BestEffort;
    bool         sound = false;
    /// Frozen call. Set at create and not rewritten when the task fires.
    std::string  tool_name;
    nlohmann::json tool_args = nlohmann::json::object();
    /// Written with the claim, before the call. An interrupted claim keeps it.
    std::string  idempotency_key;

    // ── runtime state ─────────────────────────────────────────────────────────
    std::optional<std::chrono::system_clock::time_point> next_run_at;
    std::optional<std::chrono::system_clock::time_point> last_run_at;
    int         run_count            = 0;
    std::string last_error;
    int         consecutive_failures = 0;
    /// Bumped on cancel and on each terminal write. A finish whose generation
    /// does not match leaves `enabled` as it is.
    int         generation = 0;
    std::string occurrence_id;
    OccurrenceStatus occurrence_status = OccurrenceStatus::None;
    std::optional<std::chrono::system_clock::time_point> occurrence_planned_at;
    /// One undelivered reminder. Recurring tasks can have several at once.
    /// `handoff_pending` is true exactly when this list is not empty.
    struct PendingDelivery {
        std::string delivery_id;
        std::string occurrence_id;
        std::string title;
        std::string text;
        bool        sound = false;
    };
    std::vector<PendingDelivery> pending_deliveries;
    bool handoff_pending = false;

    // ── memory (compact JSON kept small) ─────────────────────────────────────
    nlohmann::json memory_state = nlohmann::json::object();

    // ── event log (append-only, capped at k_max_events) ──────────────────────
    std::vector<nlohmann::json> events;
};

// ── Store API (all thread-safe) ──────────────────────────────────────────────

/// Assigns a unique id if task.id is empty. Returns the id.
std::string task_store_create(ScheduledTask task);

std::optional<ScheduledTask> task_store_get(const std::string& id);
std::vector<ScheduledTask>   task_store_list();

/// Full replacement (id must exist). Returns false if not found.
bool task_store_update(const ScheduledTask& task);

/// Mark as disabled (does not erase) and bump generation. Returns false if not found.
bool task_store_cancel(const std::string& id);

enum class DueBeginAction {
    Skip,
    Claimed,
    Interrupted,
    Blocked,
};

struct DueBeginResult {
    DueBeginAction action = DueBeginAction::Skip;
    ScheduledTask  task;
    int            generation = 0;
};

/// Claim one due task, or settle a claim left `running` by a dead process.
/// `run_agents` false skips agent and tool tasks without writing them
/// (notifications still claim).
DueBeginResult task_store_begin_due(const std::string& id,
                                    std::chrono::system_clock::time_point now,
                                    bool run_agents);

/// Finish the claim from `begin`. A cancel that landed during the run wins:
/// `enabled` stays false and the agent runner's copy is not written back.
bool task_store_finish_due(const std::string& id,
                           int expected_generation,
                           std::chrono::system_clock::time_point now,
                           bool ok,
                           const std::string& text,
                           const std::string& error,
                           bool notification_event,
                           const nlohmann::json& files = {});

/// Drop a claim when the agent runner was not invoked. Does not re-enable a cancel.
void task_store_release_unrun(const std::string& id, int expected_generation);

/// Drop one pending delivery after the host has accepted that payload.
/// `delivery_id` is the occurrence id, not the task id.
bool task_store_ack_handoff(const std::string& delivery_id);

/// Erase every scheduler task from memory and delete `schedules/*.json`.
/// Does not touch chat sessions. Callers that only need a reload use
/// `task_store_drop_cache`.
void task_store_clear_all();

/// Forget the in-memory task map. The next get/list/create reloads
/// `schedules/*.json`. Does not delete files.
void task_store_drop_cache();

/// Remove one scheduler task from memory and delete its schedule file.
bool task_store_erase(const std::string& id);

/// Create one far-future task, reload it from disk, cancel it, and erase it.
/// Other tasks in the current schedule directory are left alone.
nlohmann::json scheduler_persistence_selftest();

bool schedule_write_memory(const std::string& task_id, const nlohmann::json& state);
void schedule_append_event(const std::string& task_id, const nlohmann::json& event);

/// Drop the in-memory schedule cache. The next read reloads schedules/*.json.
void schedule_store_invalidate();

nlohmann::json task_to_json(const ScheduledTask& t);
nlohmann::json task_to_summary_json(const ScheduledTask& t);
/// What create tells the person: host, next time, where the result goes, and the limit.
nlohmann::json task_saved_json(const ScheduledTask& t);
const char* execution_kind_id(ExecutionKind kind);
const char* precision_id(SchedulePrecision precision);
const char* occurrence_status_id(OccurrenceStatus status);
const char* schedule_wakeup_kind(const ScheduledTask& task);

/// The calls a frozen tool task may name. Camera, shell, and delete stay off.
bool scheduled_tool_allowed(const std::string& name);

/// Next daily instant strictly after `after`. The Android host installs a
/// zone converter. Without one, the hour is read as UTC.
using ScheduleDailyClock = std::optional<std::chrono::system_clock::time_point> (*)(
    std::chrono::system_clock::time_point after,
    int hour,
    int minute,
    const std::string& timezone);
void set_schedule_daily_clock(ScheduleDailyClock clock);

} // namespace media::llm::agent

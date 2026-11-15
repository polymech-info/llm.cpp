#include "agent_schedule_store.hpp"
#include "agent_store_io.hpp"

#include "logging/logger.h"
#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include "fs/path_sanitizer.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace media::llm::agent {

namespace {

constexpr size_t k_max_events = 50;

struct ScheduleStore {
    std::mutex mutex;
    std::unordered_map<std::string, ScheduledTask> tasks;
    std::unordered_set<std::string> live;
    int next_seq = 1;
    bool tasks_loaded = false;
    int loaded_generation = -1;
};

ScheduleStore& store() {
    static ScheduleStore s;
    return s;
}

std::string gen_id(const std::string& title) {
    auto& s = store();
    std::string safe;
    for (unsigned char c : title) {
        if (std::isalnum(c))
            safe += static_cast<char>(std::tolower(c));
        else if (!safe.empty() && safe.back() != '-')
            safe += '-';
        if (safe.size() >= 20) break;
    }
    while (!safe.empty() && safe.back() == '-') safe.pop_back();
    if (safe.empty()) safe = "task";
    return safe + "-" + std::to_string(s.next_seq++);
}

} // namespace

const char* execution_kind_id(ExecutionKind kind) {
    switch (kind) {
        case ExecutionKind::Notification: return "notification";
        case ExecutionKind::Tool:         return "tool";
        case ExecutionKind::Agent:        break;
    }
    return "agent";
}

bool scheduled_tool_allowed(const std::string& name) {
    return name == "write_file" || name == "http_request";
}

const char* precision_id(SchedulePrecision precision) {
    return precision == SchedulePrecision::TimeSensitive ? "time_sensitive" : "best_effort";
}

const char* occurrence_status_id(OccurrenceStatus status) {
    switch (status) {
        case OccurrenceStatus::Pending:     return "pending";
        case OccurrenceStatus::Running:     return "running";
        case OccurrenceStatus::Completed:   return "completed";
        case OccurrenceStatus::Failed:      return "failed";
        case OccurrenceStatus::Cancelled:   return "cancelled";
        case OccurrenceStatus::Interrupted: return "interrupted";
        case OccurrenceStatus::Blocked:     return "blocked";
        case OccurrenceStatus::Deferred:    return "deferred";
        case OccurrenceStatus::None:        break;
    }
    return "none";
}

const char* schedule_wakeup_kind(const ScheduledTask& task) {
    if (!task.enabled || !task.next_run_at)
        return "none";
    if (task.occurrence_status == OccurrenceStatus::Blocked)
        return "none";
    // Agent and tool tasks wake as "work". On Android the host also arms an
    // AlarmManager catch-up when that work is under 15 minutes away. The
    // time_sensitive alarm stays a reminder-only path.
    if (task.execution != ExecutionKind::Notification)
        return "work";
    if (task.precision == SchedulePrecision::TimeSensitive)
        return "alarm";
    return "work";
}

namespace {

const char* delivery_kind_id(DeliveryKind kind) {
    switch (kind) {
        case DeliveryKind::Notification: return "notification";
        case DeliveryKind::Silent:       return "silent";
        case DeliveryKind::Inbox:        break;
    }
    return "inbox";
}

ExecutionKind execution_kind_from(const std::string& text) {
    if (text == "notification") return ExecutionKind::Notification;
    if (text == "tool") return ExecutionKind::Tool;
    return ExecutionKind::Agent;
}

DeliveryKind delivery_kind_from(const std::string& text) {
    if (text == "notification") return DeliveryKind::Notification;
    if (text == "silent") return DeliveryKind::Silent;
    return DeliveryKind::Inbox;
}

SchedulePrecision precision_from(const std::string& text) {
    if (text == "time_sensitive") return SchedulePrecision::TimeSensitive;
    return SchedulePrecision::BestEffort;
}

OccurrenceStatus occurrence_status_from(const std::string& text) {
    if (text == "pending") return OccurrenceStatus::Pending;
    if (text == "running") return OccurrenceStatus::Running;
    if (text == "completed") return OccurrenceStatus::Completed;
    if (text == "failed") return OccurrenceStatus::Failed;
    if (text == "cancelled") return OccurrenceStatus::Cancelled;
    if (text == "interrupted") return OccurrenceStatus::Interrupted;
    if (text == "blocked") return OccurrenceStatus::Blocked;
    if (text == "deferred") return OccurrenceStatus::Deferred;
    return OccurrenceStatus::None;
}

void push_event_locked(ScheduledTask& task, nlohmann::json event) {
    task.events.push_back(std::move(event));
    if (task.events.size() > k_max_events) {
        const auto extra = task.events.size() - k_max_events;
        task.events.erase(task.events.begin(),
                          task.events.begin() + static_cast<std::ptrdiff_t>(extra));
    }
}

ScheduleDailyClock g_daily_clock = nullptr;

std::optional<std::chrono::system_clock::time_point> next_daily_utc(
    std::chrono::system_clock::time_point after, int hour, int minute) {
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59)
        return std::nullopt;
    const auto after_s = std::chrono::system_clock::to_time_t(after);
    std::tm tm{};
#if defined(_WIN32)
    if (gmtime_s(&tm, &after_s) != 0)
        return std::nullopt;
#else
    if (!gmtime_r(&after_s, &tm))
        return std::nullopt;
#endif
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = 0;
#if defined(_WIN32)
    const auto candidate = _mkgmtime(&tm);
#else
    const auto candidate = timegm(&tm);
#endif
    if (candidate == static_cast<std::time_t>(-1))
        return std::nullopt;
    auto when = std::chrono::system_clock::from_time_t(candidate);
    if (when <= after)
        when += std::chrono::hours(24);
    return when;
}

std::optional<std::chrono::system_clock::time_point> next_daily_instant(
    std::chrono::system_clock::time_point after, int hour, int minute, const std::string& timezone) {
    if (g_daily_clock) {
        if (const auto zoned = g_daily_clock(after, hour, minute, timezone))
            return zoned;
    }
    return next_daily_utc(after, hour, minute);
}

void arm_after_attempt_locked(ScheduledTask& task,
                               const std::chrono::system_clock::time_point& now) {
    task.run_count++;
    task.last_run_at = now;
    const auto& spec = task.schedule;
    if (spec.kind == ScheduleKind::Every && spec.interval) {
        task.next_run_at = now + *spec.interval;
        if (spec.max_runs && task.run_count >= *spec.max_runs)
            task.enabled = false;
    } else if (spec.kind == ScheduleKind::Daily) {
        task.next_run_at = next_daily_instant(now, spec.hour, spec.minute, spec.timezone);
        if (!task.next_run_at || (spec.max_runs && task.run_count >= *spec.max_runs))
            task.enabled = false;
    } else {
        task.enabled = false;
        task.next_run_at = std::nullopt;
    }
    task.generation += 1;
}

} // namespace

// ── Serialization ────────────────────────────────────────────────────────────

nlohmann::json task_to_json(const ScheduledTask& t) {
    nlohmann::json j;
    j["id"]                   = t.id;
    j["title"]                = t.title;
    j["prompt"]               = t.prompt;
    j["enabled"]              = t.enabled;
    j["created_at"]           = t.created_at;
    j["updated_at"]           = t.updated_at;
    j["run_count"]            = t.run_count;
    j["consecutive_failures"] = t.consecutive_failures;
    if (!t.last_error.empty())   j["last_error"]   = t.last_error;
    if (!t.folder_hint.empty())  j["folder_hint"]  = t.folder_hint;
    if (t.generation != 0) j["generation"] = t.generation;
    if (t.execution != ExecutionKind::Agent)
        j["execution"] = {{"kind", execution_kind_id(t.execution)}};
    if (t.delivery != DeliveryKind::Inbox || t.sound) {
        nlohmann::json delivery = {{"kind", delivery_kind_id(t.delivery)}};
        if (t.sound) delivery["sound"] = true;
        j["delivery"] = std::move(delivery);
    }
    if (t.precision != SchedulePrecision::BestEffort)
        j["policy"] = {{"precision", precision_id(t.precision)}};
    if (!t.pending_deliveries.empty()) {
        nlohmann::json rows = nlohmann::json::array();
        for (const auto& row : t.pending_deliveries) {
            rows.push_back({
                {"delivery_id", row.delivery_id},
                {"task_id", t.id},
                {"occurrence_id", row.occurrence_id},
                {"title", row.title},
                {"text", row.text},
                {"sound", row.sound},
                {"status", "pending"},
            });
        }
        j["pending_deliveries"] = std::move(rows);
        j["handoff_pending"] = true;
    }
    if (t.occurrence_status != OccurrenceStatus::None || !t.occurrence_id.empty()) {
        nlohmann::json occurrence = {{"status", occurrence_status_id(t.occurrence_status)}};
        if (!t.occurrence_id.empty()) occurrence["id"] = t.occurrence_id;
        if (!t.idempotency_key.empty()) occurrence["idempotency_key"] = t.idempotency_key;
        if (t.occurrence_planned_at)
            occurrence["planned_at"] = format_iso8601(*t.occurrence_planned_at);
        j["occurrence"] = std::move(occurrence);
    }
    j["memory_state"]         = t.memory_state;

    nlohmann::json sched;
    switch (t.schedule.kind) {
        case ScheduleKind::At:    sched["kind"] = "at";    break;
        case ScheduleKind::In:    sched["kind"] = "in";    break;
        case ScheduleKind::Every: sched["kind"] = "every"; break;
        case ScheduleKind::Daily: sched["kind"] = "daily"; break;
    }
    if (t.schedule.interval) sched["interval_seconds"] = static_cast<int>(t.schedule.interval->count());
    if (t.schedule.delay)    sched["delay_seconds"]    = static_cast<int>(t.schedule.delay->count());
    if (t.schedule.max_runs) sched["max_runs"]         = *t.schedule.max_runs;
    if (t.schedule.run_at)   sched["run_at"]           = format_iso8601(*t.schedule.run_at);
    if (t.schedule.start_at) sched["start_at"]         = format_iso8601(*t.schedule.start_at);
    if (t.schedule.kind == ScheduleKind::Daily) {
        sched["hour"] = t.schedule.hour;
        sched["minute"] = t.schedule.minute;
        if (!t.schedule.timezone.empty()) sched["timezone"] = t.schedule.timezone;
    }
    if (t.execution == ExecutionKind::Tool || !t.tool_name.empty()) {
        j["tool"] = {
            {"name", t.tool_name},
            {"arguments", t.tool_args.is_object() ? t.tool_args : nlohmann::json::object()},
        };
    }
    j["schedule"]             = std::move(sched);

    if (t.next_run_at) j["next_run_at"] = format_iso8601(*t.next_run_at);
    if (t.last_run_at) j["last_run_at"] = format_iso8601(*t.last_run_at);

    if (!t.events.empty()) {
        j["events"] = t.events;
    }
    return j;
}

static int json_int_field(const nlohmann::json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_number()) return 0;
    try { return j[key].get<int>(); }
    catch (...) { return 0; }
}

static bool task_from_json(const nlohmann::json& j, ScheduledTask& t) {
    try {
        if (!j.is_object() || !j.contains("id") || !j["id"].is_string()) return false;
        t = {};
        t.id = j["id"].get<std::string>();
        if (t.id.empty()) return false;
        t.title = j.value("title", std::string{});
        t.prompt = j.value("prompt", std::string{});
        t.enabled = j.value("enabled", true);
        t.created_at = j.value("created_at", std::string{});
        t.updated_at = j.value("updated_at", std::string{});
        t.folder_hint = j.value("folder_hint", std::string{});
        t.run_count = json_int_field(j, "run_count");
        t.consecutive_failures = json_int_field(j, "consecutive_failures");
        t.last_error = j.value("last_error", std::string{});
        t.generation = json_int_field(j, "generation");
        t.handoff_pending = j.value("handoff_pending", false);
        if (j.contains("execution") && j["execution"].is_object())
            t.execution = execution_kind_from(j["execution"].value("kind", std::string{}));
        if (j.contains("delivery") && j["delivery"].is_object()) {
            t.delivery = delivery_kind_from(j["delivery"].value("kind", std::string{}));
            t.sound = j["delivery"].value("sound", false);
        }
        if (j.contains("policy") && j["policy"].is_object())
            t.precision = precision_from(j["policy"].value("precision", std::string{}));
        if (j.contains("occurrence") && j["occurrence"].is_object()) {
            const auto& occurrence = j["occurrence"];
            t.occurrence_status = occurrence_status_from(occurrence.value("status", std::string{}));
            t.occurrence_id = occurrence.value("id", std::string{});
            t.idempotency_key = occurrence.value("idempotency_key", std::string{});
            if (occurrence.contains("planned_at") && occurrence["planned_at"].is_string())
                t.occurrence_planned_at = parse_iso8601(occurrence["planned_at"].get<std::string>());
        }
        if (j.contains("tool") && j["tool"].is_object()) {
            t.tool_name = j["tool"].value("name", std::string{});
            if (j["tool"].contains("arguments") && j["tool"]["arguments"].is_object())
                t.tool_args = j["tool"]["arguments"];
        }
        if (j.contains("memory_state") && j["memory_state"].is_object())
            t.memory_state = j["memory_state"];
        if (j.contains("events") && j["events"].is_array()) {
            const auto& arr = j["events"];
            const std::size_t n = arr.size();
            const std::size_t start = n > k_max_events ? n - k_max_events : 0;
            t.events.reserve(n - start);
            for (std::size_t i = start; i < n; ++i)
                t.events.push_back(arr[i]);
        }
        if (j.contains("pending_deliveries") && j["pending_deliveries"].is_array()) {
            for (const auto& row : j["pending_deliveries"]) {
                if (!row.is_object()) continue;
                ScheduledTask::PendingDelivery delivery;
                delivery.delivery_id = row.value("delivery_id", std::string{});
                delivery.occurrence_id = row.value("occurrence_id", std::string{});
                delivery.title = row.value("title", t.title);
                delivery.text = row.value("text", std::string{});
                delivery.sound = row.value("sound", false);
                if (delivery.delivery_id.empty()) continue;
                t.pending_deliveries.push_back(std::move(delivery));
            }
        }
        if (t.pending_deliveries.empty() && t.handoff_pending) {
            ScheduledTask::PendingDelivery delivery;
            delivery.occurrence_id = t.occurrence_id;
            delivery.delivery_id = t.occurrence_id.empty() ? t.id : t.occurrence_id;
            delivery.title = t.title;
            delivery.text = t.prompt;
            delivery.sound = t.sound;
            for (auto it = t.events.rbegin(); it != t.events.rend(); ++it) {
                if (it->value("type", std::string{}) != "notification") continue;
                delivery.text = it->value("text", delivery.text);
                delivery.sound = it->value("sound", delivery.sound);
                if (delivery.title.empty())
                    delivery.title = it->value("title", std::string{});
                break;
            }
            t.pending_deliveries.push_back(std::move(delivery));
        }
        t.handoff_pending = !t.pending_deliveries.empty();
        if (j.contains("schedule") && j["schedule"].is_object()) {
            const auto& sched = j["schedule"];
            const auto kind = sched.value("kind", std::string{"every"});
            if (kind == "at") t.schedule.kind = ScheduleKind::At;
            else if (kind == "in") t.schedule.kind = ScheduleKind::In;
            else if (kind == "daily") t.schedule.kind = ScheduleKind::Daily;
            else t.schedule.kind = ScheduleKind::Every;
            if (sched.contains("hour") && sched["hour"].is_number())
                t.schedule.hour = json_int_field(sched, "hour");
            if (sched.contains("minute") && sched["minute"].is_number())
                t.schedule.minute = json_int_field(sched, "minute");
            if (sched.contains("timezone") && sched["timezone"].is_string())
                t.schedule.timezone = sched["timezone"].get<std::string>();
            if (sched.contains("interval_seconds") && sched["interval_seconds"].is_number())
                t.schedule.interval = std::chrono::seconds(json_int_field(sched, "interval_seconds"));
            if (sched.contains("delay_seconds") && sched["delay_seconds"].is_number())
                t.schedule.delay = std::chrono::seconds(json_int_field(sched, "delay_seconds"));
            if (sched.contains("max_runs") && sched["max_runs"].is_number())
                t.schedule.max_runs = json_int_field(sched, "max_runs");
            if (sched.contains("run_at") && sched["run_at"].is_string())
                t.schedule.run_at = parse_iso8601(sched["run_at"].get<std::string>());
            if (sched.contains("start_at") && sched["start_at"].is_string())
                t.schedule.start_at = parse_iso8601(sched["start_at"].get<std::string>());
        }
        if (j.contains("next_run_at") && j["next_run_at"].is_string())
            t.next_run_at = parse_iso8601(j["next_run_at"].get<std::string>());
        if (j.contains("last_run_at") && j["last_run_at"].is_string())
            t.last_run_at = parse_iso8601(j["last_run_at"].get<std::string>());
        return true;
    } catch (...) {
        return false;
    }
}

namespace {

const char* schedule_host_id() {
#if defined(__ANDROID__)
    return "android";
#else
    return "desktop";
#endif
}

void fill_schedule_receipt(const ScheduledTask& t, nlohmann::json& j) {
    const bool phone = std::string(schedule_host_id()) == "android";
    j["host"] = schedule_host_id();
    j["where"] = phone ? "This phone" : "This computer";
    j["delivery"] = delivery_kind_id(t.delivery);
    if (t.execution == ExecutionKind::Notification) {
        if (phone) {
            j["hears"] = "Phone notification";
            j["limit"] = t.precision == SchedulePrecision::TimeSensitive
                             ? "Exact only after exact alarms are allowed."
                             : "A short delay can fire late.";
        } else {
            j["hears"] = "Saved here";
            j["limit"] = "Does not notify the phone.";
        }
    } else if (t.execution == ExecutionKind::Tool) {
        j["hears"] = t.delivery == DeliveryKind::Notification ? (phone ? "Phone notification" : "Saved here")
                                                              : "Chat";
        j["limit"] = phone ? "Runs even when chat is closed. A short delay can fire late."
                           : "Runs while this app is open.";
    } else if (t.delivery == DeliveryKind::Silent) {
        j["hears"] = "Saved only";
        j["limit"] = "Nothing is shown when it finishes.";
    } else {
        j["hears"] = t.delivery == DeliveryKind::Notification ? (phone ? "Phone notification" : "Saved here")
                                                              : "Chat";
        j["limit"] = phone ? "Runs even when chat is closed. A short delay can fire late."
                           : "Runs while this app is open.";
    }
}

} // namespace

nlohmann::json task_saved_json(const ScheduledTask& t) {
    nlohmann::json j;
    j["task_id"] = t.id;
    j["title"] = t.title;
    j["execution"] = execution_kind_id(t.execution);
    if (t.next_run_at) j["next_run_at"] = format_iso8601(*t.next_run_at);
    fill_schedule_receipt(t, j);
    return j;
}

nlohmann::json task_to_summary_json(const ScheduledTask& t) {
    nlohmann::json j;
    j["id"]      = t.id;
    j["title"]   = t.title;
    j["enabled"] = t.enabled;
    j["run_count"] = t.run_count;
    switch (t.schedule.kind) {
        case ScheduleKind::At:    j["schedule_kind"] = "at";    break;
        case ScheduleKind::In:    j["schedule_kind"] = "in";    break;
        case ScheduleKind::Every: j["schedule_kind"] = "every"; break;
        case ScheduleKind::Daily: j["schedule_kind"] = "daily"; break;
    }
    if (t.schedule.interval) j["interval_seconds"] = static_cast<int>(t.schedule.interval->count());
    if (t.next_run_at)       j["next_run_at"]      = format_iso8601(*t.next_run_at);
    if (t.last_run_at)       j["last_run_at"]      = format_iso8601(*t.last_run_at);
    if (!t.last_error.empty() && t.consecutive_failures > 0) j["last_error"] = t.last_error;
    j["execution"] = execution_kind_id(t.execution);
    fill_schedule_receipt(t, j);
    if (t.occurrence_status != OccurrenceStatus::None)
        j["occurrence_status"] = occurrence_status_id(t.occurrence_status);
    return j;
}

// ── Optional CWD debug dump ─────────────────────────────────────────────────
// Off by default — historically these files leaked into every CLI invocation's
// working directory. Re-enable via PM_IMAGE_DEBUG_DUMPS=1 for local debugging.
// Session memory now persists via per-session files under session_store_dir().

static void dump_debug_files_locked(const ScheduleStore& s) {
    if (!debug_store_dumps_enabled()) return;

    nlohmann::json sched_arr = nlohmann::json::array();
    for (const auto& kv : s.tasks)
        sched_arr.push_back(task_to_json(kv.second));
    try { std::ofstream("scheduler.json") << sched_arr.dump(2); } catch (...) {}
}

// ── Compute initial next_run_at from spec ────────────────────────────────────

static std::optional<std::chrono::system_clock::time_point>
compute_initial_next_run(const ScheduleSpec& spec) {
    const auto now = std::chrono::system_clock::now();
    switch (spec.kind) {
        case ScheduleKind::At:
            return spec.run_at;
        case ScheduleKind::In:
            if (spec.delay) return now + *spec.delay;
            return std::nullopt;
        case ScheduleKind::Every:
            if (spec.start_at) return *spec.start_at;
            if (spec.interval) return now + *spec.interval;
            return std::nullopt;
        case ScheduleKind::Daily:
            return next_daily_instant(now - std::chrono::seconds(1), spec.hour, spec.minute, spec.timezone);
    }
    return std::nullopt;
}

void set_schedule_daily_clock(ScheduleDailyClock clock) {
    g_daily_clock = clock;
}

// ── Scheduler files ──────────────────────────────────────────────────────────

static fs::path schedule_file_for(const std::string& id) {
    const std::string fid = sanitize_session_id(id);
    if (fid.empty()) return {};
    return schedule_store_dir() / (fid + ".json");
}

static bool persist_task_locked(const ScheduledTask& t) {
    if (t.id.empty()) return false;
    const fs::path path = schedule_file_for(t.id);
    if (path.empty()) return false;
    if (!atomic_write_json(path, task_to_json(t))) {
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_ERROR, "pm-chat",
                            "schedule write failed id=%s", t.id.c_str());
#endif
        return false;
    }
    return true;
}

static void ensure_tasks_loaded_locked(ScheduleStore& s) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int gen = schedule_generation();
        if (s.tasks_loaded && s.loaded_generation == gen) return;
        s.tasks.clear();
        std::error_code ec;
        const fs::path dir = schedule_store_dir();
        if (fs::is_directory(dir, ec)) {
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                if (it->path().extension() != ".json") continue;
                nlohmann::json j;
                if (!read_json_file(it->path(), j)) continue;
                ScheduledTask loaded;
                if (!task_from_json(j, loaded) || loaded.id.empty()) continue;
                const std::string id = loaded.id;
                s.tasks.insert_or_assign(id, std::move(loaded));
            }
        }
        if (schedule_generation() == gen) {
            s.tasks_loaded = true;
            s.loaded_generation = gen;
            return;
        }
        s.tasks_loaded = false;
    }
    s.tasks_loaded = true;
    s.loaded_generation = schedule_generation();
}

// ── Store API ────────────────────────────────────────────────────────────────

std::string task_store_create(ScheduledTask task) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);

    if (task.id.empty()) {
        do {
            task.id = gen_id(task.title);
        } while (s.tasks.find(task.id) != s.tasks.end());
    }

    const std::string now = now_iso8601();
    task.created_at = now;
    task.updated_at = now;

    if (!task.next_run_at)
        task.next_run_at = compute_initial_next_run(task.schedule);

    const std::string id = task.id;
    if (!persist_task_locked(task)) {
        logger::warn("[scheduler] create persist failed id=" + id);
        return {};
    }
    s.tasks[id] = std::move(task);
    dump_debug_files_locked(s);
    return id;
}

std::optional<ScheduledTask> task_store_get(const std::string& id) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);
    auto it = s.tasks.find(id);
    if (it == s.tasks.end())
        return std::nullopt;
    return it->second;
}

std::vector<ScheduledTask> task_store_list() {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);
    std::vector<ScheduledTask> out;
    out.reserve(s.tasks.size());
    for (const auto& kv : s.tasks)
        out.push_back(kv.second);
    return out;
}

bool task_store_update(const ScheduledTask& task) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);
    auto it = s.tasks.find(task.id);
    if (it == s.tasks.end()) return false;
    ScheduledTask next = task;
    next.updated_at = now_iso8601();
    if (!persist_task_locked(next)) {
        logger::warn("[scheduler] update persist failed id=" + task.id);
        return false;
    }
    it->second = std::move(next);
    dump_debug_files_locked(s);
    return true;
}

bool task_store_cancel(const std::string& id) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);
    auto it = s.tasks.find(id);
    if (it == s.tasks.end()) return false;
    ScheduledTask next = it->second;
    next.enabled = false;
    next.generation += 1;
    if (next.occurrence_status == OccurrenceStatus::Running)
        next.occurrence_status = OccurrenceStatus::Cancelled;
    next.updated_at = now_iso8601();
    if (!persist_task_locked(next)) {
        logger::warn("[scheduler] cancel persist failed id=" + id);
        return false;
    }
    it->second = std::move(next);
    s.live.erase(id);
    dump_debug_files_locked(s);
    return true;
}

DueBeginResult task_store_begin_due(const std::string& id,
                                    std::chrono::system_clock::time_point now,
                                    bool run_agents) {
    DueBeginResult out;
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);
    auto it = s.tasks.find(id);
    if (it == s.tasks.end())
        return out;
    ScheduledTask& task = it->second;
    if (!task.enabled || !task.next_run_at || now < *task.next_run_at)
        return out;

    const bool live = s.live.count(id) != 0;
    if (task.occurrence_status == OccurrenceStatus::Running && live)
        return out;
    if (task.occurrence_status == OccurrenceStatus::Running && !live) {
        ScheduledTask next = task;
        next.occurrence_status = OccurrenceStatus::Interrupted;
        next.last_error = "interrupted";
        arm_after_attempt_locked(next, now);
        next.updated_at = now_iso8601();
        if (!persist_task_locked(next)) {
            logger::warn("[scheduler] interrupt persist failed id=" + id);
            return out;
        }
        task = std::move(next);
        dump_debug_files_locked(s);
        out.action = DueBeginAction::Interrupted;
        out.task = task;
        out.generation = task.generation;
        return out;
    }
    if ((task.execution == ExecutionKind::Agent || task.execution == ExecutionKind::Tool)
        && !run_agents) {
        out.action = DueBeginAction::Blocked;
        out.task = task;
        return out;
    }

    ScheduledTask next = task;
    next.occurrence_status = OccurrenceStatus::Running;
    next.occurrence_planned_at = next.next_run_at;
    next.occurrence_id = next.id + "|" + format_iso8601(*next.next_run_at);
    if (next.execution == ExecutionKind::Tool && next.idempotency_key.empty())
        next.idempotency_key = next.occurrence_id;
    next.updated_at = now_iso8601();
    if (!persist_task_locked(next)) {
        logger::warn("[scheduler] claim persist failed id=" + id);
        return out;
    }
    task = std::move(next);
    s.live.insert(id);
    dump_debug_files_locked(s);
    out.action = DueBeginAction::Claimed;
    out.task = task;
    out.generation = task.generation;
    return out;
}

namespace {

constexpr size_t k_pending_delivery_cap = 32;

void remember_delivery(ScheduledTask& task, const std::string& text) {
    ScheduledTask::PendingDelivery row;
    row.occurrence_id = task.occurrence_id;
    row.delivery_id = task.occurrence_id.empty() ? task.id + ":pending" : task.occurrence_id;
    for (const auto& existing : task.pending_deliveries) {
        if (existing.delivery_id == row.delivery_id) {
            task.handoff_pending = true;
            return;
        }
    }
    row.title = task.title;
    row.text = text;
    row.sound = task.sound;
    if (task.pending_deliveries.size() >= k_pending_delivery_cap) {
        logger::warn("[scheduler] dropped oldest pending delivery task=" + task.id
                     + " delivery=" + task.pending_deliveries.front().delivery_id);
        task.pending_deliveries.erase(task.pending_deliveries.begin());
    }
    task.pending_deliveries.push_back(std::move(row));
    task.handoff_pending = true;
}

} // namespace

bool task_store_finish_due(const std::string& id,
                           int expected_generation,
                           std::chrono::system_clock::time_point now,
                           bool ok,
                           const std::string& text,
                           const std::string& error,
                           bool notification_event,
                           const nlohmann::json& files) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);
    auto it = s.tasks.find(id);
    if (it == s.tasks.end()) {
        s.live.erase(id);
        logger::warn("[scheduler] finish missed task=" + id
                     + " generation=" + std::to_string(expected_generation));
        return false;
    }
    ScheduledTask& task = it->second;
    if (task.generation != expected_generation || !task.enabled) {
        s.live.erase(id);
        logger::info("[scheduler] finish lost the race task=" + id
                     + " generation=" + std::to_string(expected_generation)
                     + " current=" + std::to_string(task.generation)
                     + " enabled=" + (task.enabled ? "yes" : "no"));
        if (!task.enabled && task.occurrence_status == OccurrenceStatus::Running) {
            ScheduledTask next = task;
            next.occurrence_status = OccurrenceStatus::Cancelled;
            next.updated_at = now_iso8601();
            if (!persist_task_locked(next)) {
                logger::warn("[scheduler] finish persist failed task=" + id
                             + " generation=" + std::to_string(expected_generation));
                return false;
            }
            task = std::move(next);
            dump_debug_files_locked(s);
        }
        return false;
    }

    ScheduledTask next = task;
    nlohmann::json ev;
    ev["time"] = format_iso8601(now);
    ev["ok"] = ok;
    if (notification_event) {
        ev["type"] = "notification";
        ev["title"] = next.title;
        ev["text"] = text;
        ev["sound"] = next.sound;
    } else {
        ev["type"] = "tick_result";
        if (!next.occurrence_id.empty()) ev["run_id"] = next.occurrence_id;
        if (!text.empty()) ev["final_text"] = text;
        if (!error.empty()) ev["error"] = error;
        if (files.is_array()) {
            nlohmann::json kept = nlohmann::json::array();
            for (const auto& row : files) {
                if (!row.is_object())
                    continue;
                const std::string path = row.value("path", std::string{});
                if (path.empty())
                    continue;
                kept.push_back({
                    {"path", path},
                    {"tool", row.value("tool", std::string{})},
                });
            }
            if (!kept.empty())
                ev["files"] = std::move(kept);
        }
    }
    push_event_locked(next, std::move(ev));

    if (ok) {
        next.consecutive_failures = 0;
        next.last_error.clear();
        next.occurrence_status = OccurrenceStatus::Completed;
    } else {
        next.consecutive_failures++;
        next.last_error = error;
        next.occurrence_status = OccurrenceStatus::Failed;
    }
    arm_after_attempt_locked(next, now);
    if (notification_event && ok)
        remember_delivery(next, text);
    next.updated_at = now_iso8601();
    if (!persist_task_locked(next)) {
        logger::warn("[scheduler] finish persist failed task=" + id
                     + " generation=" + std::to_string(expected_generation));
        return false;
    }
    task = std::move(next);
    s.live.erase(id);
    dump_debug_files_locked(s);
    return true;
}

bool task_store_ack_handoff(const std::string& delivery_id) {
    if (delivery_id.empty())
        return false;
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);
    for (auto& kv : s.tasks) {
        ScheduledTask& task = kv.second;
        const auto found = std::find_if(task.pending_deliveries.begin(), task.pending_deliveries.end(),
                                         [&](const ScheduledTask::PendingDelivery& row) {
                                             return row.delivery_id == delivery_id;
                                         });
        if (found == task.pending_deliveries.end())
            continue;
        ScheduledTask next = task;
        const auto at = std::find_if(next.pending_deliveries.begin(), next.pending_deliveries.end(),
                                      [&](const ScheduledTask::PendingDelivery& row) {
                                          return row.delivery_id == delivery_id;
                                      });
        if (at == next.pending_deliveries.end())
            return false;
        next.pending_deliveries.erase(at);
        next.handoff_pending = !next.pending_deliveries.empty();
        next.updated_at = now_iso8601();
        if (!persist_task_locked(next)) {
            logger::warn("[scheduler] handoff ack persist failed delivery=" + delivery_id);
            return false;
        }
        task = std::move(next);
        dump_debug_files_locked(s);
        return true;
    }
    return false;
}

void task_store_release_unrun(const std::string& id, int expected_generation) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);
    auto it = s.tasks.find(id);
    if (it == s.tasks.end()) {
        s.live.erase(id);
        return;
    }
    ScheduledTask& task = it->second;
    if (task.generation != expected_generation || !task.enabled) {
        s.live.erase(id);
        if (!task.enabled && task.occurrence_status == OccurrenceStatus::Running) {
            ScheduledTask next = task;
            next.occurrence_status = OccurrenceStatus::Cancelled;
            next.updated_at = now_iso8601();
            if (persist_task_locked(next)) {
                task = std::move(next);
                dump_debug_files_locked(s);
            }
        }
        return;
    }
    if (task.occurrence_status != OccurrenceStatus::Running) {
        s.live.erase(id);
        return;
    }
    ScheduledTask next = task;
    next.occurrence_status = OccurrenceStatus::None;
    next.occurrence_id.clear();
    next.occurrence_planned_at = std::nullopt;
    next.updated_at = now_iso8601();
    if (!persist_task_locked(next)) {
        logger::warn("[scheduler] release persist failed id=" + id);
        return;
    }
    task = std::move(next);
    s.live.erase(id);
    dump_debug_files_locked(s);
}

void task_store_clear_all() {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);
    std::error_code ec;
    const fs::path dir = schedule_store_dir();
    std::vector<fs::path> doomed;
    if (fs::is_directory(dir, ec)) {
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const auto ext = media::path::path_to_utf8(it->path().extension());
            if (ext == ".json" || ext == ".tmp")
                doomed.push_back(it->path());
        }
    }
    for (const auto& path : doomed)
        fs::remove(path, ec);
    s.tasks.clear();
    s.live.clear();
    dump_debug_files_locked(s);
}

void task_store_drop_cache() {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    s.tasks.clear();
    s.tasks_loaded = false;
}

bool task_store_erase(const std::string& id) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);
    s.live.erase(id);
    const bool had = s.tasks.erase(id) > 0;
    std::error_code ec;
    const fs::path path = schedule_file_for(id);
    if (!path.empty()) {
        fs::remove(path, ec);
        fs::remove(media::path::path_from_utf8(media::path::path_to_utf8(path) + ".tmp"), ec);
    }
    dump_debug_files_locked(s);
    return had;
}

nlohmann::json scheduler_persistence_selftest() {
    nlohmann::json out;
    std::string id;
    try {
        ScheduledTask task;
        task.title = "persist-probe";
        task.prompt = "say pong later";
        task.schedule.kind = ScheduleKind::In;
        task.schedule.delay = std::chrono::seconds(3600);
        id = task_store_create(std::move(task));
        out["created_id"] = id;
        std::error_code ec;
        const fs::path file = schedule_file_for(id);
        out["file_exists"] = !file.empty() && fs::is_regular_file(file, ec);
        task_store_drop_cache();
        const auto loaded = task_store_get(id);
        const bool reloaded = loaded.has_value()
            && loaded->title == "persist-probe"
            && loaded->prompt == "say pong later"
            && loaded->enabled
            && loaded->schedule.kind == ScheduleKind::In
            && loaded->schedule.delay
            && loaded->schedule.delay->count() == 3600
            && loaded->next_run_at.has_value()
            && loaded->execution == ExecutionKind::Agent
            && loaded->precision == SchedulePrecision::BestEffort
            && loaded->generation == 0;
        out["reloaded"] = reloaded;
        const bool cancelled = task_store_cancel(id);
        out["cancelled"] = cancelled;
        task_store_drop_cache();
        const auto after = task_store_get(id);
        const bool disabled = after.has_value() && !after->enabled;
        out["disabled"] = disabled;
        task_store_erase(id);
        task_store_drop_cache();
        const bool erased = !task_store_get(id).has_value() && !fs::is_regular_file(file, ec);
        out["erased"] = erased;
        out["ok"] = out.value("file_exists", false) && reloaded && cancelled && disabled && erased;
        id.clear();
    } catch (const std::exception& ex) {
        if (!id.empty()) task_store_erase(id);
        out["ok"] = false;
        out["error"] = ex.what();
    } catch (...) {
        if (!id.empty()) task_store_erase(id);
        out["ok"] = false;
        out["error"] = "scheduler persistence selftest failed";
    }
    return out;
}

void schedule_store_invalidate() {
    auto& s = store();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.tasks.clear();
    s.live.clear();
    s.tasks_loaded = false;
}

bool schedule_write_memory(const std::string& task_id, const nlohmann::json& state) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);
    auto it = s.tasks.find(task_id);
    if (it == s.tasks.end())
        return false;
    ScheduledTask next = it->second;
    if (state.empty())
        next.memory_state = nlohmann::json::object();
    else
        merge_memory_state(next.memory_state, state);
    next.updated_at = now_iso8601();
    if (!persist_task_locked(next))
        return false;
    it->second = std::move(next);
    dump_debug_files_locked(s);
    return true;
}

void schedule_append_event(const std::string& task_id, const nlohmann::json& event) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);
    auto it = s.tasks.find(task_id);
    if (it == s.tasks.end())
        return;
    ScheduledTask next = it->second;
    next.events.push_back(event);
    if (next.events.size() > k_max_events) {
        next.events.erase(next.events.begin(),
                          next.events.begin() + static_cast<std::ptrdiff_t>(
                              next.events.size() - k_max_events));
    }
    next.updated_at = now_iso8601();
    if (!persist_task_locked(next))
        return;
    it->second = std::move(next);
    dump_debug_files_locked(s);
}

} // namespace media::llm::agent

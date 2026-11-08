#pragma once
//
// media::llm::agent — task store + scheduler memory + persisted chat sessions
//
// Scheduler tasks are saved on disk and reloaded the next time the store is
// used. The in-process scheduler thread fires them while the process is up.
// A task that comes due while the process is down runs once on the next start.
// Repeating tasks do not replay missed intervals.
//
// Chat sessions are persisted to disk so that:
//   * the chat panel's in-app session survives restarts, and
//   * separate CLI invocations sharing a --session-id can read/write the same
//     memory_state and event log (cross-process A2A scenarios).
//
// Disk layout:
//     <session_store_dir>/<sanitized-session-id>.json
//     <parent of session_store_dir>/schedules/<sanitized-task-id>.json
//
// Override the directory via:
//   * set_session_store_dir(path) — in-process override (tests).
//   * PM_IMAGE_SESSION_DIR env var — process-wide override (test harness).
//   * default: <roaming_config_dir>/sessions (next to settings.json).
//
// Thread-safe: all task_store_* functions lock a single global mutex.
// Disk writes happen while the mutex is held (atomic .tmp + rename).
// set/get/clear_current_task_id use thread_local so the scheduler thread and
// the UI thread never see each other's active task.
//
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace media::llm::agent {

enum class ScheduleKind {
    At,     // absolute one-shot   — run_at
    In,     // relative one-shot   — delay
    Every,  // repeating interval  — interval [+ start_at] [+ max_runs]
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

    // ── runtime state ─────────────────────────────────────────────────────────
    std::optional<std::chrono::system_clock::time_point> next_run_at;
    std::optional<std::chrono::system_clock::time_point> last_run_at;
    int         run_count            = 0;
    std::string last_error;
    int         consecutive_failures = 0;

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

/// Mark as disabled (does not erase). Returns false if not found.
bool task_store_cancel(const std::string& id);

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

/// Merge compact state JSON into the task's memory slot. Empty object clears all memory;
/// null values delete individual keys.
bool task_store_write_memory(const std::string& task_id, const nlohmann::json& state);

/// Merge non-memory metadata into a persisted chat/agent session JSON.
bool session_write_meta(const std::string& session_id, const nlohmann::json& patch);

/// Append an event object; trims to k_max_events.
void task_store_append_event(const std::string& task_id, const nlohmann::json& event);

// ── Per-turn task context (thread_local) ────────────────────────────────────

/// Set before run_turn for a scheduled tick so memory_write knows its target.
void        set_current_task_id(const std::string& id);
void        clear_current_task_id();
std::string get_current_task_id();

// ── Session memory (chat-panel lifetime, cross-turn) ────────────────────────

/// Create a session memory container (disabled — not fired by the scheduler).
/// Call once per chat-panel lifetime and store the returned id.
std::string session_create();

/// Return an existing session by id, or create it if missing.
/// When preferred_id is empty, creates a new generated session id.
std::string session_get_or_create(const std::string& preferred_id);

/// Populate Turn fields from the stored session: memory_state + recent events.
/// @param max_events  How many past auto-memory entries to inject into recent_events.
void session_load(const std::string& session_id,
                  nlohmann::json& out_memory_state,
                  nlohmann::json& out_recent_events,
                  size_t max_events = 20);

/// Append one auto-memory entry (called by ChatWebPanel after each successful turn).
/// Entry shape: { "ts", "user", "assistant", "tools": [...] }
/// Older entries are evicted when k_session_memory_max_turns is reached.
void session_append_turn(const std::string& session_id, const nlohmann::json& entry);

/// #9 mid-turn durability — append a single JSON line to the session's
/// `<session-id>.journal.jsonl` immediately after a tool call completes.
/// Survives a process kill: the next time the session loads we still know
/// which observations succeeded, so an end-of-turn memory_update is no
/// longer the single point of data persistence.
///
/// Entries are append-only and trimmed to k_session_journal_max_entries
/// (oldest dropped) once they roll over. Best-effort: silently no-ops on
/// I/O errors so it never blocks an agent turn.
void session_append_journal(const std::string& session_id, const nlohmann::json& entry);

/// Path to the journal file for a given session id (created on demand).
std::filesystem::path session_journal_path(const std::string& session_id);

/// Read every entry from the session journal (oldest-first). Returns an
/// empty array when the journal is absent / unreadable.
nlohmann::json session_journal_read(const std::string& session_id);

/// Read only the persisted memory_state blob for a session (no event log,
/// no journal recovery). Used internally to avoid recursive session_load.
bool session_read_memory_state(const std::string& session_id,
                               nlohmann::json& out_memory_state);

// ── Session-disk persistence ────────────────────────────────────────────────

/// Directory where session JSON files live. Created on demand.
std::filesystem::path session_store_dir();

/// Directory where scheduler task JSON files live. Sibling of the session
/// directory (`<parent>/schedules`). Not created until a task is saved.
std::filesystem::path schedule_store_dir();

/// Override the session-store directory for the rest of the process (tests).
/// Pass an empty path to clear the override and fall back to env/roaming.
void set_session_store_dir(const std::filesystem::path& dir);

/// Sanitize an external session id to a filename-safe form (a-z A-Z 0-9 _ - .).
/// Capped at 80 characters; never returns empty. Rejects "..".
std::string sanitize_session_id(const std::string& id);

/// Names of every persisted session on disk (sorted). Returns sanitized ids
/// (the filename stem) — these are what session_get_or_create / session_delete
/// expect.
std::vector<std::string> session_list_persisted();

/// Delete a persisted session file AND drop the in-memory cache entry.
/// Returns true if anything was removed.
bool session_delete(const std::string& session_id);

// ── Named session snapshots (P12) ───────────────────────────────────────────
//
// Snapshots are self-contained JSON files stored under
//   <config_dir>/snapshots/<sanitized-name>.json
// adjacent to the sessions directory. They capture memory_state + recent_events
// so a workflow can be "saved" and "restored" across independent sessions or
// invocations.
//
// Disk layout:
//   { "name": str, "saved_at": iso8601, "source_session_id": str,
//     "memory_state": {…}, "recent_events": [{…},…] }

/// Directory where named snapshot JSON files live. Created on demand.
std::filesystem::path snapshot_store_dir();

/// Save the current session memory (memory_state + recent events) to a named
/// snapshot. The file is written atomically under snapshot_store_dir().
/// Returns true on success. snapshot_name is sanitized to a safe filename.
bool session_snapshot_save(const std::string& session_id,
                           const std::string& snapshot_name);

/// Load a named snapshot and populate out_memory_state / out_recent_events.
/// Returns true when the file was found and parsed. Outputs are left
/// unchanged on failure.
bool session_snapshot_load(const std::string& snapshot_name,
                           nlohmann::json& out_memory_state,
                           nlohmann::json& out_recent_events);

// ── Serialization helpers ────────────────────────────────────────────────────

/// Full JSON (for memory_read, scheduler internal use).
nlohmann::json task_to_json(const ScheduledTask& t);

/// Compact summary JSON (for schedule_list).
nlohmann::json task_to_summary_json(const ScheduledTask& t);

/// Current UTC time as "YYYY-MM-DDTHH:MM:SSZ".
std::string now_iso8601();

/// Parse "YYYY-MM-DDTHH:MM:SS[Z]" (timezone ignored, treated as UTC).
/// Returns nullopt on failure.
std::optional<std::chrono::system_clock::time_point> parse_iso8601(const std::string& s);

} // namespace media::llm::agent

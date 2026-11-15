#pragma once
//
// Chat session memory, journals, and snapshots.
// Scheduled tasks live in agent_schedule_store.hpp. Paths and clocks live in
// agent_store_io.hpp. This header includes both so existing callers keep
// compiling.
//
#include "agent_schedule_store.hpp"
#include "agent_store_io.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace media::llm::agent {

/// Merge non-memory metadata into a persisted chat/agent session JSON.
bool session_write_meta(const std::string& session_id, const nlohmann::json& patch);

/// Merge compact state into a chat session. Creates the session record when
/// the id is not on disk yet. Does not look at the schedule store.
bool session_write_memory(const std::string& session_id, const nlohmann::json& state);
void session_append_event(const std::string& session_id, const nlohmann::json& event);
std::optional<ScheduledTask> session_memory_get(const std::string& id);
void session_store_invalidate();

void        set_current_task_id(const std::string& id);
void        clear_current_task_id();
std::string get_current_task_id();
void        set_current_task_is_schedule(bool schedule);
bool        current_task_is_schedule();

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

/// Session ids that have a persisted memory file, sorted.
std::vector<std::string> session_list_persisted();

/// Drop the in-memory session and delete its JSON and journal. Pending
/// schedule deliveries are not touched.
bool session_delete(const std::string& session_id);

/// Read only the persisted memory_state blob for a session (no event log,
/// no journal recovery). Used internally to avoid recursive session_load.
bool session_read_memory_state(const std::string& session_id,
                               nlohmann::json& out_memory_state);

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

} // namespace media::llm::agent

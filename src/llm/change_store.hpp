#pragma once

// ChangeStore — content-addressed edit history for path-mode mutations.
// History is generated from real before/after bytes, never from LLM patches.
//
// Root: <config_dir>/history\  (Roaming tanit profile, or --config-dir)
// Override: PM_CHANGE_HISTORY_DIR
//
//   history/objects/sha256/<aa>/<digest>   immutable snapshots
//   history/files/<uri-sha>.json           per-resource manifest
//   history/sessions/<session>/<id>.json   change records
//   history/sessions/<session>/changeset.json
//
// Lives in pm-llm-host (not the chat plugin) so the viewer exe can call
// vw_changes_get / vw_changes_resolve without importing tanit-chat-plugin.
// When a path has no pending records left, resolved records for that path,
// empty manifests/changesets, and unreferenced objects are deleted.
// changes_* agent tools and external-edit capture are later.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace media::llm::change {

struct RecordInput {
    std::string session_id;    // required in practice; empty falls back to agent task id
    std::string tool;          // file_str_replace | write_file | file_delete | editor | vfs | …
    std::string path;          // resolved absolute path (optional if uri is set later)
    std::string before;        // raw bytes before mutate (empty if create)
    std::string after;         // raw bytes after mutate (empty if delete)
    std::string ts;            // ISO-8601 UTC; empty = now
    std::int64_t ts_ms = 0;    // unix ms; 0 = derive from clock
    /// Caller-owned bag: reason, context, source, UI labels, …
    /// Persisted as `meta` on the change record. Not interpreted by the store.
    nlohmann::json meta = nlohmann::json::object();
};

struct RecordResult {
    bool ok = false;
    std::string error;
    std::string session_id;
    std::string change_id;
    std::string uri;
    std::string ts;
    std::int64_t ts_ms = 0;
    std::string before_version; // "sha256:<hex>"
    std::string after_version;
    int plus = 0;
    int minus = 0;
    nlohmann::json hunks = nlohmann::json::array();
    nlohmann::json meta = nlohmann::json::object();
};

std::filesystem::path default_local_history_dir();
std::filesystem::path history_root();
void set_history_dir_override(const std::filesystem::path& dir);
void clear_history_dir_override();

std::string content_sha256_hex(std::string_view bytes);
std::string version_token(std::string_view hex);
std::string file_uri_from_path(const std::filesystem::path& abs);

/// Optional expected_version on mutation tools. Accepts "sha256:<hex>" or raw hex.
/// Returns false and sets err when the current file does not match.
bool check_expected_version(std::string_view current_bytes,
                            const nlohmann::json& args,
                            std::string& err);

/// Thread-local tool-call id (set by the agent loop). Session is
/// RecordInput.session_id; empty input falls back to get_current_task_id().
void set_active_tool_call_id(const std::string& id);
void clear_active_tool_call_id();

/// Snapshot before/after, write objects + manifests. Never throws.
RecordResult record_mutation(const RecordInput& in);

/// Merge change_id / version / plus / minus onto a tool result object.
void attach_record_fields(nlohmann::json& result, const RecordResult& rec);

struct PendingChange {
    bool found = false;
    std::string error;
    nlohmann::json record = nlohmann::json::object();
    nlohmann::json changes = nlohmann::json::array();
    int pending_count = 0;
    std::string before_text;
    std::string after_text;
    bool text_skipped = false;
};

/// Latest pending record for a path (metadata only — no snapshot bytes).
nlohmann::json pending_summary_for_path(const std::filesystem::path& abs);

/// All pending summaries for a path, oldest first.
nlohmann::json list_pending_for_path(const std::filesystem::path& abs);

/// Latest pending (or a specific id) plus before/after text (capped).
PendingChange load_pending_for_path(const std::filesystem::path& abs);
PendingChange load_pending_for_path(const std::filesystem::path& abs,
                                    const std::string& change_id);

enum class ResolveAction { Accept, Reject };

struct ResolveResult {
    bool ok = false;
    std::string error;
    std::string state;
    std::string applied_text;
    int remaining_pending = 0;
};

/// Accept keeps disk as-is. Reject restores the before snapshot when the file
/// still matches the chain tip. Rejecting a non-latest record also rejects
/// every newer pending on the same path. When nothing pending remains for the
/// path, resolved history for that file is garbage-collected.
ResolveResult resolve_change(const std::string& session_id,
                             const std::string& change_id,
                             ResolveAction action);

/// Accept or reject every pending record for this file (clears its ChangeStore stack).
/// When nothing pending remains, resolved history for the path is garbage-collected.
ResolveResult resolve_all_for_path(const std::filesystem::path& abs,
                                   ResolveAction action);

} // namespace media::llm::change

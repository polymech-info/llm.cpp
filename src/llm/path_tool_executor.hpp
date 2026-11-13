#pragma once
//
// media::llm::path::execute — invoke a path-mode tool by name.
//
// Walks `arguments.paths[]` (or `arguments.inputs[]` for image_find), calls
// the appropriate `*_file` / `*_image` worker per entry, writes results to
// disk, and returns one PerFileResult per input. The standard envelope is
// the same shape across all path-mode tools (see docs/chat.md §4).
//
// Error semantics: per-file errors do NOT short-circuit the batch — the
// envelope reports `ok:true` with `summary.failed > 0` so the chat agent can
// surface partial successes ("✓ a.jpg ✓ b.jpg ✗ c.jpg"). A catastrophic
// failure (unknown tool name, malformed arguments) returns `ok:false` and
// an empty `results[]`.
//
#include "polymech_export.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace pm::audio { class AudioOutput; }

namespace media::llm::path {

struct PerFileResult {
    std::string    path;          // input path the model passed in
    bool           ok = false;
    std::string    error;         // populated on failure
    std::string    output_path;   // written file (resize / compress / transform)
    nlohmann::json result;        // populated for meta (parsed description JSON)
};

struct ExecuteResult {
    bool                       ok = false;
    std::string                error;        // catastrophic failure (e.g. unknown tool)
    std::vector<PerFileResult> per_file;
    nlohmann::json             envelope;     // {ok, results, summary} — what to send back to the LLM
};

/// Run a tool by catalog name. `arguments` follows the path-mode envelope
/// (see docs/chat.md §4). `arguments.paths` (or `arguments.inputs` for
/// image_find) is the list of host paths to operate on.
PM_CHAT_API ExecuteResult execute(const std::string& name,
                                   const nlohmann::json& arguments);

/// While the LLM agent turn is running, `execute` refuses tool names in this
/// set (case-insensitive). Call `clear` when the turn ends. Used with
/// `tool_catalog_openai_excluding` so the model should not see disabled tools.
PM_CHAT_API void set_agent_tool_blocklist(const std::vector<std::string>& disabled_names);
PM_CHAT_API void clear_agent_tool_blocklist();

/// When non-empty, `resolve_tool_path_string` (all path tools) resolves **relative** paths
/// against this directory first instead of `std::filesystem::current_path()`.
/// Used by the in-app / `llm agent` turn so the effective “cwd” is the Explorer
/// folder or CLI `--folder` when set. Call `clear_agent_path_base()` at end of the turn.
/// Empty = process CWD (CLI, tests, or agent with no `--folder`).
PM_CHAT_API void set_agent_path_base(const std::string& absolute_dir_or_empty);
PM_CHAT_API void clear_agent_path_base();

/// Files the user selected for this turn. A tool path that does not exist is
/// replaced by the one selected file with the same filename. Clear with the path base.
PM_CHAT_API void set_agent_selection(const std::vector<std::string>& paths);
PM_CHAT_API void clear_agent_selection();

/// When enabled, `llm_path_fs_deny` rejects any path outside `agent_path_base`
/// (hard jail for FS / search / pictures / service path tools). Used by
/// `llm agent --serve --fs-jail`. When path base is unset, jail is deny-all.
/// `run` / MCP are not covered. Clear via `clear_agent_fs_jail()`.
PM_CHAT_API void set_agent_fs_jail(bool enabled);
PM_CHAT_API void clear_agent_fs_jail();
PM_CHAT_API bool agent_fs_jail_enabled();

/// Provenance for ChangeStore (thread-local). Set around path::execute.
PM_CHAT_API void set_change_tool_call_id(const std::string& id);
PM_CHAT_API void clear_change_tool_call_id();

/// Convenience: serialise an ExecuteResult to the JSON string the agent
/// loop should send back to the LLM as the `tool` message content.
PM_CHAT_API std::string envelope_text(const ExecuteResult& r);

/// If a `speak` tool call is currently synthesising or playing audio, signal it
/// to stop immediately. Safe to call from any thread; no-op when idle.
/// Called automatically by AgentScheduler::stop() so the scheduler thread can
/// be joined cleanly when the app exits.
PM_CHAT_API void abort_active_speak();
PM_CHAT_API void pause_active_speak();
PM_CHAT_API void resume_active_speak();

/// Register the AudioOutput currently playing TTS so Ctrl+C / abort_active_speak()
/// can call stop() immediately.
PM_CHAT_API void register_active_speak_output(pm::audio::AudioOutput* out) noexcept;
/// Clear only when @p expected is still the active output (safe from destructors).
PM_CHAT_API void unregister_active_speak_output(pm::audio::AudioOutput* expected) noexcept;

/// If a `run` tool call has a child process running, terminate it immediately.
/// Safe to call from any thread; no-op when idle.
PM_CHAT_API void abort_active_run();

/// While set, `image_transform` calls @p f after **each** input file finishes
/// (the argument is a full tool `envelope` for that file only). The UI can
/// show queue rows immediately; the final `execute` result still includes all
/// files for the LLM. Clear when the turn ends; never invoke after `execute` returns.
PM_CHAT_API void set_image_transform_file_progress_hook(
    std::function<void(const nlohmann::json& one_file_envelope)> f);
PM_CHAT_API void clear_image_transform_file_progress_hook();

/// While set, `download_url` reports received/total bytes during the transfer.
/// `total` is 0 when the server sent no length. Clear when the call returns.
PM_CHAT_API void set_download_url_progress_hook(
    std::function<void(std::uint64_t received, std::uint64_t total)> f);
PM_CHAT_API void clear_download_url_progress_hook();

} // namespace media::llm::path

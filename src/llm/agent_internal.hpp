#pragma once
//
// Internal (detail) API shared across the agent translation units.
//
// agent_commons.cpp  — time helpers, emit, RAII scopes, load, env ctx, usage parsing
// agent_context.cpp  — system-prompt building
// agent.cpp          — default run_turn implementation
//
// Nothing here is part of the public media::llm::agent:: surface — keep it in detail::.
//
#include "agent.hpp"

#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace media::llm::agent::detail {

// ── String utilities ─────────────────────────────────────────────────────────


/// True only when the user asked to add a ribbon / context-menu / custom command.
/// Weak models otherwise treat create_command as a stand-in for calling a tool.
bool user_asked_for_ribbon_command(std::string_view prompt);

// ── Event emission ────────────────────────────────────────────────────────────

/// Returns false when the callback signals cancellation.
bool emit(const EventCallback& cb, const Event& ev);

// ── System-prompt file loading ────────────────────────────────────────────────

std::string trim_system_prompt_utf8(std::string s);

/// Config directory that hosts `prompts/` subdirectory (platform-specific).
std::filesystem::path system_prompt_config_parent();

struct PromptFileLoad {
    std::string           content;
    std::filesystem::path path;
};

/// Loads a prompt Markdown file and preserves the resolved path for logging.
/// `content` is empty when the file is absent or unreadable.
PromptFileLoad load_system_prompt_md();
PromptFileLoad load_planner_prompt_md();
PromptFileLoad load_realtime_prompt_md();
/// `user-preferences.md`, or `user-preferences.android.md` on the phone.
/// Empty when the file is absent. Appended at the end of the system prompt.
PromptFileLoad load_user_preferences_md();
PromptFileLoad load_security_prompt_md();
PromptFileLoad load_mode_overlay_md(TurnMode mode);

/// Loads `system-prompt.md` from beside the exe or from config_dir/prompts/.
/// Returns an empty string when the file is absent.
std::string load_system_prompt_md_if_present();

/// Loads `planner-prompt.md` from the same search locations as system-prompt.md.
/// Returns an empty string when the file is absent.
std::string load_planner_prompt_md_if_present();

/// Loads `realtime-prompt.md` (the live voice-session persona) from the same
/// search locations as system-prompt.md. Returns an empty string when absent
/// (callers fall back to a built-in default).
std::string load_realtime_prompt_md_if_present();

/// Loads `security-prompt.md` (the AI consent delegate persona) from secure
/// prompt storage. Returns an empty string when absent.
std::string load_security_prompt_md_if_present();

/// Returns the lowercase canonical name for a TurnMode (e.g. "followup", "execute").
/// Returns "auto" for TurnMode::Auto.
std::string_view turn_mode_name(TurnMode mode) noexcept;

/// Loads `mode-<name>.md` from the prompts directory for the given mode.
/// Returns an empty string when mode is Auto or the file is absent.
std::string load_mode_overlay_md_if_present(TurnMode mode);

/// Render a raw prompt file body through ${VAR} substitution.
/// Variables include standard command_variables (CWD, YYYY, …) plus
/// agent-specific ones: APP_NAME, APP_ID, PLATFORM, DATETIME, FOLDER,
/// SELECTION_COUNT, CURRENT_SELECTION, OPEN_COUNT, CURRENT_OPEN_FILES,
/// CURRENT_CURSOR, CURRENT_INNER_SELECTION, INNER_SELECTION_CHARS,
/// TASK_ID, and optionally `builtin` for planner wrapping.
/// Unknown placeholders are left intact.
///
/// CURRENT_SELECTION / CURRENT_OPEN_FILES are annotated with size / line
/// count for the agent (not the CLI path-list form).

/// One path plus optional `(N lines, size[, large])` for prompt injection.
std::string format_path_with_stats(std::string_view path);

/// Newline-joined `format_path_with_stats` for a path list. Empty if none.
std::string format_paths_with_stats(const std::vector<std::string>& paths);

/// True when a regular text file exceeds the unwindowed file_read threshold
/// (more than k_file_read_window_lines or k_file_read_large_bytes).
bool path_is_large_text_file(std::string_view path);

/// True when the path is an existing regular file that file_read would accept.
bool path_is_existing_text_file(std::string_view path);

std::string render_prompt_file(const std::string& raw,
                               const Turn& turn,
                               const std::string& builtin_text = {});

// ── Date / time ───────────────────────────────────────────────────────────────

/// "YYYY-MM-DDTHH:MM:SS±HH:MM (Timezone Name)" for the host's local clock.
std::string local_datetime_with_offset();

// ── RAII scopes ───────────────────────────────────────────────────────────────

struct PathToolBlocklistScope {
    bool active = false;
    explicit PathToolBlocklistScope(const std::vector<std::string>& disabled, int max_iter);
    ~PathToolBlocklistScope();
    PathToolBlocklistScope(const PathToolBlocklistScope&)            = delete;
    PathToolBlocklistScope& operator=(const PathToolBlocklistScope&) = delete;
};

struct AgentPathBaseScope {
    bool active = false;
    bool jail_active = false;
    explicit AgentPathBaseScope(const Turn& t);
    ~AgentPathBaseScope();
    AgentPathBaseScope(const AgentPathBaseScope&)            = delete;
    AgentPathBaseScope& operator=(const AgentPathBaseScope&) = delete;
};

// ── System-prompt assembly ────────────────────────────────────────────────────

/// Appends "Runtime context" block (OS, app, date/time, MCP note).
void append_runtime_environment_context(std::ostringstream& s);

/// Appends tool list, selection / folder block, and routing guidelines.
/// `tools` is the fully-assembled OpenAI tools JSON array (path + MCP, already filtered).
/// Only the names present in `tools` are ever mentioned in the generated text.
/// Set `emit_identity` to false when an external system-prompt.md already provides
/// the "You are …" intro so the dynamic section skips the duplicate sentence.
/// `emit_selection` / `emit_open_files` / `emit_folder` stay on for any of those
/// blocks the external file did not already substitute. A file that names only
/// one placeholder must not suppress the others.
void append_agent_tooling_and_context(std::ostringstream& s,
                                      const Turn& turn,
                                      const nlohmann::json& tools,
                                      bool emit_identity = true,
                                      bool emit_selection = true,
                                      bool emit_open_files = true,
                                      bool emit_folder = true);

// ── Shared session-memory rendering ───────────────────────────────────────────
//
// Used by both the main-agent system prompt builder (`agent_context.cpp`)
// and the planner pass (`agent_feature.cpp`) so we ship the same compact
// view to both LLM calls. Sort is LRU-aware: items with `last_used_at` are
// sorted by that timestamp descending, falling back to `created_at`.
//
// `chars_budget` caps the total injected block size; `max_items` caps the
// item count regardless of budget. Pass 0 to either to use defaults.

void append_session_items(std::ostringstream& s,
                          const nlohmann::json& memory_state,
                          std::size_t chars_budget = 0,
                          std::size_t max_items = 0);

/// Last N turn_pair items as a compact User/Assistant tail. Pass 0 for
/// k_recent_turns_inject_max. Empty when memory has no conversation pairs.
void append_recent_turns(std::ostringstream& s,
                         const nlohmann::json& memory_state,
                         std::size_t max_turns = 0,
                         bool advertise_body_recall = true);

/// Same-directory replace. The destination is left intact when the replace
/// fails. `from` is removed only after a successful replace, or by the caller
/// when the replace fails.
bool replace_file_preserving_destination(const std::filesystem::path& from,
                                         const std::filesystem::path& to);

/// Unique sibling of `dest` (`dest.<pid>.<n>.tmp`) so two writers do not share
/// one temp name.
std::filesystem::path unique_sibling_temp(const std::filesystem::path& dest);

void append_relevant_session_rules(std::ostringstream& s,
                                   const nlohmann::json& memory_state,
                                   std::size_t chars_budget = 0);

void append_pending_recall(std::ostringstream& s, std::string_view block);

// ── LLM usage parsing ─────────────────────────────────────────────────────────

std::int64_t json_usage_int64(const nlohmann::json& usage, const char* key);

/// Locate token/cost telemetry across provider-specific metadata shapes.
/// OpenRouter usually returns `usage`; LiteLLM/Pixlwiz may return `usage_object`
/// and/or `additional_usage_values`. Local llama.cpp may only have top-level
/// `prompt_tokens` / `completion_tokens` — `merge_llm_usage_from_provider_meta`
/// synthesizes a usage object from those.
const nlohmann::json* provider_meta_usage_object(const nlohmann::json& meta);

/// Parse provider_meta_json and accumulate token counts + round entry into agg.
void merge_llm_usage_from_provider_meta(nlohmann::json& agg,
                                        const std::string& provider_meta_json,
                                        const std::string& router);

// ── JSON UTF-8 safety ─────────────────────────────────────────────────────────
//
// nlohmann::json::dump() throws type_error.316 on invalid UTF-8 in string values.
// Provider metadata, tool envelopes, and run output can carry binary-ish bytes;
// repair in place before persistence or WebView bridge posts.

void sanitize_json_utf8_strings(nlohmann::json& j);

/// Sanitize then dump; returns "{}" (or "[]" for array input) on failure.
std::string safe_json_dump(const nlohmann::json& j, int indent = -1);

// Safe field readers for agent Event payloads. Unlike json::value(), these never
// throw when the key exists with the wrong JSON type (e.g. null envelope/results).
inline nlohmann::json json_child_object(const nlohmann::json& o, const char* key)
{
    if (!o.is_object()) return nlohmann::json::object();
    const auto it = o.find(key);
    if (it == o.end() || !it->is_object()) return nlohmann::json::object();
    return *it;
}

inline nlohmann::json json_child_array(const nlohmann::json& o, const char* key)
{
    if (!o.is_object()) return nlohmann::json::array();
    const auto it = o.find(key);
    if (it == o.end() || !it->is_array()) return nlohmann::json::array();
    return *it;
}

inline std::int64_t json_child_int64(const nlohmann::json& o, const char* key, std::int64_t def = -1)
{
    if (!o.is_object()) return def;
    const auto it = o.find(key);
    if (it == o.end()) return def;
    if (it->is_number_integer()) return it->get<std::int64_t>();
    if (it->is_number_unsigned()) return static_cast<std::int64_t>(it->get<std::uint64_t>());
    if (it->is_number_float()) return static_cast<std::int64_t>(it->get<double>());
    return def;
}

inline bool json_child_bool(const nlohmann::json& o, const char* key, bool def = false)
{
    if (!o.is_object()) return def;
    const auto it = o.find(key);
    if (it == o.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}

inline std::string json_child_string(const nlohmann::json& o, const char* key, std::string def = {})
{
    if (!o.is_object()) return def;
    const auto it = o.find(key);
    if (it == o.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

} // namespace media::llm::agent::detail

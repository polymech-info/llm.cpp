// Copyright PolyMech / pm-image. Internal use only.
//
// Shared internals for the path-mode agent tools (`media::llm::path::detail`).
//
// Each category lives in `src/llm/tools/<category>/<Category>Tools.{hpp,cpp}`
// and reaches back here for:
//
//   - the ExecuteResult / PerFileResult helpers (build_envelope, fatal,
//     simple_error, simple_ok)
//   - JSON-arg accessors (options_or_empty, get_nonempty_string,
//     get_string_array, get_output_path_argument)
//   - path resolution (resolve_tool_path_string, llm_path_fs_deny,
//     llm_path_write_deny, make_sibling_path, resolve_tool_paths_strings_in_json)
//   - per-thread agent state (path base, blocklist)
//
// Public symbols (execute, set_agent_*, abort_active_*, tool_catalog_*) stay
// in path_tool_executor.hpp / path_tool_catalog.hpp. This header is for
// pm-media TUs only; it never leaks into consumers of pm-media.
#pragma once

#include "llm/path_tool_catalog.hpp"
#include "llm/path_tool_executor.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace media { struct MetaOptions; }

namespace media::llm::path::detail {

namespace fs = std::filesystem;

// ── Thread-local agent state (one set per agent worker thread) ────────────
//
// `g_agent_path_base`  — pinned Explorer / `--folder` directory; relative
//                        paths in tool args resolve against this when set.
// `g_agent_tool_blocklist` — tool names refused by the dispatcher for the
//                            current turn (lowercased, trimmed).
//
// Helpers below read these implicitly; the path_tool_executor.cpp public
// setters (set_agent_path_base / set_agent_tool_blocklist) mutate them.
const std::string&                          agent_path_base() noexcept;
bool                                        agent_path_base_empty() noexcept;
const std::unordered_set<std::string>&      agent_tool_blocklist() noexcept;

// Mutation hooks called from the public surface in path_tool_executor.cpp.
void set_agent_path_base_impl(const std::string& raw);
void clear_agent_path_base_impl();
void set_agent_tool_blocklist_impl(const std::vector<std::string>& disabled);
void clear_agent_tool_blocklist_impl();
void set_agent_fs_jail_impl(bool enabled);
void clear_agent_fs_jail_impl();
bool agent_fs_jail_enabled() noexcept;

// ── Small string helpers ──────────────────────────────────────────────────
void                       trim_in_place(std::string& s);

// ── FS guard wrappers ─────────────────────────────────────────────────────
std::string llm_path_fs_deny(const std::string& abs_str);
std::string llm_path_write_deny(const std::string& abs_str);

// ── JSON-arg accessors ────────────────────────────────────────────────────
const nlohmann::json&      options_or_empty(const nlohmann::json& args);
std::vector<std::string>   get_string_array(const nlohmann::json& args, const char* key);
std::optional<std::string> get_nonempty_string(const nlohmann::json& obj, const char* key);
std::optional<std::string> get_output_path_argument(const nlohmann::json& args);

// ── Path resolution ───────────────────────────────────────────────────────
std::string                resolve_tool_path_string(const std::string& raw);
void                       resolve_tool_paths_strings_in_json(nlohmann::json& j,
                                                              const std::string* parent_obj_key = nullptr);
std::string                make_sibling_path(const std::string& input,
                                             const std::string& out_dir,
                                             const std::string& suffix,
                                             const std::string& new_ext_with_dot);

// ── Result envelope helpers ───────────────────────────────────────────────
ExecuteResult              build_envelope(const std::string& tool,
                                          std::vector<PerFileResult> per_file);
ExecuteResult              fatal(const std::string& msg);
ExecuteResult              simple_error(const std::string& msg);
ExecuteResult              simple_ok(nlohmann::json payload);

// Sidecar cache lookup used by image_meta + image_find (internal-only callers).
bool                       try_image_meta_from_sidecar(const std::string&    in_abs,
                                                       const media::MetaOptions& opt,
                                                       const nlohmann::json&  tool_options,
                                                       PerFileResult&         r);

// ── Dispatcher-side helpers ───────────────────────────────────────────────
//
// `log_path_tool_invoke_context` writes a single [info] line summarising the
// (redacted) tool args + cwd + path base. Called once per dispatched tool
// from execute_impl().
void log_path_tool_invoke_context(const std::string& tool, const nlohmann::json& args);

// Strip `$AGENT_TOOL_run` / `$TOOL_write_file` style template noise some
// models occasionally leak into the function-call `name` slot.
std::string strip_template_prefix(const std::string& name);

} // namespace media::llm::path::detail

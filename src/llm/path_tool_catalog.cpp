// Copyright PolyMech / pm-image. Internal use only.
//
// Path-mode tool catalog assembler. After the May 2026 refactor each tool's
// `make_<name>()` schema-factory lives in its own category file under
// `src/llm/tools/<category>/`. This file just composes them into the LLM-
// facing arrays and applies the standard filter (excluding / allowing / flag-
// based) helpers.
//
// See `src/llm/path_tool_executor.cpp` for the matching dispatcher comment.
#include "llm/path_tool_catalog.hpp"
#include "llm/agent_child_launch.hpp"

#include "llm/agent_tools.hpp"

#include "llm/tools/commons/PathToolCommons.hpp"
#include "llm/tools/audio/AudioTools.hpp"
#include "llm/tools/fs/FsTools.hpp"
#include "llm/tools/inquire/InquireTools.hpp"
#include "llm/tools/memory/MemoryTools.hpp"
#include "llm/tools/pictures/PictureTools.hpp"
#if defined(FEATURE_SCHEDULER_TOOL) && FEATURE_SCHEDULER_TOOL
#include "llm/tools/scheduler/SchedulerTools.hpp"
#endif
#include "llm/tools/video/VideoTools.hpp"
#if defined(FEATURE_INSTALLER_VFS) && FEATURE_INSTALLER_VFS
#include "llm/tools/service/ServiceTools.hpp"
#endif

#include "llm/tools/computer_use/Tool_ComputerUse.hpp"
#include "llm/tools/browser_use/Tool_BrowserUse.hpp"
#include "llm/tools/app_command/Tool_AppCommand.hpp"
#include "llm/tools/info/InfoTool.hpp"
#include "llm/tools/network/NetworkTools.hpp"
#if defined(FEATURE_XBLOX) && FEATURE_XBLOX
#include "llm/tools/sequencer/SequencerTool.hpp"
#endif
#if defined(FEATURE_TOOLS_COMMAND_CREATION) && FEATURE_TOOLS_COMMAND_CREATION
#include "llm/tools/command_creation/CreateCommandTool.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <vector>
#include "string/string_utils.hpp"

namespace media::llm::path {

namespace detail {

// The `run` and `info_lookup` schemas have always lived here; they don't fit
// any other category (run is its own subsystem; info_lookup is the agent
// introspection door). Keep them inline so the tools/ subtree stays focused
// on the per-category groups.

ToolDef make_run() {
    return ToolDef{
        "run",
        std::string(
            "Execute a shell command and return stdout/stderr. "
            "Each stream is capped in the result; when stdout_clipped or stderr_clipped is true, "
            "stdout_file or stderr_file is the full captured text. Read it with file_read offset/limit. "
            "Use for build commands, git, file ops, and tasks without a dedicated tool. "
            "cwd inherits the current Explorer folder. "
            "Nested agent: use ")
        + media::llm::agent::child_launch_canonical_example()
        + std::string(
            " — keep it quiet. "
            "Security policy blocks privilege escalation and download cradles; max 120 s. "
            "GUI launches (tanit --ui-preset viewer|main|chat --src <abs-path>, Start-Process) "
            "return immediately with no stdout — empty result is SUCCESS, do not retry."),
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"command", {
                    {"type", "string"},
                    {"description",
                     "Shell command (PowerShell on Windows, bash/sh on Unix). Avoid interactive commands. "
                     "Nested agent: put `--log-level error` before `llm agent`; use `--single-turn` for one-shot work."},
                }},
                {"shell", {
                    {"type", "string"},
                    {"enum", {"auto", "bash", "sh", "pwsh", "cmd"}},
                    {"description",
                     "Shell to use. 'auto' (default) picks pwsh on Windows, bash on Unix. "
                     "'cmd' is Windows cmd.exe. Rarely needed."},
                }},
                {"timeout_ms", {
                    {"type", "integer"},
                    {"minimum", 1},
                    {"maximum", 300000},
                    {"description",
                     "Wall-clock ms before the process is killed. Default 60000. "
                     "Use 120000–180000 for external-API commands; 300000 for video generation. Cap 300000."},
                }},
            }},
            {"required", nlohmann::json::array({"command"})},
        }};
}

ToolDef make_suspend_voice() {
    return ToolDef{
        "suspend_voice",
        "End the live voice session and return to standby. "
        "Call this when the user is done with voice, says stop listening, suspend, hang up, "
        "or the goal is suspend_voice. "
        "This does not cancel a running task. "
        "The session closes after this returns; do not start another voice session in the same turn.",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json::object()},
            {"required", nlohmann::json::array()},
        }};
}

ToolDef make_info_lookup() {
    return ToolDef{
        "info_lookup",
        "Look up Tanit's local catalog: CLI commands, in-app UI verbs (app_command), "
        "path tools, MCP tool names, apps, skills. "
        "Not a web search. A miss means the name is not in Tanit. "
        "For MCP: kind=mcp, query=mcp lists servers; then request_tools the mcp_* name and CALL it. "
        "Do not use this to invent ribbon buttons.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"query", {
                    {"type", "string"},
                    {"description",
                     "Name or keyword, e.g. tavily_search, file_read, image_transform, chrome. "
                     "For a list of MCP servers use kind=mcp and query=mcp."},
                }},
                {"kind", {
                    {"type", "string"},
                    {"enum", {"auto", "command", "app_command", "tool", "mcp", "app", "app_state", "skill", "resource", "all"}},
                    {"description",
                     "Surface to search. "
                     "auto: CLI and app commands first, then tools. "
                     "command: CLI slugs and in-app UI verbs. "
                     "app_command: in-app UI verbs only (run with the app_command tool). "
                     "mcp: MCP tools only (query=mcp lists them). "
                     "app: installed Windows applications. "
                     "app_state: live Tanit UI snapshot. "
                     "skill/resource: compact metadata; exact URI loads a body."},
                    {"default", "auto"},
                }},
                {"detail", {
                    {"type", "boolean"},
                    {"description", "When true, include generated help / schemas for exact syntax."},
                    {"default", true},
                }},
                {"max_results", {
                    {"type", "integer"},
                    {"minimum", 1},
                    {"maximum", 50},
                    {"description", "Maximum matches to return. Default 8."},
                }},
                {"max_bytes", {
                    {"type", "integer"},
                    {"minimum", 1024},
                    {"maximum", 98304},
                    {"description", "Maximum body bytes when materializing a resource. Default 24576."},
                }},
                {"depth", {
                    {"type", "integer"},
                    {"minimum", 0},
                    {"maximum", 4},
                    {"description", "Explicit cross-resource traversal depth for cycle/depth guards. Default 0."},
                }},
            }},
            {"required", nlohmann::json::array({"query"})},
        }};
}

#if defined(FEATURE_TOOLS_COMMAND_CREATION) && FEATURE_TOOLS_COMMAND_CREATION
ToolDef make_create_command() {
    return ToolDef{
        "create_command",
        "Add a ribbon or context-menu button. Only when the user asked to create a command/button. "
        "Not a substitute for calling an MCP or path tool. "
        "Prefer command + args. shell + shell_line is only for a pipeline, redirection, or a loop. "
        "Agent/MCP tool names are invalid here. Every field is validated strictly; "
        "on {needs_resolution:true} pick one of the returned candidates and retry with its exe path.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"label", {
                    {"type", "string"},
                    {"description", "Display name shown in the ribbon button or context-menu entry. Required, unique. Max 80 chars."},
                }},
                {"command", {
                    {"type", "string"},
                    {"description",
                     "tanit-cli path (\"audio record\", \"llm agent\", \"video record\", …) validated against the live CLI grammar, "
                     "or ONE program by name (\"vlc\", \"ffmpeg\"). A PATH hit is the bare name, never its install path. "
                     "Never a shell and never a command line — flags go in args. Call info_lookup first for the live option list."},
                }},
                {"args", {
                    {"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"description",
                     "JSON array of strings (never a single string). For tanit-cli: subcommand tokens and flags from the live schema; "
                     "positionals by position, not as --flags. Variables must be palette names: ${CURRENT_FILE}, ${CURRENT_PATH}, "
                     "${CURRENT_SELECTION}, ${SRC_NAME}, ${SRC_DIR}, ${PATH_SEP}, ${KNOWNFOLDER:Desktop}, "
                     "${PATH_DIR:CURRENT_FILE}, ${PATH_NAME:CURRENT_FILE}. A sibling file is "
                     "${PATH_DIR:CURRENT_FILE}${PATH_SEP}${PATH_NAME:CURRENT_FILE}.ext. ${ENV:…} is not allowed. "
                     "Max 40 items × 512 chars."},
                }},
                {"shell", {
                    {"type", "string"},
                    {"enum", {"cmd", "powershell", "pwsh", "bash", "sh", "zsh", "fish"}},
                    {"description", "Last resort. Only when the line needs a pipe, redirection, or a loop. "
                                    "A single program, including one that derives an output path, is command + args. "
                                    "Requires shell_line; mutually exclusive with command/args."},
                }},
                {"shell_line", {
                    {"type", "string"},
                    {"description", "The exact one-liner the shell runs, stored verbatim (max 2048 chars). Requires shell."},
                }},
                {"description", {
                    {"type", "string"},
                    {"description", "Optional tooltip / description text for the command."},
                }},
                {"surface", {
                    {"type", "string"},
                    {"enum", {"ribbon", "context_menu", "both"}},
                    {"description",
                     "Where to surface the command. "
                     "\"ribbon\": ribbon bar only (default). "
                     "\"context_menu\": sets registerInExplorer=true for Windows Explorer. "
                     "\"both\": ribbon + Explorer context menu."},
                    {"default", "ribbon"},
                }},
                {"group", {
                    {"type", "string"},
                    {"description",
                     "Ribbon group label to place the button in (matched case-insensitively). "
                     "Omitted: the \"AI Commands\" group (created if missing). "
                     "Given but missing: error listing existing groups unless create_group=true."},
                }},
                {"create_group", {
                    {"type", "boolean"},
                    {"description", "Create `group` when it does not exist. Default false."},
                }},
                {"cwd", {
                    {"type", "string"},
                    {"description", "Working directory: an existing absolute folder or a palette variable such as ${CURRENT_PATH}. "
                                    "Mutually exclusive with cwd_from_selection."},
                }},
                {"icon", {
                    {"type", "string"},
                    {"description", "Lucide icon name for the button (e.g. \"photo\", \"scissors\", \"play\", \"run\", \"microphone\"). Default: \"run\"."},
                }},
                {"tint", {
                    {"type", "string"},
                    {"description", "Accent / tint color in #RRGGBB hex format (e.g. \"#A855F7\")."},
                }},
                {"cwd_from_selection", {
                    {"type", "boolean"},
                    {"description",
                     "When true, sets cwd to ${CURRENT_PATH} so the command runs in the selected folder. Default: false."},
                    {"default", false},
                }},
                {"selection_mode", {
                    {"type", "string"},
                    {"enum", {"aggregate", "perItem"}},
                    {"description",
                     "How a multi-file selection is applied. aggregate: one run, ${CURRENT_SELECTION} holds the list. "
                     "perItem: one run per file, ${CURRENT_FILE} bound to that file."},
                }},
                {"explorer_file_types", {
                    {"type", "array"},
                    {"items", {{"type", "string"}, {"enum", {"image", "video", "audio", "text", "folder"}}}},
                    {"description", "When surface is context_menu or both, limit Explorer targets. Omit for all types."},
                }},
                {"as_llm_tool", {
                    {"type", "boolean"},
                    {"description", "Expose this command to chat as an LLM tool. Requires description; not allowed for shell one-liners. Default false."},
                }},
                {"as_voice_command", {
                    {"type", "boolean"},
                    {"description", "Allow this command after the wake phrase. Requires voice_command; not allowed for shell one-liners. Default false."},
                }},
                {"voice_command", {
                    {"type", "string"},
                    {"description", "Spoken phrase (≥3 letters/digits). Must not collide with an existing voice command. Only with as_voice_command=true."},
                }},
                {"show_in_launcher", {
                    {"type", "boolean"},
                    {"description", "Show in the command launcher. Default false."},
                }},
                {"show_in_assistant_bar", {
                    {"type", "boolean"},
                    {"description", "Show as a button on the assistant toolbar. Default false."},
                }},
                {"show_parameter_dialog", {
                    {"type", "boolean"},
                    {"description", "Prompt for CLI parameters before the command runs. Default false."},
                }},
            }},
            {"required", nlohmann::json::array({"label"})},
        }};
}
#endif

} // namespace detail

// ── Catalog assembly ──────────────────────────────────────────────────────

std::vector<ToolDef> tool_catalog() {
    using namespace detail;
    std::vector<ToolDef> out = {
        make_file_glob(),
        make_file_read(),
        make_file_search(),
        make_search_index(),
        make_search_query(),
        make_search_detail(),
        make_resize(),
        make_crop(),
        make_transform(),
        make_image_create(),
        make_create_video(),
        make_understand(),
//        make_ocr_text(),
        make_camera(),
        make_write_file(),
        make_file_compose(),
        make_download_url(),
        make_file_str_replace(),
        make_file_delete(),
        make_speak(),
        make_play_file(),
        make_suspend_voice(),
#if defined(FEATURE_STT) && FEATURE_STT
        make_audio_transcribe(),
#endif
        make_ask_user(),
#if defined(FEATURE_SCHEDULER_TOOL) && FEATURE_SCHEDULER_TOOL
        make_schedule_at(),
        make_schedule_in(),
        make_schedule_every(),
        make_schedule_cancel(),
        make_schedule_list(),
#endif
        make_memory_read(),
        make_memory_write(),
        make_memory_append_event(),
        make_memory_find(),
        make_run(),
#if defined(FEATURE_XBLOX) && FEATURE_XBLOX
        make_run_sequence(),
#endif
        make_info_lookup(),
    };
    {
        const auto network_tools = detail::network_tool_catalog();
        out.insert(out.end(), network_tools.begin(), network_tools.end());
    }
#if defined(FEATURE_INSTALLER_VFS) && FEATURE_INSTALLER_VFS
    {
        const auto service_tools = detail::service_tool_catalog();
        out.insert(out.end(), service_tools.begin(), service_tools.end());
    }
#endif
#if defined(FEATURE_TOOLS_COMMAND_CREATION) && FEATURE_TOOLS_COMMAND_CREATION
    out.push_back(detail::make_create_command());
#endif
#if defined(FEATURE_TOOLS_APP_USE) && FEATURE_TOOLS_APP_USE && defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT
    const auto computer_tools = media::llm::computer_use::tool_defs();
    out.insert(out.end(), computer_tools.begin(), computer_tools.end());
#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
    const auto browser_tools = media::llm::browser_use::tool_defs();
    out.insert(out.end(), browser_tools.begin(), browser_tools.end());
#endif
    const auto app_command_tools = media::llm::app_command::tool_defs();
    out.insert(out.end(), app_command_tools.begin(), app_command_tools.end());
#endif
    enrich_create_video_from_replicate_cache(out);
    return out;
}

std::vector<ToolDef> tool_catalog_excluding(const std::vector<std::string>& disabled) {
    if (disabled.empty()) return tool_catalog();
    std::unordered_set<std::string> dset;
    dset.reserve(disabled.size() * 2);
    for (const auto& d : disabled) {
        if (d.empty()) continue;
        std::string k = d;
        detail::trim_in_place(k);
        k = media::strutil::lower_ascii(k);
        if (!k.empty()) dset.insert(std::move(k));
    }
    if (dset.empty()) return tool_catalog();
    std::vector<ToolDef> out;
    for (const auto& t : tool_catalog()) {
        std::string nm = t.name;
        nm = media::strutil::lower_ascii(nm);
        if (dset.count(nm)) continue;
        out.push_back(t);
    }
    return out;
}

std::vector<ToolDef> tool_catalog_allowing(const std::vector<std::string>& allowed,
                                           const std::vector<std::string>& disabled) {
    if (allowed.empty())
        return tool_catalog_excluding(disabled);

    std::unordered_set<std::string> aset;
    aset.reserve(allowed.size() * 2);
    for (const auto& a : allowed) {
        if (a.empty()) continue;
        std::string k = a;
        detail::trim_in_place(k);
        k = media::strutil::lower_ascii(k);
        if (!k.empty()) aset.insert(std::move(k));
    }
    if (aset.empty())
        return tool_catalog_excluding(disabled);

    std::unordered_set<std::string> dset;
    dset.reserve(disabled.size() * 2);
    for (const auto& d : disabled) {
        if (d.empty()) continue;
        std::string k = d;
        detail::trim_in_place(k);
        k = media::strutil::lower_ascii(k);
        if (!k.empty()) dset.insert(std::move(k));
    }

    std::vector<ToolDef> out;
    for (const auto& t : tool_catalog()) {
        std::string nm = t.name;
        nm = media::strutil::lower_ascii(nm);
        if (!aset.count(nm) || dset.count(nm)) continue;
        out.push_back(t);
    }
    return out;
}

nlohmann::json tool_catalog_json() {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& t : tool_catalog()) {
        arr.push_back({
            {"name",         t.name},
            {"description",  t.description},
            {"input_schema", t.input_schema},
        });
    }
    return nlohmann::json{{"tools", arr}};
}

nlohmann::json tool_catalog_openai() {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& t : tool_catalog()) {
        arr.push_back({
            {"type", "function"},
            {"function", {
                {"name",        t.name},
                {"description", t.description},
                {"parameters",  t.input_schema},
            }},
        });
    }
    return arr;
}

nlohmann::json tool_catalog_openai_excluding(const std::vector<std::string>& disabled) {
    nlohmann::json       arr = nlohmann::json::array();
    for (const auto& t : tool_catalog_excluding(disabled)) {
        arr.push_back({
            {"type", "function"},
            {"function", {
                {"name",        t.name},
                {"description", t.description},
                {"parameters",  t.input_schema},
            }},
        });
    }
    return arr;
}

nlohmann::json tool_catalog_openai_allowing(const std::vector<std::string>& allowed,
                                            const std::vector<std::string>& disabled) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& t : tool_catalog_allowing(allowed, disabled)) {
        arr.push_back({
            {"type", "function"},
            {"function", {
                {"name",        t.name},
                {"description", t.description},
                {"parameters",  t.input_schema},
            }},
        });
    }
    return arr;
}

// ── Flag-based helpers ─────────────────────────────────────────────────────
//
// k_tool_flag_table removed — use pm::llm::agent_tool_registry_data() instead.

std::vector<std::string> agent_tools_disabled_names(pm::llm::AgentTools flags) {
    std::vector<std::string> out;
    const auto* reg  = pm::llm::agent_tool_registry_data();
    const auto  size = pm::llm::agent_tool_registry_size();
    for (std::size_t i = 0; i < size; ++i) {
        if (!pm::llm::agent_tool_enabled(flags, reg[i].flag))
            out.emplace_back(reg[i].name);
    }
    return out;
}

nlohmann::json tool_catalog_openai_for_flags(pm::llm::AgentTools flags) {
    return tool_catalog_openai_excluding(agent_tools_disabled_names(flags));
}

std::vector<ToolDef> tool_catalog_for_flags(pm::llm::AgentTools flags) {
    return tool_catalog_excluding(agent_tools_disabled_names(flags));
}

} // namespace media::llm::path

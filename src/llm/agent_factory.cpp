#include "agent_factory.hpp"
#include "json/json_utils.hpp"

#include <cstdio>
#include "agent_internal.hpp"  // detail::emit, detail::merge_llm_usage_from_provider_meta
#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
#  include "llm/agent_feature_BrowserUse.hpp"
#  include "llm/tools/browser_use/Tool_BrowserUse.hpp"
#  include "llm/tools/browser_use/browser_use_inject.hpp"
#  if defined(_WIN32)
#    include "core/host_backends.hpp"
#  endif
#endif
#include "llm/agent_feature_Workflow.hpp"

#include "fs/path_sanitizer.hpp"
#include "llm/path_tool_catalog.hpp"
#include "llm/path_tool_executor.hpp"
#include "llm/tools/run/RunTool.hpp"
#include "llm/tools/network/NetworkTools.hpp"
#include "llm/agent_tool_provider.hpp"
#if defined(FEATURE_SECURITY_AGENT) && FEATURE_SECURITY_AGENT
#  include "llm/security_review.hpp"
#endif
#include "agent_memory.hpp"
#include "agent_memory_artifacts.hpp"
#include "core/app_exe_directory.hpp"
#include "core/settings_store.hpp"
#include "core/cli_cancel.hpp"
#if defined(_WIN32)
#  include "core/library_utils.hpp"
#endif
#include "string/string_sanitizers.hpp"
#include "core/chat_settings_schema.hpp"
#include "ui_defaults.hpp"
#include "core/pixlwiz_provider_models_cli.hpp"
#include "logging/logger.h"
#include "policy/app_policy.hpp"

#include "kbot.h"
#include "llm_client.h"
#include "llama_agent_client.hpp"
#include "loopback_agent_client.hpp"

#if defined(FEATURE_LLM_COMPRESSION) && FEATURE_LLM_COMPRESSION
#  include "llm/reduce/reduce.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
#include "llm/tools/browser_use/browser_use_compact.hpp"
#endif
#include "string/string_utils.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
#include "win/web/webview_BrowserUse.hpp"
#include "llm/tools/browser_use/Tool_BrowserUse.hpp"
#endif
#endif

#if defined(FEATURE_SECURITY_AGENT) && FEATURE_SECURITY_AGENT
#  include "security/CliConsentDelegate.hpp"
#  include "security/ConsentDelegateRegistry.hpp"
#  include "security/SecurityGateway.hpp"
#  include <filesystem>
#  if defined(_WIN32) && defined(FEATURE_SECURITY_SANDBOX) && FEATURE_SECURITY_SANDBOX
#    include "security/backends/win/SandboxAppContainer.hpp"
#    define PM_AGENT_HAS_SANDBOX_BACKEND 1
#  endif
#endif

namespace media::llm::agent {
namespace {

constexpr const char* k_request_tools_name = "request_tools";

void apply_create_video_ui_overrides(nlohmann::json& args, const nlohmann::json& ov) {
    if (!ov.is_object() || !args.is_object())
        return;
    bool pinned_kf = false;
    for (auto it = ov.begin(); it != ov.end(); ++it) {
        const std::string& k = it.key();
        const auto&        v = it.value();
        if (v.is_null())
            continue;
        if (v.is_string()) {
            const std::string sv = v.get<std::string>();
            if (sv.empty())
                continue;
            if (k == "start_frame" || k == "last_frame" || k == "image")
                pinned_kf = true;
            if (sv == "true") {
                args[k] = true;
                continue;
            }
            if (sv == "false") {
                args[k] = false;
                continue;
            }
            bool all_digit = true;
            for (unsigned char c : sv)
                if (!std::isdigit(c)) {
                    all_digit = false;
                    break;
                }
            if (all_digit) {
                try {
                    args[k] = std::stoll(sv);
                } catch (...) {
                    args[k] = v;
                }
                continue;
            }
            args[k] = v;
        } else {
            if ((k == "start_frame" || k == "last_frame" || k == "image") && v.is_string())
                pinned_kf = true;
            args[k] = v;
        }
    }
    if (pinned_kf) {
        args.erase("src");
        args.erase("paths");
    }
}

std::string still_path_key(std::string p)
{
    for (char& c : p) {
        if (c == '\\')
            c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return p;
}

bool looks_like_still_path(const std::string& p)
{
    const auto slash = p.find_last_of("/\\");
    const auto dot = p.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return false;
    std::string ext = p.substr(dot);
    ext = media::strutil::lower_ascii(std::move(ext));
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".jpe"
        || ext == ".jfif" || ext == ".webp" || ext == ".gif" || ext == ".bmp"
        || ext == ".tif" || ext == ".tiff" || ext == ".avif" || ext == ".heic"
        || ext == ".heif";
}

void collect_json_path_strings(const nlohmann::json& v, std::vector<std::string>& out)
{
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        if (!s.empty())
            out.push_back(s);
        return;
    }
    if (!v.is_array())
        return;
    for (const auto& item : v) {
        if (!item.is_string())
            continue;
        const std::string s = item.get<std::string>();
        if (!s.empty())
            out.push_back(s);
    }
}

/// Extra filmstrip / Explorer stills (Turn.selection) become options.reference_images
/// unless they are already the destination in `paths`. Naming a file in the prompt
/// does not send pixels — this is what actually attaches content.png when compositing
/// into glass1.png.
void merge_context_stills_as_reference_images(nlohmann::json& args,
                                              const std::vector<std::string>& selection)
{
    if (!args.is_object() || selection.empty())
        return;

    std::unordered_set<std::string> skip;
    if (args.contains("paths")) {
        std::vector<std::string> paths;
        collect_json_path_strings(args["paths"], paths);
        for (const auto& p : paths)
            skip.insert(still_path_key(p));
    }

    nlohmann::json opts = nlohmann::json::object();
    if (args.contains("options") && args["options"].is_object())
        opts = args["options"];

    std::vector<std::string> refs;
    collect_json_path_strings(opts.contains("reference_images") ? opts["reference_images"]
                                                               : nlohmann::json(),
                             refs);
    collect_json_path_strings(opts.contains("references") ? opts["references"] : nlohmann::json(),
                             refs);
    collect_json_path_strings(opts.contains("reference") ? opts["reference"] : nlohmann::json(),
                             refs);

    std::unordered_set<std::string> have;
    for (const auto& p : refs)
        have.insert(still_path_key(p));

    std::size_t added = 0;
    for (const auto& p : selection) {
        if (p.empty() || !looks_like_still_path(p))
            continue;
        const std::string key = still_path_key(p);
        if (skip.count(key) || have.count(key))
            continue;
        refs.push_back(p);
        have.insert(key);
        ++added;
    }
    if (added == 0)
        return;

    opts.erase("references");
    opts.erase("reference");
    opts["reference_images"] = refs;
    args["options"] = std::move(opts);
    logger::info(std::string("image_transform: attached ")
                 + std::to_string(added)
                 + " filmstrip still(s) as reference_images");
}

void fill_image_understand_paths_from_selection(nlohmann::json& args,
                                                const std::vector<std::string>& selection)
{
    if (!args.is_object() || selection.empty())
        return;
    std::vector<std::string> existing;
    if (args.contains("paths"))
        collect_json_path_strings(args["paths"], existing);
    if (!existing.empty())
        return;

    nlohmann::json paths = nlohmann::json::array();
    for (const auto& p : selection) {
        if (!p.empty() && looks_like_still_path(p))
            paths.push_back(p);
    }
    if (paths.empty())
        return;
    args["paths"] = std::move(paths);
    const bool missing_prompt = !args.contains("prompt") || !args["prompt"].is_string()
        || args["prompt"].get<std::string>().empty();
    if (missing_prompt)
        args["prompt"] = "Describe this image.";
    logger::info(std::string("image_understand: filled empty paths from selection (")
                 + std::to_string(args["paths"].size()) + ")");
}

void apply_image_tool_context_refs(nlohmann::json& args, const std::string& name,
                                   const std::vector<std::string>& selection)
{
    if (name == "image_understand") {
        fill_image_understand_paths_from_selection(args, selection);
        return;
    }
    if (name != "image_transform" && name != "transform" && name != "image_create")
        return;
    merge_context_stills_as_reference_images(args, selection);
}

struct ToolTaskContextGuard {
    std::string previous;
    explicit ToolTaskContextGuard(const std::string& id)
        : previous(get_current_task_id()) {
        if (!id.empty()) set_current_task_id(id);
    }
    ~ToolTaskContextGuard() {
        if (previous.empty()) clear_current_task_id();
        else set_current_task_id(previous);
    }
};

std::string canonical_tool_name(std::string name)
{
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front())))
        name.erase(name.begin());
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
        name.pop_back();
    name = media::strutil::lower_ascii(name);
    return name;
}

/// Weak models spam the same call (31× memory_write, 6× create_command).
/// Fingerprint is canonical name + stable JSON args (nlohmann objects sort keys).
std::string tool_call_signature(const std::string& name, const nlohmann::json& args)
{
    return canonical_tool_name(name) + "\n" + (args.is_null() ? std::string{} : args.dump());
}

nlohmann::json duplicate_tool_call_envelope(const std::string& name)
{
    const std::string canon = canonical_tool_name(name);
    return nlohmann::json{
        {"ok", true},
        {"skipped", true},
        {"duplicate", true},
        {"tool", canon},
        {"next_step",
         "This exact tool call already ran this turn. Do not repeat it. "
         "Use the first result or answer the user."},
        {"message", "Skipped duplicate " + canon + " (same arguments)."},
    };
}

bool responses_streaming_enabled(const ProviderSettings& provider)
{
    if (provider.api_mode != LlmApiMode::Responses)
        return false;
    switch (provider.streaming_mode) {
    case LlmStreamingMode::Off:
        return false;
    case LlmStreamingMode::On:
        return true;
    case LlmStreamingMode::Auto:
    default:
        if (provider.router == "tanit")
            return media::pixlwiz_cli::tanit_supports_responses_streaming(provider.model);
        return true;
    }
}

std::string openai_tool_name(const nlohmann::json& tool)
{
    if (!tool.is_object() || !tool.contains("function") || !tool["function"].is_object())
        return {};
    return tool["function"].value("name", std::string{});
}

bool tool_name_starts_with(const std::string& name, const char* prefix)
{
    const std::string n = canonical_tool_name(name);
    const std::string p = canonical_tool_name(prefix);
    return n.size() >= p.size() && n.compare(0, p.size(), p) == 0;
}

nlohmann::json request_tools_schema()
{
    return nlohmann::json{
        {"type", "function"},
        {"function", {
            {"name", k_request_tools_name},
            {"description",
             "Request additional tools for this turn when the current tool set is insufficient: path "
             "tools and remote MCP tools (mcp_* names from info_lookup kind=mcp). "
             "Call each granted tool in the next step. Do not request the same name twice. "
             "Do not use create_command as a substitute."},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"names", {
                        {"type", "array"},
                        {"items", {{"type", "string"}}},
                        {"description", "Tool names to add: path tools (e.g. file_read, file_search, search_index, search_query, run, write_file, image_understand) "
                                        "or remote MCP tools by their mcp_<server>__<tool> name (from info_lookup kind=mcp)."},
                    }},
                    {"reason", {
                        {"type", "string"},
                        {"description", "Short reason these extra tools are needed."},
                    }},
                }},
                {"required", nlohmann::json::array({"names"})},
            }},
        }},
    };
}

bool has_openai_tool(const nlohmann::json& tools, const std::string& name)
{
    if (!tools.is_array()) return false;
    const std::string target = canonical_tool_name(name);
    for (const auto& tool : tools) {
        if (canonical_tool_name(openai_tool_name(tool)) == target)
            return true;
    }
    return false;
}

void append_request_tools_schema(nlohmann::json& tools)
{
    if (!tools.is_array()) tools = nlohmann::json::array();
    if (!has_openai_tool(tools, k_request_tools_name))
        tools.push_back(request_tools_schema());
}

bool response_has_request_tools(const polymech::kbot::LLMResponse& resp)
{
    for (const auto& tc : resp.tool_calls)
        if (canonical_tool_name(tc.name) == k_request_tools_name)
            return true;
    return false;
}

#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
bool response_has_browser_use_tools(const polymech::kbot::LLMResponse& resp)
{
    for (const auto& tc : resp.tool_calls)
        if (media::llm::browser_use::is_browser_use_tool(tc.name))
            return true;
    return false;
}
#endif

/// Network tools act on live devices and remote hosts: call order matters
/// (write, then read back), so they never take the parallel path.
bool response_has_network_tools(const polymech::kbot::LLMResponse& resp)
{
    for (const auto& tc : resp.tool_calls)
        if (path::detail::is_network_tool(canonical_tool_name(tc.name)))
            return true;
    return false;
}

/// After a call that may change remote state, an identical network read or
/// command returns new data, so earlier network signatures stop counting as
/// duplicates.
void forget_network_tool_signatures(std::unordered_set<std::string>& signatures)
{
    for (auto it = signatures.begin(); it != signatures.end();) {
        const std::string_view sig = *it;
        if (path::detail::is_network_tool(sig.substr(0, sig.find('\n'))))
            it = signatures.erase(it);
        else
            ++it;
    }
}

bool response_tools_available(const polymech::kbot::LLMResponse& resp, const nlohmann::json& tools)
{
    for (const auto& tc : resp.tool_calls)
        if (!has_openai_tool(tools, tc.name))
            return false;
    return true;
}

const nlohmann::json* find_mcp_catalog_tool(const nlohmann::json& catalog, const std::string& name)
{
    if (!catalog.is_array()) return nullptr;
    const std::string target = canonical_tool_name(name);
    for (const auto& tool : catalog)
        if (canonical_tool_name(openai_tool_name(tool)) == target)
            return &tool;
    return nullptr;
}

std::vector<nlohmann::json> preserve_mcp_tools(const nlohmann::json& tools)
{
    std::vector<nlohmann::json> out;
    if (!tools.is_array()) return out;
    for (const auto& tool : tools) {
        const std::string name = openai_tool_name(tool);
        if (tool_name_starts_with(name, "mcp_"))
            out.push_back(tool);
    }
    return out;
}

void rebuild_progressive_tools(Turn& working, PreparedRun& prepared)
{
    nlohmann::json rebuilt = path::tool_catalog_openai_allowing(
        working.allowed_path_tools, working.disabled_path_tools);
    for (const auto& mcp_tool : preserve_mcp_tools(prepared.tools))
        rebuilt.push_back(mcp_tool);
    if (prepared.host_tool_schemas.is_array()) {
        for (const auto& host_tool : prepared.host_tool_schemas)
            rebuilt.push_back(host_tool);
    }
    append_request_tools_schema(rebuilt);
    prepared.tools = std::move(rebuilt);
}

bool dispatch_host_tool(PreparedRun&           prepared,
                        const std::string&     session_id,
                        const std::string&     cwd,
                        const std::string&     name,
                        const nlohmann::json&  args,
                        nlohmann::json&        envelope,
                        std::string&           envelope_str)
{
    IAgentToolProvider* provider = find_host_tool(prepared.host_providers, name);
    if (!provider)
        return false;
    ToolContext ctx;
    ctx.session_id = session_id;
    ctx.cwd        = cwd;
    ToolResult r   = provider->invoke(name, args, ctx);
    envelope       = r.envelope.is_object() ? r.envelope : nlohmann::json{{"ok", r.ok}};
    if (!envelope.contains("ok"))
        envelope["ok"] = r.ok;
    envelope["provider"] = provider->id();
    envelope_str         = detail::safe_json_dump(envelope);
    return true;
}

std::vector<std::string> request_tool_names(const nlohmann::json& args)
{
    std::vector<std::string> names;
    if (!args.is_object() || !args.contains("names") || !args["names"].is_array())
        return names;
    std::unordered_set<std::string> seen;
    for (const auto& raw : args["names"]) {
        if (!raw.is_string()) continue;
        const std::string name = canonical_tool_name(raw.get<std::string>());
        // mcp_* names are allowed now — they are granted on demand from the
        // side MCP catalog rather than the path-tool catalog.
        if (name.empty() || name == k_request_tools_name)
            continue;
        if (seen.insert(name).second)
            names.push_back(name);
    }
    return names;
}

nlohmann::json handle_request_tools(Turn& working,
                                    PreparedRun& prepared,
                                    const nlohmann::json& args)
{
    const std::vector<std::string> requested = request_tool_names(args);

    // Split into MCP (mcp_*) and path-tool buckets — each has its own catalog
    // and grant path.
    std::vector<std::string> requested_path;
    std::vector<std::string> requested_mcp;
    for (const auto& name : requested) {
        if (tool_name_starts_with(name, "mcp_")) requested_mcp.push_back(name);
        else                                      requested_path.push_back(name);
    }

    // Path tools: validate against the enabled path catalog.
    std::vector<std::string> valid;
    valid.reserve(requested_path.size());
    for (const auto& def : path::tool_catalog_allowing(requested_path, working.disabled_path_tools))
        valid.push_back(canonical_tool_name(def.name));
    std::unordered_set<std::string> valid_set(valid.begin(), valid.end());

    for (auto& n : working.allowed_path_tools)
        n = canonical_tool_name(n);
    std::unordered_set<std::string> active(working.allowed_path_tools.begin(), working.allowed_path_tools.end());

    std::vector<std::string> granted;
    std::vector<std::string> already_active;
    std::vector<std::string> denied;

    for (const auto& name : requested_path) {
        if (!valid_set.count(name)) {
            denied.push_back(name);
            continue;
        }
        if (active.count(name)) {
            already_active.push_back(name);
            continue;
        }
        working.allowed_path_tools.push_back(name);
        prepared.progressive_granted_path_tools.push_back(name);
        active.insert(name);
        granted.push_back(name);
    }
    // A converted-document pass ends in write_file. Grant it with the search
    // tools so a later "do not request_tools" turn can still write the summary.
    bool granted_search = false;
    for (const auto& name : granted) {
        if (name == "search_index" || name == "search_query" || name == "search_detail")
            granted_search = true;
    }
    if (granted_search && !active.count("write_file")) {
        bool write_ok = false;
        for (const auto& def : path::tool_catalog_allowing(
                 std::vector<std::string>{"write_file"}, working.disabled_path_tools)) {
            if (canonical_tool_name(def.name) == "write_file") {
                write_ok = true;
                break;
            }
        }
        if (write_ok) {
            working.allowed_path_tools.push_back("write_file");
            prepared.progressive_granted_path_tools.push_back("write_file");
            active.insert("write_file");
            granted.push_back("write_file");
        }
    }
    working.planner_path_tool_allowlist_set = true;

    // MCP tools: copy the matching schema from the side catalog into tools[].
    // rebuild_progressive_tools() (below) preserves whatever mcp_* schemas are
    // already present in prepared.tools, so the grant persists across rounds.
    for (const auto& name : requested_mcp) {
        if (has_openai_tool(prepared.tools, name)) {
            already_active.push_back(name);
            continue;
        }
        const nlohmann::json* schema = find_mcp_catalog_tool(prepared.mcp_tool_catalog, name);
        if (!schema) {
            denied.push_back(name);
            continue;
        }
        prepared.tools.push_back(*schema);
        granted.push_back(name);
    }

    // A short model invents mcp_<exe>__download, or yt_dlp, for a program that
    // is an executable on ui.appPathDirectories. That name is denied, ok stays
    // true, and the reply says the program is missing. Grant run and name the exe.
    std::vector<std::string> resolved_exes;
    if (!denied.empty()) {
        auto consider = [&](const std::string& token) {
            if (token.empty() || token == "run")
                return;
            // Tool names use underscores (yt_dlp). The executable on the app
            // path often uses hyphens (yt-dlp). Try both spellings.
            std::vector<std::string> spellings{token};
            if (token.find('_') != std::string::npos) {
                std::string hyphens = token;
                for (char& c : hyphens)
                    if (c == '_') c = '-';
                spellings.push_back(std::move(hyphens));
            }
            if (token.find('-') != std::string::npos) {
                std::string underscores = token;
                for (char& c : underscores)
                    if (c == '-') c = '_';
                spellings.push_back(std::move(underscores));
            }
            for (const auto& spelling : spellings) {
                if (spelling.empty() || spelling == "run")
                    continue;
                if (std::find(resolved_exes.begin(), resolved_exes.end(), spelling) != resolved_exes.end())
                    return;
                if (media::app::resolve_third_party_executable(spelling).empty())
                    continue;
                resolved_exes.push_back(spelling);
                return;
            }
        };
        for (const auto& name : denied) {
            if (tool_name_starts_with(name, "mcp_")) {
                const std::string rest = name.substr(4);
                const auto sep = rest.find("__");
                if (sep != std::string::npos && sep > 0)
                    consider(rest.substr(0, sep));
                else if (!rest.empty())
                    consider(rest);
            } else {
                consider(name);
            }
        }
        if (!resolved_exes.empty()) {
            bool run_ok = false;
            for (const auto& def : path::tool_catalog_allowing(
                     std::vector<std::string>{"run"}, working.disabled_path_tools)) {
                if (canonical_tool_name(def.name) == "run") {
                    run_ok = true;
                    break;
                }
            }
            if (run_ok && !active.count("run")) {
                working.allowed_path_tools.push_back("run");
                prepared.progressive_granted_path_tools.push_back("run");
                active.insert("run");
                granted.push_back("run");
            }
        }
    }

    rebuild_progressive_tools(working, prepared);

    nlohmann::json active_tools = nlohmann::json::array();
    for (const auto& tool : prepared.tools) {
        const std::string name = canonical_tool_name(openai_tool_name(tool));
        if (!name.empty()) active_tools.push_back(name);
    }

    auto join_names = [](const std::vector<std::string>& names) {
        std::string out;
        for (const auto& n : names) {
            if (!out.empty()) out += ", ";
            out += n;
        }
        return out;
    };
    std::string next_step;
    if (!granted.empty())
        next_step = "Call " + join_names(granted)
            + " now. Do not call request_tools or create_command again.";
    else if (!already_active.empty())
        next_step = "Already in your catalog: " + join_names(already_active)
            + ". Call that tool now. Do not request it again.";
    else if (!denied.empty())
        next_step = "Those names were denied. Use info_lookup kind=mcp (query=mcp) to list real mcp_* names.";
    else
        next_step = "Pass names[] of path tools or mcp_* tools, then call the granted tool.";
    if (!resolved_exes.empty()) {
        next_step = "Call run now with command \"" + join_names(resolved_exes)
            + "\" plus the user's arguments. That name is an executable on the app path, "
              "not an mcp_* tool. Do not say the program is missing. Do not call request_tools again.";
    }

    std::string message = granted.empty()
        ? (already_active.empty() ? "No new tools were granted." : next_step)
        : "Granted. " + next_step;
    if (!resolved_exes.empty())
        message = next_step;

    nlohmann::json out{
        {"ok", true},
        {"requested", requested},
        {"granted", granted},
        {"already_active", already_active},
        {"denied", denied},
        {"active_tools", active_tools},
        {"next_step", next_step},
        {"message", message},
    };
    if (!resolved_exes.empty())
        out["resolved_executables"] = resolved_exes;
    return out;
}

#if defined(_WIN32)
std::wstring agent_getenv_w(const wchar_t* name)
{
    const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return {};
    std::wstring value(needed, L'\0');
    const DWORD n = ::GetEnvironmentVariableW(name, value.data(), needed);
    if (n == 0) return {};
    value.resize(n);
    return value;
}

bool agent_path_contains_part(const std::wstring& path, const std::wstring& part)
{
    if (path.empty() || part.empty()) return false;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = path.find(L';', start);
        std::wstring item = path.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        std::wstring cmp = part;
        while (!item.empty() && (item.back() == L'\\' || item.back() == L'/')) item.pop_back();
        while (!cmp.empty() && (cmp.back() == L'\\' || cmp.back() == L'/')) cmp.pop_back();
        if (!item.empty() && ::CompareStringOrdinal(item.c_str(), -1, cmp.c_str(), -1, TRUE) == CSTR_EQUAL)
            return true;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return false;
}

std::wstring agent_exe_adjacent_bin_dir()
{
    std::wstring buf(MAX_PATH, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    const size_t slash = buf.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    std::wstring bin = buf.substr(0, slash + 1) + L"bin";
    const DWORD attrs = ::GetFileAttributesW(bin.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return {};
    return bin;
}

std::wstring agent_utf8_to_wide(const std::string& text)
{
    if (text.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          out.data(), n);
    return out;
}

void ensure_agent_process_path_has_exe_bin()
{
    static std::once_flag once;
    std::call_once(once, [] {
        const std::wstring bin = agent_exe_adjacent_bin_dir();
        if (bin.empty()) return;

        // SetEnvironmentVariableW — helps child processes find DLLs in bin/.
        std::wstring path = agent_getenv_w(L"Path");
        if (path.empty()) path = agent_getenv_w(L"PATH");
        if (!agent_path_contains_part(path, bin)) {
            path = path.empty() ? bin : bin + L";" + path;
            ::SetEnvironmentVariableW(L"Path", path.c_str());
        }

        // add_search_dir — helps the current process loader find DLLs in bin/
        // (SetEnvironmentVariableW alone does not affect the already-running process).
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, bin.c_str(), -1,
                                            nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            std::string bin_u8(static_cast<size_t>(n - 1), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, bin.c_str(), -1,
                                  bin_u8.data(), n, nullptr, nullptr);
            media::library::add_search_dir(bin_u8);
        }
        media::library::add_search_dir("${ENV:CUDA_PATH}\\bin");
        media::library::add_search_dir("${ENV:CUDNN_PATH}");
    });
}
#else
void ensure_agent_process_path_has_exe_bin() {}
#endif

void prepend_app_search_dirs_to_process_path()
{
    static std::mutex path_mu;
    std::lock_guard<std::mutex> lock(path_mu);
    const auto dirs = media::ui_defaults::app_search_path_dirs();
    if (dirs.empty())
        return;

#if defined(_WIN32)
    std::wstring path = agent_getenv_w(L"Path");
    if (path.empty())
        path = agent_getenv_w(L"PATH");
    bool changed = false;
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
        if (it->empty())
            continue;
        const std::wstring dir = agent_utf8_to_wide(*it);
        if (dir.empty() || agent_path_contains_part(path, dir))
            continue;
        path = path.empty() ? dir : dir + L";" + path;
        changed = true;
    }
    if (changed)
        ::SetEnvironmentVariableW(L"Path", path.c_str());
#else
    const char* current = std::getenv("PATH");
    std::string path = current ? current : "";
    const auto contains = [](const std::string& haystack, const std::string& part) {
        size_t start = 0;
        while (start <= haystack.size()) {
            const size_t end = haystack.find(':', start);
            std::string item = haystack.substr(
                start, end == std::string::npos ? std::string::npos : end - start);
            while (!item.empty() && item.back() == '/')
                item.pop_back();
            std::string cmp = part;
            while (!cmp.empty() && cmp.back() == '/')
                cmp.pop_back();
            if (!item.empty() && item == cmp)
                return true;
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        return false;
    };
    bool changed = false;
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
        if (it->empty() || contains(path, *it))
            continue;
        path = path.empty() ? *it : *it + ":" + path;
        changed = true;
    }
    if (changed)
        ::setenv("PATH", path.c_str(), 1);
#endif
}

#if defined(FEATURE_LLM_COMPRESSION) && FEATURE_LLM_COMPRESSION
// ── Pre-API context reduction filter ──────────────────────────────────────────
//
// The "pre-API filter": runs over the message history immediately before each
// LLM call and shrinks oversized payloads in a wire copy. The canonical
// `messages` vector (and the transcript) keep full fidelity — only the bytes
// handed to the provider are reduced. reduce() auto-routes: JSON/code tool
// envelopes take the structural terminal path; free text takes the deterministic
// attention pipeline (protect → dedup → levels → entropy → lexical), conditioned
// on `objective`. Model-backed score stages are not supplied here yet.

/// Reduce a single message body in place. Returns true when it shrank.
bool reduce_message_body(std::string& content,
                         std::string_view objective,
                         const Turn& turn)
{
    if (content.size() < turn.context_reduction_min_bytes) return false;

    namespace mr = media::llm::reduce;
    mr::ReduceRequest req;
    req.content = content;
    if (!objective.empty()) req.objective = objective;
    req.budget = mr::Budget{mr::BudgetKind::Ratio, turn.context_reduction_ratio};
    // Deterministic core only — no embed/kompress callbacks at this seam yet.

    const mr::ReduceResult r = mr::reduce(req);
    if (!r.ok || r.compressed.empty() || r.compressed.size() >= content.size())
        return false;  // never grow; keep the original on a no-op/failure
    content = r.compressed;
    return true;
}

/// Build a reduced wire copy of `messages` according to the turn's policy.
/// `bytes_saved` / `msgs_reduced` accumulate telemetry for the debug log.
std::vector<polymech::kbot::ChatMessage>
pre_api_reduce(const std::vector<polymech::kbot::ChatMessage>& in,
               const Turn& turn,
               std::size_t& bytes_saved,
               std::size_t& msgs_reduced)
{
    std::vector<polymech::kbot::ChatMessage> out = in;

    const std::string_view objective =
        turn.context_reduction_objective.empty()
            ? std::string_view(turn.user_prompt)
            : std::string_view(turn.context_reduction_objective);

    // Protect the live user turn (the actual ask) from reduction.
    std::size_t last_user = out.size();
    for (std::size_t i = out.size(); i-- > 0;)
        if (out[i].role == "user") { last_user = i; break; }

    const auto mode = turn.context_reduction_mode;
    for (std::size_t i = 0; i < out.size(); ++i) {
        auto& m = out[i];
        if (m.role == "system") continue;     // never touch the system carrier
        if (i == last_user)      continue;     // never touch the live user turn
        if (m.content.empty())   continue;     // assistant tool-call carriers, etc.

        const bool is_tool = (m.role == "tool");
        const bool is_ctx  = (m.role == "assistant" || m.role == "user");
        const bool eligible =
            (mode == pm::llm::ContextReductionMode::ToolResults && is_tool) ||
            (mode == pm::llm::ContextReductionMode::All && (is_tool || is_ctx));
        if (!eligible) continue;

        const std::size_t before = m.content.size();
        if (reduce_message_body(m.content, objective, turn)) {
            ++msgs_reduced;
            bytes_saved += before - m.content.size();
        }
    }
    return out;
}
#endif  // FEATURE_LLM_COMPRESSION

#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
/// Drop stale browser tool payloads from agent history before each LLM round.
/// Keeps the latest read and latest batch actionable; older entries become stubs.
/// Click/scope/navigate also decay.
void compact_stale_browser_tool_messages(std::vector<polymech::kbot::ChatMessage>& messages)
{
    std::size_t last_read_idx = messages.size();
    std::size_t last_batch_idx = messages.size();
    std::size_t last_action_idx = messages.size();

    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].role != "tool")
            continue;
        try {
            const nlohmann::json parsed = nlohmann::json::parse(messages[i].content);
            const std::string tool = parsed.value("tool", std::string{});
            if (tool == "browser_read" || tool == "browser_find")
                last_read_idx = i;
            else if (tool == "browser_batch")
                last_batch_idx = i;
            else if (tool == "browser_click" || tool == "browser_type" || tool == "browser_select"
                     || tool == "browser_scope" || tool == "browser_navigate")
                last_action_idx = i;
        } catch (...) {
        }
    }

    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].role != "tool")
            continue;
        try {
            nlohmann::json parsed = nlohmann::json::parse(messages[i].content);
            const std::string tool = parsed.value("tool", std::string{});
            if (!media::llm::browser_use::is_browser_history_tool(tool))
                continue;

            if (tool == "browser_batch") {
                const bool keep = i == last_batch_idx;
                parsed = media::llm::browser_use::compact_batch_envelope(std::move(parsed), keep);
                if (!keep)
                    parsed["history_compacted"] = true;
            } else if (tool == "browser_read" || tool == "browser_find") {
                const bool keep = i == last_read_idx;
                if (parsed.contains("data"))
                    parsed["data"] = media::llm::browser_use::compact_read_data(parsed["data"], keep);
                if (!keep)
                    parsed["history_compacted"] = true;
            } else {
                const bool keep = i == last_action_idx;
                parsed = media::llm::browser_use::compact_action_envelope(std::move(parsed), keep);
                if (!keep)
                    parsed["history_compacted"] = true;
            }

            messages[i].content = detail::safe_json_dump(parsed);
        } catch (...) {
        }
    }
}
#endif  // FEATURE_BROWSER

#if defined(FEATURE_SECURITY_AGENT) && FEATURE_SECURITY_AGENT
// ── Security gateway helpers ─────────────────────────────────────────────────

/// Derive the primary Action for a tool call from its registered name.
static pm::security::Action security_action_for_tool(const std::string& name)
{
    // ── Write ──────────────────────────────────────────────────────────────
    if (name == "write_file" || name == "create_file" ||
        name == "file_compose" ||
        name == "file_str_replace" || name == "download_url" ||
        name == "image_transform" || name == "transform" ||
        name == "image_create" || name == "image_from_camera" ||
        name == "create_video" ||
        name == "memory_write" || name == "memory_append_event")
        return pm::security::Action::Write;
    // app_screenshot writes an image file (output_path)
    if (name == "app_screenshot")
        return pm::security::Action::Write;

    // ── Delete ─────────────────────────────────────────────────────────────
    if (name == "file_delete" || name == "delete_file" || name == "remove_file"
        || name == "service_files_delete")
        return pm::security::Action::Delete;

    // ── Execute ────────────────────────────────────────────────────────────
    // Shell / command execution
    if (name == "run" || name == "run_sequence")
        return pm::security::Action::Execute;
    // Computer-use: inspect, click, drag, type, hotkey, open, close, batch
    // These interact with the OS / other applications at a higher privilege
    // than a simple file read, so Execute is the right semantic.
    if (name == "app_inspect_dump" || name == "app_inspect_find" ||
        name == "app_click"        || name == "app_drag" ||
        name == "app_type"         || name == "app_hotkey" ||
        name == "app_close"        || name == "app_batch" ||
        name == "app_open")
        return pm::security::Action::Execute;
    if (name == "browser_click" || name == "browser_type" || name == "browser_select"
        || name == "browser_batch" || name == "browser_navigate")
        return pm::security::Action::Execute;
    if (name == "app_command")
        return pm::security::Action::Execute;
    // Remote command execution and device / broker writes
    if (name == "ssh_exec" || name == "mqtt" || name == "modbus_write")
        return pm::security::Action::Execute;
    // Scheduling creates or modifies OS-level timers
    if (name == "schedule_at" || name == "schedule_in" || name == "schedule_every")
        return pm::security::Action::Execute;
    // Custom command authoring has code-execution implications
    if (name == "create_command")
        return pm::security::Action::Execute;

    // ── Default: read ──────────────────────────────────────────────────────
    return pm::security::Action::Read;
}

/// Build a ToolRequest for an agent-dispatched tool call.
static pm::security::ToolRequest build_agent_tool_request(
    const std::string&             tool_name,
    const nlohmann::json&          args,
    bool                           is_mcp,
    const std::string&             session_id,
    const std::string&             task_id,
    const std::filesystem::path&   workspace_root)
{
    namespace sec = pm::security;
    sec::ToolRequest req;
    req.actor        = sec::Actor::LLM;
    req.sessionId    = session_id;
    req.taskId       = task_id;
    req.workspaceRoot = workspace_root;
    if (!workspace_root.empty())
        req.workspaceId = media::path::path_to_generic_utf8(workspace_root);
    try { req.argumentsJson = args.dump(); } catch (...) {}
    // Signal sandbox availability so PolicyEngine can reflect containment in the
    // Execute risk level shown on the consent card (Medium = sandboxed, High = bare).
#if defined(FEATURE_SECURITY_SANDBOX) && FEATURE_SECURITY_SANDBOX
    req.sandboxAvailable = true;
#endif

    if (is_mcp) {
        req.tool.toolId   = tool_name;
        req.tool.provider = "mcp";
        req.action        = sec::Action::Execute;
        req.profile       = sec::Profile::Developer;  // Ask (not hard-Deny) so Phase 2 consent can grant
        sec::ResourceTarget tgt;
        tgt.uri = "mcp://" + tool_name;
        req.target = std::move(tgt);
    } else {
        req.tool.toolId   = "builtin." + tool_name;
        req.tool.provider = "builtin";
        req.action        = security_action_for_tool(tool_name);
        // Developer profile: workspace writes are Allow without consent,
        // out-of-workspace writes require Ask, dangerous actions (Execute for
        // non-file tools) require Ask. This is the right posture for the LLM
        // agent whose user has already opted in to the agent session.
        // Phase 2 will let the user set the profile via CLI flag or settings.
        req.profile = sec::Profile::Developer;

        // Extract the most useful resource target from common argument shapes.
        // Resolve relative paths against the workspace root so the consent card
        // shows the full absolute path rather than a bare filename like "dir.md".
        auto resolve_file_target = [&](const std::string& raw) -> sec::ResourceTarget {
            sec::ResourceTarget t;
            std::filesystem::path p(raw);
            if (p.is_relative() && !workspace_root.empty())
                p = workspace_root / p;
            std::error_code ec;
            p = std::filesystem::weakly_canonical(p, ec);
            t.path = p;
            t.uri  = "file://" + media::path::path_to_generic_utf8(p);
            return t;
        };
        sec::ResourceTarget tgt;
        if (tool_name == "service_files_delete") {
            // Remote VFS paths: never resolve them against the local workspace.
            const std::string mount = args.value("mount", std::string("home"));
            std::vector<std::string> paths;
            if (args.contains("path") && args["path"].is_string())
                paths.push_back(args["path"].get<std::string>());
            if (args.contains("paths") && args["paths"].is_array())
                for (const auto& p : args["paths"])
                    if (p.is_string())
                        paths.push_back(p.get<std::string>());
            tgt.uri = "vfs://" + mount + "/" + (paths.empty() ? std::string() : paths.front());
            if (paths.size() > 1)
                tgt.uri += " (+" + std::to_string(paths.size() - 1) + " more)";
        } else if (tool_name == "download_url") {
            if (args.contains("path") && args["path"].is_string()
                && !args["path"].get<std::string>().empty()) {
                tgt = resolve_file_target(args["path"].get<std::string>());
            } else {
                std::filesystem::path dest = workspace_root;
                if (dest.empty()) {
                    std::error_code ec;
                    dest = std::filesystem::current_path(ec);
                    if (ec)
                        dest.clear();
                }
                if (dest.empty())
                    dest = media::settings::get_config_dir() / "chrome-downloads";
                std::error_code ec;
                dest = std::filesystem::weakly_canonical(dest, ec);
                tgt.path = dest;
            }
            if (args.contains("url") && args["url"].is_string())
                tgt.uri = args["url"].get<std::string>();
        } else if (args.contains("path") && args["path"].is_string()) {
            tgt = resolve_file_target(args["path"].get<std::string>());
        } else if (args.contains("output_path") && args["output_path"].is_string()) {
            tgt = resolve_file_target(args["output_path"].get<std::string>());
        } else if (args.contains("glob_pattern") && args["glob_pattern"].is_string()) {
            // file_glob: show the pattern anchored to workspace root.
            const std::string pat = args["glob_pattern"].get<std::string>();
            std::filesystem::path base = workspace_root.empty()
                ? media::path::path_from_utf8(pat) : (workspace_root / pat);
            tgt.path = base;
            tgt.uri  = "file://" + media::path::path_to_generic_utf8(base);
        } else if (tool_name == "app_command") {
            if (args.contains("url") && args["url"].is_string())
                tgt.uri = args["url"].get<std::string>();
            else if (args.contains("command") && args["command"].is_string())
                tgt.uri = "app://" + args["command"].get<std::string>();
        } else if (args.contains("command") && args["command"].is_string()) {
            tgt.uri = "cmd://" + args["command"].get<std::string>();
        } else if (args.contains("url") && args["url"].is_string()) {
            tgt.uri = args["url"].get<std::string>();
        } else if (args.contains("app_name") && args["app_name"].is_string()) {
            // Computer-use tools identify their target by app_name.
            tgt.uri = "screen://" + args["app_name"].get<std::string>();
        } else if (args.contains("exe") && args["exe"].is_string() &&
                   !args["exe"].get<std::string>().empty()) {
            // app_open launches by exe (+ optional args). Surface it so the
            // consent dialog shows WHICH app is being started; keep the
            // screen:// scheme so computer-use policy / AI-review routing is
            // unchanged (scope stays broad — see buildConsentRequest).
            std::string launch = args["exe"].get<std::string>();
            if (args.contains("args") && args["args"].is_string() &&
                !args["args"].get<std::string>().empty())
                launch += " " + args["args"].get<std::string>();
            tgt.uri = "screen://" + launch;
        } else if (args.contains("process") && args["process"].is_string() &&
                   !args["process"].get<std::string>().empty()) {
            tgt.uri = "screen://" + args["process"].get<std::string>();
#if defined(_WIN32) && defined(FEATURE_BROWSER) && FEATURE_BROWSER
        } else if (tool_name.rfind("browser_", 0) == 0) {
            std::string raw;
            std::string browser_err;
            const nlohmann::json request{
                {"v", 1}, {"id", "security-context"}, {"cmd", "status"},
                {"args", nlohmann::json::object()},
            };
            if (pm::host::browser_use::rpc_active(
                    request.dump(), raw, 3000, browser_err)) {
                try {
                    const auto page = nlohmann::json::parse(raw);
                    tgt.uri = page.value("url", std::string{});
                } catch (...) {
                }
            }
            if (tgt.uri.empty())
                tgt.uri = "browser://active";
#endif
        } else if (tool_name == "app_screenshot" || tool_name == "app_inspect_dump" ||
                   tool_name == "app_inspect_find" || tool_name == "app_click"  ||
                   tool_name == "app_drag"          || tool_name == "app_type"  ||
                   tool_name == "app_hotkey"         || tool_name == "app_close" ||
                   tool_name == "app_batch"          || tool_name == "app_open") {
            // Computer-use tool without a named target — target is the screen.
            tgt.uri = "screen://";
        }
        if (!tgt.uri.empty())
            req.target = std::move(tgt);
    }
    return req;
}

/// Produce a "denied by security gateway" error envelope.
/// The model-visible error is intentionally generic — returning the specific
/// deny reason would act as a bypass roadmap, letting the model craft a
/// variation that avoids the matched pattern.  The real reason is logged
/// server-side (see the warn call at the call-site).
static nlohmann::json security_deny_envelope(
    const std::string& tool_name, const pm::security::PolicyDecision& decision)
{
    nlohmann::json env{
        {"ok",              false},
        {"error",           "Tool call blocked by security policy. "
                            "If you believe this is a mistake, ask the user to "
                            "adjust the security settings for '" + tool_name + "'."},
        {"security_denied", true},
        {"decision",        std::string(pm::security::to_string(decision.kind))},
    };
    return env;
}

class CliAiReviewConsentDelegate final : public pm::security::ConsentDelegate {
public:
    CliAiReviewConsentDelegate(ProviderSettings review_provider, bool json_mode)
        : review_provider_(std::move(review_provider)), cli_(json_mode) {}

    pm::security::ConsentResult requestConsent(const pm::security::ConsentRequest& cr) override
    {
        run_review(cr);
        return cli_.requestConsent(cr);
    }

private:
    ProviderSettings review_provider_;
    pm::security::CliConsentDelegate cli_;

    void emit_review_event(const nlohmann::json& event) const
    {
        std::cerr << event.dump() << "\n";
    }

    void run_review(const pm::security::ConsentRequest& cr)
    {
        auto result = security_review::run_review(cr, review_provider_);
        if (!result.ok) {
            media::llm::agent::session_persist_security_review(
                cr.request.sessionId,
                {},
                security_review::consent_request_payload(cr),
                security_review::error_review(result.error.empty()
                    ? "security reviewer failed"
                    : result.error),
                result.provider.router,
                result.provider.model);
            emit_review_event({
                {"event", "ai_consent_review_error"},
                {"error", result.error.empty() ? "security reviewer failed" : result.error},
                {"router", result.provider.router},
                {"model", result.provider.model},
                {"mock", result.mock},
            });
            return;
        }

        media::llm::agent::session_persist_security_review(
            cr.request.sessionId,
            {},
            security_review::consent_request_payload(cr),
            result.review,
            result.provider.router,
            result.provider.model);

        emit_review_event({
            {"event", "ai_consent_review"},
            {"review", result.review},
            {"router", result.provider.router},
            {"model", result.provider.model},
            {"mock", result.mock},
        });
    }
};
#endif // FEATURE_SECURITY_AGENT

} // namespace

// ── The loop engine ───────────────────────────────────────────────────────────

Result Agent::run(const Turn&             turn,
                  const ProviderSettings& provider,
                  EventCallback           on_event) const
{
    ensure_agent_process_path_has_exe_bin();
    AgentFactory::ensure_app_search_path();

    Result res;
    res.transcript                        = nlohmann::json::object();
    res.transcript["messages"]            = nlohmann::json::array();
    res.llm_usage_aggregate               = nlohmann::json::object();
    res.llm_usage_aggregate["prompt_tokens"]     = 0;
    res.llm_usage_aggregate["completion_tokens"] = 0;
    res.llm_usage_aggregate["total_tokens"]      = 0;
    res.llm_usage_aggregate["llm_rounds"]        = nlohmann::json::array();

    // ── Phase 1: let features prepare the run ─────────────────────────────────
    Turn working = turn;            // mutable working copy
    // Session/tool documents can contain legacy ANSI bytes (for example a
    // Windows-1252 dash in captured command output). Repair all prompt-bearing
    // turn state before any feature serializes it for the planner or provider.
    // Repair first — never construct nlohmann::json from the raw bytes.
    // `json j = std::string` validates UTF-8 and abort()s when nlohmann is
    // built with JSON_NOEXCEPTION (Windows-1252 em-dash 0x97 from ACP argv,
    // captured command output, etc.).
    const auto repair_string = [](std::string& value) {
        value = media::str_san::normalise_text_for_json_storage(value);
    };
    repair_string(working.user_prompt);
    repair_string(working.folder_hint);
    repair_string(working.system_extra);
    repair_string(working.workflow_id);
    repair_string(working.workflow_inject);
    repair_string(working.system_prompt_override);
    repair_string(working.planner_prompt_override);
    repair_string(working.pending_recall_inject);
    repair_string(working.task_id);
    repair_string(working.source_log_session_id);
    repair_string(working.source_log_id);
    repair_string(working.source_log_path);
    for (auto& value : working.selection) repair_string(value);
    for (auto& value : working.open_files) repair_string(value);
    detail::sanitize_json_utf8_strings(working.memory_state);
    detail::sanitize_json_utf8_strings(working.recent_events);
    detail::sanitize_json_utf8_strings(working.create_video_ui_overrides);
    detail::sanitize_json_utf8_strings(working.planner_meta);
    PreparedRun prepared;
    prepared.cb = on_event;
    prepared.host_providers =
        tool_providers::resolve(opts_.tool_providers, working.tool_provider_ids);
    for (const auto& extra : working.extra_tool_providers) {
        if (!extra || extra->id().empty())
            continue;
        bool seen = false;
        for (const auto& have : prepared.host_providers) {
            if (have && have->id() == extra->id()) {
                seen = true;
                break;
            }
        }
        if (!seen)
            prepared.host_providers.push_back(extra);
    }

    // Session bookkeeping runs after the answer exists. A null JSON field in
    // that save must not replace a finished turn with ok=false.
    const auto finish_features = [&]() {
        for (const auto& f : opts_.features) {
            if (!f)
                continue;
            try {
                f->on_finish(working, res, prepared);
            } catch (const std::exception& ex) {
                logger::error(std::string("agent on_finish ") + f->name() + ": " + ex.what());
            }
        }
    };

    // Fire before host prepare (planner HTTP, skills, MCP, …) so the UI is not
    // silent for the whole prepare window. Payload only needs selection/folder.
    if (!detail::emit(prepared.cb,
                      Event{Event::TurnStarted, working.user_prompt, "",
                            nlohmann::json{{"selection", working.selection},
                                           {"folder",    working.folder_hint}}})) {
        res.cancelled = true;
        finish_features();
        return res;
    }

    for (const auto& f : opts_.features) {
        f->on_prepare(working, provider, prepared);
    }

    // Offer request_tools on every tool-capable turn. Skills and other resource
    // metadata may identify a required schema even when the planner is disabled;
    // the grant path remains the enabled global catalog. Text-only realtime
    // (max_iterations <= 0) stays without tools.
    if (provider.max_iterations > 0)
        append_request_tools_schema(prepared.tools);

    if (working.planner_enabled)
        res.transcript["planner"] = working.planner_meta;

    // Fallback: if no feature provided a system prompt, build the default one.
    if (!prepared.system_prompt_is_null && !prepared.system_prompt_set && prepared.system_prompt.empty()) {
        prepared.system_prompt = build_system_prompt(working, prepared.tools, &prepared.prompt_meta);
        prepared.system_prompt_set = true;
    }
    if (prepared.prompt_meta.is_object() && !prepared.prompt_meta.empty())
        res.transcript["prompts"] = prepared.prompt_meta;

    const bool use_loopback    = loopback_client::is_loopback_router(provider.router);
    const bool use_local_llama = !use_loopback && llama_client::is_llama_router(provider.router);

#if defined(FEATURE_SECURITY_AGENT) && FEATURE_SECURITY_AGENT
    // ── Security gateway (Phase 2: WebView consent surface + CLI fallback) ───
    // Prefer the in-app WebViewConsentDelegate when a chat panel is alive
    // (registered via ConsentDelegateRegistry).  Fall back to
    // CliConsentDelegate for headless / CLI invocations.
    pm::security::CliConsentDelegate  sec_consent_cli(/*json_mode=*/true);
    std::unique_ptr<pm::security::ConsentDelegate> sec_consent_review;
    pm::security::ConsentDelegate*    sec_consent_active =
        pm::security::active_consent_delegate();
    if (!sec_consent_active && provider.ai_consent_review) {
        ProviderSettings review_provider = provider;
        if (review_provider.router != provider.ai_consent_router) {
            review_provider.router = provider.ai_consent_router;
            review_provider.base_url.clear();
            review_provider.api_key.clear();
        }
        review_provider.model = provider.ai_consent_model;
        media::settings_schema::apply_custom_router_base(
            review_provider.router, provider.custom_base_url, review_provider.base_url,
            /*overwrite=*/true);
        sec_consent_review = std::make_unique<CliAiReviewConsentDelegate>(
            std::move(review_provider), /*json_mode=*/true);
    }
    pm::security::ConsentDelegate*    sec_consent =
        sec_consent_active ? sec_consent_active
                           : (sec_consent_review ? sec_consent_review.get() : &sec_consent_cli);
    pm::security::SecurityGateway sec_gateway("pm-image", sec_consent);
#if defined(PM_AGENT_HAS_SANDBOX_BACKEND)
    // Register the AppContainer backend so decisions that resolve to
    // sandboxBackend="appcontainer" can be routed through the gateway's router.
    static pm::security::SandboxAppContainer sec_sandbox_appcontainer;
    sec_gateway.registerSandboxBackend(&sec_sandbox_appcontainer);
#endif
    // Seed the in-memory store with any permanent/session grants the user has
    // previously approved (via CLI or the settings UI).  Mirrors what cli.cpp
    // does via load_active_grants().  Failure is non-fatal — the agent just
    // starts with an empty grant set and will prompt when needed.
    {
        std::string sec_load_err;
        if (!pm::security::persist::load_grants(sec_gateway.permissions(), sec_load_err))
            logger::warn("[sec] grant load failed: " + sec_load_err);
        else {
            const auto loaded = sec_gateway.permissions().list();
            logger::trace(std::string("[sec] loaded ") + std::to_string(loaded.size())
                         + " grant(s) from settings before agent turn");
        }
    }
    // Helper: persist a newly-consented grant via upsert so that pre-existing
    // on-disk grants are never silently dropped. Once grants remain ephemeral;
    // session/task grants are stored because they are bound to the active context.
    auto sec_persist_grant = [](const pm::security::PolicyDecision& d) {
        if (!d.grant) {
            // grant was either Once-lifetime, store-rejected, or not applicable.
            if (d.kind == pm::security::DecisionKind::Allow)
                logger::debug("[sec] no grant after Allow (ephemeral or store-rejected)");
            return;
        }
        std::string err;
        const bool ok = pm::security::persist::upsert_grant(*d.grant, err);
        if (!ok)
            logger::warn(std::string("[sec] grant upsert failed id=") + d.grant->id + ": " + err);
        else if (!err.empty())
            logger::trace(std::string("[sec] grant upsert skipped: ") + err);
        else
            logger::info(std::string("[sec] grant upserted id=") + d.grant->id
                         + " lifetime=" + std::string(pm::security::to_string(d.grant->lifetime)));
    };
    // Session ID: use the active multi-turn/session id when present, else a
    // run-scoped id derived from the steady clock.
    const std::string sec_session_id = !working.task_id.empty()
        ? working.task_id
        : ("run-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    logger::debug(std::string("[sec] agent security sessionId=") + sec_session_id);
    // Workspace root from the folder_hint set in the turn (same source as
    // AgentPathBaseScope that was applied during on_prepare above).
    const std::filesystem::path sec_workspace =
        working.folder_hint.empty()
            ? std::filesystem::path{}
            : media::path::path_from_utf8(working.folder_hint);
#endif

    // ── Phase 2: initial message history ──────────────────────────────────────
    // The system prompt carries the agent persona, turn context, and — for
    // --multi-turn — the recalled session memory / recent events. It is sent for
    // both cloud and local routers. (Local llama used to skip it because gemma-3
    // has no native `system` role; gemma-4 restores native system-role support,
    // so dropping it would silently break multi-turn recall on the local path.)
    //
    // Optional `seed_chat_messages` (OpenAI-compat daemon) are spliced between
    // system and the current user_prompt so client-resubmitted history is wired.
    std::vector<polymech::kbot::ChatMessage> messages;
    if (!prepared.system_prompt_is_null)
        messages.push_back({"system", prepared.system_prompt, {}, nlohmann::json()});
    if (working.seed_chat_messages.is_array()) {
        for (const auto& m : working.seed_chat_messages) {
            if (!m.is_object())
                continue;
            const std::string role = m.value("role", "");
            if (role != "user" && role != "assistant")
                continue;
            std::string content;
            if (m.contains("content")) {
                if (m["content"].is_string())
                    content = m["content"].get<std::string>();
                else if (m["content"].is_array()) {
                    // OpenAI multimodal parts — concatenate text leaves only.
                    for (const auto& part : m["content"]) {
                        if (part.is_object() && part.contains("text") && part["text"].is_string())
                            content += part["text"].get<std::string>();
                        else if (part.is_string())
                            content += part.get<std::string>();
                    }
                }
            }
            if (content.empty())
                continue;
            messages.push_back({role, content, {}, nlohmann::json()});
        }
    }
    messages.push_back({"user",   working.user_prompt,     {}, nlohmann::json()});

    for (const auto& m : messages)
        res.transcript["messages"].push_back({{"role", m.role}, {"content", m.content}});

    // Cancellation: the process-global Ctrl+C flag OR this turn's own cancel
    // signal (lets a single run be stopped in isolation — e.g. a voice-delegated
    // agent — without killing other subsystems that watch the global flag).
    const auto interrupted = [&working]() -> bool {
        return media::cli::cancel_requested()
            || (working.cancel_signal && working.cancel_signal->load())
            || (working.cancel_check && working.cancel_check());
    };
    // Make the turn-local signal visible to every blocking subsystem that
    // already polls cli::cancel_requested() (run tool, external runners, MCP,
    // HTTP progress callbacks). ScopedIgnoreGlobalCancel may suppress the
    // process-global latch for realtime delegations, but never this predicate.
    media::cli::ScopedCancelPredicate scoped_turn_cancel([&working]() {
        return (working.cancel_signal && working.cancel_signal->load())
            || (working.cancel_check && working.cancel_check());
    });

    const std::string agent_run_id = !working.task_id.empty()
        ? ("run-" + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()))
        : std::string{};
    if (!working.task_id.empty()) {
        session_append_journal(working.task_id, nlohmann::json{
            {"kind",   "turn_started"},
            {"run_id", agent_run_id},
        });
    }

    // ── Phase 3: configure LLM backend ────────────────────────────────────────
    polymech::kbot::KBotOptions opts;
    opts.router         = provider.router;
    opts.base_url       = provider.base_url;
    opts.api_key        = provider.api_key;
    opts.model          = provider.model;
    opts.llm_timeout_ms = provider.timeout_ms;
    opts.cancel_check   = interrupted;
    std::unique_ptr<polymech::kbot::LLMClient> kbot_client;
    if (!use_local_llama)
        kbot_client = std::make_unique<polymech::kbot::LLMClient>(opts);

    logger::debug(std::string("agent: router=") + provider.router
                  + (use_loopback    ? " backend=loopback"
                     : use_local_llama ? " backend=llama.cpp" : " backend=kbot")
                  + " api_mode="
                  + (provider.api_mode == LlmApiMode::Responses ? "responses"
                     : provider.api_mode == LlmApiMode::Realtime ? "realtime"
                     : "completion")
                  + " streaming="
                  + (provider.streaming_mode == LlmStreamingMode::On ? "on"
                     : provider.streaming_mode == LlmStreamingMode::Off ? "off"
                     : "auto"));

    nlohmann::json tool_choice;  // null → let the model decide ("auto")

    // ── Phase 4: LLM + tool loop ──────────────────────────────────────────────
    const int max_iter = std::max(1, provider.max_iterations);
    std::string prev_response_id;

    // Security: abort the turn after this many consecutive security denials.
    // Continuing would let the model iterate on bypasses using our denial
    // messages as feedback.  Two is the maximum useful signal (first denial
    // tells the model it was blocked; second confirms the block is stable).
    constexpr int k_max_consecutive_sec_denials = 2;
    int consecutive_sec_denials = 0;
    std::unordered_set<std::string> tool_call_signatures_this_turn;
#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
    bool browser_tools_seen_this_turn = false;

    const auto abort_prompt_injection = [&](std::string reason) -> Result {
        if (reason.empty())
            reason = "Prompt injection detected in browser content.";
        logger::warn("[browser-use] abort turn: " + reason);
        res.ok = false;
        res.error = "Turn aborted: " + reason
            + " Page content is untrusted; shell/download instructions from the page were blocked.";
        res.transcript["error"] = res.error;
        res.transcript["prompt_injection_abort"] = true;
        (void)detail::emit(prepared.cb, Event{Event::Error, res.error, "", nlohmann::json()});
        finish_features();
        return res;
    };

    const auto note_browser_injection_from_envelope = [&](const nlohmann::json& env) {
        if (!media::llm::browser_use::envelope_flags_prompt_injection(env))
            return;
        working.browser_prompt_injection_abort = true;
        working.browser_prompt_injection_reason = env.value(
            "prompt_injection_reason",
            env.value("error", std::string{"prompt injection detected in browser content"}));
    };
#endif

    for (int iter = 0; iter < max_iter; ++iter) {
        if (interrupted()) {
            res.cancelled           = true;
            res.error               = "interrupted (Ctrl+C)";
            res.transcript["error"] = res.error;
            (void)detail::emit(prepared.cb, Event{Event::Error, res.error, "", nlohmann::json()});
            finish_features();
            return res;
        }
#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
        if (working.browser_prompt_injection_abort)
            return abort_prompt_injection(working.browser_prompt_injection_reason);
#endif
        ++res.iterations;

#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
        compact_stale_browser_tool_messages(messages);
#endif

        // ── Pre-API context reduction (the "pre-API filter") ──────────────────
        // Opt-in via Turn::context_reduction_mode. Builds a reduced wire copy of
        // the history; `messages` (and the transcript) stay at full fidelity, so
        // only the bytes sent to the provider this round are shrunk. No-op when
        // the mode is Off or the FEATURE_LLM_COMPRESSION build is disabled.
        const std::vector<polymech::kbot::ChatMessage>* wire = &messages;
#if defined(FEATURE_LLM_COMPRESSION) && FEATURE_LLM_COMPRESSION
        std::vector<polymech::kbot::ChatMessage> reduced_wire;
        if (working.context_reduction_mode != pm::llm::ContextReductionMode::Off) {
            std::size_t saved = 0, nred = 0;
            reduced_wire = pre_api_reduce(messages, working, saved, nred);
            if (nred > 0) {
                wire = &reduced_wire;
                logger::debug("agent: pre-api reduction shrank "
                              + std::to_string(nred) + " message(s), saved "
                              + std::to_string(saved) + " bytes (mode="
                              + std::to_string(static_cast<int>(working.context_reduction_mode))
                              + ")");
            }
        }
#endif
        const std::vector<polymech::kbot::ChatMessage>& wire_messages = *wire;

        // LLM call —————————————————————————————————————————————————————————
        const auto t_llm = std::chrono::steady_clock::now();
        polymech::kbot::LLMResponse resp;
        if (use_loopback) {
            // Deterministic offline router: replay a scripted JSON array.
            // `provider.model` carries the script source (inline JSON or @file).
            // PM_LOOPBACK_SCRIPT env var is the fallback when model is empty.
            // Never reachable in a production build unless router=="loopback"|"mock".
            resp = loopback_client::execute_chat_messages(
                wire_messages, provider.model, prepared.tools, tool_choice,
                provider.timeout_ms, [&interrupted]() { return interrupted(); });
        } else if (use_local_llama) {
            if (provider.api_mode != LlmApiMode::ChatCompletion) {
                res.error               = "llama router supports chat completion only (not responses/realtime)";
                res.transcript["error"] = res.error;
                (void)detail::emit(prepared.cb, Event{Event::Error, res.error, "", nlohmann::json()});
                finish_features();
                return res;
            }
            // Native tool calling: the model template (common_chat_*) consumes the
            // OpenAI-style catalog and returns structured tool_calls, so the shared
            // loop below dispatches tools + synthesises exactly like the kbot path.
            // Pass the interrupted predicate so Ctrl+C / per-turn cancel stops
            // the generation loop rather than blocking until n_predict tokens.
            try {
                resp = llama_client::execute_chat_messages(
                    wire_messages, provider.model, prepared.tools, tool_choice,
                    provider.timeout_ms,
                    [&interrupted]() { return interrupted(); });
            } catch (const std::exception& ex) {
                logger::error(std::string("agent llama: execute exception: ") + ex.what());
                throw;
            } catch (...) {
                logger::error("agent llama: execute unknown exception");
                throw;
            }
        } else if (provider.api_mode == LlmApiMode::Responses
                   || (provider.api_mode == LlmApiMode::Realtime && provider.max_iterations > 0)) {
            if (provider.api_mode == LlmApiMode::Realtime) {
                logger::warn("agent realtime: tool loop requested; using /responses instead of text-only realtime");
            }
            const bool stream_on = responses_streaming_enabled(provider);
            if (stream_on) {
                resp = kbot_client->execute_responses_stream(
                    wire_messages, prepared.tools, tool_choice, prev_response_id,
                    [&](const std::string& delta) -> bool {
                        if (!delta.empty())
                            return detail::emit(prepared.cb, Event{Event::TextDelta, delta, "", nlohmann::json()});
                        return true;
                    });
                // Provider/proxy stream compatibility fallback:
                // if streaming returns no text/tool/reasoning/error, run one non-stream call
                // to avoid blank assistant outputs under --streaming on.
                if (resp.success && resp.text.empty() && resp.tool_calls.empty()
                        && resp.reasoning_text.empty() && resp.error.empty()) {
                    logger::warn("agent responses streaming: empty stream payload; "
                                 "falling back to non-stream /responses for this round");
                    resp = kbot_client->execute_responses(wire_messages, prepared.tools, tool_choice, prev_response_id);
                }
            } else {
                resp = kbot_client->execute_responses(wire_messages, prepared.tools, tool_choice, prev_response_id);
            }
        } else if (provider.api_mode == LlmApiMode::Realtime) {
#if defined(FEATURE_REALTIME) && FEATURE_REALTIME
            resp = kbot_client->execute_realtime_text(wire_messages,
                [&](const std::string& delta) -> bool {
                    if (!delta.empty())
                        return detail::emit(prepared.cb,
                                            Event{Event::TextDelta, delta, "", nlohmann::json()});
                    return true;
                });
#else
            resp.success = false;
            resp.error = "FEATURE_REALTIME is disabled in this build";
#endif
        } else if (provider.streaming_mode != LlmStreamingMode::Off) {
            resp = kbot_client->execute_chat_messages(
                wire_messages, prepared.tools, tool_choice, std::nullopt,
                [&](const std::string& delta) -> bool {
                    if (!delta.empty())
                        return detail::emit(prepared.cb, Event{Event::TextDelta, delta, "", nlohmann::json()});
                    return true;
                });
            if (resp.success && resp.text.empty() && resp.tool_calls.empty()
                    && resp.reasoning_text.empty() && resp.error.empty()) {
                logger::warn("agent completion streaming: empty stream payload; "
                             "falling back to non-stream /chat/completions for this round");
                resp = kbot_client->execute_chat_messages(wire_messages, prepared.tools, tool_choice);
            }
        } else {
            resp = kbot_client->execute_chat_messages(wire_messages, prepared.tools, tool_choice);
        }
        const auto llm_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_llm).count();

        // If local llama.cpp or liboai/curl stopped by a cancel predicate, return
        // a clean cancellation rather than surfacing as a generic LLM error.
        if (!resp.success && (resp.cancelled || interrupted())) {
            res.cancelled           = true;
            res.error               = "interrupted (Ctrl+C)";
            res.transcript["error"] = res.error;
            (void)detail::emit(prepared.cb, Event{Event::Error, res.error, "", nlohmann::json()});
            finish_features();
            return res;
        }

        if (!resp.success) {
            res.error               = "LLM error: " + resp.error;
            res.transcript["error"] = res.error;
            (void)detail::emit(prepared.cb, Event{Event::Error, res.error, "", nlohmann::json()});
            finish_features();
            return res;
        }

        if (!resp.response_id.empty()) prev_response_id = resp.response_id;

        detail::merge_llm_usage_from_provider_meta(
            res.llm_usage_aggregate, resp.provider_meta_json, provider.router);

        // Stamp duration onto the last llm_rounds[] entry.
        if (res.llm_usage_aggregate["llm_rounds"].is_array()
                && !res.llm_usage_aggregate["llm_rounds"].empty())
            res.llm_usage_aggregate["llm_rounds"].back()["duration_ms"] = llm_ms;

        // Emit per-round telemetry —————————————————————————————————————————
        {
            nlohmann::json rp;
            rp["round"]       = iter + 1;
            rp["duration_ms"] = llm_ms;
            rp["stateful"]    = (provider.api_mode == LlmApiMode::Responses
                                 && iter > 0 && !resp.response_id.empty());
            if (!resp.response_id.empty()) rp["response_id"] = resp.response_id;
            if (!resp.provider_meta_json.empty()) {
                try {
                    auto meta = nlohmann::json::parse(resp.provider_meta_json);
                    rp["provider_meta"] = meta;
                    if (const nlohmann::json* usage = detail::provider_meta_usage_object(meta)) {
                        const auto& u = *usage;
                        nlohmann::json tok;
                        if (u.contains("input_tokens"))               tok["input"]     = u["input_tokens"];
                        else if (u.contains("prompt_tokens"))         tok["input"]     = u["prompt_tokens"];
                        if (u.contains("output_tokens"))              tok["output"]    = u["output_tokens"];
                        else if (u.contains("completion_tokens"))     tok["output"]    = u["completion_tokens"];
                        if (u.contains("output_tokens_details")
                                && u["output_tokens_details"].contains("reasoning_tokens"))
                            tok["reasoning"] = u["output_tokens_details"]["reasoning_tokens"];
                        if (!tok.empty()) rp["tokens"] = std::move(tok);
                        if (u.contains("tokens_per_second") && u["tokens_per_second"].is_number())
                            rp["tokens_per_second"] = u["tokens_per_second"];
                        rp["usage"] = u;
                    }
                    if (meta.contains("model") && meta["model"].is_string())
                        rp["model"] = meta["model"];
                    else if (meta.contains("model_map_information") && meta["model_map_information"].is_object()) {
                        const auto& mi = meta["model_map_information"];
                        if (mi.contains("model_map_key") && mi["model_map_key"].is_string())
                            rp["model"] = mi["model_map_key"];
                    }
                } catch (...) {}
            }
            (void)detail::emit(prepared.cb, Event{Event::LlmRound, "", "", std::move(rp)});
        }

        if (!resp.reasoning_text.empty()) {
            logger::trace("agent responses: emit Thinking chars="
                            + std::to_string(resp.reasoning_text.size()));
            (void)detail::emit(prepared.cb,
                               Event{Event::Thinking, resp.reasoning_text, "", nlohmann::json()});
        } else {
            logger::trace("agent responses: no reasoning_text this LLM round");
        }

        if (!resp.finish_reason.empty())
            res.llm_finish_reason = resp.finish_reason;

        // Final text — no more tool calls ——————————————————————————————————
        if (resp.tool_calls.empty()) {
            res.final_text = resp.text;
            logger::trace("agent responses: final_text chars=" + std::to_string(resp.text.size())
                          + " finish_reason=" + (resp.finish_reason.empty() ? "?" : resp.finish_reason));
            messages.push_back({"assistant", resp.text, {}, nlohmann::json()});
            res.transcript["messages"].push_back({{"role", "assistant"}, {"content", resp.text}});
            if (!resp.reasoning_text.empty())
                res.transcript["messages"].back()["reasoning"] = resp.reasoning_text;
            (void)detail::emit(prepared.cb,
                               Event{Event::AssistantText, resp.text, "", nlohmann::json()});
            res.ok = true;
            if (!working.task_id.empty()) {
                session_append_journal(working.task_id, nlohmann::json{
                    {"kind",   "turn_completed"},
                    {"run_id", agent_run_id},
                    {"ok",     true},
                });
            }
            finish_features();
            (void)detail::emit(prepared.cb,
                               Event{Event::Done,          resp.text, "", nlohmann::json()});
            return res;
        }

        // Serialise the assistant's tool-call request ——————————————————————
        // OpenAI spec: function.arguments MUST be a JSON string on the wire.
        nlohmann::json tc_array = nlohmann::json::array();
        for (const auto& tc : resp.tool_calls) {
            nlohmann::json entry{
                {"id",   tc.id},
                {"type", "function"},
                {"function", {{"name",      tc.name},
                              {"arguments", tc.arguments.is_string()
                                                ? tc.arguments.get<std::string>()
                                                : tc.arguments.dump()}}},
            };
            if (!tc.item_id.empty()) entry["item_id"] = tc.item_id;
            tc_array.push_back(std::move(entry));
        }
        polymech::kbot::ChatMessage asst_msg{"assistant", "", "", tc_array};
        if (!resp.responses_output_items.empty())
            asst_msg.responses_output_items = resp.responses_output_items;
        messages.push_back(std::move(asst_msg));
        res.transcript["messages"].push_back({{"role",       "assistant"},
                                              {"content",    nullptr},
                                              {"tool_calls", tc_array}});

        // Dispatch tool calls ——————————————————————————————————————————————
        const std::string interrupt_msg = "interrupted (Ctrl+C)";
        std::unordered_set<std::string> create_command_labels_this_turn;
        std::unordered_set<std::string> request_tools_names_this_turn;
        const bool allow_create_command =
            detail::user_asked_for_ribbon_command(working.user_prompt);
#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
        std::size_t browser_tools_dispatched = 0;
#endif

        if (working.parallel_tool_dispatch && resp.tool_calls.size() > 1
                && !response_has_request_tools(resp)
#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
                && !response_has_browser_use_tools(resp)
#endif
                && !response_has_network_tools(resp)
                && response_tools_available(resp, prepared.tools)) {
            // ── P6: Parallel tool dispatch ─────────────────────────────────────
            // All N tool calls fire concurrently via std::async(launch::async).
            // Results are collected and emitted in original model order.
            // Per-file progress streaming (image_transform / run chunk hooks) is
            // omitted — those hooks are single-threaded state; final envelopes
            // are still complete. Falls back to serial when only 1 call present.
            // Assumes path::execute and mcp_bridge::call_tool are re-entrant.
            if (interrupted()) {
                for (const auto& tcr : resp.tool_calls) {
                    nlohmann::json err_env{{"ok", false}, {"error", interrupt_msg}};
                    (void)detail::emit(prepared.cb,
                                       Event{Event::ToolResult, "", tcr.name,
                                             nlohmann::json{{"id", tcr.id}, {"envelope", err_env}}});
                    messages.push_back({"tool", err_env.dump(), tcr.id, nlohmann::json()});
                    res.transcript["messages"].push_back(
                        {{"role", "tool"}, {"tool_call_id", tcr.id}, {"content", err_env.dump()}});
                }
                res.cancelled           = true;
                res.error               = interrupt_msg;
                res.transcript["error"] = res.error;
                (void)detail::emit(prepared.cb,
                                   Event{Event::Error, res.error, "", nlohmann::json()});
                finish_features();
                return res;
            }

            // Capture per-call inputs by value (thread safety: no shared mutation).
            struct ParCallInput {
                std::size_t    index     = 0;
                std::string    id;
                std::string    name;
                nlohmann::json args;
                bool           args_valid = false;
            };
            std::vector<ParCallInput> par_inputs;
            par_inputs.reserve(resp.tool_calls.size());
            for (std::size_t i = 0; i < resp.tool_calls.size(); ++i) {
                const auto& tc = resp.tool_calls[i];
                ParCallInput in;
                in.index      = i;
                in.id         = tc.id;
                in.name       = tc.name;
                in.args_valid = tc.arguments.is_object();
                in.args       = tc.arguments;
                if (in.args_valid && tc.name == "create_video")
                    apply_create_video_ui_overrides(in.args, working.create_video_ui_overrides);
                if (in.args_valid)
                    apply_image_tool_context_refs(in.args, tc.name, working.selection);
                par_inputs.push_back(std::move(in));
            }

            // Emit ToolCall events upfront in model order.
            for (const auto& in : par_inputs) {
                if (!in.args_valid) continue;
                (void)detail::emit(prepared.cb,
                                   Event{Event::ToolCall, "", in.name,
                                         nlohmann::json{{"id", in.id}, {"arguments", in.args}}});
            }

            // Launch async tasks; invalid-arg calls get a pre-wrapped error future.
            struct ParCallResult {
                std::size_t    index      = 0;
                std::string    id;
                std::string    name;
                std::string    envelope_str;
                nlohmann::json tool_result_envelope;
                long long      tool_ms    = 0;
                bool           args_valid = false;
            };

#if defined(FEATURE_SECURITY_AGENT) && FEATURE_SECURITY_AGENT
            // Pre-authorize all parallel calls in the main thread — consent prompts
            // must never race across threads (all consent I/O is serial UI-thread).
            std::vector<pm::security::PolicyDecision> par_sec_decisions;
            par_sec_decisions.reserve(par_inputs.size());
            for (const auto& in : par_inputs) {
                if (!in.args_valid) {
                    par_sec_decisions.push_back({});  // placeholder; skipped in launch loop
                    continue;
                }
                const bool is_mcp_call = prepared.mcp_bridge
                    && prepared.mcp_bridge->is_mcp_openai_function(in.name);
                pm::security::ToolRequest sec_req = build_agent_tool_request(
                    in.name, in.args, is_mcp_call, sec_session_id,
                    working.task_id, sec_workspace);
                auto par_d = sec_gateway.authorizeWithConsent(sec_req);
                session_persist_security_decision(sec_session_id, {}, sec_req, par_d);
                sec_persist_grant(par_d);
                par_sec_decisions.push_back(std::move(par_d));
            }
#endif

            std::vector<std::future<ParCallResult>> par_futures;
            par_futures.reserve(par_inputs.size());
            for (std::size_t i = 0; i < par_inputs.size(); ++i) {
                if (!par_inputs[i].args_valid) {
                    ParCallResult r;
                    r.index      = par_inputs[i].index;
                    r.id         = par_inputs[i].id;
                    r.name       = par_inputs[i].name;
                    r.args_valid = false;
                    r.tool_result_envelope = nlohmann::json{
                        {"ok", false},
                        {"error", "Tool call for '" + r.name + "' had no arguments "
                                  "(output was likely truncated by max_tokens). "
                                  "Retry with a shorter response or split into smaller steps."}};
                    r.envelope_str = detail::safe_json_dump(r.tool_result_envelope);
                    par_futures.push_back(std::async(std::launch::deferred,
                        [r2 = std::move(r)]() mutable -> ParCallResult { return r2; }));
                    continue;
                }
                if (!tool_call_signatures_this_turn.insert(
                        tool_call_signature(par_inputs[i].name, par_inputs[i].args)).second) {
                    ParCallResult r;
                    r.index      = par_inputs[i].index;
                    r.id         = par_inputs[i].id;
                    r.name       = par_inputs[i].name;
                    r.args_valid = true;
                    r.tool_result_envelope = duplicate_tool_call_envelope(par_inputs[i].name);
                    r.envelope_str = detail::safe_json_dump(r.tool_result_envelope);
                    par_futures.push_back(std::async(std::launch::deferred,
                        [r2 = std::move(r)]() mutable -> ParCallResult { return r2; }));
                    continue;
                }
                if (canonical_tool_name(par_inputs[i].name) == "create_command") {
                    std::string label;
                    if (par_inputs[i].args.contains("label") && par_inputs[i].args["label"].is_string())
                        label = par_inputs[i].args["label"].get<std::string>();
                    if (label.empty())
                        label = par_inputs[i].args.dump();
                    const bool duplicate_label =
                        !create_command_labels_this_turn.insert(label).second;
                    if (!allow_create_command || duplicate_label) {
                        ParCallResult r;
                        r.index      = par_inputs[i].index;
                        r.id         = par_inputs[i].id;
                        r.name       = par_inputs[i].name;
                        r.args_valid = true;
                        r.tool_result_envelope = nlohmann::json{
                            {"ok", false},
                            {"skipped", true},
                            {"error", allow_create_command
                                 ? "create_command already ran this turn with the same label. "
                                   "Do not create another button."
                                 : "create_command is only for a ribbon or context-menu button "
                                   "when the user asked for one. Call the granted mcp_* / path tool instead."},
                        };
                        r.envelope_str = detail::safe_json_dump(r.tool_result_envelope);
                        par_futures.push_back(std::async(std::launch::deferred,
                            [r2 = std::move(r)]() mutable -> ParCallResult { return r2; }));
                        continue;
                    }
                }
#if defined(FEATURE_SECURITY_AGENT) && FEATURE_SECURITY_AGENT
                // Security check (already resolved in main thread above).
                // Placeholder decisions (args_valid=false or run tool) have kind=Allow
                // by default-construction, so they fall through to normal dispatch.
                if (i < par_sec_decisions.size() && par_inputs[i].name != "run"
                        && par_sec_decisions[i].kind != pm::security::DecisionKind::Allow) {
                    logger::warn("[sec] tool denied (parallel) tool=" + par_inputs[i].name
                                 + " reason=" + par_sec_decisions[i].reason
                                 + " decision=" + std::string(pm::security::to_string(par_sec_decisions[i].kind)));
                    const bool cached_write_denial =
                        (par_inputs[i].name == "write_file"
                         || par_inputs[i].name == "file_compose"
                         || par_inputs[i].name == "file_str_replace"
                         || par_inputs[i].name == "file_delete")
                        && par_sec_decisions[i].reason == "covered by prior user denial";
                    if (!cached_write_denial)
                        ++consecutive_sec_denials;
                    ParCallResult r;
                    r.index             = par_inputs[i].index;
                    r.id                = par_inputs[i].id;
                    r.name              = par_inputs[i].name;
                    r.args_valid        = true;
                    r.tool_result_envelope = security_deny_envelope(r.name, par_sec_decisions[i]);
                    r.envelope_str      = detail::safe_json_dump(r.tool_result_envelope);
                    r.tool_ms           = 0;
                    par_futures.push_back(std::async(std::launch::deferred,
                        [r2 = std::move(r)]() mutable -> ParCallResult { return r2; }));
                    continue;
                }
#endif
                par_futures.push_back(std::async(std::launch::async,
                    [&par_inputs, i, &prepared,
                     task_id = working.task_id,
                     folder_hint = working.folder_hint,
                     cancel_signal = working.cancel_signal,
                     cancel_check = working.cancel_check]() -> ParCallResult {
                        // Path base is thread_local. Without this, a parallel
                        // file_glob resolves `**/*` against the process cwd.
                        struct WorkerPathBase {
                            bool armed = false;
                            explicit WorkerPathBase(const std::string& hint) {
                                if (hint.empty())
                                    return;
                                media::llm::path::set_agent_path_base(hint);
                                armed = true;
                            }
                            ~WorkerPathBase() {
                                if (armed)
                                    media::llm::path::clear_agent_path_base();
                            }
                        } path_base(folder_hint);
                        ToolTaskContextGuard task_context(task_id);
                        std::unique_ptr<media::cli::ScopedIgnoreGlobalCancel>
                            ignore_unrelated_global;
                        if (cancel_signal)
                            ignore_unrelated_global =
                                std::make_unique<media::cli::ScopedIgnoreGlobalCancel>();
                        media::cli::ScopedCancelPredicate scoped_parallel_cancel(
                            [cancel_signal, cancel_check]() {
                                return (cancel_signal && cancel_signal->load())
                                    || (cancel_check && cancel_check());
                            });
                        const ParCallInput& in = par_inputs[i];
                        ParCallResult r;
                        r.index      = in.index;
                        r.id         = in.id;
                        r.name       = in.name;
                        r.args_valid = true;
                        const auto t = std::chrono::steady_clock::now();
                        if (dispatch_host_tool(prepared, task_id, folder_hint, in.name, in.args,
                                               r.tool_result_envelope, r.envelope_str)) {
                        } else if (prepared.mcp_bridge
                                && prepared.mcp_bridge->is_mcp_openai_function(in.name)) {
                            std::string mcp_err;
                            if (!prepared.mcp_bridge->call_tool(
                                    in.name, in.args, r.envelope_str, mcp_err)) {
                                r.tool_result_envelope = {
                                    {"ok",    false},
                                    {"error", mcp_err.empty() ? "MCP tool dispatch failed"
                                                              : mcp_err}};
                                r.envelope_str = detail::safe_json_dump(r.tool_result_envelope);
                            } else {
                                try {
                                    r.tool_result_envelope =
                                        nlohmann::json::parse(r.envelope_str);
                                } catch (...) {
                                    r.tool_result_envelope = nlohmann::json::object();
                                }
                            }
                        } else {
                            path::set_change_tool_call_id(in.id);
                            path::ExecuteResult er = path::execute(in.name, in.args);
                            r.envelope_str         = path::envelope_text(er);
                            r.tool_result_envelope = std::move(er.envelope);
                        }
                        r.tool_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t).count();
                        return r;
                    }));
            }

            // Collect in original order: emit ToolResult, journal, append messages.
            for (auto& fut : par_futures) {
                for (;;) {
                    const auto status = fut.wait_for(std::chrono::milliseconds(100));
                    if (status == std::future_status::ready
                            || status == std::future_status::deferred)
                        break;
                    // The async worker has the same per-turn predicate installed;
                    // polling here avoids an opaque fut.get() stall while it
                    // unwinds without aborting unrelated concurrent runs.
                    (void)interrupted();
                }
                const auto r = fut.get();
                (void)detail::emit(prepared.cb,
                                   Event{Event::ToolResult, "", r.name,
                                         nlohmann::json{{"id",          r.id},
                                                        {"envelope",    r.tool_result_envelope},
                                                        {"duration_ms", r.tool_ms}}});
                if (!working.task_id.empty()) {
                    nlohmann::json je = nlohmann::json::object();
                    je["tool"]         = r.name;
                    je["tool_call_id"] = r.id;
                    je["duration_ms"]  = r.tool_ms;
                    bool ok_value = false;
                    try { ok_value = r.tool_result_envelope.value("ok", false); } catch (...) {}
                    je["ok"] = ok_value;
                    try {
                        if (r.tool_result_envelope.contains("results")
                                && r.tool_result_envelope["results"].is_array()) {
                            nlohmann::json paths = nlohmann::json::array();
                            for (const auto& rr : r.tool_result_envelope["results"]) {
                                if (!rr.is_object()) continue;
                                if (rr.contains("output_path")
                                        && rr["output_path"].is_string())
                                    paths.push_back(rr["output_path"]);
                                else if (rr.contains("path") && rr["path"].is_string())
                                    paths.push_back(rr["path"]);
                            }
                            if (!paths.empty()) je["output_paths"] = std::move(paths);
                        }
                        if (!ok_value && r.tool_result_envelope.contains("error")
                                && r.tool_result_envelope["error"].is_string())
                            je["error"] = r.tool_result_envelope["error"];
                    } catch (...) {}
                    session_append_journal(working.task_id, je);
                }
                messages.push_back({"tool", r.envelope_str, r.id, nlohmann::json()});
                res.transcript["messages"].push_back(
                    {{"role",         "tool"},
                     {"tool_call_id", r.id},
                     {"content",      r.tool_result_envelope}});
            }

            // Abort if the parallel batch pushed us over the deny threshold.
            if (consecutive_sec_denials >= k_max_consecutive_sec_denials) {
                logger::warn("[sec] abort turn: " + std::to_string(consecutive_sec_denials)
                             + " consecutive security denials (parallel batch) — stopping agent loop");
                res.ok    = false;
                res.error = "Turn aborted: repeated security policy violations. "
                            "Review security settings if this was unintentional.";
                res.transcript["error"] = res.error;
                (void)detail::emit(prepared.cb,
                                   Event{Event::Error, res.error, "", nlohmann::json()});
                finish_features();
                return res;
            }

        } else {
        // ── Serial dispatch ────────────────────────────────────────────────
        for (std::size_t tci = 0; tci < resp.tool_calls.size(); ++tci) {
            if (interrupted()) {
                for (std::size_t tj = tci; tj < resp.tool_calls.size(); ++tj) {
                    const auto& tcr = resp.tool_calls[tj];
                    nlohmann::json err_env{{"ok", false}, {"error", interrupt_msg}};
                    (void)detail::emit(prepared.cb,
                                       Event{Event::ToolResult, "", tcr.name,
                                             nlohmann::json{{"id", tcr.id}, {"envelope", err_env}}});
                    messages.push_back({"tool", err_env.dump(), tcr.id, nlohmann::json()});
                    res.transcript["messages"].push_back(
                        {{"role", "tool"}, {"tool_call_id", tcr.id}, {"content", err_env.dump()}});
                }
                res.cancelled           = true;
                res.error               = interrupt_msg;
                res.transcript["error"] = res.error;
                (void)detail::emit(prepared.cb,
                                   Event{Event::Error, res.error, "", nlohmann::json()});
                finish_features();
                return res;
            }

            const auto& tc = resp.tool_calls[tci];

            // Guard: if the model's arguments are null/non-object (e.g. output
            // truncated by max_tokens), skip the call with a clear error so the
            // model knows it must retry with a shorter response.
            if (!tc.arguments.is_object()) {
                nlohmann::json err_env{
                    {"ok", false},
                    {"error", "Tool call for '" + tc.name + "' had no arguments "
                              "(output was likely truncated by max_tokens). "
                              "Retry with a shorter response or split into smaller steps."}};
                (void)detail::emit(prepared.cb,
                                   Event{Event::ToolResult, "", tc.name,
                                         nlohmann::json{{"id", tc.id}, {"envelope", err_env}}});
                messages.push_back({"tool", err_env.dump(), tc.id, nlohmann::json()});
                res.transcript["messages"].push_back(
                    {{"role", "tool"}, {"tool_call_id", tc.id}, {"content", err_env.dump()}});
                continue;
            }

            // Merge UI-level overrides into args before execution.
            nlohmann::json args = tc.arguments;
            if (tc.name == "create_video")
                apply_create_video_ui_overrides(args, working.create_video_ui_overrides);
            apply_image_tool_context_refs(args, tc.name, working.selection);

            std::string    envelope_str;
            nlohmann::json tool_result_envelope;

            if (!has_openai_tool(prepared.tools, tc.name)) {
                tool_result_envelope = nlohmann::json{
                    {"ok", false},
                    {"error", "Tool '" + tc.name + "' is not available in the current tool set. "
                              "Call request_tools first with the tool name (path tools, or mcp_* names from "
                              "info_lookup kind=mcp), then retry."},
                    {"missing_tool", tc.name},
                    {"hint", k_request_tools_name},
                };
                envelope_str = detail::safe_json_dump(tool_result_envelope);
                (void)detail::emit(prepared.cb,
                                   Event{Event::ToolResult, "", tc.name,
                                         nlohmann::json{{"id",       tc.id},
                                                        {"envelope", tool_result_envelope}}});
                messages.push_back({"tool", envelope_str, tc.id, nlohmann::json()});
                res.transcript["messages"].push_back({{"role",         "tool"},
                                                      {"tool_call_id", tc.id},
                                                      {"content",      tool_result_envelope}});
                continue;
            }

#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
            if (media::llm::browser_use::is_browser_use_tool(tc.name)) {
                if (browser_tools_dispatched >= 1) {
                    nlohmann::json err_env{
                        {"ok", false},
                        {"error",
                         "At most one browser-use tool per LLM round (many rounds are allowed). "
                         "Use browser_batch for same-epoch click/wait/read sequences."},
                        {"skipped", true},
                    };
                    envelope_str = detail::safe_json_dump(err_env);
                    (void)detail::emit(prepared.cb,
                                       Event{Event::ToolResult, "", tc.name,
                                             nlohmann::json{{"id", tc.id}, {"envelope", err_env}}});
                    messages.push_back({"tool", envelope_str, tc.id, nlohmann::json()});
                    res.transcript["messages"].push_back({{"role", "tool"},
                                                          {"tool_call_id", tc.id},
                                                          {"content", err_env}});
                    continue;
                }
                ++browser_tools_dispatched;
            }
#endif

            (void)detail::emit(prepared.cb,
                               Event{Event::ToolCall, "", tc.name,
                                     nlohmann::json{{"id", tc.id}, {"arguments", args}}});

#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
            // After hostile page content, block shell follow-through. Ordinary
            // `run`/download tasks are unaffected unless the command itself
            // matches exfil / pipe-to-shell / download-and-run shapes.
            if ((tc.name == "run" || tc.name == "run_sequence")
                && (browser_tools_seen_this_turn
                    || working.browser_prompt_injection_abort)) {
                std::string cmd;
                try {
                    if (args.contains("command") && args["command"].is_string())
                        cmd = args["command"].get<std::string>();
                    else if (args.contains("commands") && args["commands"].is_array()) {
                        for (const auto& c : args["commands"]) {
                            if (c.is_string()) {
                                if (!cmd.empty()) cmd.push_back('\n');
                                cmd += c.get<std::string>();
                            } else if (c.is_object() && c.contains("command")
                                       && c["command"].is_string()) {
                                if (!cmd.empty()) cmd.push_back('\n');
                                cmd += c["command"].get<std::string>();
                            }
                        }
                    }
                } catch (...) {
                }
                if (auto hit = media::llm::browser_use::detect_injection_followthrough_command(cmd)) {
                    working.browser_prompt_injection_abort = true;
                    working.browser_prompt_injection_reason = hit->reason;
                    nlohmann::json err_env{
                        {"ok", false},
                        {"error",
                         "Blocked: shell command looks like prompt-injection follow-through ("
                             + hit->kind + "). " + hit->reason},
                        {"prompt_injection_detected", true},
                        {"prompt_injection_kind", hit->kind},
                        {"prompt_injection_reason", hit->reason},
                        {"security_denied", true},
                    };
                    envelope_str = detail::safe_json_dump(err_env);
                    (void)detail::emit(prepared.cb,
                                       Event{Event::ToolResult, "", tc.name,
                                             nlohmann::json{{"id", tc.id}, {"envelope", err_env}}});
                    messages.push_back({"tool", envelope_str, tc.id, nlohmann::json()});
                    res.transcript["messages"].push_back({{"role", "tool"},
                                                          {"tool_call_id", tc.id},
                                                          {"content", err_env}});
                    return abort_prompt_injection(hit->reason);
                }
            }
#endif

            {
                const std::string canon = canonical_tool_name(tc.name);
                if (canon == "modbus_write" || canon == "ssh_exec" || canon == "mqtt")
                    forget_network_tool_signatures(tool_call_signatures_this_turn);
            }
            if (!tool_call_signatures_this_turn.insert(
                    tool_call_signature(tc.name, args)).second) {
                tool_result_envelope = duplicate_tool_call_envelope(tc.name);
                envelope_str = detail::safe_json_dump(tool_result_envelope);
                (void)detail::emit(prepared.cb,
                                   Event{Event::ToolResult, "", tc.name,
                                         nlohmann::json{{"id",          tc.id},
                                                        {"envelope",    tool_result_envelope},
                                                        {"duration_ms", 0}}});
                messages.push_back({"tool", envelope_str, tc.id, nlohmann::json()});
                res.transcript["messages"].push_back({{"role",         "tool"},
                                                      {"tool_call_id", tc.id},
                                                      {"content",      tool_result_envelope}});
                continue;
            }

            if (canonical_tool_name(tc.name) == "create_command") {
                std::string label;
                if (args.contains("label") && args["label"].is_string())
                    label = args["label"].get<std::string>();
                if (label.empty())
                    label = args.dump();
                const bool duplicate_label =
                    !create_command_labels_this_turn.insert(label).second;
                if (!allow_create_command || duplicate_label) {
                    nlohmann::json err_env{
                        {"ok", false},
                        {"skipped", true},
                        {"error", allow_create_command
                             ? "create_command already ran this turn with the same label. "
                               "Do not create another button. If an mcp_* tool was granted, call it now."
                             : "create_command is only for a ribbon or context-menu button "
                               "when the user asked for one. Call the granted mcp_* / path tool instead."},
                    };
                    envelope_str = detail::safe_json_dump(err_env);
                    (void)detail::emit(prepared.cb,
                                       Event{Event::ToolResult, "", tc.name,
                                             nlohmann::json{{"id", tc.id}, {"envelope", err_env}}});
                    messages.push_back({"tool", envelope_str, tc.id, nlohmann::json()});
                    res.transcript["messages"].push_back({{"role", "tool"},
                                                          {"tool_call_id", tc.id},
                                                          {"content", err_env}});
                    continue;
                }
            }

            if (canonical_tool_name(tc.name) == k_request_tools_name) {
                const std::vector<std::string> names = request_tool_names(args);
                bool all_repeat = !names.empty();
                for (const auto& n : names) {
                    if (request_tools_names_this_turn.insert(n).second)
                        all_repeat = false;
                }
                if (all_repeat) {
                    tool_result_envelope = nlohmann::json{
                        {"ok", true},
                        {"skipped", true},
                        {"already_requested", names},
                        {"next_step",
                         "Those tools were already requested. CALL the granted tool now. "
                         "Do not request_tools or create_command again."},
                        {"message", "Already requested. CALL the granted tool now."},
                    };
                } else {
                    tool_result_envelope = handle_request_tools(working, prepared, args);
                }
                envelope_str = detail::safe_json_dump(tool_result_envelope);
                (void)detail::emit(prepared.cb,
                                   Event{Event::ToolResult, "", tc.name,
                                         nlohmann::json{{"id",          tc.id},
                                                        {"envelope",    tool_result_envelope},
                                                        {"duration_ms", 0}}});
                messages.push_back({"tool", envelope_str, tc.id, nlohmann::json()});
                res.transcript["messages"].push_back({{"role",         "tool"},
                                                      {"tool_call_id", tc.id},
                                                      {"content",      tool_result_envelope}});
                continue;
            }

            // Wire per-file progress for image_transform.
            const bool image_transform_progress =
                (tc.name == "image_transform" || tc.name == "transform") && prepared.cb;
            struct ClearTransformHook {
                bool armed = false;
                ~ClearTransformHook() {
                    if (armed) path::clear_image_transform_file_progress_hook();
                }
            } clear_hook;
            if (image_transform_progress) {
                clear_hook.armed = true;
                path::set_image_transform_file_progress_hook(
                    [&](const nlohmann::json& one_file_env) {
                        (void)detail::emit(prepared.cb,
                                           Event{Event::ToolFileProgress, "", tc.name,
                                                 nlohmann::json{{"id",       tc.id},
                                                                {"envelope", one_file_env}}});
                    });
            }

            // Wire streaming output hook for run and download_url (UI "start" is
            // deferred until after security consent — emitting start before
            // authorizeWithConsent showed a live bubble with nothing behind it,
            // so Stop could not clear the spinner while consent was unanswered).
            const bool run_streaming = (tc.name == "run" || tc.name == "download_url") && prepared.cb;
            struct ClearRunHook {
                bool armed = false;
                bool download = false;
                ~ClearRunHook() {
                    if (!armed) return;
                    if (download) path::clear_download_url_progress_hook();
                    else run::clear_output_chunk_hook();
                }
            } clear_run_hook;
            bool run_ui_live = false;

            const auto begin_run_ui = [&]() {
                if (!run_streaming || run_ui_live) return;
                run_ui_live = true;
                clear_run_hook.armed = true;
                clear_run_hook.download = (tc.name == "download_url");
                std::string cmd_text;
                try {
                    cmd_text = clear_run_hook.download
                        ? args.value("url", std::string{})
                        : args.value("command", std::string{});
                } catch (...) {}
                cmd_text = media::str_san::normalise_text_for_json_storage(cmd_text);
                (void)detail::emit(prepared.cb,
                                   Event{Event::ToolFileProgress, "", tc.name,
                                         nlohmann::json{{"id",      tc.id},
                                                        {"event",   "start"},
                                                        {"command", cmd_text}}});
                if (clear_run_hook.download) {
                    path::set_download_url_progress_hook(
                        [&](std::uint64_t received, std::uint64_t total) {
                            auto fmt = [](std::uint64_t n) {
                                char buf[32];
                                if (n >= (1ull << 30))
                                    std::snprintf(buf, sizeof(buf), "%.1f GB", n / 1073741824.0);
                                else if (n >= (1ull << 20))
                                    std::snprintf(buf, sizeof(buf), "%.1f MB", n / 1048576.0);
                                else if (n >= (1ull << 10))
                                    std::snprintf(buf, sizeof(buf), "%.0f KB", n / 1024.0);
                                else
                                    std::snprintf(buf, sizeof(buf), "%llu B",
                                                  static_cast<unsigned long long>(n));
                                return std::string(buf);
                            };
                            std::string line = fmt(received);
                            if (total > 0)
                                line += " / " + fmt(total);
                            line += "\n";
                            (void)detail::emit(prepared.cb,
                                               Event{Event::ToolFileProgress, "", tc.name,
                                                     nlohmann::json{{"id",     tc.id},
                                                                    {"stream", "stdout"},
                                                                    {"chunk",  line}}});
                        });
                    return;
                }
                run::set_output_chunk_hook(
                    [&](const std::string& stream, const std::string& chunk) {
                        // Repair before json construction. Captured console
                        // bytes are often not UTF-8, and this hook runs on the
                        // process reader thread where a strict dump is fatal.
                        const std::string text =
                            media::str_san::normalise_text_for_json_storage(chunk);
                        (void)detail::emit(prepared.cb,
                                           Event{Event::ToolFileProgress, "", tc.name,
                                                 nlohmann::json{{"id",     tc.id},
                                                                {"stream", stream},
                                                                {"chunk",  text}}});
                    });
            };

            const auto finish_run_ui = [&](int exit_code, int64_t duration_ms, bool ok) {
                if (!run_ui_live || !prepared.cb) return;
                (void)detail::emit(prepared.cb,
                                   Event{Event::ToolFileProgress, "", tc.name,
                                         nlohmann::json{{"id",          tc.id},
                                                        {"event",       "done"},
                                                        {"exit_code",   exit_code},
                                                        {"duration_ms", duration_ms},
                                                        {"ok",          ok}}});
            };

            const auto     t_tool = std::chrono::steady_clock::now();

#if defined(FEATURE_SECURITY_AGENT) && FEATURE_SECURITY_AGENT
            // ── Security gateway intercept ─────────────────────────────────────
            // Authorize before any tool dispatch (builtin or MCP). The gateway
            // checks policy, session/task grants, and — on Ask — prompts through
            // the ConsentDelegate (CLI fallback or in-app WebView surface).
            // All tools including `run` are intercepted. Execute is always Ask
            // in Developer profile; consent fatigue is handled via grants
            // (e.g. a cmd://** Execute grant covers repeat run calls).
            // validate_command in RunTool.cpp provides a hard-deny second layer
            // after gateway approval; sandbox provides containment when enabled.
                {
                    const bool is_mcp_call = prepared.mcp_bridge
                        && prepared.mcp_bridge->is_mcp_openai_function(tc.name);
                    pm::security::ToolRequest sec_req = build_agent_tool_request(
                        tc.name, args, is_mcp_call, sec_session_id,
                        working.task_id, sec_workspace);
                    pm::security::PolicyDecision sec_decision =
                        sec_gateway.authorizeWithConsent(sec_req);
                    session_persist_security_decision(sec_session_id, {}, sec_req, sec_decision);
                    sec_persist_grant(sec_decision);
                if (sec_decision.kind == pm::security::DecisionKind::Allow
                        && !sec_decision.sandboxBackend.empty()) {
                    // Phase B routing: the gateway decided this permitted call
                    // must run inside a sandbox. Hand the backend name to the
                    // tool dispatch (RunTool honours args["sandbox"]). This is
                    // authoritative — it overrides any caller-supplied flag.
                    args["sandbox"] = sec_decision.sandboxBackend;
                }
                if (sec_decision.kind != pm::security::DecisionKind::Allow) {
                    // Log the real reason internally; the model-visible envelope
                    // is intentionally generic (avoids bypass-roadmap feedback).
                    logger::warn("[sec] tool denied tool=" + tc.name
                                 + " reason=" + sec_decision.reason
                                 + " decision=" + std::string(pm::security::to_string(sec_decision.kind)));
                    const bool cached_write_denial =
                        (tc.name == "write_file"
                         || tc.name == "file_compose"
                         || tc.name == "file_str_replace"
                         || tc.name == "file_delete")
                        && sec_decision.reason == "covered by prior user denial";
                    if (!cached_write_denial)
                        ++consecutive_sec_denials;

                    tool_result_envelope = security_deny_envelope(tc.name, sec_decision);
                    envelope_str = detail::safe_json_dump(tool_result_envelope);
                    const auto deny_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t_tool).count();
                    (void)detail::emit(prepared.cb,
                                       Event{Event::ToolResult, "", tc.name,
                                             nlohmann::json{{"id",          tc.id},
                                                            {"envelope",    tool_result_envelope},
                                                            {"duration_ms", deny_ms}}});
                    if (!working.task_id.empty()) {
                        nlohmann::json je;
                        je["tool"]         = tc.name;
                        je["tool_call_id"] = tc.id;
                        je["duration_ms"]  = deny_ms;
                        je["ok"]           = false;
                        je["error"]        = tool_result_envelope.value("error", std::string{});
                        session_append_journal(working.task_id, je);
                    }
                    messages.push_back({"tool", envelope_str, tc.id, nlohmann::json()});
                    res.transcript["messages"].push_back(
                        {{"role",         "tool"},
                         {"tool_call_id", tc.id},
                         {"content",      tool_result_envelope}});

                    finish_run_ui(-1, deny_ms, false);

                    if (consecutive_sec_denials >= k_max_consecutive_sec_denials) {
                        logger::warn("[sec] abort turn: " + std::to_string(consecutive_sec_denials)
                                     + " consecutive security denials — stopping agent loop");
                        res.ok    = false;
                        res.error = "Turn aborted: repeated security policy violations. "
                                    "Review security settings if this was unintentional.";
                        res.transcript["error"] = res.error;
                        (void)detail::emit(prepared.cb,
                                           Event{Event::Error, res.error, "", nlohmann::json()});
                        finish_features();
                        return res;
                    }
                    continue;
                }
            }
#endif

            begin_run_ui();

            if (dispatch_host_tool(prepared, working.task_id, working.folder_hint, tc.name, args,
                                   tool_result_envelope, envelope_str)) {
            } else if (prepared.mcp_bridge
                    && prepared.mcp_bridge->is_mcp_openai_function(tc.name)) {
                std::string mcp_err;
                if (!prepared.mcp_bridge->call_tool(tc.name, args, envelope_str, mcp_err)) {
                    tool_result_envelope = nlohmann::json{
                        {"ok", false},
                        {"error", mcp_err.empty() ? "MCP tool dispatch failed" : mcp_err}};
                    envelope_str = detail::safe_json_dump(tool_result_envelope);
                } else {
                    tool_result_envelope = media::json::parse_or(envelope_str, nlohmann::json::object());
                }
            } else {
                // Tool execution may cross an internal dispatch boundary where
                // the feature-level thread-local task guard is not visible.
                // Rebind the active session on the exact path-tool execution
                // thread so memory_find can enforce same-session payload access.
                ToolTaskContextGuard task_context(working.task_id);
                path::set_change_tool_call_id(tc.id);
                path::ExecuteResult er = path::execute(tc.name, args);
                envelope_str           = path::envelope_text(er);
                tool_result_envelope   = std::move(er.envelope);
            }

            // A successful tool dispatch resets the consecutive-deny counter.
            // This lets a session that had one exploratory denial continue
            // normally; the counter only aborts on a run of back-to-back denials.
            {
                bool tool_ok = false;
                try { tool_ok = tool_result_envelope.value("ok", false); } catch (...) {}
                if (tool_ok) consecutive_sec_denials = 0;
            }

            const auto tool_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_tool).count();

            (void)detail::emit(prepared.cb,
                               Event{Event::ToolResult, "", tc.name,
                                     nlohmann::json{{"id",          tc.id},
                                                    {"envelope",    tool_result_envelope},
                                                    {"duration_ms", tool_ms}}});

            // #9 mid-turn durability: persist a one-line summary of every
            // tool call immediately, before the next LLM round. If the
            // process dies between here and the end-of-turn memory_update
            // we still know what work succeeded — the journal is the
            // authoritative ground truth for replay/recovery.
            if (!working.task_id.empty()) {
                nlohmann::json je = nlohmann::json::object();
                je["tool"]         = tc.name;
                je["tool_call_id"] = tc.id;
                je["duration_ms"]  = tool_ms;
                bool ok_value = false;
                try { ok_value = tool_result_envelope.value("ok", false); } catch (...) {}
                je["ok"] = ok_value;
                // Surface produced paths so a recovery pass can detect / promote
                // them into session items even when the patch never landed.
                try {
                    if (tool_result_envelope.contains("results") && tool_result_envelope["results"].is_array()) {
                        nlohmann::json paths = nlohmann::json::array();
                        for (const auto& r : tool_result_envelope["results"]) {
                            if (!r.is_object()) continue;
                            if (r.contains("output_path") && r["output_path"].is_string())
                                paths.push_back(r["output_path"]);
                            else if (r.contains("path") && r["path"].is_string())
                                paths.push_back(r["path"]);
                        }
                        if (!paths.empty()) je["output_paths"] = std::move(paths);
                    }
                    if (!ok_value && tool_result_envelope.contains("error")
                        && tool_result_envelope["error"].is_string()) {
                        je["error"] = tool_result_envelope["error"];
                    }
                } catch (...) {}
                session_append_journal(working.task_id, je);
            }

            if (run_ui_live) {
                bool run_ok = false;
                try { run_ok = tool_result_envelope.value("ok", false); } catch (...) {}
                int exit_code = run_ok ? 0 : -1;
                try {
                    if (tool_result_envelope.contains("exit_code"))
                        exit_code = tool_result_envelope.value("exit_code", exit_code);
                } catch (...) {}
                finish_run_ui(exit_code, tool_ms, run_ok);
            }

            messages.push_back({"tool", envelope_str, tc.id, nlohmann::json()});
            res.transcript["messages"].push_back({{"role",         "tool"},
                                                  {"tool_call_id", tc.id},
                                                  {"content",      tool_result_envelope}});

#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
            if (media::llm::browser_use::is_browser_use_tool(tc.name)) {
                browser_tools_seen_this_turn = true;
                note_browser_injection_from_envelope(tool_result_envelope);
                if (working.browser_prompt_injection_abort)
                    return abort_prompt_injection(working.browser_prompt_injection_reason);
            }
#endif
        } // end serial for
        } // end else (serial dispatch)
    }

    // Exhausted max_iterations.
    res.error = "agent: hit max_iterations (" + std::to_string(max_iter)
                + ") without a final response \u2014 model kept calling tools";
    res.transcript["error"] = res.error;
    (void)detail::emit(prepared.cb, Event{Event::Error, res.error, "", nlohmann::json()});
    finish_features();
    return res;
}

// ── AgentFactory::default_agent ───────────────────────────────────────────────

namespace AgentFactory {

void ensure_app_search_path()
{
    prepend_app_search_dirs_to_process_path();
}

Agent default_agent() {
    return default_agent(DefaultAgentOptions{});
}

Agent default_agent(DefaultAgentOptions options) {
    AgentOptions opts;
#if FEATURE_COMMAND_LLAMA_INFERENCE
    opts.features.push_back(feature_embed_inference()); // Scope(10): lazy-init embedding backend (before everything)
#endif
    opts.features.push_back(feature_path_scope());    // Scope(10): pin base path + blocklist (RAII)
#if FEATURE_HOME_LLM_TOOLS
    opts.features.push_back(feature_planner());       // Plan(20):  optional compact planner -> allowlist + TurnMode
#if defined(_WIN32) && defined(FEATURE_BROWSER) && FEATURE_BROWSER \
    && defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT \
    && defined(FEATURE_TOOLS_APP_USE) && FEATURE_TOOLS_APP_USE \
    && defined(FEATURE_AGENT_COMPUTER_USE) && FEATURE_AGENT_COMPUTER_USE
    if (pm::policy::browser_use_allowed())
        opts.features.push_back(feature_browser_use());   // Plan(20): active WebView context + browser schema retention
#endif
    opts.features.push_back(feature_tools());         // Tools(30): build path-catalog tools JSON
    opts.features.push_back(feature_host_tools());    // Tools(30): IAgentToolProvider schemas
#if FEATURE_HOME_LLM_TOOLS && FEATURE_MCP_CLIENT
#if defined(FEATURE_WINDOWS_GPO) && FEATURE_WINDOWS_GPO
    if (pm::policy::is_agent_feature_enabled("Agent.EnableMcpClient"))
#endif
        opts.features.push_back(feature_mcp());       // Tools(30): merge mcp.json tools + create bridge
#endif
#endif
    opts.features.push_back(feature_memory());        // Context(40): thread-local task-ID scope + artifact persist
    if (options.tool_checkpoints_enabled)
        opts.features.push_back(feature_tool_checkpoints()); // Context(40): replaceable tool outcomes
    opts.features.push_back(feature_memory_recall()); // Context(40): hybrid recall index + pending inject
    opts.features.push_back(feature_snapshot());      // Context(40): P12 load/save named snapshots (after memory)
#if FEATURE_HOME_LLM_SKILLS
#if defined(FEATURE_WINDOWS_GPO) && FEATURE_WINDOWS_GPO
    if (pm::policy::is_agent_feature_enabled("Agent.EnableSkills"))
#endif
        opts.features.push_back(feature_skills());    // Context(40): discover skills and inject summary
#endif
    opts.features.push_back(feature_agent_md());      // Context(40): Agent.md from the current folder
    opts.features.push_back(feature_workflow());     // Context(40): filmstrip / workflow inject
    opts.features.push_back(feature_system_prompt()); // Prompt(50): assemble system prompt (sees final Turn + tools)
    opts.features.push_back(feature_self_check());    // Finish(60): P9 structural quality gate (after all others)

    // ── P8: phase ordering assertion ─────────────────────────────────────────
    // Verify that features are registered in non-decreasing phase order.
    // Out-of-order features silently corrupt the pipeline (e.g. the system
    // prompt is built before memory is injected).  This check fires at startup
    // so the violation is caught immediately rather than producing subtle bugs.
    {
        FeaturePhase prev_phase = FeaturePhase::Scope;
        for (std::size_t i = 0; i < opts.features.size(); ++i) {
            const FeaturePhase fp = opts.features[i]->phase();
            if (static_cast<uint8_t>(fp) < static_cast<uint8_t>(prev_phase)) {
                logger::warn(std::string("AgentFactory: feature[")
                    + std::to_string(i)
                    + "] phase("
                    + std::to_string(static_cast<int>(fp))
                    + ") is out of order (prev phase="
                    + std::to_string(static_cast<int>(prev_phase))
                    + ") — feature pipeline may not behave as expected");
            }
            prev_phase = fp;
        }
    }

    return Agent(std::move(opts));
}

Agent security_agent() {
    AgentOptions opts;
    opts.features.push_back(feature_security_prompt());
    return Agent(std::move(opts));
}

} // namespace AgentFactory

} // namespace media::llm::agent

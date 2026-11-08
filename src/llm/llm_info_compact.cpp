#include "llm_info_compact.hpp"
#include "agent_child_launch.hpp"

#include "agent_skills.hpp"
#include "agent_tools.hpp"
#include "core/chat_settings_schema.hpp"
#include "cli/pm_image_command_info.hpp"
#include "core/settings_runtime.hpp"
#include "core/settings_store.hpp"
#include "mcp_probe.hpp"
#include "mcp_config.hpp"
#include "path_tool_catalog.hpp"
#include "string/string_sanitizers.hpp"

#if defined(_WIN32)
#include "core/host_backends.hpp"
#endif

#if defined(FEATURE_XBLOX) && FEATURE_XBLOX
#include "blocks/builtin_blocks.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

namespace media::llm::info {
namespace {

#if defined(_WIN32)
namespace catalog = pm::host::cli;
#else
namespace catalog = pm::cli;
#endif

std::string trim(std::string s)
{
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](char c) { return !is_space(static_cast<unsigned char>(c)); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [&](char c) { return !is_space(static_cast<unsigned char>(c)); }).base(), s.end());
    return s;
}

std::string lower_trim(std::string s)
{
    s = trim(std::move(s));
    s = media::strutil::lower_ascii(std::move(s));
    return s;
}

std::string preset_display_name(const runtime_settings::ChatProviderPreset& p)
{
    return p.name.empty() ? p.id : p.name;
}

std::optional<runtime_settings::ChatProviderPreset> resolve_context_preset(const std::string& requested,
                                                                           std::string& warning)
{
    std::vector<runtime_settings::ChatProviderPreset> presets;
    std::string err;
    if (!runtime_settings::load_chat_provider_presets(presets, err)) {
        warning = err.empty() ? "could not load chat presets" : err;
        return std::nullopt;
    }
    if (!err.empty() && warning.empty())
        warning = err;

    std::string select_err;
    if (const auto* preset = media::settings_schema::select_chat_provider_preset(presets, requested, &select_err))
        return *preset;
    if (!select_err.empty())
        warning = select_err;
    return std::nullopt;
}

std::string squash(std::string s)
{
    std::string out;
    out.reserve(s.size());
    bool in_ws = false;
    bool in_md = false;
    for (char c : s) {
        if (c == '`' || c == '*') {
            in_md = !in_md;
            continue;
        }
        (void)in_md;
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!in_ws) out.push_back(' ');
            in_ws = true;
            continue;
        }
        out.push_back(c);
        in_ws = false;
    }
    return trim(out);
}

std::string shorten(std::string s, std::size_t max_chars)
{
    s = squash(std::move(s));
    if (s.size() <= max_chars)
        return s;
    if (max_chars <= 3)
        return media::str_san::utf8_safe_prefix(s, max_chars);
    return media::str_san::utf8_safe_ellipsize(s, max_chars);
}

std::string type_suffix(const nlohmann::json& p)
{
    if (!p.is_object())
        return {};
    const std::string t = p.value("type", std::string{});
    if (t == "array")
        return "[]";
    return {};
}

bool json_string_array_contains(const nlohmann::json& arr, const std::string& needle)
{
    if (!arr.is_array())
        return false;
    for (const auto& item : arr) {
        if (item.is_string() && item.get<std::string>() == needle)
            return true;
    }
    return false;
}

struct Arg {
    std::string name;
    bool required = false;
    std::string suffix;
};

void push_arg(std::vector<Arg>& args, std::unordered_set<std::string>& seen, Arg arg)
{
    if (arg.name.empty() || seen.count(arg.name))
        return;
    seen.insert(arg.name);
    args.push_back(std::move(arg));
}

std::vector<Arg> schema_args(const nlohmann::json& schema)
{
    std::vector<Arg> args;
    std::unordered_set<std::string> seen;
    const auto props_it = schema.find("properties");
    if (props_it == schema.end() || !props_it->is_object())
        return args;

    const nlohmann::json required = schema.value("required", nlohmann::json::array());
    for (auto it = props_it->begin(); it != props_it->end(); ++it) {
        const std::string name = it.key();
        const auto& prop = it.value();
        if (prop.is_object() && prop.value("type", std::string{}) == "object" && prop.contains("properties")) {
            const nlohmann::json child_required = prop.value("required", nlohmann::json::array());
            const auto& child_props = prop["properties"];
            if (child_props.is_object()) {
                for (auto cit = child_props.begin(); cit != child_props.end(); ++cit) {
                    push_arg(args, seen, Arg{cit.key(), json_string_array_contains(child_required, cit.key()), type_suffix(cit.value())});
                }
            }
            continue;
        }
        push_arg(args, seen, Arg{name, json_string_array_contains(required, name), type_suffix(prop)});
    }
    std::stable_sort(args.begin(), args.end(), [](const Arg& a, const Arg& b) {
        if (a.required != b.required)
            return a.required > b.required;
        return false;
    });
    return args;
}

std::string arg_list(std::vector<Arg> args, std::size_t max_args = 8)
{
    std::ostringstream os;
    for (std::size_t i = 0; i < args.size() && i < max_args; ++i) {
        if (i) os << ", ";
        os << args[i].name << args[i].suffix;
        if (!args[i].required)
            os << "?";
    }
    if (args.size() > max_args)
        os << ", ...";
    return os.str();
}

const char* group_label(pm::llm::AgentToolGroup g)
{
    switch (g) {
    case pm::llm::AgentToolGroup::File: return "file";
    case pm::llm::AgentToolGroup::Image: return "image";
    case pm::llm::AgentToolGroup::Utility: return "utility";
    case pm::llm::AgentToolGroup::Scheduler: return "scheduler";
    case pm::llm::AgentToolGroup::Memory: return "memory";
    case pm::llm::AgentToolGroup::Computer: return "computer";
    case pm::llm::AgentToolGroup::Browser: return "browser";
    case pm::llm::AgentToolGroup::Service: return "service";
    case pm::llm::AgentToolGroup::App: return "app";
    case pm::llm::AgentToolGroup::Network: return "network";
    }
    return "tool";
}

bool omit_compact_builtin_command(const char* id)
{
    if (!id || !id[0])
        return true;
    static const std::unordered_set<std::string> hidden = {
        "register-explorer",
        "register-startmenu",
        "service",
        "login",
        "logout",
        "settings",
        "info",
        "status",
        "daemon",
        "provider",
    };
    return hidden.count(id) != 0;
}

#if FEATURE_MCP_CLIENT

struct McpToolRow {
    std::string function_name;
    std::string server_name;
    std::string remote_name;
    std::string description;
    std::string input_schema_json;
};

struct McpPolicySummary {
    std::vector<std::string> enabled_servers;
    std::vector<std::string> problems;
    std::vector<McpToolRow> tools;
    int disabled_count = 0;
};

std::string openai_slug(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if (std::isalnum(c))
            out += static_cast<char>(std::tolower(c));
        else if (c == '_' || c == '-')
            out += static_cast<char>(c);
        else
            out += '_';
    }
    return out.empty() ? std::string{"srv"} : out;
}

std::string truncate_fn_name(std::string s)
{
    if (s.size() > 64)
        s.resize(64);
    return s;
}

std::string make_mcp_function_name(const std::string& server_key,
                                   const std::string& remote_tool,
                                   std::unordered_set<std::string>& existing)
{
    std::string base = truncate_fn_name("mcp_" + openai_slug(server_key) + "__" + openai_slug(remote_tool));
    if (!existing.count(base)) {
        existing.insert(base);
        return base;
    }
    for (int i = 2; i < 1000; ++i) {
        const std::string suffix = "_" + std::to_string(i);
        std::string cand = base;
        if (cand.size() + suffix.size() > 64)
            cand.resize(64 - suffix.size());
        cand += suffix;
        if (!existing.count(cand)) {
            existing.insert(cand);
            return cand;
        }
    }
    existing.insert(base + "_x");
    return base + "_x";
}

#endif // FEATURE_MCP_CLIENT

void append_commands(std::ostream& os)
{
    os << "## Commands\n";
    os << "Assume `" << pm::brand::k_exe_basename_cli_u8 << "` is in PATH. Custom commands are CLI subcommands: `" << pm::brand::k_exe_basename_cli_u8 << " <custom.id> [args]`. "
       << "Spawn another agent quietly with " << media::llm::agent::child_launch_canonical_example() << ".\n"
       << "**Open files in " << pm::brand::k_app_display_u8 << " UI** (GUI binary, NOT " << pm::brand::k_exe_stem_cli_u8 << "):\n"
       << "- Viewer: `" << pm::brand::k_app_id_u8 << " --ui-preset viewer --src \"<absolute_path>\"`\n"
       << "- Main workbench: `" << pm::brand::k_app_id_u8 << " --ui-preset main --src \"<absolute_path>\"`\n"
       << "- Chat: `" << pm::brand::k_app_id_u8 << " --ui-preset chat --src \"<absolute_path>\"`\n"
       << "GUI launches return no stdout — empty run result is success, do NOT retry.\n";

    std::vector<std::string> builtins;
    for (const auto& c : catalog::registered_cli_commands()) {
        if (!c.available || omit_compact_builtin_command(c.id))
            continue;
        builtins.emplace_back(c.id);
    }
    if (!builtins.empty()) {
        os << "- builtins: ";
        for (std::size_t i = 0; i < builtins.size(); ++i) {
            if (i) os << ", ";
            os << "`" << builtins[i] << "`";
        }
        os << "\n";
    }

    const auto custom = catalog::visible_custom_commands();
    if (custom.empty()) {
        os << "- custom: none enabled/visible\n\n";
        return;
    }

    os << "- custom enabled/visible:\n";
    for (const auto& c : custom) {
        os << "  - `" << shorten(c.id, 80) << "`";
        if (!c.label.empty())
            os << " - " << shorten(c.label, 80);
        os << "\n";
    }
    os << "\n";
}

void append_presets(std::ostream& os)
{
    std::vector<runtime_settings::ChatProviderPreset> presets;
    std::string err;
    (void)runtime_settings::load_chat_provider_presets(presets, err);
    os << "## Presets\n";
    os << "Use " << media::llm::agent::child_launch_canonical_example() << " for narrow child agents; save current args with `--save-preset <name>`.\n";
    if (!err.empty())
        os << "- warning: " << shorten(err, 100) << "\n";
    if (presets.empty()) {
        os << "- none saved\n\n";
        return;
    }
    for (const auto& p : presets) {
        os << "- " << preset_display_name(p)
           << ": router=" << (p.chat.router.empty() ? "?" : p.chat.router)
           << " model=" << (p.chat.model.empty() ? "?" : p.chat.model)
           << " planner=" << (p.chat.planner_enabled ? "on" : "off");
        // Only mention the dedicated planner router/model when actually overridden — the common
        // case is "inherit Text router/model" and we don't want to bloat the compact index.
        if (!p.chat.planner_router.empty())
            os << " planner_router=" << p.chat.planner_router;
        if (!p.chat.planner_model.empty())
            os << " planner_model=" << p.chat.planner_model;
        if (!p.chat.security_router.empty())
            os << " security_router=" << p.chat.security_router;
        if (!p.chat.security_model.empty())
            os << " security_model=" << p.chat.security_model;
        if (p.has_tools) {
            os << " tools(disabled=" << p.tools.disabled_path_tools.size()
               << ",mcp=" << (p.tools.mcp_tools_enabled ? "on" : "off") << ")";
        }
        if (p.has_skills) {
            os << " skills(" << (p.skills.enabled ? "on" : "off")
               << ",pinned=" << p.skills.pinned.size()
               << ",disabled=" << p.skills.disabled.size() << ")";
        }
        os << " agent_md=" << (p.load_agent_md ? "on" : "off");
        os << "\n";
    }
    os << "\n";
}

void append_tools(std::ostream& os, const std::vector<std::string>* disabled_path_tools)
{
    const auto tools = disabled_path_tools
        ? media::llm::path::tool_catalog_excluding(*disabled_path_tools)
        : media::llm::path::tool_catalog();
    const auto* reg = pm::llm::agent_tool_registry_data();
    const auto reg_size = pm::llm::agent_tool_registry_size();

    os << "## Tools\n";
    os << "Path tools. Expand exact schema only after choosing a tool.\n";
    if (disabled_path_tools && !disabled_path_tools->empty())
        os << "- policy: " << disabled_path_tools->size() << " path tools disabled; listing enabled tools only.\n";
    for (const auto& t : tools) {
        const pm::llm::AgentToolInfo* info = nullptr;
        for (std::size_t i = 0; i < reg_size; ++i) {
            if (t.name == reg[i].name) {
                info = &reg[i];
                break;
            }
        }
        os << "- " << t.name << "(" << arg_list(schema_args(t.input_schema)) << ")";
        if (info)
            os << " [" << group_label(info->group) << "]";
        os << " - " << shorten(info ? info->short_desc : t.description, 140) << "\n";
    }
    os << "\n";
}

#if FEATURE_MCP_CLIENT

McpPolicySummary summarize_mcp_policy(const nlohmann::json& probe,
                                      bool mcp_tools_enabled,
                                      const std::vector<std::string>& disabled_mcp_servers)
{
    McpPolicySummary summary;
    if (!mcp_tools_enabled)
        return summary;

    std::unordered_set<std::string> disabled;
    for (const auto& name : disabled_mcp_servers)
        disabled.insert(lower_trim(name));

    std::unordered_set<std::string> existing_names;
    const auto servers_it = probe.find("servers");
    if (servers_it == probe.end() || !servers_it->is_array())
        return summary;

    for (const auto& server : *servers_it) {
        if (!server.is_object())
            continue;
        const std::string name = server.value("name", std::string{});
        if (name.empty())
            continue;

        const bool disabled_by_policy = disabled.count(lower_trim(name)) != 0;
        const bool skipped = server.value("skipped", false);
        const std::string skip_reason = server.value("skip_reason", std::string{});
        const bool config_disabled = skipped && lower_trim(skip_reason) == "disabled";

        if (disabled_by_policy || config_disabled) {
            ++summary.disabled_count;
        }

        const bool tools_ok = server.value("tools_list_ok", false);
        std::vector<McpToolRow> server_tools;
        if (tools_ok && server.contains("tools") && server["tools"].is_array()) {
            for (const auto& tool : server["tools"]) {
                if (!tool.is_object() || !tool.contains("name") || !tool["name"].is_string())
                    continue;
                const std::string remote_name = tool["name"].get<std::string>();
                std::string input_schema_json;
                if (tool.contains("inputSchema") && tool["inputSchema"].is_object())
                    input_schema_json = tool["inputSchema"].dump();
                server_tools.push_back(McpToolRow{
                    make_mcp_function_name(name, remote_name, existing_names),
                    name,
                    remote_name,
                    tool.value("description", std::string{}),
                    std::move(input_schema_json),
                });
            }
        }

        if (disabled_by_policy || config_disabled)
            continue;
        if (skipped) {
            summary.problems.push_back(name + ": " + shorten(skip_reason.empty() ? "skipped" : skip_reason, 90));
            continue;
        }

        std::string server_label = name;
        const std::string transport = server.value("transport", std::string{});
        if (!transport.empty())
            server_label += "(" + transport + ")";
        summary.enabled_servers.push_back(std::move(server_label));

        if (!server.value("handshake_ok", false)) {
            summary.problems.push_back(name + ": handshake failed"
                                       + (server.contains("handshake_error")
                                           ? (": " + shorten(server["handshake_error"].get<std::string>(), 90))
                                           : std::string{}));
            continue;
        }
        if (!tools_ok) {
            summary.problems.push_back(name + ": tools/list failed"
                                       + (server.contains("tools_list_error")
                                           ? (": " + shorten(server["tools_list_error"].get<std::string>(), 90))
                                           : std::string{}));
            continue;
        }
        summary.tools.insert(summary.tools.end(), server_tools.begin(), server_tools.end());
    }

    return summary;
}

bool same_disabled_server_policy(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
    std::unordered_set<std::string> aa;
    std::unordered_set<std::string> bb;
    for (const auto& v : a)
        aa.insert(lower_trim(v));
    for (const auto& v : b)
        bb.insert(lower_trim(v));
    return aa == bb;
}

bool append_mcp_policy_summary(std::ostream& os,
                               const std::string& label,
                               bool mcp_tools_enabled,
                               const std::vector<std::string>& disabled_mcp_servers,
                               const nlohmann::json& probe)
{
    if (!mcp_tools_enabled)
        return false;

    const McpPolicySummary summary = summarize_mcp_policy(probe, mcp_tools_enabled, disabled_mcp_servers);
    if (summary.enabled_servers.empty() && summary.tools.empty() && summary.problems.empty())
        return false;

    os << "- " << label << ": ";
    if (summary.enabled_servers.empty()) {
        os << "no enabled servers";
    } else {
        os << "servers ";
        for (std::size_t i = 0; i < summary.enabled_servers.size(); ++i) {
            if (i)
                os << ", ";
            os << "`" << summary.enabled_servers[i] << "`";
        }
    }
    os << "\n";

    constexpr int k_max_tools = 30;
    int shown = 0;
    for (const auto& tool : summary.tools) {
        if (shown >= k_max_tools)
            break;
        os << "  - `" << tool.function_name << "`";
        if (!tool.description.empty())
            os << " - " << shorten(tool.description, 100);
        os << "\n";
        ++shown;
    }
    if (static_cast<int>(summary.tools.size()) > shown)
        os << "  - ... " << (static_cast<int>(summary.tools.size()) - shown) << " more MCP tools\n";
    for (const auto& problem : summary.problems)
        os << "  - warning: " << problem << "\n";
    return true;
}

void append_mcp_servers(std::ostream& os,
                        bool allow_probe,
                        const runtime_settings::ChatProviderPreset* selected_preset)
{
    os << "## MCP Servers\n";
    runtime_settings::GlobalToolSettings global_policy;
    std::string policy_err;
    (void)runtime_settings::load_global_tool_settings(global_policy, policy_err);
    if (!policy_err.empty())
        os << "- warning: " << shorten(policy_err, 100) << "\n";

    std::string probe_note;
    const nlohmann::json probe = media::llm::mcp::load_or_probe_mcp_tools(allow_probe, probe_note);
    (void)probe_note;

    if (!probe.value("exists", true)) {
        os << "- none configured (`mcp.json` missing)\n\n";
        return;
    }
    if (probe.contains("read_error"))
        os << "- read error: " << shorten(probe["read_error"].get<std::string>(), 120) << "\n";
    if (probe.contains("parse_error"))
        os << "- parse error: " << shorten(probe["parse_error"].get<std::string>(), 120) << "\n";
    if (probe.contains("config_shape_error"))
        os << "- config error: " << shorten(probe["config_shape_error"].get<std::string>(), 120) << "\n";

    bool shown_any = false;
    if (selected_preset && selected_preset->has_tools) {
        shown_any = append_mcp_policy_summary(os, "preset `" + preset_display_name(*selected_preset) + "`",
                                              selected_preset->tools.mcp_tools_enabled,
                                              selected_preset->tools.disabled_mcp_servers, probe);
    } else {
        shown_any = append_mcp_policy_summary(os, "enabled", global_policy.mcp_tools_enabled,
                                              global_policy.disabled_mcp_servers, probe);
    }

    if (!selected_preset) {
        std::vector<runtime_settings::ChatProviderPreset> presets;
        std::string preset_err;
        (void)runtime_settings::load_chat_provider_presets(presets, preset_err);
        if (!preset_err.empty())
            os << "- preset warning: " << shorten(preset_err, 100) << "\n";
        for (const auto& preset : presets) {
            if (!preset.has_tools)
                continue;
            if (preset.tools.mcp_tools_enabled == global_policy.mcp_tools_enabled
                && same_disabled_server_policy(preset.tools.disabled_mcp_servers,
                                               global_policy.disabled_mcp_servers)) {
                continue;
            }
            shown_any = append_mcp_policy_summary(os, "preset `" + preset_display_name(preset) + "`",
                                                  preset.tools.mcp_tools_enabled,
                                                  preset.tools.disabled_mcp_servers, probe) || shown_any;
        }
    }
    if (!shown_any)
        os << "- no enabled MCP tools under current tool policies\n";
    os << "\n";
}

#else // FEATURE_MCP_CLIENT

void append_mcp_servers(std::ostream& os,
                        bool /*allow_probe*/,
                        const runtime_settings::ChatProviderPreset* /*selected_preset*/)
{
    os << "## MCP Servers\n- not compiled in this build\n\n";
}

#endif // FEATURE_MCP_CLIENT

const char* skill_source_label(skills::SkillSource source)
{
    switch (source) {
    case skills::SkillSource::Roaming: return "roaming";
    case skills::SkillSource::Workspace: return "workspace";
    }
    return "unknown";
}

void append_skills(std::ostream& os)
{
    runtime_settings::AgentSkillsSettings cfg;
    std::string err;
    (void)runtime_settings::load_agent_skills_settings(cfg, err);

    skills::SkillPolicy policy;
    policy.enabled = cfg.enabled;
    policy.roaming_enabled = cfg.roaming_enabled;
    policy.workspace_enabled = cfg.workspace_enabled;
    policy.pinned = cfg.pinned;
    policy.disabled = cfg.disabled;

    if (!policy.enabled)
        return;

    const auto snap = skills::discover_skills(policy, media::path::path_to_utf8(std::filesystem::current_path()));
    if (!snap.policy.enabled)
        return;

    os << "## Skills\n";
    os << "Load only when relevant; active means always/pinned and available.\n";
    if (!err.empty())
        os << "- warning: " << shorten(err, 100) << "\n";
    int shown = 0;
    for (const auto& e : snap.entries) {
        if (e.disabled)
            continue;
        os << "- " << e.name << " [" << skill_source_label(e.source);
        if (e.active) os << ", active";
        else if (!e.available) os << ", unavailable";
        os << "] - " << shorten(e.description, 120) << "\n";
        ++shown;
    }
    if (shown == 0)
        os << "- none discovered\n";
    os << "\n";
}

std::string block_params(const nlohmann::json& params, const nlohmann::json& fallback_block)
{
    static const std::unordered_set<std::string> runtime_noise = {
        "enabled", "continueOnError", "continue_on_error", "onError", "abortOnError",
        "background", "defaultDelayMs", "defaultWaitTimeoutMs", "defaultWaitIntervalMs",
    };
    std::vector<Arg> args;
    std::unordered_set<std::string> seen;
    if (params.is_array()) {
        for (const auto& p : params) {
            if (!p.is_object())
                continue;
            const std::string name = p.value("name", std::string{});
            if (runtime_noise.count(name))
                continue;
            push_arg(args, seen, Arg{name, p.value("required", false), p.value("type", std::string{}) == "array" ? "[]" : std::string{}});
        }
    }
    if (args.empty() && fallback_block.is_object()) {
        for (auto it = fallback_block.begin(); it != fallback_block.end(); ++it) {
            const std::string name = it.key();
            if (name == "kind" || name == "id")
                continue;
            push_arg(args, seen, Arg{name, false, it.value().is_array() ? "[]" : std::string{}});
        }
    }
    return arg_list(std::move(args), 9);
}

void append_xblox(std::ostream& os)
{
    os << "## XBlox Blocks\n";
#if defined(FEATURE_XBLOX) && FEATURE_XBLOX
    os << "Block JSON uses kind plus args. Child lists are arrays of blocks.\n";
    const auto defs = media::xblox::blocks::registered_block_definitions();
    for (const auto& b : defs) {
        os << "- " << b.kind << "(" << block_params(b.params, b.default_block) << ")";
        if (!b.group.empty())
            os << " [" << b.group << "]";
        if (!b.description.empty())
            os << " - " << shorten(b.description, 90);
        os << "\n";
    }
#else
    os << "- not compiled in this build\n";
#endif
    os << "\n";
}

} // namespace

#if FEATURE_MCP_CLIENT

nlohmann::json mcp_probe_cached(bool allow_probe, std::string* note)
{
    return media::llm::mcp::mcp_probe_cached(allow_probe, note);
}

std::vector<McpToolInventoryEntry> mcp_tool_inventory(bool allow_probe)
{
    std::string probe_note;
    const nlohmann::json probe = media::llm::mcp::load_or_probe_mcp_tools(allow_probe, probe_note);
    (void)probe_note;

    std::unordered_map<std::string, std::size_t> by_name;
    std::vector<McpToolInventoryEntry> out;
    auto append_policy = [&](const std::string& label,
                             bool mcp_tools_enabled,
                             const std::vector<std::string>& disabled_mcp_servers) {
        const McpPolicySummary summary = summarize_mcp_policy(probe, mcp_tools_enabled, disabled_mcp_servers);
        for (const auto& tool : summary.tools) {
            auto it = by_name.find(tool.function_name);
            if (it == by_name.end()) {
                McpToolInventoryEntry row;
                row.function_name = tool.function_name;
                row.server_name = tool.server_name;
                row.remote_name = tool.remote_name;
                row.description = tool.description;
                row.input_schema_json = tool.input_schema_json;
                row.policy_labels.push_back(label);
                by_name.emplace(row.function_name, out.size());
                out.push_back(std::move(row));
            } else {
                auto& labels = out[it->second].policy_labels;
                if (std::find(labels.begin(), labels.end(), label) == labels.end())
                    labels.push_back(label);
            }
        }
    };

    runtime_settings::GlobalToolSettings global_policy;
    std::string policy_err;
    (void)runtime_settings::load_global_tool_settings(global_policy, policy_err);
    append_policy("global", global_policy.mcp_tools_enabled, global_policy.disabled_mcp_servers);

    std::vector<runtime_settings::ChatProviderPreset> presets;
    std::string preset_err;
    (void)runtime_settings::load_chat_provider_presets(presets, preset_err);
    for (const auto& preset : presets) {
        if (!preset.has_tools)
            continue;
        const std::string name = preset.name.empty() ? preset.id : preset.name;
        append_policy("preset:" + name, preset.tools.mcp_tools_enabled, preset.tools.disabled_mcp_servers);
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.function_name < b.function_name;
    });
    return out;
}

#else // FEATURE_MCP_CLIENT

nlohmann::json mcp_probe_cached(bool /*allow_probe*/, std::string* /*note*/)
{
    return nlohmann::json::object();
}

std::vector<McpToolInventoryEntry> mcp_tool_inventory(bool /*allow_probe*/)
{
    return {};
}

#endif // FEATURE_MCP_CLIENT

std::string build_compressed_markdown(const CompressedMarkdownOptions& options)
{
    std::ostringstream os;
    os << "# " << pm::brand::k_app_id_u8 << " planner context\n\n";
    os << "Keep prompt small: select needed skills/tool groups/blocks first; expand details only on demand.\n\n";

    std::string preset_warning;
    const auto selected_preset = resolve_context_preset(options.preset, preset_warning);
    if (selected_preset) {
        os << "- effective preset: `" << preset_display_name(*selected_preset) << "`\n\n";
    } else if (!preset_warning.empty()) {
        os << "- preset warning: " << shorten(preset_warning, 100) << "\n\n";
    }

    if (options.include_commands)
        append_commands(os);
    if (options.include_presets)
        append_presets(os);
    if (options.include_tools)
        append_tools(os, selected_preset && selected_preset->has_tools
            ? &selected_preset->tools.disabled_path_tools
            : nullptr);
    if (options.include_mcp)
        append_mcp_servers(os, options.allow_mcp_probe, selected_preset ? &*selected_preset : nullptr);
    if (options.include_skills)
        append_skills(os);
    if (options.include_xblox)
        append_xblox(os);
    return os.str();
}

} // namespace media::llm::info

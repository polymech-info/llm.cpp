#include "agent_runner_claude_defaults.hpp"
#include "fs/path_sanitizer.hpp"
#include "agent_internal.hpp"

#include "constants.hpp"
#include "mcp_config.hpp"

#include "core/command_variables.hpp"
#include "core/settings_runtime.hpp"
#include "core/settings_store.hpp"
#include "logging/pm_log.hpp"

#include <cctype>
#include <fstream>
#include <filesystem>
#include <vector>
#include "string/string_utils.hpp"

namespace fs = std::filesystem;

namespace media::llm::agent {
namespace {

constexpr const char kClaudeDefaultsFile[] = "claude_defaults.json";
constexpr const char kLogChannel[]         = "agent-runner:claude";

fs::path resolve_working_dir(const std::string& working_dir)
{
    std::error_code ec;
    if (!working_dir.empty()) {
        fs::path p = media::path::path_from_utf8(working_dir);
        if (!p.is_absolute())
            p = fs::absolute(p, ec);
        if (!ec && fs::exists(p, ec))
            return fs::is_directory(p, ec) ? p : p.parent_path();
    }
    return fs::current_path(ec);
}

std::string normalize_flag_key(std::string key)
{
    while (!key.empty() && key.front() == '-')
        key.erase(key.begin());
    return key;
}

void merge_flags(nlohmann::json& dst, const nlohmann::json& src)
{
    if (!src.is_object())
        return;
    for (auto it = src.begin(); it != src.end(); ++it)
        dst[it.key()] = it.value();
}

bool load_defaults_file(const fs::path& path, ClaudeDefaults& out)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
        return false;

    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;

    nlohmann::json j;
    try {
        in >> j;
    } catch (...) {
        return false;
    }

    if (j.is_object()) {
        nlohmann::json flags = nlohmann::json::object();
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.key() == "sources" || (!it.key().empty() && it.key().front() == '_'))
                continue;
            flags[it.key()] = it.value();
        }
        merge_flags(out.flags, flags);
    }

    out.sources.push_back(media::path::path_to_utf8(path));
    return true;
}

nlohmann::json normalize_mcp_config_for_claude(const nlohmann::json& src)
{
    nlohmann::json out{{"mcpServers", nlohmann::json::object()}};
    if (!src.is_object() || !src.contains("mcpServers") || !src["mcpServers"].is_object())
        return out;

    for (auto it = src["mcpServers"].begin(); it != src["mcpServers"].end(); ++it) {
        if (!it.value().is_object())
            continue;
        const auto& s = it.value();
        if (s.contains("enabled") && s["enabled"].is_boolean() && !s["enabled"].get<bool>())
            continue;

        std::string type;
        if (s.contains("type") && s["type"].is_string() && !s["type"].is_null())
            type = s["type"].get<std::string>();
        else if (s.contains("url") && s["url"].is_string() && !s["url"].get<std::string>().empty())
            type = "http";
        else if (s.contains("command") && s["command"].is_string() && !s["command"].get<std::string>().empty())
            type = "stdio";
        if (type.empty())
            continue;
        if (type == "streamable-http")
            type = "http";

        nlohmann::json dst{{"type", type}};
        if (type == "http" || type == "sse") {
            if (!s.contains("url") || !s["url"].is_string() || s["url"].get<std::string>().empty())
                continue;
            dst["url"] = s["url"];
            if (s.contains("headers") && s["headers"].is_object()) {
                nlohmann::json hdrs = nlohmann::json::object();
                for (auto hit = s["headers"].begin(); hit != s["headers"].end(); ++hit) {
                    if (hit.value().is_string() && !hit.value().get<std::string>().empty())
                        hdrs[hit.key()] = hit.value();
                }
                if (!hdrs.empty())
                    dst["headers"] = std::move(hdrs);
            }
        } else if (type == "stdio") {
            dst["command"] = s["command"];
            if (s.contains("args") && s["args"].is_array())
                dst["args"] = s["args"];
            if (s.contains("env") && s["env"].is_object()) {
                nlohmann::json env = nlohmann::json::object();
                for (auto eit = s["env"].begin(); eit != s["env"].end(); ++eit) {
                    if (eit.value().is_string())
                        env[eit.key()] = eit.value();
                }
                if (!env.empty())
                    dst["env"] = std::move(env);
            }
        } else {
            continue;
        }

        out["mcpServers"][it.key()] = std::move(dst);
    }
    return out;
}

fs::path claude_mcp_export_path()
{
    return media::runtime_settings::effective_temp_dir() / "claude-mcp-config.json";
}

void merge_local_tanit_mcp(nlohmann::json& claude_cfg, const std::string& tanit_mcp_url)
{
    if (tanit_mcp_url.empty())
        return;
    if (!claude_cfg.is_object())
        claude_cfg = nlohmann::json::object();
    if (!claude_cfg.contains("mcpServers") || !claude_cfg["mcpServers"].is_object())
        claude_cfg["mcpServers"] = nlohmann::json::object();
    // The profile often already has "Tanit" → https://llm.polymech.info/mcp.
    // That host asks for an interactive /mcp login, which headless claude -p
    // cannot complete, and Claude then reports the whole Tanit tool set as
    // unauthorized. The runner's loopback replaces every tanit-named server.
    auto& servers = claude_cfg["mcpServers"];
    std::vector<std::string> drop;
    for (auto it = servers.begin(); it != servers.end(); ++it) {
        std::string key = it.key();
        key = media::strutil::lower_ascii(std::move(key));
        if (key == "tanit")
            drop.push_back(it.key());
    }
    for (const auto& key : drop)
        servers.erase(key);
    servers["tanit"] = nlohmann::json{
        {"type", "http"},
        {"url", tanit_mcp_url},
    };
}

fs::path write_claude_mcp_export(const nlohmann::json& claude_cfg)
{
    const fs::path out = claude_mcp_export_path();
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f)
        return {};
    f << claude_cfg.dump(2);
    if (!f.good())
        return {};
    return out;
}

fs::path materialize_mcp_json_for_cli(const fs::path& config_dir,
                                      const std::string& tanit_mcp_url)
{
    nlohmann::json cfg;
    std::string err;
    nlohmann::json claude_cfg{{"mcpServers", nlohmann::json::object()}};
    if (!config_dir.empty() && media::llm::mcp::load_mcp_config(config_dir, cfg, err))
        claude_cfg = normalize_mcp_config_for_claude(cfg);
    merge_local_tanit_mcp(claude_cfg, tanit_mcp_url);

    if (!claude_cfg.contains("mcpServers") || !claude_cfg["mcpServers"].is_object()
        || claude_cfg["mcpServers"].empty()) {
        pm::log::warn(kLogChannel, "claude mcp export: no enabled MCP servers after normalize");
        if (tanit_mcp_url.empty())
            return {};
    }
    return write_claude_mcp_export(claude_cfg);
}

media::commands::VariableMap build_claude_defaults_variable_map(const Turn& turn,
                                                                const fs::path& mcp_config_path)
{
    media::commands::VariableContext ctx;
    ctx.cwd             = turn.folder_hint;
    ctx.selection_paths = turn.selection;
    if (!turn.selection.empty())
        ctx.source_file = turn.selection.front();

    auto vars = media::commands::make_variable_map(ctx);

    const fs::path cwd = resolve_working_dir(turn.folder_hint);
    fs::path config_dir;
    try {
        config_dir = media::settings::get_config_dir();
    } catch (...) {}

    const std::string config_u8 = media::path::path_to_utf8(config_dir);
    const std::string cwd_u8    = media::path::path_to_utf8(cwd);
    const std::string mcp_u8    = media::path::path_to_utf8(mcp_config_path);

    vars["FOLDER"]          = turn.folder_hint;
    vars["SELECTION_COUNT"] = std::to_string(turn.selection.size());
    vars["OPEN_COUNT"]      = std::to_string(turn.open_files.size());
    vars["TASK_ID"]         = turn.task_id;
    vars["CURRENT_SELECTION"]  = media::llm::agent::detail::format_paths_with_stats(turn.selection);
    if (!vars.count("CURRENT_PATH"))
        vars["CURRENT_PATH"] = turn.folder_hint;
    vars["CURRENT_OPEN_FILES"] = media::llm::agent::detail::format_paths_with_stats(turn.open_files);

    // Same file as ${MCP_CONFIG} / ${TEMP_DIR}/claude-mcp-config.json.
    vars["mcp_config"] = mcp_u8;
    if (!mcp_u8.empty())
        vars["MCP_CONFIG"] = mcp_u8;

    // Lowercase aliases kept for early claude_defaults.json templates.
    if (!config_u8.empty()) vars["config_dir"] = config_u8;
    if (!cwd_u8.empty())    vars["cwd"]        = cwd_u8;

    return vars;
}

void append_resolved_flag(std::vector<std::string>& argv,
                          const std::string& flag,
                          const std::string& raw_value,
                          const media::commands::VariableMap& vars)
{
    std::string err;
    const std::string value = media::commands::deep_resolve_variables(raw_value, vars, &err);
    if (!err.empty())
        pm::log::warn(kLogChannel, "claude_defaults " + flag + ": " + err);
    if (value.empty())
        return;
    argv.push_back("--" + flag);
    argv.push_back(value);
}

void append_flag_value(std::vector<std::string>& argv,
                       const std::string& raw_key,
                       const nlohmann::json& value,
                       const media::commands::VariableMap& vars)
{
    const std::string flag = normalize_flag_key(raw_key);
    if (flag.empty() || (!raw_key.empty() && raw_key.front() == '_'))
        return;

    if (value.is_null())
        return;

    if (value.is_boolean()) {
        if (value.get<bool>())
            argv.push_back("--" + flag);
        return;
    }

    if (value.is_string()) {
        append_resolved_flag(argv, flag, value.get<std::string>(), vars);
        return;
    }

    if (value.is_array()) {
        for (const auto& item : value) {
            if (item.is_string())
                append_resolved_flag(argv, flag, item.get<std::string>(), vars);
        }
    }
}

} // namespace

ClaudeDefaults load_claude_defaults(const std::string& working_dir)
{
    ClaudeDefaults out;
    fs::path config_dir;
    try {
        config_dir = media::settings::get_config_dir();
    } catch (...) {}

    (void)load_defaults_file(config_dir / kClaudeDefaultsFile, out);

    const fs::path cwd = resolve_working_dir(working_dir);
    if (!cwd.empty()) {
        ClaudeDefaults overlay;
        if (load_defaults_file(cwd / kClaudeDefaultsFile, overlay)) {
            merge_flags(out.flags, overlay.flags);
            for (const auto& src : overlay.sources) {
                if (src.is_string())
                    out.sources.push_back(src);
            }
        }
    }

    return out;
}

std::vector<std::string> resolve_claude_default_argv(const ClaudeDefaults& defs,
                                                     const Turn& turn,
                                                     const std::string& tanit_mcp_url)
{
    fs::path config_dir;
    try {
        config_dir = media::settings::get_config_dir();
    } catch (...) {}

    const fs::path cwd = resolve_working_dir(turn.folder_hint);
    fs::path mcp_path  = materialize_mcp_json_for_cli(config_dir, tanit_mcp_url);
    if (mcp_path.empty() && !cwd.empty())
        mcp_path = materialize_mcp_json_for_cli(cwd, tanit_mcp_url);

    const media::commands::VariableMap vars = build_claude_defaults_variable_map(turn, mcp_path);

    std::vector<std::string> resolved;
    if (defs.flags.is_object()) {
        resolved.reserve(defs.flags.size() * 2);
        for (auto it = defs.flags.begin(); it != defs.flags.end(); ++it)
            append_flag_value(resolved, it.key(), it.value(), vars);
    }
    return resolved;
}

std::string format_claude_argv_for_log(const std::vector<std::string>& argv, std::size_t max_len)
{
    std::string out;
    out.reserve(512);
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i > 0)
            out.push_back(' ');
        if (i > 0 && (argv[i - 1] == "-p" || argv[i - 1] == "--print")) {
            out += "[prompt:";
            out += std::to_string(argv[i].size());
            out += " chars]";
            continue;
        }
        const std::string& arg = argv[i];
        const bool quote = arg.find_first_of(" \t\"") != std::string::npos;
        if (quote)
            out.push_back('"');
        out += arg;
        if (quote)
            out.push_back('"');
        if (out.size() >= max_len)
            break;
    }
    if (out.size() > max_len) {
        out.resize(max_len);
        out += "...";
    }
    return out;
}

} // namespace media::llm::agent

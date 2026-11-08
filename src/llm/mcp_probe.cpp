#include "mcp_probe.hpp"

#include "mcp_config.hpp"

#include <fstream>
#include "fs/path_sanitizer.hpp"

#ifndef FEATURE_SECURE_STORAGE
#define FEATURE_SECURE_STORAGE 0
#endif

namespace media::llm::mcp {
namespace {

nlohmann::json minimal_config_echo(const nlohmann::json& srv)
{
    nlohmann::json e = nlohmann::json::object();
    if (srv.contains("type"))
        e["type"] = srv["type"];
    if (srv.contains("url"))
        e["url"] = srv["url"];
    if (srv.contains("command"))
        e["command"] = srv["command"];
    if (srv.contains("args"))
        e["args"] = srv["args"];
    if (srv.contains("headers") && srv["headers"].is_object()) {
        nlohmann::json header_names = nlohmann::json::object();
        for (auto it = srv["headers"].begin(); it != srv["headers"].end(); ++it) {
            if (it.value().is_string())
                header_names[it.key()] = "<set>";
        }
        e["headers"] = std::move(header_names);
    }
    return e;
}

std::string transport_label(const McpServerEndpoint& endpoint)
{
    if (endpoint.type == "stdio")
        return "stdio";
    if (!endpoint.url.empty())
        return "http";
    if (!endpoint.command.empty())
        return "stdio";
    return {};
}

} // namespace

nlohmann::json probe_mcp_config(const std::filesystem::path& profile_config_dir)
{
    namespace fs = std::filesystem;
    nlohmann::json out;

    const fs::path dir = resolve_mcp_config_dir(profile_config_dir);
    const fs::path mcp_path = mcp_json_path(profile_config_dir);
    out["mcp_config_uri"] = kMcpConfigUri;
#if FEATURE_SECURE_STORAGE
    out["mcp_json_path"] = "(plain mcp.json ignored; use mcp-store import-plain)";
#else
    out["mcp_json_path"] = dir.empty() ? std::string("(no profile dir)/mcp.json") : media::path::path_to_generic_utf8(mcp_path);
#endif
    out["profile_config_dir"] =
        dir.empty() ? std::string("(empty — could not resolve profile)") : media::path::path_to_generic_utf8(dir);

#if FEATURE_SECURE_STORAGE
    const bool exists = false;
#else
    std::error_code ec;
    const bool      exists = !dir.empty() && fs::is_regular_file(mcp_path, ec);
#endif
    out["exists"]          = exists;
    out["servers"]         = nlohmann::json::array();

    if (dir.empty()) {
        out["note"] = "Profile directory unknown; cannot locate mcp.json (use --config-dir or the platform profile).";
        return out;
    }

    nlohmann::json root;
    std::string load_err;
    if (!load_mcp_config(profile_config_dir, root, load_err)) {
#if FEATURE_SECURE_STORAGE
        if (load_err.rfind("document not found:", 0) == 0) {
            out["note"] = "No secure MCP config document — agent uses path tools only. Use mcp-store import-plain.";
        } else {
            out["read_error"] = "Could not load secure MCP config: " + load_err;
        }
#else
        if (load_err == "mcp.json not found") {
            out["note"] = "No mcp.json in profile dir — agent uses path tools only.";
        } else if (load_err == "could not open mcp.json for reading") {
            out["read_error"] = "Could not open mcp.json for reading";
        } else {
            out["parse_error"] = load_err;
        }
#endif
        return out;
    }

#if FEATURE_SECURE_STORAGE
    out["exists"] = true;
#endif

    for (auto it = root["mcpServers"].begin(); it != root["mcpServers"].end(); ++it) {
        const std::string server_name = it.key();
        nlohmann::json    one           = nlohmann::json::object();
        one["name"]                     = server_name;
        one["config"]                   = it.value().is_object() ? minimal_config_echo(it.value()) : it.value();

        if (!it.value().is_object()) {
            one["skipped"]        = true;
            one["skip_reason"]    = "value is not a JSON object (each mcpServers entry must be an object)";
            one["handshake_ok"]   = false;
            one["tools_list_ok"]  = false;
            one["tool_count"]     = 0;
            one["tools"]          = nlohmann::json::array();
            out["servers"].push_back(std::move(one));
            continue;
        }

        const auto& srv = it.value();
        if (srv.contains("enabled") && srv["enabled"].is_boolean() && !srv["enabled"].get<bool>()) {
            one["skipped"] = true;
            one["skip_reason"] = "disabled";
            one["handshake_ok"] = false;
            one["tools_list_ok"] = false;
            one["tool_count"] = 0;
            one["tools"] = nlohmann::json::array();
            out["servers"].push_back(std::move(one));
            continue;
        }

        McpServerEndpoint endpoint;
        parse_mcp_server_endpoint(srv, endpoint);

        McpSessionCreateOptions session_opts;
        session_opts.cancel_check = mcp_cancel_after_ms_from_env();
        std::string session_err;
        std::unique_ptr<IMcpSession> client = create_mcp_session(srv, server_name, session_err, session_opts);
        if (!client) {
            one["skipped"]        = true;
            one["skip_reason"]    = session_err.empty() ? "need non-empty url or command" : session_err;
            one["handshake_ok"]   = false;
            one["tools_list_ok"]  = false;
            one["tool_count"]     = 0;
            one["tools"]          = nlohmann::json::array();
            out["servers"].push_back(std::move(one));
            continue;
        }

        one["transport"] = transport_label(endpoint);

        std::string init_err;
        if (!client->initialize(init_err)) {
            one["handshake_ok"]  = false;
            one["handshake_error"] = init_err;
            one["tools_list_ok"] = false;
            one["tool_count"]    = 0;
            one["tools"]         = nlohmann::json::array();
            client->try_delete_session();
            out["servers"].push_back(std::move(one));
            continue;
        }
        one["handshake_ok"] = true;

        std::string list_err;
        nlohmann::json tools = client->tools_list_all(list_err);

        if (!list_err.empty()) {
            client->try_delete_session();
            one["tools_list_ok"]   = false;
            one["tools_list_error"] = list_err;
            one["tool_count"]      = 0;
            one["tools"]           = nlohmann::json::array();
            one["resources"]       = nlohmann::json::array();
            out["servers"].push_back(std::move(one));
            continue;
        }

        one["tools_list_ok"] = true;
        nlohmann::json tool_summaries = nlohmann::json::array();
        int            ntools         = 0;
        if (tools.is_array()) {
            for (const auto& tj : tools) {
                if (!tj.is_object() || !tj.contains("name") || !tj["name"].is_string())
                    continue;
                ++ntools;
                nlohmann::json row;
                row["name"]        = tj["name"].get<std::string>();
                row["description"] = tj.value("description", std::string());
                if (tj.contains("inputSchema") && tj["inputSchema"].is_object())
                    row["inputSchema"] = tj["inputSchema"];
                else
                    row["inputSchema"] = nlohmann::json::object();
                tool_summaries.push_back(std::move(row));
            }
        }
        one["tool_count"] = ntools;
        one["tools"]      = std::move(tool_summaries);

        std::string res_err;
        nlohmann::json raw_resources = client->resources_list_all(res_err);
        client->try_delete_session();

        nlohmann::json resource_summaries = nlohmann::json::array();
        if (raw_resources.is_array()) {
            for (const auto& rj : raw_resources) {
                if (!rj.is_object()) continue;
                nlohmann::json row = nlohmann::json::object();
                if (rj.contains("uri")         && rj["uri"].is_string())         row["uri"]         = rj["uri"];
                if (rj.contains("name")        && rj["name"].is_string())        row["name"]        = rj["name"];
                if (rj.contains("title")       && rj["title"].is_string())       row["title"]       = rj["title"];
                if (rj.contains("description") && rj["description"].is_string()) row["description"] = rj["description"];
                if (rj.contains("mimeType")    && rj["mimeType"].is_string())    row["mimeType"]    = rj["mimeType"];
                resource_summaries.push_back(std::move(row));
            }
        }
        one["resources"]       = std::move(resource_summaries);
        one["resource_count"]  = static_cast<int>(one["resources"].size());
        out["servers"].push_back(std::move(one));
    }

    out["note"] = "Per-server: handshake = JSON-RPC initialize + notifications/initialized; tools = tools/list. "
                  "Errors here match what the chat agent would see when loading mcp.json.";
    return out;
}

nlohmann::json probe_inline_mcp_server(const nlohmann::json& server_cfg,
                                        const std::string& server_key,
                                        int timeout_ms)
{
    nlohmann::json out;
    out["servers"] = nlohmann::json::array();

    nlohmann::json one = nlohmann::json::object();
    one["name"] = server_key;

    if (!server_cfg.is_object()) {
        one["skipped"]       = true;
        one["skip_reason"]   = "serverConfig is not a JSON object";
        one["handshake_ok"]  = false;
        one["tools_list_ok"] = false;
        one["tool_count"]    = 0;
        one["tools"]         = nlohmann::json::array();
        out["servers"].push_back(std::move(one));
        return out;
    }

    McpServerEndpoint endpoint;
    parse_mcp_server_endpoint(server_cfg, endpoint);

    McpSessionCreateOptions session_opts;
    // Use caller-supplied timeout or fall back to the default (60 s in production).
    // Never leave timeout_ms at 0: a hanging stdio server would stall the probe
    // thread indefinitely.  The env-var cancel check is additive — it fires
    // earlier when PM_MCP_CANCEL_AFTER_MS is set (test harnesses).
    session_opts.timeout_ms = timeout_ms > 0 ? timeout_ms : mcp_default_timeout_ms();
    session_opts.cancel_check = mcp_cancel_after_ms_from_env();
    std::string session_err;
    std::unique_ptr<IMcpSession> client = create_mcp_session(server_cfg, server_key, session_err, session_opts);
    if (!client) {
        one["skipped"]       = true;
        one["skip_reason"]   = session_err.empty() ? "need non-empty url or command" : session_err;
        one["handshake_ok"]  = false;
        one["tools_list_ok"] = false;
        one["tool_count"]    = 0;
        one["tools"]         = nlohmann::json::array();
        out["servers"].push_back(std::move(one));
        return out;
    }

    one["transport"] = transport_label(endpoint);

    std::string init_err;
    if (!client->initialize(init_err)) {
        one["handshake_ok"]    = false;
        one["handshake_error"] = init_err;
        one["tools_list_ok"]   = false;
        one["tool_count"]      = 0;
        one["tools"]           = nlohmann::json::array();
        client->try_delete_session();
        out["servers"].push_back(std::move(one));
        return out;
    }
    one["handshake_ok"] = true;

    std::string list_err;
    nlohmann::json tools = client->tools_list_all(list_err);
    client->try_delete_session();

    if (!list_err.empty()) {
        one["tools_list_ok"]   = false;
        one["tools_list_error"] = list_err;
        one["tool_count"]      = 0;
        one["tools"]           = nlohmann::json::array();
        out["servers"].push_back(std::move(one));
        return out;
    }

    one["tools_list_ok"] = true;
    nlohmann::json tool_summaries = nlohmann::json::array();
    int ntools = 0;
    if (tools.is_array()) {
        for (const auto& tj : tools) {
            if (!tj.is_object() || !tj.contains("name") || !tj["name"].is_string())
                continue;
            ++ntools;
            nlohmann::json row;
            row["name"]        = tj["name"].get<std::string>();
            row["description"] = tj.value("description", std::string{});
            if (tj.contains("inputSchema") && tj["inputSchema"].is_object())
                row["inputSchema"] = tj["inputSchema"];
            else
                row["inputSchema"] = nlohmann::json::object();
            tool_summaries.push_back(std::move(row));
        }
    }
    one["tool_count"] = ntools;
    one["tools"]      = std::move(tool_summaries);
    out["servers"].push_back(std::move(one));
    return out;
}

} // namespace media::llm::mcp

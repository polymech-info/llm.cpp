#include "llm/tools/mcp/McpBridge.hpp"

#include "core/cli_cancel.hpp"
#include "llm/mcp_config.hpp"

#include "logging/logger.h"

#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace media::llm::mcp {
namespace {

std::string openai_slug(std::string_view in)
{
    std::string o;
    o.reserve(in.size());
    for (unsigned char c : in) {
        if (std::isalnum(c))
            o += static_cast<char>(std::tolower(c));
        else if (c == '_' || c == '-')
            o += static_cast<char>(c);
        else
            o += '_';
    }
    if (o.empty())
        return "srv";
    return o;
}

std::string truncate_fn_name(std::string s)
{
    if (s.size() > 64)
        s.resize(64);
    return s;
}

bool schema_lists_type(const nlohmann::json& schema, const char* want)
{
    if (!schema.is_object() || !want)
        return false;
    if (schema.contains("type")) {
        const auto& t = schema["type"];
        if (t.is_string() && t.get<std::string>() == want)
            return true;
        if (t.is_array()) {
            for (const auto& item : t)
                if (item.is_string() && item.get<std::string>() == want)
                    return true;
        }
    }
    for (const char* key : {"anyOf", "oneOf", "allOf"}) {
        const auto it = schema.find(key);
        if (it == schema.end() || !it->is_array())
            continue;
        for (const auto& branch : *it)
            if (schema_lists_type(branch, want))
                return true;
    }
    return false;
}

const nlohmann::json* property_schema(const nlohmann::json& parameters, const std::string& key)
{
    if (!parameters.is_object())
        return nullptr;
    const auto props = parameters.find("properties");
    if (props != parameters.end() && props->is_object()) {
        const auto prop = props->find(key);
        if (prop != props->end() && prop->is_object())
            return &(*prop);
    }
    for (const char* combo : {"anyOf", "oneOf", "allOf"}) {
        const auto it = parameters.find(combo);
        if (it == parameters.end() || !it->is_array())
            continue;
        for (const auto& branch : *it) {
            if (const nlohmann::json* found = property_schema(branch, key))
                return found;
        }
    }
    return nullptr;
}

bool json_value_text(const std::string& s)
{
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
    if (i >= s.size())
        return false;
    const char c = s[i];
    if (c == '{' || c == '[')
        return true;
    return s.compare(i, 4, "null") == 0;
}

// Some models emit a nested object as a JSON string. MCP servers validate the
// declared type, so parse the string when the schema accepts that JSON type.
void coerce_stringified_json_args(nlohmann::json& arguments, const nlohmann::json& parameters)
{
    if (!arguments.is_object())
        return;
    for (auto it = arguments.begin(); it != arguments.end(); ++it) {
        try {
            const nlohmann::json* prop = property_schema(parameters, it.key());
            auto& value = it.value();
            if (value.is_string() && prop) {
                const std::string raw = value.get<std::string>();
                const bool want_object = schema_lists_type(*prop, "object");
                const bool want_array = schema_lists_type(*prop, "array");
                const bool want_null = schema_lists_type(*prop, "null");
                if ((want_object || want_array || want_null) && json_value_text(raw)) {
                    const nlohmann::json parsed = nlohmann::json::parse(raw);
                    if (parsed.is_object() && want_object)
                        value = parsed;
                    else if (parsed.is_array() && want_array)
                        value = parsed;
                    else if (parsed.is_null() && want_null)
                        value = nullptr;
                }
            }
            if (value.is_object() && prop)
                coerce_stringified_json_args(value, *prop);
        } catch (const std::exception& ex) {
            logger::error(std::string("MCP: argument \"") + it.key() + "\" JSON parse failed: " + ex.what());
        } catch (...) {
            logger::error(std::string("MCP: argument \"") + it.key() + "\" JSON parse failed");
        }
    }
}

int env_int_or_default(const char* name, int fallback)
{
    const char* env = std::getenv(name);
    if (!env || !*env)
        return fallback;
    try {
        const int v = std::stoi(env);
        return v > 0 ? v : fallback;
    } catch (...) {
        return fallback;
    }
}

bool path_is_within_or_equal(const std::filesystem::path& child,
                             const std::filesystem::path& parent)
{
    auto c = child.begin();
    auto p = parent.begin();
    for (; p != parent.end(); ++p, ++c) {
        if (c == child.end() || *c != *p)
            return false;
    }
    return true;
}

bool sandbox_grants_write_path(const nlohmann::json& session_info,
                               const std::filesystem::path& requested)
{
    if (!session_info.contains("granted_paths") || !session_info["granted_paths"].is_array())
        return false;
    std::error_code ec;
    const auto want = std::filesystem::weakly_canonical(requested, ec);
    std::error_code aec;
    const auto normalized = ec ? std::filesystem::absolute(requested, aec) : want;
    for (const auto& grant : session_info["granted_paths"]) {
        if (!grant.is_object())
            continue;
        const std::string access = grant.value("access", std::string{});
        if (access != "write" && access != "read-write")
            continue;
        const std::string path = grant.value("path", std::string{});
        if (path.empty())
            continue;
        std::error_code gec;
        const auto base = std::filesystem::weakly_canonical(path, gec);
        std::error_code baec;
        const auto normalized_base = gec ? std::filesystem::absolute(path, baec) : base;
        if (!baec && path_is_within_or_equal(normalized, normalized_base))
            return true;
    }
    return false;
}

nlohmann::json sandbox_denied_write_envelope(const std::string& server_key,
                                             const std::string& remote_tool_name,
                                             const std::string& path,
                                             const nlohmann::json& session_info)
{
    nlohmann::json envelope{
        {"ok", false},
        {"security_denied", true},
        {"error", "MCP stdio sandbox denied write outside granted paths"},
        {"mcp", {
            {"server", server_key},
            {"tool", remote_tool_name},
            {"sandboxed", true},
        }},
        {"privilegeEscalationRequest", {
            {"tool", remote_tool_name},
            {"action", "write"},
            {"path", path},
            {"reason", "sandboxed MCP server requested write outside granted paths"},
            {"callSite", "AgentMcpBridge::call_tool"},
            {"suggestedFix", "Grant the path to the AppContainer-backed MCP server or choose a path inside the granted workspace."},
        }},
    };
    if (session_info.contains("sandbox_backend"))
        envelope["mcp"]["sandbox_backend"] = session_info["sandbox_backend"];
    if (session_info.contains("containerSid")) {
        envelope["mcp"]["containerSid"] = session_info["containerSid"];
        envelope["privilegeEscalationRequest"]["containerSid"] = session_info["containerSid"];
    }
    return envelope;
}

std::string utf8_prefix_bytes(const std::string& s, std::size_t max_bytes)
{
    if (s.size() <= max_bytes)
        return s;
    std::size_t n = max_bytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
        --n;
    if (n == 0)
        n = max_bytes;
    return s.substr(0, n);
}

bool looks_like_json_text(const std::string& s)
{
    for (unsigned char c : s) {
        if (std::isspace(c))
            continue;
        return c == '{' || c == '[';
    }
    return false;
}

void compact_mcp_json(nlohmann::json& value, int string_limit, int raw_content_limit, int array_limit);

std::string compact_json_text_string(const std::string& s, int string_limit, int raw_content_limit, int array_limit)
{
    if (!looks_like_json_text(s))
        return {};
    try {
        nlohmann::json parsed = nlohmann::json::parse(s);
        compact_mcp_json(parsed, string_limit, raw_content_limit, array_limit);
        return parsed.dump();
    } catch (...) {
        return {};
    }
}

void compact_mcp_json(nlohmann::json& value, int string_limit, int raw_content_limit, int array_limit)
{
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            const std::string key = it.key();
            if (it.value().is_string()) {
                std::string s = it.value().get<std::string>();
                if (const std::string compacted = compact_json_text_string(s, string_limit, raw_content_limit, array_limit);
                        !compacted.empty()) {
                    s = compacted;
                }
                const int limit = (key == "raw_content" || key == "rawContent" || key == "html")
                    ? raw_content_limit
                    : string_limit;
                if (static_cast<int>(s.size()) > limit) {
                    const std::size_t original = s.size();
                    s = utf8_prefix_bytes(s, static_cast<std::size_t>(limit));
                    s += "\n...[truncated " + std::to_string(original - s.size()) + " bytes from MCP result]";
                }
                it.value() = std::move(s);
            } else {
                compact_mcp_json(it.value(), string_limit, raw_content_limit, array_limit);
            }
        }
        return;
    }
    if (value.is_array()) {
        for (auto& item : value)
            compact_mcp_json(item, string_limit, raw_content_limit, array_limit);
        if (array_limit > 0 && static_cast<int>(value.size()) > array_limit) {
            const auto original = value.size();
            nlohmann::json clipped = nlohmann::json::array();
            for (int i = 0; i < array_limit; ++i)
                clipped.push_back(std::move(value[static_cast<std::size_t>(i)]));
            clipped.push_back(nlohmann::json{
                {"truncated", true},
                {"omitted_items", original - static_cast<std::size_t>(array_limit)}
            });
            value = std::move(clipped);
        }
        return;
    }
    if (value.is_string()) {
        std::string s = value.get<std::string>();
        if (const std::string compacted = compact_json_text_string(s, string_limit, raw_content_limit, array_limit);
                !compacted.empty()) {
            s = compacted;
        }
        if (static_cast<int>(s.size()) > string_limit) {
            const std::size_t original = s.size();
            s = utf8_prefix_bytes(s, static_cast<std::size_t>(string_limit));
            s += "\n...[truncated " + std::to_string(original - s.size()) + " bytes from MCP result]";
        }
        value = std::move(s);
    }
}

nlohmann::json compact_mcp_result_for_model(nlohmann::json result)
{
    const int string_limit = env_int_or_default("PM_MCP_RESULT_STRING_LIMIT", 4000);
    const int raw_content_limit = env_int_or_default("PM_MCP_RESULT_RAW_CONTENT_LIMIT", 1500);
    const int array_limit = env_int_or_default("PM_MCP_RESULT_ARRAY_LIMIT", 12);
    const int json_limit = env_int_or_default("PM_MCP_RESULT_JSON_LIMIT", 50000);

    compact_mcp_json(result, string_limit, raw_content_limit, array_limit);
    if (static_cast<int>(result.dump().size()) > json_limit) {
        compact_mcp_json(result, 1200, 600, 6);
        std::string dumped = result.dump();
        if (static_cast<int>(dumped.size()) > json_limit) {
            dumped = utf8_prefix_bytes(dumped, static_cast<std::size_t>(json_limit));
            result = nlohmann::json{
                {"truncated", true},
                {"note", "MCP result exceeded PM_MCP_RESULT_JSON_LIMIT after compaction"},
                {"preview_json", dumped}
            };
        }
    }
    return result;
}

std::string make_prefixed_tool_name(const std::string& server_key, const std::string& remote_tool,
                                    const std::unordered_map<std::string, RegisteredMcpTool>& existing)
{
    std::string base = truncate_fn_name("mcp_" + openai_slug(server_key) + "__" + openai_slug(remote_tool));
    if (!existing.count(base))
        return base;
    for (int i = 2; i < 1000; ++i) {
        const std::string suffix = "_" + std::to_string(i);
        std::string       cand   = base;
        if (cand.size() + suffix.size() > 64)
            cand.resize(64 - suffix.size());
        cand += suffix;
        if (!existing.count(cand))
            return cand;
    }
    return base + "_x";
}

} // namespace

std::unique_ptr<AgentMcpBridge> AgentMcpBridge::try_create(const std::filesystem::path& profile_config_dir,
                                                          nlohmann::json&                inout_openai_tools,
                                                          std::string&                   warnings_out,
                                                          std::function<bool()>          cancel_check)
{
    warnings_out.clear();
    namespace fs = std::filesystem;
    auto effective_cancel_check = [cancel_check = std::move(cancel_check)] {
        if (media::cli::cancel_requested())
            return true;
        if (cancel_check) {
            try { return cancel_check(); }
            catch (...) { return false; }
        }
        return false;
    };

    fs::path dir = profile_config_dir;
    if (dir.empty())
        dir = resolve_mcp_config_dir();
    if (dir.empty()) {
        warnings_out = "MCP: no profile config directory (skip mcp.json)\n";
        return nullptr;
    }

    nlohmann::json root;
    std::string load_err;
    if (!load_mcp_config(profile_config_dir, root, load_err)) {
        if (!load_err.empty())
            warnings_out = "MCP: " + load_err + "\n";
        return nullptr;
    }

    // Schemas from process cache, then disk (stale ok if fingerprint matches).
    // Live handshake only on miss. Never tools/list per turn.
    std::string probe_note;
    const nlohmann::json probe = load_or_probe_mcp_tools(
        /*allow_probe=*/true, probe_note, dir, /*allow_stale=*/true);
    if (probe.value("skipped", false)) {
        warnings_out = "MCP: tool catalog unavailable (" +
                       (probe_note.empty() ? std::string("probe skipped") : probe_note) + ")\n";
        return nullptr;
    }

    std::unordered_map<std::string, const nlohmann::json*> probe_by_name;
    if (probe.contains("servers") && probe["servers"].is_array()) {
        for (const auto& s : probe["servers"]) {
            if (!s.is_object() || !s.contains("name") || !s["name"].is_string())
                continue;
            probe_by_name[s["name"].get<std::string>()] = &s;
        }
    }

    auto bridge = std::unique_ptr<AgentMcpBridge>(new AgentMcpBridge());
    bridge->cancel_check_ = std::move(effective_cancel_check);

    if (!inout_openai_tools.is_array())
        inout_openai_tools = nlohmann::json::array();

    const bool require_registered = require_registered_mcp(root);
    std::size_t tools_registered = 0;

    for (auto it = root["mcpServers"].begin(); it != root["mcpServers"].end(); ++it) {
        const std::string server_key = it.key();
        const auto&       srv        = it.value();
        if (!srv.is_object()) {
            warnings_out += "MCP: server \"" + server_key + "\" skipped (not an object)\n";
            continue;
        }
        if (srv.contains("enabled") && srv["enabled"].is_boolean() && !srv["enabled"].get<bool>()) {
            logger::trace("MCP: server \"" + server_key + "\" skipped (disabled)");
            continue;
        }

        McpServerEndpoint endpoint;
        parse_mcp_server_endpoint(srv, endpoint);

        {
            std::string policy_warning;
            if (!mcp_server_policy_allows(srv, server_key, endpoint.command, require_registered, policy_warning)) {
                warnings_out += policy_warning;
                continue;
            }
        }

        const auto pit = probe_by_name.find(server_key);
        if (pit == probe_by_name.end()) {
            warnings_out += "MCP: server \"" + server_key + "\" skipped (not in tools cache/probe)\n";
            continue;
        }
        const nlohmann::json& probed = *pit->second;
        if (probed.value("skipped", false) || !probed.value("tools_list_ok", false)) {
            const std::string why = probed.value("skip_reason",
                probed.value("tools_list_error",
                    probed.value("handshake_error", std::string("tools/list unavailable"))));
            warnings_out += "MCP: server \"" + server_key + "\" skipped (" + why + ")\n";
            continue;
        }
        const auto& tools = probed["tools"];
        if (!tools.is_array() || tools.empty()) {
            warnings_out += "MCP: server \"" + server_key + "\" returned no tools\n";
            continue;
        }

        const size_t server_index = bridge->servers_.size();
        ServerSlot slot;
        slot.server_key = server_key;
        slot.config     = srv;
        bridge->servers_.push_back(std::move(slot));

        std::size_t n = 0;
        for (const auto& tj : tools) {
            if (!tj.is_object() || !tj.contains("name") || !tj["name"].is_string())
                continue;
            const std::string remote_name = tj["name"].get<std::string>();
            const std::string openai_name = make_prefixed_tool_name(server_key, remote_name, bridge->registry_);
            const std::string desc        = tj.value("description", std::string());

            RegisteredMcpTool reg;
            reg.server_index     = server_index;
            reg.server_key       = server_key;
            reg.remote_tool_name = remote_name;
            reg.parameters       = parameters_from_mcp_tool(tj);
            bridge->registry_.emplace(openai_name, std::move(reg));

            inout_openai_tools.push_back(nlohmann::json{
                {"type", "function"},
                {"function",
                 {{"name", openai_name},
                  {"description", std::string("[MCP ") + server_key + "] " + desc},
                  {"parameters", bridge->registry_.at(openai_name).parameters}}}});
            ++n;
        }
        tools_registered += n;

        std::string log_where = !endpoint.url.empty() ? endpoint.url : ("stdio:" + endpoint.command);
        logger::debug(std::string("MCP: catalog server \"") + server_key + "\" via " + log_where
                     + " tools=" + std::to_string(n)
                     + " (" + (probe_note.empty() ? "cache/probe" : probe_note) + ")");
    }

    if (bridge->registry_.empty()) {
        bridge.reset();
        if (warnings_out.empty())
            warnings_out = "MCP: no usable servers in mcp.json\n";
        return nullptr;
    }
    logger::debug(std::string("MCP: agent catalog ready tools=") + std::to_string(tools_registered)
                  + " servers=" + std::to_string(bridge->servers_.size())
                  + " — " + probe_note);
    return bridge;
}

AgentMcpBridge::~AgentMcpBridge()
{
    for (auto& s : servers_) {
        if (s.client)
            s.client->try_delete_session();
    }
}

bool AgentMcpBridge::is_mcp_openai_function(const std::string& name) const
{
    return registry_.count(name) != 0;
}

bool AgentMcpBridge::ensure_client(size_t server_index, std::string& err)
{
    err.clear();
    if (server_index >= servers_.size()) {
        err = "MCP server index out of range";
        return false;
    }
    ServerSlot& slot = servers_[server_index];
    if (slot.client)
        return true;

    McpSessionCreateOptions session_opts;
    session_opts.apply_persisted_sandbox = true;
    session_opts.cancel_check = cancel_check_;
    std::string session_err;
    auto client = create_mcp_session(slot.config, slot.server_key, session_err, session_opts);
    if (!client) {
        err = session_err.empty() ? "need non-empty \"url\" or \"command\"" : session_err;
        return false;
    }
    std::string init_err;
    if (!client->initialize(init_err)) {
        err = init_err.empty() ? "initialize failed" : init_err;
        client->try_delete_session();
        return false;
    }
    slot.client = std::move(client);
    logger::info(std::string("MCP: lazy-connected server \"") + slot.server_key + "\" for tools/call");
    return true;
}

bool AgentMcpBridge::call_tool(const std::string& openai_function_name, const nlohmann::json& arguments,
                               std::string& result_json, std::string& err)
{
    err.clear();
    result_json.clear();
    const auto it = registry_.find(openai_function_name);
    if (it == registry_.end()) {
        err = "unknown MCP tool binding";
        return false;
    }
    if (!ensure_client(it->second.server_index, err))
        return false;

    IMcpSession* client = servers_[it->second.server_index].client.get();
    if (!client) {
        err = "MCP client missing after connect";
        return false;
    }

    nlohmann::json args = arguments;
    if (args.is_object())
        coerce_stringified_json_args(args, it->second.parameters);

    std::string              call_err;
    const nlohmann::json      session_info = client->session_info();
    if (session_info.value("sandboxed", false) &&
        it->second.remote_tool_name == "write_probe" &&
        args.contains("path") && args["path"].is_string()) {
        const std::string path = args["path"].get<std::string>();
        if (!sandbox_grants_write_path(session_info, path)) {
            result_json = sandbox_denied_write_envelope(
                it->second.server_key, it->second.remote_tool_name, path, session_info).dump();
            return true;
        }
    }

    const nlohmann::json tr = client->tools_call(it->second.remote_tool_name, args, call_err);
    nlohmann::json            envelope;
    envelope["ok"]             = call_err.empty();
    envelope["mcp"]["server"] = it->second.server_key;
    envelope["mcp"]["tool"]   = it->second.remote_tool_name;
    if (!session_info.empty()) {
        if (session_info.contains("pid"))
            envelope["mcp"]["pid"] = session_info["pid"];
        if (session_info.contains("sandboxed")) {
            envelope["mcp"]["sandboxed"] = session_info["sandboxed"];
            envelope["sandboxed"] = session_info["sandboxed"];
        }
        if (session_info.contains("sandbox_backend")) {
            envelope["mcp"]["sandbox_backend"] = session_info["sandbox_backend"];
            envelope["sandbox_backend"] = session_info["sandbox_backend"];
        }
        if (session_info.contains("containerSid")) {
            envelope["mcp"]["containerSid"] = session_info["containerSid"];
            envelope["containerSid"] = session_info["containerSid"];
        }
    }
    if (!call_err.empty()) {
        envelope["error"] = call_err;
        result_json       = envelope.dump();
        return true;
    }
    envelope["mcp"]["result"] = compact_mcp_result_for_model(tr);
    result_json               = envelope.dump();
    return true;
}

} // namespace media::llm::mcp

#include "llm/agent_tool_provider.hpp"

#include "logging/logger.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <mutex>
#include <sstream>
#include <utility>

namespace media::llm::agent {
namespace {

struct ParsedHttpUrl {
    std::string host;
    std::string path = "/";
    std::string query;
    std::string session;
    int         port = 80;
    bool        ok   = false;
};

std::string trim_copy(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

bool host_is_loopback(std::string_view host)
{
    if (host == "127.0.0.1" || host == "localhost" || host == "::1")
        return true;
    if (host.size() >= 4 && host.substr(0, 4) == "127.")
        return true;
    return false;
}

ParsedHttpUrl parse_http_url(std::string url)
{
    ParsedHttpUrl out;
    url = trim_copy(std::move(url));
    if (url.rfind("http://", 0) != 0)
        return out;
    url.erase(0, 7);
    const auto slash = url.find('/');
    const std::string hostport = slash == std::string::npos ? url : url.substr(0, slash);
    out.path                   = slash == std::string::npos ? "/" : url.substr(slash);
    if (out.path.empty())
        out.path = "/";
    if (!hostport.empty() && hostport.front() == '[') {
        const auto rb = hostport.find(']');
        if (rb == std::string::npos)
            return out;
        out.host = hostport.substr(1, rb - 1);
        if (rb + 1 < hostport.size() && hostport[rb + 1] == ':') {
            try {
                out.port = std::stoi(hostport.substr(rb + 2));
            } catch (...) {
                return out;
            }
        }
    } else {
        const auto colon = hostport.rfind(':');
        if (colon != std::string::npos) {
            out.host = hostport.substr(0, colon);
            try {
                out.port = std::stoi(hostport.substr(colon + 1));
            } catch (...) {
                return out;
            }
        } else {
            out.host = hostport;
        }
    }
    if (out.host.empty() || out.port <= 0 || out.port > 65535)
        return out;
    if (!host_is_loopback(out.host))
        return out;
    const auto qmark = out.path.find('?');
    if (qmark != std::string::npos) {
        out.query = out.path.substr(qmark + 1);
        out.path.resize(qmark);
        if (out.path.empty())
            out.path = "/";
    }
    std::string kept;
    std::size_t at = 0;
    while (at < out.query.size()) {
        const auto amp = out.query.find('&', at);
        const std::string part = out.query.substr(at, amp == std::string::npos ? std::string::npos : amp - at);
        if (part.rfind("token=", 0) == 0)
            out.session = part.substr(6);
        else if (!part.empty()) {
            if (!kept.empty())
                kept.push_back('&');
            kept += part;
        }
        if (amp == std::string::npos)
            break;
        at = amp + 1;
    }
    out.query = std::move(kept);
    out.ok = true;
    return out;
}

httplib::Headers session_headers(const ParsedHttpUrl& parsed)
{
    httplib::Headers headers;
    if (!parsed.session.empty())
        headers.emplace("X-Tanit-Session", parsed.session);
    return headers;
}

std::string with_query(std::string path, const std::string& query)
{
    if (query.empty())
        return path;
    return path + "?" + query;
}

std::string join_url_path(std::string base, std::string_view suffix)
{
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    if (suffix.empty())
        return base.empty() ? "/" : base;
    if (suffix.front() != '/')
        return base + "/" + std::string(suffix);
    return base + std::string(suffix);
}

class HttpHostToolProvider final : public IAgentToolProvider {
public:
    HttpHostToolProvider(std::string id, std::string base_url)
        : id_(std::move(id)), base_url_(std::move(base_url))
    {
        parsed_ = parse_http_url(base_url_);
        if (parsed_.ok)
            base_url_ = "http://" + parsed_.host + ":" + std::to_string(parsed_.port) + parsed_.path;
        if (!parsed_.ok) {
            std::string shown = base_url_;
            const auto qmark = shown.find('?');
            if (qmark != std::string::npos)
                shown.resize(qmark);
            logger::warn(std::string("host tool provider '") + id_
                         + "': rejected URL (loopback http only): " + shown);
        }
    }

    std::string id() const override { return id_; }

    std::string name() const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        return display_name_.empty() ? id_ : display_name_;
    }

    std::vector<ToolDefinition> tools() const override
    {
        refresh_catalog(false);
        std::lock_guard<std::mutex> lock(mu_);
        return tools_;
    }

    std::string context_prompt(const ToolContext& ctx) const override
    {
        if (!parsed_.ok)
            return {};
        const std::string path = join_url_path(parsed_.path, "/context");
        httplib::Client cli(parsed_.host, parsed_.port);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(8, 0);
        cli.set_write_timeout(4, 0);
        (void)ctx;
        const auto res = cli.Get(with_query(path, parsed_.query).c_str(), session_headers(parsed_));
        if (!res || res->status != 200) {
            logger::warn(std::string("host tool provider '") + id_
                         + "': context GET " + path + " failed"
                         + (res ? (" HTTP " + std::to_string(res->status)) : " (no response)"));
            return {};
        }
        try {
            const auto j = nlohmann::json::parse(res->body);
            if (j.is_object()) {
                if (j.contains("prompt") && j["prompt"].is_string())
                    return j["prompt"].get<std::string>();
                if (j.contains("context") && j["context"].is_string())
                    return j["context"].get<std::string>();
            }
        } catch (...) {
        }
        return res->body;
    }

    ToolResult invoke(std::string_view      tool_name,
                      const nlohmann::json& arguments,
                      const ToolContext&    context) override
    {
        if (!parsed_.ok)
            return ToolResult::error("host tool provider URL is not loopback http");
        nlohmann::json body{
            {"name", std::string(tool_name)},
            {"arguments", arguments.is_object() ? arguments : nlohmann::json::object()},
            {"context",
             {{"session_id", context.session_id},
              {"cwd", context.cwd},
              {"extra", context.extra.is_object() ? context.extra : nlohmann::json::object()}}},
        };
        httplib::Client cli(parsed_.host, parsed_.port);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(60, 0);
        cli.set_write_timeout(10, 0);
        const std::string path = join_url_path(parsed_.path, "/call");
        const auto        res = cli.Post(with_query(path, parsed_.query).c_str(), session_headers(parsed_),
                                         body.dump(), "application/json");
        if (!res)
            return ToolResult::error(std::string("host tool HTTP failed: ") + id_);
        if (res->status < 200 || res->status >= 300) {
            std::ostringstream msg;
            msg << "host tool HTTP " << res->status;
            if (!res->body.empty())
                msg << ": " << res->body.substr(0, 400);
            return ToolResult::error(msg.str());
        }
        try {
            return ToolResult::from_envelope(nlohmann::json::parse(res->body));
        } catch (...) {
            return ToolResult::error("host tool returned non-JSON");
        }
    }

private:
    void refresh_catalog(bool force) const
    {
        if (!parsed_.ok)
            return;
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!force && catalog_ok_
                && now - catalog_at_ < std::chrono::seconds(5))
                return;
        }
        httplib::Client cli(parsed_.host, parsed_.port);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(8, 0);
        cli.set_write_timeout(4, 0);
        const auto res = cli.Get(with_query(parsed_.path, parsed_.query).c_str(), session_headers(parsed_));
        if (!res || res->status != 200) {
            logger::warn(std::string("host tool provider '") + id_
                         + "': catalog GET failed");
            return;
        }
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(res->body);
        } catch (...) {
            logger::warn(std::string("host tool provider '") + id_
                         + "': catalog is not JSON");
            return;
        }
        std::vector<ToolDefinition> next;
        std::string                 display = id_;
        if (j.is_object()) {
            if (j.contains("name") && j["name"].is_string()) {
                const std::string n = j["name"].get<std::string>();
                if (!n.empty())
                    display = n;
            }
            const nlohmann::json* tools = nullptr;
            if (j.contains("tools") && j["tools"].is_array())
                tools = &j["tools"];
            if (tools) {
                for (const auto& row : *tools) {
                    if (!row.is_object() || !row.contains("name") || !row["name"].is_string())
                        continue;
                    ToolDefinition def;
                    def.name = row["name"].get<std::string>();
                    if (def.name.empty())
                        continue;
                    if (row.contains("description") && row["description"].is_string())
                        def.description = row["description"].get<std::string>();
                    if (row.contains("parameters") && row["parameters"].is_object())
                        def.parameters = row["parameters"];
                    else
                        def.parameters = nlohmann::json{{"type", "object"},
                                                        {"properties", nlohmann::json::object()}};
                    next.push_back(std::move(def));
                }
            }
        }
        std::lock_guard<std::mutex> lock(mu_);
        tools_       = std::move(next);
        display_name_ = std::move(display);
        catalog_ok_  = true;
        catalog_at_  = now;
    }

    std::string   id_;
    std::string   base_url_;
    ParsedHttpUrl parsed_;

    mutable std::mutex                   mu_;
    mutable std::vector<ToolDefinition>  tools_;
    mutable std::string                  display_name_;
    mutable bool                         catalog_ok_ = false;
    mutable std::chrono::steady_clock::time_point catalog_at_{};
};

} // namespace

std::shared_ptr<IAgentToolProvider> make_http_host_tool_provider(std::string id,
                                                                std::string base_url)
{
    id       = trim_copy(std::move(id));
    base_url = trim_copy(std::move(base_url));
    if (id.empty() || base_url.empty())
        return nullptr;
    return std::make_shared<HttpHostToolProvider>(std::move(id), std::move(base_url));
}

bool register_http_host_tool_provider(std::string id, std::string url, std::string* err)
{
    auto provider = make_http_host_tool_provider(std::move(id), std::move(url));
    if (!provider) {
        if (err)
            *err = "invalid host tool provider (need id + loopback http URL)";
        return false;
    }
    tool_providers::add(std::move(provider));
    return true;
}

nlohmann::json list_registered_host_tool_providers()
{
    nlohmann::json rows = nlohmann::json::array();
    for (const auto& p : tool_providers::list()) {
        if (!p)
            continue;
        rows.push_back({{"id", p->id()}, {"name", p->name()}});
    }
    return {{"providers", std::move(rows)}};
}

} // namespace media::llm::agent

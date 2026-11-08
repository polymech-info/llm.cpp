#pragma once

#include <nlohmann/json.hpp>
#include <functional>
#include <string>

namespace media::llm::mcp {

/** Common surface for Streamable HTTP and stdio MCP transports (initialize → tools/list → tools/call). */
struct IMcpSession {
    virtual ~IMcpSession() = default;

    virtual void set_request_options(int timeout_ms, std::function<bool()> cancel_check)
    {
        (void)timeout_ms;
        (void)cancel_check;
    }

    virtual bool initialize(std::string& err) = 0;
    virtual void try_delete_session()         = 0;
    virtual nlohmann::json tools_list_all(std::string& err) = 0;
    virtual nlohmann::json tools_call(const std::string& remote_tool_name, const nlohmann::json& arguments,
                                      std::string& err) = 0;

    /// Transport/process metadata surfaced by brokers. Empty for transports
    /// without a local child process (for example HTTP).
    virtual nlohmann::json session_info() const
    {
        return nlohmann::json::object();
    }

    /** Fetch resources/list (paginated). Returns array of resource objects or empty array on error/unsupported. */
    virtual nlohmann::json resources_list_all(std::string& /*err*/)
    {
        return nlohmann::json::array();
    }
};

} // namespace media::llm::mcp

#pragma once

#include "mcp_session.hpp"

#include <nlohmann/json.hpp>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace media::llm::mcp {

/**
 * Minimal MCP client for Streamable HTTP (JSON-RPC over POST), matching the flow used by
  * (pm_image_mcp_streamable.cpp): initialize → notifications/initialized → tools/list → tools/call.
 */
class StreamHttpClient final : public IMcpSession {
public:
    explicit StreamHttpClient(std::string mcp_endpoint_url);
    StreamHttpClient(std::string mcp_endpoint_url, std::map<std::string, std::string> headers);
    ~StreamHttpClient();

    StreamHttpClient(const StreamHttpClient&)            = delete;
    StreamHttpClient& operator=(const StreamHttpClient&) = delete;

    bool initialize(std::string& err) override;
    /** Optional explicit session end (MCP HTTP DELETE). */
    void try_delete_session() override;
    void set_request_options(int timeout_ms, std::function<bool()> cancel_check) override;

    nlohmann::json tools_list_all(std::string& err) override;
    nlohmann::json resources_list_all(std::string& err) override;
    nlohmann::json tools_call(const std::string& remote_tool_name, const nlohmann::json& arguments,
                              std::string& err) override;

private:
    struct HttpPostResult {
        long          http_status = 0;
        std::string   content_type;
        std::string   body;
        std::string   session_header_out;   // from response (initialize)
        std::string   protocol_header_out; // optional
    };

    HttpPostResult http_post(const std::string& json_body, std::string& err);
    static nlohmann::json parse_jsonrpc_result_or_error(const nlohmann::json& envelope, std::string& err);
    static nlohmann::json parse_sse_data_json(const std::string& body, std::string& err);

    std::string mcp_endpoint_url_;
    std::map<std::string, std::string> headers_;
    std::string session_id_;
    std::string protocol_version_;
    int         next_jsonrpc_id_ = 1;
    int         timeout_ms_ = 60000;
    std::function<bool()> cancel_check_;
    std::mutex  send_mu_;
};

} // namespace media::llm::mcp

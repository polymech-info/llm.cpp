// Copyright PolyMech / pm-image. Internal use only.
//
// Moved here from src/llm/agent_mcp_bridge.{hpp,cpp}. Same `media::llm::mcp`
// namespace + `AgentMcpBridge` class — only the file location and include
// path changed. The supporting MCP transport implementations
// (mcp_session.hpp, mcp_stdio_client.{hpp,cpp}, mcp_stream_client.{hpp,cpp},
// mcp_probe.{hpp,cpp}) still live in src/llm/ so they can be reused outside
// the agent context (e.g. CLI `mcp probe`).
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#ifndef FEATURE_HOME_LLM_TOOLS
#define FEATURE_HOME_LLM_TOOLS 1
#endif
#ifndef FEATURE_MCP_CLIENT
#define FEATURE_MCP_CLIENT 1
#endif

#if FEATURE_HOME_LLM_TOOLS && FEATURE_MCP_CLIENT
#include "llm/mcp_session.hpp"
#include <unordered_map>
#include <vector>
#endif

namespace media::llm::mcp {

#if FEATURE_HOME_LLM_TOOLS && FEATURE_MCP_CLIENT
struct RegisteredMcpTool {
    size_t              server_index = 0;
    std::string         server_key;
    std::string         remote_tool_name;
    nlohmann::json      parameters = nlohmann::json::object();
};

/**
 * Loads `%AppData%/.../pm-image/mcp.json` (same profile root as settings / system-prompt.md on Windows;
 * macOS/Linux/Unix: same folder as portable `settings.json` (e.g. `~/Library/…/pm-image`, `~/.pm-image`,
 * or `--config-dir`). Connects to each `mcpServers` entry:
 * **Streamable HTTP** (`url`, e.g. Cursor) or **stdio** (`type: "stdio"`, `command` + `args`, e.g.
 * [OfficeMCP](https://github.com/officemcp/officemcp) with `uvx officemcp`). Tools are merged into the
 * OpenAI `tools` array as `mcp_<server>__<tool>` (sanitized).
 *
 * Tool *schemas* come from `mcp-tools-cache.json` (8h TTL via load_or_probe_mcp_tools).
 * Live initialize / tools/list is not done on every agent turn — sessions are
 * opened lazily on the first tools/call for that server.
 */
class AgentMcpBridge {
public:
    static std::unique_ptr<AgentMcpBridge> try_create(const std::filesystem::path& profile_config_dir,
                                                      nlohmann::json&                inout_openai_tools,
                                                      std::string&                   warnings_out,
                                                      std::function<bool()>          cancel_check = {});

    ~AgentMcpBridge();

    AgentMcpBridge(const AgentMcpBridge&)            = delete;
    AgentMcpBridge& operator=(const AgentMcpBridge&) = delete;

    bool empty() const { return registry_.empty(); }

    bool is_mcp_openai_function(const std::string& name) const;

    /** On success sets @p result_json to a JSON string suitable for a `tool` message. */
    bool call_tool(const std::string& openai_function_name, const nlohmann::json& arguments, std::string& result_json,
                   std::string& err);

private:
    struct ServerSlot {
        std::string                  server_key;
        nlohmann::json               config;
        std::unique_ptr<IMcpSession> client;
    };

    AgentMcpBridge() = default;

    bool ensure_client(size_t server_index, std::string& err);

    std::vector<ServerSlot>                            servers_;
    std::unordered_map<std::string, RegisteredMcpTool> registry_;
    std::function<bool()>                              cancel_check_;
};
#else
class AgentMcpBridge {
public:
    static std::unique_ptr<AgentMcpBridge> try_create(const std::filesystem::path&,
                                                      nlohmann::json&,
                                                      std::string& warnings_out,
                                                      std::function<bool()> = {})
    {
        warnings_out.clear();
        return nullptr;
    }

    bool empty() const { return true; }
    bool is_mcp_openai_function(const std::string&) const { return false; }
    bool call_tool(const std::string&, const nlohmann::json&, std::string&, std::string& err)
    {
        err = "Agent MCP bridge disabled (requires FEATURE_HOME_LLM_TOOLS and FEATURE_MCP_CLIENT).";
        return false;
    }
};
#endif

} // namespace media::llm::mcp

#pragma once

#include <functional>
#include <string>

namespace media::llm::agent {

struct TanitMcpListenInfo {
    bool listening = false;
    int port = 0;
    std::string bind_host;
    std::string url;
};

using TanitMcpEnsureFn = int (*)();
using TanitMcpStatusFn = TanitMcpListenInfo (*)();
using TanitMcpStopFn = bool (*)();

/// Host (exe/CLI) registers the in-process MCP listener. The chat plugin
/// must not link `mcp_embed` — it only calls these hooks.
void register_tanit_mcp_host(TanitMcpEnsureFn ensure,
                             TanitMcpStatusFn status,
                             TanitMcpStopFn stop);

void register_tanit_mcp_ensure(TanitMcpEnsureFn ensure);

int ensure_tanit_mcp_listen_port();
TanitMcpListenInfo tanit_mcp_listen_info();
bool stop_tanit_mcp_listener();

void set_tanit_mcp_activity_changed(std::function<void()> cb);
void notify_tanit_mcp_activity_changed();

} // namespace media::llm::agent

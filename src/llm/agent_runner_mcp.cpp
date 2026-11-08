#include "agent_runner_mcp.hpp"

#include <mutex>
#include <utility>

namespace media::llm::agent {
namespace {

std::mutex g_mu;
TanitMcpEnsureFn g_ensure = nullptr;
TanitMcpStatusFn g_status = nullptr;
TanitMcpStopFn g_stop = nullptr;
std::function<void()> g_activity_changed;

} // namespace

void register_tanit_mcp_host(TanitMcpEnsureFn ensure,
                             TanitMcpStatusFn status,
                             TanitMcpStopFn stop)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_ensure = ensure;
    g_status = status;
    g_stop = stop;
}

void register_tanit_mcp_ensure(TanitMcpEnsureFn ensure)
{
    register_tanit_mcp_host(ensure, nullptr, nullptr);
}

int ensure_tanit_mcp_listen_port()
{
    TanitMcpEnsureFn fn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        fn = g_ensure;
    }
    return fn ? fn() : 0;
}

TanitMcpListenInfo tanit_mcp_listen_info()
{
    TanitMcpStatusFn fn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        fn = g_status;
    }
    return fn ? fn() : TanitMcpListenInfo{};
}

bool stop_tanit_mcp_listener()
{
    TanitMcpStopFn fn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        fn = g_stop;
    }
    return fn ? fn() : false;
}

void set_tanit_mcp_activity_changed(std::function<void()> cb)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_activity_changed = std::move(cb);
}

void notify_tanit_mcp_activity_changed()
{
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        cb = g_activity_changed;
    }
    if (cb)
        cb();
}

} // namespace media::llm::agent

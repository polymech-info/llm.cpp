#include "llm/chat_host_events.hpp"

#include <functional>
#include <mutex>
#include <utility>

#if defined(FEATURE_COMMAND_IPC_NEXT) && FEATURE_COMMAND_IPC_NEXT
#include "core/cmd_ipc.hpp"
#include <cstdint>
#include <optional>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif
#endif

namespace media::llm {
namespace {
std::mutex g_mu;
std::function<void(const std::string&, nlohmann::json)> g_emit;

#if defined(FEATURE_COMMAND_IPC_NEXT) && FEATURE_COMMAND_IPC_NEXT
#if defined(_WIN32)
std::int64_t parent_pid_of(std::int64_t pid)
{
    if (pid <= 0)
        return 0;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    std::int64_t parent = 0;
    if (::Process32FirstW(snap, &pe)) {
        do {
            if (static_cast<std::int64_t>(pe.th32ProcessID) == pid) {
                parent = static_cast<std::int64_t>(pe.th32ParentProcessID);
                break;
            }
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);
    return parent;
}
#endif

std::optional<media::cmd_ipc::InstanceDescriptor> llm_agent_instance()
{
    const auto instances = media::cmd_ipc::list_instances();
    std::optional<media::cmd_ipc::InstanceDescriptor> newest;
    for (const auto& inst : instances) {
        if (inst.kind != media::cmd_ipc::ProcessKind::LlmAgent)
            continue;
        if (!newest || inst.started_unix_ms >= newest->started_unix_ms)
            newest = inst;
    }
#if defined(_WIN32)
    std::int64_t pid = parent_pid_of(static_cast<std::int64_t>(::GetCurrentProcessId()));
    for (int hop = 0; hop < 8 && pid > 4; ++hop) {
        for (const auto& inst : instances) {
            if (inst.kind == media::cmd_ipc::ProcessKind::LlmAgent && inst.pid == pid)
                return inst;
        }
        pid = parent_pid_of(pid);
    }
#endif
    return newest;
}

bool emit_via_cmd_ipc(const std::string& method, const nlohmann::json& args)
{
    const auto inst = llm_agent_instance();
    if (!inst)
        return false;
    nlohmann::json reply;
    std::string err;
    nlohmann::json extra{{"method", method}, {"args", args}};
#if defined(_WIN32)
    extra["pid"] = static_cast<std::uint64_t>(::GetCurrentProcessId());
#endif
    const bool sent = media::cmd_ipc::send_action(*inst, "chat_host", extra, reply, err, 3000);
    return sent && reply.value("type", std::string{}) == "chat_host_ack";
}
#endif
}

void set_chat_host_emit(std::function<void(const std::string& method, nlohmann::json args)> fn)
{
    std::lock_guard lock(g_mu);
    g_emit = std::move(fn);
}

bool emit_chat_host(const std::string& method, nlohmann::json args)
{
    std::function<void(const std::string&, nlohmann::json)> fn;
    {
        std::lock_guard lock(g_mu);
        fn = g_emit;
    }
    if (fn) {
        fn(method, std::move(args));
        return true;
    }
#if defined(FEATURE_COMMAND_IPC_NEXT) && FEATURE_COMMAND_IPC_NEXT
    return emit_via_cmd_ipc(method, args);
#else
    (void)method;
    return false;
#endif
}
} // namespace media::llm

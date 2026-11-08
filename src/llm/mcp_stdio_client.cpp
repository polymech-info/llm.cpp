#include "mcp_stdio_client.hpp"

#include "core/stdio_child_runner.hpp"
#include "logging/logger.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <thread>

namespace media::llm::mcp {
namespace {

void parse_jsonrpc_result(const nlohmann::json& envelope, nlohmann::json& result_out, std::string& err)
{
    err.clear();
    result_out = nlohmann::json::object();
    if (envelope.contains("error") && envelope["error"].is_object()) {
        const auto& e = envelope["error"];
        err           = e.value("message", std::string("jsonrpc error"));
        err += " (code " + std::to_string(e.value("code", 0)) + ")";
        return;
    }
    if (envelope.contains("result"))
        result_out = envelope["result"];
}

} // namespace

struct StdioMcpClient::Impl {
    std::unique_ptr<media::process::StdioChildProcess> child;
    std::map<std::string, std::string> env_extra; // from mcp.json "env" object

    std::mutex              proc_mu;
    std::mutex              rpc_mu;
    std::mutex              q_mu;
    std::condition_variable q_cv;
    std::deque<std::string> lines;
    bool                    reader_eof = false;
    std::thread             reader_th;
    int                       next_jsonrpc_id = 1;
    bool                      initialized     = false;
    int                       timeout_ms      = 60000;
    std::function<bool()>      cancel_check;
    std::string                launch_error;
    std::vector<media::process::StdioPathGrant> launch_paths;

    void reader_thread_body()
    {
        std::string acc;
        char        buf[16384];
        for (;;) {
            std::size_t n = 0;
            std::string err;
            if (!child || !child->read_some(buf, sizeof(buf), n, err) || n == 0)
                break;
            if (n == 0)
                break;
            acc.append(buf, n);
            for (;;) {
                const auto pos = acc.find('\n');
                if (pos == std::string::npos)
                    break;
                std::string line = acc.substr(0, pos);
                acc.erase(0, pos + 1);
                while (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (!line.empty()) {
                    std::lock_guard<std::mutex> lk(q_mu);
                    lines.push_back(std::move(line));
                    q_cv.notify_one();
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(q_mu);
            reader_eof = true;
        }
        q_cv.notify_all();
    }

    void start_reader() { reader_th = std::thread([this] { reader_thread_body(); }); }

    bool write_all(const std::string& data, std::string& err)
    {
        std::lock_guard<std::mutex> proc_lk(proc_mu);
        if (!child) {
            err = "MCP stdio: process not started";
            return false;
        }
        return child->write_all(data, err);
    }

    bool cancelled() const
    {
        try {
            return cancel_check && cancel_check();
        } catch (...) {
            return false;
        }
    }

    bool wait_response_for_id(int want_id, const char* op, nlohmann::json& envelope, std::string& err)
    {
        namespace chr = std::chrono;
        const auto deadline = timeout_ms > 0
            ? chr::steady_clock::now() + chr::milliseconds(timeout_ms)
            : chr::steady_clock::time_point::max();
        for (;;) {
            std::unique_lock<std::mutex> lk(q_mu);
            q_cv.wait_for(lk, chr::milliseconds(100), [&] { return !lines.empty() || reader_eof; });
            if (lines.empty() && reader_eof) {
                err = "MCP stdio: stdout closed before JSON-RPC response";
                return false;
            }
            if (cancelled()) {
                lk.unlock();
                err = std::string("MCP stdio: ") + op + " cancelled";
                logger::trace("[cancel] MCP stdio: cancelling active JSON-RPC id=" + std::to_string(want_id));
                stop_process();
                logger::trace("[cancel] MCP stdio: stop_process returned for JSON-RPC id=" + std::to_string(want_id));
                return false;
            }
            if (timeout_ms > 0 && chr::steady_clock::now() >= deadline) {
                lk.unlock();
                err = std::string("MCP stdio: ") + op + " timed out after "
                    + std::to_string(timeout_ms) + "ms";
                logger::warn(err + " (jsonrpc_id=" + std::to_string(want_id) + ")");
                stop_process();
                logger::trace("MCP stdio: stop_process returned after timeout for JSON-RPC id=" + std::to_string(want_id));
                return false;
            }
            if (lines.empty())
                continue;
            std::string line = std::move(lines.front());
            lines.pop_front();
            lk.unlock();

            nlohmann::json j;
            try {
                j = nlohmann::json::parse(line);
            } catch (...) {
                logger::debug(std::string("MCP stdio: skip non-JSON line: ") + line.substr(0, 120));
                continue;
            }
            if (j.contains("method") && !j.contains("id"))
                continue;
            if (j.contains("id")) {
                int rid = -1;
                if (j["id"].is_number_integer())
                    rid = static_cast<int>(j["id"].get<long long>());
                else if (j["id"].is_number_unsigned())
                    rid = static_cast<int>(j["id"].get<unsigned long long>());
                if (rid == want_id) {
                    envelope = std::move(j);
                    return true;
                }
                logger::debug("MCP stdio: ignoring unexpected JSON-RPC response id="
                              + std::to_string(rid) + " while waiting for id="
                              + std::to_string(want_id));
            }
        }
    }

    void stop_process()
    {
        std::unique_lock<std::mutex> proc_lk(proc_mu);
        if (child)
            child->terminate_tree();
        proc_lk.unlock();
        if (reader_th.joinable())
            reader_th.join();
        proc_lk.lock();
        child.reset();
        initialized = false;
        {
            std::lock_guard<std::mutex> q_lk(q_mu);
            lines.clear();
            reader_eof = true;
        }
        q_cv.notify_all();
    }
};

StdioMcpClient::StdioMcpClient(std::string command, std::vector<std::string> args,
                               std::map<std::string, std::string> env)
    : StdioMcpClient(std::move(command), std::move(args), std::move(env),
                     media::process::StdioLaunchOptions{})
{}

StdioMcpClient::StdioMcpClient(std::string command, std::vector<std::string> args,
                               std::map<std::string, std::string> env,
                               media::process::StdioLaunchOptions opts)
    : impl_(std::make_unique<Impl>())
{
    impl_->env_extra = std::move(env);
    impl_->launch_paths = opts.paths;
    opts.executable = std::move(command);
    opts.args = std::move(args);
    opts.extra_env = impl_->env_extra;
    std::string err;
    auto child = media::process::launch_stdio_child(opts, err);
    if (!child.valid()) {
        logger::warn("MCP stdio: process not started: " + err);
        impl_->launch_error = err.empty() ? "unknown spawn failure" : err;
        return;
    }
    impl_->child = std::make_unique<media::process::StdioChildProcess>(std::move(child));
}

StdioMcpClient::~StdioMcpClient()
{
    if (!impl_)
        return;
    impl_->stop_process();
}

bool StdioMcpClient::initialize(std::string& err)
{
    err.clear();
    if (!impl_ || !impl_->child || !impl_->child->valid()) {
        err = "MCP stdio: process not started";
        if (impl_ && !impl_->launch_error.empty())
            err += ": " + impl_->launch_error;
        else
            err += " (spawn failed — is `uvx` on PATH?)";
        return false;
    }

    impl_->start_reader();

    std::lock_guard<std::mutex> rpc_lk(impl_->rpc_mu);
    const int init_id = impl_->next_jsonrpc_id++;
    nlohmann::json params = nlohmann::json::object();
    params["protocolVersion"] = "2025-11-25";
    params["capabilities"]    = nlohmann::json::object();
    params["clientInfo"]      = nlohmann::json{{"name", "pm-image"}, {"version", "mcp-stdio"}};

    const nlohmann::json req = {{"jsonrpc", "2.0"}, {"id", init_id}, {"method", "initialize"}, {"params", params}};

    if (!impl_->write_all(req.dump() + "\n", err))
        return false;

    nlohmann::json env;
    if (!impl_->wait_response_for_id(init_id, "initialize", env, err))
        return false;

    std::string    perr;
    nlohmann::json init_result;
    parse_jsonrpc_result(env, init_result, perr);
    if (!perr.empty()) {
        err = perr;
        return false;
    }
    (void)init_result;

    const nlohmann::json notif = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}, {"params", nlohmann::json::object()}};
    if (!impl_->write_all(notif.dump() + "\n", err))
        return false;

    impl_->initialized = true;
    return true;
}

void StdioMcpClient::try_delete_session()
{
    if (!impl_)
        return;
    impl_->stop_process();
    impl_->initialized = false;
}

nlohmann::json StdioMcpClient::session_info() const
{
    nlohmann::json info = nlohmann::json::object();
    if (!impl_ || !impl_->child)
        return info;
    const auto& child = impl_->child->info();
    info["pid"] = child.pid;
    info["sandboxed"] = child.sandboxed;
    if (!child.sandbox_backend.empty())
        info["sandbox_backend"] = child.sandbox_backend;
    if (!child.container_sid.empty())
        info["containerSid"] = child.container_sid;
    if (!impl_->launch_paths.empty()) {
        nlohmann::json paths = nlohmann::json::array();
        for (const auto& pg : impl_->launch_paths)
            paths.push_back({{"path", pg.path}, {"access", pg.access}});
        info["granted_paths"] = std::move(paths);
    }
    return info;
}

void StdioMcpClient::set_request_options(int timeout_ms, std::function<bool()> cancel_check)
{
    if (!impl_)
        return;
    std::lock_guard<std::mutex> lk(impl_->rpc_mu);
    impl_->timeout_ms = timeout_ms > 0 ? timeout_ms : 60000;
    impl_->cancel_check = std::move(cancel_check);
}

nlohmann::json StdioMcpClient::tools_list_all(std::string& err)
{
    err.clear();
    if (!impl_ || !impl_->initialized) {
        err = "MCP stdio: not initialized";
        return {};
    }

    std::lock_guard<std::mutex> rpc_lk(impl_->rpc_mu);
    nlohmann::json all = nlohmann::json::array();
    std::string    cursor;
    for (;;) {
        const int rid = impl_->next_jsonrpc_id++;
        nlohmann::json params = nlohmann::json::object();
        if (!cursor.empty())
            params["cursor"] = cursor;

        const nlohmann::json req = {{"jsonrpc", "2.0"}, {"id", rid}, {"method", "tools/list"}, {"params", params}};
        if (!impl_->write_all(req.dump() + "\n", err))
            return {};

        nlohmann::json env;
        if (!impl_->wait_response_for_id(rid, "tools/list", env, err))
            return {};

        std::string    perr;
        nlohmann::json result;
        parse_jsonrpc_result(env, result, perr);
        if (!perr.empty()) {
            err = perr;
            return {};
        }
        if (result.contains("tools") && result["tools"].is_array()) {
            for (const auto& t : result["tools"])
                all.push_back(t);
        }
        cursor.clear();
        if (result.contains("nextCursor") && result["nextCursor"].is_string()) {
            cursor = result["nextCursor"].get<std::string>();
            if (!cursor.empty())
                continue;
        }
        break;
    }
    return all;
}

/**
https://modelcontextprotocol.io/specification/2025-11-25/server/resources
@todo : mcp resources 
 */
nlohmann::json StdioMcpClient::resources_list_all(std::string& /*err*/)
{
    if (!impl_ || !impl_->initialized)
        return nlohmann::json::array();

    std::lock_guard<std::mutex> rpc_lk(impl_->rpc_mu);
    nlohmann::json all = nlohmann::json::array();
    std::string    cursor;
    for (;;) {
        const int rid = impl_->next_jsonrpc_id++;
        nlohmann::json params = nlohmann::json::object();
        if (!cursor.empty())
            params["cursor"] = cursor;

        const nlohmann::json req = {{"jsonrpc", "2.0"}, {"id", rid}, {"method", "resources/list"}, {"params", params}};
        std::string write_err;
        if (!impl_->write_all(req.dump() + "\n", write_err))
            return all;

        nlohmann::json env;
        std::string    read_err;
        if (!impl_->wait_response_for_id(rid, "resources/list", env, read_err))
            return all; // server may not support resources — not fatal

        std::string    perr;
        nlohmann::json result;
        parse_jsonrpc_result(env, result, perr);
        if (!perr.empty())
            return all; // method not supported

        if (result.contains("resources") && result["resources"].is_array()) {
            for (const auto& r : result["resources"])
                all.push_back(r);
        }
        cursor.clear();
        if (result.contains("nextCursor") && result["nextCursor"].is_string()) {
            cursor = result["nextCursor"].get<std::string>();
            if (!cursor.empty())
                continue;
        }
        break;
    }
    return all;
}

nlohmann::json StdioMcpClient::tools_call(const std::string& remote_tool_name, const nlohmann::json& arguments,
                                          std::string& err)
{
    err.clear();
    if (!impl_ || !impl_->initialized) {
        err = "MCP stdio: not initialized";
        return {};
    }

    std::lock_guard<std::mutex> rpc_lk(impl_->rpc_mu);
    const int        rid = impl_->next_jsonrpc_id++;
    nlohmann::json   params = {{"name", remote_tool_name},
                             {"arguments", arguments.is_object() ? arguments : nlohmann::json::object()}};
    const nlohmann::json req  = {{"jsonrpc", "2.0"}, {"id", rid}, {"method", "tools/call"}, {"params", params}};
    if (!impl_->write_all(req.dump() + "\n", err))
        return {};

    nlohmann::json env;
    if (!impl_->wait_response_for_id(rid, remote_tool_name.c_str(), env, err))
        return {};

    std::string    perr;
    nlohmann::json result;
    parse_jsonrpc_result(env, result, perr);
    if (!perr.empty()) {
        err = perr;
        return {};
    }
    return result;
}

} // namespace media::llm::mcp

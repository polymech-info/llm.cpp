#include "agent_runner_codex.hpp"
#include "fs/path_sanitizer.hpp"

#include "agent_runner_context.hpp"

#include "constants.hpp"
#include "core/cli_cancel.hpp"
#include "process/process_runner.hpp"
#include "llm/path_tool_catalog.hpp"
#include "logging/pm_log.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace media::llm::agent {
namespace {

bool emit_event(const EventCallback& cb, const Event& ev)
{
    if (!cb) return true;
    return cb(ev);
}

std::string json_string_field(const nlohmann::json& j, std::initializer_list<const char*> keys)
{
    for (const char* k : keys) {
        if (j.contains(k) && j[k].is_string())
            return j[k].get<std::string>();
    }
    return {};
}

std::string codex_error_text(const nlohmann::json& j)
{
    std::string text = json_string_field(j, {"message", "error"});
    if (!text.empty())
        return text;
    if (j.contains("error") && j["error"].is_object()) {
        text = json_string_field(j["error"], {"message", "detail", "type"});
        if (!text.empty())
            return text;
        return j["error"].dump();
    }
    return {};
}

std::string toml_string(std::string_view value)
{
    std::string out{"\""};
    out.reserve(value.size() + 2);
    for (const char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

std::string toml_string_array(const std::vector<std::string>& values)
{
    std::string out{"["};
    for (const auto& value : values) {
        if (value.empty())
            continue;
        if (out.size() > 1)
            out.push_back(',');
        out += toml_string(value);
    }
    out.push_back(']');
    return out;
}

std::string item_type(const nlohmann::json& item)
{
    return json_string_field(item, {"type"});
}

std::string item_text(const nlohmann::json& item)
{
    if (item.contains("text") && item["text"].is_string())
        return item["text"].get<std::string>();
    if (item.contains("message") && item["message"].is_string())
        return item["message"].get<std::string>();
    return {};
}

bool handle_codex_item_event(const std::string& phase,
                             const nlohmann::json& item,
                             EventCallback on_event,
                             std::string& final_text,
                             std::vector<ExternalMcpToolRecord>& mcp_tools)
{
    const std::string kind = item_type(item);
    if (kind == "agent_message") {
        const std::string text = item_text(item);
        if (phase == "item.completed" && !text.empty()) {
            final_text = text;
            return emit_event(on_event, Event{Event::AssistantText, text, {}, {}});
        }
        if (phase == "item.updated" && !text.empty())
            return emit_event(on_event, Event{Event::TextDelta, text, {}, {}});
    } else if (kind == "reasoning") {
        const std::string text = item_text(item);
        if (!text.empty())
            return emit_event(on_event, Event{Event::Thinking, text, {}, {}});
    } else if (kind == "command_execution") {
        const std::string cmd = json_string_field(item, {"command"});
        if (phase == "item.started" && !cmd.empty()) {
            nlohmann::json payload{{"arguments", nlohmann::json{{"command", cmd}}}};
            return emit_event(on_event, Event{Event::ToolCall, {}, "shell", std::move(payload)});
        }
        if (phase == "item.completed") {
            nlohmann::json payload{
                {"command", cmd},
                {"exit_code", item.value("exit_code", -1)},
                {"output", json_string_field(item, {"aggregated_output", "output"})},
                {"status", json_string_field(item, {"status"})},
            };
            return emit_event(on_event, Event{Event::ToolResult, {}, "shell", std::move(payload)});
        }
    } else if (kind == "file_change") {
        if (phase == "item.completed") {
            if (item.contains("changes") && item["changes"].is_array()) {
                for (const auto& change : item["changes"]) {
                    if (!change.is_object())
                        continue;
                    nlohmann::json payload = change;
                    payload["codex"] = item;
                    if (!emit_event(on_event, Event{Event::ToolResult, {}, "write_file", std::move(payload)}))
                        return false;
                }
                return true;
            }
            nlohmann::json payload = item;
            payload["path"] = json_string_field(item, {"path", "file"});
            payload["action"] = json_string_field(item, {"action", "change_type"});
            return emit_event(on_event, Event{Event::ToolResult, {}, "write_file", std::move(payload)});
        }
    } else if (kind == "mcp_tool_call") {
        const std::string tool = json_string_field(item, {"tool", "name"});
        const std::string server = json_string_field(item, {"server"});
        const std::string native = tanit_native_tool_name(tool, server);
        const std::string shown = native.empty() ? tool : native;
        if (phase == "item.started" && !tool.empty()) {
            nlohmann::json args = item.contains("arguments") ? item["arguments"] : nlohmann::json::object();
            nlohmann::json payload{{"arguments", std::move(args)}, {"server", server}};
            if (!native.empty())
                payload["mcp_tool"] = tool;
            return emit_event(on_event, Event{Event::ToolCall, {}, shown, std::move(payload)});
        }
        if (phase == "item.completed") {
            nlohmann::json payload = item;
            nlohmann::json env = tanit_envelope_from_mcp_value(item);
            if (!native.empty() && !env.empty()) {
                payload["envelope"] = env;
                payload["mcp_tool"] = tool;
                nlohmann::json args = nlohmann::json::object();
                if (item.contains("arguments") && item["arguments"].is_object())
                    args = item["arguments"];
                std::string id = json_string_field(item, {"id", "call_id"});
                if (id.empty())
                    id = "mcp_" + std::to_string(mcp_tools.size());
                mcp_tools.push_back(ExternalMcpToolRecord{id, native, std::move(args), env});
            }
            return emit_event(on_event, Event{Event::ToolResult, {}, shown.empty() ? "mcp" : shown, std::move(payload)});
        }
    }
    return true;
}

bool handle_codex_line(const std::string& line,
                       EventCallback on_event,
                       Result& result,
                       std::string& thread_id,
                       bool& turn_completed,
                       std::vector<ExternalMcpToolRecord>& mcp_tools)
{
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(line);
    } catch (...) {
        return true;
    }

    result.transcript.push_back(j);

    const std::string type = json_string_field(j, {"type"});
    if (!emit_event(on_event, Event{Event::ExternalStream, type.empty() ? "codex" : type, {}, j}))
        return false;

    if (type == "thread.started") {
        thread_id = json_string_field(j, {"thread_id"});
        nlohmann::json payload{{"runner", "codex"}, {"thread_id", thread_id}};
        return emit_event(on_event, Event{Event::TurnStarted, {}, {}, std::move(payload)});
    }
    if (type == "turn.started")
        return true;

    if (type == "turn.completed") {
        turn_completed = true;
        if (j.contains("usage") && j["usage"].is_object())
            result.llm_usage_aggregate = j["usage"];
        result.ok = true;
        return emit_event(on_event, Event{Event::Done, result.final_text, {}, {{"usage", j.value("usage", nlohmann::json{})}}});
    }

    if (type == "turn.failed" || type == "error") {
        const std::string msg = codex_error_text(j);
        result.error = msg.empty() ? type : msg;
        result.ok = false;
        return emit_event(on_event, Event{Event::Error, result.error, {}, j});
    }

    if (type == "item.started" || type == "item.updated" || type == "item.completed") {
        if (!j.contains("item") || !j["item"].is_object())
            return true;
        return handle_codex_item_event(type, j["item"], on_event, result.final_text, mcp_tools);
    }

    return true;
}

std::vector<std::string> build_codex_argv(const Turn& turn, const AgentRunnerOptions& opt)
{
    const ExternalContextBundle ctx = build_external_context(turn);
    std::vector<std::string> args;

    // Codex 0.155 treats these as global options. Keeping them before `exec`
    // is required because the resume parser does not accept -s/-a/-C.
    if (!opt.sandbox.empty())
        args.push_back("-s"), args.push_back(opt.sandbox);
    if (!opt.approval.empty())
        args.push_back("-a"), args.push_back(opt.approval);
    if (!ctx.working_root.empty())
        args.push_back("-C"), args.push_back(media::path::path_to_utf8(ctx.working_root));
    if (!opt.model.empty())
        args.push_back("-m"), args.push_back(opt.model);
    if (!opt.profile.empty())
        args.push_back("-p"), args.push_back(opt.profile);
    for (const auto& dir : opt.additional_dirs)
        if (!dir.empty())
            args.push_back("--add-dir"), args.push_back(dir);
    for (const auto& override_value : opt.config_overrides)
        if (!override_value.empty())
            args.push_back("-c"), args.push_back(override_value);

    if (!opt.tanit_mcp_url.empty() && turn.mcp_tools_enabled) {
        args.push_back("-c");
        args.push_back("mcp_servers.tanit.url=" + toml_string(opt.tanit_mcp_url));
        args.push_back("-c");
        args.push_back("mcp_servers.tanit.required=true");
        if (turn.planner_path_tool_allowlist_set) {
            args.push_back("-c");
            args.push_back("mcp_servers.tanit.enabled_tools="
                           + toml_string_array(turn.allowed_path_tools));
        }
        if (!turn.disabled_path_tools.empty()) {
            args.push_back("-c");
            args.push_back("mcp_servers.tanit.disabled_tools="
                           + toml_string_array(turn.disabled_path_tools));
        }
        const auto approved_tools = turn.planner_path_tool_allowlist_set
            ? path::tool_catalog_allowing(turn.allowed_path_tools, turn.disabled_path_tools)
            : path::tool_catalog_excluding(turn.disabled_path_tools);
        for (const auto& tool : approved_tools) {
            args.push_back("-c");
            args.push_back("mcp_servers.tanit.tools." + tool.name
                           + ".approval_mode=\"approve\"");
        }
    }

    args.push_back("exec");
    if (!opt.resume_id.empty())
        args.push_back("resume");
    args.push_back("--json");
    if (opt.ephemeral)
        args.push_back("--ephemeral");
    if (opt.skip_git_repo_check)
        args.push_back("--skip-git-repo-check");
    if (opt.ignore_user_config)
        args.push_back("--ignore-user-config");
    if (opt.ignore_rules)
        args.push_back("--ignore-rules");
    for (const auto& img : ctx.image_attachments)
        args.push_back("-i"), args.push_back(media::path::path_to_utf8(img));
    if (!opt.resume_id.empty())
        args.push_back(opt.resume_id);
    if (!ctx.user_prompt.empty())
        args.push_back(ctx.user_prompt);

    return args;
}

} // namespace

Result run_codex_agent_turn(const Turn& turn, const AgentRunnerOptions& opt, EventCallback on_event)
{
    Result result;
    result.transcript = nlohmann::json::array();

    if (turn.user_prompt.empty()) {
        result.error = "prompt is required";
        emit_event(on_event, Event{Event::Error, result.error, {}, {}});
        return result;
    }
    if (opt.ephemeral && !opt.resume_id.empty()) {
        result.error = "Codex --ephemeral cannot resume a persisted session";
        emit_event(on_event, Event{Event::Error, result.error, {}, {}});
        return result;
    }

    media::process::RunOptions proc;
    apply_external_cli_command(proc, "codex", opt.executable_override, build_codex_argv(turn, opt));
    proc.cwd        = turn.folder_hint;
    proc.mode       = media::process::RunMode::Captured;
    proc.timeout_ms = opt.timeout_ms > 0 ? opt.timeout_ms : 600000;
    proc.capture_max_bytes = 16 * 1024 * 1024;
    proc.augment_path = true;
    proc.redirect_stdin_null = true;
    std::atomic<bool> event_cancelled{false};
    proc.on_cancel_check = [&turn, &event_cancelled] {
        if (event_cancelled.load()) return true;
        if (turn.cancel_check && turn.cancel_check()) return true;
        if (turn.cancel_signal && turn.cancel_signal->load()) return true;
        return media::cli::cancel_requested();
    };

    std::string line_buf;
    std::string thread_id;
    bool turn_completed = false;
    std::vector<ExternalMcpToolRecord> mcp_tools;
    proc.on_output_chunk = [&](const std::string& stream, const std::string& chunk) {
        if (stream != "stdout") return;
        line_buf.append(chunk);
        for (;;) {
            const auto pos = line_buf.find('\n');
            if (pos == std::string::npos) break;
            std::string line = line_buf.substr(0, pos);
            line_buf.erase(0, pos + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            if (line.empty()) continue;
            if (!handle_codex_line(line, on_event, result, thread_id, turn_completed, mcp_tools))
                event_cancelled.store(true);
        }
    };

    logger::info("[agent-runner:codex] spawn "
                 + (proc.use_shell ? proc.shell_command : proc.executable));
    const media::process::RunResult run = media::process::run(proc);

    if (!line_buf.empty()) {
        std::string tail = line_buf;
        while (!tail.empty() && (tail.back() == '\r' || tail.back() == '\n'))
            tail.pop_back();
        if (!tail.empty())
            (void)handle_codex_line(tail, on_event, result, thread_id, turn_completed, mcp_tools);
    }

    const bool turn_cancelled = run.cancelled
        || event_cancelled.load()
        || (turn.cancel_signal && turn.cancel_signal->load())
        || (turn.cancel_check && turn.cancel_check())
        || media::cli::cancel_requested();
    if (turn_cancelled) {
        result.cancelled = true;
        result.ok = false;
        if (result.error.empty())
            result.error = "cancelled";
        return result;
    }

    if (!run.ok) {
        result.ok = false;
        result.error = run.error.empty() ? "codex process failed" : run.error;
        if (!run.stderr_text.empty() && result.error.find(run.stderr_text) == std::string::npos)
            result.error += ": " + run.stderr_text;
        emit_event(on_event, Event{Event::Error, result.error, {}, {}});
        return result;
    }

    if (run.exit_code != 0 && !result.ok) {
        result.ok = false;
        if (result.error.empty())
            result.error = "codex exited with code " + std::to_string(run.exit_code);
        if (!run.stderr_text.empty()) {
            std::string err = run.stderr_text;
            while (!err.empty() && (err.back() == '\r' || err.back() == '\n'))
                err.pop_back();
            if (!err.empty() && result.error.find(err) == std::string::npos)
                result.error += ": " + err;
        }
        emit_event(on_event, Event{Event::Error, result.error, {}, {}});
        return result;
    }

    if (run.exit_code == 0 && !turn_completed) {
        result.ok = false;
        if (result.error.empty())
            result.error = "Codex exited without a turn.completed event";
        emit_event(on_event, Event{Event::Error, result.error, {}, {}});
    }

    if (!thread_id.empty()) {
        nlohmann::json meta{{"external_session_id", thread_id}, {"runner", "codex"}};
        result.transcript.push_back(meta);
    }

    if (result.ok)
        persist_external_mcp_artifacts(turn, result.final_text, mcp_tools);

    return result;
}

} // namespace media::llm::agent

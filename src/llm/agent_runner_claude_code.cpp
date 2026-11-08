#include "agent_runner_claude_code.hpp"
#include "fs/path_sanitizer.hpp"

#include "agent_runner_claude_defaults.hpp"
#include "agent_runner_context.hpp"

#include "constants.hpp"
#include "core/cli_cancel.hpp"
#include "process/process_runner.hpp"
#include "llm/path_tool_catalog.hpp"
#include "logging/pm_log.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

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

std::string claude_stream_label(const nlohmann::json& j)
{
    const std::string type = json_string_field(j, {"type"});
    if (type.empty()) return "claude";
    const std::string subtype = json_string_field(j, {"subtype"});
    return subtype.empty() ? type : (type + ":" + subtype);
}

std::string map_claude_permission_mode(const AgentRunnerOptions& opt)
{
    // acceptEdits auto-approves workspace file writes without yolo'ing
    // bash/network (bypassPermissions). Headless Tanit defaults to never
    // prompts, so that is the workspace-write path — not OS sandbox.
    if (opt.approval == "never" || opt.approval.empty())
        return "acceptEdits";
    if (opt.approval == "on-request")
        return "default";
    if (opt.approval == "bypass")
        return "bypassPermissions";
    return {};
}

std::string claude_mcp_tool_name(const std::string& name)
{
    return "mcp__tanit__" + name;
}

std::string join_csv(const std::vector<std::string>& values)
{
    std::string out;
    for (const auto& value : values) {
        if (!out.empty())
            out.push_back(',');
        out += value;
    }
    return out;
}

nlohmann::json write_tool_envelope(const std::string& path)
{
    nlohmann::json env{{"summary", {{"total", 1}, {"succeeded", 1}, {"failed", 0}}}};
    env["results"] = nlohmann::json::array({
        nlohmann::json{{"ok", true}, {"path", path}, {"output_path", path}},
    });
    return env;
}

bool handle_claude_assistant_message(const nlohmann::json& message,
                                     EventCallback on_event,
                                     std::string& final_text,
                                     std::unordered_map<std::string, std::string>& tool_use_names,
                                     std::unordered_map<std::string, nlohmann::json>& tool_use_args)
{
    if (!message.contains("content") || !message["content"].is_array())
        return true;

    for (const auto& block : message["content"]) {
        if (!block.is_object())
            continue;
        const std::string kind = json_string_field(block, {"type"});
        if (kind == "text") {
            const std::string text = json_string_field(block, {"text"});
            if (!text.empty()) {
                final_text = text;
                if (!emit_event(on_event, Event{Event::AssistantText, text, {}, {}}))
                    return false;
            }
        } else if (kind == "thinking") {
            const std::string text = json_string_field(block, {"thinking"});
            if (!text.empty()) {
                if (!emit_event(on_event, Event{Event::Thinking, text, {}, block}))
                    return false;
            }
        } else if (kind == "tool_use") {
            const std::string tool = json_string_field(block, {"name"});
            const std::string id   = json_string_field(block, {"id"});
            nlohmann::json args = block.contains("input") ? block["input"] : nlohmann::json::object();
            if (!id.empty() && !tool.empty())
                tool_use_names[id] = tool;
            if (!id.empty())
                tool_use_args[id] = args;
            if (!tool.empty()) {
                const std::string shown = tanit_native_tool_name(tool, "tanit");
                nlohmann::json payload{{"arguments", args}, {"tool_use_id", id}};
                if (!shown.empty())
                    payload["mcp_tool"] = tool;
                if (!emit_event(on_event,
                                Event{Event::ToolCall, {}, shown.empty() ? tool : shown, std::move(payload)}))
                    return false;
            }
        }
    }
    return true;
}

bool handle_claude_user_message(const nlohmann::json& message,
                                EventCallback on_event,
                                const std::unordered_map<std::string, std::string>& tool_use_names,
                                const std::unordered_map<std::string, nlohmann::json>& tool_use_args,
                                const std::unordered_map<std::string, std::string>& tool_use_inputs,
                                std::vector<ExternalMcpToolRecord>& mcp_tools)
{
    if (!message.contains("content") || !message["content"].is_array())
        return true;

    for (const auto& block : message["content"]) {
        if (!block.is_object())
            continue;
        if (json_string_field(block, {"type"}) != "tool_result")
            continue;

        const std::string use_id = json_string_field(block, {"tool_use_id"});
        std::string tool_name;
        if (auto it = tool_use_names.find(use_id); it != tool_use_names.end())
            tool_name = it->second;

        std::string content_text;
        if (block.contains("content")) {
            if (block["content"].is_string())
                content_text = block["content"].get<std::string>();
            else if (block["content"].is_array()) {
                for (const auto& part : block["content"]) {
                    if (part.is_object() && json_string_field(part, {"type"}) == "text") {
                        content_text += json_string_field(part, {"text"});
                    } else if (part.is_string()) {
                        content_text += part.get<std::string>();
                    }
                }
            }
        }

        nlohmann::json payload{{"tool_use_id", use_id}, {"content", content_text}, {"claude", block}};
        const std::string tanit_tool = tanit_native_tool_name(tool_name, {});
        nlohmann::json tanit_env = tanit_envelope_from_mcp_value(nlohmann::json(content_text));
        if (tanit_env.empty() && block.contains("content"))
            tanit_env = tanit_envelope_from_mcp_value(block["content"]);
        if (!tanit_tool.empty() && !tanit_env.empty()) {
            payload["envelope"] = tanit_env;
            payload["mcp_tool"] = tool_name;
            nlohmann::json args = nlohmann::json::object();
            if (auto it = tool_use_args.find(use_id); it != tool_use_args.end() && it->second.is_object())
                args = it->second;
            const std::string id = use_id.empty() ? ("mcp_" + std::to_string(mcp_tools.size())) : use_id;
            mcp_tools.push_back(ExternalMcpToolRecord{id, tanit_tool, std::move(args), tanit_env});
            if (!emit_event(on_event, Event{Event::ToolResult, content_text, tanit_tool, std::move(payload)}))
                return false;
            continue;
        }

        std::string native_tool = tool_name;
        if (tool_name == "Write" || tool_name == "NotebookEdit")
            native_tool = "write_file";
        if (tool_name == "Edit")
            native_tool = "file_str_replace";

        if (native_tool == "write_file" || native_tool == "file_str_replace") {
            std::string path;
            if (auto it = tool_use_inputs.find(use_id); it != tool_use_inputs.end())
                path = it->second;
            if (path.empty() && block.contains("content") && block["content"].is_string()) {
                const std::string c = block["content"].get<std::string>();
                if (!c.empty() && (c.find('/') != std::string::npos || c.find('\\') != std::string::npos))
                    path = c;
            }
            if (!path.empty())
                payload["envelope"] = write_tool_envelope(path);
        }

        if (!emit_event(on_event, Event{Event::ToolResult, content_text, native_tool.empty() ? tool_name : native_tool,
                                        std::move(payload)}))
            return false;
    }
    return true;
}

bool handle_claude_line(const std::string& line,
                        EventCallback on_event,
                        Result& result,
                        std::string& session_id,
                        std::unordered_map<std::string, std::string>& tool_use_names,
                        std::unordered_map<std::string, nlohmann::json>& tool_use_args,
                        std::unordered_map<std::string, std::string>& tool_use_inputs,
                        std::vector<ExternalMcpToolRecord>& mcp_tools)
{
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(line);
    } catch (...) {
        return true;
    }

    result.transcript.push_back(j);

    if (!emit_event(on_event, Event{Event::ExternalStream, claude_stream_label(j), {}, j}))
        return false;

    const std::string type = json_string_field(j, {"type"});
    if (type == "system" && json_string_field(j, {"subtype"}) == "init") {
        session_id = json_string_field(j, {"session_id"});
        nlohmann::json payload{{"runner", "claude-code"}, {"session_id", session_id}};
        return emit_event(on_event, Event{Event::TurnStarted, {}, {}, std::move(payload)});
    }

    if (type == "stream_event" && j.contains("event") && j["event"].is_object()) {
        const auto& ev = j["event"];
        const std::string ev_type = json_string_field(ev, {"type"});
        if (ev_type == "content_block_delta" && ev.contains("delta") && ev["delta"].is_object()) {
            const auto& delta = ev["delta"];
            const std::string delta_type = json_string_field(delta, {"type"});
            if (delta_type == "text_delta") {
                const std::string text = json_string_field(delta, {"text"});
                if (!text.empty())
                    return emit_event(on_event, Event{Event::TextDelta, text, {}, {}});
            } else if (delta_type == "thinking_delta") {
                const std::string text = json_string_field(delta, {"thinking"});
                if (!text.empty())
                    return emit_event(on_event, Event{Event::Thinking, text, {}, delta});
            }
        }
        return true;
    }

    if (type == "assistant" && j.contains("message") && j["message"].is_object()) {
        const auto& msg = j["message"];
        if (msg.contains("content") && msg["content"].is_array()) {
            for (const auto& block : msg["content"]) {
                if (!block.is_object() || json_string_field(block, {"type"}) != "tool_use")
                    continue;
                const std::string id = json_string_field(block, {"id"});
                const nlohmann::json& input = block.contains("input") ? block["input"] : nlohmann::json::object();
                if (id.empty() || !input.is_object())
                    continue;
                if (input.contains("file_path") && input["file_path"].is_string())
                    tool_use_inputs[id] = input["file_path"].get<std::string>();
                else if (input.contains("notebook_path") && input["notebook_path"].is_string())
                    tool_use_inputs[id] = input["notebook_path"].get<std::string>();
            }
        }
        return handle_claude_assistant_message(msg, on_event, result.final_text, tool_use_names, tool_use_args);
    }

    if (type == "user" && j.contains("message") && j["message"].is_object())
        return handle_claude_user_message(j["message"], on_event, tool_use_names, tool_use_args,
                                          tool_use_inputs, mcp_tools);

    if (type == "result") {
        const std::string subtype = json_string_field(j, {"subtype"});
        if (j.value("is_error", false) || subtype == "error" || subtype == "error_during_execution") {
            result.ok = false;
            result.error = json_string_field(j, {"result", "error", "message"});
            if (result.error.empty())
                result.error = subtype.empty() ? "claude result error" : subtype;
            return emit_event(on_event, Event{Event::Error, result.error, {}, j});
        }

        const std::string text = json_string_field(j, {"result"});
        if (!text.empty())
            result.final_text = text;
        if (session_id.empty())
            session_id = json_string_field(j, {"session_id"});
        if (j.contains("usage") && j["usage"].is_object())
            result.llm_usage_aggregate = j["usage"];
        result.ok = true;
        return emit_event(on_event,
                          Event{Event::Done, result.final_text, {}, {{"usage", j.value("usage", nlohmann::json{})}}});
    }

    return true;
}

std::vector<std::string> build_claude_argv(const Turn& turn, const AgentRunnerOptions& opt)
{
    const ExternalContextBundle ctx = build_external_context(turn);
    const ClaudeDefaults defs = load_claude_defaults(turn.folder_hint);
    const std::string tanit_url = (turn.mcp_tools_enabled) ? opt.tanit_mcp_url : std::string{};
    std::vector<std::string> args = resolve_claude_default_argv(defs, turn, tanit_url);

    if (!defs.sources.empty())
        pm::log::debug("agent-runner:claude", "claude_defaults sources=" + defs.sources.dump());
    if (!args.empty())
        pm::log::debug("agent-runner:claude", "claude_defaults argv=" + format_claude_argv_for_log(args));

    args.push_back("-p");
    args.push_back(ctx.user_prompt);
    args.push_back("--output-format");
    args.push_back("stream-json");
    args.push_back("--verbose");
    args.push_back("--include-partial-messages");

    const std::string permission_mode = map_claude_permission_mode(opt);
    if (!permission_mode.empty()) {
        args.push_back("--permission-mode");
        args.push_back(permission_mode);
    }
    if (!opt.model.empty()) {
        args.push_back("--model");
        args.push_back(opt.model);
    }
    bool has_mcp_config = false;
    for (const auto& arg : args) {
        if (arg == "--mcp-config") {
            has_mcp_config = true;
            break;
        }
    }
    if (!has_mcp_config && !opt.tanit_mcp_url.empty() && turn.mcp_tools_enabled) {
        const nlohmann::json mcp_config{
            {"mcpServers", {
                {"tanit", {
                    {"type", "http"},
                    {"url", opt.tanit_mcp_url},
                }},
            }},
        };
        args.push_back("--mcp-config");
        args.push_back(mcp_config.dump());
    }
    if (!opt.tanit_mcp_url.empty() && turn.mcp_tools_enabled) {
        // acceptEdits does not auto-approve MCP. Claude 2.1 also defers MCP
        // tools behind ToolSearch; without --allowedTools the first speak /
        // info_lookup call returns "requested permissions ... haven't granted".
        // Empty allowlist = full catalog minus disabled (same as native runner).
        // mcp__tanit__* is the Claude wildcard; ToolSearch is required to
        // select deferred MCP tools when the allowlist is treated as exclusive.
        std::vector<std::string> allowed;
        allowed.push_back("ToolSearch");
        if (!turn.planner_path_tool_allowlist_set)
            allowed.push_back("mcp__tanit__*");
        for (const auto& tool : path::tool_catalog_allowing(
                 turn.allowed_path_tools, turn.disabled_path_tools))
            allowed.push_back(claude_mcp_tool_name(tool.name));
        if (!allowed.empty()) {
            args.push_back("--allowedTools");
            args.push_back(join_csv(allowed));
        }

        std::vector<std::string> denied;
        if (turn.planner_path_tool_allowlist_set) {
            std::unordered_set<std::string> selected;
            for (const auto& tool : path::tool_catalog_allowing(
                     turn.allowed_path_tools, turn.disabled_path_tools))
                selected.insert(tool.name);
            for (const auto& tool : path::tool_catalog()) {
                if (!selected.count(tool.name))
                    denied.push_back(claude_mcp_tool_name(tool.name));
            }
        } else {
            denied.reserve(turn.disabled_path_tools.size());
            for (const auto& name : turn.disabled_path_tools)
                denied.push_back(claude_mcp_tool_name(name));
        }
        if (!denied.empty()) {
            args.push_back("--disallowedTools");
            args.push_back(join_csv(denied));
        }
    }
    if (!opt.resume_id.empty()) {
        args.push_back("--resume");
        args.push_back(opt.resume_id);
    }

    std::error_code ec;
    auto add_dir_from_path = [&](const std::string& sel) {
        fs::path p = media::path::path_from_utf8(sel);
        if (!p.is_absolute())
            p = fs::absolute(p, ec);
        fs::path dir = p.has_extension() ? p.parent_path() : p;
        if (dir.empty() || !fs::exists(dir, ec))
            return;
        args.push_back("--add-dir");
        args.push_back(media::path::path_to_utf8(dir));
    };
    for (const auto& sel : turn.selection)
        add_dir_from_path(sel);
    for (const auto& open : turn.open_files)
        add_dir_from_path(open);

    return args;
}

} // namespace

Result run_claude_code_agent_turn(const Turn& turn, const AgentRunnerOptions& opt, EventCallback on_event)
{
    Result result;
    result.transcript = nlohmann::json::array();

    if (turn.user_prompt.empty()) {
        result.error = "prompt is required";
        emit_event(on_event, Event{Event::Error, result.error, {}, {}});
        return result;
    }

    media::process::RunOptions proc;
    apply_external_cli_command(proc, "claude", opt.executable_override, build_claude_argv(turn, opt));
    proc.cwd        = turn.folder_hint;
    proc.mode       = media::process::RunMode::Captured;
    proc.timeout_ms = opt.timeout_ms > 0 ? opt.timeout_ms : 600000;
    proc.capture_max_bytes = 16 * 1024 * 1024;
    proc.augment_path = true;
    std::atomic<bool> event_cancelled{false};
    proc.on_cancel_check = [&turn, &event_cancelled] {
        if (event_cancelled.load()) return true;
        if (turn.cancel_check && turn.cancel_check()) return true;
        if (turn.cancel_signal && turn.cancel_signal->load()) return true;
        return media::cli::cancel_requested();
    };

    std::string line_buf;
    std::string session_id;
    std::unordered_map<std::string, std::string> tool_use_names;
    std::unordered_map<std::string, nlohmann::json> tool_use_args;
    std::unordered_map<std::string, std::string> tool_use_inputs;
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
            if (!handle_claude_line(line, on_event, result, session_id,
                                    tool_use_names, tool_use_args, tool_use_inputs, mcp_tools))
                event_cancelled.store(true);
        }
    };

    logger::debug("[agent-runner:claude] spawn "
                 + (proc.use_shell ? proc.shell_command : proc.executable));
    pm::log::trace_lazy("agent-runner:claude", [&] {
        return std::string("argv=") + format_claude_argv_for_log(proc.args);
    });
    const media::process::RunResult run = media::process::run(proc);

    if (!line_buf.empty()) {
        std::string tail = line_buf;
        while (!tail.empty() && (tail.back() == '\r' || tail.back() == '\n'))
            tail.pop_back();
        if (!tail.empty()) {
            (void)handle_claude_line(tail, on_event, result, session_id,
                                     tool_use_names, tool_use_args, tool_use_inputs, mcp_tools);
        }
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
        result.error = run.error.empty() ? "claude process failed" : run.error;
        if (!run.stderr_text.empty() && result.error.find(run.stderr_text) == std::string::npos)
            result.error += ": " + run.stderr_text;
        emit_event(on_event, Event{Event::Error, result.error, {}, {}});
        return result;
    }

    if (run.exit_code != 0 && !result.ok) {
        result.ok = false;
        if (result.error.empty())
            result.error = "claude exited with code " + std::to_string(run.exit_code);
        emit_event(on_event, Event{Event::Error, result.error, {}, {}});
        return result;
    }

    if (!result.ok && run.exit_code == 0 && !result.final_text.empty())
        result.ok = true;

    if (!result.ok && run.exit_code == 0 && result.error.empty())
        result.ok = true;

    if (!session_id.empty()) {
        nlohmann::json meta{{"external_session_id", session_id}, {"runner", "claude-code"}};
        result.transcript.push_back(meta);
    }

    if (result.ok)
        persist_external_mcp_artifacts(turn, result.final_text, mcp_tools);

    return result;
}

} // namespace media::llm::agent

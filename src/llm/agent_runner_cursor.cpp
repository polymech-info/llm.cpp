#include "agent_runner_cursor.hpp"
#include "fs/path_sanitizer.hpp"

#include "agent_runner_context.hpp"

#include "constants.hpp"
#include "core/cli_cancel.hpp"
#include "process/process_runner.hpp"
#include "logging/pm_log.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <sstream>

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

bool handle_assistant_message(const nlohmann::json& message,
                              EventCallback on_event,
                              std::string& final_text)
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
        } else if (kind == "tool_use") {
            const std::string tool = json_string_field(block, {"name"});
            nlohmann::json args = block.contains("input") ? block["input"] : nlohmann::json::object();
            if (!tool.empty()) {
                if (!emit_event(on_event,
                                Event{Event::ToolCall, {}, tool, {{"arguments", std::move(args)}}}))
                    return false;
            }
        }
    }
    return true;
}

bool handle_cursor_line(const std::string& line,
                        EventCallback on_event,
                        Result& result,
                        std::string& session_id)
{
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(line);
    } catch (...) {
        return true;
    }

    result.transcript.push_back(j);

    const std::string type = json_string_field(j, {"type"});

    if (type == "system" && json_string_field(j, {"subtype"}) == "init") {
        session_id = json_string_field(j, {"session_id", "conversation_id", "chat_id"});
        nlohmann::json payload{{"runner", "cursor"}, {"session_id", session_id}};
        return emit_event(on_event, Event{Event::TurnStarted, {}, {}, std::move(payload)});
    }

    if (type == "stream_event" && j.contains("event") && j["event"].is_object()) {
        const auto& ev = j["event"];
        const std::string ev_type = json_string_field(ev, {"type"});
        if (ev_type == "content_block_delta" && ev.contains("delta") && ev["delta"].is_object()) {
            const auto& delta = ev["delta"];
            if (json_string_field(delta, {"type"}) == "text_delta") {
                const std::string text = json_string_field(delta, {"text"});
                if (!text.empty())
                    return emit_event(on_event, Event{Event::TextDelta, text, {}, {}});
            }
        }
        return true;
    }

    if (type == "assistant" && j.contains("message") && j["message"].is_object())
        return handle_assistant_message(j["message"], on_event, result.final_text);

    if (type == "tool_result" || type == "tool_result_delta") {
        const std::string tool = json_string_field(j, {"tool_name", "name"});
        return emit_event(on_event, Event{Event::ToolResult, {}, tool.empty() ? "tool" : tool, j});
    }

    if (type == "result") {
        const std::string subtype = json_string_field(j, {"subtype"});
        if (j.value("is_error", false) || subtype == "error") {
            result.ok = false;
            result.error = json_string_field(j, {"result", "error", "message"});
            if (result.error.empty())
                result.error = subtype.empty() ? "cursor result error" : subtype;
            return emit_event(on_event, Event{Event::Error, result.error, {}, j});
        }

        const std::string text = json_string_field(j, {"result"});
        if (!text.empty())
            result.final_text = text;
        if (session_id.empty())
            session_id = json_string_field(j, {"session_id", "conversation_id", "chat_id"});
        if (j.contains("usage") && j["usage"].is_object())
            result.llm_usage_aggregate = j["usage"];
        result.ok = true;
        return emit_event(on_event,
                          Event{Event::Done, result.final_text, {}, {{"usage", j.value("usage", nlohmann::json{})}}});
    }

    if (type == "error") {
        result.ok = false;
        result.error = json_string_field(j, {"message", "error"});
        if (result.error.empty())
            result.error = "cursor error";
        return emit_event(on_event, Event{Event::Error, result.error, {}, j});
    }

    return true;
}

std::vector<std::string> build_cursor_argv(const Turn& turn, const AgentRunnerOptions& opt)
{
    const ExternalContextBundle ctx = build_external_context(turn);

    std::vector<std::string> args{
        "-p", turn.user_prompt,
        "--output-format", "stream-json",
        "--stream-partial-output",
        "--trust",
    };

    if (opt.approval == "never")
        args.push_back("-f");
    if (!opt.mode.empty()) {
        args.push_back("--mode");
        args.push_back(opt.mode);
    }
    if (!opt.model.empty()) {
        args.push_back("--model");
        args.push_back(opt.model);
    }
    if (!opt.resume_id.empty()) {
        args.push_back("--resume");
        args.push_back(opt.resume_id);
    }

    if (!ctx.working_root.empty()) {
        args.push_back("--workspace");
        args.push_back(media::path::path_to_utf8(ctx.working_root));
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

Result run_cursor_agent_turn(const Turn& turn, const AgentRunnerOptions& opt, EventCallback on_event)
{
    Result result;
    result.transcript = nlohmann::json::array();

    if (turn.user_prompt.empty()) {
        result.error = "prompt is required";
        emit_event(on_event, Event{Event::Error, result.error, {}, {}});
        return result;
    }

    media::process::RunOptions proc;
    apply_external_cli_command(proc, "agent", opt.executable_override, build_cursor_argv(turn, opt));
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
    std::string session_id;
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
            if (!handle_cursor_line(line, on_event, result, session_id))
                event_cancelled.store(true);
        }
    };

    logger::info("[agent-runner:cursor] spawn "
                 + (proc.use_shell ? proc.shell_command : proc.executable));
    const media::process::RunResult run = media::process::run(proc);

    if (!line_buf.empty()) {
        std::string tail = line_buf;
        while (!tail.empty() && (tail.back() == '\r' || tail.back() == '\n'))
            tail.pop_back();
        if (!tail.empty())
            (void)handle_cursor_line(tail, on_event, result, session_id);
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
        result.error = run.error.empty() ? "cursor agent process failed" : run.error;
        if (!run.stderr_text.empty() && result.error.find(run.stderr_text) == std::string::npos)
            result.error += ": " + run.stderr_text;
        emit_event(on_event, Event{Event::Error, result.error, {}, {}});
        return result;
    }

    if (run.exit_code != 0 && !result.ok) {
        result.ok = false;
        if (result.error.empty())
            result.error = "cursor agent exited with code " + std::to_string(run.exit_code);
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

    if (!result.ok && run.exit_code == 0 && !result.final_text.empty())
        result.ok = true;

    if (!result.ok && run.exit_code == 0 && result.error.empty())
        result.ok = true;

    if (!session_id.empty()) {
        nlohmann::json meta{{"external_session_id", session_id}, {"runner", "cursor"}};
        result.transcript.push_back(meta);
    }

    return result;
}

} // namespace media::llm::agent

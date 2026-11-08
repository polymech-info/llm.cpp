#include "agent_runner_context.hpp"

#include "agent_memory_artifacts.hpp"
#include "constants.hpp"
#include "core/app_exe_directory.hpp"
#include "process/process_helpers.hpp"
#include "string/string_sanitizers.hpp"
#include "llm/llm_constants.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace media::llm::agent {
namespace {

bool looks_like_image_path(const fs::path& p)
{
    if (!p.has_extension()) return false;
    std::string ext = media::path::path_to_utf8(p.extension());
    ext = media::strutil::lower_ascii(std::move(ext));
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".gif"
        || ext == ".webp" || ext == ".bmp" || ext == ".tif" || ext == ".tiff"
        || ext == ".heic" || ext == ".avif";
}

std::optional<std::string> npm_js_for_cmd(const fs::path& cmd_path,
                                          const std::string& tool_name)
{
    const fs::path modules = cmd_path.parent_path() / "node_modules";
    std::error_code ec;
    if (!fs::exists(modules, ec))
        return std::nullopt;

    const fs::path candidates[] = {
        modules / tool_name / "bin" / (tool_name + ".js"),
        modules / "@openai" / tool_name / "bin" / (tool_name + ".js"),
    };
    for (const fs::path& js : candidates) {
        if (fs::is_regular_file(js, ec))
            return media::path::path_to_utf8(js);
    }

    for (const auto& scope : fs::directory_iterator(modules, ec)) {
        if (ec || !scope.is_directory())
            continue;
        for (const auto& pkg : fs::directory_iterator(scope.path(), ec)) {
            if (ec || !pkg.is_directory())
                continue;
            const fs::path js = pkg.path() / "bin" / (tool_name + ".js");
            if (fs::is_regular_file(js, ec))
                return media::path::path_to_utf8(js);
        }
    }
    return std::nullopt;
}

#if defined(_WIN32)
std::optional<std::string> npm_native_exe_for_cmd(const fs::path& cmd_path,
                                                  const std::string& tool_name)
{
    const fs::path modules = cmd_path.parent_path() / "node_modules";
    std::error_code ec;
    if (!fs::exists(modules, ec))
        return std::nullopt;

    const fs::path candidates[] = {
        modules / "@anthropic-ai" / "claude-code" / "bin" / (tool_name + ".exe"),
        modules / tool_name / "bin" / (tool_name + ".exe"),
        modules / "@openai" / "codex" / "bin" / (tool_name + ".exe"),
    };
    for (const fs::path& exe : candidates) {
        if (fs::is_regular_file(exe, ec))
            return media::path::path_to_utf8(exe);
    }
    return std::nullopt;
}
#endif

std::string resolve_node_executable(const fs::path& cmd_path)
{
    std::error_code ec;
    const fs::path bundled = cmd_path.parent_path() / "node.exe";
    if (fs::is_regular_file(bundled, ec))
        return media::path::path_to_utf8(bundled);

#if defined(_WIN32)
    wchar_t found[MAX_PATH * 4]{};
    const DWORD n = ::SearchPathW(nullptr, L"node.exe", nullptr,
                                  static_cast<DWORD>(std::size(found)), found, nullptr);
    if (n > 0 && n < std::size(found))
        return media::path::path_to_utf8(std::filesystem::path(found));
#endif
    return "node";
}

struct NodeScriptEntry {
    std::string node_exe;
    std::string script_js;
};

std::optional<NodeScriptEntry> cursor_agent_entry_for_cmd(const fs::path& cmd_path)
{
    const fs::path root = cmd_path.parent_path();
    std::error_code ec;

    const fs::path flat_node = root / "node.exe";
    const fs::path flat_js   = root / "index.js";
    if (fs::is_regular_file(flat_node, ec) && fs::is_regular_file(flat_js, ec))
        return NodeScriptEntry{media::path::path_to_utf8(flat_node), media::path::path_to_utf8(flat_js)};

    const fs::path versions = root / "versions";
    if (!fs::is_directory(versions, ec))
        return std::nullopt;

    fs::path best;
    for (const auto& entry : fs::directory_iterator(versions, ec)) {
        if (ec || !entry.is_directory())
            continue;
        const std::string name = media::path::path_to_utf8(entry.path().filename());
        if (name.size() < 8 || name[0] < '0' || name[0] > '9')
            continue;
        if (best.empty() || name > media::path::path_to_utf8(best.filename()))
            best = entry.path();
    }
    if (best.empty())
        return std::nullopt;

    const fs::path node = best / "node.exe";
    const fs::path js   = best / "index.js";
    if (!fs::is_regular_file(node, ec) || !fs::is_regular_file(js, ec))
        return std::nullopt;
    return NodeScriptEntry{media::path::path_to_utf8(node), media::path::path_to_utf8(js)};
}

constexpr std::string_view k_agent_md_header = "## Agent.md\n";

const char* k_agent_md_names[] = {
    "Agent.md",
    "AGENTS.md",
    "agents.md",
    "AGENT.md",
};

fs::path agent_md_directory(const Turn& turn)
{
    fs::path dir;
    if (!turn.folder_hint.empty())
        dir = media::path::path_from_utf8(turn.folder_hint);
    else {
        std::error_code ec;
        dir = fs::current_path(ec);
        if (ec)
            return {};
    }
    std::error_code ec;
    if (fs::is_regular_file(dir, ec))
        dir = dir.parent_path();
    return dir;
}

std::string prompt_text(std::string raw)
{
    return media::str_san::apply_text_sanitizer(
        raw, media::str_san::TextSanitizer::StripUtf8Bom);
}

bool looks_binary_text(std::string_view buf)
{
    if (buf.find('\0') != std::string_view::npos)
        return true;
    const std::size_t scan = std::min(buf.size(), static_cast<std::size_t>(65536));
    std::size_t ctrl = 0;
    for (std::size_t i = 0; i < scan; ++i) {
        const auto c = static_cast<unsigned char>(buf[i]);
        if (c < 32 && c != '\t' && c != '\n' && c != '\r')
            ++ctrl;
    }
    return scan > 8000 && ctrl * 10 > scan;
}

bool path_stays_in_dir(const fs::path& dir, const fs::path& file)
{
    std::error_code ec;
    const fs::path base = fs::weakly_canonical(dir, ec);
    if (ec || base.empty())
        return false;
    ec.clear();
    const fs::path abs = fs::weakly_canonical(file, ec);
    if (ec || abs.empty())
        return false;
    const fs::path rel = abs.lexically_relative(base);
    const std::string s = media::path::path_to_generic_utf8(rel);
    if (s.empty() || s == "." || s == "..")
        return false;
    if (s.rfind("../", 0) == 0)
        return false;
    return s.find("/../") == std::string::npos;
}

bool offset_fits(std::uintmax_t off)
{
    return off <= static_cast<std::uintmax_t>(std::numeric_limits<std::streamoff>::max());
}

std::string read_span(std::ifstream& in, std::uintmax_t offset, std::size_t n)
{
    if (n == 0 || !offset_fits(offset))
        return {};
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    if (!in)
        return {};
    std::string buf(n, '\0');
    in.read(buf.data(), static_cast<std::streamsize>(n));
    buf.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
    return buf;
}

std::size_t count_text_lines_stream(std::ifstream& in, std::uintmax_t size)
{
    if (size == 0 || size > pm::llm::k_run_output_memory_capture_max_bytes)
        return 0;
    if (!offset_fits(0))
        return 0;
    in.clear();
    in.seekg(0);
    if (!in)
        return 0;

    std::size_t lines = 0;
    std::uintmax_t seen = 0;
    char buf[65536];
    bool saw_cr = false;
    bool any = false;
    bool ended_on_break = false;
    while (seen < size && in) {
        const auto room = size - seen;
        const auto chunk = static_cast<std::streamsize>(
            std::min<std::uintmax_t>(room, sizeof(buf)));
        in.read(buf, chunk);
        const auto got = in.gcount();
        if (got <= 0)
            break;
        for (std::streamsize i = 0; i < got; ++i) {
            any = true;
            const char c = buf[static_cast<std::size_t>(i)];
            if (saw_cr) {
                saw_cr = false;
                if (c == '\n') {
                    ended_on_break = true;
                    continue;
                }
            }
            if (c == '\n' || c == '\r') {
                ++lines;
                ended_on_break = true;
                if (c == '\r')
                    saw_cr = true;
            } else {
                ended_on_break = false;
            }
        }
        seen += static_cast<std::uintmax_t>(got);
    }
    if (!any)
        return 0;
    if (!ended_on_break)
        ++lines;
    return lines;
}

std::string clipping_marker(std::size_t omitted_bytes,
                            std::size_t line_count,
                            const fs::path& path)
{
    std::string m;
    m.reserve(256);
    m += "\n\n[... Agent.md clipped: ";
    m += std::to_string(omitted_bytes);
    m += " bytes omitted";
    if (line_count > 0) {
        m += ", ";
        m += std::to_string(line_count);
        m += " lines total";
    }
    m += ".\nFull file: ";
    m += media::path::path_to_utf8(path);
    m += "\nUse file_read with offset (1-based line) and limit. ";
    if (line_count > 0) {
        const std::size_t window = static_cast<std::size_t>(pm::llm::k_file_read_window_lines);
        const std::size_t tail_line = line_count > window ? line_count - window + 1 : 1;
        m += "The tail starts near offset ";
        m += std::to_string(tail_line);
        m += ". ";
    }
    m += "...]\n\n";
    return m;
}

std::string read_agent_md_body(const fs::path& path)
{
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec)
        return {};
    const auto sz = fs::file_size(path, ec);
    if (ec || sz == 0)
        return {};

    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};

    if (sz <= pm::llm::k_run_output_inline_max_bytes) {
        std::string body = prompt_text(read_span(in, 0, static_cast<std::size_t>(sz)));
        if (body.empty() || looks_binary_text(body))
            return {};
        return body;
    }

    // Read one code point past the budget so utf8_safe_prefix can back up
    // off a cut, instead of treating a buffer that already ends at the limit
    // as complete.
    constexpr std::size_t k_utf8_slack = 3;
    constexpr std::size_t k_bom_bytes = 3;
    std::string head = prompt_text(read_span(
        in, 0, pm::llm::k_run_output_head_bytes + k_utf8_slack + k_bom_bytes));
    if (looks_binary_text(head))
        return {};
    head = media::str_san::utf8_safe_prefix(head, pm::llm::k_run_output_head_bytes);

    const std::size_t tail_n = pm::llm::k_run_output_tail_bytes;
    const std::size_t tail_read = tail_n + k_utf8_slack;
    if (sz < tail_read || !offset_fits(sz - tail_read)) {
        const std::size_t omitted = head.size() >= sz ? 0 : static_cast<std::size_t>(sz - head.size());
        if (omitted == 0)
            return head;
        return head + clipping_marker(omitted, 0, path);
    }
    std::string tail = read_span(in, sz - tail_read, tail_read);
    if (looks_binary_text(tail))
        return {};
    tail = media::str_san::utf8_safe_suffix(tail, tail_n);

    const std::uintmax_t shown = static_cast<std::uintmax_t>(head.size() + tail.size());
    const std::size_t omitted = shown >= sz ? 0 : static_cast<std::size_t>(sz - shown);
    if (omitted == 0) {
        head += tail;
        return head;
    }

    const std::size_t lines = count_text_lines_stream(in, sz);
    head += clipping_marker(omitted, lines, path);
    head += tail;
    return head;
}

fs::path find_agent_md(const fs::path& dir)
{
    if (dir.empty())
        return {};
    std::error_code ec;
    for (const char* name : k_agent_md_names) {
        const fs::path cand = dir / name;
        if (!fs::is_regular_file(cand, ec) || ec) {
            ec.clear();
            continue;
        }
        if (!path_stays_in_dir(dir, cand))
            continue;
        return cand;
    }
    return {};
}

} // namespace

std::string agent_md_prompt_block(const Turn& turn)
{
    if (!turn.load_agent_md)
        return {};
    const fs::path found = find_agent_md(agent_md_directory(turn));
    if (found.empty())
        return {};
    const std::string body = read_agent_md_body(found);
    if (body.empty())
        return {};

    std::ostringstream s;
    s << k_agent_md_header
      << "path: " << media::path::path_to_utf8(found) << "\n\n"
      << body;
    if (body.back() != '\n')
        s << '\n';
    return s.str();
}

ExternalContextBundle build_external_context(const Turn& turn)
{
    ExternalContextBundle out;
    out.user_prompt = turn.user_prompt;

    if (!turn.folder_hint.empty())
        out.working_root = media::path::path_from_utf8(turn.folder_hint);
    else if (!turn.selection.empty())
        out.working_root = media::path::path_from_utf8(turn.selection.front()).parent_path();
    else if (!turn.open_files.empty())
        out.working_root = media::path::path_from_utf8(turn.open_files.front()).parent_path();

    std::ostringstream ctx;
    if (!turn.selection.empty()) {
        ctx << "## Tanit selection (" << turn.selection.size() << " file(s))\n";
        for (std::size_t i = 0; i < turn.selection.size(); ++i)
            ctx << "  [" << i << "] " << turn.selection[i] << "\n";
        ctx << "Treat these as the user's active context unless they say otherwise.\n\n";
    }
    if (!turn.open_files.empty()) {
        ctx << "## Tanit open files (" << turn.open_files.size() << " file(s))\n";
        for (std::size_t i = 0; i < turn.open_files.size(); ++i)
            ctx << "  [" << i << "] " << turn.open_files[i] << "\n";
        ctx << "These are files open in viewer tabs; they may differ from the Explorer selection.\n\n";
    }
    if (turn.selection.empty() && turn.open_files.empty() && !turn.folder_hint.empty()) {
        ctx << "## Tanit working folder\n" << turn.folder_hint << "\n\n";
    }

    if (!turn.system_extra.empty())
        ctx << turn.system_extra << "\n\n";

    const std::string agent_md = agent_md_prompt_block(turn);
    if (!agent_md.empty()
        && turn.system_extra.find(k_agent_md_header) == std::string::npos)
        ctx << agent_md << "\n\n";

    out.append_system = ctx.str();

    auto attach_images = [&](const std::vector<std::string>& paths) {
        for (const auto& sel : paths) {
            const fs::path p = media::path::path_from_utf8(sel);
            if (!looks_like_image_path(p))
                continue;
            if (std::find(out.image_attachments.begin(), out.image_attachments.end(), p)
                != out.image_attachments.end())
                continue;
            out.image_attachments.push_back(p);
        }
    };
    attach_images(turn.selection);
    attach_images(turn.open_files);

    if (!out.append_system.empty() && !out.user_prompt.empty()) {
        out.user_prompt = out.append_system + out.user_prompt;
        out.append_system.clear();
    }

    return out;
}

std::string resolve_external_cli_executable(const std::string& default_name,
                                            const std::string& override_path)
{
    if (!override_path.empty())
        return override_path;

    const fs::path bundled = media::app::resolve_third_party_executable(default_name);
    if (!bundled.empty()) {
        const std::string ext = media::path::path_to_utf8(bundled.extension());
        if (ext == ".exe" || ext == ".cmd" || ext == ".bat")
            return media::path::path_to_utf8(bundled);
    }

#if defined(_WIN32)
    const std::wstring wname = media::path::path_from_utf8(default_name).wstring();
    auto search_with_ext = [&](const wchar_t* ext) -> std::string {
        wchar_t found[MAX_PATH * 4]{};
        const DWORD n = ::SearchPathW(nullptr,
                                      wname.c_str(),
                                      ext,
                                      static_cast<DWORD>(std::size(found)),
                                      found,
                                      nullptr);
        if (n == 0 || n >= std::size(found))
            return {};
        fs::path path(found);
        std::error_code ec;
        if (!fs::is_regular_file(path, ec) || ec)
            return {};
        return media::path::path_to_utf8(path);
    };

    if (std::string exe = search_with_ext(L".exe"); !exe.empty())
        return exe;
    if (std::string cmd = search_with_ext(L".cmd"); !cmd.empty())
        return cmd;
#else
    if (!bundled.empty())
        return media::path::path_to_utf8(bundled);
#endif

    return default_name;
}

void apply_external_cli_command(media::process::RunOptions& proc,
                                const std::string& default_name,
                                const std::string& override_path,
                                std::vector<std::string> args)
{
    const std::string resolved = resolve_external_cli_executable(default_name, override_path);

#if defined(_WIN32)
    const fs::path exe_path = media::path::path_from_utf8(resolved);
    const std::wstring ext = exe_path.extension().wstring();
    if (_wcsicmp(ext.c_str(), L".cmd") == 0 || _wcsicmp(ext.c_str(), L".bat") == 0) {
        if (std::optional<std::string> native = npm_native_exe_for_cmd(exe_path, default_name)) {
            proc.use_shell     = false;
            proc.shell_command.clear();
            proc.executable    = *native;
            proc.args          = std::move(args);
            return;
        }
        if (std::optional<std::string> js = npm_js_for_cmd(exe_path, default_name)) {
            std::vector<std::string> node_args;
            node_args.push_back(*js);
            node_args.insert(node_args.end(),
                             std::make_move_iterator(args.begin()),
                             std::make_move_iterator(args.end()));
            proc.use_shell     = false;
            proc.shell_command.clear();
            proc.executable    = resolve_node_executable(exe_path);
            proc.args          = std::move(node_args);
            return;
        }

        if (std::optional<NodeScriptEntry> entry = cursor_agent_entry_for_cmd(exe_path)) {
            std::vector<std::string> node_args;
            node_args.push_back(entry->script_js);
            node_args.insert(node_args.end(),
                             std::make_move_iterator(args.begin()),
                             std::make_move_iterator(args.end()));
            proc.use_shell     = false;
            proc.shell_command.clear();
            proc.executable    = entry->node_exe;
            proc.args          = std::move(node_args);
            return;
        }

        std::wstring inner = media::process::quote_process_arg(exe_path.wstring());
        for (const auto& arg : args)
            inner += L" " + media::process::quote_process_arg(media::process::widen_utf8(arg));
        const auto shell = media::process::detect_shell("cmd");
        proc.use_shell     = false;
        proc.shell_command.clear();
        proc.executable    = shell.exe;
        proc.args          = {"/c", media::process::narrow_utf8(inner)};
        return;
    }
#endif

    proc.use_shell     = false;
    proc.shell_command.clear();
    proc.executable    = resolved;
    proc.args          = std::move(args);
}

namespace {


bool starts_with(const std::string& s, const char* prefix)
{
    const std::size_t n = std::char_traits<char>::length(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

bool looks_like_tanit_envelope(const nlohmann::json& j)
{
    return j.is_object() && (j.contains("results") || j.contains("ok"));
}

nlohmann::json envelope_from_text(const std::string& text, int depth);

nlohmann::json envelope_from_value(const nlohmann::json& value, int depth)
{
    if (depth > 6 || value.is_null())
        return nlohmann::json();
    if (value.is_string())
        return envelope_from_text(value.get<std::string>(), depth + 1);
    if (value.is_array()) {
        for (const auto& part : value) {
            nlohmann::json inner = envelope_from_value(part, depth + 1);
            if (!inner.empty())
                return inner;
        }
        return nlohmann::json();
    }
    if (!value.is_object())
        return nlohmann::json();
    if (value.contains("results") && value["results"].is_array())
        return value;
    for (const char* key : {"content", "text", "result", "output"}) {
        if (!value.contains(key))
            continue;
        nlohmann::json inner = envelope_from_value(value[key], depth + 1);
        if (!inner.empty())
            return inner;
    }
    if (looks_like_tanit_envelope(value))
        return value;
    return nlohmann::json();
}

nlohmann::json envelope_from_text(const std::string& text, int depth)
{
    const auto start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos || depth > 6)
        return nlohmann::json();
    const char c = text[start];
    if (c != '{' && c != '[')
        return nlohmann::json();
    try {
        return envelope_from_value(nlohmann::json::parse(text.substr(start)), depth + 1);
    } catch (...) {
        return nlohmann::json();
    }
}

} // namespace

std::string tanit_native_tool_name(const std::string& tool, const std::string& server)
{
    if (starts_with(tool, "mcp__tanit__") && tool.size() > 12)
        return tool.substr(12);
    if (starts_with(tool, "mcp_tanit__") && tool.size() > 11)
        return tool.substr(11);
    if (media::strutil::lower_ascii(server) == "tanit" && !tool.empty() && !starts_with(tool, "mcp"))
        return tool;
    return {};
}

nlohmann::json tanit_envelope_from_mcp_value(const nlohmann::json& value)
{
    return envelope_from_value(value, 0);
}

void persist_external_mcp_artifacts(const Turn& turn,
                                    const std::string& final_text,
                                    const std::vector<ExternalMcpToolRecord>& calls)
{
    if (turn.task_id.empty() || calls.empty() || turn.structured_memory_features == 0)
        return;

    nlohmann::json tool_calls = nlohmann::json::array();
    nlohmann::json tool_msgs = nlohmann::json::array();
    for (const auto& call : calls) {
        if (call.id.empty() || call.name.empty() || !call.envelope.is_object())
            continue;
        const std::string args = call.args.is_object() ? call.args.dump() : std::string("{}");
        tool_calls.push_back({
            {"id", call.id},
            {"type", "function"},
            {"function", {{"name", call.name}, {"arguments", args}}},
        });
        tool_msgs.push_back({
            {"role", "tool"},
            {"tool_call_id", call.id},
            {"content", call.envelope},
        });
    }
    if (tool_calls.empty())
        return;

    Result synthetic;
    synthetic.ok = true;
    synthetic.final_text = final_text;
    synthetic.transcript = nlohmann::json::object();
    synthetic.transcript["messages"] = nlohmann::json::array();
    synthetic.transcript["messages"].push_back({
        {"role", "assistant"},
        {"content", final_text},
        {"tool_calls", std::move(tool_calls)},
    });
    for (auto& msg : tool_msgs)
        synthetic.transcript["messages"].push_back(std::move(msg));

    (void)session_persist_artifacts_from_result(
        turn.task_id, turn.user_prompt, synthetic,
        turn.structured_memory_features, {},
        turn.source_log_session_id, turn.source_log_id, turn.source_log_path,
        turn.planner_meta);
}

} // namespace media::llm::agent

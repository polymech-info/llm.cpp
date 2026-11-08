#include "agent_turn_options.hpp"

#include "agent_tools.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

namespace media::llm::agent::options {
namespace fs = std::filesystem;

namespace {

std::string trim_value(std::string s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

using media::strutil::lower_ascii;

bool read_prompt_file(const std::string& value,
                      std::string& text,
                      std::string& resolved_path,
                      std::string& err)
{
    err.clear();
    text.clear();
    resolved_path.clear();
    std::string path_value = value;
    if (!path_value.empty() && path_value.front() == '@')
        path_value.erase(path_value.begin());
    else if (path_value.rfind("file:", 0) == 0)
        path_value.erase(0, 5);
    path_value = trim_value(path_value);
    if (path_value.empty()) {
        err = "empty prompt file path";
        return false;
    }

    std::error_code ec;
    fs::path fp(path_value);
    if (!fp.is_absolute())
        fp = fs::absolute(fp, ec);
    if (ec) {
        err = "could not resolve prompt file: " + path_value + ": " + ec.message();
        return false;
    }
    if (!fs::is_regular_file(fp, ec)) {
        err = "prompt file does not exist: " + media::path::path_to_utf8(fp);
        return false;
    }
    std::ifstream in(fp, std::ios::binary);
    if (!in) {
        err = "could not open prompt file: " + media::path::path_to_utf8(fp);
        return false;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    text = buf.str();
    resolved_path = media::path::path_to_utf8(fp);
    return true;
}

bool apply_prompt_option(const std::string& raw,
                         bool& override_set,
                         bool& is_null,
                         std::string& override_text,
                         std::string& override_source,
                         std::string& override_path,
                         std::string& err)
{
    err.clear();
    if (raw.empty())
        return true;

    const std::string value = trim_value(raw);
    const std::string low   = lower_ascii(value);
    override_set = true;
    is_null      = (low == "null" || low == "none");
    override_text.clear();
    override_source = is_null ? "override-null" : "override";
    override_path.clear();
    if (is_null)
        return true;

    std::error_code exists_ec;
    const bool file_exists = fs::exists(media::path::path_from_utf8(value), exists_ec);
    const bool looks_like_file = !value.empty() && (value.front() == '@' || value.rfind("file:", 0) == 0
        || media::path::path_from_utf8(value).is_absolute() || file_exists);
    if (looks_like_file) {
        if (!read_prompt_file(value, override_text, override_path, err))
            return false;
        override_source = "override-file";
        return true;
    }

    override_text = value;
    return true;
}

void normalize_tool_token(std::string& tok)
{
    tok = trim_value(tok);
    tok = media::strutil::lower_ascii(std::move(tok));
}

} // namespace

bool apply_system_prompt_option(const std::string& raw, Turn& turn, std::string& err)
{
    return apply_prompt_option(raw,
                               turn.system_prompt_override_set,
                               turn.system_prompt_is_null,
                               turn.system_prompt_override,
                               turn.system_prompt_override_source,
                               turn.system_prompt_override_path,
                               err);
}

bool apply_planner_prompt_option(const std::string& raw, Turn& turn, std::string& err)
{
    return apply_prompt_option(raw,
                               turn.planner_prompt_override_set,
                               turn.planner_prompt_is_null,
                               turn.planner_prompt_override,
                               turn.planner_prompt_override_source,
                               turn.planner_prompt_override_path,
                               err);
}

pm::llm::ContextReductionMode parse_context_reduction_mode(const std::string& raw)
{
    const std::string low = lower_ascii(trim_value(raw));
    if (low == "tool-results" || low == "tool_results" || low == "tools" || low == "tool")
        return pm::llm::ContextReductionMode::ToolResults;
    if (low == "all")
        return pm::llm::ContextReductionMode::All;
    return pm::llm::ContextReductionMode::Off;
}

void apply_disable_tools_list(const std::vector<std::string>& tools,
                              bool no_tools,
                              std::vector<std::string>& path_out,
                              std::vector<std::string>* mcp_out)
{
    path_out.clear();
    if (mcp_out)
        mcp_out->clear();
    if (no_tools)
        return;

    auto is_known = [](const std::string& name) {
        return pm::llm::agent_tool_find_by_name(name) != nullptr;
    };

    for (std::string tok : tools) {
        normalize_tool_token(tok);
        if (tok.empty() || tok.rfind("--", 0) == 0)
            continue;
        if (tok.rfind("mcp_", 0) == 0) {
            if (mcp_out)
                mcp_out->push_back(std::move(tok));
            else
                path_out.push_back(std::move(tok));
        } else if (!is_known(tok)) {
            path_out.push_back(std::move(tok));
        } else {
            path_out.push_back(std::move(tok));
        }
    }
}

void apply_enable_tools_list(const std::vector<std::string>& tools,
                             bool no_tools,
                             std::vector<std::string>& allowed_out,
                             bool& allowlist_set_out)
{
    allowed_out.clear();
    allowlist_set_out = false;
    if (tools.empty() || no_tools)
        return;

    auto is_known = [](const std::string& name) {
        return pm::llm::agent_tool_find_by_name(name) != nullptr;
    };

    for (std::string tok : tools) {
        normalize_tool_token(tok);
        if (tok.empty() || tok.rfind("--", 0) == 0)
            continue;
        if (!is_known(tok) && tok.rfind("mcp_", 0) != 0)
            continue;
        allowed_out.push_back(std::move(tok));
    }
    allowlist_set_out = !allowed_out.empty();
}

} // namespace media::llm::agent::options

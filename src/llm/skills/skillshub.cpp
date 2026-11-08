#include "llm/skills/skillshub.hpp"

#include "llm/agent_skills.hpp"
#include "llm/mcp_config.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

namespace media::llm::skills::hub {

namespace {

std::atomic<std::uint64_t> hub_cancelled_search_id{0};

bool hub_search_cancelled(std::uint64_t search_id)
{
    return search_id != 0 && hub_cancelled_search_id.load(std::memory_order_acquire) == search_id;
}

std::string trim_copy(std::string text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    return text;
}


bool tool_name_matches(const std::string& published, const std::string& bare)
{
    if (published == bare)
        return true;
    const std::string prefixed = "skillhub-" + bare;
    return published == prefixed;
}

nlohmann::json mcp_text_payload(const nlohmann::json& result)
{
    if (!result.is_object() || !result.contains("content") || !result["content"].is_array())
        return result.is_object() ? result : nlohmann::json{{"value", result}};
    std::string text;
    for (const auto& part : result["content"]) {
        if (!part.is_object() || part.value("type", std::string{}) != "text")
            continue;
        if (!part.contains("text") || !part["text"].is_string())
            continue;
        if (!text.empty())
            text.push_back('\n');
        text += part["text"].get<std::string>();
    }
    if (text.empty())
        return result;
    const std::string trimmed = trim_copy(text);
    if (!trimmed.empty() && (trimmed.front() == '{' || trimmed.front() == '[')) {
        try {
            return nlohmann::json::parse(trimmed);
        } catch (...) {
        }
    }
    return nlohmann::json{{"text", std::move(text)}};
}

std::string first_string(const nlohmann::json& obj, std::initializer_list<const char*> keys)
{
    if (!obj.is_object())
        return {};
    for (const char* key : keys) {
        if (obj.contains(key) && obj[key].is_string()) {
            const std::string value = trim_copy(obj[key].get<std::string>());
            if (!value.empty())
                return value;
        }
    }
    return {};
}

bool safe_dirname(const std::string& name)
{
    if (name.empty() || name == "." || name == "..")
        return false;
    if (name.front() == '.' )
        return false;
    for (unsigned char c : name) {
        if (c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return false;
    }
    return true;
}

std::string slug_from_detail(const nlohmann::json& data, const std::string& skill_id)
{
    std::string slug;
    if (data.contains("skill") && data["skill"].is_object())
        slug = first_string(data["skill"], {"slug", "name", "id"});
    if (slug.empty())
        slug = first_string(data, {"slug", "name", "skill_id", "id"});
    if (slug.empty())
        slug = skill_id;
    slug = trim_copy(slug);
    for (char& c : slug) {
        if (c == ' ' || c == '/')
            c = '-';
    }
    return slug;
}

std::string markdown_from_detail(const nlohmann::json& data)
{
    if (data.contains("files") && data["files"].is_array()) {
        for (const auto& file : data["files"]) {
            if (!file.is_object())
                continue;
            const std::string path = media::strutil::lower_ascii(first_string(file, {"path", "name", "file"}));
            if (path == "skill.md" || path.size() >= 9 && path.compare(path.size() - 9, 9, "/skill.md") == 0)
                return first_string(file, {"content", "text", "body"});
        }
    }
    const nlohmann::json* skill = (data.contains("skill") && data["skill"].is_object()) ? &data["skill"] : nullptr;
    std::string body = first_string(data, {"content", "skill_md", "skillMd", "markdown", "body"});
    if (body.empty() && skill)
        body = first_string(*skill, {"content", "skill_md", "skillMd", "markdown", "body"});
    if (body.empty())
        body = first_string(data, {"text"});
    return body;
}

bool path_is_under(const std::filesystem::path& root, const std::filesystem::path& child)
{
    std::error_code ec;
    const auto root_abs = std::filesystem::weakly_canonical(root, ec);
    if (ec || root_abs.empty())
        return false;
    const auto child_abs = std::filesystem::weakly_canonical(child, ec);
    if (ec || child_abs.empty())
        return false;
    const auto mismatch = std::mismatch(root_abs.begin(), root_abs.end(), child_abs.begin(), child_abs.end());
    return mismatch.first == root_abs.end();
}

bool skill_dir_exists(const std::filesystem::path& dir)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(dir / "SKILL.md", ec);
}

struct ResolvedTool {
    std::string server;
    nlohmann::json server_cfg;
    std::string tool;
};

bool resolve_tool(const std::string& bare, ResolvedTool& out, std::string& err)
{
#if !FEATURE_MCP_CLIENT
    (void)bare;
    (void)out;
    err = "MCP client is disabled in this build";
    return false;
#else
    std::string note;
    const nlohmann::json probe = media::llm::mcp::load_or_probe_mcp_tools(true, note, {}, true);
    if (!probe.contains("servers") || !probe["servers"].is_array()) {
        err = note.empty() ? "MCP tools cache is empty" : note;
        return false;
    }

    std::string server_name;
    std::string tool_name;
    for (const auto& srv : probe["servers"]) {
        if (!srv.is_object() || !srv.contains("tools") || !srv["tools"].is_array())
            continue;
        for (const auto& tool : srv["tools"]) {
            if (!tool.is_object())
                continue;
            const std::string name = tool.value("name", std::string{});
            if (!tool_name_matches(name, bare))
                continue;
            const bool prefixed = name.rfind("skillhub-", 0) == 0;
            if (tool_name.empty() || prefixed) {
                server_name = srv.value("name", std::string{});
                tool_name = name;
            }
        }
    }
    if (server_name.empty() || tool_name.empty()) {
        err = "SkillHub tool not found in mcp.json: " + bare;
        return false;
    }

    nlohmann::json cfg;
    std::string load_err;
    if (!media::llm::mcp::load_mcp_config({}, cfg, load_err)) {
        err = load_err.empty() ? "failed to load mcp config" : load_err;
        return false;
    }
    if (!cfg.contains("mcpServers") || !cfg["mcpServers"].is_object() || !cfg["mcpServers"].contains(server_name)) {
        err = "MCP server not found: " + server_name;
        return false;
    }
    out.server = server_name;
    out.server_cfg = cfg["mcpServers"][server_name];
    out.tool = tool_name;
    return true;
#endif
}

bool item_header_line(const std::string& line)
{
    std::size_t i = 0;
    while (i < line.size() && line[i] == ' ')
        ++i;
    if (i >= line.size() || !std::isdigit(static_cast<unsigned char>(line[i])))
        return false;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i])))
        ++i;
    return i + 2 <= line.size() && line[i] == '.' && line[i + 1] == ' ' && line.find("**") != std::string::npos;
}

bool parse_item_header(const std::string& line, nlohmann::json& row)
{
    const auto bold0 = line.find("**");
    if (bold0 == std::string::npos)
        return false;
    const auto bold1 = line.find("**", bold0 + 2);
    if (bold1 == std::string::npos)
        return false;
    const std::string name = trim_copy(line.substr(bold0 + 2, bold1 - (bold0 + 2)));
    const auto paren0 = line.find('(', bold1);
    const auto paren1 = paren0 == std::string::npos ? std::string::npos : line.find(')', paren0);
    if (paren0 == std::string::npos || paren1 == std::string::npos)
        return false;
    const std::string id = trim_copy(line.substr(paren0 + 1, paren1 - (paren0 + 1)));
    if (name.empty() || id.empty())
        return false;
    row = nlohmann::json{{"name", name}, {"id", id}, {"slug", id}};
    const auto br0 = line.find('[', paren1);
    const auto br1 = br0 == std::string::npos ? std::string::npos : line.find(']', br0);
    if (br0 != std::string::npos && br1 != std::string::npos) {
        const std::string category = trim_copy(line.substr(br0 + 1, br1 - (br0 + 1)));
        if (!category.empty())
            row["category"] = category;
    }
    return true;
}

bool skip_detail_line(const std::string& line)
{
    return trim_copy(line).empty();
}

bool metric_line(const std::string& line)
{
    const std::string trimmed = trim_copy(line);
    if (trimmed.find("**") != std::string::npos)
        return false;
    return trimmed.find("Score:") != std::string::npos || trimmed.find("Relevance:") != std::string::npos;
}

std::string metric_field(const std::string& line, const char* key)
{
    const auto pos = line.find(key);
    if (pos == std::string::npos)
        return {};
    std::size_t start = pos + std::char_traits<char>::length(key);
    while (start < line.size() && line[start] == ' ')
        ++start;
    std::size_t end = line.find('|', start);
    if (end == std::string::npos)
        end = line.size();
    return trim_copy(line.substr(start, end - start));
}

void apply_metric_line(const std::string& line, nlohmann::json& row)
{
    const std::string score = metric_field(line, "Score:");
    if (!score.empty())
        row["score"] = score;
    const std::string relevance = metric_field(line, "Relevance:");
    if (!relevance.empty())
        row["relevance"] = relevance;
}

void put_field(nlohmann::json& row, const nlohmann::json& hit, const nlohmann::json* skill, const char* key)
{
    std::string value = first_string(hit, {key});
    if (value.empty() && skill)
        value = first_string(*skill, {key});
    if (value.empty() && hit.contains(key) && hit[key].is_number())
        value = hit[key].dump();
    if (value.empty() && skill && skill->contains(key) && (*skill)[key].is_number())
        value = (*skill)[key].dump();
    if (!value.empty())
        row[key] = value;
}

nlohmann::json entries_from_markdown(const std::string& text)
{
    nlohmann::json rows = nlohmann::json::array();
    nlohmann::json current;
    std::string description;
    auto flush = [&]() {
        if (current.is_null() || current.empty())
            return;
        if (!description.empty())
            current["description"] = description;
        rows.push_back(std::move(current));
        current = nlohmann::json();
        description.clear();
    };
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const auto next = text.find('\n', pos);
        const std::string line = text.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        if (item_header_line(line)) {
            flush();
            nlohmann::json row;
            if (parse_item_header(line, row))
                current = std::move(row);
        } else if (!current.empty() && metric_line(line)) {
            apply_metric_line(line, current);
        } else if (!current.empty() && !skip_detail_line(line)) {
            const std::string trimmed = trim_copy(line);
            if (trimmed.rfind("Use ", 0) != 0) {
                if (!description.empty())
                    description.push_back(' ');
                description += trimmed;
            }
        }
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    flush();
    return rows;
}

nlohmann::json entry_from_object(const nlohmann::json& hit)
{
    if (!hit.is_object())
        return nlohmann::json();
    const nlohmann::json* skill = (hit.contains("skill") && hit["skill"].is_object()) ? &hit["skill"] : nullptr;
    auto pick = [&](std::initializer_list<const char*> keys) {
        std::string value = first_string(hit, keys);
        if (value.empty() && skill)
            value = first_string(*skill, keys);
        return value;
    };
    const std::string id = [&]() {
        std::string value = pick({"slug", "id", "skill_id"});
        if (value.empty())
            value = pick({"name"});
        return value;
    }();
    if (id.empty())
        return nlohmann::json();
    nlohmann::json row{{"id", id}, {"slug", id}};
    const std::string name = pick({"name", "slug"});
    row["name"] = name.empty() ? id : name;
    const std::string description = pick({"description"});
    if (!description.empty())
        row["description"] = description;
    put_field(row, hit, skill, "category");
    put_field(row, hit, skill, "score");
    put_field(row, hit, skill, "rating");
    put_field(row, hit, skill, "relevance");
    put_field(row, hit, skill, "stars");
    return row;
}

} // namespace

void cancel_inflight(std::uint64_t search_id)
{
    if (search_id != 0)
        hub_cancelled_search_id.store(search_id, std::memory_order_release);
}

ToolReply call_tool(const std::string& tool, const nlohmann::json& args, std::uint64_t search_id)
{
    ToolReply reply;
#if !FEATURE_MCP_CLIENT
    (void)tool;
    (void)args;
    (void)search_id;
    reply.error = "MCP client is disabled in this build";
    return reply;
#else
    ResolvedTool resolved;
    if (!resolve_tool(tool, resolved, reply.error))
        return reply;

    const auto env_cancel = media::llm::mcp::mcp_cancel_after_ms_from_env();
    media::llm::mcp::McpSessionCreateOptions session_opts;
    session_opts.timeout_ms = media::llm::mcp::mcp_default_timeout_ms();
    session_opts.cancel_check = [search_id, env_cancel] {
        if (hub_search_cancelled(search_id))
            return true;
        return env_cancel && env_cancel();
    };

    std::string session_err;
    std::unique_ptr<media::llm::mcp::IMcpSession> client =
        media::llm::mcp::create_mcp_session(resolved.server_cfg, resolved.server, session_err, session_opts);
    if (!client) {
        reply.error = session_err.empty() ? "failed to create MCP session" : session_err;
        return reply;
    }
    std::string init_err;
    if (!client->initialize(init_err)) {
        client->try_delete_session();
        reply.error = init_err.empty() ? "MCP handshake failed" : init_err;
        return reply;
    }
    std::string call_err;
    const nlohmann::json result = client->tools_call(resolved.tool, args.is_object() ? args : nlohmann::json::object(), call_err);
    client->try_delete_session();
    if (!call_err.empty()) {
        reply.error = call_err;
        return reply;
    }
    if (result.is_object() && result.value("isError", false)) {
        const nlohmann::json payload = mcp_text_payload(result);
        reply.error = payload.is_object() ? first_string(payload, {"text", "error", "message"}) : std::string{};
        if (reply.error.empty())
            reply.error = "SkillHub tool returned isError";
        reply.data = payload;
        return reply;
    }
    reply.ok = true;
    reply.data = mcp_text_payload(result);
    return reply;
#endif
}

ToolReply search_skills(const std::string& query, int limit, const std::string& category, std::uint64_t search_id)
{
    if (hub_search_cancelled(search_id)) {
        ToolReply reply;
        reply.error = "cancelled";
        return reply;
    }
    nlohmann::json args{{"query", query}};
    if (limit > 0)
        args["limit"] = limit;
    if (!category.empty())
        args["category"] = category;
    return call_tool("search_skills", args, search_id);
}

ToolReply browse_catalog(int limit, int offset, const std::string& sort, const std::string& category)
{
    nlohmann::json args = nlohmann::json::object();
    if (limit > 0)
        args["limit"] = limit;
    if (offset > 0)
        args["offset"] = offset;
    if (!sort.empty())
        args["sort"] = sort;
    if (!category.empty())
        args["category"] = category;
    return call_tool("browse_catalog", args);
}

ToolReply get_skill_detail(const std::string& skill_id, bool include_content)
{
    return call_tool("get_skill_detail", {
        {"skill_id", skill_id},
        {"include_content", include_content},
    });
}

ToolReply recommend_skills(const std::string& context, int limit, const std::vector<std::string>& current_skills)
{
    nlohmann::json current = nlohmann::json::array();
    for (const auto& name : current_skills)
        current.push_back(name);
    nlohmann::json args{{"context", context}, {"current_skills", std::move(current)}};
    if (limit > 0)
        args["limit"] = limit;
    return call_tool("recommend_skills", args);
}

nlohmann::json catalog_entries(const nlohmann::json& data)
{
    nlohmann::json rows = nlohmann::json::array();
    const nlohmann::json* listed = nullptr;
    if (data.is_array())
        listed = &data;
    else if (data.is_object()) {
        for (const char* key : {"skills", "results", "items", "recommendations", "catalog"}) {
            if (data.contains(key) && data[key].is_array()) {
                listed = &data[key];
                break;
            }
        }
    }
    if (listed) {
        for (const auto& hit : *listed) {
            nlohmann::json row = entry_from_object(hit);
            if (!row.empty())
                rows.push_back(std::move(row));
        }
        if (!rows.empty())
            return rows;
    }
    if (data.is_object()) {
        const std::string text = first_string(data, {"text", "markdown", "content"});
        if (!text.empty())
            return entries_from_markdown(text);
    }
    return rows;
}

std::string catalog_markdown(const nlohmann::json& data)
{
    if (data.is_object()) {
        const std::string text = first_string(data, {"text", "markdown"});
        if (!text.empty())
            return text;
    }
    const nlohmann::json rows = catalog_entries(data);
    if (!rows.is_array() || rows.empty())
        return "No matches.\n";
    std::string markdown;
    for (const auto& row : rows) {
        if (!row.is_object())
            continue;
        markdown += "- **";
        markdown += row.value("name", std::string{});
        markdown += "** (`";
        markdown += row.value("id", std::string{});
        markdown += "`)";
        const std::string description = row.value("description", std::string{});
        if (!description.empty()) {
            markdown += " - ";
            markdown += description;
        }
        markdown += "\n";
    }
    return markdown;
}

InstallReply install_roaming(const std::string& skill_id, bool force)
{
    InstallReply reply;
    const std::string id = trim_copy(skill_id);
    if (id.empty()) {
        reply.error = "skill id is required";
        return reply;
    }
    const ToolReply detail = get_skill_detail(id, true);
    if (!detail.ok) {
        reply.error = detail.error.empty() ? "get_skill_detail failed" : detail.error;
        return reply;
    }
    const std::string slug = slug_from_detail(detail.data, id);
    if (!safe_dirname(slug)) {
        reply.error = "skill slug is not a safe folder name: " + slug;
        return reply;
    }
    const std::string body = markdown_from_detail(detail.data);
    if (body.empty()) {
        reply.error = "skill detail did not include SKILL.md content";
        return reply;
    }

    const std::filesystem::path root = default_roaming_skills_root();
    const std::filesystem::path dir = root / slug;
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        reply.error = "could not create skills root";
        return reply;
    }
    const bool exists = skill_dir_exists(dir);
    if (exists && !force) {
        reply.error = "skill already exists: " + media::path::path_to_generic_utf8(dir) + " (pass --force to replace SKILL.md)";
        reply.name = slug;
        reply.path = media::path::path_to_generic_utf8(dir / "SKILL.md");
        return reply;
    }
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        reply.error = "could not create " + media::path::path_to_generic_utf8(dir);
        return reply;
    }
    const auto out_path = dir / "SKILL.md";
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        reply.error = "could not write " + media::path::path_to_generic_utf8(out_path);
        return reply;
    }
    out << body;
    if (!body.empty() && body.back() != '\n')
        out << '\n';
    if (!out.good()) {
        reply.error = "write failed: " + media::path::path_to_generic_utf8(out_path);
        return reply;
    }

    const std::filesystem::path workspace = default_workspace_skills_root({}) / slug;
    reply.ok = true;
    reply.replaced = exists;
    reply.shadowed_by_workspace = skill_dir_exists(workspace);
    reply.name = slug;
    reply.path = media::path::path_to_generic_utf8(out_path);
    return reply;
}

RemoveReply remove_roaming(const std::string& name_in)
{
    RemoveReply reply;
    const std::string name = trim_copy(name_in);
    if (!safe_dirname(name)) {
        reply.error = "skill name is not a safe folder name";
        return reply;
    }
    const std::filesystem::path root = default_roaming_skills_root();
    const std::filesystem::path dir = root / name;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        reply.error = "roaming skill not found: " + name;
        return reply;
    }
    if (!path_is_under(root, dir)) {
        reply.error = "refusing to remove a path outside the roaming skills root";
        return reply;
    }
    const auto parent = dir.parent_path();
    std::error_code parent_ec;
    const auto parent_abs = std::filesystem::weakly_canonical(parent, parent_ec);
    const auto root_abs = std::filesystem::weakly_canonical(root, ec);
    if (parent_ec || ec || parent_abs != root_abs) {
        reply.error = "refusing to remove a nested path";
        return reply;
    }
    const std::uintmax_t removed = std::filesystem::remove_all(dir, ec);
    if (ec || removed == 0) {
        reply.error = "could not remove " + media::path::path_to_generic_utf8(dir);
        return reply;
    }
    const std::filesystem::path workspace = default_workspace_skills_root({}) / name;
    reply.ok = true;
    reply.workspace_remains = skill_dir_exists(workspace);
    reply.name = name;
    reply.path = media::path::path_to_generic_utf8(dir);
    return reply;
}

} // namespace media::llm::skills::hub

#include "llm/agent_workflow.hpp"

#include <string>
#include <vector>

namespace media::llm::agent {
namespace {

const nlohmann::json* pick(const nlohmann::json& send, const nlohmann::json& w, const char* key)
{
    if (w.is_object() && w.contains(key))
        return &w[key];
    if (send.contains(key))
        return &send[key];
    return nullptr;
}

void append_text(std::string& dst, const std::string& extra)
{
    if (extra.empty())
        return;
    if (dst.empty()) {
        dst = extra;
        return;
    }
    if (dst.find(extra) != std::string::npos)
        return;
    dst += "\n";
    dst += extra;
}

std::vector<std::string> string_list(const nlohmann::json& v)
{
    std::vector<std::string> out;
    if (!v.is_array())
        return out;
    for (const auto& item : v) {
        if (item.is_string()) {
            const std::string s = item.get<std::string>();
            if (!s.empty())
                out.push_back(s);
        }
    }
    return out;
}

} // namespace

void apply_workflow_send_json(const nlohmann::json& send, Turn& turn)
{
    if (!send.is_object())
        return;

    nlohmann::json w = nlohmann::json::object();
    if (send.contains("workflow") && send["workflow"].is_object())
        w = send["workflow"];
    else if (send.contains("workflow") && send["workflow"].is_string())
        turn.workflow_id = send["workflow"].get<std::string>();

    if (const auto* v = pick(send, w, "id"); v && v->is_string())
        turn.workflow_id = v->get<std::string>();

    if (const auto* v = pick(send, w, "inject"); v && v->is_string())
        turn.workflow_inject = v->get<std::string>();

    if (const auto* v = pick(send, w, "system_extra"); v && v->is_string())
        append_text(turn.system_extra, v->get<std::string>());

    if (const auto* v = pick(send, w, "prompt_prefix"); v && v->is_string()) {
        const std::string prefix = v->get<std::string>();
        if (!prefix.empty() && turn.user_prompt.find(prefix) != 0)
            turn.user_prompt = prefix + turn.user_prompt;
    }
    if (const auto* v = pick(send, w, "prompt_suffix"); v && v->is_string()) {
        const std::string suffix = v->get<std::string>();
        if (!suffix.empty()) {
            const auto n = suffix.size();
            if (turn.user_prompt.size() < n
                || turn.user_prompt.compare(turn.user_prompt.size() - n, n, suffix) != 0)
                turn.user_prompt += suffix;
        }
    }

    if (const auto* v = pick(send, w, "create_video_ui_overrides"); v && v->is_object()) {
        if (!turn.create_video_ui_overrides.is_object())
            turn.create_video_ui_overrides = nlohmann::json::object();
        for (auto it = v->begin(); it != v->end(); ++it) {
            if (it->is_null())
                continue;
            turn.create_video_ui_overrides[it.key()] = it.value();
        }
    }

    if (const auto* v = pick(send, w, "allowed_path_tools"); v && v->is_array()) {
        turn.allowed_path_tools = string_list(*v);
        turn.planner_path_tool_allowlist_set = true;
    }
    if (const auto* v = pick(send, w, "planner_path_tool_allowlist_set"); v && v->is_boolean())
        turn.planner_path_tool_allowlist_set = v->get<bool>();
    if (const auto* v = pick(send, w, "disabled_path_tools"); v && v->is_array())
        turn.disabled_path_tools = string_list(*v);
    if (const auto* v = pick(send, w, "tool_policy_override_set"); v && v->is_boolean())
        turn.tool_policy_override_set = v->get<bool>();
    if (const auto* v = pick(send, w, "planner_enabled"); v && v->is_boolean())
        turn.planner_enabled = v->get<bool>();
    if (const auto* v = pick(send, w, "skills_enabled"); v && v->is_boolean())
        turn.skills_enabled = v->get<bool>();
    if (const auto* v = pick(send, w, "mcp_tools_enabled"); v && v->is_boolean())
        turn.mcp_tools_enabled = v->get<bool>();

    if (const auto* v = pick(send, w, "system_prompt_override"); v && v->is_string()) {
        turn.system_prompt_override = v->get<std::string>();
        turn.system_prompt_override_set = true;
    }
    if (const auto* v = pick(send, w, "system_prompt_override_set"); v && v->is_boolean())
        turn.system_prompt_override_set = v->get<bool>();
    if (const auto* v = pick(send, w, "system_prompt_is_null"); v && v->is_boolean())
        turn.system_prompt_is_null = v->get<bool>();

    if (const auto* v = pick(send, w, "planner_prompt_override"); v && v->is_string()
        && !turn.planner_prompt_override_set) {
        std::string raw = v->get<std::string>();
        if (!raw.empty()) {
            if (raw.find("${builtin}") == std::string::npos)
                raw = "${builtin}\n\n" + raw;
            turn.planner_prompt_override_set = true;
            turn.planner_prompt_override = std::move(raw);
        }
    }
}

} // namespace media::llm::agent

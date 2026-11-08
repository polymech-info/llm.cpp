#include "agent_runner_session.hpp"

#include "agent_memory.hpp"

#include <algorithm>
#include <cctype>
#include "string/string_utils.hpp"

namespace media::llm::agent {
namespace {

std::string runner_storage_key(AgentRunnerKind kind)
{
    return agent_runner_kind_name(kind);
}

std::string read_nested_string(const nlohmann::json& root,
                               std::initializer_list<const char*> path)
{
    const nlohmann::json* cur = &root;
    for (const char* key : path) {
        if (!cur->is_object() || !cur->contains(key))
            return {};
        cur = &(*cur)[key];
    }
    return cur->is_string() ? cur->get<std::string>() : std::string{};
}

} // namespace

std::optional<AgentRunnerKind> parse_agent_runner_kind(std::string_view raw)
{
    std::string s(raw);
    s = media::strutil::lower_ascii(std::move(s));
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t'))
        ++start;
    if (start > 0)
        s = s.substr(start);

    if (s.empty() || s == "native" || s == "default" || s == "pixlwiz")
        return AgentRunnerKind::Native;
    if (s == "codex")
        return AgentRunnerKind::Codex;
    if (s == "claude" || s == "claude-code" || s == "claude_code")
        return AgentRunnerKind::ClaudeCode;
    // if (s == "cursor" || s == "cursor-agent" || s == "agent")
    //     return AgentRunnerKind::Cursor; // incomplete; keep for later
    return std::nullopt;
}

std::string external_session_lookup(const std::string& pixlwiz_session_id,
                                    AgentRunnerKind runner)
{
    if (pixlwiz_session_id.empty() || runner == AgentRunnerKind::Native)
        return {};

    nlohmann::json memory_state, recent_events;
    session_load(pixlwiz_session_id, memory_state, recent_events, 0);
    return read_nested_string(memory_state,
                              {"external_agents", runner_storage_key(runner).c_str(), "session_id"});
}

void external_session_store(const std::string& pixlwiz_session_id,
                            AgentRunnerKind runner,
                            const std::string& external_session_id)
{
    if (pixlwiz_session_id.empty() || external_session_id.empty()
        || runner == AgentRunnerKind::Native)
        return;

    nlohmann::json patch{
        {"external_agents",
         {{runner_storage_key(runner), {{"session_id", external_session_id}}}}},
    };
    (void)task_store_write_memory(pixlwiz_session_id, patch);
}

std::string extract_external_session_id(const Result& result)
{
    if (!result.transcript.is_array())
        return {};

    for (auto it = result.transcript.rbegin(); it != result.transcript.rend(); ++it) {
        if (!it->is_object())
            continue;
        if (it->contains("external_session_id") && (*it)["external_session_id"].is_string())
            return (*it)["external_session_id"].get<std::string>();
        if ((*it).value("type", std::string{}) == "system"
            && (*it).value("subtype", std::string{}) == "init"
            && (*it).contains("session_id") && (*it)["session_id"].is_string())
            return (*it)["session_id"].get<std::string>();
    }
    return {};
}

} // namespace media::llm::agent

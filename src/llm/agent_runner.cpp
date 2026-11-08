#include "agent_runner.hpp"

#include "agent_factory.hpp"
#include "agent_runner_mcp.hpp"

#if FEATURE_AGENT_CODEX
#include "agent_runner_codex.hpp"
#endif
#if FEATURE_AGENT_CLAUDE
#include "agent_runner_claude_code.hpp"
#endif
#if 0 // Cursor runner incomplete; keep for later
#if FEATURE_AGENT_CURSOR
#include "agent_runner_cursor.hpp"
#endif
#endif

#include <string>

namespace media::llm::agent {

namespace {

void attach_tanit_mcp_if_needed(AgentRunnerOptions& runner)
{
    if (!runner.add_tanit_mcp_server || !runner.tanit_mcp_url.empty())
        return;
    if (runner.kind != AgentRunnerKind::Codex && runner.kind != AgentRunnerKind::ClaudeCode)
        return;
    const int port = ensure_tanit_mcp_listen_port();
    if (port > 0)
        runner.tanit_mcp_url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
}

} // namespace

bool agent_runner_kind_enabled(AgentRunnerKind kind)
{
    switch (kind) {
    case AgentRunnerKind::Native:     return true;
#if FEATURE_AGENT_CODEX
    case AgentRunnerKind::Codex:      return true;
#else
    case AgentRunnerKind::Codex:      return false;
#endif
#if FEATURE_AGENT_CLAUDE
    case AgentRunnerKind::ClaudeCode: return true;
#else
    case AgentRunnerKind::ClaudeCode: return false;
#endif
#if 0 // Cursor runner incomplete; keep for later
#if FEATURE_AGENT_CURSOR
    case AgentRunnerKind::Cursor:     return true;
#else
    case AgentRunnerKind::Cursor:     return false;
#endif
#else
    case AgentRunnerKind::Cursor:     return false;
#endif
    }
    return false;
}

const char* agent_runner_kind_name(AgentRunnerKind kind)
{
    switch (kind) {
    case AgentRunnerKind::Native:     return "native";
    case AgentRunnerKind::Codex:      return "codex";
    case AgentRunnerKind::ClaudeCode: return "claude-code";
    case AgentRunnerKind::Cursor:     return "cursor";
    }
    return "unknown";
}

Result run_agent_turn(const Turn&             turn,
                      const ProviderSettings& provider,
                      const AgentRunnerOptions& runner,
                      EventCallback           on_event)
{
    AgentFactory::ensure_app_search_path();

    if (runner.kind == AgentRunnerKind::Native)
        return AgentFactory::default_agent().run(turn, provider, on_event);

    if (!agent_runner_kind_enabled(runner.kind)) {
        Result r;
        r.error = std::string("agent runner '") + agent_runner_kind_name(runner.kind)
                + "' is not enabled in this build (see FEATURE_AGENT_* in CMakePresets.json)";
        if (on_event)
            on_event(Event{Event::Error, r.error, {}, {}});
        return r;
    }

    AgentRunnerOptions effective = runner;
    attach_tanit_mcp_if_needed(effective);

#if FEATURE_AGENT_CODEX
    if (effective.kind == AgentRunnerKind::Codex)
        return run_codex_agent_turn(turn, effective, on_event);
#endif

#if FEATURE_AGENT_CLAUDE
    if (effective.kind == AgentRunnerKind::ClaudeCode)
        return run_claude_code_agent_turn(turn, effective, on_event);
#endif

#if 0 // Cursor runner incomplete; keep for later
#if FEATURE_AGENT_CURSOR
    if (effective.kind == AgentRunnerKind::Cursor)
        return run_cursor_agent_turn(turn, effective, on_event);
#endif
#endif

    Result r;
    r.error = std::string("agent runner '") + agent_runner_kind_name(runner.kind) + "' is not implemented yet";
    if (on_event)
        on_event(Event{Event::Error, r.error, {}, {}});
    return r;
}

} // namespace media::llm::agent

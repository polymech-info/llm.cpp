#pragma once
//
// External CLI agent runners (Codex, Claude Code, Cursor) — same Turn / Event /
// Result contract as AgentFactory::run().
//

#include "agent.hpp"

#include <string>
#include <vector>

#ifndef FEATURE_AGENT_CODEX
#define FEATURE_AGENT_CODEX 0
#endif
#ifndef FEATURE_AGENT_CLAUDE
#define FEATURE_AGENT_CLAUDE 0
#endif
#ifndef FEATURE_AGENT_CURSOR
#define FEATURE_AGENT_CURSOR 0
#endif

namespace media::llm::agent {

enum class AgentRunnerKind {
    Native,
    Codex,
    ClaudeCode,
    Cursor,
};

struct AgentRunnerOptions {
    AgentRunnerKind kind = AgentRunnerKind::Native;
    std::string model;
    /// Codex configuration profile layered from $CODEX_HOME/<name>.config.toml.
    std::string profile;
    /// Trusted one-run Codex TOML overrides (`key=value`), passed as repeated `-c`.
    std::vector<std::string> config_overrides;
    /// Extra writable roots for Codex (`--add-dir`).
    std::vector<std::string> additional_dirs;
    /// Codex: read-only | workspace-write | danger-full-access
    std::string sandbox = "read-only";
    /// Codex: never | on-request. Headless Tanit defaults to never.
    std::string approval = "never";
    /// Existing Tanit Streamable HTTP MCP endpoint, injected for this run only.
    std::string tanit_mcp_url;
    /// Start Tanit's loopback MCP on demand and hand it to Codex/Claude.
    /// Callers may still set tanit_mcp_url explicitly; that wins.
    bool add_tanit_mcp_server = true;
    /// Cursor: plan | ask (empty = default agent mode)
    std::string mode;
    bool ephemeral           = false;
    bool ignore_user_config  = false;
    bool ignore_rules        = false;
    bool trust_workspace     = false;
    bool skip_git_repo_check = true;
    bool bare_startup        = true;
    int  timeout_ms          = 600000;
    std::string resume_id;
    std::string executable_override;
};

PM_CHAT_API bool agent_runner_kind_enabled(AgentRunnerKind kind);
PM_CHAT_API const char* agent_runner_kind_name(AgentRunnerKind kind);

/// Dispatch to native AgentFactory or an external CLI runner.
PM_CHAT_API Result run_agent_turn(const Turn&             turn,
                                   const ProviderSettings& provider,
                                   const AgentRunnerOptions& runner,
                                   EventCallback           on_event = nullptr);

} // namespace media::llm::agent

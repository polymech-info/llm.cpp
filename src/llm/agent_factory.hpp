#pragma once
//
// Agent factory — compose an Agent from a list of features.
//
// Usage:
//
//   // Default agent (all enabled standard features):
//   auto result = AgentFactory::default_agent().run(turn, provider, cb);
//
//   // Custom agent (tools only, no memory, custom prompt):
//   Agent a(AgentOptions{
//       .features = {
//           AgentFactory::feature_path_scope(),
//           AgentFactory::feature_tools(),
//           my_custom_prompt_feature(),
//       }
//   });
//   auto result = a.run(turn, provider, cb);
//
// The feature ordering matters:
//   PathScopeFeature    — must come first (blocklist active before tools are built)
//   PlannerFeature      — optional compact preselection of path-tool schemas
//   ToolsFeature        — builds path-catalog tools JSON (no MCP)
//   HostToolsFeature    — merges IAgentToolProvider schemas + context
//   McpFeature          — optional; merges mcp.json tools (later, not OpenDesign)
//   MemoryFeature       — installs thread-local task-ID scope
//   SystemPromptFeature — runs last so it sees the fully assembled Turn and tools
//
#include "agent_feature.hpp"
#include "llm/agent_tool_provider.hpp"

#include <memory>
#include <vector>

#ifndef FEATURE_HOME_LLM_TOOLS
#define FEATURE_HOME_LLM_TOOLS 1
#endif
#ifndef FEATURE_HOME_LLM_SKILLS
#define FEATURE_HOME_LLM_SKILLS 1
#endif
#ifndef FEATURE_COMMAND_LLAMA_INFERENCE
#define FEATURE_COMMAND_LLAMA_INFERENCE 1
#endif

namespace media::llm::agent {

// ── Options object ────────────────────────────────────────────────────────────

struct AgentOptions {
    std::vector<std::shared_ptr<IAgentFeature>> features;
    /// Extra host providers for this Agent instance (merged with the process registry).
    std::vector<std::shared_ptr<IAgentToolProvider>> tool_providers;
};

/// Optional parts of the standard agent assembly. Custom AgentOptions callers
/// can still compose features directly; this controls only default_agent().
struct DefaultAgentOptions {
    bool tool_checkpoints_enabled = true;
};

// ── Agent ─────────────────────────────────────────────────────────────────────

class Agent {
public:
    explicit Agent(AgentOptions opts) : opts_(std::move(opts)) {}

    /// Synchronous turn. Blocks until done, cancelled, or max_iterations hit.
    /// On_event follows the same contract as the legacy run_turn signature.
    Result run(const Turn&            turn,
               const ProviderSettings& provider,
               EventCallback           on_event = nullptr) const;

private:
    AgentOptions opts_;
};

// ── Factory helpers ───────────────────────────────────────────────────────────

namespace AgentFactory {

/// Standard built-in features (available individually for custom assemblies).
std::shared_ptr<IAgentFeature> feature_path_scope();    ///< Path-base + blocklist RAII
#if FEATURE_HOME_LLM_TOOLS
std::shared_ptr<IAgentFeature> feature_planner();       ///< Optional compact planner -> path-tool allowlist
std::shared_ptr<IAgentFeature> feature_tools();         ///< Path catalog (disabled tools excluded)
std::shared_ptr<IAgentFeature> feature_host_tools();    ///< IAgentToolProvider schemas + context
#endif
#if FEATURE_HOME_LLM_TOOLS && FEATURE_MCP_CLIENT
std::shared_ptr<IAgentFeature> feature_mcp();           ///< MCP bridge + tool merge (runs after feature_tools)
#endif
std::shared_ptr<IAgentFeature> feature_memory();        ///< Thread-local task-ID scope + memory persist on_finish
std::shared_ptr<IAgentFeature> feature_tool_checkpoints(); ///< Replaceable tool outcome checkpoints
std::shared_ptr<IAgentFeature> feature_memory_recall(); ///< Hybrid recall index + pending inject (opt-in flags)
std::shared_ptr<IAgentFeature> feature_snapshot();      ///< P12: load/save named session snapshots
#if FEATURE_HOME_LLM_SKILLS
std::shared_ptr<IAgentFeature> feature_skills();        ///< Discover skills + inject summary into system text
#endif
std::shared_ptr<IAgentFeature> feature_agent_md();      ///< Inject Agent.md from the current folder
std::shared_ptr<IAgentFeature> feature_workflow();     ///< Filmstrip / workflow context inject
std::shared_ptr<IAgentFeature> feature_system_prompt(); ///< Assemble canonical system prompt
std::shared_ptr<IAgentFeature> feature_security_prompt(); ///< Assemble AI-consent security prompt
std::shared_ptr<IAgentFeature> feature_self_check();    ///< P9: post-turn structural quality gate (Finish phase)
#if FEATURE_COMMAND_LLAMA_INFERENCE
/// Scope(10): lazy-init embedding model and register as strutil backend.
/// No-op when the model file is not present (fuzzy fallback remains active).
std::shared_ptr<IAgentFeature> feature_embed_inference();
#endif

/// Prepend the saved app-path directories onto this process PATH.
/// Called at the start of every agent run so Claude, Codex, and other child
/// CLIs resolve from the folders registered in settings.
void ensure_app_search_path();

/// Fully-assembled default agent (PathScope -> optional tools/MCP/skills -> Memory -> SystemPrompt).
/// Tool checkpoints can be disabled without disabling session memory itself.
Agent default_agent();
Agent default_agent(DefaultAgentOptions options);

/// Text-only agent for AI consent review. No tools, MCP, skills, planner,
/// memory, snapshots, or self-check.
Agent security_agent();

} // namespace AgentFactory

} // namespace media::llm::agent

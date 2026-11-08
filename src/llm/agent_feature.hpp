#pragma once
//
// Agent feature interface — composable building blocks for agent behaviour.
//
// An IAgentFeature has two hook points:
//
//   on_prepare(turn, provider, run)
//     Called once before the LLM loop starts.
//     Features may:
//       - Modify `turn`  (e.g. inject memory state, override selection)
//       - Populate/augment `run.tools`
//       - Set `run.system_prompt` (SystemPromptFeature) or append to it
//       - Wrap `run.cb` to intercept events
//       - Install RAII scopes via run.add_scope(…) — held for the full run
//       - Attach an MCP bridge via run.mcp_bridge
//
//   on_finish(turn, result, run)
//     Called after the loop exits (success, error, or cancelled).
//     E.g. auto-memory append, telemetry flush.
//
// AgentFactory::default_agent() assembles the standard feature set.
// Pass a custom AgentOptions to suppress or replace individual features.
//

#include "agent.hpp"
#include "llm/agent_tool_provider.hpp"
#include "llm/tools/mcp/McpBridge.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace media::llm::agent {

// ── RAII scope base ───────────────────────────────────────────────────────────

/// Opaque lifetime guard returned by IAgentFeature::on_prepare.
/// Destructor runs after the LLM loop exits, before on_finish.
struct FeatureScope {
    virtual ~FeatureScope() = default;
    FeatureScope()                               = default;
    FeatureScope(const FeatureScope&)            = delete;
    FeatureScope& operator=(const FeatureScope&) = delete;
};

// ── Run context — assembled by features before the loop ───────────────────────

struct PreparedRun {
    /// System prompt (assembled by SystemPromptFeature or set directly).
    std::string system_prompt;
    bool        system_prompt_set     = false;
    bool        system_prompt_is_null = false;
    nlohmann::json prompt_meta        = nlohmann::json::object();

    /// Tools JSON array sent to the LLM (assembled by ToolsFeature + host providers).
    nlohmann::json tools = nlohmann::json::array();

    /// Resolved host / app providers for this run (see IAgentToolProvider).
    std::vector<std::shared_ptr<IAgentToolProvider>> host_providers;

    /// OpenAI function schemas already merged from host_providers (kept across
    /// request_tools rebuilds of the path catalog).
    nlohmann::json host_tool_schemas = nlohmann::json::array();

    /// Path-tool names that were granted after the planner seed. The loop
    /// appends to this when the executioner calls request_tools, then rebuilds
    /// tools[] for the next LLM round.
    std::vector<std::string> progressive_granted_path_tools;

    /// Optional MCP bridge (created by ToolsFeature when mcp.json is present).
    std::unique_ptr<mcp::AgentMcpBridge> mcp_bridge;

    /// Full catalog of MCP tool schemas available this turn (OpenAI function
    /// shape, already filtered for disabled servers/tools). MCP tools are NOT
    /// shipped in `tools` up front — the executioner grants them on demand via
    /// `request_tools` (mcp_* names), which copies the matching schema from here
    /// into `tools`. Discovery of names is via info_lookup (kind=mcp).
    nlohmann::json mcp_tool_catalog = nlohmann::json::array();

    /// Effective event callback — features may wrap this to intercept events.
    EventCallback cb;

    /// RAII scopes held until the end of Agent::run() (path base, blocklist, etc.).
    std::vector<std::unique_ptr<FeatureScope>> scopes;

    void add_scope(std::unique_ptr<FeatureScope> s) {
        if (s) scopes.push_back(std::move(s));
    }
};

// ── P8: Feature phase — ordering contract ─────────────────────────────────────
//
// Each feature declares the phase it belongs to. AgentFactory::default_agent()
// asserts at construction that the feature list is phase-ordered (non-decreasing).
// Multiple features may share a phase (e.g. Tools=30 and Mcp=30 both expand tools).
// Custom features that don't override phase() default to Context=40.
//
enum class FeaturePhase : uint8_t {
    Scope   = 10,   // RAII environment guards (path base, blocklist)
    Plan    = 20,   // pre-LLM selection, intent classification (planner)
    Tools   = 30,   // tool schema assembly (path catalog + MCP merge)
    Context = 40,   // memory, skills, external context injection
    Prompt  = 50,   // final system prompt assembly
    Finish  = 60,   // post-run hooks (memory persist, telemetry)
};

// ── Feature interface ─────────────────────────────────────────────────────────

class IAgentFeature {
public:
    virtual ~IAgentFeature() = default;

    /// Phase this feature belongs to. Used by AgentFactory to verify ordering.
    /// Default: Context (40) — safe middle-ground for unknown features.
    virtual FeaturePhase phase() const noexcept { return FeaturePhase::Context; }

    /// Short name for prepare-timing logs. Default is fine for custom features.
    virtual const char* name() const noexcept { return "feature"; }

    /// Setup phase — called once before the LLM loop.
    /// `turn` is a mutable working copy; `provider` is const.
    virtual void on_prepare(Turn& turn,
                            const ProviderSettings& provider,
                            PreparedRun& run) {}

    /// Post-run phase — called after the loop exits (any outcome).
    virtual void on_finish(const Turn& turn,
                           const Result& result,
                           const PreparedRun& run) {}
};

#if FEATURE_HOME_LLM_TOOLS
/// Run the opt-in planner pass and mutate `turn.allowed_path_tools` /
/// `turn.planner_meta`. Used by PlannerFeature and CLI dry-run preview.
PM_CHAT_API void apply_planner_tool_selection(Turn& turn, const ProviderSettings& provider);
#endif

} // namespace media::llm::agent

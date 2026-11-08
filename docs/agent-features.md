# Agent Framework — Feature Reference

Documents the composable feature pipeline used by `AgentFactory::default_agent()`,
including all improvements shipped as part of the SuperClaude-inspired refactor
(P1–P8). Written as a reference rather than a design doc — see §7 for the original
analysis that motivated these changes.

Related docs:
- [llm-agent-planner.md](llm-agent-planner.md)
- [llm-agent-factory.md](llm-agent-factory.md)
- [llm-agent-memory-overview.md](llm-agent-memory-overview.md)

---

## 1. Feature Pipeline

`AgentFactory::default_agent()` composes a fixed ordered chain. Each feature
implements `IAgentFeature` and declares its `FeaturePhase` (P8).

```
PathScopeFeature    Scope(10)   — pin base path + blocklist (RAII)
PlannerFeature      Plan(20)    — compact index → intent + tool allowlist
ToolsFeature        Tools(30)   — build OpenAI schema array
McpFeature          Tools(30)   — merge mcp.json tools + create bridge
MemoryFeature       Context(40) — thread-local task scope + artifact persistence
SnapshotFeature     Context(40) — P12 load/save named snapshots (after Memory)
SkillsFeature       Context(40) — inject active SKILL.md bodies
SystemPromptFeature Prompt(50)  — assemble final system prompt
SelfCheckFeature    Finish(60)  — P9 structural quality gate (last to run)
```

The `Agent::run()` loop calls `on_prepare(Turn&, ProviderSettings&, PreparedRun&)` on
each feature in order, then runs the LLM+tool loop, then calls `on_finish` on each
feature in reverse-order order.

---

## 2. Turn Structure

`Turn` is the central input to every agent run. Key fields relevant to the
feature set below:

```cpp
struct Turn {
    std::string user_prompt;
    std::vector<std::string> selection;
    std::string folder_hint;

    // P1 — populated by PlannerFeature after apply_planner_tool_selection()
    nlohmann::json planner_meta;               // intent, action, subject, confidence, …
    std::vector<std::string> allowed_path_tools;
    bool planner_path_tool_allowlist_set = false;

    // P3 — set by PlannerFeature from planner_meta.intent; callers may override
    TurnMode mode = TurnMode::Auto;

    // P4 — overrides planner_enabled; defaults to Auto (falls back to bool)
    PlannerStrategy planner_strategy = PlannerStrategy::Auto;
    bool planner_enabled = false;
    int  planner_schema_budget = 8;

    // Structured memory, session scope, scheduled-task context …

    // P6 — parallel tool dispatch (on by default)
    bool parallel_tool_dispatch = true;

    // P9 — post-turn quality gate
    bool self_check_enabled = true;

    // P12 — named session snapshots
    std::string load_snapshot_name;  // non-empty: load before run
    std::string save_snapshot_name;  // non-empty: save after run
};
```

---

## 3. Implemented Features (P1–P8)

### P1 — Planner context packet → main agent

**What**: The planner JSON response now carries `intent`, `action`, `subject`, and
`confidence` alongside `selected_tools`. `PlannerFeature` promotes them into
`turn.planner_meta`. `SystemPromptFeature` renders a `## Turn Context` block from
these fields so the main agent receives typed routing intent rather than re-deriving
it from prose.

**Key fields** (all optional — absent = planner did not classify):

| Field | Example values | Injected into prompt |
|-------|---------------|----------------------|
| `intent` | `"query"` `"execute"` `"followup"` `"write"` `"spawn"` `"create"` | `- intent: followup` |
| `action` | `"status_check"` `"file_write"` `"run_command"` | `- action: status_check` |
| `subject` | `"video"` `"report.md"` `"mic"` | `- subject: video` |
| `confidence` | 0.0–1.0 | warning injected if `< 0.5` |

**Prompt template**: `dist/data/prompts/planner-prompt.md` — `## Output` shape and
`### Intent fields` taxonomy table.

**Files**: `agent_feature.cpp` (`apply_planner_tool_selection`), `agent_context.cpp`
(`build_system_prompt_impl`), `planner-prompt.md`.

---

### P2 — Planner confidence signal + `ClarificationNeeded` event

**What**: After `apply_planner_tool_selection`, if the planner ran successfully
(`fallback=false`) and returned `confidence < 0.4`, `PlannerFeature` emits an
`Event::ClarificationNeeded` with a payload carrying confidence, intent, action,
subject, and reason. The turn still proceeds — callers may intercept this event to
surface an inline hint before the first LLM round.

**New event kind** (in `agent.hpp`):
```cpp
Event::ClarificationNeeded  // payload: { confidence, intent, action, subject, reason }
```

**Files**: `agent.hpp` (event kind), `agent_feature.cpp` (PlannerFeature::on_prepare).

---

### P3 — `TurnMode` enum + mode-specific prompt overlays

**What**: A `TurnMode` enum controls which behavioral overlay `.md` file is injected
into the system prompt. `PlannerFeature` derives the mode from the planner's `intent`
field. `SystemPromptFeature` loads `mode-<name>.md` from the prompts directory and
injects a `## Behavioral Mode: <name>` block after `## Turn Context`.

**Enum** (in `agent.hpp`):

| Value | Mapped from `intent` | Overlay file |
|-------|---------------------|--------------|
| `Auto` | (default / unrecognized) | none |
| `Lookup` | `"query"` | `mode-lookup.md` |
| `Execute` | `"execute"` | `mode-execute.md` |
| `Research` | `"research"` | `mode-research.md` (optional) |
| `Orchestrate` | `"spawn"` | `mode-orchestrate.md` (optional) |
| `Create` | `"write"` / `"create"` | `mode-create.md` |
| `FollowUp` | `"followup"` | `mode-followup.md` |

Overlay files live in `dist/data/prompts/` (beside-exe wins; `config_dir/prompts/`
is the fallback — same search path as `planner-prompt.md`). Missing files are a
graceful no-op. Callers may set `turn.mode` directly to bypass the planner derivation.

**Files**: `agent.hpp`, `agent_feature.cpp` (PlannerFeature intent→mode mapping),
`agent_context.cpp`, `agent_commons.cpp` (`turn_mode_name`, `load_mode_overlay_md_if_present`),
`agent_internal.hpp`, `dist/data/prompts/mode-*.md`.

---

### P4 — `PlannerStrategy` enum

**What**: Replaces the blunt `planner_enabled` bool with a typed strategy field.
`planner_strategy` takes precedence; when `Auto`, falls back to `planner_enabled`
for backward compatibility — no existing call sites changed.

**Enum** (in `agent.hpp`):

| Value | Behaviour |
|-------|-----------|
| `Auto` | Falls back to `planner_enabled` bool |
| `Disabled` | Skip planner entirely |
| `IntentOnly` | Run LLM call; extract intent/action/subject/confidence; skip tool-schema expansion and `allowed_path_tools` |
| `Full` | Full planner pass (equivalent to `planner_enabled=true`) |

`IntentOnly` exits after P1 intent promotion, so P3 `TurnMode` derivation still fires.
`planner_meta["strategy"]` is set to `"full"` / `"intent_only"` / `"disabled"` for
log visibility.

**Files**: `agent.hpp`, `agent_feature.cpp` (`apply_planner_tool_selection`).

---

### P5 — Error-reflexion sink in structured memory

**What**: When a tool call returns `ok=false` and the whole turn fails or is cancelled,
`MemoryFeature::on_finish` writes a `kind=error_reflexion` session item. Future planner
calls see this item via the memory injection block and can route around the same
failure pattern.

**Item shape**:
```json
{ "kind": "error_reflexion", "error_tool": "…", "error_message": "…",
  "user_prompt_summary": "…", "iterations": N }
```

Gated by a new `CaptureErrorReflexions = 1u << 12` bit in `StructuredMemoryFeature`
(included in `k_default_structured_memory_features`).

**New event kind**:
```cpp
Event::ErrorReflexion  // payload: { error_tool, error_message, user_prompt_summary, iterations }
```

**Files**: `agent.hpp` (event kind), `llm_constants.hpp` (feature bit),
`agent_memory_artifacts.hpp/.cpp` (`session_persist_error_reflexion`),
`agent_feature.cpp` (MemoryFeature::on_finish).

---

### P7 — `AgentChildLaunchSpec` builder

**What**: Centralises the canonical child-agent shell command in one struct and one
function, replacing four duplicated prose strings that had drifted out of sync (one
used `--log-level none` instead of `error`).

**API** (`src/llm/agent_child_launch.hpp`):

```cpp
struct AgentChildLaunchSpec {
    std::string log_level   = "error";
    std::string preset      = "superfast";
    bool        single_turn = true;
    std::string prompt;
    std::string session_id;
    std::string extra_args;
};

std::string build_child_shell_command(const AgentChildLaunchSpec&, ShellDialect);
std::string child_launch_canonical_example(ShellDialect = PowerShell);
```

`child_launch_canonical_example()` returns:
```
`tanit-cli.exe --log-level error llm agent --preset superfast --single-turn --prompt "<child prompt>"`
```

**Injection sites** (all now call `child_launch_canonical_example()`):

| File | Context |
|------|---------|
| `path_tool_catalog.cpp` | `make_run()` tool description |
| `llm_info_compact.cpp` | Commands section spawn hint |
| `llm_info_compact.cpp` | Presets section narrow-child hint |
| `agent_context.cpp` | Spawn-child routing guideline bullet |

**Files**: `src/llm/agent_child_launch.hpp`, `src/llm/agent_child_launch.cpp`,
`CMakeLists.txt` (source entry), all four injection sites above.

---

### P8 — `FeaturePhase` enum + ordering assertion

**What**: Each `IAgentFeature` subclass declares its phase. `AgentFactory::default_agent()`
walks the assembled list and emits `logger::warn` for any out-of-order feature. Non-fatal
runtime check that fires at startup, not silently at the first misbehaving turn.

**Enum** (`agent_feature.hpp`):

```cpp
enum class FeaturePhase : uint8_t {
    Scope   = 10,   // RAII environment guards
    Plan    = 20,   // pre-LLM selection + intent
    Tools   = 30,   // tool schema assembly
    Context = 40,   // memory, skills, context injection  ← default for unknown features
    Prompt  = 50,   // final system prompt assembly
    Finish  = 60,   // post-run hooks
};
```

**Phase assignments**:

| Feature | Phase |
|---------|-------|
| `PathScopeFeature` | Scope (10) |
| `PlannerFeature` | Plan (20) |
| `ToolsFeature` | Tools (30) |
| `McpFeature` | Tools (30) |
| `MemoryFeature` | Context (40) |
| `SkillsFeature` | Context (40) |
| `SystemPromptFeature` | Prompt (50) |

Custom features that don't override `phase()` get `Context=40` — a safe middle-ground.

**Files**: `agent_feature.hpp` (enum + virtual method), `agent_feature.cpp` (overrides),
`agent_factory.cpp` (ordering check in `default_agent()`), `test-agent.mjs` (P8
stderr assertion in main runner loop).

---

## 4. Implemented — P6, P9, P12

When the LLM returns N > 1 tool calls in a single response and
`turn.parallel_tool_dispatch = true`, all calls are dispatched concurrently via
`std::async(launch::async)`. Results are collected and emitted in original model
order, so the event stream is deterministic.

**API** (`src/llm/agent.hpp`):

```cpp
// P6 — off by default; requires re-entrant path tools and mcp_bridge::call_tool.
bool parallel_tool_dispatch = false;
```

**CLI** (guarded by `FEATURE_HOME_LLM_TOOLS`):

```
# on by default — opt out with:
tanit-cli.exe llm agent --no-parallel-tools --prompt "..."
```

**Implementation** (`agent_factory.cpp` dispatch section):

```
if (parallel_tool_dispatch && tool_calls.size() > 1) {
    1. Cancel check (one-shot, before burst).
    2. Capture all inputs by value (id, name, args, create_video ui overrides).
    3. Emit ToolCall events in model order (serial, before any execution).
    4. std::async(launch::async) for each call:
         • path tools  → path::execute()
         • MCP tools   → mcp_bridge->call_tool()
    5. Join futures in original order; emit ToolResult, journal, append messages.
} else {
    // serial loop (unchanged)
}
```

**Known limitations**:
- Per-file progress streaming (`image_transform`, `run` chunk events) is suppressed
  in parallel mode — those hooks are single-threaded state. Final envelopes are
  complete.
- `path::execute` and `mcp_bridge::call_tool` are assumed re-entrant. If two parallel
  calls target the same output file the behaviour is undefined (user responsibility).
- Providers that return only one `tool_call` per response (most models) see no change.

**Files**: `src/llm/agent.hpp` (field), `src/llm/agent_factory.cpp` (parallel branch),
`src/cli/pm_image_cli_state.hpp` (`ag_parallel_tools`),
`src/cli/pm_image_cmd_llm_agent.cpp` (flag wire-up + CLI registration).

---

### P9 — `SelfCheckFeature` (post-turn quality gate)

**What**: After every turn, a lightweight `on_finish` pass scans the completed
transcript without any LLM call. It counts `ok=false` tool results, tests the
final reply text against a set of deflection phrases, and derives a
`completion_confidence` score. An `Event::SelfCheckResult` is emitted, and when
`CaptureSelfCheck` is set in the memory feature mask a `kind=self_check` session
item is written so future planner calls can see quality trends across turns.

**Event payload**:
```json
{
  "passed":               true,
  "tool_ok_rate":         1.0,
  "deflection_detected":  false,
  "final_text_chars":     142,
  "total_tools":          2,
  "failed_tools":         0,
  "completion_confidence": 1.0,
  "issues":               []        // populated on failure
}
```

**Feature phase**: `Finish(60)` — runs after all context / prompt features, so it
sees the fully committed session state.

**Deflection phrases checked** (case-sensitive, substring match):
`"I cannot"`, `"I'm unable"`, `"I am unable"`, `"I don't have"`,
`"I do not have"`, `"cannot complete"`, `"not able to"`.

**`completion_confidence`** = `tool_ok_rate × (0.5 if deflection) × (1 if ok else 0)`.

**Controlling persistence** (`llm_constants.hpp`):
```cpp
StructuredMemoryFeature::CaptureSelfCheck = 1u << 13  // included in k_default
```

**Disable** per-turn: `turn.self_check_enabled = false`.

**Files**: `src/llm/agent.hpp` (event kind + `Turn::self_check_enabled`),
`src/llm/llm_constants.hpp` (`CaptureSelfCheck`),
`src/llm/agent_feature.cpp` (`SelfCheckFeature`),
`src/llm/agent_factory.hpp/.cpp` (`feature_self_check`),
`src/cli/pm_image_cmd_llm_agent.cpp` (`case K::SelfCheckResult`).

---

### P12 — Named session snapshots

**What**: Two new CLI flags allow a workflow's session state (memory items +
recent events) to be saved to a named file and restored in a later invocation.

```
# Save at the end of a turn:
tanit-cli llm agent --multi-turn --save-snapshot my-workflow --prompt "…"

# Restore at the start of a future turn:
tanit-cli llm agent --multi-turn --load-snapshot my-workflow --prompt "…"
```

**Disk layout**:
```
<config_dir>/snapshots/<sanitized-name>.json
```
Adjacent to the sessions dir (inside the same `<config_dir>/` root). In the test
harness the session dir is overridden via `PM_IMAGE_SESSION_DIR`; snapshot dir
follows automatically (parent / `snapshots`).

**Snapshot format**:
```json
{
  "name": "my-workflow",
  "saved_at": "2026-05-29T18:01:37Z",
  "source_session_id": "case-snapshot-mpr41vao",
  "memory_state": { … },
  "recent_events": [ … ]
}
```

**`SnapshotFeature` (Context phase = 40, after `MemoryFeature`)**:
- `on_prepare`: merges snapshot `memory_state` (session keys win) and prepends
  snapshot `recent_events` (capped at 30 total) into the Turn before
  `SystemPromptFeature` builds the system prompt.
- `on_finish`: writes the current session state to the snapshot file after
  `MemoryFeature` has already persisted this turn's artifacts.

**Requirements**: `--save-snapshot` requires `--multi-turn` (turn needs a
`task_id`); `--load-snapshot` works with any session mode but only injects
items when the turn has a `task_id` (i.e. `--multi-turn` is active).

**Files**: `src/llm/agent.hpp` (`Turn::load/save_snapshot_name`),
`src/llm/agent_memory.hpp/.cpp` (`session_snapshot_save/load`, `snapshot_store_dir`),
`src/llm/agent_feature.cpp` (`SnapshotFeature`),
`src/llm/agent_factory.hpp/.cpp` (`feature_snapshot`),
`src/cli/pm_image_cli_state.hpp` (`ag_load/save_snapshot`),
`src/cli/pm_image_cmd_llm_agent.cpp` (CLI registration + wire-up).

---

## 5. Status Summary

| # | Feature | Status | Phase | Files |
|---|---------|--------|-------|-------|
| P1 | Planner context packet → main agent | ✅ | Plan→Prompt | `agent_feature.cpp`, `agent_context.cpp`, `planner-prompt.md` |
| P2 | Planner confidence + `ClarificationNeeded` | ✅ | Plan | `agent.hpp`, `agent_feature.cpp` |
| P3 | `TurnMode` + mode overlays | ✅ | Plan→Prompt | `agent.hpp`, `agent_feature.cpp`, `agent_context.cpp`, `agent_commons.cpp`, `mode-*.md` |
| P4 | `PlannerStrategy` enum | ✅ | Plan | `agent.hpp`, `agent_feature.cpp` |
| P5 | Error-reflexion sink | ✅ | Finish | `agent.hpp`, `llm_constants.hpp`, `agent_memory_artifacts.hpp/.cpp` |
| P6 | Optional parallel tool dispatch | ✅ | Tools | `agent_factory.cpp`, `agent.hpp`, `pm_image_cmd_llm_agent.cpp` |
| P7 | `AgentChildLaunchSpec` builder | ✅ | — | `agent_child_launch.hpp/.cpp`, 4× injection sites |
| P8 | `FeaturePhase` enum + ordering assertion | ✅ | — | `agent_feature.hpp`, `agent_factory.cpp` |
| P9 | `SelfCheckFeature` quality gate | ✅ | Finish(60) | `agent.hpp`, `llm_constants.hpp`, `agent_feature.cpp`, `agent_factory.cpp` |
| P12 | Named session snapshots | ✅ | Context(40) | `agent_memory.hpp/.cpp`, `agent_feature.cpp`, `agent_factory.cpp`, CLI |

---

## 6. Smoke Test

All implemented features are covered by `npm run test:agent:planner-context`
(three-round multi-turn run: R1=query/lookup, R2=execute/mic, R3=followup/video).

Key assertions per round:

| Check | Covers |
|-------|--------|
| `planner_meta.intent` populated | P1 |
| `## Turn Context` block in system prompt | P1 |
| `## Behavioral Mode:` block in system prompt | P3 |
| No `ClarificationNeeded` event when confidence ≥ 0.4 | P2 |
| No `ErrorReflexion` event on successful turn | P5 |
| No `feature[…] out of order` in stderr | P8 |
| R3 intent is `"followup"`, subject contains `"video"` | P1+P3 |

P6 is covered by `case-parallel-tools` (`npm run test:agent:parallel-tools`). That case
asks the agent to read two fixture files in one shot; when the model batches both
`file_read` calls into a single response the burst pattern is verified
(`✓ 2 tool calls dispatched in parallel`). The test passes silently if the provider
returns calls one-at-a-time (no burst = serial fallback, still correct).

P9 is covered by `case-self-check` (`npm run test:agent:self-check`). The agent writes
a small file and finishes normally. The harness asserts `self_check_events.length > 0`,
`passed=true`, and `deflection_detected=false`.

P12 is covered by `case-snapshot` (`npm run test:agent:snapshot`). Round 1 writes a
fixture file and saves a snapshot via `--save-snapshot`. Round 2 loads the snapshot
via `--load-snapshot`, reads the fixture file, and replies with the content sentinel.
The harness asserts the snapshot JSON file exists on disk and round 2's reply contains
`SNAP_CONTENT`.

Test script: `tests/orchestrator/test-agent.mjs`, case `case-planner-context`.

---

## 7. Background — Original Analysis

*Why these changes were made.*

### Our framework before this refactor

- Planner output flowed only into `turn.allowed_path_tools` — the main agent had no
  typed intent from the planner, only the inferred tool schemas.
- Follow-up turns ("and the video?") required the main agent to re-derive intent from
  memory + prose guidelines, which regressed whenever the system prompt drifted.
- No confidence gate between planner selection and the first LLM round.
- No error-reflexion — failed turns left no structured trace for future planner calls.
- Child-agent spawn guidance was duplicated in four places and had drifted (`--log-level
  none` vs `error`).
- Feature ordering was documented but not enforced.

### SuperClaude constructs that informed the design

| Construct | What we took |
|-----------|-------------|
| Behavioral modes (7 named modes) | `TurnMode` enum + `mode-*.md` overlays (P3) |
| Confidence-first (5-dim check) | Single confidence field + `ClarificationNeeded` event (P2) |
| Adaptive planning (3 strategies) | `PlannerStrategy` enum: Disabled / IntentOnly / Full (P4) |
| Intent/context packet | `planner_meta` intent fields → `## Turn Context` block (P1) |
| ReflexionMemory | `kind=error_reflexion` session items (P5) |
| `/sc:spawn` typed task hierarchy | `AgentChildLaunchSpec` + single canonical example (P7) |
| PRINCIPLES.md phase labels | `FeaturePhase` enum + ordering assertion (P8) |

*Original full analysis in git history (commit message: "docs: agent-features.md
SuperClaude gap analysis and P1–P8 proposals").*

---

*Last updated: 2026-05-29 — P9 (SelfCheckFeature) and P12 (named snapshots) added*

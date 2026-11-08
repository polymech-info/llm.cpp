# Agent Launch Status

Status note for the current `llm agent` / chat launch pipeline. Older LLM docs may describe previous wiring and should not be treated as source of truth for this area.

## Current Shape

The core agent loop is still `media::llm::agent::run_turn(Turn, ProviderSettings, EventCallback)`.

There is now a small shared launch resolver in `src/llm/agent_launch.*`:

- `AgentLaunchSpec` captures caller intent and overrides.
- `resolve_agent_launch(...)` loads chat settings, resolves an optional preset, overlays explicit provider/model/API flags, applies timeout and iteration defaults, sets API/streaming mode, and fills provider credentials.
- `apply_launch_policy_to_turn(...)` stamps preset-aware planner, tool, MCP, and skill policy onto a `Turn`.

This is deliberately only a resolution layer. It does not own lifecycle, UI events, cancellation, logs, sessions, memory, scheduler state, or tool output sinks yet.

## CLI Status

`tanit-cli llm agent` uses `agent_launch` for normal text mode and mic mode provider/policy assembly.

Still owned locally by the CLI:

- Prompt sources: `--prompt`, stdin, and `--embed`.
- `--cwd` and `--include` validation.
- `--system-prompt` handling.
- Session memory load/save.
- Dry-run rendering and JSON logging.
- Interactive multi-turn loop.
- Save-preset serialization.
- Ctrl+C and tool abort handling.

Recent planner hot checks passed:

- Dry markdown selection picked `file_read`.
- `--planner --no-tools` short-circuited with `tools disabled`.
- Hot command lookup picked and called `info_lookup`.
- Hot markdown read picked and called `file_read`, answering the expected `violet-harbor` / `file_read` fixture.

## UI Status

The WebView chat path now resolves provider, preset, planner, tool, MCP, and skill policy through `agent_launch` for each send. This keeps live Settings / chat preset changes effective on the next run.

It still keeps important UI-specific ownership:

- Web send payload overrides for router/model/preset/API mode.
- Chat settings persistence and planner toggle.
- UI stop/cancel bridge.
- Worker thread and event dispatch into the web panel.
- Per-run UI log path.
- Session and auto-memory handling.
- Scheduler provider callback.
- Named per-run state for concurrently running chat agents.
- Per-agent JSON log paths with agent id/display/session metadata.

The first parallel-agent UI slice keeps one shared transcript stream and labels assistant/tool/error output with the agent display name.

## Presets And Policies

Chat presets can carry:

- Chat provider settings.
- Planner enabled flag.
- Tool policy: disabled path tools, MCP master toggle, disabled MCP servers.
- Skill policy: enabled flag, roaming/workspace toggles, pinned skills, disabled skills.

CLI and WebView chat preset loading now flow through `resolve_agent_launch(...)`, and preset policy is applied through `apply_launch_policy_to_turn(...)`.

## Next Minimal Todos

1. Add visible per-agent stop/status controls in the chat UI instead of the current stop-all behavior.
2. Add rename support and a small persisted agent registry that maps display names to safe internal ids/session ids.
3. Add a small `AgentLaunchSpec` builder for CLI so the field-copy block in `pm_image_cmd_llm_agent.cpp` is isolated and testable.
4. Fix or document cwd normalization before sharing cwd validation.
5. Add a tiny smoke script for planner hot checks that writes temporary logs outside tracked fixtures and deletes them after success.
6. After CLI and UI both use the resolver, design `AgentRunRequest` / `AgentRunHandle` for lifecycle work. Do not add nested agent tools until this boundary is stable.

## Non-Goals For This Phase

- No rewrite of `run_turn`.
- No new nested agent tool.
- No full lifecycle API yet.
- No scheduler or memory ownership changes.
- No separate agent tabs/sidebar yet.

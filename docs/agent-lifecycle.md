# Agent Lifecycle Investigation

Status: lifecycle hardening notes for stuck `llm agent` runs. Critical MCP, IPC visibility, and Responses cancellation fixes have been implemented and smoke-tested.

Repro command:

```powershell
tanit-cli --log-level trace llm agent --prompt "research cnc spindle repair services spain"
```

UI-triggered repro command:

```powershell
tanit --ui-preset chat --log-level trace --prompt "research cnc spindle repair services spain" --send
```

Related references:

- `src/cli/pm_image_cmd_llm_agent.cpp`
- `src/llm/agent_factory.cpp`
- `src/llm/tools/mcp/McpBridge.cpp`
- `src/llm/mcp_stdio_client.cpp`
- `src/llm/mcp_stream_client.cpp`
- `src/core/agent_session_ipc.cpp`
- `src/cli/pm_image_cmd_run_ipc.cpp`
- `docs/command-runner.md`
- `ref/sessions/session-06-07-11-22-32/pm-image.log`

## Current Lifecycle

Normal CLI text mode builds a `Turn`, resolves provider settings, registers the run in the cross-process agent registry when `FEATURE_COMMAND_IPC_NEXT` is enabled, then blocks synchronously in `media::llm::agent::run_turn()`.

The agent registry is RAII-scoped around the `run_turn()` call:

```text
cmd_llm_agent()
  -> agent_ipc::Registration::begin(...)
  -> run_turn(turn, prov, cb)
  -> Registration::~Registration()
```

`run-ipc agents` has two modes:

- no target: directly reads `<config>/run-ipc/agents/*.json`
- target: asks a live command IPC server for its `agents` action

Plain CLI agents do not run a command IPC server. They are only visible through the direct registry path, not through `run-ipc list`, unless a UI/host process is queried.

UI chat-triggered regular agents now register the worker-thread `run_turn()` scope in the same cross-process registry. `run-ipc agents --json` should show both CLI agents (`tanit-cli.exe`) and UI agents (`tanit.exe`) while a turn is active.

Cancellation is boundary-based. `Agent::run()` checks the global cancel flag, per-turn signal, and optional predicate before each LLM round and before serial tool dispatch. Built-in path tools such as `run` add their own abort hooks. MCP tools bypass the path-tool executor, so they now have a separate timeout/cancel path.

## Log Findings

The session log shows a healthy start, MCP setup, and several Tavily tool calls:

- `11:22:59`: Tavily remote MCP server connects through `stdio:npx -y mcp-remote ...`.
- `11:23:04`: first Tavily search returns.
- `11:23:18`: the Responses API asks for four Tavily searches in one round.
- No matching Tavily tool result is logged for that four-call batch.
- From `11:29:24` onward, repeated UI stop IPC requests arrive and flip the global cancel flag.
- Every stop calls `abort_active_run()`, but there is no active `run` tool process to abort.
- Scheduler restart logs appear after each stop, including the selected `task_store_clear_all()` / `scheduler.start()` lines, but the stuck turn never emits `Done`, `Error`, or `interrupted`.

This points away from the command-runner `run` tool timeout and toward either an MCP tool call or an LLM HTTP call that does not observe cancellation until it returns.

## Suspected Failure Points

### MCP stdio had no per-call timeout

`StdioMcpClient::tools_call()` writes a JSON-RPC request, then calls `wait_response_for_id()`. The old wait used `q_cv.wait(...)` with no deadline and no cancel predicate. If `npx mcp-remote` or the remote Tavily call never emitted the matching id, the agent could block forever.

The stdio client now uses timed waits, a cancel predicate, and bounded process teardown. The default MCP timeout is 60s and can be overridden for tests with `PM_MCP_TIMEOUT_MS`.

Parallel tool dispatch made this worse because multiple stdio requests could consume each other's JSON-RPC responses from the same stdout queue. Stdio MCP calls are now serialized per session, which avoids lost responses while still allowing other tool transports to run normally.

### MCP cancellation was not wired

The shared cancel watcher calls path-tool listeners (`speak`, `run`). MCP calls now also observe the shared cancel predicate and stop their stdio process tree after a short graceful window.

### Responses calls needed active abort handling

`Agent::run()` passes the timeout to `LLMClient`. The kbot/liboai path now also exposes the active per-call cancel predicate to the curl progress hook, so in-flight `/responses` requests can abort on Ctrl+C or UI stop instead of waiting for the HTTP timeout.

The observed log does not prove the stuck point is the Responses call, because the last visible event is a four-tool Tavily batch. It remains a secondary suspect until the harness captures active phase telemetry.

### Agent registry visibility is easy to misread

`run-ipc agents` with no target should see a stuck CLI agent if:

- `FEATURE_COMMAND_IPC_NEXT` is enabled in that binary,
- `agent_ipc::Registration::begin()` succeeds,
- the caller uses the same config directory as the agent,
- the owner pid is still alive according to `OpenProcess`,
- the descriptor was not removed by premature RAII scope exit.

The CLI agent will not appear in Windows Task Manager as `tanit-cli.exe` if the launched binary is the GUI-subsystem `tanit.exe`, or if Task Manager groups/shows the underlying process image differently than the command line. `run-ipc agents --json` now reports `pid`, `cmdline`, `descriptorPath`, `processImageName`, and `processImagePath`.

### Restart hangs suggest surviving child/process resources

The stdio MCP server process is bound to a lifetime job on Windows, which protects against host death, but normal cancellation does not currently stop an in-flight MCP call. If the owning process remains blocked in the wait, the child `npx` / node process and pipes remain alive too. That can explain why restarting the UI/CLI app hangs until `taskkill /f /im tanit-cli.exe` or `taskkill /f /im tanit.exe`.

## Todos

### Logging And Diagnostics

- [ ] Add phase telemetry for each agent turn: `turn_id`, `run_id`, pid, provider mode, round number, `prev_response_id`, and phase transitions (`llm_start`, `llm_done`, `tool_batch_start`, `tool_start`, `tool_done`, `tool_timeout`, `cancel_seen`, `turn_done`).
- [x] Log MCP transport metadata per server and call basics: server key, transport (`stdio` / HTTP), JSON-RPC id, timeout, and result/error state.
- [ ] Add MCP child pid and last-call state to live diagnostics.
- [ ] For parallel tool dispatch, log all launched tool ids and emit a heartbeat while waiting for futures so the stuck future is identifiable.
- [ ] Include the current active phase in `run-ipc agents --json`, not only static registration metadata.
- [ ] Add an `agent diagnose` or `run-ipc agents --verbose` path that reports descriptor path, pid liveness, process image/cmdline, CPU/RSS, active MCP child pids, and last lifecycle event.
- [x] Add a startup log line for the effective config dir so `run-ipc agents` registry mismatches are obvious.
- [ ] On shutdown/cancel, log whether each cleanup action actually affected a live subsystem (`active_run=no`, `active_mcp=...`, `active_llm=...`) instead of only logging that the abort function was called.

### MCP Timeouts And Cancellation

- [x] Add explicit MCP timeouts separate from LLM round timeouts for initialize, `tools/list`, and `tools/call` through transport request options.
- [x] Make `StdioMcpClient::wait_response_for_id()` use `wait_for()` with a deadline and return a timeout error.
- [x] Thread a cancel predicate into MCP `tools_call()` and `tools_list_all()` so Ctrl+C/UI stop can interrupt waits.
- [x] Close stdio handles / terminate the MCP process tree on cancel or timeout.
- [x] For Windows stdio MCP teardown, do not wait forever for graceful exit; escalate to `terminate_process_tree()` after a short grace period.
- [x] Serialize stdio MCP JSON-RPC calls per session so parallel tool batches cannot steal each other's responses.
- [ ] Make parallel tool dispatch collect futures with deadlines or cooperative cancellation at the agent layer so any future hung outside MCP is bounded too.
- [ ] Add per-tool timeout overrides for known long-running tools. Keep `image_transform` and `image_create` generous, but keep MCP search/extract bounded unless explicitly overridden.

### LLM HTTP Cancellation

- [x] Verify `polymech::kbot::LLMClient` applies `llm_timeout_ms` to `/responses` in both stateful and stateless modes.
- [x] Add cancellation support to non-streaming `execute_responses()` through the liboai curl progress hook and per-call cancel predicate.
- [x] Log the configured LLM timeout at every Responses call.
- [x] Add a fake transport harness case that simulates a stuck `/responses` request and proves interruption unwinds promptly.
- [ ] Extend the same explicit cancelled classification to `execute_chat_messages()` if chat/completions hangs become a repro path.

### IPC And Process Tracking

- [x] Verify the distributed registry path used by CLI and UI is the same under default settings and under `--config-dir`.
- [x] Add `run-ipc agents --json` coverage for a real long-running `llm agent` process, not only the `run-ipc host --register-agent` fixture.
- [x] Include process image name and executable path in `AgentDescriptor` samples on Windows, so Task Manager mismatches can be diagnosed from CLI output.
- [x] Add stale descriptor diagnostics instead of silently skipping malformed/partial descriptors.
- [x] Register UI chat-triggered regular agents so `run-ipc agents --json` sees active `tanit.exe --ui-preset chat --prompt ... --send` runs.
- [ ] Consider an explicit CLI-agent command IPC server or a lightweight cancel endpoint for regular CLI agents, if direct descriptor discovery is not enough for UI stop/cancel flows.
- [x] Ensure UI stop addresses the active regular agent turn and MCP/LLM child work through per-run cancel, global cancel, MCP cancel predicates, and liboai curl abort.

### Test Harness

- [x] Add a slim harness beside `tests/orchestrator/test-agent.mjs` for lifecycle failures, with shorter runtime and fewer dependencies.
- [ ] Harness case: spawn `tanit-cli --log-level trace llm agent --prompt ...`, wait until `run-ipc agents --json` sees the descriptor, then send Ctrl+C and assert exit code `130` within a short deadline.
- [x] Harness case: fake stdio MCP server accepts `tools/call` and never replies; assert MCP timeout produces a tool error and the agent exits cleanly.
- [x] Harness case: fake stdio MCP server hangs, then interruption is sent; assert the run unwinds promptly.
- [x] Harness case: run `run-ipc agents --json` during a live CLI agent and assert `pid`, `cmdline`, `cwd`, `model`, RSS/CPU, descriptor path, and process image fields are present.
- [ ] Harness case: fake MCP emits four parallel tool calls where one hangs; assert the batch times out/cancels without blocking forever.
- [ ] Harness case: run a UI `--ui-preset chat --prompt ... --send` agent and assert `run-ipc agents --json` sees `tanit.exe` while the turn is active.
- [ ] Harness case: simulate app restart after orphan cleanup and assert the next UI/CLI launch does not hang.

### Documentation

- [ ] Update `docs/command-runner.md` to make clear that MCP tools do not use `ProcessRunner::Captured` and therefore need their own timeout/cancel path.
- [ ] Document the difference between `run-ipc list` and `run-ipc agents`: app/host command IPC instances versus decentralized live agent descriptors.
- [ ] Document the process-name expectations on Windows (`tanit.exe` GUI host vs `tanit-cli.exe` CLI binary) and recommend `run-ipc agents --json` / command line inspection over Task Manager display names.

## Verification Notes

- `npm run build:cpp` passed after the lifecycle changes.
- `node tests/orchestrator/test-agent-lifecycle.mjs` passed:
  - fake MCP stdio timeout recovery,
  - live CLI agent registry visibility and cleanup,
  - fake stuck `/responses` interruption.
- Real CLI repro completed successfully:
  - `tanit-cli --log-level trace llm agent --prompt "research cnc spindle repair services spain"`
  - exit code `0`, runtime about 83s,
  - Tavily stdio MCP connected and completed the formerly risky multi-search batch,
  - `run-ipc --json agents` showed `tanit-cli.exe` while active and `[]` after cleanup.
- UI-triggered agent visibility was verified with:
  - `tanit --ui-preset chat --log-level trace --prompt "research cnc spindle repair services spain" --send`
  - `run-ipc --json agents` showed the active UI agent with `processImageName=tanit.exe`, prompt, model, descriptor path, and session id.

## Next

1. Add active phase / last lifecycle event into `AgentDescriptor`, updated during LLM and tool phases, so `run-ipc agents --json` can say exactly what a live agent is waiting on.
2. Extend the lifecycle harness with a UI `--ui-preset chat --prompt ... --send` case and a four-tool MCP batch case where one call hangs.
3. Add `run-ipc agents --verbose` or `agent diagnose` for child MCP pids, last event, descriptor path, process image, and stale descriptor reasons.
4. Document `run-ipc list` vs `run-ipc agents` in `docs/command-runner.md`, including Windows `tanit.exe` vs `tanit-cli.exe` expectations.

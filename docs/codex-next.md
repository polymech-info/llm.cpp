# Codex runner — next work

Status: implementation in progress against Codex CLI 0.155.0 (Windows).

## Verified baseline

- `codex exec --json` works with ChatGPT authentication.
- JSONL emits `thread.started`, `turn.started`, `item.*`, and `turn.completed`.
- A saved `thread_id` can be resumed by a later process without keeping Codex alive.
- Tanit's Streamable HTTP endpoint (`http://127.0.0.1:<port>/mcp`) is accepted through
  per-invocation `mcp_servers.tanit.url` configuration.
- Codex can discover and call Tanit's tools. MCP calls need their own approval mode even
  when the process approval policy is `never`.

## P0 — correctness

- [x] Put Codex global flags before `exec`.
  - `-s`, `-a`, `-C`, `-m`, `-p`, and repeated `-c` are global.
  - `--json`, `--ephemeral`, `--skip-git-repo-check`, and images are exec/resume flags.
- [x] Fix resume construction to:
  `codex <globals> exec resume <exec-options> <thread-id> <prompt>`.
- [x] Pass `AgentRunnerOptions::approval`; headless Tanit defaults to `never` until an
  interactive approval bridge exists.
- [x] Reject `--ephemeral` with a requested resume id.
- [x] Require a parsed `turn.completed` event before reporting success.
- [x] Preserve structured Codex errors where `error` is an object.
- [x] Emit every parsed JSONL record as `Event::ExternalStream`, matching Claude.

## P0 — Tanit MCP handoff

- [x] Start the in-process loopback MCP on demand from `run_agent_turn`; do not
  bind it for every CLI/UI process, and do not spawn a second foreground
  `tanit mcp` process for chat turns.
- [x] Carry the endpoint through `AgentRunnerOptions` and the chat plugin JSON boundary.
- [x] Inject the endpoint per invocation instead of modifying the user's
  `%USERPROFILE%\.codex\config.toml`.
- [x] Mark Tanit MCP as required so startup/configuration failures fail visibly.
- [x] Repeat MCP overrides on resumed turns because each turn starts a fresh Codex
  process.
- [x] Auto-approve the effective preset allowlist; omitted/disabled tools are not approved.
- [x] Keep Tanit's preset/tool policy and executor checks as the server-side security
  boundary.
- [x] Advertise the endpoint to Codex over loopback only.

## P1 — tool scope and token cost

The 57-tool development catalog increased a trivial turn from roughly 16k to 99k input
tokens. Full-catalog handoff is therefore not an acceptable steady-state default.

- [x] Forward an explicit planner allowlist when one exists and always forward disabled
  path tools. A small non-planner baseline remains to be designed.
- [ ] Define a small external-runner baseline (catalog, file context, image/media tools
  selected by the preset).
- [ ] Investigate Codex tool search or another lazy schema mechanism before enabling the
  entire Tanit catalog.
- [ ] Add per-tool approval mapping:
  - read-only discovery: `approve`
  - writes/actions: `prompt` when an approval bridge exists
  - destructive tools: deny/omit unless explicitly enabled
- [ ] Record advertised tool count and estimated schema bytes in the agent log.

## P1 — configuration

- [x] Add optional Codex profile support (`-p/--profile`).
- [x] Add repeatable, trusted `-c/--config` overrides.
- [x] Add `--add-dir`, `--ignore-user-config`, and `--ignore-rules`.
- [x] Add a default-on, per-preset `add_tanit_mcp_server` switch for external runners.
  Codex and Claude receive the endpoint per invocation. Cursor persists the preference
  but cannot consume it until its CLI supports a one-run MCP config.
- [ ] Keep secrets out of argv. Use Codex bearer-token environment indirection for
  authenticated HTTP MCP endpoints.
- [ ] Do not create a second `codex_defaults.json`; Codex TOML/profile layering remains
  the source of truth.

## P1 — events and artifacts

- [ ] Handle `web_search` and `plan_update` items.
- [x] Map grouped `file_change` records to concrete Tanit artifact payloads.
- [x] Include MCP server name in tool event payloads.
- [ ] Surface MCP tool errors as failed tool results without turning a completed model
  response into a process success when the turn itself failed.
- [ ] Bound retained transcript data while continuing to stream events live.

## P1 — Windows runtime

- [ ] Diagnose Codex elevated-sandbox `setup refresh had errors` seen with read-only shell
  execution. `codex doctor` reported sandbox provisioning complete, while unified exec
  still failed.
- [ ] Do not silently fall back to `danger-full-access`.
- [ ] Add a user-facing diagnostic that recommends `codex doctor --json` when helper
  setup fails.

## P1 — cancel, timeout, spawn errors

Runners already honor `timeout_ms`, `turn.cancel_*`, and CLI Ctrl+C, and they
forward `CreateProcessW` / stderr into `Result.error`. That is not enough for
chat or CLI users.

- [ ] Map a missing `codex` / `claude` executable to a runner-specific
  "not installed" message (install hint + `--executable`), not
  `CreateProcessW failed: code 2: The system cannot find the file specified.`
- [ ] Same mapping for a bad `--executable` override (path does not exist).
- [ ] Keep `FEATURE_AGENT_*` OFF as a build-flag error, distinct from "not installed".
- [ ] Show the same `error` string on chat transcript, CLI stderr, and `--json`.
- [ ] Cancellation (UI Stop, CLI Ctrl+C) must kill the child, set `cancelled`,
  return CLI 130 / chat stop reason `cancel`, and not look like a crash.
- [ ] Timeout must kill the child and keep the `timed out after Nms` text
  (include the configured budget).
- [ ] Pass through child auth/login failures (not logged in) without wrapping
  them as a spawn error.

## Verification after build

Harness:

```powershell
node tests/orchestrator/test-codex-runner.mjs
$env:PM_CODEX_LIVE = "1"
node tests/orchestrator/test-codex-runner.mjs
```

- [x] Initial text-only turn.
- [x] Two-turn resume with a remembered nonce.
- [ ] Cancellation: UI Stop and CLI Ctrl+C kill the child; `cancelled` + exit 130 /
      chat reason `cancel`; not a crash / generic process-failed toast.
- [ ] Timeout: `--timeout-ms` (and the chat preset budget) surfaces
      `timed out after Nms` in CLI and chat.
- [ ] Missing CLI: `claude` / `codex` absent from PATH (and not bundled) reports
      "not installed" with an install / `--executable` hint — not raw
      `CreateProcessW`.
- [ ] Bad `--executable` path reports the same class of error.
- [ ] `FEATURE_AGENT_*` OFF stays a build-flag error, not "not installed".
- [ ] Child auth/login failure (not logged in) is readable on CLI and chat.
- [ ] Read-only command event.
- [x] Tanit MCP discovery (`info_lookup`) with no shell use.
- [ ] One safe Tanit media query.
- [ ] Missing MCP endpoint must fail when `required=true`.
- [ ] Missing/deleted Codex thread returns a clear recoverable-session error.
- [ ] Confirm no user Codex config files are modified.
- [ ] Confirm the same Tanit endpoint and policy are applied on resume.

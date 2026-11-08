# Agent-aware terminal sessions

Status: investigation and implementation backlog only.

## Outcome

Do not turn the existing `run` tool into a PTY/session tool.

`run` is a captured, one-process operation backed by `ProcessRunner`. It has
clear timeout, cancellation, sandbox, stdout/stderr, and exit-code semantics.
An interactive terminal has a different lifetime and can already contain
state, foreground programs, partial input, prompts, and output from commands
the user ran. Adding an optional `shellId` to `run` would create two unrelated
execution modes behind one schema and would make cancellation, completion,
sandboxing, and output ownership ambiguous.

Instead:

1. Move PTY ownership out of `CWebConsoleContainer` into a thread-safe native
   terminal-session broker under `src/pty`.
2. Keep `run` unchanged for isolated commands.
3. Add dedicated terminal tools backed by that broker.
4. Add explicit terminal references to `Turn`, populated from chat `@` pills.
5. Inject only terminal metadata. Never inject terminal output automatically.
6. Require a terminal to be explicitly referenced before an agent can read,
   write, or run in it.

The broker may reuse command validation and shell-specific completion helpers
from the run subsystem, but it must not call `RunTool::execute` to interact
with an existing PTY.

## Layering: keep terminal logic out of UI code

The terminal domain must not live in `CWebConsoleContainer`, `ChatWebPanel`,
React stores, or an MFC main-frame callback. Those are adapters only.

Use four layers:

```text
Agent / Chat / Console UI
        |
        v
ITerminalSessionService                 platform-neutral application API
        |
        v
TerminalSessionBroker + transcript ring platform-neutral lifecycle/state
        |
        v
ITerminalPtyBackend                     platform abstraction
        |
        +-- ConPTY backend               Windows
        +-- POSIX PTY backend            Linux/macOS
```

### Domain/service layer

Prefer a new platform-neutral module instead of growing Win32-oriented UI
files:

- `src/terminal/TerminalTypes.hpp`
- `src/terminal/ITerminalSessionService.hpp`
- `src/terminal/TerminalSessionBroker.hpp/.cpp`
- `src/terminal/TerminalTranscriptBuffer.hpp/.cpp`
- `src/terminal/TerminalCommandProtocol.hpp/.cpp`

This layer owns:

- opaque session ids and generations;
- session lifecycle and metadata;
- transcript cursors and bounded rings;
- subscribers;
- command serialization and completion tracking;
- capability checks presented by its caller;
- platform-neutral error/result DTOs.

It must not include MFC, WebView2, React concepts, `HWND`, ConPTY handles, or
agent headers. All public strings should be UTF-8. Native path conversion
belongs at the backend boundary.

### Platform backend layer

Refactor the current `PtySession` implementation behind a narrow backend
interface:

```cpp
class ITerminalPty {
public:
    virtual ~ITerminalPty() = default;
    virtual void write(std::string_view bytes) = 0;
    virtual void resize(int cols, int rows) = 0;
    virtual void close() = 0;
};

class ITerminalPtyBackend {
public:
    virtual ~ITerminalPtyBackend() = default;
    virtual TerminalCapabilities capabilities() const = 0;
    virtual Expected<std::unique_ptr<ITerminalPty>>
        spawn(const TerminalSpawnOptions&, TerminalEventSink) = 0;
};
```

Backends:

- Windows: `src/pty/win/ConPtyBackend.cpp`, extracted from `PtySession.cpp`.
- Linux/macOS: `src/pty/posix/PosixPtyBackend.cpp`.

The POSIX backend uses `posix_openpt`/`grantpt`/`unlockpt` (or `openpty` where
appropriate), `fork`, `setsid`, `TIOCSCTTY`, `exec`, `poll`, `waitpid`, and
`TIOCSWINSZ`. Close/kill must target the child process group, not an unrelated
process. macOS and Linux share the interface but may need small implementation
branches for headers, `kqueue`/`poll`, and process-exit behavior.

Do not expose backend details through tool schemas or web messages.

### Host/UI adapter layer

The native UI adapters should be thin:

- `CWebConsoleContainer`: JSON protocol ↔ `ITerminalSessionService`.
- `ChatWebPanel_Provider`: list/focus RPC ↔ service.
- `ChatWebPanel_Agent`: validates web references and copies ids into `Turn`.
- shell React app: renders sessions and forwards user input/resize.
- chat React app: renders picker pills and sends references.

No UI component should:

- own the authoritative PTY;
- parse completion markers;
- retain the authoritative output history;
- determine permissions;
- construct agent prompt text;
- contain Windows-versus-POSIX process logic.

Move the current completion parsing from `apps/shell/.../consoleRun.ts` into
`TerminalCommandProtocol`. The web app may show command state, but it must not
decide that a native operation completed.

Use shared JSON DTO definitions or generated TypeScript types for the host/web
protocol. Keep those DTOs presentation-neutral (`id`, `label`, `state`,
`shell_type`, `cwd`, `generation`, `cursor`) and version every message.

### Agent dependency injection

Do not make the LLM layer reach into `CMainFrame` or a global dock pointer.

Preferred shape:

```cpp
struct DefaultAgentOptions {
    bool tool_checkpoints_enabled = true;
    std::shared_ptr<ITerminalSessionService> terminal_service;
};
```

`feature_terminal_use(service)` receives the interface. CLI/headless agents
without a service get no terminal context or schemas. The chat host passes the
same service instance used by the Console adapter.

The current path-tool dispatcher lacks an explicit services object. The target
design is a `ToolExecutionContext` passed to executors. A transitional
thread-local `TerminalServiceScope` is acceptable if it is installed by the
feature with RAII and contains only a weak/shared interface pointer plus the
turn capability set. It must not contain UI pointers.

### Build and feature boundaries

Use independent gates:

- `FEATURE_TERMINAL_SESSIONS`: broker, protocol, agent tools.
- `FEATURE_TERMINAL_CONPTY`: Windows backend.
- `FEATURE_TERMINAL_POSIX_PTY`: Linux/macOS backend.
- `FEATURE_CONSOLE`: visual console surface.

This allows headless/CLI builds to compile the agent without MFC or React and
allows a terminal service to exist without the Console panel. Unsupported
platforms return capabilities with `available=false`; they do not require
Win32 stubs throughout agent code.

## Current state

### Console and PTY ownership

- `CWebConsoleContainer` owns
  `std::map<std::string, std::unique_ptr<pm::pty::PtySession>> m_sessions`.
- A session is created lazily from `shell_start`, `shell_data`, or
  `shell_resize`.
- Session output is posted directly to the shell WebView as `shell_output`.
- The native side keeps no output history, cursor, line index, terminal name,
  active-tab state, or reusable session snapshot.
- `CWebConsoleContainer::~CWebConsoleContainer` closes every PTY.
- `PtySession` exposes only `write`, `resize`, and `close`; it has no metadata,
  read/tail API, process state, or subscriber API.
- The actual ConPTY implementation already has the process id and asynchronous
  reader/waiter threads, but those details are private.

Consequently, an agent cannot currently enumerate or read open PTYs without
going through the shell WebView, and the WebView itself is not an appropriate
agent data store.

### Shell web state

- `apps/shell/src/shell/shellStore.ts` is the only owner of terminal names,
  ordering, active tab, type, and creation time.
- Shell ids are generated in JavaScript.
- `consoleRun.ts` can target an explicit `shellId`, but this is a UI/host
  command path, not an agent API.
- `Terminal.tsx` owns xterm's scrollback buffer. Native code sees output chunks
  but does not retain them.
- The existing `RunInWebConsole` path is command insertion plus a textual
  completion marker. It does not expose output to its native caller.

### Chat context

- Chat sends an immutable file-context snapshot as `context_paths` and
  `folder`.
- `ChatWebPanel_Agent.cpp` maps that snapshot to `Turn::selection`,
  `Turn::folder_hint`, and the live host list to `Turn::open_files`.
- `Turn` has no terminal-reference field.
- `agent_context.cpp` injects selected paths, open files, and the folder.
- `BrowserUseFeature` demonstrates the correct feature-stage pattern:
  enumerate live resources, retain the relevant schemas after planning, and
  append compact context before `SystemPromptFeature`.

### Composer references

- `@` currently combines file search and MRU file extras.
- `/` currently lists skills only, but `docs/llm/slash-commands.md` explicitly
  reserves it for more source kinds later.
- Collected values use a generic pill wire format:
  `@[kind:label](description)`.
- `agentContextPaths.ts` currently treats every collected pill as a path.
  A terminal pill must be excluded there or `terminal://...` will be resolved
  as a filesystem path.
- Transcript pill clicks currently route file and skill pills to
  `openContextPath`; terminal pills need a host action that focuses the console
  and activates the referenced tab.

### Existing `run` tool

- `run` uses `ProcessRunner` in captured mode.
- It validates one command, resolves one cwd, waits for completion, and returns
  one bounded result envelope.
- Its global output hook is temporary and scoped to the active agent tool
  call.
- It cannot attach to a PTY, preserve shell state, or distinguish output
  written before and after a command.
- `abort_active_run` aborts a captured process. It must not start killing a
  user-owned terminal session.

## Required behavior

### Passive awareness

The agent context may enumerate open native terminal sessions using metadata
only:

```text
## Open terminal sessions
- terminal://term-7 "PowerShell (1)" type=powershell state=running active=true cwd=C:\work
- terminal://term-9 "WSL (2)" type=wsl state=running active=false cwd=/home/user/project

Terminal output is not included. A terminal must be explicitly referenced by
the user before terminal_read, terminal_write, or terminal_run can target it.
```

Hard caps:

- At most 12 sessions in prompt context.
- At most 160 characters per session line.
- No command history, output tail, environment variables, or process command
  line in the prompt.
- Sort active first, then most recently used.

### Explicit references

The primary UX is an `@` terminal reference:

```text
@[terminal:PowerShell (1)](terminal://term-7)
```

Typing `@` should add a `Terminals` group before filesystem search results.
Picking a terminal inserts a normal collect pill. The visible label is friendly
but the opaque native session id is authoritative.

At send time the web app must parse terminal pills separately and post:

```json
{
  "kind": "send",
  "prompt": "Check why the last command failed @[terminal:PowerShell (1)](terminal://term-7)",
  "terminal_refs": [
    {
      "id": "term-7",
      "generation": 3,
      "uri": "terminal://term-7",
      "label": "PowerShell (1)"
    }
  ]
}
```

The host validates each id against the broker and creates a frozen
`Turn::terminal_refs` snapshot. Labels and metadata supplied by JavaScript are
display hints only.

Suggested slash actions:

- `/terminal` opens the terminal picker.
- `/new-terminal` creates a new terminal, focuses it, and inserts its reference.

These are explicit UI actions, not prompt text that the model must interpret.
They can be added after `@` references work. Do not overload `/` skill pills
with terminal session ids.

### Referenced-session capabilities

Once a session is referenced, the agent may:

- read a bounded output tail;
- write raw input for genuinely interactive work;
- run a command and wait for its completion marker;
- optionally focus the terminal for the user.

Creating or closing a terminal remains a separate, explicit operation.

## Permissions and consent

An `@terminal` reference is explicit user intent and a resource capability,
but it is not blanket permission to mutate or execute in that terminal.

Recommended policy:

- `terminal_list`: low-risk metadata read; allow without consent.
- `terminal_read`: the explicit pill creates a Task-lifetime, exact-session
  Read capability capped by bytes/lines. No output is injected automatically.
- `terminal_write`: Ask. Raw input can answer password prompts, send control
  keys, or control an unrelated foreground program.
- `terminal_run`: Ask as Execute. Permit Once, Task, or Session grants scoped
  to the exact terminal and tool identity.
- `terminal_open`: Ask when model-initiated. A direct `/new-terminal` UI action
  is a user action and does not need LLM consent.
- `terminal_close`: destructive; fresh consent only in the first version.

Use distinct resource targets:

```text
terminal://term-7/output
terminal://term-7/input
terminal://term-7/command
```

Add `terminal` URI handling to security scope matching. Suggested grants must
remain exact to the session:

```text
terminal://term-7/**
```

Do not use `cmd://**` for terminal grants: it would authorize commands outside
the referenced session. Tool identity and action are also part of the grant,
so a `terminal_read` grant cannot match `terminal_run`.

At send time the host may mint the narrow Task-lifetime Read capability because
the human directly selected the pill. It must audit this as a user-originated
attachment grant. Write and Execute still pass through `SecurityGateway`.

Initially suppress `allow_always` for terminal input, execute, and close.
Session grants are already broad enough for an interactive terminal. A remote
SSH-backed session is unsandboxed: AppContainer cannot contain an existing
user PTY or the remote machine. Consent must display the remote host alias when
known and report `sandboxed=false`.

Memory, prompt text, active-tab state, and a remembered terminal id never grant
authority. The frozen `TerminalRef` capability plus `SecurityGateway` is the
only executable authority.

## Native terminal-session broker

Add a process-lifetime broker independent of MFC and WebView2. Use one
directory convention:

- `src/terminal/` contains domain types, service, broker, transcript, and
  command protocol.
- `src/pty/win/` and `src/pty/posix/` contain backend mechanics only.

The broker should own `PtySession` instances and expose thread-safe operations:

```cpp
struct TerminalSessionInfo {
    std::string id;
    std::string label;
    std::string shell_type;
    std::string cwd;
    TerminalSessionState state;
    bool active = false;
    std::uint64_t generation = 0;
    std::uint64_t output_cursor = 0;
};

struct TerminalReadRequest {
    std::string id;
    std::uint64_t generation = 0;
    std::optional<std::uint64_t> after_cursor;
    std::size_t tail_lines = 120;
    std::size_t max_bytes = 24 * 1024;
    bool strip_ansi = true;
};

struct TerminalReadResult {
    TerminalSessionInfo session;
    std::string text;
    std::uint64_t start_cursor = 0;
    std::uint64_t next_cursor = 0;
    bool truncated = false;
    bool gap = false;
};
```

Required broker operations:

- `list()`
- `get(id, generation)`
- `create(options)`
- `write(id, generation, bytes)`
- `resize(id, generation, cols, rows)`
- `read(request)`
- `start(id, generation, command, options)`
- `status(operation_id)`
- `run(id, generation, command, timeout, cancel_check)` as later convenience
  sugar over `start` + bounded wait;
- `close(id, generation)`
- `subscribe(listener)` / `unsubscribe(token)`
- `set_label(id, generation, label)`
- `set_active(id, generation)`

Broker constraints:

- Generate or validate opaque session ids natively.
- Require `{id, generation}` on every session-targeting operation. Validate it
  at the broker boundary even when the host/agent already validated the
  `TerminalRef`.
- Keep process ids internal to the backend/broker and diagnostic logs. Do not
  expose them in ordinary agent inventory or tool results.
- Retain output in native memory, not xterm or the chat transcript.
- Use a byte/line ring per session. Initial limits: 256 KiB and 2,000 lines,
  whichever is reached first.
- Assign monotonic byte cursors before fan-out to WebView subscribers.
- Preserve raw PTY bytes in the ring; strip ANSI and normalize line endings in
  `read`, not on ingest.
- Cut results on UTF-8 boundaries.
- Report a cursor gap when requested output has already been evicted.
- Never block the PTY reader thread on WebView posting, an LLM call, or a
  terminal tool waiting for completion.
- Serialize `run` operations per session. Raw user/agent writes during a
  tracked run must either be rejected or explicitly joined to that run.
- A broker close must wake pending reads/runs with a deterministic
  `session_closed` result.
- Broker-owned PTYs survive Console dock closure, WebView destruction/reload,
  and chat surface recreation. Only explicit session close or application
  shutdown terminates them.

`PtySession` needs either getters/events for pid and state or richer start
results. Avoid exposing ConPTY handles.

## Console migration

`CWebConsoleContainer` becomes a broker client:

- Remove direct `m_sessions` ownership.
- Forward `shell_start`, `shell_data`, `shell_resize`, and `shell_close` to the
  broker.
- Subscribe to broker output/state events and continue posting the current
  `shell_ready`, `shell_output`, and `shell_exit` protocol to the web app.
- Add host messages for label and active-tab synchronization:
  `shell_register`, `shell_rename`, `shell_activate`, and `shell_unregister`.
- On shell web `ready`, send a broker snapshot so a WebView reload can
  reconstruct existing tabs instead of creating duplicate PTYs.
- Keep `CMainFrame::RunInWebConsole` working during migration, but route it
  through broker `run` once marker handling is centralized.

The source of truth should be split deliberately:

- Native broker: id, generation, type, cwd, internal pid, state, output,
  operation state, and lifetime.
- Shell web store: visual ordering and settings.
- Label and active state: mirrored to the broker for enumeration.

## Agent turn and feature

Add to `agent.hpp`:

```cpp
struct TerminalRef {
    std::string id;
    std::uint64_t generation = 0;
};

std::vector<TerminalRef> terminal_refs;
```

Add `agent_feature_TerminalUse.hpp/.cpp`, following
`agent_feature_BrowserUse.cpp`:

- Phase: `Plan`, after `feature_planner()` and before `feature_tools()`.
- Query broker metadata only.
- Validate and deduplicate `turn.terminal_refs`.
- Append the compact inventory block to `turn.system_extra`.
- If referenced sessions exist, retain terminal tool schemas after planner
  selection.
- Disable parallel dispatch for multiple operations against the same session;
  different sessions may be parallelized only after broker locking is proven.
- Do not query output during context preparation.
- Do not make unavailable or unreferenced sessions executable merely because
  they were listed.

Register `feature_terminal_use()` in `agent_factory.hpp`,
`agent_factory.cpp`, and the default agent assembly beside BrowserUse.

The planner input should receive referenced terminal ids and labels, not
terminal output. Update planner prompt guidance so terminal intents select
terminal tools rather than `run`.

## Tool surface

Prefer dedicated schemas over a polymorphic extension to `run`:

### `terminal_read`

Inputs:

- `session_id` required;
- `generation` required;
- `after_cursor` optional;
- `tail_lines` default 120, maximum 500;
- `max_bytes` default 24 KiB, maximum 64 KiB;
- `strip_ansi` default true.

It returns metadata, text, cursor values, and truncation/gap flags.

### `terminal_write`

Inputs:

- `session_id` required;
- `generation` required;
- exactly one of `data` or a small key enum;
- `append_enter` default false.

This is raw PTY input. It is not a command runner and must be described as an
interactive escape hatch.

### `terminal_run`

Inputs:

- `session_id` required;
- `generation` required;
- `command` required;
- `timeout_ms` bounded like `run`;
- optional `tail_lines`/`max_bytes` for returned output.

It must:

1. apply the same command validation policy as `run`;
2. snapshot the output cursor;
3. enqueue one shell-specific wrapped command with a random native token;
4. wait for an exact completion marker or close/cancel/timeout;
5. return only output after the snapshot, with the marker removed;
6. leave the terminal alive on completion;
7. never infer completion from the prompt text.

Factor shell wrapping/marker parsing into shared helpers. The current
PowerShell-only wrapping in `RunInWebConsole` and duplicate JavaScript marker
parser should not become a third implementation.

### `terminal_open`

Inputs:

- shell type;
- optional cwd;
- optional label;
- optional focus flag.

Gate this on explicit user intent (`/new-terminal` or an unambiguous request).
Return the new id and generation. It should not be silently selected merely
because no existing session was referenced.

### `terminal_close`

Defer until read/write/run/open are stable. Closing a user-owned persistent
terminal is destructive and requires separate consent/lifetime policy.

Tool enforcement must happen in the executor, not only in prompt text:

- `terminal_read`, `terminal_write`, and `terminal_run` reject ids absent from
  the frozen `Turn::terminal_refs` allowlist.
- The executor passes both id and generation to the broker. A matching id with
  a stale generation is still rejected.
- Store the allowlist in an RAII/thread-local scope similar to the path base,
  or pass a per-run capability object to the tool executor.
- Never accept a label, active index, or JavaScript-provided URI as authority.
- A reference authorizes one agent turn by default, not the whole chat session.

Add schemas in a dedicated `src/llm/tools/terminal` category and compose them
from `path_tool_catalog.cpp`. Do not leave more inline terminal schemas beside
`make_run`.

## Long-running work and SSH

### Local long-running commands

A synchronous `terminal_run` may time out while leaving the PTY and command
alive. That is useful, but timeout must return an operation id and output
cursor rather than looking like a failed/aborted process.

Add an asynchronous operation shape:

```text
terminal_start(session_id, command) -> operation_id, start_cursor
terminal_status(operation_id)       -> running|exited|unknown
terminal_read(session_id, after_cursor)
```

`terminal_run` can remain a convenience wrapper around start + bounded wait.
The broker owns operation state; an agent turn or WebView ending must not erase
it. Monitoring across later turns requires the terminal to be referenced
again.

### Existing interactive SSH sessions

An existing terminal may currently be inside `ssh`, Python, a pager, `sudo`, a
database console, or another REPL. The broker knows the shell originally
spawned locally; it does not know the current foreground protocol or the
remote shell.

Therefore:

- `terminal_read` is reliable for bounded observation.
- `terminal_write` can send explicit raw input after consent.
- `terminal_run` must not assume the original local shell wrapper still works.
- If foreground mode is unknown, return `interactive_state_unknown` and require
  explicit raw input or a dedicated remote-exec path.

Do not infer “currently in SSH” from terminal text. Optional shell integration
can later report foreground process/protocol, but it remains advisory.

### Durable remote tasks

For work that must survive a local app exit or SSH disconnect, start it inside
remote durability primitives such as:

- `tmux` or `screen`;
- `systemd-run --user`;
- a scheduler/queue native to the remote system;
- `nohup` with explicit log and pid/status files.

Persist only a safe remote locator: configured host alias, tmux/session/job id,
log cursor/path, timestamps, and status. Never persist passwords, private keys,
tokens, or raw secret-bearing commands.

A local PTY and its SSH connection die when the application process exits.
Remote work survives only when detached remotely. The current in-process
agent scheduler also does not survive application restart, so it cannot be the
durability mechanism for a remote job.

Longer term, a dedicated SSH executor can provide stronger semantics than an
opaque PTY: host identity, remote shell selection, exit status, reconnect, and
operation ids. It should still use the terminal reference/consent model when
the user asks it to operate through an existing visible terminal.

## Chat web changes

### Host

- Add a terminal-list RPC or host-pushed terminal inventory. Prefer a small
  `chatTerminalSessionsList` provider RPC initially because the composer
  already uses that pattern for skills.
- Wire the RPC to the native broker, not to the shell WebView store.
- Extend `HandleAgentSend` to parse, validate, and freeze `terminal_refs`.
- Include validated refs in `Turn`.
- Add host actions to focus the console and activate a terminal pill.
- Subscribe the chat surface to broker inventory changes if live picker updates
  are required while the composer is open.

### Composer

- Add terminal `SearchExtraItem`s with `kind: "terminal"` and
  `payload: { id, generation, uri, label, shellType, cwd, state }`.
- Add a `Terminals` scope group for `@`.
- Keep terminal items client-filtered; do not pass terminal names to filesystem
  `searchStart`.
- Add terminal icon/state text and indicate the active terminal.
- Parse terminal pills into `terminal_refs` in `pmChatStore.send`.
- Exclude `kind: "terminal"` from `collectPathsFromComposerPlainText`.
- Preserve terminal pills in the user transcript.
- On terminal-pill click, post a host focus/switch action rather than calling
  `openContextPath`.
- Handle stale/closed pills visibly and let the host reject them safely.

### Shell web

- Post tab metadata changes to the host.
- Accept native session snapshots on reload.
- Switch/focus a shell by native id when requested from a chat pill.
- Avoid creating a second session when restoring an existing broker session.

## Agent memory and resumability

`agent_memory.cpp` is the durable chat/task store. It should record compact
terminal operation references, not own PTYs, output buffers, or permissions.

Its useful roles are:

- retain operation/session locators across chat turns;
- journal successful tool outcomes during a turn;
- expose compact checkpoints to planner/session context;
- remember a remote durable-job locator after the local SSH interaction ends;
- record an error reflexion when a terminal operation fails or completion is
  unknown.

Add a structured item such as:

```json
{
  "kind": "terminal_operation",
  "role": "execution",
  "locator": {
    "broker_instance": "broker-...",
    "session_id": "term-7",
    "generation": 3,
    "operation_id": "op-...",
    "remote_host_alias": "buildbox",
    "remote_job_id": "tmux:agent-build"
  },
  "status": "running",
  "summary": "Remote build started",
  "metadata": {
    "command_hash": "...",
    "command_preview": "npm run build",
    "start_cursor": 19220,
    "next_cursor": 19402,
    "started_at": "...",
    "updated_at": "..."
  }
}
```

Rules:

- Store summaries, hashes, cursors, and locators; do not store full terminal
  output by default.
- Never store credentials or raw secret-bearing input.
- Broker session ids are process-local. On reload, compare broker instance and
  generation; stale items remain historical context, not live handles.
- A remote durable locator may survive local broker loss, but reconnect still
  requires normal SSH authentication and consent.
- Memory can suggest what to inspect next; it cannot read a terminal itself.
- Memory can never restore or imply a capability grant. The user must reference
  the live terminal again.

`ToolCheckpointFeature` currently recognizes only selected `run` commands
(notably npm test/install/build). Extend checkpoint extraction for
`terminal_start`, `terminal_run`, `terminal_status`, and terminal reads that
change an operation's known state. Keep exact payload recovery in the agent
log/journal rather than injecting large output into session items.

## Security and trust boundaries

- Terminal output can contain secrets. Inventory is safe to inject; output is
  tool-fetched only.
- Apply strict output caps both in the broker and the tool envelope.
- Treat terminal output as untrusted data. A command can print prompt-injection
  text just as a web page can.
- Reuse `RunValidators` for `terminal_run`.
- `terminal_write` needs separate security classification because arbitrary
  bytes can answer password prompts, send control keys, or interact with an
  unrelated foreground program.
- Do not expose environment blocks, process handles, hidden command lines, or
  full scrollback in list/status calls.
- Do not let an agent write to the active terminal merely because it is active.
  Only the referenced id is authorized.
- Keep user keystrokes and agent writes ordered. Include origin metadata in
  broker audit events.
- Log metadata and byte counts, not full terminal output.
- `terminal_open` cwd must pass the same filesystem guard used by `run`.
- Existing AppContainer behavior cannot be inherited by attaching to a normal
  user PTY. The tool result must report `sandboxed: false`; policy must not
  pretend otherwise.

## Completion protocol risks

Interactive PTY completion is the most failure-prone part.

- Prompt detection is not reliable.
- `$LASTEXITCODE` and `$?` differ in PowerShell.
- Shell echo can include the marker before command completion.
- Applications can emit matching text.
- A shell can be inside Python, SSH, a pager, REPL, or another foreground
  process when `terminal_run` is requested.
- User input can interleave with the wrapped command.
- Shell type can change after entering another shell.

Minimum safeguards:

- random 128-bit marker token;
- exact framed marker with start/end delimiters;
- per-session command mutex;
- cursor snapshot before write;
- shell-type-specific wrapper;
- reject `terminal_run` when the broker knows a tracked command is already
  active;
- timeout does not kill the whole terminal by default;
- return `completion_unknown` when the marker is absent;
- retain `terminal_write` for explicit interactive recovery rather than
  pretending `terminal_run` succeeded.

Longer term, use a small shell integration protocol or launch an agent-owned
PTY session with a cooperating shell shim. Do not infer robust command state
from xterm text.

## Test plan

Most coverage must be headless. UI automation is the final wiring check, not
the primary correctness test.

### 1. Pure unit tests

Run without a PTY, WebView, MFC, shell executable, or LLM.

Transcript buffer:

- append chunks split across UTF-8 code points;
- line counting across chunk boundaries;
- 256 KiB and 2,000-line eviction;
- exact `start_cursor`/`next_cursor`;
- `gap=true` after eviction;
- tail-line and byte caps together;
- raw versus ANSI-stripped output;
- CR, LF, and CRLF normalization;
- empty output and oversized single-line output.

Identity and lifecycle:

- opaque id generation;
- generation increments/replacement behavior;
- every stale `{id, generation}` operation fails;
- unknown id and closed session errors are deterministic;
- list ordering: active first, then recently used;
- agent inventory omits pid, output, environment, and process command line.

Command protocol:

- random marker framing;
- markers split across chunks;
- echoed marker text does not complete an operation;
- wrong token and malformed exit code are ignored;
- timeout produces `completion_unknown` without closing the PTY;
- marker removal does not remove neighboring user output;
- PowerShell, cmd, and POSIX wrapper serialization;
- secret-bearing command text is excluded from normal logs.

Security and memory:

- terminal URI parsing and exact-session scope matching;
- Read grant cannot match Write or Execute;
- terminal id match with generation mismatch is denied;
- terminal pill creates only the bounded Task-lifetime Read capability;
- write/run/close invoke consent;
- terminal-operation memory item contains locator/cursors but no output or
  capability grant;
- stale broker instance remains historical memory only.

### 2. Broker tests with a fake PTY backend

Use a deterministic in-memory `ITerminalPtyBackend`.

- create, list, get, write, resize, read, and explicit close;
- session survives all UI subscriber removal;
- application shutdown closes every backend instance;
- multiple subscribers receive ordered cursors;
- a slow subscriber cannot block PTY ingestion;
- close wakes pending read/start/status waits;
- simultaneous writes preserve broker ordering;
- per-session operation serialization;
- different sessions remain independent;
- async `start → operation_id → status → read`;
- cancellation/timeout leaves the user session alive;
- operation state survives the creating agent turn;
- ring eviction while an operation is running;
- broker rejects stale generation even if a caller bypasses host validation.

The fake backend should provide explicit controls to emit output, exit, block,
and fail. Avoid sleeps where a latch/condition variable can make the test
deterministic.

### 3. Real backend integration tests

Use a small deterministic PTY helper executable/process instead of depending
on human prompt text.

Shared contract suite:

- spawn and receive a ready token;
- UTF-8 input/output round trip;
- resize notification/observable dimensions;
- partial output followed by exit;
- exit-code propagation;
- explicit close terminates the child process group;
- repeated close is safe;
- high-volume output does not deadlock;
- broker remains usable after one backend spawn failure.

Platform matrix:

- Windows: ConPTY backend on supported Windows CI.
- Linux: POSIX PTY backend.
- macOS: POSIX PTY backend with macOS-specific process/PTY behavior.

Skip only the unavailable backend suite; the fake-broker and pure tests remain
mandatory on every platform.

### 4. Headless agent/tool tests

Inject a fake `ITerminalSessionService` into `default_agent` or a custom feature
assembly. No provider API is required for executor-level tests.

- no service means no terminal context or terminal schemas;
- inventory injects capped metadata only;
- referenced sessions retain terminal schemas after planner selection;
- unreferenced and guessed ids fail;
- stale generation fails;
- `terminal_read` caps its envelope;
- `terminal_write` forwards exact bytes only after consent;
- planner/user text cannot widen the frozen capability set;
- output containing prompt-injection text remains tool data;
- parallel operations against one session are serialized/disabled;
- terminal checkpoint and error-reflexion persistence;
- existing captured `run` behavior, cancellation, and sandbox envelope remain
  unchanged.

Use deterministic fake planner/tool calls where routing behavior itself is
tested. Do not call a paid or network LLM from the core test suite.

### 5. Web/component tests

Chat:

- `@` shows a separate terminal group without starting filesystem search;
- active and inactive terminal labels/states render correctly;
- selecting a terminal produces a terminal pill;
- send payload contains `{id, generation}` in `terminal_refs`;
- terminal pills are excluded from `context_paths`;
- stale/closed pills render as unavailable;
- transcript click posts focus/switch instead of `openContextPath`.

Shell:

- store hydrates from a native broker snapshot;
- WebView reload does not emit a duplicate `shell_start`;
- rename/activate messages carry id and generation;
- output events route to the correct terminal;
- completion status is accepted from native code, not parsed authoritatively
  in JavaScript.

### 6. Final Win32/WebView2/UIA tests

Keep `tests/orchestrator/test-chat-ui.mjs` as the fast bootstrap/composer smoke
test. Prefer a separate `tests/orchestrator/test-agent-shell-ui.mjs` for the
larger terminal flow so terminal failures do not make the basic chat smoke
test noisy.

Final UI flow:

1. Launch the chat workbench and open Console.
2. Create a deterministic terminal session.
3. Type a known line and verify it appears.
4. Focus chat and type `@`.
5. Verify the open session appears in the terminal group.
6. Select it and verify the pill is inserted.
7. Send or snapshot the composer and verify `{id, generation}` reached the
   host.
8. Verify the user transcript preserves the pill.
9. Click the pill and verify Console plus the correct tab receive focus.
10. Destroy/reload the shell WebView and verify the same broker session is
    restored without a duplicate process.

Do not require a real LLM/API for this UI suite. Exercise actual terminal tools
through headless tests or a deterministic test-only host/IPC seam. As with the
existing UI smoke, skip when no interactive Windows desktop is available
unless an explicit `PM_AGENT_SHELL_UI_REQUIRE=1` gate is set.

### 7. SSH and long-running scenarios

Run these as opt-in integration tests against a disposable local SSH container
or VM, never a developer-configured host.

- raw read/write through an SSH foreground process;
- `terminal_run` refuses unknown interactive state;
- remote command detached into tmux/systemd-run/nohup;
- local SSH disconnect while remote job continues;
- reconnect using a non-secret host alias and inspect status/log cursor;
- local broker/app restart marks the old terminal locator stale;
- remote durable-job locator remains useful but does not restore permission;
- no private key, token, password, or full remote output enters session memory.

### Test commands and CI split

Add focused scripts rather than one all-or-nothing suite:

```text
test:terminal-unit
test:terminal-broker
test:terminal-backend
test:terminal-agent
test:terminal-web
test:agent-shell-ui
test:terminal-ssh
```

Expected CI tiers:

- every PR/all OSes: unit, fake broker, headless agent;
- web-capable jobs: component tests;
- Windows desktop job: ConPTY integration and UIA;
- Linux/macOS jobs: POSIX PTY contract suite;
- opt-in/nightly: disposable SSH and long-running scenarios.

## Implementation backlog

### Phase 0 — contracts and tests

- [ ] Define `TerminalSessionInfo`, ids, generations, states, cursors, caps,
  and error codes.
- [ ] Lock lifetime policy: broker sessions survive UI destruction; explicit
  close or application shutdown terminates them.
- [ ] Decide whether existing JavaScript ids remain accepted as requested ids
  or native ids replace them immediately.
- [ ] Define raw versus ANSI-stripped read behavior.
- [ ] Define per-turn terminal capability enforcement.
- [ ] Freeze the four-layer API boundary: UI adapter → service → broker →
  platform backend.
- [ ] Define `TerminalCapabilities` so UI and agents do not hardcode OS checks.
- [ ] Define terminal URI resource scopes and tool-specific consent choices.
- [ ] Add the pure unit and fake-broker suites before migrating UI ownership.

### Phase 1 — broker extraction

- [ ] Add platform-neutral `ITerminalSessionService` and
  `TerminalSessionBroker`.
- [ ] Add `ITerminalPtyBackend`.
- [ ] Extract the current ConPTY implementation into the Windows backend.
- [ ] Add a POSIX PTY backend for Linux/macOS.
- [ ] Move `PtySession` ownership out of `CWebConsoleContainer`.
- [ ] Retain pid internally for lifecycle/diagnostics without exposing it to
  normal agent metadata.
- [ ] Add bounded native transcript rings.
- [ ] Add thread-safe event subscriptions.
- [ ] Migrate `shell_start/data/resize/close`.
- [ ] Preserve existing shell WebView protocol and behavior.
- [ ] Restore broker sessions after shell WebView reload.
- [ ] Remove native completion-marker parsing from shell React code.

### Phase 2 — terminal metadata in chat

- [ ] Add `chatTerminalSessionsList`.
- [ ] Add terminal extra items and the `Terminals` `@` group.
- [ ] Add terminal collect-pill rendering.
- [ ] Exclude terminal pills from path snapshots.
- [ ] Add `terminal_refs` to the web send payload.
- [ ] Validate refs in `HandleAgentSend`.
- [ ] Add `TerminalRef` to `Turn`.
- [ ] Add transcript click-to-focus/switch.
- [ ] Pass `ITerminalSessionService` through agent options; no UI/global dock
  access from agent code.

### Phase 3 — read-only agent support

- [ ] Add per-turn terminal capability scope.
- [ ] Add `terminal_read` schema and executor.
- [ ] Add `TerminalUseFeature` metadata injection and schema retention.
- [ ] Add planner routing guidance.
- [ ] Add read caps and cursor/gap envelopes.
- [ ] Verify an unreferenced terminal cannot be read by guessed id.

### Phase 4 — interactive write and creation

- [ ] Add security classification/consent for `terminal_write`.
- [ ] Add generation-bound `terminal_write`.
- [ ] Add generation-bound `terminal_open`.
- [ ] Add `/terminal` and `/new-terminal` UI actions.
- [ ] Add audit events with session id, generation, action, origin, and byte
  counts.
- [ ] Defer `terminal_close` until destructive consent is defined.

This phase deliberately ships list + read + write before command execution.
The broker and trust boundary are useful without reliable arbitrary-shell
completion.

### Phase 5 — asynchronous operation foundation

- [ ] Extract shared command validation helpers from `RunTool`.
- [ ] Centralize shell-specific completion wrapping/parsing.
- [ ] Add broker command serialization.
- [ ] Add generation-bound `terminal_start`.
- [ ] Add `terminal_status`.
- [ ] Wire cancellation and timeout without killing the user PTY.
- [ ] Persist operation ids/cursors as compact memory checkpoints.
- [ ] Remove duplicate marker parsing where safe after migration.

### Phase 6 — `terminal_run` convenience

- [ ] Implement `terminal_run` as start + bounded status wait + cursor read.
- [ ] Return only post-cursor output.
- [ ] Refuse unknown SSH/REPL/pager foreground state.
- [ ] Return `completion_unknown` honestly when no valid marker arrives.

### Phase 7 — integration coverage

- [ ] Multiple PowerShell sessions with one active and one referenced inactive.
- [ ] CMD, bash, WSL, and Git Bash wrappers.
- [ ] Session renamed while a pill exists.
- [ ] Session closed between send and first tool call.
- [ ] WebView reload with live PTYs.
- [ ] Ring eviction between reads.
- [ ] ANSI-heavy and non-UTF-8-ish output.
- [ ] Long-running command cancellation and timeout.
- [ ] Async start/status/read across separate chat turns.
- [ ] User keystroke collision with `terminal_run`.
- [ ] Foreground REPL/SSH/pager rejection or `completion_unknown`.
- [ ] Detached remote task survives SSH disconnect.
- [ ] Local app restart marks broker locators stale while preserving remote job
  locators.
- [ ] Agent tries a guessed/unreferenced id.
- [ ] Terminal output containing prompt-injection instructions.
- [ ] Console dock hidden, moved, destroyed, and reopened.
- [ ] Existing captured `run` behavior and sandbox envelopes remain unchanged.
- [ ] Run the headless test matrix on Windows, Linux, and macOS.
- [ ] Add final `test-agent-shell-ui.mjs` UIA coverage.

## Likely files

Native terminal:

- `src/pty/PtySession.h`
- `src/pty/PtySession.cpp`
- new `src/terminal/TerminalTypes.hpp`
- new `src/terminal/ITerminalSessionService.hpp`
- new `src/terminal/TerminalSessionBroker.*`
- new `src/terminal/TerminalTranscriptBuffer.*`
- new `src/terminal/TerminalCommandProtocol.*`
- new `src/pty/win/ConPtyBackend.*`
- new `src/pty/posix/PosixPtyBackend.*`
- `src/win/web/CWebConsole.h`
- `src/win/web/CWebConsole.cpp`
- `src/win/ui_next/Mainfrm_Web.cpp`

Agent:

- `src/llm/agent.hpp`
- `src/llm/agent_context.cpp`
- `src/llm/agent_factory.hpp`
- `src/llm/agent_factory.cpp`
- new `src/llm/agent_feature_TerminalUse.*`
- `src/llm/path_tool_catalog.cpp`
- `src/llm/path_tool_executor.cpp`
- `src/llm/agent_tools.*`
- new `src/llm/tools/terminal/*`
- `src/llm/tools/run/RunTool.*`
- `src/llm/tools/run/RunValidators.*`
- `src/llm/agent_memory.cpp`
- `src/llm/agent_memory_artifacts.*`

Chat host:

- `src/win/ui_next/ChatWebPanel_Agent.cpp`
- `src/win/ui_next/ChatWebPanel_Provider.cpp`
- `src/win/ui_next/ChatWebPanel_internal.hpp`
- `src/win/ui_next/ChatWebPanel.cpp`

Chat web:

- `apps/chat-next/src/pm-chat/ChatComposerDock.tsx`
- `apps/chat-next/src/pm-chat/pmChatStore.ts`
- `apps/chat-next/src/pm-chat/agentContextPaths.ts`
- `apps/chat-next/src/pm-chat/ChatTranscript.tsx`
- `apps/chat-next/src/pm-chat/hostBridge.ts`
- `apps/shared/search/searchCollect*.ts`
- `apps/shared/search/SearchCollectPill.tsx`

Shell web:

- `apps/shell/src/main.tsx`
- `apps/shell/src/shell/shellStore.ts`
- `apps/shell/src/shell/consoleRun.ts`
- `apps/shell/src/shell/Terminal.tsx`
- `apps/shell/src/shell/ShellPanel.tsx`

## Acceptance criteria

- Chat `@` lists live terminal sessions without reading their output.
- A selected terminal becomes an immutable, validated turn reference.
- Every broker operation validates both session id and generation.
- An agent cannot read/write/run in an unreferenced terminal, even with a
  guessed id.
- `terminal_read` is capped, cursor-based, and works without the shell WebView.
- `terminal_run` preserves terminal state and reports unknown completion
  honestly.
- `run` remains a stateless captured-process tool with unchanged behavior.
- WebView reloads do not duplicate or silently destroy broker-owned sessions.
- Closing/recreating Console UI does not terminate broker-owned sessions.
- Terminal output never enters the system prompt unless fetched through a
  terminal tool.
- Agent and broker code compile without MFC/WebView2.
- Linux/macOS use the same service/tool contracts with only the PTY backend
  replaced.
- Existing SSH terminals are read/write capable but never falsely treated as a
  known local shell.
- Long-running operation state is resumable across turns; remote durability is
  provided remotely, not by chat memory.

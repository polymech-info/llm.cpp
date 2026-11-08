# LLM agent OpenAI-compatible server

Handoff for `tanit-cli llm agent --serve`: a **daemon** that exposes an
OpenAI-compatible HTTP surface in front of the same agent turn pipeline as
chat UI / CLI (`resolve_agent_launch` → `run_turn` / `run_agent_turn`).

OpenAI compatibility describes the **transport / request shape**. It does **not**
mean the caller owns the tool loop.

> **The agent endpoint owns the tool loop. Clients contribute capabilities
> through registered `IAgentToolProvider`s, never by executing assistant
> `tool_calls`. `tools[]` is not implicitly bridged into the agent. MCP is
> optional and later — not the OpenDesign path.**

That sentence is the contract. OpenDesign’s emit-JSON / strip / `canFollow*`
path violated it and must not be copied into the next app.

Related: [slash-commands.md](./slash-commands.md), MCP embed (`pm_image_mcp_embed.cpp`),
orchestrator `tests/orchestrator/test-mcp.mjs` / `test-llm-server.mjs`.

---

## Architecture

```text
OpenDesign / other app
        │
        │ OpenAI-compatible
        │ POST /v1/chat/completions
        │ POST /v1/responses          (same agent semantics)
        ▼
┌───────────────────────────────────────┐
│ tanit-cli llm agent --serve           │
│                                       │
│  request → AgentLaunchSpec → Turn     │
│                     │                 │
│                     ▼                 │
│              SINGLE AGENT LOOP        │
│                     │                 │
│       ┌─────────────┼──────────────┐  │
│       ▼             ▼              ▼  │
│   Tanit tools    Skills/MCP     Host tools
│   files/image    existing MCP    design_*
│   lookup/etc.                   canvas_*
│       │             │              │
│       └─────────────┴──────────────┘
│                     │
│         tool mutations + SSE events   │
│                     │                 │
│                final answer           │
└───────────────────────────────────────┘
```

One agent loop. One tool registry. Multiple **tool providers**.

| Layer | Location |
|-------|----------|
| CLI flag / register | `pm_image_cmd_llm_agent.cpp` (`--serve`) |
| Server impl | `pm_image_cmd_llm_agent_server.cpp` / `.hpp` |
| Launch / tools / skills | `agent_launch`, `agent_factory`, MCP, skills features |
| HTTP | **cpp-httplib** (same family as MCP embed) — **not** Drogon |
| Queue | STL mutex + cv + fixed inflight — **no** Boost queue lib |

Do **not** overload `/v1/chat/completions` with a passthrough / `--no-tools` “fake
client loop.” If a raw model façade is needed later, give it a different path:

```text
POST /v1/model/chat/completions
# or a separate binary: tanit-cli llm proxy --serve
```

---

## Tool providers

The missing abstraction is not `tools[]` forwarding. It is a **trusted external
tool-provider attachment**.

Shipped as `IAgentToolProvider` (`src/llm/agent_tool_provider.hpp`):

```cpp
class IAgentToolProvider {
    std::string id() const;
    std::string name() const;
    std::vector<ToolDefinition> tools() const;
    ToolResult invoke(std::string_view, const json&, const ToolContext&);
    std::string context_prompt(const ToolContext&) const;
};

tool_providers::add(host_tools);          // process registry
AgentOptions.tool_providers.push_back();  // per-Agent extras
// AgentFactory::default_agent() already includes feature_host_tools()
```

`HostToolsFeature` merges schemas into the live `tools[]` (not behind
`request_tools`), injects `context_prompt`, and skips a host name that collides
with a builtin (warn, never silent shadow). The loop calls `invoke()` before
path execute / MCP.

MCP remains a later optional feature (`feature_mcp`). Do not route OpenDesign
through MCP for now. A host provider may `invoke()` over HTTP back to the app;
that is still a provider, not a second agent loop.

### Registration levels

1. **In-process** — `tool_providers::add` / `AgentOptions.tool_providers`
2. **Per-server** — app launches Tanit and registers a callback provider
3. **Per-session** — `tanit.tool_providers: ["opendesign"]` selects from the registry

Do **not** let arbitrary HTTP callers register new providers. Daemon startup
establishes the whitelist:

```powershell
tanit-cli llm agent --serve `
  --host-tool-provider opendesign=http://127.0.0.1:3727/api/agent-tools
```

Env: `TANIT_HOST_TOOL_PROVIDER=id=url;id2=url2`. Already-running daemons accept
loopback `POST /v1/host-tool-providers` with `{ id, url }` and an optional
`token`. The token is kept for the outbound provider calls (`X-Tanit-Session`
on that provider, not this daemon). The JSON response and the log line return
the URL with any `token` query removed.

Requests may select from that list only:

```json
{
  "model": "quick",
  "messages": [{ "role": "user", "content": "Make the title larger." }],
  "tanit": {
    "session_id": "...",
    "tool_providers": ["opendesign"],
    "selection": ["C:/project/.OpenDesign/uploads/photo.png"]
  }
}
```

Or a preset decides:

```text
preset: design
  builtins:  image_understand, file_read
  providers: opendesign
  skills:    design-editor
```

### Namespaces

Internal IDs stay qualified. Model-facing aliases may be short.

```text
builtin.image_understand
builtin.info_lookup
host.opendesign.design_query
host.opendesign.design_update
```

Collision = **startup error** unless an explicit alias override exists. Never
silently shadow.

### Host context

Providers contribute a short context prompt (document id, canvas size, selection).
The client then sends a trivial user message. Application integration is
`tools + context + optional skills`, not a custom agent prompt in every frontend.

### Mutations and revisions

State-changing tool results should carry revision ids:

```json
{
  "ok": true,
  "document_id": "abc",
  "revision_before": 470,
  "revision_after": 471,
  "changed": ["objects/title"]
}
```

Mutations may accept `expected_revision`. Mismatch → tool error (re-query, then
retry). Disk remains durable source of truth; **SSE is the low-latency notify**.
Do not make editor disk-polling the primary apply path.

Preferred stream events (OpenAI SDKs ignore named events):

```text
event: tanit.tool_call
event: tanit.tool_result
event: tanit.mutation     # provider, document, revision, operations
```

Editor: apply/refresh on `tanit.mutation`; on reconnect read revision and catch up.

---

## `tools[]` is not the host loop

Three meanings hide under one field:

| | Meaning | Who executes |
|---|---|---|
| **A** | “Here are tools I will run.” | Client (OpenAI default) |
| **B** | “Make these remote tools available to your agent.” | Tanit |
| **C** | “Constrain your existing universe to these definitions.” | Tanit, filtered |

`--serve` wants **B**. Interpreting **A** as **B** recreates the OpenDesign mess.

Today: `tools[]` is ignored (compat). When this surface is productized, **reject**:

```json
{
  "error": {
    "type": "unsupported_parameter",
    "param": "tools",
    "message": "Client-executed tools are not supported by the agent endpoint. Register tools as a Tanit/MCP tool provider."
  }
}
```

`tanit.tool_providers` selects **server-owned** providers. Do not merge request
`tools[]` into the agent registry.

---

## Constraints

1. **Not** image `tanit serve` / Drogon (`FEATURE_SERVE*`). Separate product surface.
2. **Not** a second LiteLLM — façade + agent loop only.
3. Preset/defaults inherited. Client `tools[]` is **not** an executor and must
   not be merged. Capabilities arrive as trusted providers / MCP.
4. Headless consent: `--consent-ui` is `auto-deny`, `auto-allow`, `cli`, `win32`,
   or `stream`. An empty flag on a serve turn falls through to the preset, then
   `auto-deny`. `stream` posts the card on the open SSE (or the realtime event
   queue) and blocks until `POST /v1/chat/consent`. A process already listening
   keeps the mode it was started with.
5. Concurrent turns: soft gate via `--concurrency N` (default small, e.g. 2–4).
6. Store / shipping: keep off product matrices until explicitly productized.
7. llama-server sidecar: **out of scope** (parked); local remains `router=llama` in-process or Custom→HTTP.
8. No new queue/network deps for MVP.
9. **Known hang:** >~8 concurrent agent turns (loopback) can wedge until client timeout — harness defaults `--in-flight` ≤ 8. Root cause still open in the turn pipeline.

---

## Endpoints

| Method | Path | Notes |
|--------|------|-------|
| GET | `/health` | `{ ok, ready, auth_required, concurrency{…}, … }` |
| GET | `/ready` | Lightweight readiness (`ready`, inflight/waiting/limit) |
| GET | `/v1/sysprobe` | `pm::perf::sample()` (+ peaks); `?mode=last` peek. 501 off Windows |
| GET | `/v1/models` | Presets as OpenAI model rows. Open on the direct HTTP port. The TLS hop requires a paired device or the serve key |
| GET | `/v1/credit` | PixlWiz budget `{ ok, data:{ spend, max_budget } }`. `ok:false` when signed out |
| POST | `/v1/chat/completions` | **Agent** endpoint. Non-stream + live `stream:true` SSE |
| POST | `/v1/responses` | **Agent** endpoint (planned). Same Turn / tool loop. Not a provider proxy |
| GET | `/v1/host-tool-providers` | Loopback list of registered `IAgentToolProvider`s |
| POST | `/v1/host-tool-providers` | Loopback register `{ id, url, token? }` HTTP provider |
| GET | `/v1/chat/commands` | `{ ok, data }` launcher command catalog |
| POST | `/v1/chat/commands/run` | Body `{ id }`. Variable toggle or process start. HTTP 200 always |
| POST | `/v1/chat/consent` | Unblock a `--consent-ui stream` ask. Body is the card reply, or `{ cancel:true }` |
| GET | `/v1/chat/skills` | `{ ok, data }` skills for `?cwd=` |
| GET | `/v1/chat/recent-paths` | `{ ok, data:{ files, folders } }` from the saved window layout (Windows) |
| GET | `/v1/chat/xblox-artifacts` | `{ ok, data:{ version, items } }`. Optional `?limit=`. A shared item also has `uri` (`vfs://desktop/…`) |
| GET | `/v1/chat/sessions` | `{ ok, data:{ sessions } }` |
| POST | `/v1/chat/sessions` | Save a session object. Requires `id`. 400 on bad JSON |
| GET | `/v1/chat/session` | `?id=` → `{ ok, data:{ session } }` |
| DELETE | `/v1/chat/session` | `?id=` → `{ ok, removed }` |
| POST | `/v1/speak` | Body `{ text }`. Runs the `speak` path tool (default chat voice) |
| POST | `/v1/speak/stop` | Abort the active speak |
| POST | `/v1/realtime/start` | Body `{ preset, chat_session_id, mic_send, voice?, instructions? }` |
| POST | `/v1/realtime/stop` | Stop the live voice session |
| POST | `/v1/realtime/ptt` | Body `{ held, finishInputTurn? }` |
| POST | `/v1/realtime/speaker` | Body `{ enabled }`. Mute or resume desktop realtime playback |
| POST | `/v1/realtime/cancel-agent` | Cancel the delegated `run_pm_agent` without stopping voice |
| GET | `/v1/realtime/events` | SSE `{ method, args }`. `realtimeLevel` is the latest mic level, delivered on its own so a caught-up client still sees movement. Keepalive every 15s |
| POST | `/v1/llm/path-tools/call` | Body `{ name, arguments }`. Direct path-tool execute. 400 without `name` |
| GET | `/v1/vfs/mounts` | Folder bookmarks with `share`. Each row is `vfs://desktop/<name>` |
| GET | `/v1/vfs/resolve` | Query `path` (absolute). `{ ok, item:{ path, shared, uri?, mount?, rel? } }` |
| POST | `/v1/vfs/resolve` | Body `{ paths:[...] }` (max 64). `{ ok, items:[...] }`. Unshared rows are `shared:false` |
| GET | `/v1/vfs/list` | Query `mount`, `path`. Lists one shared bookmark |
| GET | `/v1/vfs/stat` | Query `mount`, `path` |
| GET | `/v1/vfs/read` | Query `mount` + `path`, or `file` (absolute). Raw bytes. `file` only when that path is inside a shared bookmark |
| PUT | `/v1/vfs/write` | Query `mount`, `path`. Raw body, 8MB cap |
| POST | `/v1/vfs/mkdir` | Body `{ mount, path }` |
| POST | `/v1/vfs/delete` | Body `{ mount, path, recursive? }` |
| GET | `/v1/video/status` | Remote viewer. Idle until `POST /v1/video/start`. See below |
| POST | `/v1/video/start` | Body `{ input?, fps?, mjpegQuality?, bitrateKbps?, includeCursor?, encoder? }`. Starts capture |
| POST | `/v1/video/stop` | Stops capture. Idle again |
| GET | `/v1/video/sources` | Cameras, monitors, and windows. Each `spec` is an `input` string |
| POST | `/v1/video/source` | Body `{ input }`. Switch capture, or store the input while idle |
| GET | `/v1/video/mjpeg` | `multipart/x-mixed-replace` JPEG stream. 409 while idle |
| GET | `/v1/video/snapshot.jpg` | One JPEG. 409 while idle, 503 if no frame yet |
| POST | `/v1/video/control` | Pointer, keys, and text. Same body as the remote prototype |
| GET | `/v1/video/signal` | WebRTC mailbox. `?since=&waitMs=` |
| POST | `/v1/video/signal` | Push one signaling message |
| POST | `/v1/video/transcribe` | Raw audio body (WAV). Same as the prototype `/transcribe` |
| POST | `/v1/video/speak` | Body `{ text, format? }`. Returns audio bytes for the client to play |

Auth (when `--serve-api-key` / `TANIT_SERVE_API_KEY` set): required on every
`/v1/*` route except `GET /v1/models` on the direct HTTP port. `/health` and `/ready` stay open.
Host-tool register and list, and `POST /v1/browser/page-model/rank`, are machine-local
(403 from a paired phone). A paired-device token is a second bearer; see LAN pairing below.

### LAN discovery and pairing (desktop)

`--lan` (or `TANIT_SERVE_LAN=1`) keeps the existing HTTP server on `--host`/`--port`
and adds a TLS listener. Default TLS port is `--port + 1` (`--lan-port` overrides).
Phones use that TLS port. Cleartext HTTP stays loopback unless you also set a serve key.
The TLS terminator proxies to HTTP and injects `X-Tanit-Lan-Hop` so a phone is never
treated as loopback. Windows only (`DnsServiceRegister` + Schannel). Other platforms
return an error from `--lan`.

Discovery is `_tanit._tcp`. TXT keys are `txtvers=1`, `v=1` (protocol), `fp` (lowercase
SHA-256 hex of the SubjectPublicKeyInfo), and `name`. No secret and no token. mDNS
can fail on guest Wi-Fi or across VLANs; the process still serves TLS and prints
every IPv4 address plus the colon fingerprint. Trust that fingerprint, not the
advertised name. An address change does not require pairing again. A fingerprint
change (lost key container `TanitLanIdentity.rsa.v1`) revokes every device.

`--pair` (implies `--lan`) or loopback `POST /v1/lan/pair/begin` opens one short-lived
single-use ticket (default 300s, clamped 30–900). The QR and the `tanit://pair` URI
carry host, port, fingerprint, display name, and the secret (`tp1_` + 16 random bytes).
The phone must verify the TLS key against that fingerprint **before** sending the secret.
Eight wrong secrets lock pairing for 30 seconds.

```
tanit://pair?v=1&host=&port=&fp=&secret=&name=
```

`POST /v1/lan/pair` is accepted only through that TLS hop, or from the machine itself.
Body: `{ secret, device_id, device_name, scopes? }`. Status: 200 grant, 401 bad secret,
410 expired, 429 locked (`Retry-After: 30`), 404 no ticket, 403 rejected.
Success returns `{ token, device_id, fingerprint, scopes, protocol }`. The token is
`tn1_` plus 32 random bytes. The store keeps only its SHA-256. Re-pairing the same
device id rotates the token. Max 64 devices.

Scopes are `chat`, `files`, `tools`, and `video`. Omit `scopes` to take the ticket's grant
(default: chat, files, and tools — not video). `chat` is completions, sessions, speak,
realtime control/events, and `POST /v1/logs`. Starting realtime requires both `chat` and `tools`, because
the Voice Agent exposes delegated `run_pm_agent`; a chat-only device forces `no_tools`
on completions. `files` is `POST /v1/llm/path-tools/call`,
`POST /v1/ui/open-file`, and `/v1/vfs/*`. `tools` is the agent tool loop plus `/v1/chat/commands/run`,
`/v1/llm/each`, `/v1/llm/decide`, and `/v1/ui/open`. `video` is `/v1/video/*` (screen
capture plus pointer and key injection). A chat-only device cannot call it. Ask for it
on the ticket (`--pair-scopes chat,files,tools,video` or `POST /v1/lan/pair/begin`).
Host-tool providers and page-model rank stay on the desktop. The serve API key is full
access, including video. Direct loopback with an empty serve key stays open, as before.

| Method | Path | Who |
|--------|------|-----|
| GET | `/v1/lan/info` | Anyone. Name, fingerprint, TLS port, addresses. No secret |
| POST | `/v1/lan/pair/begin` | Machine-local. Body `{ scopes?, ttl_sec? }`. Returns the URI and secret |
| POST | `/v1/lan/pair` | TLS hop or machine-local. Exchanges the secret for a device token |
| GET | `/v1/lan/devices` | Machine-local. No token hashes |
| POST | `/v1/lan/devices/revoke` | Machine-local. Body `{ id }` |
| POST | `/v1/logs` | Paired device with `chat`, or the serve key. Phone log batch |

A paired phone posts its own Logcat to `POST /v1/logs` on the TLS host from
`tanit://pair` (`https://<host>:<port>/v1/logs`). The body is
`{ session_id, device_model, lines:[{ level, component, message, _time }] }`,
at most 200 lines. The desktop stamps `source=tanit-android`, `via=pair`, and
the paired `device` id, then forwards the batch to loopback VictoriaLogs
(`TANIT_LOGS_URL`, default `http://127.0.0.1:9428`). The phone never opens port
9428. `503` means the store is down and the phone should retry; `401`/`403` are
auth. This route does not add a scope, so an existing chat grant can call it.
System and other-app log buffers are not included; those stay on the ADB bridge.

`--lan-devices` and `--lan-revoke <id>` edit the same file (`lan/devices.json` under
the roaming config dir) and do not need `--serve`. A running daemon reloads it.
`GET /health` grows a `lan` object (protocol, name, fingerprint, tls_port, pairing, mdns, addresses) when `--lan` is on.

The phone app is separate. It should browse with `NsdManager`, pin `fp`, then call
the routes above. On Android 17 / API 37, broad LAN access needs `ACCESS_LOCAL_NETWORK`;
the system NSD picker can grant one device instead. That also applies to the sockets
the phone uses after pairing. Cloud rendezvous and Bluetooth are out of scope.

`/v1/vfs/*` serves folder bookmarks whose `share` flag is on. The mount name is the bookmark label, or the folder name when the label is empty. The address is `vfs://desktop/<name>/<path>`. `searchable` still only means chat search. `share` defaults off, so an existing bookmark is not on the LAN until it is turned on in Settings. The jail is that folder: list, read, write, mkdir, and delete stay inside it. `vfs://phone-n/<name>` is the same flag on a phone's own folder list. That sheet is not built yet, so a phone does not publish mounts. The Android composer, when the chat source is LAN, browses these desktop mounts and downloads picked files into the phone profile workspace.

Agent artifacts (status-bar outputs, `output_path`, and `GET /v1/chat/xblox-artifacts`) stay absolute desktop paths. A path becomes a link only when it is inside a shared bookmark. `POST /v1/vfs/resolve` answers that for up to 64 paths and does not read the file. A shared row is `{ path, shared:true, uri, mount, rel }` with `uri` of the form `vfs://desktop/<mount>/<rel>` (`rel` segments are percent-encoded). A relative path with a slash or backslash is shared when that file exists in exactly one shared folder. A bare filename, a path with `..`, a path outside every shared folder, and the shared folder itself are `{ path, shared:false }`. `GET /v1/vfs/read?file=<absolute>` returns the bytes for a shared file and `404` otherwise; it does not open the rest of the disk. `GET /v1/chat/xblox-artifacts` adds that same `uri` on items whose `path` is shared. The phone rewrites those paths into links and downloads with the existing read route.

Cwd for path tools: `X-Tanit-Cwd` header, or body `cwd` / `tanit.cwd` (else CLI `--cwd`).

With `--fs-jail` (or `TANIT_SERVE_FS_JAIL=1`), FS / search / pictures / service tools
**hard-deny** any path outside that cwd. Relative paths still resolve under cwd;
absolute paths and `..` escapes are rejected. `run` and MCP are unchanged (jail
those later if needed). pm-pics Tanit daemon passes `--fs-jail` by default.

Selection (disk paths, same as Explorer filmstrip): body `selection` / `tanit.selection`. Copied onto `Turn.selection`. Empty `image_understand.paths` is filled from those raster paths.

Session (agent memory / multi-turn, not OpenAI-standard): `X-Tanit-Session` header, or body `session_id` / `tanit.session_id`. Echoed as `X-Tanit-Session` + `tanit.session_id`. Empty creates a new id. Same id reuses session memory (CLI `--session-id`). This header is the chat session. It is not the Chrome broker token.

Stream extras on `POST /v1/chat/completions`: `event: tanit.thinking`,
`event: tanit.tool_call`, `event: tanit.tool_result`, and with
`--consent-ui stream` also `event: tanit.consent` / `event: tanit.consent.dismiss`.
The consent payload is the same card body the desktop transcript posts
(`reqId`, `title`, `risk`, `tool`, `action`, `resource`, `choices`, …).
`POST /v1/chat/consent` takes that reply (`decision` `allow` / `deny` /
`settings`, plus `lifetime`, `scope` or `scopes`, `aiDelegated`,
`sandboxBackend`). `{ "cancel": true }` denies every ask still waiting.
`settings` is stored as deny. A missing emitter denies rather than auto-allowing.

`POST /v1/chat/commands/run` returns the `run_chat_command` JSON at HTTP 200.
Variable commands: `{ ok, data:{ kind:"variable", id, active } }`. Process
commands: `{ ok, data:{ kind:"process", id, runId, status:"started" } }`.
Commands that need the desktop window: `{ ok:false, needsUi:true, error, id }`.
Invalid JSON is `{ ok:false, error:"invalid JSON body" }`.

Realtime SSE events use `{ method, args }`, the same names the chat panel
already calls (`realtimeTextDelta`, `consentRequest`, `dismissConsent`, …).
`GET /v1/realtime/events` follows the epoch from the latest `start`. A newer
start closes the previous stream.

Host providers invoke over HTTP; editor reload still uses revision poll
(`updated_by=cli`) until `tanit.mutation` lands.

Timing: `tanit.queue_wait_ms` / `tanit.exec_ms` (+ `X-Tanit-*` headers on non-stream).

### Remote video

`/v1/video/*` is the remote desktop viewer for a later phone client. Capture, MJPEG,
pointer mapping, and the signaling mailbox are the same code as
`tanit-cli video stream --server` (`pm_image_cmd_video_streaming.cpp`). The xblox
`videoStream` block still launches that command as its own process. The agent server
embeds one session in-process and does not open a second port.

The session stays **idle** until `POST /v1/video/start`. `--serve` does not capture
the screen by itself. `POST /v1/video/stop` releases it. One session per process:
one camera, screen, or window at a time. Several clients can watch that same
stream. `GET /v1/video/sources` lists every camera on the host plus monitors and
windows. Pass an entry's `spec` as `input` on `start` or `POST /v1/video/source`.
When the last MJPEG client disconnects, capture stops after about 1.5 seconds. A reconnect in that window keeps the session. Another viewer that is still connected keeps it too. `POST /v1/video/stop` stops immediately, including while a viewer is connected, and that viewer's later disconnect does not stop a session started afterward. A failed switch leaves the current capture running and still delivering frames. `tanit-cli video stream --server` does not do this; that process keeps capturing until it exits.

That string is the capture schema already used by `video record` and `videoSource`:
a camera id (Win32 symbolic link, `/dev/videoN`, or AVFoundation uniqueID), a
camera friendly name, or a screen spec (`screen:0`, `screen:0:hwnd=`,
`screen:0:wintitle=`). A friendly name still opens the first camera whose name
contains it. Empty `input` on start keeps the session default (`screen:0`).

Auth is the serve bearer or a paired device with the `video` scope. There is no
stream `token` and no `X-PM-Token` on these routes. `GET /v1/video/mjpeg` needs that
same `Authorization` header (OkHttp can set it; an `<img>` tag cannot). The web
prototype in `apps/remote` keeps talking to `video stream --server`, where `/status`,
`/mjpeg`, and `/snapshot.jpg` stay open and the other routes use the stream token.

Paths match the prototype with a `/v1/video` prefix:

| Prototype | Agent server |
|-----------|----------------|
| `GET /status` | `GET /v1/video/status` |
| `GET /sources` | `GET /v1/video/sources` |
| `POST /source` | `POST /v1/video/source` |
| `GET /mjpeg` | `GET /v1/video/mjpeg` |
| `GET /snapshot.jpg` | `GET /v1/video/snapshot.jpg` |
| `POST /control` | `POST /v1/video/control` |
| `GET` / `POST /signal` | `GET` / `POST /v1/video/signal` |
| `POST /transcribe` | `POST /v1/video/transcribe` |
| `POST /speak` | `POST /v1/video/speak` |

`/v1/video/speak` returns audio bytes for the phone to play. `POST /v1/speak` is the
desktop path tool and is a different route. Control bodies are unchanged (`action`
`move` / `click` / `drag` / `scroll` / `type` / `hotkey` / `key`, with `nx`/`ny` in
0..1). While idle, MJPEG, snapshot, control, and signal return
`409 { ok:false, error, phase:"idle" }`. Status returns `phase:"idle"` and
`running:false`. `media.endpoint` is the path to open (`/v1/video/mjpeg`).

A binary built without `FEATURE_VIDEO_STREAM` answers these routes with HTTP 501
`video_unavailable`. An MJPEG client holds one HTTP worker until it disconnects.

---

## OpenDesign (do / don’t)

**Do:** implement `IAgentToolProvider` whose `invoke()` runs `design_*` (in-process
or HTTP back to the editor). Chat sends plain `messages[]`. Tanit calls
`design_query` / `design_update`; builtins still own `image_understand`.
Successful `image_transform` / `image_create` writes are applied onto the selected
or source canvas `img` via `POST /api/agent-tools/apply-media` (not a second LLM loop).
SSE `tanit.mutation` (or revision poll as fallback) updates the live canvas.
Keep `createDesignTools` as the provider implementation, not an LLM overlay.
MCP is out of scope for this path.

**Don’t** (delete from the editor chat path):

- client-side `createDesignTools` / `extraToolsRef` as an LLM overlay
- extract / strip JSON from assistant content
- `canFollow*` and synthetic continue turns
- “respond only with tool JSON” / “never summarize”
- host-side duplicate `image_understand` after the agent already ran it

Frontend target:

```ts
sendMessage(text);

onSSE("tanit.mutation", mutation => {
  designStore.applyOrRefresh(mutation);
});

onAssistantDelta(delta => {
  appendChat(delta);
});
```

---

## Concurrency policy

Default today: **queue-block** (wait on the gate; no HTTP 429). Keep that for local
models — Local Gemma already showed clean queueing (`parallel ≈ serial` at
`concurrency=1`).

Per-preset defaults later (multiplexer value beyond an OpenAI shim):

| Preset class | Concurrency | Gate |
|--------------|-------------|------|
| Local small model | 1–2 | queue |
| GPU large model | 1 | queue |
| Remote API | N | soft / provider |
| Reasoning API | N + provider limits | soft / provider |

CLI `--concurrency` remains an override; preset policy supplies the default.

---

## Performance finish line

| Benchmark | Command / note |
|-----------|----------------|
| **Proxy-only overhead** | `npm run test:llm-server:bench` — loopback serial p50 (target &lt;1–3 ms) |
| **TTFT** | Same — live SSE first-byte |
| **Concurrency 1/2/4/8** | Same — queue_wait vs exec |
| **Warm tok/s** | Real models — still outstanding |
| **Mixed-preset queue** | Real models — still outstanding |

### `sysprobe` / CPU semantics

| Use | Metric |
|-----|--------|
| UI / statusbar | Instantaneous interval `%` from `pm::perf::sample()` (~2s cadence) |
| Benchmark process CPU | `ΔhostCpuTimeMs / wall` → `daemonCpuCores` |
| Benchmark system CPU | Mid-flight samples only → **avg + peak** (ignore `*-after` %) |

---

## Daemon install (sketch)

```powershell
# Dev / interactive
tanit-cli --no-gui llm agent --serve --host 127.0.0.1 --port 8090 `
  --concurrency 2 --consent-ui stream --serve-api-key $env:TANIT_SERVE_API_KEY

# Optional: Windows service / Task Scheduler — run the same command at logon.
# Probe: GET http://127.0.0.1:8090/ready  → { "ready": true }
# Health: GET /health  (includes concurrency + auth_required)
```

Keep bind on loopback unless you intentionally expose the agent surface.

---

## Todos

### Core

- [x] httplib + gate + OAI codec + `--serve`
- [x] Multi-turn `messages[]` → `Turn::seed_chat_messages`
- [x] `queue_wait_ms` / `exec_ms`
- [x] Live SSE TextDelta (+ `tanit.thinking` / `tanit.tool_call` events)
- [x] `X-Tanit-Cwd` / body `cwd`
- [x] Bearer `--serve-api-key` / `TANIT_SERVE_API_KEY`
- [x] Desktop LAN: `_tanit._tcp`, TLS proxy, QR pairing, per-device revoke (`--lan` / `--pair`)
- [ ] `POST /v1/responses` — same agent Turn, not a provider proxy
- [x] `IAgentToolProvider` + process registry + `HostToolsFeature` + `tanit.tool_providers`
- [ ] `--tool-provider` CLI whitelist (startup-trusted only)
- [ ] Reject request `tools[]` with `unsupported_parameter` (stop silent ignore)
- [x] SSE `tanit.tool_result` (display). `tanit.mutation` still open (revision poll)
- [x] Chat catalog routes (`commands`, `commands/run`, `skills`, `recent-paths`, `xblox-artifacts`, sessions)
- [x] `--consent-ui stream` + `POST /v1/chat/consent` + `tanit.consent` SSE
- [x] `POST /v1/speak`, realtime start/stop/ptt/events, `POST /v1/llm/path-tools/call`, `GET /v1/credit`
- [x] `/v1/video/*` remote viewer (same capture as `video stream --server`; scope `video`)
- [ ] Per-preset concurrency defaults (local queue vs remote N)
- [ ] Fix >~8 concurrent agent-turn hang
- [x] OpenDesign: host `IAgentToolProvider` + chat cleanup (no JSON scrape / follow-up flags)

### Ops

- [x] `/ready` + `/health.ready` / `auth_required`
- [x] Daemon install sketch (this doc)
- [ ] Productized Windows service wrapper

### Tests

- [x] loopback / live / local-gemma / stress / matrix
- [x] `npm run test:llm-server:bench` — proxy + TTFT + concurrency 1/2/4/8
- [x] Safe default `--in-flight` ≤ 8; `matrix:1k` uses 8
- [x] Tools suite: `npm run test:llm-server:tools` (loopback) + `:tools:local-gemma`
- [ ] Warm tokens/sec (real models)
- [ ] Mixed-preset queue latency (real models)

**Note:** Loopback RPS is headroom only — do **not** optimize the HTTP layer on it.

---

## Testing

```powershell
npm run build:cpp-fast   # or ninja tanit-cli
npm run test:llm-server
npm run test:llm-server:tools
npm run test:llm-server:tools:local-gemma   # optional
npm run test:llm-server:bench
npm run test:llm-server:stress
npm run test:llm-server:matrix
npm run test:llm-server:live            # optional
```

Skip (exit 0) when binary missing (fresh checkout).

---

## CLI sketch

```powershell
tanit-cli --no-gui llm agent --serve --host 127.0.0.1 --port 8090 --concurrency 2 --consent-ui stream --serve-api-key secret
```

Optional overlays: `--router`, `--model`, `--no-tools`, `--cwd`, `--fs-jail`,
`--http-workers`.
Later: `--tool-provider <id>` (repeatable whitelist).

## STA threads

This should become a general “host capability execution” pattern, not an `app_inspect_dump` special case.

Options:

1. Dedicated STA thread
- One message-pumping COM apartment.
- Post UI Automation jobs to it.
- Smallest change and likely fixes the server-worker difference.
- Weakness: if `FindAll` itself hangs, the STA thread is permanently poisoned. Stop still cannot recover it.

2. Out-of-process capability broker — recommended
- A small Tanit host process handles UI Automation over IPC.
- Give every request an ID, deadline, cancellation signal, and phase logs.
- If UIA exceeds its deadline or the client disconnects, terminate/restart the broker.
- Protects server concurrency slots and reliably recovers from broken UIA providers.
- Reusable for shell COM, cameras/codecs, browser drivers, plugins, Office automation, and other APIs that may block indefinitely.

3. Spawn the existing CLI command
- Server invokes the known-working `tanit-cli assistant app-inspect dump ...`.
- Hard timeout and kill are straightforward.
- Good short-term diagnostic/fix.
- More startup overhead, and cached inspect sessions must be returned to the server.

4. Send work to the desktop GUI process
- The GUI already has suitable Windows/UI infrastructure.
- Server calls it through IPC.
- Not appropriate as the only solution because `tanit-cli --serve` should work without the GUI.

5. Improve the in-process UIA traversal
- Require a target window when possible.
- Replace unrestricted `FindAll(TrueCondition)` with property conditions, cache requests, or bounded tree walking.
- Helps performance but cannot guarantee recovery from a hung external UIA provider.

6. Add a separate `app_list_open` fast path
- “What apps are open?” only needs `EnumWindows`; it does not need UI Automation.
- Fast and safe, but it does not replace deep inspection.

Recommended design:

- `inline` lane: deterministic shared C++.
- `sta` lane: trusted, bounded COM calls.
- `isolated` lane: UIA and other potentially unbounded host APIs.
- Every lane receives `request_id`, deadline, cancellation and progress callbacks.
- Client disconnect cancels the request.
- Only the isolated lane guarantees that a stuck native/COM call can be killed and its server slot released.

For now, I would use `app_list_open` for this prompt and make `app_inspect_dump` the first consumer of an isolated Windows capability broker. A dedicated STA thread alone is useful, but it does not solve “stopping won’t help.”

---

Yes—**move Windows host APIs behind an execution boundary, but don’t make STA the universal answer.** Your broker recommendation is sound; the UIA apartment assumption needs correcting.

**UI Automation clients should generally run on a dedicated MTA thread that owns no windows.** STA is appropriate for some other COM APIs, including many shell and Office operations. The GUI working while a server worker fails does **not** establish that STA is the missing ingredient.

Microsoft documents this in [UI Automation threading issues](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-threading). I couldn’t retrieve the page through browsing just now.

For Tanit, separate **apartment requirements** from **process isolation**:

| Capability | Execution context | Isolation |
|---|---|---|
| List open windows | User’s interactive session/desktop; `EnumWindows` | Usually in-process |
| UIA inspection/actions | Dedicated MTA thread; owns UIA objects | Broker process |
| Shell dialogs / STA COM | STA thread with message pump | Broker when calls can block |
| Office automation | Dedicated STA with message pump | Separate broker |
| Pure C++ processing | Ordinary worker pool | Usually in-process |

An isolated process can contain either STA or MTA executors. These are independent choices.

**First diagnose the CLI/server difference**

Log this at the actual API call site in both paths:

- Process ID, thread ID, session ID, target HWND and target PID.
- `CoInitializeEx` result and current apartment via `CoGetApartmentType`.
- Process elevation/integrity and whether the server is launched interactively or as a service.
- Calling thread’s desktop and process window station.
- Each phase: `COM init → UIA creation → root acquisition → traversal → property reads → serialization`.

Two common traps:

1. **COM initialization is per thread.** Initializing it on server startup does nothing for unrelated pool workers. A continuation may also resume on a different worker. Check `RPC_E_CHANGED_MODE`; do not silently continue after it.
2. **An apartment does not grant desktop access.** A service in Session 0, another user session, or a different desktop cannot be fixed by adding an STA thread. A desktop-inspection broker needs to run in the intended user’s interactive session.

Also create, use and release UIA interfaces inside their owning executor. Pass plain serialized results back to the server; avoid passing raw COM pointers between workers.

**The broker contract needs a few additional rules**

```text
CapabilityRequest
  request_id
  capability
  arguments
  timeout_ms

CapabilityPolicy                 // host-owned configuration
  apartment: none | mta | sta
  isolation: in_process | process
  concurrency_limit
  maximum_timeout_ms
```

The host—not the model—selects execution policy. Convert the request’s time budget into a local monotonic deadline.

For the first implementation:

- **One active UIA request per broker.** Killing a hung request then cannot destroy unrelated active work. Use a small broker pool if concurrency is needed.
- **The server supervises the deadline.** A watchdog inside the blocked executor cannot recover it.
- **Cancellation is initially cooperative; termination is the fallback.** COM cancellation is not a universal escape hatch from broken providers.
- **Disconnect cancellation must reach the supervisor.** It must not queue behind the hung UIA call.
- **Use a Windows Job Object for broker lifetime management**, including kill-on-close where appropriate.
- **Treat UIA handles as broker-local state.** Return session IDs tagged with a broker generation. Restarting the broker invalidates them; the server must reacquire the target.
- **Release the server request slot through a single terminal-completion path.** Success, timeout, disconnect and broker crash must race safely without double completion.

Bounded traversal, property caches, node limits and maximum depth still matter—but **a single property read can hang**, so traversal limits do not replace isolation.

My concrete first implementation would be:

```text
LLM server
  → async capability supervisor
  → authenticated local IPC
  → tanit-host --capability=uia
      → dedicated MTA executor
      → UIA objects and traversal
```

Add a separate pumped STA executor when a capability actually requires it. Keep `app_list_open` as the `EnumWindows` fast path.

**The architectural goal is that server workers never directly enter potentially unbounded desktop APIs.** They await supervised broker results, and cancellation remains effective even when the underlying API never returns.

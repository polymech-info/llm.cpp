# Network path tools — SSH, MQTT, Modbus

Status: desktop shipped. `npm run test:llm:network` (loopback, 26 checks) and `npm run test:llm:network:live` (prompt-driven, 12 checks) pass. Android compiles the same tool sources as stubs: the tools stay out of the phone catalog until the NDK transports are linked (see [Android](#android)).

The xblox blocks in `src/xblox/blocks/network_blocks.cpp` (`sshExec`), `mqtt_blocks.cpp` (`mqttClient`), and `modbus_blocks.cpp` (`modbusRead` / `modbusWrite`) are the behavior reference. The agent tools call the libraries those blocks already wrap. They do not call `BlockIO` or `run_block_roots`.

Android chat sets `FEATURE_XBLOX=0` in `cmake/android-chat-features.cmake`. Root `CMakeLists.txt` then forces `FEATURE_SSH_XBLOX`, `FEATURE_MQTT_XBLOX`, and `FEATURE_MODBUS_XBLOX` off. `dist/shared/features-android.json` listing those `*_XBLOX` flags as ON does not change the NDK compile. The NDK source list is `cmake/android-agent.cmake`.

## Tools

Four path tools. Each call opens a connection, does one operation, and closes it before returning.

| Tool | Library | What one call does | Security action |
|---|---|---|---|
| `ssh_exec` | libssh (`FEATURE_SSH`) | Resolve the host (including an `~/.ssh/config` alias), authenticate, run one command, return exit status and capped stdout/stderr. | Execute |
| `mqtt` | mqtt_cpp sync client (`FEATURE_MQTT`, new) | `publish`, `subscribe` (one message), or `roundtrip` (subscribe, publish, wait for the echo). | Execute |
| `modbus_read` | `media::modbus` (`FEATURE_MODBUS`) | Read one area on a `tcp:` or `rtu:` URL. | Read |
| `modbus_write` | `media::modbus` (`FEATURE_MODBUS`) | Write registers or coils. | Execute |

Modbus is two tools because `security_action_for_tool` in `src/llm/agent_factory.cpp` decides from the tool name alone. A read must not prompt. A write can move a machine. MQTT stays one tool and is always Execute: a publish to a command topic is the same kind of actuation, and subscribe is too rare to justify a fourth name.

Brokers and the Modbus TCP server stay test fixtures. The agent catalog does not grow `mqttServer`, `modbusServer`, or `modbusConnection`.

### `ssh_exec`

Matches `ssh_exec_block`: parse SSH config, optional identity file, password if given, otherwise `ssh_userauth_publickey_auto`, then one exec channel.

```json
{
  "host": "polymech",
  "command": "uptime",
  "timeout_ms": 30000
}
```

| Field | Required | Default | Notes |
|---|---|---|---|
| `host` | yes | | Config alias or hostname. `polymech` is the existing key-auth host on the dev machine. |
| `command` | yes | | One remote command. No PTY, no shell session. |
| `port` | | 22, or the config file | Sent to libssh only when the caller set it, same as the block. |
| `username` | | config file | |
| `private_key` | | agent / default keys | Absolute path. Empty keeps `ssh_userauth_publickey_auto`. |
| `timeout_ms` | | 30000 | Cap 120000. |
| `accept_unknown_host` | | false | Host verification stays on. An unknown key fails unless this is true. |

No `password` or `passphrase` field in the LLM schema. A secret in tool arguments ends up in the transcript, the session log, and the provider request. Key auth only for v1.

Call `pm::policy::ssh_access_allowed()` first. The Windows GPO build can deny it. The non-GPO stub returns true.

Success envelope:

```json
{
  "ok": true,
  "host": "polymech",
  "port": 22,
  "exit_status": 0,
  "stdout": " 16:05:01 up 3 days, ...\n",
  "stderr": "",
  "stdout_clipped": false,
  "stderr_clipped": false
}
```

Cap each stream at 32 KiB in the envelope. A non-zero exit is `ok: false` with the same fields and `error` set, matching the block (`BlockResult::Error` when `exitStatus != 0`).

### `mqtt`

Matches `run_mqtt_client` in `mqtt_blocks.cpp`: sync client, clean session, one topic, disconnect when the action finishes or `timeout_ms` fires.

| Field | Required | Default |
|---|---|---|
| `action` | yes | `publish` \| `subscribe` \| `roundtrip` |
| `topic` | yes | |
| `payload` | publish and roundtrip | |
| `host` | | `127.0.0.1` |
| `port` | | `1883` |
| `qos` | | `0` (0–2) |
| `client_id` | | `tanit-mqtt` |
| `timeout_ms` | | `5000`, cap `60000` |

Success envelope: `ok`, `action`, `host`, `port`, `topic`, `connected`, `published`, `subscribed`, `messages: [{topic, payload}]`. Subscribe with an empty `messages` array is a failure (`MQTT subscribe received no messages`).

Username and password auth is a later addition. The current client has no user field, and it would hit the same secrets-in-arguments problem as SSH passwords.

### `modbus_read` / `modbus_write`

Use the one-shot functions in `src/modbus/modbus_client.hpp` (`read_holding_registers`, `read_input_registers`, `read_coils`, `read_discrete_inputs`, `write_register`, `write_registers`, `write_coil`, `write_coils`). Each one opens a `ClientConnection`, does the operation, and closes it. The named `PooledModbusConnection` in the block stays on the xblox run scope.

URL forms, from `parse_endpoint` in `src/modbus/modbus_client.cpp`:

```text
tcp:<host>:<port>[:<unit>]
rtu:<device>:<baud>[:8N1][:<slave>]
```

| Field | Tool | Required | Default |
|---|---|---|---|
| `url` | both | yes | |
| `area` | both | | `holdingRegisters`. Read also takes `inputRegisters`, `coils`, `discreteInputs`. Write takes `holdingRegisters` or `coils` only. |
| `address` | both | | `0`. Accept the block's `40001` / `30001` / `10001` forms with the same `normalize_address` rules. |
| `count` | read | | `1`, cap 125 registers / 2000 bits (Modbus PDU limits) |
| `value` or `values` | write | yes | one integer, or an array. Coils accept `0`/`1`/`true`/`false`. |
| `slave` | both | | `0` = keep the unit in the URL. A value above 0 overrides it (`ClientConnection::open`). |
| `timeout_ms` | both | | `5000`, cap `30000` |

Envelope for both: `{ok, area, url, address, values: [...]}`.

Move `read_area_from_io`, `normalize_address`, `uint16_values`, and `bit_values` out of `modbus_blocks.cpp` into a small `src/modbus/modbus_args.{hpp,cpp}` that takes `nlohmann::json` instead of `BlockIO`. The block and the tool both call it, so the address rules cannot drift.

The Android v1 schema description says TCP. RTU on a phone needs USB serial permission, which this plan does not add.

## Connection lifecycle

v1 lifetime is the tool call.

```text
execute("ssh_exec" | "mqtt" | "modbus_read" | "modbus_write")
  → stack session
  → connect
  → one operation
  → destructor disconnects
  → envelope returned
```

No process-wide map, no `connection` / `instance` argument, no session id in the result.

Cancellation follows `run`: poll `media::cli::cancel_requested()`.

- SSH: the channel read loop checks it where `ssh_exec_block` checks `io.cancelled()`, every 10 ms. Return `error: "cancelled"`.
- MQTT: a second `steady_timer` on the same `io_context` fires every 50 ms. It checks the flag and calls `force_disconnect()`.
- Modbus: libmodbus calls block for at most `timeout_ms`. No extra polling.

Every call owns its session, so there is nothing to lock. `ensure_ssh_initialized` guards `ssh_init` with a static.

Two agent-loop rules in `agent_factory.cpp` keep device calls honest:

- A response that contains any network tool never takes the parallel dispatch path (`response_has_network_tools`). A write followed by a read must run in that order.
- The per-turn duplicate-call guard forgets earlier network signatures when `modbus_write`, `ssh_exec`, or `mqtt` runs (`forget_network_tool_signatures`). Reading the same registers after a write returns new data and is not a repeat.

Later, a `hold_ms` argument can park that same session object:

- Key: protocol + endpoint + auth identity (host/port/user/key for SSH, host/port/client_id for MQTT, url + slave for Modbus).
- A timer thread drops the session when `hold_ms` elapses or the process shuts down.
- The tool signatures stay the same. Omitting `hold_ms` means close before return, which is the v1 path.
- Cap the hold at about 60 s. A turn must not pin a PLC or a broker for the life of the chat.

Build the session types so that parking is a move into a map, and v1 simply lets the destructor run. Do not add the map or the timer in this change.

## Code layout

Lift the transport out of the block files so xblox and the tools share one implementation. The blocks become thin `BlockIO` adapters.

| New file | From | Gate |
|---|---|---|
| `src/lib/net/ssh_exec.hpp` + `ssh_exec.cpp` | `ssh_exec_block` in `network_blocks.cpp` (session, known-host check, publickey auto, channel read loop) | `FEATURE_SSH` |
| `src/lib/net/mqtt_client.hpp` + `mqtt_client.cpp` | `run_mqtt_client` in `mqtt_blocks.cpp` | `FEATURE_MQTT` |
| `src/modbus/modbus_args.hpp` + `modbus_args.cpp` | argument parsers in `modbus_blocks.cpp` | `FEATURE_MODBUS` |
| `src/llm/tools/network/NetworkTools.hpp` + `NetworkTools.cpp` | new | per-tool feature flags |

`FEATURE_MQTT` means the mqtt_cpp client is linked. It does not require `FEATURE_XBLOX`. Desktop default is ON. It uses the boost and `packages/mqtt_cpp` include setup that `FEATURE_MQTT_XBLOX` already has. `FEATURE_MQTT_XBLOX` keeps registering the blocks and starts depending on `FEATURE_MQTT`.

The SSH and MQTT transport signatures take a cancel callback (`std::function<bool()>`). The blocks pass `io.cancelled()`. The tools pass `media::cli::cancel_requested`.

`ssh_exec_block` and `mqtt_client_block` call the new functions and keep their current JSON fields (`timeoutMs`, `privateKey`, `password`, `acceptUnknownHost`). The block keeps password auth. Only the LLM schema drops it. Existing xblox documents, including `tests/xblox/ssh.xblox` and `tests/xblox/mqtt.xblox`, stay valid.

Tool sources follow `src/llm/tools/service/`:

- `network_tool_catalog()` returns the `ToolDef`s whose feature flags are on.
- `is_network_tool` and `do_network_tool` are what `path_tool_executor.cpp` dispatches.
- When a feature is off, its tool name is absent from the catalog. A stray call returns `unknown tool`.

### Catalog, registry, and policy

`AgentTool` is a `uint64_t` and bits 0–62 are taken (`src/constants.hpp`). Bit 63 is the last one. All four tools share it:

```text
AgentTool::Network = 1ULL << 63
```

1. `src/constants.hpp`: the `Network` enumerator, always in `k_agent_tools_all`. The registry rows are feature-gated, so the bit is harmless when no transport is built.
2. `src/llm/agent_tools.hpp`: new `AgentToolGroup::Network`.
3. `src/llm/agent_tools.cpp`: four registry rows, same flag, group `Network`, in catalog order. Turning the bit off hides all four. Hiding one name goes through `disabled_path_tools` / `tool_catalog_excluding`, which presets already use.
4. `src/llm/path_tool_catalog.cpp`: insert `network_tool_catalog()` after `make_info_lookup()`, matching the registry order (after `info_lookup`, before the service tools).
5. `src/llm/path_tool_executor.cpp`: dispatch next to `info_lookup`.
6. `src/llm/agent_factory.cpp` `security_action_for_tool`: `ssh_exec`, `mqtt`, `modbus_write` → `Action::Execute`. Without this they fall to the default `Read` and run without consent.
7. `src/policy/policy_catalog.cpp`: the `Agent.Tools.EnableNetworkGroup` policy (default on) and its `by_group(AgentToolGroup::Network)` mapping; `src/security/cli.cpp` `gpo list-tools` row `network_group`. `node scripts/generate-admx.mjs` regenerates the ADMX/ADML templates.
8. Desktop `CMakeLists.txt`: `src/lib/net/*` and `src/modbus/modbus_args.cpp` go into `pm-media` (which already links libssh, libmodbus, and Boost). `NetworkTools.cpp` goes into `_pm_llm_chat_sources` (the chat plugin). New option `FEATURE_MQTT` (default ON); `FEATURE_MQTT_XBLOX` now requires it.
9. `cmake/android-agent.cmake`: add the same sources beside `ServiceTools.cpp`.

Descriptions should tell the model when to use these tools instead of `run`. Use `ssh_exec` for one remote command on a configured host, `mqtt` for one broker exchange, and `modbus_read` / `modbus_write` for registers and coils. `run` remains the local shell.

## Android

Current state: `cmake/android-agent.cmake` compiles `NetworkTools.cpp`, `ssh_exec.cpp`, and `mqtt_client.cpp`, and `cmake/android-chat-features.cmake` sets `FEATURE_SSH=0`, `FEATURE_MQTT=0`, `FEATURE_MODBUS=0`. Each transport builds its stub, `network_tool_catalog()` returns nothing, and `npm run build:cpp-android` links. The remaining steps below turn the transports on one at a time.

The target state flips the definitions, with `FEATURE_XBLOX` left at 0:

```text
FEATURE_SSH=1
FEATURE_MODBUS=1
FEATURE_MQTT=1
```

**Consent.** Android builds with `FEATURE_SECURITY_AGENT` off, so item 6 above does nothing on the phone. Until the phone has a consent path, its default preset leaves out `ssh_exec`, `mqtt`, and `modbus_write`: they go in `disabled_path_tools` in `src/core/settings_chat_presets_android.cpp`. The user turns them on in settings. `modbus_read` can stay on.

Link into `pm_android_agent_stack`:

- **Modbus.** `src/modbus/modbus_client.cpp`, `modbus_args.cpp`, and libmodbus. Root `CMakeLists.txt` fetches libmodbus 3.1.12 and writes a minimal `config.h`. Move that block into `cmake/libmodbus.cmake` so `android-agent.cmake` can include it. This is the smallest of the three.
- **SSH.** Build libssh against the mbedtls that `android-agent.cmake` already fetches for curl (v3.6.2, the same tag as desktop). Do not fetch a second mbedtls. Two things from the desktop SSH block have to come with it:
  - **Threading.** Desktop non-Win32 builds mbedtls with `pm_mbedtls_user_config.h`, which sets `MBEDTLS_THREADING_C` and `MBEDTLS_THREADING_PTHREAD`. Without it, libssh `crypto_thread_init()` fails ("Error in auto_init()"). The Android mbedtls targets need the same definition, and curl gets it too.
  - **Symbol renames.** The `crypto_sign_ed25519*` / `crypto_scalarmult*` renames on the libssh target.

  Move both into `cmake/libssh.cmake`.
- **SSH files on the phone.** There is no `~/.ssh`. Set `SSH_OPTIONS_SSH_DIR` to `<profile>/ssh` (`files/tanit-profile/ssh`). That puts `config`, `known_hosts`, and `id_ed25519` there. The `polymech` alias resolves only if that `config` defines it. On the phone `private_key` is normally required.
- **MQTT.** mqtt_cpp is header-heavy and needs Boost.Asio. Desktop fetches boost 1.88 for `FEATURE_MQTT_XBLOX`. The first `npm run build:cpp-android` that links it is the gate. If that link fails, set `FEATURE_MQTT=0` on the NDK target and leave `mqtt` out of the phone catalog. Keep it on desktop, and record the link error in this doc. A stub that reports a publish it never sent is worse than no tool.

Cloud chat on the phone runs these tools in-process (`pm_android_agent` → `agent_factory`). LAN mode still executes tools on the desktop `llm agent --serve`. No Kotlin API is needed for v1.

## Tests

`tests/orchestrator/test-llm-network-tools.mjs`, with `--only modbus,mqtt,ssh,consent` and `--mode loopback|live`.

- **Loopback** (default) scripts the tool calls and asserts on the envelopes.
- **Live** sends plain prompts to the user's default chat preset (or `--preset`), for example `run uptime on polymech`. It asserts that the model called the right tool with the host or port from the prompt, and that the envelope carries real data. Hosts come from the prompt for now; user preferences or an `AGENTS.md` host list can feed the same prompts later.
- **Consent** (loopback only) runs with `--consent-ui auto-deny`: `modbus_read` still runs, while `modbus_write` and `ssh_exec` come back `security_denied` without connecting.

Use `runAgentRound` from `tests/orchestrator/test-agent-commons.mjs`. It runs `tanit-cli llm agent --router loopback --single-turn --json --log <file>` with `PM_LOOPBACK_SCRIPT`. The log's `tool_result` events carry the full tool envelope, and `toolResultEnvelope(bundlePath, toolName)` reads it out. `test-agent-memory.mjs` asserts envelopes the same way.

Do not use the `--serve` harness (`test-llm-server-tools.mjs`) for value checks. Its SSE `tanit.tool_result` sends `{name, text, id, duration_ms, path}`, not the envelope (`pm_image_cmd_llm_agent_server.cpp`). The existing serve tools suite already proves catalog-to-dispatch over HTTP.

Each round's loopback script is a single `tool_calls` turn followed by fixed final text. Pass `--consent-ui auto-allow` in `extraArgs`. These tools are Execute, so without it the run blocks on a prompt.

The fixtures reuse the servers the block tests already start. The thing under test is the path tool, not the block.

**Modbus.** Spawn the server the way `tests/orchestrator/test-modbus.js` does: a background `tanit-cli xblox run` with `modbusServer`, `holdingRegisters: [11, 22, 33]`, and a free `tcp:127.0.0.1:<port>`. Every tool call opens its own connection, so raise `maxRequests` above that file's `3` (use `10`) and set `durationMs` to cover all three rounds. Wait on `probeTcpPort` before the first round. Then:

1. `modbus_read` `holdingRegisters` address 0 count 3: `values` equals `[11, 22, 33]`.
2. `modbus_write` address 1 value 77: `ok`.
3. `modbus_read` again: `[11, 77, 33]`.

`npm run test:xblox:modbus` stays the block-level test.

**MQTT.** The `network/mqtt` suite in `tests/orchestrator/test-xblox-network.mjs` starts and stops the broker inside one document, which is too short-lived here. Spawn a background `xblox run` with `mqttServer` (`action: Start`, free port), then `wait` (`ms: 20000`), then `mqttServer` `Stop`. Wait on `probeTcpPort`, then:

1. `mqtt` `roundtrip` on a unique topic and payload.
2. The envelope's `messages[0]` equals that topic and payload.
3. Kill the broker process in `finally`.

**SSH.** Host `polymech`, overridable with `PM_SSH_HOST`. Command `uptime`. No `private_key`. libssh reads the default config and uses key auth, as `tests/xblox/ssh.xblox` already does for that alias. Assert `ok`, `exit_status === 0`, and stdout matching `/ up /` or `load average`.

If the alias does not resolve or the host refuses the connection, skip and print the reason. `--require-ssh` turns that skip into a failure. The dev machine is expected to pass with `--require-ssh`.

### npm scripts

```json
"test:llm:network": "node tests/orchestrator/test-llm-network-tools.mjs",
"test:llm:network:fast-build": "npm run build:cpp-fast && node tests/orchestrator/test-llm-network-tools.mjs",
"test:llm:network:ssh": "node tests/orchestrator/test-llm-network-tools.mjs --only ssh --require-ssh",
"test:llm:network:mqtt": "node tests/orchestrator/test-llm-network-tools.mjs --only mqtt",
"test:llm:network:modbus": "node tests/orchestrator/test-llm-network-tools.mjs --only modbus",
"test:llm:network:consent": "node tests/orchestrator/test-llm-network-tools.mjs --only consent",
"test:llm:network:live": "node tests/orchestrator/test-llm-network-tools.mjs --mode live --only modbus,mqtt,ssh",
"test:llm:network:live:ssh": "node tests/orchestrator/test-llm-network-tools.mjs --mode live --only ssh --require-ssh"
```

The MQTT broker fixture runs without `--no-wait`; that flag skips `wait` blocks, and the broker would stop before the first round.

`test:llm:network:ssh` is the check that key auth to `polymech` still runs `uptime`.

## Build order

1. Extract `src/lib/net/ssh_exec.*`, `src/lib/net/mqtt_client.*`, and `src/modbus/modbus_args.*`, and point the existing blocks at them. Run `npm run build:cpp-fast`. Then run `npm run test:xblox:modbus` and `npm run test:xblox-network -- --only mqtt`. The blocks must behave exactly as before.
2. Add `NetworkTools`, `AgentTool::Network`, `AgentToolGroup::Network`, the catalog, executor, security action, policy, and CMake changes.
3. Run `npm run test:llm:network:fast-build`, then `npm run test:llm:network:ssh`.
4. Android:
   - Add the feature flags and source lists.
   - Add `cmake/libmodbus.cmake`.
   - Add `cmake/libssh.cmake` against the existing mbedtls, with threading.
   - Add mqtt_cpp last.
   - Set the default preset's `disabled_path_tools`.
   - Run `npm run build:cpp-android`. The NDK library must link. Running SSH against a real host from the phone is out of scope.

## Out of scope

- Holding a connection across turns. `hold_ms` is only a shape the session types must allow.
- Agent-facing MQTT or Modbus servers, named pools, MQTT username auth, and SSH password auth from the LLM.
- RTU on Android.
- A second SSH stack beside `src/vfs/ssh` (file-panel SFTP). `ssh_exec` is a command channel, not a directory listing.
- Widening `AgentTools` past 64 bits. That is a separate change the next new tool will need.

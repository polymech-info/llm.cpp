# Sidecar Sandboxing

We follow Chrome's model: **AppContainer** as the primary isolation primitive on Windows.
No VMs, no Docker — OS-native, per-process, deny-by-default.

## Threat model

Sidecar tools (uvx, node, ruff, ty) run arbitrary user-supplied or LLM-generated code.
Goals:
- Tool cannot read/write outside the granted workspace folder
- Tool cannot access other processes or user credentials
- Network blocked by default (opt-in per capability)
- All child processes die when the parent task finishes
- No persistent side-effects outside `%LOCALAPPDATA%\Packages\pm-sidecar\`

## Isolation stack (layered, Chrome-style)

```
┌─────────────────────────────────────────────────────┐
│  AppContainer (LPAC)                                │  ← deny-by-default FS/net/process
│   ├── filesystem: only workspace + sidecar bin/    │
│   ├── network: none (or internetClient on demand)  │
│   └── process: can't see/open handles outside      │
│                                                     │
│  Job Object  (KILL_ON_JOB_CLOSE)                   │  ← fork bomb prevention, cleanup
│   └── CPU / memory / PID limits                    │
│                                                     │
│  Process Mitigation Policies                        │  ← DEP, ASLR, no Win32k, no DLL inject
│   └── PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY      │
└─────────────────────────────────────────────────────┘
```

| Layer | What it prevents |
|---|---|
| AppContainer | FS reads/writes outside allowlist, network, foreign process handles |
| Job Object | Orphan processes, resource exhaustion, fork bombs |
| Restricted token / Low IL | Privilege escalation if AppContainer somehow bypassed |
| Mitigation policies | DEP bypass, ASLR bypass, Win32k attacks, DLL injection |

## Library: arapuca

**[arapuca](https://github.com/sergio-correia/arapuca)** — Rust library with **C FFI**, covers the full stack in one call.
Ships as a static `.lib` + header, callable directly from our C++ code.

```cpp
#include "arapuca.h"

arapuca_sandbox_t* sb = arapuca_create("pm-sidecar");

// grant only what the tool needs
arapuca_grant_path(sb, workspaceDir,  ARAPUCA_READ | ARAPUCA_WRITE);
arapuca_grant_path(sb, sidecarBinDir, ARAPUCA_READ | ARAPUCA_EXEC);
arapuca_grant_path(sb, uvCacheDir,    ARAPUCA_READ | ARAPUCA_WRITE); // uv cache

// network: off by default, flip for tools that need pip/npm
// arapuca_set_network(sb, ARAPUCA_NET_OUTBOUND_ONLY);

arapuca_spawn(sb, "uvx ruff check .", workspaceDir);
arapuca_wait(sb);
arapuca_destroy(sb);  // deletes profile, kills job, reverts ACLs
```

Profile creation is cached after first use — per-spawn overhead is ~2ms.

## Folder sharing

AppContainer is deny-by-default. Grant paths explicitly via ACL at spawn time:

| Path | Access | Why |
|---|---|---|
| `workspaceDir` | R+W | User files the tool operates on |
| `dist/win-x64/bin/` | R+X | node.exe, uvx.exe, ruff.exe, ty.exe |
| `dist/win-x64/npm-global/` | R+X | Pre-installed globals (tsc, tsx, prettier) |
| `%TEMP%\pm-sidecar-*` | R+W | Scratch space, cleaned up on exit |
| `UV_CACHE_DIR` | R+W | uv package cache (optional, speeds up repeated runs) |

The container's own isolated storage (`%LOCALAPPDATA%\Packages\pm-sidecar\`) is always writable without a grant.

## Network policy

| Tool | Default | Override |
|---|---|---|
| `ruff`, `ty` | ❌ none | — stateless, never needs network |
| `uvx <tool>` | ❌ none | `ARAPUCA_NET_OUTBOUND_ONLY` for first-run downloads |
| `node` scripts | ❌ none | `ARAPUCA_NET_OUTBOUND_ONLY` if script needs fetch |
| `npm install` | `ARAPUCA_NET_OUTBOUND_ONLY` | — needs registry access |

Pre-warming the uv cache (via `setup:astral`) means uvx runs fully offline after first install.

## Tiers (incremental adoption)

### Tier 1 — Job Object only (ship now, no API changes)

Minimum viable: guarantees cleanup, CPU/mem limits. No FS isolation.

```cpp
HANDLE hJob = CreateJobObject(nullptr, nullptr);
JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
jeli.BasicLimitInformation.LimitFlags =
    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
    JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
jeli.ProcessMemoryLimit = 512 * 1024 * 1024; // 512 MB
SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
AssignProcessToJobObject(hJob, hProcess);
```

### Tier 2 — Low Integrity token (privilege drop, ~10 lines)

Prevents writing to anything above Low IL (AppData\Roaming, system32, registry hives).
Readable everywhere, writable only to Low IL paths and explicitly granted dirs.

```cpp
// drop to Low IL before CreateProcess
HANDLE hToken, hLowToken;
OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE, &hToken);
DuplicateTokenEx(hToken, TOKEN_ALL_ACCESS, nullptr,
                 SecurityImpersonation, TokenPrimary, &hLowToken);
// set Low integrity label on hLowToken, then CreateProcessAsUser
```

### Tier 3 — Full AppContainer via arapuca (target)

Full Chrome-style isolation. See usage example above.

## References

- [AppContainer isolation — Win32 docs](https://learn.microsoft.com/en-us/windows/win32/secauthz/appcontainer-isolation)
- [Launch an AppContainer — Win32 docs](https://learn.microsoft.com/en-us/windows/win32/secauthz/implementing-an-appcontainer)
- [Cheap sandboxing with AppContainers](https://blahcat.github.io/2020-12-29-cheap-sandboxing-with-appcontainers/)
- [arapuca — Rust/C sandbox library](https://github.com/sergio-correia/arapuca)
- [rappct — AppContainer/LPAC Rust toolkit](https://crates.io/crates/rappct)
- [Microsoft LaunchAppContainer reference](https://github.com/microsoft/SandboxSecurityTools/blob/main/LaunchAppContainer/LaunchAppContainer/LaunchAppContainer.cpp)
- [A field guide to sandboxes for AI](https://luiscardoso.dev/blog/sandboxes-for-ai)

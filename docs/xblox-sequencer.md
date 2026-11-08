# XBlox Sequencer — Design & Roadmap

> **Core principle:** opt in to AI only when needed.  The AI orchestrates and
> sets up — the heavy lifting runs through existing CLI commands and the
> sequencer without further LLM round-trips.

---

## Problem

For tasks like "take 3 photos every 5 seconds, speak a cue before each,
transform all results to sketch", the naive agent loop burns **one LLM
inference per step**:

```
LLM → capture1 → LLM → transform1 → LLM → wait 5s → LLM → capture2 → …
```

That is 8+ round-trips (~20 s of pure overhead) for work that is purely
mechanical after the initial plan is formed.  Spawning a child
`tanit-cli llm agent` for each timed step is even worse (new session,
new planner, full system prompt boot).

---

## Solution — `run_sequence`

A new LLM tool (`run_sequence`) that accepts a **structured list of steps**
and executes them deterministically in C++ via the **xblox block runtime**
(`media::xblox::run_block_roots`).

```
LLM round 1  →  run_sequence([speak, capture, wait, speak, capture, wait, speak, capture])
LLM round 2  →  [image_transform×3 in parallel via P6 dispatch]
LLM round 3  →  final reply
```

Total: **3 LLM rounds** instead of 8+.  No child agents.  Wall-clock-accurate
`wait` blocks.

---

## What xblox already provides

The xblox runtime (`src/xblox/xblox_commands.cpp`) supports:

| Block kind        | What it does                                           |
|-------------------|--------------------------------------------------------|
| `shell`           | Run a shell command (powershell / /bin/sh).  `storeAs` captures stdout. `background: true` fires without waiting. |
| `wait`            | `std::this_thread::sleep_for` — wall-clock precise.    |
| `log`             | Emit a log message (trace/debug/info/warn/error).      |
| `command`         | Invoke a registered CLI / app / ribbon command.        |
| `setVariable`     | Write to the shared context map.                       |
| `if`              | Conditional branch.                                    |
| `while`           | Loop with `condition` and `loopLimit`.                 |
| `for`             | Numeric range loop.                                    |
| `switch`/`case`   | Multi-branch dispatch.                                 |

The `run_sequence` tool exposes only **`shell`, `wait`, and `log`** to the LLM.
The more complex control-flow kinds (loops, conditionals) are not leaked to the
planner — they remain an internal detail available via the xblox CLI
(`tanit xblox run`) for hand-authored automations.

---

## `run_sequence` tool

**File:** `src/llm/tools/sequencer/SequencerTool.hpp/.cpp`

### Input schema

```json
{
  "steps": [
    {
      "kind": "shell",
      "command": "tanit audio tts --text \"Cheese!\" --play",
      "storeAs": "speak_result"
    },
    {
      "kind": "shell",
      "command": "tanit video image --dst cam_001.jpg"
    },
    { "kind": "wait", "ms": 5000 },
    {
      "kind": "shell",
      "command": "tanit audio tts --text \"Cheese!\" --play"
    },
    {
      "kind": "shell",
      "command": "tanit video image --dst cam_002.jpg"
    }
  ],
  "context": {}
}
```

### Output

```json
{
  "ok": true,
  "step_count": 5,
  "error_count": 0,
  "context": {
    "speak_result": ["audio played"]
  },
  "steps": [
    { "step": "0", "kind": "shell", "status": "ok", "message": "" },
    { "step": "1", "kind": "shell", "status": "ok", "message": "",
      "stdout": ["C:/work/cam_001.jpg"] },
    { "step": "2", "kind": "wait",  "status": "ok", "message": "5000 ms" },
    ...
  ]
}
```

The `context` map carries all `storeAs` captures.  The agent uses the returned
paths in a follow-up parallel `image_transform` dispatch.

---

## Typical camera-burst flow

```
User: "Take 3 photos every 5 seconds, speak cheese first, transform all to sketch"

Planner selects: run_sequence, image_transform

LLM round 1 — run_sequence:
  steps:
    log   "starting 3-shot burst"
    shell "tanit audio tts --text 'Cheese!' --play"
    shell "tanit video image --dst cam_001.jpg"   storeAs: cap1
    wait  5000
    shell "tanit audio tts --text 'Cheese!' --play"
    shell "tanit video image --dst cam_002.jpg"   storeAs: cap2
    wait  5000
    shell "tanit audio tts --text 'Cheese!' --play"
    shell "tanit video image --dst cam_003.jpg"   storeAs: cap3

→ C++ executes all 9 steps, returns {context: {cap1, cap2, cap3}}

LLM round 2 — parallel P6 dispatch:
  image_transform(cam_001.jpg → sketch_001.jpg)
  image_transform(cam_002.jpg → sketch_002.jpg)
  image_transform(cam_003.jpg → sketch_003.jpg)

LLM round 3 — "Here are your 3 sketches: …"
```

---

## xblox → scheduler bridge (future)

The user's note: *"we might introduce a scheduler in xblox"*.

Proposed new block kind `schedule_every` (xblox-native, NOT exposed to LLM):

```json
{
  "kind": "schedule_every",
  "interval_ms": 5000,
  "count": 3,
  "items": [
    { "kind": "shell", "command": "tanit audio tts --text 'Cheese!' --play" },
    { "kind": "shell", "command": "tanit video image --dst {{auto_path}}" }
  ]
}
```

This would collapse the 3-iteration loop into a single block handled entirely
by the xblox runtime, with no explicit `wait` blocks needed.  `{{auto_path}}`
would use an xblox variable resolver to generate a unique timestamped filename
per iteration.

**Implementation sketch:**
- Add `schedule_every` to `builtin_blocks.hpp` as a loop variant
- State: `iterations_done`, `next_fire_at` (steady_clock)
- Sleeps until `next_fire_at`, executes `items`, increments counter, repeats
- Honours `cancel_requested` on every sleep tick

---

## Files changed

| File | Change |
|------|--------|
| `src/llm/tools/sequencer/SequencerTool.hpp` | New — tool header |
| `src/llm/tools/sequencer/SequencerTool.cpp` | New — `do_run_sequence`, `make_run_sequence` |
| `src/llm/path_tool_catalog.cpp` | Added `make_run_sequence()` to `tool_catalog()` |
| `src/llm/path_tool_executor.cpp` | Added `"run_sequence"` dispatch |

---

## Test coverage

`tests/orchestrator/test-agent.mjs` — **`case-timed-capture`**

Goal: verify the planner selects `run_sequence` + `image_transform` for a
timed multi-shot camera task instead of spawning child agents or using `run`
with shell sleeps.

Checks:
1. Planner selected `run_sequence` (not `run` alone, not child agent pattern)
2. Agent called `run_sequence` with ≥ 2 shell steps containing `video image`
3. Agent followed up with `image_transform` call(s)
4. No `run` call pattern-matching `tanit-cli llm agent`

See `npm run test:agent:timed-capture`.

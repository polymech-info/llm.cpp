# FEATURE_LLAMA — Embedding-based semantic matching

GGUF embedding model support via [llama.cpp](https://github.com/ggml-org/llama.cpp),
used for zero-synonym-table tool and command routing inside the agent pipeline.

---

## Why

The `info_lookup` tool previously matched user queries against command descriptions
using a hand-curated glossary + stemmer + Levenshtein fuzzy matching (`string_utils.hpp`).
That works for known synonyms, but fails silently when a user phrase doesn't overlap
lexically with any registered command.

Embedding-based matching solves this by projecting both the query and each candidate
description into a shared semantic vector space. Cosine similarity then ranks candidates
without any synonym table.

```
"grab a frame"  ───embed──►  [0.23, -0.11, …]
                                       │
"webcam capture" ───embed──► [0.21, -0.09, …] ◄─── cos(q,c) ≈ 0.92 ✅
"audio record"  ───embed──►  [−0.44, 0.31, …] ◄─── cos(q,c) ≈ 0.12 ❌
```

---

## Architecture

```
src/
  lib/llm/llama/
    EmbeddingModel.hpp    ← public API (PIMPL, zero llama.h leakage)
    EmbeddingModel.cpp    ← llama.h confined here; model load + encode

  string_utils.hpp/.cpp  ← optional backend hook (FEATURE_LLAMA guard)
    set_embed_backend()   ← registers EmbeddingModel as fallback in match_score()
    has_embed_backend()
    embed_match_score()

  cli/
    pm_image_cmd_llama.hpp/.cpp  ← tanit-cli llama subcommand
    pm_image_register_cli.cpp    ← wires Cmd::Llama into CLI11 tree
    pm_image_cli_state.hpp       ← CLI11 state fields for llama options
```

### EmbeddingModel

PIMPL class — all `llama.h` / `ggml.h` symbols are confined to `EmbeddingModel.cpp`.
Consumers only include `EmbeddingModel.hpp`.

```cpp
media::llama::EmbeddingModel m;
m.load("dist/models/all-MiniLM-L6-v2.Q4_K_M.gguf", /*n_threads=*/4);

auto q = m.embed("take a photo");                     // EmbedResult (L2-normalised)
auto ranked = m.rank("take a photo", candidates);     // vector<MatchResult>

// Pre-embed intents at startup, then route at runtime (zero re-encode cost):
std::vector<media::llama::Intent> intents = …;
auto [name, score] = m.nearest_intent("grab a frame", intents);
```

All returned embeddings are **L2-normalised** — cosine similarity reduces to a
plain dot product, which is SIMD-friendly.

### string_utils backend

`string_utils.hpp` exposes an optional embedding hook compiled only when
`FEATURE_LLAMA=1`. The hook lets any code that already calls `match_score()` get
semantic scoring without knowing about `EmbeddingModel`:

```cpp
// Call once at startup (e.g. in agent_factory or InfoTool::init):
media::strutil::set_embed_backend([&model](const std::string& q, const std::string& c) {
    return media::llama::cosine_similarity(model.embed(q).embedding,
                                           model.embed(c).embedding);
});

// Existing callers of match_score() / matches_query() automatically use it:
float score = media::strutil::embed_match_score("webcam", "camera capture");
```

---

## CMake feature flags

| Flag | Default | Description |
|:-----|:-------:|:------------|
| `FEATURE_LLAMA` | **ON** | Build EmbeddingModel + link vendored `packages/llama` (prebuilt libs by default) |
| `FEATURE_LLAMA_SERVER` | **ON** | Copy `llama-server.exe` → `dist/win-x64/tanit-llama-server.exe` (requires FEATURE_LLAMA) |
| `FEATURE_COMMAND_LLAMA` | **ON** | Build `tanit-cli llama` CLI subcommand (requires FEATURE_LLAMA) |

Flags are set in `CMakePresets.json` and propagated as compile definitions to
`pm-media` and both executable targets (`tanit`, `tanit-cli`).

### llama.cpp dependency

Vendored at `packages/llama` (not a Git submodule, not a GitHub `FetchContent` clone).

The **main tanit build does not compile llama.cpp**. It links
`packages/llama/prebuilt/` (`FEATURE_LLAMA_USE_PREBUILT=ON`, the default) and
copies runtime DLLs into `dist/win-x64/`.

If prebuilt libs are missing and `FEATURE_LLAMA_USE_PREBUILT=OFF`, CMake builds
the same vendored tree in-process (`SOURCE_DIR packages/llama`) with server/tools
**off** so the main build stays small.

Update the vendored sources (does **not** compile). Overlays in
`packages/llama/TANIT-OVERLAY.txt` are kept (`build-cuda.ps1`, `build-metal.sh`,
`README-PREBUILT.md`, `prebuilt/`, `patches/`). Parent-tree CMake workarounds
(`/Zc:char8_t-`, `MATH_LIBRARY=""`) stay in the root `CMakeLists.txt`.

```powershell
npm run llama:update            # dry-run: resolve latest bNNNN tag
npm run llama:update:apply      # copy sources; re-apply patches/*.patch
# or pin: node packages/llama/update-sources.mjs --ref b10442 --apply
```

Writes `packages/llama/UPSTREAM.txt`. After APPLY, `prebuilt/` is stale until
you refresh it.

Refresh the Windows CUDA prebuilt (long, isolated — do **not** run this from the
main `npm run build:cpp` path):

```powershell
cd packages/llama
.\build-cuda.ps1
```

That script now also builds `llama-server.exe` (`LLAMA_BUILD_SERVER=ON` +
`LLAMA_BUILD_TOOLS=ON` — server lives under `tools/`). After the next tanit
link, CMake copies it to `dist/win-x64/tanit-llama-server.exe`.

Override the sidecar path at runtime with `LLAMA_SERVER_PATH` (planned spawn
path; see [hg-local-meta.md](llm/hg-local-meta.md)).

Main-tree / source-fallback flags (keep the in-app compile small):

| Option | Main tree | `build-cuda.ps1` |
|:-------|:---------:|:----------------:|
| `LLAMA_BUILD_TESTS` | OFF | OFF |
| `LLAMA_BUILD_EXAMPLES` | OFF | OFF |
| `LLAMA_BUILD_SERVER` | OFF | **ON** |
| `LLAMA_BUILD_TOOLS` | OFF | **ON** (required for server) |
| `LLAMA_BUILD_COMMON` | ON | ON |
| `LLAMA_BUILD_UI` | — | OFF (HTTP API only; no npm/HF web UI) |
| `LLAMA_OPENSSL` | — | OFF (local `127.0.0.1`) |
| `LLAMA_CURL` | OFF | OFF |
| `GGML_OPENMP` | OFF | OFF |
| `GGML_CUDA` | via `FEATURE_LLAMA_CUDA` | ON |

**MSVC workaround** (source fallback only): C++20 makes `char8_t` a distinct type,
breaking `llama.cpp`'s use of `u8""` literals with `std::ostream`. Fixed with:
```cmake
target_compile_options(llama PRIVATE /Zc:char8_t-)
```

---

### `tanit-llama-server.exe`

Windows sidecar of upstream `llama-server` (OpenAI-compatible HTTP on
`127.0.0.1`). Same ggml/llama DLLs as the in-process `TextChatModel` path.

```
dist/win-x64/tanit-llama-server.exe
```

The agent still uses in-process `llama_agent_client` today. Spawn/kill of this
binary is the planned `LlamaServerBackend` (sidecar `llama_server` hints already
written by `hg download`). Until then the exe is just shipped next to `tanit.exe`.

```powershell
# after packages/llama/build-cuda.ps1 + a tanit link:
.\dist\win-x64\tanit-llama-server.exe -m dist\models\<model>.gguf --host 127.0.0.1 --port 8080 --jinja
```

---

## CLI — `tanit-cli llama`

```
tanit-cli llama <subcommand> --model <path.gguf> [options]
```

### Subcommands

#### `embed` — Encode one or more texts

```powershell
tanit-cli llama embed `
  --model dist/models/all-MiniLM-L6-v2.Q4_K_M.gguf `
  --text "take a photo" `
  --text "record audio"
```

Output (JSON array, one item per text):
```json
[
  { "text": "take a photo",  "ok": true, "dim": 384 },
  { "text": "record audio",  "ok": true, "dim": 384 }
]
```

Add `--full-vec` to include the raw 384-float vector.

#### `match` — Rank candidates against a query

```powershell
tanit-cli llama match `
  --model dist/models/all-MiniLM-L6-v2.Q4_K_M.gguf `
  --query  "take a snapshot" `
  --candidate "capture photo from webcam camera" `
  --candidate "record microphone audio" `
  --candidate "resize image dimensions" `
  --top 2
```

Output:
```json
{
  "query": "take a snapshot",
  "best": "capture photo from webcam camera",
  "results": [
    { "candidate": "capture photo from webcam camera", "score": 0.87 },
    { "candidate": "resize image dimensions",          "score": 0.31 }
  ]
}
```

#### `benchmark` — Run built-in semantic cases

```powershell
tanit-cli llama benchmark `
  --model dist/models/all-MiniLM-L6-v2.Q4_K_M.gguf
```

Runs 5 hardcoded intent-routing cases; exits 0 when all pass.

### Options

| Option | Default | Description |
|:-------|:-------:|:------------|
| `--model` | *(required)* | Path to `.gguf` embedding model |
| `--threads` | 4 | CPU threads for decode |
| `--ctx` | 512 | Max context length (tokens) |
| `--top` | 0 (all) | Truncate ranked results to top-N |
| `--full-vec` | false | Include raw float vector in embed output |
| `--json` | false | Force JSON output (benchmark) |

---

## Models

Assets live in `dist/models/`. The directory is `.gitignore`d — download separately.

### Current default

| Model | Dims | Params | Size (Q4_K_M) | Load time | Encode / call |
|:------|:----:|:------:|:-------------:|:---------:|:-------------:|
| **all-MiniLM-L6-v2** | 384 | 22 M | **20 MB** | ~80 ms | ~15 ms |

```powershell
# Download (PowerShell)
Invoke-WebRequest `
  -Uri "https://huggingface.co/sentence-transformers/all-MiniLM-L6-v2/resolve/main/gguf/all-MiniLM-L6-v2.Q4_K_M.gguf" `
  -OutFile "dist/models/all-MiniLM-L6-v2.Q4_K_M.gguf"
```

### Do we need a larger model?

**Short answer: no, not yet.**

The L6 model scores **42/42** on the full agent-tool + CLI-command routing suite
(`test:llama:tooling`) covering 17 tool categories and 8 CLI commands, with a total
test runtime of ~7 seconds including process spawn overhead.

The one known failure mode (`"look up how to use the resize command"` pulling toward
`image_resize`) is a **structural problem**, not a model-capacity problem — any
embedding model will anchor on the prominent noun "resize". The fix is to strip
known tool names from meta-queries before embedding (or use a cross-encoder reranker
as a second pass).

That said, here is the upgrade ladder when/if quality becomes a bottleneck:

| Model | Dims | Size (Q4) | Expected gain | When to use |
|:------|:----:|:---------:|:-------------|:------------|
| all-MiniLM-**L12**-v2 | 384 | ~30 MB | +5-8 % on edge cases | More nuanced phrasing |
| BAAI/**bge-small**-en-v1.5 | 384 | ~30 MB | +10 % retrieval quality | Retrieval-tuned alternative |
| BAAI/**bge-base**-en-v1.5 | 768 | ~70 MB | +15-20 % overall | Clear quality step-up |
| **nomic-embed-text-v1.5** | 768 | ~100 MB | best open CPU model | Premium, still CPU-only |

All four are GGUF-compatible and drop in as `--model` arguments with no code changes.

#### Testing a different model

```powershell
# Download bge-small (example)
Invoke-WebRequest `
  -Uri "https://huggingface.co/BAAI/bge-small-en-v1.5/resolve/main/gguf/bge-small-en-v1.5-q4_k_m.gguf" `
  -OutFile "dist/models/bge-small-en-v1.5-q4_k_m.gguf"

# Run both test suites against it:
node tests/orchestrator/test-llama.mjs         --model dist/models/bge-small-en-v1.5-q4_k_m.gguf
node tests/orchestrator/test-llama-tooling.mjs --model dist/models/bge-small-en-v1.5-q4_k_m.gguf
```

---

## Tests

```
npm run test:llama:base     # smoke + timing (embed, match, benchmark)
npm run test:llama:tooling  # agent-tool + CLI routing correctness
```

### `test:llama:base`

`tests/orchestrator/test-llama.mjs`

Verifies core CLI plumbing:
- Single and batched `embed` (dimension, ok flag, JSON shape)
- `match` correctness for 7 semantic cases
- `--top N` result truncation
- Built-in `benchmark` exit code

**Typical result**: 24/24 passed in ~2 s (model already warm from process launch).

### `test:llama:tooling`

`tests/orchestrator/test-llama-tooling.mjs`

Two groups of routing correctness tests.
Candidates are short descriptive phrases (the same text the system will embed when
wired into the planner); the model maps user intents to the closest label, which is
then resolved back to a canonical tool/command name.

**Group 1 — agent-tool routing** (26 queries → 17 tool categories):

| Query | Expected |
|:------|:---------|
| "take a photo with the webcam" | `image_from_camera` |
| "transcribe speech from microphone" | `audio_transcribe` |
| "turn the photo into a pencil sketch" | `image_transform` |
| "chain multiple steps in a pipeline" | `run_sequence` |
| "what CLI commands are available?" | `info_lookup` |
| "say the text using text-to-speech" | `speak` |
| "do this in 5 minutes" | `schedule_in` |
| … 19 more … | |

**Group 2 — CLI command routing** (16 queries → 8 subcommands):

| Query | Expected |
|:------|:---------|
| "pencil sketch effect on photo" | `transform` |
| "take a webcam screenshot" | `video` |
| "ask the LLM to summarise something" | `llm` |
| "execute an xblox workflow" | `xblox` |
| "find closest matching phrase in a list" | `llama` |
| … 11 more … | |

**Typical result**: 42/42 passed in ~7 s (42 separate `match` subprocesses, ~165 ms each).

Both suites skip gracefully (exit 0) if the model file is not present.

---

## Resource usage

Measured on a mid-range Windows laptop (Intel i7, 16 GB RAM), 4 threads:

| Metric | Value |
|:-------|:------|
| Model file size | 20 MB |
| RAM after load | ~45 MB (model mmap + KV cache 4.5 MB + compute buffer 18 MB) |
| CPU during encode | ~4 cores × 1–2 s per call (then idle) |
| Encode latency (1 text) | ~15 ms (amortised; 80 ms cold start) |
| Match latency (17 candidates) | ~165 ms (1 query encode + 17 candidate encodes) |

"Not even a dent" is accurate — the full tooling suite (42 `match` calls × 17–8
candidates) completes in 7 seconds wall-clock including process-spawn overhead.
In-process use (model loaded once, embeddings cached) would run the same 42 matches
in under 1 second.

---

## Known limitations

### Tool-name-in-meta-query disambiguation

Querying `"look up how to use the resize command"` will rank `image_resize` higher
than `info_lookup` because "resize" appears prominently in both the query and the
`image_resize` candidate label.

This is expected MiniLM behaviour: the model is bag-of-words-weighted at 384 dims
and can't easily distinguish "resize as a verb" from "resize as a noun naming a tool".

**Workarounds (not yet implemented)**:
1. Strip known tool/command names from queries before embedding ("resolve the intent,
   not the noun").
2. Prefix candidates with a type tag: `[tool] resize image dimensions` vs
   `[meta] look up how to use a command` — the model will separate these.
3. Cross-encoder reranker as a second pass (heavier; only needed for borderline cases).

### BERT 512-token context cap

All BERT-family models (MiniLM, bge-*) cap at 512 WordPiece tokens. Long command
descriptions that exceed this are silently truncated. In practice all current
candidate labels are well under 50 tokens.

### Windows llama.cpp verbose stderr

`llama.cpp` logs model-loading details (tensor layout, KV sizes, etc.) to stderr even
at default log level. The CLI captures and suppresses this for the `--json` output
path; it is visible in raw terminal runs. This is a known cosmetic issue in the
llama.cpp library.

---

## Agent stack — application map

Complete survey of every decision point in the agent pipeline where embedding
similarity could replace a keyword heuristic or eliminate an LLM round.
Each point is rated by **gain** (token/round savings + precision) and **effort**.

---

### A1 — `lookup_tools()` in `InfoTool.cpp`

**Location**: `src/llm/tools/info/InfoTool.cpp` — `lookup_tools()`, line ~223  
**Current code**:
```cpp
if (!q_lower.empty() && hay.find(q_lower) == std::string::npos)
    continue;  // pure substring — misses synonyms entirely
```
**Problem**: literal substring matching against the tool's name + description.
`"speak"` won't find `audio_transcribe`; `"detect faces"` won't find `image_understand`.
This is the **weakest matcher in the stack**.

**Embedding fix**: replace `hay.find(q_lower)` with
`embed_match_score(query, tool.description) >= threshold` when `has_embed_backend()`.

**Gain**: High — eliminates "no matches" for semantically obvious queries, saving one
full LLM round per failed lookup.  
**Effort**: XS — 3-line change in `lookup_tools()`.  
**Risk**: Low — additive; falls back to substring when no backend is registered.

---

### A2 — `lookup_commands()` in `InfoTool.cpp`

**Location**: `src/llm/tools/info/InfoTool.cpp` — `lookup_commands()`, line ~116  
**Current code**: Uses `match_score(hay, q)` (stemmer + fuzzy coverage) with a
hard cutoff of `0.5`. Coverage scoring already handles many synonyms but still
fails for domain-shift queries (`"snapshot"` vs `"video"` command).

**Embedding fix**: for candidates below the fuzzy threshold, compute
`embed_match_score(q, command.description)` as a secondary score; fold into the
integer rank.

**Gain**: Medium — closes the remaining gap where fuzzy score < 0.5 but semantic
similarity is high. Avoids agent retry rounds.  
**Effort**: S — small scoring change, add threshold tuning.  
**Risk**: Low — existing path unchanged when embed score is not better.

---

### A3 — `prune_irrelevant_raster_tools()` in `agent_feature.cpp`

**Location**: `src/llm/agent_feature.cpp` — `prune_irrelevant_raster_tools()` / `turn_has_raster_in_scope()`, line ~313  
**Current code**: Prunes `image_understand / image_resize / image_transform` from
the planner's selection when no raster is detected in `turn.selection`, session
memory items, or a filename pattern in `turn.user_prompt`.

**Problem**: misses implied raster intent.
`"enhance this portrait"` or `"sharpen the face"` carry a raster operation even
when no file is attached yet; the heuristic over-prunes and forces an extra
`info_lookup` or `file_glob` round.

**Embedding fix**: add an embedding check alongside the raster-extension probe:
if `embed_match_score(user_prompt, "process edit transform resize an image or photo") >= 0.65`,
skip pruning even without an explicit file in scope.

**Gain**: Medium — prevents unnecessary pruning of raster tools, saving 0–1 rounds.  
**Effort**: S — one `has_embed_backend()` guard + score call.  
**Risk**: Low-medium — over-retention is safer than over-pruning (a no-op raster call
returns an error the model recovers from; over-pruning forces an extra planning round).

---

### A4 — `planner_prefers_info_lookup()` in `agent_feature.cpp`

**Location**: `src/llm/agent_feature.cpp` line ~230  
**Current code**:
```cpp
return q.find("command") != std::string::npos
    || q.find(" cli ") != std::string::npos
    || q.find("syntax") != std::string::npos;
```
A keyword scan on `user_prompt` that decides whether to **force** the tool
selection to `info_lookup` (read-only lookup turns).

**Problem**: Any prompt mentioning the word "command" (even `"run the sequence
command"`) triggers the collapse. There are already guards around it
(`execution_intent`, `has_action_tool`) but the root signal is still lexical.

**Embedding fix**: replace the `q.find(...)` calls with:
```cpp
embed_match_score(user_prompt, "look up discover documentation for a command or tool") >= 0.55
```
A pre-embedded reference phrase is cheap (one stored vector, one dot product).

**Gain**: High — this heuristic is the most frequent source of wrong collapses
to `info_lookup` when the actual intent is `execute`.  
**Effort**: S — single embed check replacing the OR chain; reference phrase
pre-embedded at startup.  
**Risk**: Low-medium — current guards remain; embedding is an additional signal, not a replacement.

---

### A5 — planner tool-name correction (`planner_valid_tool_names`)

**Location**: `src/llm/agent_feature.cpp` — `planner_valid_tool_names()`, line ~190  
**Current code**:
```cpp
if (!pm::llm::agent_tool_find_by_name(name))
    continue;  // hallucinated name silently dropped
```
When the planner LLM halluccinates a tool name (e.g., `"capture_image"` instead
of `"image_from_camera"`), it is silently discarded. The fallback is then
`info_lookup`, costing one extra round.

**Embedding fix**: when exact lookup fails, compute nearest-intent against the
pre-embedded tool-name corpus. If the best match score ≥ 0.75, substitute the
correct name and log a `planner_corrected_tools` diagnostic.

```cpp
// Pseudo-code:
if (!agent_tool_find_by_name(name) && has_embed_backend()) {
    auto [best, score] = embed_nearest_tool_name(name);
    if (score >= 0.75f) { name = best; /* re-validate */ }
}
```

**Gain**: High — directly converts hallucinated names into valid selections without
an extra round. Especially valuable for smaller/faster planner models.  
**Effort**: M — requires a pre-embedded tool-name table at agent startup; small
change to the validation loop.  
**Risk**: Low — wrong correction is still filtered by `agent_tool_find_by_name`
(the substitute must be a real tool name).

---

### A6 — `SkillsFeature` relevance filtering

**Location**: `src/llm/agent_feature.cpp` — `SkillsFeature::on_prepare()`, line ~660  
**Current code**: Injects **all** active skills' full `SKILL.md` bodies into the
system prompt (up to 96 KiB total) regardless of whether the current turn needs them.

**Problem**: A turn asking `"resize this image"` injects the full body of the
`tanit` skill, the `create-rule` skill, the `canvas` skill, etc. — all
irrelevant context that fills the context window and increases latency.

**Embedding fix**: filter `snap.entries` before injection:
```cpp
for (const auto& e : snap.entries) {
    if (!e.active) continue;
    if (has_embed_backend() &&
        embed_match_score(turn.user_prompt, e.description) < 0.3f)
        continue;  // skip irrelevant skill
    // inject body …
}
```
`SkillEntry::description` is already populated by `discover_skills()`.

**Gain**: Very High — this is the largest token-saving opportunity in the stack.
Irrelevant skill bodies can consume 5–40 KiB per turn. Filtering to only
relevant skills can cut system prompt size by 50–80%.  
**Effort**: S — two lines added to the injection loop.  
**Risk**: Low — `e.always` / `e.pinned` entries should bypass the filter (always inject).
Net effect: fewer tokens, same or better instruction following.

---

### A7 — planner compact-index pre-filtering

**Location**: `src/llm/agent_feature.cpp` — `apply_planner_tool_selection()`, line ~882  
**Current code**:
```cpp
const std::string compact_index = media::llm::info::build_compressed_markdown(info_opts);
```
The **entire** compact index (all tools + commands, typically 2000–5000 chars) is
sent to the planner on every turn.

**Embedding fix**: filter which tool descriptions enter the compact index by
embedding-similarity to the user prompt. Keep top-K most relevant + always
include `run`, `info_lookup`, `run_sequence` (always-available tools).

**Gain**: High — reduces planner input by 40–70%, improving speed and cost.
Especially important for small/fast planner models with tight context limits.  
**Effort**: M — requires pre-embedded tool-description corpus; compact-index
builder needs an optional relevance filter parameter.  
**Risk**: Medium — over-filtering might exclude a needed tool from the planner's
view. Mitigation: always include the full `info_lookup` schema so the agent can
self-recover via tool discovery.

---

### A8 — intent → TurnMode mapping robustness

**Location**: `src/llm/agent_feature.cpp` — `PlannerFeature::on_prepare()`, line ~360  
**Current code**:
```cpp
if      (intent == "query")   turn.mode = TurnMode::Lookup;
else if (intent == "execute") turn.mode = TurnMode::Execute;
// …
```
Exact string match against the planner LLM's `"intent"` field. If the model
outputs `"information"`, `"lookup"`, or `"search"` instead of `"query"`, the
mode stays `Auto`.

**Embedding fix**: pre-embed the canonical intent labels; when exact match fails,
use `nearest_intent(planner_intent_string, canonical_intents)`.

**Gain**: Low-medium — mode only affects which overlay prompt is loaded; already
degrades gracefully to `Auto`. Worth doing when the TurnMode system is used more
aggressively.  
**Effort**: XS — pre-embed 6 labels at startup; one fallback lookup.  
**Risk**: Very low — purely additive, Auto is the safe default.

---

### Priority order

| # | Site | Gain | Effort | Dependencies |
|:--|:-----|:----:|:------:|:------------|
| **A1** | `lookup_tools()` substring → embedding | High | XS | `has_embed_backend()` |
| **A6** | Skills: inject only relevant bodies | Very High | S | `SkillEntry::description` populated |
| **A5** | Planner: correct hallucinated tool names | High | M | pre-embedded tool corpus at startup |
| **A4** | `planner_prefers_info_lookup` keyword → embed | High | S | pre-embedded reference phrase |
| **A2** | `lookup_commands()` embed fallback | Medium | S | `has_embed_backend()` |
| **A3** | Raster pruning: embed intent probe | Medium | S | pre-embedded raster phrase |
| **A7** | Compact index pre-filtering | High | M | pre-embedded tool corpus |
| **A8** | intent → TurnMode embed fallback | Low | XS | pre-embedded 6 labels |

**Recommended first wave** (opt-in with `has_embed_backend()` guard, no regression risk):
A1 → A6 → A4 → A2  

**Second wave** (requires pre-embedded corpus at startup, small init cost):  
A5 → A3 → A8  

**Third wave** (architectural change, higher risk):  
A7 (compact-index filtering)

---

---

## FEATURE_ONNX — ONNX Runtime inference

Parallel feature flag that adds a general-purpose [ONNX Runtime](https://onnxruntime.ai/docs/get-started/with-cpp.html#api-reference)
inference backend alongside the existing llama.cpp embedding backend.

### Why

ONNX Runtime provides a runtime for **any** ONNX-format model (classification,
detection, regression, custom NLP, etc.) via a stable C++ API. Key advantages:

- Models exported from PyTorch / TensorFlow / scikit-learn with `torch.onnx.export`
  or `tf2onnx` run without recompilation.
- The pre-built `.lib`/`.dll` package is ~12 MB; no build step required.
- Future: DirectML execution provider for GPU acceleration on Windows.

### Architecture

```
src/
  lib/onnx/
    OnnxSession.hpp    ← public API (PIMPL, zero onnxruntime.h leakage)
    OnnxSession.cpp    ← onnxruntime_cxx_api.h confined here; load + run

  cli/
    pm_image_cmd_onnx.hpp/.cpp  ← tanit-cli onnx subcommand
    pm_image_register_cli.cpp   ← wires Cmd::Onnx into CLI11 tree
    pm_image_cli_state.hpp      ← onnx_cmd, onnx_info_cmd, onnx_run_cmd fields
```

### OnnxSession

PIMPL class — all `onnxruntime_cxx_api.h` / `onnxruntime_c_api.h` symbols are
confined to `OnnxSession.cpp`. Consumers only include `OnnxSession.hpp`.

```cpp
media::onnx::OnnxSession s;
s.load("dist/models/mnist-12.onnx", /*n_threads=*/4);

// Inspect metadata
for (const auto& t : s.input_info())
    printf("input: %s  shape: [...]  type: %s\n", t.name.c_str(), t.elem_type.c_str());

// Run with all-zero inputs (dynamic dims → 1)
auto r = s.run();
// r.ok, r.outputs_f32[i], r.output_info[i].shape
```

### CMake feature flags

| Flag | Default | Description |
|:-----|:-------:|:------------|
| `FEATURE_ONNX` | **ON** | Download ONNX Runtime pre-built package and build `OnnxSession` |
| `FEATURE_COMMAND_ONNX` | **ON** | Build `tanit-cli onnx` subcommand (requires `FEATURE_ONNX`) |

Both flags are set in `CMakePresets.json` and propagated as compile definitions.

### ONNX Runtime dependency

Auto-downloaded by CMake on first configure — no Git submodule, no manual step:

```cmake
set(_ORT_VERSION "1.20.1")
file(DOWNLOAD
    "https://github.com/microsoft/onnxruntime/releases/download/v${_ORT_VERSION}/onnxruntime-win-x64-${_ORT_VERSION}.zip"
    "${CMAKE_SOURCE_DIR}/third_party/onnxruntime-win-x64-${_ORT_VERSION}.zip" ...)
```

Extracted to `third_party/onnxruntime-win-x64-1.20.1/`; `onnxruntime.dll` is copied
to `dist/win-x64/` post-build.

| Component | Path inside zip |
|:----------|:----------------|
| Headers   | `include/onnxruntime_cxx_api.h` (and `onnxruntime_c_api.h`, etc.) |
| Import lib | `lib/onnxruntime.lib` |
| Runtime DLL | `lib/onnxruntime.dll` |

---

## CLI — `tanit-cli onnx`

```
tanit-cli onnx <subcommand> --model <path.onnx> [options]
```

### Subcommands

#### `info` — Inspect model inputs and outputs

```powershell
tanit-cli onnx info --model dist/models/mnist-12.onnx
```

Output:
```json
{
  "model": "mnist-12.onnx",
  "inputs":  [{ "name": "Input3",           "elem_type": "float32", "shape": [1,1,28,28] }],
  "outputs": [{ "name": "Plus214_Output_0", "elem_type": "float32", "shape": [1,10]      }]
}
```

#### `run` — Run inference with zero inputs

```powershell
tanit-cli onnx run --model dist/models/mnist-12.onnx
```

Output:
```json
{
  "ok": true,
  "outputs": [
    { "name": "Plus214_Output_0", "elem_type": "float32", "shape": [1,10], "size": 10 }
  ]
}
```

Add `--full-vec` to include the raw float32 output values.

### Options

| Option | Default | Description |
|:-------|:-------:|:------------|
| `--model` | *(required)* | Path to `.onnx` model file |
| `--threads` | 4 | Intra-op CPU thread count |
| `--full-vec` | false | Include raw float32 output vectors in `run` output |

---

## Models for ONNX smoke tests

Test models live in `dist/models/`. The directory is `.gitignore`d — download separately.

### MNIST digit classifier (smoke-test default)

| Model | Input | Output | Size |
|:------|:------|:-------|:-----|
| **mnist-12.onnx** | float32[1,1,28,28] | float32[1,10] | **26 KB** |

```powershell
# Download (PowerShell)
Invoke-WebRequest `
  -Uri "https://media.githubusercontent.com/media/onnx/models/main/validated/vision/classification/mnist/model/mnist-12.onnx" `
  -OutFile "dist/models/mnist-12.onnx"
```

The test suite (`test:onnx`) downloads this model automatically when absent.

### Upgrade options (drop-in)

| Model | Input | Task | Size |
|:------|:------|:-----|:-----|
| `squeezenet1.0-12.onnx` | float32[1,3,224,224] | ImageNet classification | ~5 MB |
| Any PyTorch/TF export   | model-specific | Any | varies |

All are drop-in as `--model` arguments to `onnx info` / `onnx run`.

---

## Tests

```
npm run test:onnx          # smoke: info + run (downloads MNIST model automatically)
npm run test:onnx:model    # pass --model <path> for a custom .onnx file
```

### `test:onnx`

`tests/orchestrator/test-onnx.mjs`

- Downloads `dist/models/mnist-12.onnx` if absent (~26 KB).
- Runs `onnx info` — checks `inputs`/`outputs` arrays present and well-formed.
- Runs `onnx run` — checks `ok=true` and at least one output with `size > 0`.

**Typical result**: 4/4 passed in ~300 ms (model load + two subprocesses).

Both suites skip gracefully (exit 0) if the binary is not built.

---

---

## FEATURE_VLM — Vision Language Model inference

Multi-modal image-to-text via [llama.cpp](https://github.com/ggml-org/llama.cpp) `mtmd` (multi-modal) layer.
Loads a GGUF language model + a separate vision projector (mmproj), then runs image + text → text generation.

### Architecture

```
src/lib/llm/llama/
  VlmModel.hpp   ← public API (PIMPL)
  VlmModel.cpp   ← llama.h / mtmd.h confined here

src/cli/
  pm_image_cmd_llama.hpp/.cpp  ← llama vlm subcommand
```

**VlmModel::generate()** pipeline:

1. Load image → `mtmd_bitmap`
2. Build chat prompt via `llama_chat_apply_template()` — auto-detects model's built-in template (ChatML, DeepSeek, etc.)
3. Tokenise text + image with `mtmd_tokenize()` — image marker `<__media__>` replaced by vision tokens
4. Eval all chunks via `mtmd_helper_eval_chunks()`
5. Greedy decode loop — each token streamed to `stderr` live, full JSON to `stdout` on exit

### CLI — `tanit-cli llama vlm`

```powershell
tanit-cli llama vlm `
  --model   dist/models/MiniCPM-V-4_5-Q4_K_M.gguf `
  --mmproj  dist/models/MiniCPM-V-4_5-mmproj-f16.gguf `
  --image   tests/onnx/xblox.png `
  --prompt  "Describe all UI elements you see." `
  --threads 8 `
  --n-predict 256 `
  --ctx 4096
```

Tokens stream to **stderr** as they are generated (live preview). Final JSON on **stdout**:

```json
{
  "ok": true,
  "image": "tests/onnx/xblox.png",
  "prompt": "Describe all UI elements you see.",
  "text": "The screenshot shows tanit / PiXilwiz …",
  "elapsed_ms": 98700
}
```

| Option | Default | Description |
|:-------|:-------:|:------------|
| `--model` | *(required)* | Path to GGUF language model |
| `--mmproj` | *(required)* | Path to vision projector (mmproj) |
| `--image` | *(required)* | Image path to analyse |
| `--prompt` | *(required)* | User question / instruction |
| `--threads` | 4 | CPU threads — **use 8 on Ryzen 3700X** |
| `--n-predict` | 1024 | Max tokens to generate |
| `--ctx` | 8192 | KV-cache context length |

### Recommended model — MiniCPM-V 4.5

[MiniCPM-V 4.5](https://huggingface.co/openbmb/MiniCPM-V-4_5) — 8.7 B params, built on Qwen3-8B + SigLIP2.
Best-in-class OCR / document parsing, surpasses GPT-4o-latest on OCRBench.

| File | Size | Purpose |
|:-----|:----:|:--------|
| `MiniCPM-V-4_5-Q4_K_M.gguf` | ~5 GB | Quantised language model |
| `MiniCPM-V-4_5-mmproj-f16.gguf` | ~1 GB | Vision projector |

```powershell
# Auto-downloaded by the test script on first run:
npm run test:minicpm
```

Or download manually from `openbmb/MiniCPM-V-4_5-GGUF` on HuggingFace.

### Performance (CPU-only)

Tested on **Ryzen 7 3700X** (8C/16T, AVX2):

| Threads | 32-token probe | Notes |
|:-------:|:--------------:|:------|
| 4 | ~50 s | default before tuning |
| **8** | **~40 s** ← use this | physical cores, memory-bandwidth sweet spot |
| 16 | ~45 s | hyperthreading overhead |

Approximate throughput: **~1.5 tok/s** at 8 threads.  
For comparison, iPhone 15 Pro Neural Engine: ~35 tok/s (dedicated NPU, not CPU).

> **Video understanding** (MiniCPM-V 4.5's 3D-Resampler, 96× token compression) requires
> the Python `transformers` stack with GPU — it is **not available through llama.cpp**.
> The `llama vlm` command processes single frames only.

### Test script

```
tests/orchestrator/test-minicpm.mjs
```

Downloads models on first run, runs inference on test images, writes:
- `tests/onnx/report-minicpm.md`
- `tests/onnx/report-minicpm.json`

```powershell
npm run test:minicpm                          # default images
npm run test:minicpm:screen                   # capture screen:0 first
npm run test:minicpm -- --image path/to/img.png --prompt "What is this?"
npm run test:minicpm -- --threads 8 --n-predict 128
```

---

## Next steps

| # | Task | Effort |
|:--|:-----|:------:|
| 1 | A1: Wire `EmbeddingModel` into `lookup_tools()` via `set_embed_backend()` | XS |
| 2 | A6: Filter `SkillsFeature` injection by embed similarity | S |
| 3 | A4: Replace `planner_prefers_info_lookup()` keyword chain with embed check | S |
| 4 | A2: Add embed fallback to `lookup_commands()` when fuzzy score < 0.5 | S |
| 5 | A5: Pre-embed tool corpus at agent startup; fix hallucinated planner names | M |
| 6 | Add `--model` to `tanit-cli llm info` output (active model visible) | XS |
| 7 | After `packages/llama/build-cuda.ps1`, confirm `dist/win-x64/tanit-llama-server.exe` is copied | XS |
| 8 | Evaluate `bge-small-en-v1.5` as quality upgrade (drop-in, same test scripts) | M |
| 9 | A7: Compact-index pre-filtering (planner token reduction) | M |
| 10 | Cross-encoder reranker pass for meta-queries containing tool names | L |

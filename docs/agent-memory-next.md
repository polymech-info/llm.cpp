# Agent memory — next phase

Roadmap extracted from [jcode.md](./jcode.md) memory/recall concepts, mapped to
PixlWiz code that already exists. This doc is the working plan for hybrid recall,
background injection, and durability follow-ups — not a second comparison essay.

Related:

- [jcode.md](./jcode.md) — full jcode vs PixlWiz analysis
- [embeddings.md](../embeddings.md) — local vector index design (`VectorStore`, USearch target)
- [vector-store-local.md](./vector-store-local.md) — shipped GGUF `VectorStore` (dense leg only)
- [test-agent-memory.mjs](../../tests/orchestrator/test-agent-memory.mjs) — loopback/live harness
- [test-agent-commons.mjs](../../tests/orchestrator/test-agent-commons.mjs) — shared bootstrap

---

## Design principle (from jcode)

> Keep structured session artifacts as ground truth. Add a **retrieval layer**
> rather than replacing session memory.

That means:

1. **`memory_state.items` / `artifacts` / `instructions`** remain authoritative.
2. **`memory_write` / journal recovery / turn persistence** keep writing the store.
3. A new recall path **selects** what to inject each turn — it does not become the only store.
4. Start with **lexical + embedding retrieval fused by RRF**; add a listwise judge only after the cheap path is measured.

---

## What PixlWiz already has

### Structured session memory (authoritative store)

| Concern | Location | Behavior |
|---------|----------|----------|
| Per-session `memory_state`, events, artifacts | `src/llm/agent_memory.cpp`, `agent_memory_artifacts.cpp` | End-of-turn persistence, item caps, error reflexions |
| Prompt injection (items, rules, transcript replay) | `src/llm/agent_context.cpp`, `agent_feature.cpp` | **Recency + byte budgets**, not broad retrieval |
| Feature bitmask (`InjectItems`, `ReplayTranscript`, …) | `src/llm/llm_constants.hpp` | Controls what gets injected |
| Memory tools | `src/llm/tools/memory/MemoryTools.cpp` | `memory_read` / `memory_write` / `memory_find` |
| Journal + recovery v1 | `agent_factory.cpp`, `session_recover_from_journal()` | Mid-turn journal; promote path outputs on `session_load()` |

Injection today is **deterministic caps**, not candidate retrieval:

```155:191:src/llm/agent_context.cpp
void append_session_items(..., chars_budget, max_items)
{
    // newest-first sort, stop at max_items / chars_budget
}
```

### Cross-session aggregation (not hybrid ranker)

`chat_session_store.cpp` is often mistaken for the retrieval engine. It is the
**session store + query surface**:

| API | Role |
|-----|------|
| `load` / `save` / `list_meta` / `prune` | Chat session transcripts (secure + legacy) |
| `load/save/delete_session_memory_document` | Sidecar `pm://session-memory/{id}` docs |
| **`query_store()`** | Walks all sessions + sidecars → flat `memoryItems[]`, `artifacts[]`, `securityItems[]` |

`memory_find` with `searchPastChats=true` calls `query_store()` then filters with
**case-insensitive substring** over `row.dump()` — not BM25, not embeddings, not RRF.
Its separate exact-turn mode (`itemId` + `includeBody`) is pointer materialization,
not ranked search:

```450:488:src/llm/tools/memory/MemoryTools.cpp
if (search_past_chats) {
    const nlohmann::json store = media::llm::chat_sessions::query_store(err);
    // matches_query: lowercase haystack.find(needle)
}
```

**Gap:** `query_store` is the right **candidate pool**; ranking is still naive.

### Exact assistant-turn continuity

Successful `turn_pair` items remain compact, but now retain a trusted
`metadata.source_log` locator plus `body_available=true` and `assistant_chars`.
The canonical per-run session log remains authoritative for the full reply.

The Session Items block renders only the turn ID and availability hint. For terse
follow-ups such as “write as md file,” the planner keeps `memory_find` alongside
`write_file`; the executor calls:

```text
memory_find(itemId="turn_…", includeBody=true, maxBytes=65536, offset=0)
```

`chat_session_store` resolves secure `pm://session-log/{session}__{log}` documents
first, then the runner-owned JSON fallback, and returns a UTF-8-safe bounded slice
of `result.final_text`. `next_offset` continues large bodies. Missing/deleted logs,
wrong session/item IDs, and non-turn items do not fall through to arbitrary
transcript or file reads. Ordinary `memory_find` remains summary-only.

This is progressive disclosure rather than `ReplayTranscript`: no automatic
artifact promotion and no broad transcript injection.

### Exact same-session tool-result continuity

Turn-pair metadata now retains compact tool-call IDs, status, payload byte count,
and `payload_available`; it does not copy result envelopes into memory. The
Session Items hint lets the agent recover one exact canonical result with:

```text
memory_find(itemId="turn_…", toolCallId="call_…", includePayload=true,
            maxBytes=65536, offset=0)
```

`chat_session_store` resolves that call ID inside the trusted run log and returns
a bounded UTF-8 slice of the serialized `tool_result` envelope. Retrieval is
same-session only, sensitive tool names are denied deterministically, and file
outputs continue to use artifact locators rather than payload copies. Missing
logs and unknown call IDs fail closed.

The guarantee begins at the canonical agent envelope. MCP bridge wire reduction
can truncate an oversized server response before it is logged; exact retrieval
cannot reconstruct bytes absent from canonical evidence. The
`tool-memory-bench` suite records this boundary separately from retrieval
correctness and compares quality, prompt bytes, memory-patch bytes, tool calls,
tokens, cost, and latency.

### Local vector search (files / commands / indexed content)

| Layer | Location | Notes |
|-------|----------|-------|
| **`VectorStore`** | `src/lib/llm/local_vector_store/VectorStore.hpp` | GGUF embed + brute-force cosine; `manifest.json`, `docs.jsonl`, `vectors.bin`; `source` tag includes `"memory"` |
| **Search facade** | `src/search/search_facade.cpp` | Modes: `name`, `grep`, `semantic`; backends: `exact`, `vector` |
| **Vector backend** | `src/search/vector_backend.cpp` | Index build/search over filesystem paths; adapter registry, fingerprint cache |
| **Design doc** | `docs/embeddings.md` | ONNX/USearch target; same public API shape |

Search is wired for **workspace/file discovery**, not yet as the agent memory
recall pipeline. Modes are orthogonal today — no RRF fusion across grep + semantic.

### Embeddings in the agent (pointwise, not indexed memory)

| Use | Location | Behavior |
|-----|----------|----------|
| Lazy embed backend | `EmbedInferenceFeature`, `EmbedInference.cpp` | `std::call_once` singleton; fuzzy tool/command match |
| Skill relevance gate | `agent_feature.cpp` (`SkillsFeature`) | Single cosine vs description; threshold 0.30; not hybrid |
| TurnMode detection | `agent_feature.cpp` | Cosine vs reference phrases |
| Wire reduction | `src/llm/reduce/reduce.cpp` | Lexical relevance scoring for compression |

No idle-unload lifecycle, bounded embed LRU, or `embedding_model` tagging on stored memories yet.

### Remote vector stores (xblox / LiteLLM — separate path)

`src/xblox/blocks/vector_blocks.cpp` talks to **remote** OpenAI-compatible
endpoints (`/v1/embeddings`, `/pgvector/v1/vector_stores/...`). Useful for workflow
blocks; **not** the local agent memory stack.

---

## jcode concepts to adopt (memory-only extract)

From jcode's live memory path (verified in `ref/jcode/crates/jcode-base/src/memory*.rs`):

### 1. Hybrid retrieval pipeline

1. Build a **broad candidate pool** (generous per-retriever limit, no hard cosine floor).
2. Rank independently:
   - **Dense:** cosine over stored embeddings (same `embedding_model` only).
   - **Sparse:** BM25 over normalized search text.
3. **Fuse** with reciprocal rank fusion (RRF).
4. **Precision layer** (optional, later):
   - One listwise LLM judge, or
   - Dynamic score-relative gate (variable k, no extra LLM cost).
5. Place winners in a **pending channel**; inject on the **next** turn once.

Important jcode details worth copying:

- Hard cosine thresholds hurt recall — avoid on the live fused path.
- Persist `embedding_model` on every vector; gate dense comparisons by model id.
- Lexical path keeps old vectors reachable after a model swap.
- Storage-level semantic duplicates **reinforce** existing entries, don't fork.
- Skills can be synthetic retrieval entries in the same index.

### 2. Async / non-blocking recall

- Turn N: background worker scores candidates.
- Turn N+1: `take_pending_memory()` injects once.
- Dedup: injected IDs, content signatures, TTL, overlap suppression.
- Failures never block the hot provider call.

### 3. Durability (partially done)

Journal recovery v1 is in; still open from jcode:

- Torn/corrupt JSONL salvage + backup before rewrite.
- Explicit `turn_id` on tool journal rows.
- Incomplete-turn detection → planner recovery event (partial).
- Crash-recovery tests in `test-agent-memory.mjs` harness.

### 4. Compaction (separate but coupled)

jcode's `CompactionManager` tracks `compacted_count` + durable summary; wire
reduction is a different layer. PixlWiz wire reducer exists; durable compaction
does not. See jcode.md P1 compaction section when context limits bite.

### 5. Embedding lifecycle

Load → share safely → bounded result cache → metrics → idle unload → force unload.
PixlWiz only has load-on-first-use today.

### 6. Skills as retrievable capabilities

Implemented foundation: `agent_resources.*` adapts legacy `SKILL.md` packages and
explicit `skill.json` / `resource.json` manifests into typed resources with
branding-derived stable URIs (`tanit://skill/<scope>/<id>`). The shared
`agent_retrieval.*` BM25/RRF primitives rank both memory and resources; the
`VectorStore` keeps searchable `source=resource` metadata while source files
remain authoritative. `SkillsFeature` injects a deterministic compact catalog,
never bodies. `info_lookup(kind=skill|resource)` searches metadata and
materializes an exact URI/qualified name on demand with sanitization, byte,
link, duplicate, cycle/depth, availability, and disabled-policy guards.

Relative links resolve within a skill package. Cross-skill references use
branded URIs and require another explicit `info_lookup`; they are never
recursively injected. Skill `allowed-tools` / `declared_tools` are ignored and cannot bypass
the independent `request_tools` schema-grant policy.

---

## Gap matrix

| Capability | jcode | PixlWiz today | Best reuse |
|------------|-------|---------------|------------|
| Authoritative session store | yes | yes (`agent_memory*`) | keep |
| Cross-session candidate pool | yes | **`query_store()`** | extend normalized search text |
| BM25 sparse rank | yes | **v1** (`agent_memory_recall.cpp`) | extend normalization |
| Dense cosine rank | yes | **v1** (`VectorStore` when `FEATURE_LLAMA`) | per-entry vectors |
| RRF fusion | yes | **v1** (RRF k=60) | tune pool sizes |
| Listwise judge / dynamic gate | yes | no | after benchmark |
| Pending inject (turn N → N+1) | yes | **v1** (sync `on_finish`; disk `.pending_recall.txt`) | async worker optional |
| Inject dedup (IDs, signatures) | yes | partial (caps + no re-enqueue after consume) | extend TTL/signatures |
| `embedding_model` on entries | yes | manifest-level only | per-entry field |
| Agent memory index | yes | **v1** upsert + BM25; `memory_find` still substring | wire `memory_find` to ranker |
| Full paths for read/selected refs | yes | **basenames only** in turn_pair | persist locators; see **Referenced resources** |
| Workspace scope for resources | yes | **no workspace model** | introduce workspace id + roots |
| File/workspace semantic search | n/a | yes (`search_facade`) | share embed backend |
| Remote pgvector (xblox) | n/a | yes (`vector_blocks`) | keep separate |
| Journal recovery consumer | yes | **v1 done** | tests + salvage |
| Background compaction | yes | wire reducer only | later |

---

## Full landscape: BM25 + RRF gap, overhead, impact, flags

This section is the consolidated map of **every retrieval path** in PixlWiz today,
what [vector-store-local.md](./vector-store-local.md) actually ships, what hybrid
recall adds, what it costs, and where to turn it off.

### Retrieval paths today (four separate stacks)

Paths 1–4 remain separate from **hybrid recall v1** (`MemoryRecallFeature` +
`agent_memory_recall.cpp`), which fuses BM25 (+ optional dense) and injects a
`## Recalled memory` block on the next turn. `memory_find` and file VectorStore
are still not unified with that ranker.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ 1. Structured session inject (hot path, every turn)                       │
│    agent_context.cpp → newest-first relevant items/rules up to caps         │
│    Main agent + planner: max 10 items; self-check/security rows excluded    │
│    llm_constants.hpp: k_session_items_max=50, inject budget=4000 chars      │
│    Policy: recency + byte budget — NOT query-conditioned retrieval          │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│ 2. memory_find tool (agent-initiated, on demand)                            │
│    MemoryTools.cpp → query_store() + case-insensitive substring             │
│    Optional searchPastChats → cross-session flat pool, still substring only  │
│    Exact itemId + includeBody → bounded canonical turn final_text            │
│    Policy: agent must call tool; no ranking; no semantic match               │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│ 3. Local VectorStore (files / xblox / workspace — NOT agent memory)         │
│    VectorStore.hpp + search_facade.cpp + vector_backend.cpp                 │
│    Dense cosine only; chunk ingest; see vector-store-local.md               │
│    Policy: workspace discovery; separate store dirs; no RRF with grep       │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│ 4. Pointwise embed in agent (no memory index)                               │
│    EmbedInferenceFeature → lazy GGUF singleton                              │
│    SkillsFeature → single cosine vs description (threshold 0.30)            │
│    TurnMode / fuzzy tool match — one-shot scores, not indexed recall        │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│ 5. Remote pgvector (xblox blocks — workflow only)                           │
│    vector_blocks.cpp → LiteLLM / OpenAI-compatible endpoints                │
│    Not local agent memory; keep separate                                    │
└─────────────────────────────────────────────────────────────────────────────┘
```

**Ground truth (unchanged by hybrid recall):** `agent_memory.cpp` session JSON,
session-memory sidecars (`chat_session_store.cpp`), journal recovery, and
`memory_write` / artifact promotion. Hybrid recall is a **read-side ranker +
injector** on top of that store — not a replacement.

### Hybrid recall v1 (shipped baseline)

```
Turn N user prompt
       │
       ├─ hot path: structured inject (items/rules as today)
       │
       └─ on_finish (sync today; async worker optional later):
              collect candidates
                ← current session memory_state.items
                ← default_memory.doc (memory_write prefs)
                ← query_store() cross-session pool
              normalize search_text per entry
              BM25 rank ──┐
              dense rank ─┼─► RRF fuse (no hard cosine cutoff)
                            │
              suppress hits already in recent Session Items (max 10)
              emit MemoryUpdate.recall telemetry
                (candidates, pool, bm25_ms, dense_ms, overlap_suppressed,
                 injected_hits/bytes, pending_enqueued)
              persist pending_recall → {session}.pending_recall.txt

Turn N+1 on_prepare
       │
       ├─ take_pending_recall() → "## Recalled memory" block (once)
       ├─ skip re-enqueue if this turn consumed pending
       └─ agent_context.cpp appends block to system prompt
```

Overlap suppression keeps Recalled memory complementary to the live Session Items
window: same-session IDs already in the newest-10 inject set are dropped;
cross-session / global hits still inject.

Hybrid recall is enabled in `k_default_structured_memory_features`. It can still
be disabled for comparison with:
`PM_LLM_STRUCTURED_MEMORY_FEATURES=default,-IndexMemoryEntries,-HybridRecallSearch,-InjectPendingRecall`.

### What `vector-store-local.md` gives vs what BM25 + RRF still needs

[vector-store-local.md](./vector-store-local.md) documents the **dense leg** only:
GGUF embed → L2-normalized vectors → brute-force cosine → top-K. Suitable for
≤ ~100 k chunks; in-process **< 10 ms/query** at ~6 k chunks (per benchmark).

| Piece | In VectorStore / doc | Needed for agent hybrid recall |
|-------|----------------------|--------------------------------|
| GGUF embed + manifest | ✅ `manifest.json` (`embedding_model`, dims) | Reuse; share `EmbedInference` singleton |
| Dense top-K search | ✅ `VectorStore::search()` | Wire to memory entries, not just files |
| Chunking / embed text | ✅ `make_embedding_text`, chunkers | Memory items are short → usually **1 vector/item** |
| Atomic save / reload | ✅ `save()`, `reload()` | Garden worker batch upsert after turn |
| `source` tag | ✅ e.g. `"memory"` in design | `source=memory\|skill\|session_item` |
| **BM25 sparse rank** | ❌ not in VectorStore | ✅ `agent_memory_recall.cpp` over normalized text |
| **RRF fusion** | ❌ | ✅ v1 (`k_recall_rrf_k=60`) |
| **Candidate pool builder** | ❌ (files only) | ✅ session items + `default_memory.doc` + `query_store()` |
| **`search_text` field** | ❌ on memory items | ✅ `memory_recall_normalize_search_text()` |
| **`embedding_model` gate** | ✅ manifest-level | Per-entry tag still TODO; manifest gates dense store |
| **Pending channel** | ❌ | ✅ turn N score → N+1 inject; disk sidecar for CLI |
| **Index gardening hook** | ❌ for sessions | ✅ `MemoryRecallFeature::on_finish` upsert via `VectorStore` |
| **Inject dedup** | ❌ (caps only) | ✅ overlap vs recent-10 Session Items; signatures/TTL still TODO |
| **RRF with grep/workspace** | ❌ | Out of scope initially; file search stays separate |

**Important VectorStore limitations for memory gardening** (from vector-store-local.md):

- Path dedup is for **files** — memory index must upsert by **stable entry id**.
- Not thread-safe — serialize background worker + save batches.
- Cross-store model mismatch errors — re-embed or BM25-only until rebuild.
- Brute-force scale — fine for chat memory (~10 k entries); USearch later per embeddings.md.

BM25 can run **without** loading the embed model (lexical-only profile). Dense
leg requires `FEATURE_COMMAND_LLAMA_INFERENCE` + a GGUF on disk.

### Overhead: session ingest + background gardening

Assume hybrid recall is **off the LLM hot path** (jcode pattern: score turn N,
inject turn N+1). Costs below are **incremental** over today's structured memory.

| Resource | Order of magnitude | Notes |
|----------|-------------------|--------|
| Embed model RAM | ~33 MB (bge-small q8) – ~109 MB (bge-base q8) | Shared singleton; already loaded if skills embed gate runs |
| Per new/changed memory item | ~5–50 ms CPU embed | Summaries capped at 400 chars → one vector typical |
| Index disk per entry | ~3 KB (384-dim f32 + jsonl row) | 3 k entries ≈ ~9 MB on disk |
| Search at recall time | O(n) scan; **< 10 ms @ ~6 k** in-process | BM25 adds cheap CPU over same candidate list |
| Gardening I/O | `save()` after batch upsert | Debounce/coalesce (e.g. 1–2 s after turn) |
| Hot-path LLM latency | **0 ms** if async | Sync recall in `on_prepare` would add embed+scan — avoid |
| Prompt tokens (inject) | +~200–2000 chars tunable | Separate budget from `k_session_items_inject_chars` |

**Scale sanity check (chat memory, not doc corpus):**

- Per session: max **50 items** evicted oldest-first.
- 100 sessions × 30 retained items ≈ **3 k vectors** → trivial for brute force.
- Pain points: **100 k+** entries, low-RAM machines with model always resident,
  or sync recall on every turn — not typical chat usage.

**Background worker responsibilities:**

1. **Index** — diff items written this turn (artifacts, turn pairs, memory_write,
   journal recovery promotions) → embed new/changed → upsert `{config}/agent-memory-index/`.
2. **Recall** — on turn finish (or debounced): run BM25 + dense + RRF for
   *next* turn's query = user prompt (+ optional planner objective).
3. **Garden** — tombstone evicted items; optional periodic full reconcile from
   `query_store()`; reinforce semantic duplicates (bump count, no fork).

Worker must not block provider calls; failures log and skip inject — same as jcode.

### User-visible impact (chat memory retrieval)

| Today | With hybrid recall |
|-------|-------------------|
| Recent 10 relevant items injected deterministically (planner + executor) | Query-conditioned snippets from current + past sessions supplement the fast recent window |
| Agent must call `memory_find`; substring only | Top-K auto-injected next turn; paraphrase + exact token |
| Cross-session facts need lucky wording match | BM25 catches tokens; dense catches paraphrase |
| Long sessions lose early decisions from inject window | Evicted items still reachable via recall block |
| Skills: catalog in prompt + optional cosine gate | Skills can share index (`source=skill`) — later P1 |

**What it does not replace:**

- Authoritative writes (`memory_write`, artifacts, journal) — unchanged.
- Scoped / secure items — same filters as `query_store()` before ranking.
- Full transcript — targeted recall block, not `ReplayTranscript` (already off by default).

**Risks / mitigations:**

| Risk | Mitigation |
|------|------------|
| Wrong memory injected | RRF + optional judge; dedup signatures; shadow mode |
| Extra prompt tokens | `k_recall_inject_chars`, top-K cap |
| Stale dense index | BM25 over live `search_text` from JSON; re-garden on write |
| Model swap | Dense off for old vectors; BM25 still serves lexical leg |

**When impact is low:** single-session, short chats where 50 newest items already
cover everything — hybrid recall adds little over structured inject.

**When impact is high:** multi-session workflows, codenames/preferences from prior
chats, long-running tasks where evicted items matter, reduced `memory_find` tool churn.

### Referenced resources, paths, and workspaces

Session memory is **path-centric for outputs we write**, weak for everything else.
There is **no workspace concept** — roots, selection, and file indexes are ad hoc
per turn or per search invocation.

#### Three layers (not unified)

| Layer | Tracks | Content stored? |
|-------|--------|-----------------|
| **Session memory items** | Agent **produced** outputs (`write_file`, images, …) | `locator.path` + summary + metadata — **not file bytes** |
| **Turn context** | User **selection** + tools that **read** paths | Live prompt: full paths; durable: **basenames only** |
| **File VectorStore** | Indexed files on disk (`search_facade`) | Chunked content — **not** wired to session locators |

#### What we persist today

| Source | Session item with `locator.path`? | Durable record |
|--------|-----------------------------------|----------------|
| `write_file`, `image_*`, `create_video`, screenshot tools | ✅ yes | Full path, mtime/size fingerprint, summary from write-time content scan |
| `file_read` | ❌ no | Basename only in `turn_pair.metadata.tool_calls` |
| `turn.selection` (Explorer) | ❌ no | Basenames in `turn_pair.metadata.context_files`; full paths only in **current** turn prompt |
| `file_glob` | ❌ (unless followed by write) | Pattern + count in turn_pair |
| MCP observations | ✅ yes (text) | Summary, no path |

Prompt inject for written artifacts (`agent_context.cpp`): pointer row — `path=…`,
`content_type=…`, summary, live **staleness** probe — model must `file_read` for body.

Usage bump: substring match on `locator.path` / `locator.url` in tool args + final text.

#### Gaps

1. **Basename-only refs** — cross-turn recall cannot disambiguate `report.md` or
   re-resolve a read/selected file after cwd changes. Blocks reliable BM25 on paths
   and any path-join to the file index.
2. **No workspace model** — unlike jcode-style workspace roots, PixlWiz has:
   - `Turn::selection` (ephemeral full paths)
   - `PathScopeFeature` base path per run
   - `search_facade` index roots chosen at CLI/build time
   
   Nothing ties session memory entries, selection, and file index to a **stable
   workspace id** (project folder, config dir, or named workspace). Recall cannot
   scope “resources in *this* project” vs global memory.

#### Target direction (for hybrid recall + resources)

- **`search_text`** for written items: path + basename + role + summary + json_keys (v1).
- **Full-path reference items** (`kind=reference` or extend turn_pair): promote
  `file_read` paths and `turn.selection` to durable locators — not basenames.
- **Workspace record** (new): `{ workspace_id, root_paths[], label }` on session or
  turn; memory entries tagged with `workspace_id`; recall candidate pool filtered by
  active workspace; optional link `locator.path` → file VectorStore under same roots.
- **Path join (v2):** memory pointer + `VectorStore` `filterPath` for semantic body
  search without duplicating write content in the memory index.

Do **not** duplicate full file bodies into session memory — keep pointers + summaries;
use file index for content semantics.

### Enable / disable — feature flags and wiring

Primary location: **`src/llm/agent_feature.cpp`** + **`src/llm/llm_constants.hpp`**
(alongside existing `StructuredMemoryFeature` bitmask and `MemoryFeature`).

#### `StructuredMemoryFeature` bits (shipped, default **on**)

In `llm_constants.hpp`:

| Bit | Purpose |
|-----|---------|
| `IndexMemoryEntries` | Background embed + upsert to agent-memory index on turn finish / recovery |
| `HybridRecallSearch` | Run BM25 (+ dense when model available) + RRF after turn; write pending |
| `InjectPendingRecall` | `on_prepare`: inject `## Recalled memory` block once from pending channel |

Existing master kill switch stays: `structured_memory_features == 0` disables
all of `MemoryFeature::on_finish` today — extend recall features to respect the same mask.

Env / CLI (reuse existing parser in `agent_memory_artifacts.cpp`):

```bash
# examples
PM_LLM_STRUCTURED_MEMORY_FEATURES=default,+HybridRecallSearch,+InjectPendingRecall
PM_LLM_STRUCTURED_MEMORY_FEATURES=default,-IndexMemoryEntries
```

#### `MemoryRecallFeature` (shipped)

Registered in `AgentFactory::default_agent()` **after** `feature_memory()`, **before**
`feature_system_prompt()` — Context(40) phase ordering is already asserted at startup.

| Hook | Action |
|------|--------|
| `on_prepare` | If `InjectPendingRecall`: `take_pending_recall(session_id)` → attach to `Turn` |
| `on_finish` | If `IndexMemoryEntries`: enqueue garden job; if `HybridRecallSearch`: enqueue recall job |

`SystemPromptFeature` renders the pending block (or `MemoryRecallFeature` sets a
field consumed by `agent_context.cpp` — same pattern as session items).

#### Profile matrix (recommended defaults)

User-facing **recall profiles** (settings `custom.agent_memory.recall_profile`,
env `PM_LLM_MEMORY_RECALL_PROFILE`):

| Profile | Flags | RAM | CPU | API cost |
|---------|-------|-----|-----|----------|
| **off** | clear Index / Hybrid / Inject recall bits | — | — | — |
| **hybrid** (ship default) | all three recall bits | embed model | embed + scan on finish | none |
| **lexical** | `HybridRecallSearch` + `InjectPendingRecall` (no index/dense) | none | BM25 on turn | none |
| **shadow** | Index + HybridSearch, **no** `InjectPendingRecall` | varies | score + log | none |

**Richness** (`custom.agent_memory.richness` / harness presets):

| Richness | top_k | inject_chars |
|----------|-------|--------------|
| concise | 3 | 800 |
| balanced (default) | 5 | 1500 |
| rich | 8 | 2500 |

Resolution: env (`PM_LLM_*`) > settings.json > code defaults.
Settings UI: Chat tab → Memory recall (profile + richness).
Compile-time strip: `FEATURE_COMMAND_LLAMA_INFERENCE=0` removes dense leg; lexical still works.

#### Budgets vs corpus size (adaptive shortlist)

Limits are **inject/latency budgets**, not “how many memories exist.” Collect all
eligible candidates; BM25 scores the full set; RRF takes an **adaptive shortlist**:

- floor ≈ `max(10 × top_k, 100)` (keep small corpora intact)
- grow with `√N`, ceiling `k_recall_shortlist_ceiling` (2000)
- override with `PM_LLM_RECALL_POOL` for harness presets (`0`/unset = adaptive)

Dense latency budget default `k_recall_dense_budget_ms=150` (settings /
`PM_LLM_RECALL_DENSE_BUDGET_MS`). Telemetry sets `dense_over_budget` when
exceeded; the **next** recall turn skips dense (lexical-only fallback).

```cpp
k_recall_top_k              = 5;      // inject top-K (Balanced)
k_recall_inject_chars       = 1500;   // UTF-8 budget for "## Recalled memory"
k_recall_candidate_pool     = 100;    // adaptive shortlist floor hint
k_recall_shortlist_ceiling  = 2000;
k_recall_dense_budget_ms    = 150;
k_recall_rrf_k              = 60;
```

Harness richness presets (env): `concise` / `balanced` / `rich` /
`legacy_pool50` in `tests/orchestrator/seed-memory-corpus.mjs`.

Offline bench:

```bash
npm run test:agent:memory:recall-bench          # ~1000 items (default)
npm run test:agent:memory:recall-bench:quick    # ~200 items
# or: --only memory-recall-bench --bench-size=10000
```

Writes `runs/<id>/recall-bench.json` (hit@K, pool policy, dense_ms, surfaced
requirements such as cross-session garden index gaps).

**Bug found while wiring bench:** `default_memory` items store facts in `value`
without `summary`. Search text was built before copying `value` → summary, so
BM25 could miss Buddy/codewords. Fixed in `push_candidate` / global index path.

#### What stays in `MemoryFeature` vs new feature

| Concern | Owner |
|---------|--------|
| Task-id scope, artifact persist, error reflexion | `MemoryFeature` (unchanged) |
| Index upsert, pending recall (disk + in-process), ranker | `MemoryRecallFeature` (`agent_memory_recall.cpp`) |
| Lazy embed singleton | `EmbedInferenceFeature` (unchanged; load only when needed) |
| Prompt assembly | `SystemPromptFeature` / `agent_context.cpp` |

Do **not** put BM25/RRF inside `query_store()` — keep it a store enumerator;
rankers live in `agent_memory_recall.cpp` or equivalent.

---

## Proposed architecture

```
┌─────────────────────────────────────────────────────────────────┐
│ Agent turn (agent_factory.cpp)                                  │
│   MemoryFeature.on_prepare  → take_pending_recall(session_id)   │
│   SystemPromptFeature       → inject pending block once         │
└───────────────┬─────────────────────────────────────────────────┘
                │ async (turn N-1)
┌───────────────▼─────────────────────────────────────────────────┐
│ MemoryRecallFeature / agent_memory_recall.cpp (v1 sync)             │
│   1. Collect candidates:                                          │
│        - current session memory_state items/instructions        │
│        - chat_sessions::query_store() memoryItems (scoped)        │
│        - optional: skill synthetic entries                        │
│   2. Normalize search_text per entry                              │
│   3. BM25 rank + dense rank (VectorStore or inline vectors)       │
│   4. RRF fuse → top pool                                        │
│   5. Optional judge / dynamic gate                              │
│   6. write pending_recall[session_id]                             │
└───────────────┬─────────────────────────────────────────────────┘
                │ read/write
┌───────────────▼─────────────────────────────────────────────────┐
│ Ground truth (unchanged)                                          │
│   agent_memory.cpp / session JSON / default_memory.doc            │
│   chat_session_store session + session-memory sidecars            │
│   journal → session_recover_from_journal on load                  │
└─────────────────────────────────────────────────────────────────┘
                │ index maintenance
┌───────────────▼─────────────────────────────────────────────────┐
│ Vector index (reuse)                                            │
│   VectorStore under e.g. {config}/agent-memory-index/             │
│   source=memory|skill|session_item                                │
│   OR embed vectors inline on MemoryEntry with lazy reindex        │
└─────────────────────────────────────────────────────────────────┘
```

### Recommended index location

Option A (align with `embeddings.md`):

```
{config_dir}/agent-memory-index/
  manifest.json      # embedding_model, dims, metric
  docs.jsonl         # MemoryEntry records + search_text
  vectors.bin        # or vectors.usearch when USearch lands
```

Option B: store small embedding arrays on each item inside `memory_state` and
use BM25 + brute-force cosine in-process (fine until ~10k entries).

Reuse **`media::llm::VectorStore`** for Option A — it already supports
`source: "memory"` and matches the embeddings doc layout.

### `chat_session_store.cpp` role (clarified)

| Keep | Add |
|------|-----|
| `query_store()` as cross-session **enumerator** | Normalized `search_text` field when indexing items |
| Secure session-memory sidecars | Incremental index update on `save_session_memory_document` |
| Legacy agent session JSON migration | Hook to enqueue recall refresh after write |

Do **not** put BM25/RRF inside `query_store()` — keep it a store API; rankers live in
`agent_memory_recall.cpp` (`MemoryRecallFeature`).

### `search_facade.cpp` relationship

Use for **workspace/file** semantic search (already production). Agent memory
recall should share:

- Same `EmbedInference` / GGUF model path
- Same normalization rules
- Possibly the same `VectorStore` directory layout

Do not route agent turn injection through CLI `search --mode semantic` — too
heavy and wrong latency profile. Call `VectorStore` or an in-process ranker directly.

---

## MemoryEntry sketch (versioned)

Align with jcode P1 TODO; store inside index sidecar or inline metadata:

```json
{
  "id": "mem_abc123",
  "scope": "session|project|global",
  "category": "fact|preference|artifact|skill",
  "source": "memory_write|journal_recovery|skill|turn_extract",
  "session_id": "…",
  "trust": "user|agent|recovery",
  "active": true,
  "search_text": "normalized lowercase tokens for BM25",
  "summary": "one line for injection",
  "embedding_model": "all-MiniLM-L6-v2",
  "embedding": null,
  "created_at": "…",
  "updated_at": "…",
  "reinforce_count": 1
}
```

Semantic dedup: new write with high cosine to existing → bump `reinforce_count`,
refresh timestamps, do not fork IDs.

---

## TODO (memory-focused)

### Done

- [x] Journal recovery consumer (`session_recover_from_journal`)
- [x] Turn markers in journal (`turn_started` / `turn_completed`)
- [x] Loopback + live harness (`test-agent-memory.mjs`) — **75 loopback assertions pass**;
      live `skill-resources` against roaming + VFS-staged assets
- [x] Cross-session pool (`query_store`)
- [x] Local `VectorStore` + search facade for files
- [x] Pointwise embed for skills/tools
- [x] Hybrid recall v1 baseline (BM25 + RRF + optional dense + pending inject)
- [x] Harness consumer cases: pet prefs, trip codeword, grocery artifact paths
- [x] Skills as typed resources (`agent_resources` / `info_lookup` on-demand bodies)

### P0 — durability

- [ ] Corrupt/torn JSONL journal salvage + backup
- [ ] Crash-recovery cases in memory harness (kill mid-turn → resume)
- [ ] `turn_id` on tool journal entries
- [ ] Incomplete-turn flag → concise planner recovery event

### P1 — referenced resources & workspaces

- [x] Persist **full paths** for `file_read` in turn_pair tool_calls; `context_file_paths` for selection
- [ ] Introduce **workspace** model: stable `workspace_id`, root path(s), label;
      attach to session/turn; tag memory entries; filter recall + `query_store` by
      active workspace
- [ ] Optional `CaptureReadArtifacts` feature bit — lightweight reference item on
      successful `file_read` (path + mtime fingerprint, no content body)
- [ ] Include path + basename in `search_text` normalization for written artifacts (v1: `locator.path`; turn_pair `context_file_paths` not yet)
- [ ] Path join (v2): memory `locator.path` → file VectorStore lookup under workspace roots

### P1 — hybrid recall (core of lines 169–171)

- [x] Define `MemoryEntry` + `search_text` normalization helper (`memory_recall_normalize_search_text`)
- [x] Index session items on write (MemoryFeature finish + `MemoryRecallFeature` + `VectorStore` upsert)
- [x] BM25 ranker over candidate pool (`agent_memory_recall.cpp`)
- [x] Dense rank via `VectorStore`; **`embedding_model` via store manifest**
- [x] RRF fuse (no hard cosine cutoff on fused path)
- [x] `pending_recall` per session; inject once in `on_prepare`
- [x] Disk sidecar `{session}.pending_recall.txt` (survives CLI multi-process turns)
- [x] `default_memory.doc` in candidate pool (`load_global_memory_document`)
- [x] Skip re-enqueue when turn consumed pending inject
- [x] Harness: `memory-recall-pending`, `memory-recall-consumer`, `memory-artifacts`
- [x] Recall enabled in `k_default_structured_memory_features`
- [x] Fast recent window: max 10 relevant items in planner + executor
- [x] Exclude self-check/security bookkeeping from recent-item prompt budget
- [x] Prompt guidance + regression: reuse exact artifact locator before `file_glob`
- [x] Staged system-prompt context test: folder, selection count/path, no unresolved variables
- [ ] Async/debounced recall worker (v1 runs sync in `on_finish`)
- [x] Overlap suppression vs recent Session Items (same-session IDs in newest-10)
- [ ] Dedup: injected IDs, content signatures, TTL (beyond overlap window)
- [x] Retrieval telemetry (candidates, pool, BM25/dense ms, overlap, injected bytes)
- [x] Adaptive RRF shortlist + dense latency budget (`MemoryRecallSettings`)
- [x] Offline recall benchmark (`memory-recall-bench` + `seed-memory-corpus.mjs`)
- [x] Settings profiles: `custom.agent_memory` + Chat-tab UI; env overrides
- [x] Dense over-budget → skip dense on next turn (lexical fallback)
- [ ] Cross-session dense garden reconcile (`query_store` donors not auto-indexed)
- [ ] Optional listwise judge behind feature flag
- [ ] Dynamic score-relative gate when judge off
- [x] Feature flags: `IndexMemoryEntries`, `HybridRecallSearch`, `InjectPendingRecall`
- [x] `VectorStore::upsert` / `remove_id` stable path keys (`memory://session/item`)

### P1 — embedding lifecycle

- [ ] Bounded embed LRU + load/unload metrics
- [ ] Idle unload (configurable seconds)
- [ ] Force unload API for settings/low-memory
- [ ] Clear index cache on model/backend change

### P1 — skills in same index

- [x] Register typed skill metadata in the shared index with stable branded URI
      identity (`source=resource`, `type=skill` locator metadata)
- [x] Share lexical/RRF ranking and materialize source bodies only through exact
      `info_lookup(kind=skill|resource)` lookup
- [x] Keep deterministic compact skill metadata/URI catalog in the prompt;
      never inject full bodies automatically
- [x] Preserve workspace-over-roaming, enabled/disabled/pinned/always, and
      OS/bin/env availability policy
- [x] Keep `request_tools` as the sole schema-grant path; declared skill tools
      confer no authority
- [x] Add explicitly configured CMS/VFS manifest providers, trust gates,
      credential-at-fetch behavior, token-redacted offline metadata cache, and
      branded `tanit://skill/vfs/<mount>/<id>` identities

### P2 — cross-cutting

- [x] Shadow retrieval mode (log only, no inject) — profile `shadow` / UI / `PM_LLM_MEMORY_RECALL_PROFILE`
- [x] Wire `memory_find` to hybrid ranker (tool API matches auto-recall)
- [x] Replaceable `tool_checkpoint` items for npm install/build/test outcomes;
      stable IDs supersede prior state and retain canonical payload locators
- [x] Optional `ToolCheckpointFeature`; disable it with
      `AgentFactory::default_agent({.tool_checkpoints_enabled = false})` or omit
      `feature_tool_checkpoints()` from a custom assembly
- [x] Cascading LRU eviction removes matching `memory://session/item` vector rows

---

## Test plan

| Test | Mode | Assert |
|------|------|--------|
| memory-tools | loopback + live | write/read/find (existing) |
| cross-session | loopback + live | session B finds session A token |
| journal recovery | loopback | kill after `write_file`, resume promotes item |
| hybrid recall | loopback | ✅ `memory-recall-pending`, `memory-recall-consumer` |
| recall bench | loopback | ✅ `memory-recall-bench` (+ `:quick` / `--bench-size`) hit@K + telemetry summary |
| pending inject | loopback | ✅ turn N scores, turn N+1 injects once, turn N+2 clean |
| recent artifact reuse | loopback + live case | ✅ exact locator in planner/executor; `case-dir` avoids re-glob |
| exact turn retrieval | loopback | ✅ long chat-only body, head/tail sentinel, cap/offset, wrong session, missing log, short reply |
| tool checkpoint continuity | live coding case | ✅ compact npm-test checkpoint injected; exact payload recovered by checkpoint item ID |
| no-replay exact export | live case | ✅ `case-exact-turn-export` materializes the prior body, then writes Markdown with both sentinels |
| staged prompt context | loopback | ✅ folder + selected-file count/path; no unresolved `${...}` |
| model swap | unit | old vectors reachable via BM25 only |
| reference paths | loopback | session B resolves full path from session A `file_read` / selection |
| workspace scope | loopback | recall in workspace A does not inject workspace B items |
| skill resource catalog | loopback | compact metadata + URI present; unique body sentinel absent |
| skill materialization | loopback | exact `info_lookup(kind=skill)` returns sanitized capped body + links |
| skill policy boundary | loopback | skill tool declarations ignored; `request_tools` still validates global policy |
| staged skill inventory | offline | unique canonical URIs; parseable metadata; safe/missing relative links reported |
| roaming VFS skill e2e | live | `service files pull --mount assets --path skills/*` into roaming; `--skills` forces discovery against user config; model loads meal-helper via `info_lookup` |

Run:

```bash
npm run test:agent:memory:harness
npm run test:agent:memory:harness -- --only memory-recall-pending,memory-recall-consumer,memory-artifacts
npm run test:agent:memory:recall-bench:quick
npm run test:agent:memory:harness:live
```

Recall suites use the default mask directly, so they fail if recall stops being
enabled by default. Profile tests: `PM_LLM_MEMORY_RECALL_PROFILE=off|lexical|hybrid|shadow`
and/or `PM_LLM_STRUCTURED_MEMORY_FEATURES=…`.

---

## Recommended implementation order

1. ~~**Normalize + index**~~ — shipped v1 (`MemoryRecallFeature` + stable `VectorStore` upsert).
2. ~~**BM25 + brute-force cosine + RRF**~~ — shipped v1 in `agent_memory_recall.cpp`.
3. ~~**Pending channel**~~ — shipped sync + disk sidecar; async remains optional.
4. ~~**Overlap suppression + telemetry**~~ — shipped: suppress same-session hits already in the recent-10 Session Items window; `MemoryUpdate.recall` + `memory_recall:` log line expose candidates, BM25/dense ms, overlap, injected bytes.
5. ~~**Offline benchmark**~~ — `memory-recall-bench` seeds cross-session corpus; presets compare adaptive vs `pool=50`; summary surfaces next requirements.
6. ~~**Settings profiles**~~ — `custom.agent_memory` + Chat-tab UI (profile/richness/dense budget); env still wins.
7. **Cross-session dense garden** — reconcile / index `query_store` donors so dense matches BM25 coverage (bench gap).
8. **Capture read references** — lightweight `file_read` locator items (no file body).
9. **Journal hardening** — salvage + crash-recovery cases.
10. **Lifecycle / judge / workspace scope** — defer until measured need.

Do **not** start with graph traversal, cross-encoder rerank, or semantic compaction.
jcode's live path shows hybrid retrieval + optional listwise judge is the useful baseline.

---

## Open questions

1. **Global vs session scope** — **v1:** `default_memory.doc` entries appear in the same BM25/RRF candidate pool (session id `global`); dense index upserts on `IndexMemoryEntries`. Same VectorStore dir vs inline vectors still open for scale.
2. **Index co-location** — per config-dir index vs per-session sidecar vectors?
3. **Secure storage** — embed vectors inside encrypted docs vs separate index dir with same protection policy?
4. **ReplayTranscript vs recall** — when hybrid recall is on, disable or cap transcript replay to avoid double context? Default today: `ReplayTranscript` off in `k_default_structured_memory_features`; recall inject uses separate `k_recall_inject_chars` budget.
5. **Workspace identity** — one workspace per config dir, per chat project folder, or user-named workspaces in settings?
6. **Reference item caps** — do `file_read` / selection refs share `k_session_items_max` with write artifacts or separate budget?

Track decisions here as implementation proceeds.

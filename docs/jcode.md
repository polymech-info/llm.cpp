# jcode concepts relevant to the PixlWiz agent

This document compares PixlWiz's agent runtime with the jcode reference tree.
It focuses on reusable concepts: memory, lazy model lifecycle, context
compaction, skills, tool catalogs, durability, and runtime modularity.

The jcode `Cargo.toml` is only an architectural index. Claims below were checked
against source under `ref/jcode`; workspace member names alone are not treated
as evidence of an implemented feature.

## Executive summary

PixlWiz already has several strong foundations:

- Progressive built-in and MCP tool grants through `request_tools`.
- Planner-seeded tool allowlists.
- Lazy MCP connections.
- Deterministic pre-API reduction that preserves the canonical transcript.
- Exact, bounded re-materialization of prior assistant replies from canonical
  per-run logs (`turn_pair` pointer → `memory_find(itemId, includeBody)`).
- Structured session artifacts, error reflexions, snapshots, security records,
  and a mid-turn tool journal.
- Bounded and optionally relevance-filtered `SKILL.md` injection.
- A phase-ordered prepare/finish feature pipeline.

jcode's most useful additions are:

1. Hybrid memory recall: embeddings + BM25 + reciprocal-rank fusion (RRF).
2. One listwise LLM relevance judge after cheap broad retrieval.
3. A pending-memory channel that injects asynchronous recall once, with dedup.
4. Background conversation compaction with reactive, proactive, and semantic
   policies.
5. A complete lazy embedding lifecycle: load, bounded cache, telemetry, idle
   unload, force unload, and retained-heap release.
6. Skills represented as retrievable capabilities rather than only prompt text.
7. Fingerprinted MCP schema caching with live reconciliation.
8. Append-only session recovery with torn/corrupt journal salvage.
9. Composition-root registration to avoid upward dependencies.

The most urgent PixlWiz gap was journal recovery: tool outcomes were journaled
mid-turn but never consumed on resume. A v1 recovery pass now runs inside
`session_load()` and promotes successful path outputs into session memory.

## Architecture comparison

### PixlWiz today

`src/llm/agent_factory.cpp` is the synchronous turn engine. Its main stages are:

1. Run ordered `IAgentFeature::on_prepare` hooks.
2. Assemble the prompt and initial message history.
3. Configure the selected provider.
4. Iterate LLM calls and tool dispatch.
5. Run `IAgentFeature::on_finish` on exit.

The default feature order is:

1. `EmbedInferenceFeature` — scope/lazy embedding setup.
2. `PathScopeFeature` — path base and blocklist.
3. `PlannerFeature` — intent and initial tool allowlist.
4. `ToolsFeature` — built-in schemas.
5. `McpFeature` — MCP side catalog and bridge.
6. `MemoryFeature` — session scope and end-of-turn persistence.
7. `SnapshotFeature` — named snapshot load/save.
8. `SkillsFeature` — skill discovery and prompt context.
9. `SystemPromptFeature` — final prompt assembly.
10. `SelfCheckFeature` — post-turn structural quality signals.

The feature contract only has prepare and finish hooks. Runtime policies such as
compression, security interception, progressive grants, parallel dispatch,
journaling, provider routing, and termination remain embedded in
`Agent::run`.

Evidence:

- `src/llm/agent_factory.cpp:687-1701`
- `src/llm/agent_factory.cpp:1704-1757`
- `src/llm/agent_feature.hpp:50-122`
- `src/llm/agent_feature.cpp`

### jcode shape

jcode splits the stack into smaller crates:

- `jcode-base`: memory, embeddings, compaction, skills, sessions, MCP manager.
- `jcode-app-core`: tool runtime, agent/server loop, dynamic registration.
- `jcode-compaction-core`: pure compaction helpers and policies.
- `jcode-embedding`: local ONNX embedding and reranking primitives.
- Provider runtime crates: provider-specific registration and execution.
- Type crates: memory, tools, messages, sessions, protocols, background work.

The important lesson is not “use Rust crates.” It is to define stable subsystem
boundaries so context policy, resource lifecycle, persistence, retrieval, and
tool dispatch can evolve independently.

Evidence:

- `ref/jcode/Cargo.toml:8-91`
- `ref/jcode/crates/jcode-base/src/lib.rs`
- `ref/jcode/crates/jcode-app-core/src/tool/mod.rs`
- `ref/jcode/src/cli/startup.rs`

## Memory and recall

### PixlWiz memory implementation

PixlWiz memory is session- and artifact-oriented:

- A thread-local task/session ID scopes memory tools.
- Session JSON stores structured state and a recent-event ring.
- Successful turns persist artifacts and turn pairs.
- Failed/cancelled turns can persist error reflexions.
- Items and instructions are injected into planner and system prompt context.
- Named snapshots preserve memory state and recent events.
- Tool outcomes are journaled immediately during a turn.

This is strong durability-oriented memory, but it is not yet a general semantic
recall engine. Selection is primarily based on structured caps and existing
session context rather than broad cross-session candidate retrieval.

Evidence:

- `src/llm/agent_feature.cpp:698-825`
- `src/llm/agent_memory.cpp:593-638`
- `src/llm/agent_memory.cpp:641-769`
- `src/llm/agent_memory_artifacts.cpp`
- `src/llm/agent_context.cpp:465-509`
- `src/llm/llm_constants.hpp:127-175`

### jcode memory implementation

jcode stores project and global memories and retrieves them with a two-stage
pipeline:

1. Generate a broad candidate pool.
2. Rank independently using:
   - Dense embedding cosine similarity.
   - Sparse BM25 matching over normalized search text.
3. Fuse both rankings with RRF.
4. Use either:
   - One listwise LLM judge for precision, or
   - A dynamic score-relative gate when no judge is used.
5. Place selected memories in a pending per-session channel.
6. Inject them once on a later turn with TTL, overlap, and ID suppression.

Important details:

- It does not use a hard cosine threshold on the live hybrid path because that
  damaged recall in benchmarks.
- Embeddings are tagged with a stable model ID.
- Dense comparisons only happen within the same vector space.
- Memories from a different embedding backend remain reachable through BM25.
- Storage-level semantic duplicates reinforce the existing memory rather than
  creating another entry.
- Skills can register synthetic memory entries without `memory` depending on
  the skill layer.

Evidence:

- `ref/jcode/crates/jcode-base/src/memory.rs:64-96`
- `ref/jcode/crates/jcode-base/src/memory.rs:392-460`
- `ref/jcode/crates/jcode-base/src/memory.rs:633-728`
- `ref/jcode/crates/jcode-base/src/memory/pending.rs`
- `ref/jcode/crates/jcode-base/src/memory_agent.rs`
- `ref/jcode/crates/jcode-base/src/memory_rerank.rs`
- `ref/jcode/crates/jcode-base/src/embedding_backend.rs:11-23`

### Memory lessons for PixlWiz

PixlWiz implementation roadmap: [agent-memory-next.md](./agent-memory-next.md).

- Keep current structured session artifacts as ground truth.
- Add a retrieval layer rather than replacing session memory.
- Start with lexical + embedding retrieval and RRF.
- Add a single listwise judge only after measuring the cheap retriever.
- Persist `embedding_model` with every vector.
- Use a pending-memory handoff so background recall never blocks the active
  provider call.
- Track injected IDs and content signatures to prevent repeated context.
- Treat skills as retrieval candidates in the same index.

## Lazy evaluation and model lifecycle

### PixlWiz lazy-runtime implementation

PixlWiz lazily initializes the embedding backend through `std::call_once`.
Repeated turns pay effectively no bootstrap overhead, and missing models fall
back to fuzzy matching.

The local llama client also loads a model on first use, but it has one cached
model slot. A request for a different model while the slot has active calls can
fail as busy.

Current gaps:

- No embedding idle-unload path was found.
- No explicit force-unload API was found.
- No bounded lifecycle telemetry comparable to load count, unload count, idle
  time, cache hits, and average embedding latency.
- A process-lifetime singleton can hold a heavyweight model indefinitely.
- One local text-model slot limits concurrent multi-model use.

Evidence:

- `src/llm/agent_feature.cpp:1655-1681`
- `src/lib/llm/llama/EmbedInference.cpp`
- `src/llm/llama_agent_client.cpp:71-107`

### jcode lazy-runtime implementation

jcode uses a process-wide lazy embedder with:

- Load on first use.
- `Arc` ownership so an idle unload cannot invalidate in-flight users.
- A bounded 128-entry embedding-result LRU.
- Last-used and loaded-at timestamps.
- Load, unload, call, failure, latency, cache-hit, and memory estimates.
- Periodic idle checks from the server runtime.
- Idle unload and explicit force unload.
- LRU clearing and retained-heap release after unload.
- A feature-compatible stub when local embeddings are not compiled.
- A pluggable backend trait for local and OpenAI-compatible embeddings.

Evidence:

- `ref/jcode/crates/jcode-base/src/embedding.rs:17-299`
- `ref/jcode/crates/jcode-base/src/embedding_backend.rs:29-336`
- `ref/jcode/crates/jcode-base/src/embedding_stub.rs`
- `ref/jcode/crates/jcode-app-core/src/server.rs:1439-1463`

### Model-lifecycle lessons for PixlWiz

Treat lazy initialization as one part of a resource lifecycle:

1. Acquire/load.
2. Share safely with in-flight requests.
3. Cache bounded results.
4. Record last use and runtime metrics.
5. Unload after configurable idle time.
6. Release retained allocator memory where possible.
7. Allow explicit unload for settings/debug/low-memory flows.

## Context compression and compaction

### PixlWiz compression implementation

PixlWiz's pre-API reducer creates a temporary wire copy before each provider
call:

- Canonical messages and transcript retain full fidelity.
- System messages are protected.
- The latest user request is protected.
- Policies can reduce only tool results or all eligible historical messages.
- Structural JSON/code and deterministic text reduction paths are available.
- Failed or non-shrinking reductions keep the original.

This is a good stateless safety filter, especially for large tool envelopes.

Current gaps:

- Reduction is off by default.
- Only the provider wire copy shrinks; canonical in-turn messages continue to
  accumulate full payloads.
- There is no durable summary or compacted-prefix cursor.
- No asynchronous compaction starts before the hard limit.
- Model/embedding-backed compressor stages are not connected at this seam.
- System prompt and tool-schema budgets are outside this reducer.

Evidence:

- `src/llm/agent_factory.cpp:377-451`
- `src/llm/agent_factory.cpp:887-908`
- `src/llm/reduce/reduce.hpp`
- `src/llm/agent.hpp:216-232`

### jcode compaction implementation

jcode has a `CompactionManager` that does not own conversation messages. It
tracks:

- `compacted_count`: the leading message prefix covered by a summary.
- The current durable summary.
- A rolling active-character estimate.
- Provider-observed token usage.
- A background summarization task.
- Token growth history.
- Semantic embedding history and cache.

Its modes are:

- Reactive: trigger around a fixed context threshold.
- Proactive: predict growth with an EWMA and compact earlier.
- Semantic: use topic/relevance signals to choose a better cutoff.

Additional safeguards:

- Normal threshold around 80%.
- Hard/critical behavior around 95%.
- Recent turns remain verbatim.
- Safe cutoffs avoid splitting tool-use/tool-result relationships.
- Images and large tool results have separate emergency handling.
- HTTP 413 payload recovery is separate from normal token-budget compaction.
- Compaction state is persisted in session metadata.

Evidence:

- `ref/jcode/crates/jcode-base/src/compaction.rs:1-41`
- `ref/jcode/crates/jcode-base/src/compaction.rs:128-205`
- `ref/jcode/crates/jcode-base/src/compaction.rs:603-675`
- `ref/jcode/crates/jcode-base/src/compaction.rs:927-1027`
- `ref/jcode/crates/jcode-compaction-core/src/lib.rs`

### Compaction lessons for PixlWiz

Use two complementary layers:

1. **Wire reduction** for deterministic local shrinking of oversized messages.
2. **Conversation compaction** for durable, coherent long-session management.

The compactor should not own the canonical message store. It should return a
summary and covered-prefix position that the message assembler uses when
building provider payloads.

### Canonical-log progressive disclosure

PixlWiz now applies the same authority boundary to cross-turn response continuity:

- The per-run session log owns the unabridged `result.final_text`.
- A `turn_pair` stores only its capped preview, exact item ID, assistant size, and
  trusted secure-log/file locator.
- `Session Items` advertises `body_available=true` without injecting the body.
- `memory_find(itemId="turn_…", includeBody=true)` resolves that exact locator and
  returns a bounded UTF-8 slice with `next_offset`.
- Terse write/export follow-ups keep `memory_find` and `write_file` in the planner
  schema set, so the agent retrieves before writing.

This aligns with jcode's canonical-transcript principle: reduction and prompt
assembly are derived views, not the only surviving copy. PixlWiz uses its
completed-run log as the durable authority because its normal structured memory
intentionally does not replay full transcripts. It does not guess which replies
are “noteworthy,” auto-promote prose to artifacts, or enable broad
`ReplayTranscript`.

Coverage:

- `test-agent-memory.mjs` / `exact-turn-retrieval`: long head/tail sentinel,
  summary cap, byte cap/offset, wrong session, missing log, and short response.
- `test-agent.mjs` / `case-exact-turn-export`: no-replay “write as md file”
  materializes the exact prior body and preserves both sentinels.

## Skills

### PixlWiz skills implementation

PixlWiz discovers roaming and workspace `SKILL.md` files plus explicitly typed
`skill.json` / `resource.json` manifests. It adapts them to generic
`RetrievalResource` records with stable branding-derived URIs, preserving
enabled, disabled, pinned, always, workspace precedence, and OS/bin/env
requirements.

`SkillsFeature` injects deterministic compact metadata for **pinned/always**
skills only (stable prompt prefix: `name: description`). Non-pinned skills are
not mentioned in the prompt; discover them only via
`info_lookup(kind=skill|resource)` (shared BM25/dense/RRF). An exact URI or
qualified name materializes the sanitized source body under a 24 KiB default /
96 KiB hard cap. Relative package links and branded cross-skill links remain
explicit on-demand traversals.

Current gaps:

- VFS provider refresh/latency telemetry and signed-manifest verification need
  broader production measurement; unverified roots remain unavailable.
- Direct slash invocation is not part of this agent path.
- The index stores compact resource metadata; source files remain authoritative
  and bodies are intentionally not persisted in prompt/session memory.

Evidence:

- `src/llm/agent_feature.cpp:828-1009`
- `src/llm/agent_skills.cpp`
- `src/llm/agent_skills.hpp`
- `src/llm/agent_resources.cpp`
- `src/llm/agent_retrieval.cpp`

### jcode skills implementation

jcode:

- Loads global skills once into a shared `Arc<RwLock<SkillRegistry>>`.
- Keeps project-local skills as a per-workspace overlay.
- Reloads project overlays from disk so changes do not require daemon restart.
- Imports compatible Claude Code and Codex skill directories.
- Supports direct `/skill-name prompt...` invocation.
- Sorts skill listings deterministically to protect provider prefix/KV caches.
- Registers each skill as a synthetic memory entry.
- Retrieves relevant skills through hybrid memory search.
- Supports an agent-facing skill-management tool.

Evidence:

- `ref/jcode/crates/jcode-base/src/skill.rs:14-91`
- `ref/jcode/crates/jcode-base/src/skill.rs:221-280`
- `ref/jcode/crates/jcode-base/src/skill.rs:478-542`
- `ref/jcode/crates/jcode-base/src/skill.rs:880-921`
- `ref/jcode/crates/jcode-base/src/skill/invocation.rs`
- `ref/jcode/crates/jcode-app-core/src/tool/skill.rs`

### Skills lessons for PixlWiz

- Keep only deterministic skill metadata in the stable prompt prefix.
- Index complete skill content as retrievable entries.
- Materialize a full body only when retrieved, pinned, or explicitly invoked.
- Separate global registry state from workspace overlays.
- Sort every prompt-facing catalog deterministically.
- Ignore skill `allowed-tools` / `declared_tools` for grants; `request_tools` owns schemas.

## Tool catalogs and MCP

### PixlWiz tool-catalog implementation

PixlWiz has a strong three-tier tool model:

1. The planner seeds a compact built-in allowlist.
2. The initial provider catalog contains that subset plus `request_tools`.
3. The model can request additional built-in or MCP schemas during the turn.

MCP schemas live in a side catalog and are copied into the live provider tool
array only when granted. MCP servers connect lazily on `tools/call`.

Benefits:

- Remote schemas do not consume tokens on every turn.
- Disabled servers/tools are removed before they can be granted.
- The live catalog remains explicit and auditable.
- Missing-tool errors teach the model to call `request_tools`.

Evidence:

- `src/llm/agent_factory.cpp:57-298`
- `src/llm/agent_factory.cpp:710-715`
- `src/llm/agent_factory.cpp:1433-1470`
- `src/llm/agent_feature.cpp:544-692`
- `src/llm/tools/mcp/McpBridge.cpp`
- `src/llm/path_tool_catalog.cpp`

### jcode tool-catalog implementation

jcode adds a useful cold-start optimization:

- Cache MCP tool schemas on disk.
- Fingerprint normalized server configuration: command, args, environment, and
  sharing mode.
- Advertise cached schemas immediately when the fingerprint matches.
- Connect to the real server lazily/in the background.
- Wait for the live connection only when a tool is called.
- Reconcile live `tools/list` results back into the cache.
- Treat cached schemas as hints, never authority.

It also keeps built-in tool definitions deterministic and can dynamically
register runtime tools.

Evidence:

- `ref/jcode/crates/jcode-base/src/mcp/schema_cache.rs`
- `ref/jcode/crates/jcode-base/src/mcp/manager.rs`
- `ref/jcode/crates/jcode-app-core/src/tool/mod.rs`
- `ref/jcode/crates/jcode-app-core/src/tool/discover.rs`

### Tool-catalog lessons for PixlWiz

Do not replace progressive grants. Add a warm side-catalog cache:

- Key cached schemas by server-config fingerprint.
- Validate names against current policy before exposing them.
- Reconcile drift after a live connection.
- Preserve deterministic ordering for provider prompt-cache stability.
- Add fuzzy suggestions/aliases for hallucinated tool names where safe.

## Session durability and recovery

### PixlWiz durability implementation

PixlWiz writes a bounded journal entry immediately after each tool result. Each
entry can include:

- Tool name.
- Tool-call ID.
- Duration.
- Success state.
- Output paths.
- Error text.
- Timestamp.

The regular filesystem path is append-only JSONL; secure storage uses a bounded
document. Named session snapshots and end-of-turn session state are separate.

Critical gap:

`session_journal_read` exists, but no call site under `src/` consumes it. The
journal therefore cannot currently:

- Promote successfully produced output files after a crash.
- Reconstruct completed tool work.
- Prevent re-executing already successful side effects.
- Explain an interrupted turn to the next planner.
- Restore in-flight provider or message state.

Evidence:

- `src/llm/agent_factory.cpp:1640-1673`
- `src/llm/agent_memory.cpp:641-769`
- `src/llm/agent_memory.hpp:153-155`

### jcode recovery implementation

jcode combines:

- Full session snapshots.
- Append-only JSONL journal entries.
- Persisted compaction metadata.
- Persisted memory injection state.
- Salvage of torn or glued journal lines.
- Backup of corrupt data.
- Forced checkpoint after recovery.
- Crash detection.
- Explicit recovery sessions cloned from interrupted state.

Evidence:

- `ref/jcode/crates/jcode-base/src/session/journal.rs`
- `ref/jcode/crates/jcode-base/src/session/persistence.rs:26-200`
- `ref/jcode/crates/jcode-base/src/session/crash.rs`

### Recovery lessons for PixlWiz

The existing journal should become an idempotent recovery log:

1. Give each agent run a stable run/turn ID.
2. Mark turn start and clean completion.
3. On load, detect a started but incomplete turn.
4. Parse journal entries defensively.
5. Validate output paths and side-effect evidence.
6. Promote valid outputs into structured memory.
7. Add a concise recovery event for the planner.
8. Track recovered tool-call IDs to prevent duplicate promotion.
9. Rotate/checkpoint the journal only after recovery state is committed.

## Runtime modularity

### PixlWiz issue

`agent_factory.cpp` combines:

- Progressive tool catalog policy.
- Windows process/DLL path setup.
- Context reduction.
- Security gateway and consent.
- Provider selection and streaming.
- Serial and parallel tool dispatch.
- UI progress hooks.
- Journaling.
- Usage telemetry.
- Loop termination.
- Default feature composition.

The feature pipeline cannot intercept a provider round or tool call. This causes
duplication between serial and parallel paths and makes isolated testing harder.

Notable risks:

- Security behavior is duplicated across serial and parallel dispatch.
- UI override merging is duplicated.
- `on_finish` calls are repeated across many exits.
- Feature ordering only warns; it does not sort or fail.
- Compile flags fragment a large translation unit.
- Recovery must cross the factory/memory boundary.

### jcode lesson

jcode uses composition-root registration for providers, synthetic memory
sources, reload hooks, and other cross-layer integrations. Lower layers define
registrar interfaces; startup code connects them.

For PixlWiz, useful seams would be:

- `ConversationStore`: canonical messages, summary, compacted prefix.
- `ContextPolicy`: token accounting, wire reduction, compaction decisions.
- `ToolRegistry`: built-ins, discovery, side catalogs, grants, snapshots.
- `ToolDispatcher`: authorization, execution, progress, journaling.
- `ProviderSession`: provider-specific state, streaming, response IDs.
- `ResourceManager`: local model acquisition, concurrency, idle unload.
- `RecoveryManager`: journal replay, output validation, idempotent promotion.

The current feature mechanism can remain for turn composition, but it needs
round/tool middleware if runtime policies are expected to be pluggable.

## Implemented versus inferred in jcode

Verified as implemented:

- Hybrid dense + BM25 + RRF memory retrieval.
- Listwise LLM reranking and a no-LLM dynamic gate.
- Pending-memory injection with dedup.
- Local ONNX embedding, LRU cache, telemetry, and idle unload.
- Optional OpenAI-compatible embedding backend.
- Reactive, proactive, and semantic compaction.
- Background summarization and hard-limit fallback.
- `SKILL.md` registry, reload, overlays, invocation, and memory indexing.
- MCP manager, lazy connections, dynamic registration, and schema cache.
- Append-only session journal, corruption salvage, and crash recovery.
- Composition-root registration for providers and synthetic memory.

Present but not on the primary live path:

- Memory graph cascade retrieval exists but live auto-recall uses hybrid
  retrieval plus reranking.
- A cross-encoder reranker exists in the embedding crate, but production memory
  reranking uses the listwise LLM path.

Treat older plan documents cautiously when they conflict with runtime code.

## PixlWiz TODO list

### P0 — close correctness and durability gaps

- [x] Add a recovery consumer for `session_journal_read` (`session_recover_from_journal`).
- [x] Add stable `run_id` and turn markers (`turn_started` / `turn_completed`) in the journal.
- [ ] Persist explicit `turn_id` on each tool journal entry (optional follow-up).
- [ ] Detect incomplete turns during session load (partial: `journal_has_incomplete_turn` flags recovery events).
- [x] Validate recovered `output_paths` before promotion (file must exist on disk).
- [x] Promote valid recovered outputs into structured session items/artifacts.
- [x] Record recovered tool-call IDs under `memory_state.journal_recovery` for idempotency.
- [x] Inject a concise `journal_recovery` event when promotion occurs.
- [ ] Add corrupt/torn JSONL salvage and preserve a backup before rewrite.
- [ ] Add crash-recovery tests: completed output, missing output, duplicate replay, torn final line, malformed middle line, and secure-storage path.
- [ ] Change feature phase-order violation from warning to startup failure or deterministic sorting with explicit same-phase order.

### P1 — hybrid recall

- [ ] Define a versioned `MemoryEntry` with stable ID, scope, category, trust,
      source, timestamps, active state, search text, optional embedding, and
      `embedding_model`.
- [ ] Preserve existing session items/artifacts as the authoritative store.
- [ ] Add normalized lexical search text for memories and skills.
- [ ] Implement BM25 candidate ranking.
- [ ] Implement dense cosine ranking with model-ID gating.
- [ ] Fuse lexical and dense ranks with RRF.
- [ ] Avoid a hard cosine cutoff on the fused live path.
- [ ] Add storage-level semantic dedup that reinforces an existing memory.
- [ ] Add project and global memory scopes if cross-session recall is desired.
- [ ] Add retrieval telemetry: candidates, lexical/dense overlap, selected IDs,
      latency, and injected bytes.
- [ ] Build an offline recall benchmark before tuning thresholds.
- [ ] Add one listwise relevance-judge call after cheap retrieval.
- [ ] Add a circuit breaker and deterministic no-judge fallback.
- [ ] Add a per-session pending-memory channel.
- [ ] Add TTL, content-signature, overlap, and injected-ID suppression.
- [ ] Keep retrieval/background failures non-fatal to the main turn.

### P1 — durable context compaction

- [ ] Introduce a non-owning `CompactionManager`.
- [ ] Track `compacted_count` and an active summary.
- [ ] Use provider-reported input tokens when available.
- [ ] Keep the existing deterministic wire reducer as a separate layer.
- [ ] Start background summarization before the hard context limit.
- [ ] Preserve recent turns verbatim.
- [ ] Prevent cutoffs between a tool request and its results.
- [ ] Add reactive mode first.
- [ ] Add proactive EWMA growth prediction after telemetry exists.
- [ ] Add semantic cutoff selection only after embedding recall is stable.
- [ ] Persist compaction state in session storage.
- [ ] Separate token-budget compaction from HTTP 413/image-payload recovery.
- [ ] Add emergency tool-result and image clamps.
- [ ] Add tests for cancellation, pending-summary timeout, provider failure,
      resume, tool-pair boundaries, and summary non-growth.

### P1 — embedding resource lifecycle

- [ ] Wrap the embedding model in a shareable in-flight-safe handle.
- [ ] Add a bounded embedding-result LRU.
- [ ] Track load count, unload count, calls, failures, latency, cache hits,
      loaded time, idle time, and estimated bytes.
- [ ] Add configurable idle unload.
- [ ] Add explicit force unload.
- [ ] Clear related caches on model/backend change.
- [ ] Release retained allocator memory after unload where supported.
- [ ] Expose lifecycle state to diagnostics/settings.
- [ ] Add concurrency tests for unload during active embedding calls.
- [ ] Replace or extend the single local text-model slot with a resource manager
      if concurrent multi-model local sessions are required.

### P1 — skills as retrievable capabilities

- [x] Split stable skill metadata from full skill bodies.
- [x] Sort all prompt-facing skill metadata deterministically.
- [x] Add global registry plus per-workspace overlay semantics.
- [x] Convert skills to typed retrieval entries.
- [x] Retrieve skill metadata with the shared lexical+dense/RRF pipeline.
- [x] Materialize a body only through an exact on-demand lookup.
- [ ] Support direct skill invocation with a trailing user prompt.
- [x] Parse declared/allowed tool hints while enforcing grants independently
      through `request_tools`.
- [x] Keep per-resource and hard total byte caps as final safeguards.
- [ ] Add reload/cache-invalidation telemetry.

### P2 — MCP and tool catalog improvements

- [ ] Persist MCP schemas in a versioned cache.
- [ ] Fingerprint normalized command, args, environment, and sharing mode.
- [ ] Ignore cached schemas on fingerprint mismatch.
- [ ] Advertise matching cached schemas through the side catalog without
      eagerly connecting.
- [ ] Reconcile live `tools/list` results after connection.
- [ ] Keep progressive `request_tools` grants as the provider-facing mechanism.
- [ ] Sort tool definitions deterministically.
- [ ] Add safe alias/fuzzy suggestions for unknown tool names.
- [ ] Record catalog cache hits, misses, drift, and provider prompt-cache
      invalidations.

### P2 — runtime extraction

- [ ] Add a single scoped finalization guard so `on_finish` runs once on every
      exit without repeated code.
- [ ] Extract provider routing into `ProviderSession`.
- [ ] Extract context reduction/compaction into `ContextPolicy`.
- [ ] Extract progressive catalog behavior into `ToolRegistry`.
- [ ] Extract serial/parallel execution into one `ToolDispatcher` pipeline.
- [ ] Represent authorization, argument adaptation, execution, telemetry,
      journaling, and result append as ordered middleware stages.
- [ ] Keep consent prompts serialized before parallel execution.
- [ ] Unify serial and parallel UI override logic.
- [ ] Unify serial and parallel security-denial handling.
- [ ] Add round-level and tool-level extension hooks.
- [ ] Move Windows process path setup out of the agent loop translation unit.
- [ ] Unit-test extracted policies without a real provider.

### P2 — measurement and rollout

- [ ] Add per-round context composition telemetry by category: system, skills,
      memory, tools, messages, and tool results.
- [ ] Measure prompt-cache hits/misses after deterministic catalog ordering.
- [ ] Add feature flags for recall, compaction, recovery, and schema caching.
- [ ] Run shadow retrieval before injecting memories.
- [ ] Record judge acceptance/rejection and user correction signals.
- [ ] Define latency and token budgets for every background feature.
- [ ] Add migration/version fields to memory, journal, schema-cache, and
      compaction state.

## Recommended implementation order

1. Make the existing journal recoverable and idempotent.
2. Add instrumentation for context composition and embedding lifecycle.
3. Implement lexical BM25 retrieval and a benchmark harness.
4. Add model-tagged embeddings and RRF fusion.
5. Add pending-memory injection and dedup.
6. Add one listwise judge behind a feature flag.
7. Add reactive background compaction while keeping wire reduction.
8. Add embedding idle unload.
9. Index skills as retrievable entries.
10. Add fingerprinted MCP schema caching.
11. Extract the runtime loop into provider/context/tool/recovery boundaries.

Do not start with graph traversal, cross-encoder reranking, or semantic
compaction. jcode contains those ideas, but its live path demonstrates that
hybrid retrieval, listwise judging, and simpler compaction policies provide the
more useful baseline.

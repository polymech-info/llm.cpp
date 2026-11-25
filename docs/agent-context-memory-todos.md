# **Agent context & memory — remaining implementation TODOs**

The adjustments look sensible. The new field limits, scheduled-event caps, tool availability checks, safer continuation instructions and adaptive recall shortlist address several of the previous findings.

However, the documented Android continuity problem is still largely independent of hybrid recall quality.

**The agent should not need a memory search to understand the immediately preceding conversation.** Structured memory should supplement conversation history, not substitute for it.

I would focus on the following changes before extending the memory architecture further.

## **1. Immediate correctness fixes**

| ID  | Location                     | TODO                                                                                                                                              |
| --- | ---------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| C01 | `agent_feature.cpp`          | Pass explicit recall capabilities to the planner's `append_recent_turns()`. Remove the default argument that implicitly advertises `memory_find`. |
| C02 | `agent_context.cpp`          | Use the same retrieval capability checks for recent turns, session items and recalled memory.                                                     |
| C03 | `agent_memory_artifacts.cpp` | Remove synthesized hard rules that automatically interpret terse follow-ups as references to the newest artifact.                                 |
| C04 | `MemoryTools.cpp`            | Make omitted `task_id` resolve to the current authenticated session, when one exists.                                                             |
| C05 | `MemoryTools.cpp`            | Require explicit scope for global memory writes. Do not silently fall back to `default_memory.doc`.                                               |
| C06 | `MemoryTools.cpp`            | Correct success messages claiming that written memory will automatically appear in the next prompt.                                               |
| C07 | `system-prompt.android.md`   | Correct the descriptions of `memory_write` and `memory_find` to match their actual scopes and retrieval modes.                                    |
| C08 | `agent_context.cpp`          | Add an explicit feature flag for `Stored Memory`, which currently bypasses the structured-memory injection flags.                                 |
| C09 | `pm_android_agent.cpp`       | Verify that `req.session_id` remains stable across sends, retries, activity recreation and voice delegation.                                      |
| C10 | `agent_factory.cpp`          | Ensure the master memory-disable mask also suppresses recall injection and indexing where intended.                                               |
| C11 | All prompt templates         | Search for remaining references encouraging the storage of secrets.                                                                               |
| C12 | Memory ingestion             | Validate JSON field types before persistence, not only during rendering.                                                                          |

C01–C09 follow directly from the problems documented in `agent-context.md`, including conflicting synthesized rules and incorrect global-memory write semantics.&#x20;

Pasted markdown (2)(9).md

Pasted markdown (2)(9).md



### **Memory tool contract**

I would settle on this behavior:

| Invocation                                                     | Scope                           |
| -------------------------------------------------------------- | ------------------------------- |
| `memory_write(key, value)`                                     | Current session                 |
| `memory_write(scope="global", ...)`                            | Persistent user memory          |
| `memory_read()`                                                | Current session                 |
| `memory_read(scope="global")`                                  | Persistent user memory          |
| `memory_find(query=...)`                                       | Current session by default      |
| `memory_find(searchPastChats=true, ...)`                       | Authorized cross-session search |
| `memory_find(itemId=..., includeBody=true)`                    | Exact prior reply               |
| `memory_find(itemId=..., toolCallId=..., includePayload=true)` | Exact canonical tool result     |

The host should supply the actual session identity. The model should not be responsible for generating or selecting internal session IDs.

## **2. Same-session continuity — highest UX impact**

Currently, desktop and Android send essentially:

```
system:
    persona
    executor rules
    recent turn summaries
    session items
    recalled memory

user:
    current request
```

The four recent turn pairs are each restricted to approximately 192 bytes per speaker, with a 520-byte record cap. That is insufficient for many ordinary follow-ups.&#x20;

Pasted markdown (2)(9).md

Pasted markdown (2)(9).md



### **Obvious change: reuse the existing message pipeline**

**Current**

SYSTEM

Policy, memory, flattened history, artifacts

USER

Now make it shorter.

Previous reply must be reconstructed or retrieved.

**Proposed**

SYSTEM

Policy, runtime, memory references

USER

Write a description.

ASSISTANT

Previous description...

USER

Now make it shorter.

Natural continuation using original message roles.

No new memory subsystem is needed. The existing `seed_chat_messages` mechanism is a potential integration point, subject to checking its current semantics.

| ID  | TODO                                                                                                           |
| --- | -------------------------------------------------------------------------------------------------------------- |
| H01 | Hydrate the last 2–4 completed conversation pairs from the authoritative session transcript.                   |
| H02 | Preserve native user/assistant message roles.                                                                  |
| H03 | Apply a shared history token budget, configurable per model.                                                   |
| H04 | Preserve the immediately preceding reply in full when it fits.                                                 |
| H05 | For oversized replies, retain a bounded excerpt and an exact recall reference.                                 |
| H06 | Exclude historic tool messages unless their complete tool-call/result grouping can be reconstructed correctly. |
| H07 | Do not inject the same turn pairs again through `append_recent_turns()`.                                       |
| H08 | Keep a smaller historical tail for the planner, including the latest subject and artifact references.          |
| H09 | Ensure the live user prompt is not duplicated in hydrated history.                                             |
| H10 | Keep `ReplayTranscript` disabled; native-role history is not legacy transcript dumping.                        |
| H11 | Add an `InjectRecentMessages` feature flag independently of `InjectItems`.                                     |
| H12 | Use the same hydration helper for Android, desktop and CLI.                                                    |

Suggested starting history budgets:

| Profile              | Recent history budget |
| -------------------- | --------------------- |
| Small local model    | 512–1,024 tokens      |
| Standard local model | 1,024–2,048 tokens    |
| Cloud model          | 2,048–4,096 tokens    |

These are initial tuning values, not benchmark results.

A previous assistant reply of reasonable length should normally be cheaper to include directly than to pay for another model iteration, a `memory_find` call and the subsequently recovered result.

## **3. Low-level recall correctness**

The documents contain conflicting descriptions of recall timing:

- `agent-context.md` describes query-conditioned recall in `on_prepare`, using the current user message.
- `agent-memory-next.md` still describes several flows based on scoring turn N and injecting into turn N+1.
- The constants still describe pending recall as a next-turn mechanism.

These need to be reconciled before further optimization.&#x20;

Pasted markdown (2)(9).md

Pasted markdown(20261125-180349).md



### **Retrieval TODOs**

| ID  | TODO                                                                                                    |
| --- | ------------------------------------------------------------------------------------------------------- |
| R01 | Confirm that recall is conditioned on the current user request, not merely the previous turn's query.   |
| R02 | Associate pending results with `session_id`, query hash and generation number.                          |
| R03 | Reject pending recall generated for another query or session.                                           |
| R04 | Do not consume pending recall permanently before a provider request has accepted the assembled context. |
| R05 | Clear pending recall when its originating session is deleted.                                           |
| R06 | Ensure disabled recall profiles cannot consume stale pending files.                                     |
| R07 | Perform overlap suppression against records actually rendered, not the theoretical newest-10 window.    |
| R08 | Distinguish full-content overlap from summary overlap.                                                  |
| R09 | Continue examining ranked candidates when an oversized candidate does not fit.                          |
| R10 | Filter inactive, deleted, unauthorized and sensitive records before ranking.                            |
| R11 | Deduplicate using the composite identity `(scope, session_id, item_id)`.                                |
| R12 | Use deterministic secondary sorting for equal RRF scores.                                               |
| R13 | Return no recall hits for an empty or unusable query unless explicit reference resolution applies.      |
| R14 | Introduce a minimal relevance gate so that RRF does not always produce five unrelated memories.         |
| R15 | Revalidate candidate existence and authorization immediately before materialization.                    |
| R16 | Normalize full reference paths and basenames into BM25 search text.                                     |
| R17 | Verify that lexical-only mode never initializes the embedding backend.                                  |

### **R07/R08 — particularly important**

Current overlap suppression can discard an item because it is among the recent items, even when its useful content was truncated or never rendered.

A simple solution:

```
enum class InjectedCoverage {
    None,
    LocatorOnly,
    Summary,
    Full
};
```

Track coverage for each injected item. Only suppress recall when the useful information has already been supplied.

A file locator is not equivalent to a file summary, and a 192-byte conversation excerpt is not equivalent to the original reply.

## **4. Memory eviction and persistence**

The shared pool of 50 items is a poor fit for different memory lifecycles. Conversation pairs, artifacts, checkpoints and internal diagnostic records compete for the same space.&#x20;

Pasted markdown (2)(9).md



I would keep the existing JSON schema for now and introduce category-specific eviction policies.

| ID  | TODO                                                                                        |
| --- | ------------------------------------------------------------------------------------------- |
| E01 | Give `turn_pair` records a separate eviction quota.                                         |
| E02 | Protect at least the newest four completed conversation pairs.                              |
| E03 | Exclude security bookkeeping and self-check records from user-memory quotas.                |
| E04 | Reserve a bounded diagnostic quota instead.                                                 |
| E05 | Ensure a large batch of newly written items cannot permanently exceed the store cap.        |
| E06 | Use deterministic eviction ordering for equal timestamps.                                   |
| E07 | Update `last_used_at` when an item is explicitly retrieved with `memory_find`.              |
| E08 | Bump usage on successful artifact resolution, not only substring matches in generated text. |
| E09 | Keep explicit user preferences separate from evictable model-generated hints.               |
| E10 | Never merge distinct user facts solely because their embeddings are similar.                |
| E11 | Cascade deletions to vector entries, pending recall and derived caches.                     |
| E12 | Ensure stale background indexing jobs cannot resurrect deleted records.                     |
| E13 | Serialize concurrent session updates or introduce revision-checked writes.                  |
| E14 | Make memory and index writes atomic where practical.                                        |

Suggested starting quotas:

```
constexpr size_t k_session_artifacts_max    = 50;
constexpr size_t k_session_turn_pairs_max   = 24;
constexpr size_t k_session_checkpoints_max  = 12;
constexpr size_t k_session_diagnostics_max  = 16;
constexpr size_t k_session_rules_max        = 20;
```

These can initially remain within `memory_state.items`, with eviction grouped by kind. No schema migration is required just to separate quotas.

The authoritative conversation and tool logs should continue to exist independently of these compact records.

## **5. Prompt renderer cleanup**

The new byte caps are useful defensive limits, but they are not yet an effective model-context budget.

The 160 KiB final cap represents approximately 40,960 tokens at four bytes per token, before tool schemas, the live request and output capacity. Actual token counts vary significantly. The Android system template alone is documented as approximately 11 KB.&#x20;

Pasted markdown (2)(9).md



| ID  | TODO                                                                                            |
| --- | ----------------------------------------------------------------------------------------------- |
| P01 | Rename byte-based `*_chars` constants to `*_bytes`, or document their exact units consistently. |
| P02 | Move the hard-coded 520-byte turn-record limit into `llm_constants.hpp`.                        |
| P03 | Include headings, separators and omission markers in section budgets.                           |
| P04 | Remove whole-record truncation where it can destroy IDs and recall handles.                     |
| P05 | Never truncate paths, URLs, item IDs or tool-call IDs into unusable values.                     |
| P06 | When a locator exceeds its field budget, emit an opaque resolvable handle instead.              |
| P07 | Serialize untrusted fields with quoting and control-character escaping.                         |
| P08 | Do not treat newline flattening as prompt-injection protection.                                 |
| P09 | Move stored conversation text out of the privileged system message where supported.             |
| P10 | Replace final string ellipsizing with complete-section removal under pressure.                  |
| P11 | Ensure security constraints are never discarded by truncation.                                  |
| P12 | Validate feature-mask enforcement inside `${VAR}` substitution.                                 |
| P13 | Skip prompt-file loading when its feature is disabled.                                          |
| P14 | Verify that template substitution cannot recursively expand user-controlled content.            |
| P15 | Cache parsed templates and invalidate on revision or modification.                              |
| P16 | Verify that no synchronous filesystem freshness checks remain on the rendering path.            |
| P17 | Strip or redact credentials embedded in URLs, commands and stored metadata.                     |
| P18 | Build planner and executor context from the same validated session snapshot.                    |

### **P05 — important regression risk**

The new constant:

```
k_prompt_field_chars = 240;
```

is reasonable for a summary, note or description. It is not appropriate for all fields indiscriminately.

A 300-byte path must not become an invented 240-byte path that the model attempts to open.

Use distinct serializers:

```
render_text(value, budget);
render_identifier(value);
render_locator(locator, budget);
render_untrusted_literal(value, budget);
```

Keep the implementation lightweight. Four helpers are sufficient.

## **6. Constants and defaults**

Several constants currently mix defensive storage limits, performance targets and model-context budgets.

### **Recommended changes in&#x20;**`llm_constants.hpp`

| Constant                        | Current | Change                                            |
| ------------------------------- | ------- | ------------------------------------------------- |
| `k_session_items_max`           | 50      | Separate quotas by category                       |
| `k_recent_turns_inject_max`     | 4       | Keep for planner summaries                        |
| `k_recent_turns_user_chars`     | 192     | Keep for compact summaries, not primary history   |
| `k_recent_turns_asst_chars`     | 192     | Same                                              |
| `k_recent_turns_inject_chars`   | 2,200   | Keep for legacy summary path                      |
| `k_prompt_field_chars`          | 240     | Apply to text only                                |
| `k_prompt_path_list_max`        | 32      | Also enforce a total byte/token budget            |
| `k_prompt_json_inject_chars`    | 2,000   | Keep; ensure serialized JSON remains valid        |
| `k_system_extra_inject_chars`   | 32 KiB  | Make model-dependent                              |
| `k_system_prompt_max_chars`     | 160 KiB | Keep as emergency ceiling only                    |
| `k_recall_top_k`                | 5       | Keep                                              |
| `k_recall_inject_chars`         | 1,500   | Keep initially                                    |
| `k_recall_dense_budget_ms`      | 150     | Distinguish soft target from enforceable deadline |
| `k_file_read_window_bytes`      | 96 KiB  | Clamp against remaining model-context capacity    |
| `k_session_journal_max_entries` | 200     | Protect unfinished turns from compaction          |

The current 150 ms dense budget is documented as triggering lexical fallback on the *next* turn. It is not necessarily a deadline on the current operation.

That distinction should be explicit in naming and telemetry.

### **Feature masks**

Both feature enums currently use:

```
All = 0xFFFFFFFFu
```

Prefer an explicit mask of known features. Otherwise, unknown and future bits are automatically enabled.

Also add:

```
StructuredMemoryFeature::InjectRecentMessages
StructuredMemoryFeature::InjectStoredFacts
StructuredMemoryFeature::InjectTaskState
StructuredMemoryFeature::InjectTaskEvents
```

No need to add dozens of new flags. These four address concrete gaps.

## **7. Memory retrieval performance**

I would not change BM25, RRF or the underlying vector index yet.

The current retrieval stack is sufficiently capable for personal chat memory. More benefit is likely available from eliminating redundant work.

| ID  | TODO                                                                                                                            |
| --- | ------------------------------------------------------------------------------------------------------------------------------- |
| F01 | Do not enumerate every past chat on every ordinary same-session follow-up unless cross-session retrieval is enabled and useful. |
| F02 | Cache normalized `search_text` by item revision.                                                                                |
| F03 | Cache BM25 corpus statistics by corpus generation.                                                                              |
| F04 | Embed the current query once per retrieval pass.                                                                                |
| F05 | Do not re-embed unchanged memory entries.                                                                                       |
| F06 | Batch vector-store saves instead of saving after every upsert.                                                                  |
| F07 | Serialize access to the non-thread-safe VectorStore.                                                                            |
| F08 | Avoid full JSON copies during candidate enumeration and sorting.                                                                |
| F09 | Precompute timestamps and relevance keys before sorting.                                                                        |
| F10 | Avoid loading the last 20 `recent_events` when no consumer requires them.                                                       |
| F11 | Reuse the already hydrated session snapshot rather than reloading it within individual features.                                |
| F12 | Keep slow dense inference off the provider-critical path, with lexical results available as fallback.                           |
| F13 | Do not persist an empty pending-recall result.                                                                                  |
| F14 | Enforce a maximum candidate text size before normalization and tokenization.                                                    |
| F15 | Add cancellation checks between candidate collection, BM25, dense ranking and serialization.                                    |
| F16 | Release the embedding backend on Android memory-pressure events where safe.                                                     |

One consideration: asynchronous indexing and asynchronous retrieval are different jobs. Indexing can generally happen after a turn. Retrieval for an unknown future query cannot be completed in advance.

## **8. Exact body and payload retrieval**

The canonical-log approach is good. Keep it.

The docs already establish bounded retrieval, same-session payload access and failure on missing or mismatched logs.&#x20;

Pasted markdown (3)(2).md



Remaining low-level checks:

| ID  | TODO                                                                                      |
| --- | ----------------------------------------------------------------------------------------- |
| X01 | Clearly distinguish `body_available`, `body_missing` and `body_denied`.                   |
| X02 | Validate `offset` and `maxBytes` against overflow and signed-to-unsigned conversion.      |
| X03 | Define offsets consistently as UTF-8 byte offsets.                                        |
| X04 | Guarantee that every nonterminal page advances `next_offset`.                             |
| X05 | Handle an offset in the middle of a UTF-8 code point consistently.                        |
| X06 | Return `total_bytes`, `returned_bytes`, `next_offset` and `complete`.                     |
| X07 | Attach a source revision or content digest for paginated retrieval.                       |
| X08 | Recheck access policy on every retrieval request.                                         |
| X09 | Evaluate sensitivity by payload provenance and content, not only tool name.               |
| X10 | Never describe reduced or truncated canonical payloads as the original upstream response. |
| X11 | Invalidate availability hints when source logs are pruned.                                |
| X12 | Distinguish an empty successful payload from an unavailable payload.                      |

Especially X09: a benign tool such as `file_read`, shell execution or HTTP retrieval can still return credentials. A sensitive-tool-name denylist alone is insufficient.

## **9. Android and realtime**

The documentation identifies a potentially separate continuity problem in the realtime delegation path.&#x20;

Pasted markdown (2)(9).md



| ID  | TODO                                                                                                   |
| --- | ------------------------------------------------------------------------------------------------------ |
| A01 | Log the effective session ID at Android request preparation.                                           |
| A02 | Persist session identity independently of Compose lifecycle.                                           |
| A03 | Ensure activity recreation cannot create a fresh agent session for an existing UI thread.              |
| A04 | Pass the parent session identity through `run_pm_agent`.                                               |
| A05 | Verify that realtime and text-agent writes become visible to each other.                               |
| A06 | Prevent concurrent voice and text turns from overwriting session memory.                               |
| A07 | Rehydrate memory after a delegated child turn completes.                                               |
| A08 | Ensure cancellation does not leave thread-local memory scope attached to the next invocation.          |
| A09 | Use an explicit scope guard for thread-local session state.                                            |
| A10 | Propagate session context explicitly into background workers; do not rely on thread-local inheritance. |
| A11 | Disable unnecessary dense recall for simple voice commands.                                            |
| A12 | Measure cold embedding initialization separately from ordinary recall latency.                         |
| A13 | Verify artifact locators retain the owning node for paired desktop resources.                          |
| A14 | Never resolve a desktop locator against the Android application's local filesystem.                    |

I would reuse an existing connection or node identity in artifact metadata rather than introduce the full workspace model for this purpose.

## **10. Durability**

The journal improvements already listed in the roadmap remain worthwhile.

| ID  | TODO                                                                                     |
| --- | ---------------------------------------------------------------------------------------- |
| D01 | Add `turn_id` to every journal tool record.                                              |
| D02 | Salvage complete JSONL records when the final record is torn.                            |
| D03 | Back up corrupt journals before rewriting.                                               |
| D04 | Preserve records belonging to unfinished turns during ring compaction.                   |
| D05 | Make recovery promotion idempotent by stable artifact ID.                                |
| D06 | Record incomplete turns without automatically re-executing their actions.                |
| D07 | Verify atomic replacement of pending-recall sidecars.                                    |
| D08 | Synchronize journal compaction with concurrent appenders.                                |
| D09 | Reconcile the vector index against the authoritative store after interrupted indexing.   |
| D10 | Apply equivalent privacy and storage protection to derived indexes and recall sidecars.  |
| D11 | Make session deletion cascade to journals, derived indexes and associated pending state. |
| D12 | Revalidate artifact existence before recovery promotion.                                 |

The index should always be disposable and reconstructable. It must never become the only surviving copy of a user fact.

## **11. Documentation inconsistencies to clean up**

The roadmap appears to contain both current implementation descriptions and older design proposals.

| Subject                | Inconsistency                                                                                                    |
| ---------------------- | ---------------------------------------------------------------------------------------------------------------- |
| Recall timing          | Current-turn `on_prepare` versus next-turn `on_finish`                                                           |
| `memory_find`          | Described as substring-only in several sections, but marked as wired to the hybrid ranker in the completed TODOs |
| Reference paths        | Described as basename-only, but full-path persistence marked completed                                           |
| Index gardening        | Described as background, synchronous and proposed in different places                                            |
| Evicted memory         | Described as reachable through recall, while cascading index deletion is marked implemented                      |
| Embedding feature gate | References both `FEATURE_LLAMA` and `FEATURE_COMMAND_LLAMA_INFERENCE`                                            |
| Rule quota             | Comments differ on whether inactive rules consume the quota                                                      |
| Memory scope           | Session-only persistence versus omitted-ID global persistence                                                    |
| Recall test plan       | Several tests still assert the older N-to-N+1 pending behavior                                                   |

For example, `agent-memory-next.md` marks full-path capture and hybrid `memory_find` integration as completed while earlier sections describe those capabilities as missing.

Before assigning more development work, verify these against the implementation and remove obsolete descriptions. Otherwise, coding agents may inadvertently reimplement or revert existing functionality.

## **12. Additional regression tests**

### **Implementation regression checklist**

0 / 24

T01

Write a description → now make it shorter

T02

Choose option B → explain why we chose it

T03

Create a long response → export it verbatim

T04

Create 50 artifacts → previous conversation remains available

T05

Previous unfinished delete request → unrelated new question

T06

Memory write without task_id → remains session-scoped

T07

Explicit global preference → available in another session

T08

Recall disabled → stale pending sidecar ignored

T09

Different consecutive queries → no stale query-conditioned recall

T10

Item present only as locator → useful summary can still be recalled

T11

Two sessions with identical item IDs → no cross-session collision

T12

Delete session during indexing → no resurrected memory

T13

Oversized Unicode locator → no corrupted path

T14

Malformed JSON memory → no exception during prompt build

T15

Prompt budget exhausted → policy and live request preserved

T16

Android activity recreation → same conversation identity

T17

Voice turn followed by text follow-up → shared continuity

T18

Concurrent voice/text writes → no lost updates

T19

Lexical profile → embedding backend never initializes

T20

Torn journal → deterministic recovery without duplicate actions

T21

Paged body retrieval → exact UTF-8 reconstruction

T22

Sensitive tool payload → no unauthorized recall

T23

Paired desktop file → correct node resolution

T24

All memory injection features disabled → no memory leakage

&#x20;Copy as Markdown

## **13. Telemetry additions**

Add these fields to existing turn telemetry rather than introducing a separate monitoring subsystem.

```
{
  "context": {
    "system_bytes": 0,
    "history_bytes": 0,
    "history_messages": 0,
    "artifact_bytes": 0,
    "recall_bytes": 0,
    "tool_schema_bytes": 0,
    "estimated_input_tokens": 0,
    "omitted_items": 0,
    "omitted_turns": 0,
    "budget_exhausted": false,
    "build_ms": 0
  },
  "memory": {
    "session_id": "...",
    "session_revision": 0,
    "turn_pairs": 0,
    "artifacts": 0,
    "evictions": 0,
    "load_ms": 0,
    "save_ms": 0
  },
  "recall": {
    "candidate_collection_ms": 0,
    "normalization_ms": 0,
    "bm25_ms": 0,
    "embedding_ms": 0,
    "dense_ms": 0,
    "fusion_ms": 0,
    "overlap_suppressed": 0,
    "stale_candidates": 0,
    "injected_hits": 0,
    "cold_start": false
  }
}
```

Do not log raw memory contents, credentials or private search queries by default.

## **14. Implementation order**

| Phase | Scope                                      | Priority |
| ----- | ------------------------------------------ | -------- |
| 1     | Native-role recent conversation history    | P0       |
| 2     | Session identity and memory tool scope     | P0       |
| 3     | Remove contradictory synthesized rules     | P0       |
| 4     | Fix recall timing and overlap suppression  | P1       |
| 5     | Separate eviction quotas                   | P1       |
| 6     | Prompt budget and locator correctness      | P1       |
| 7     | Android/realtime concurrency and lifecycle | P1       |
| 8     | Journal hardening and index consistency    | P1       |
| 9     | Eliminate redundant retrieval work         | P2       |
| 10    | Documentation reconciliation               | P2       |

## **15. Things I would explicitly defer**

- Graph-based memory retrieval.
- Cross-encoder reranking.
- LLM-based listwise recall judges.
- Semantic memory compaction.
- A dedicated memory service.
- Another vector database.
- A comprehensive workspace-management subsystem.
- Separate Android and desktop memory implementations.

The existing BM25/RRF/VectorStore combination is sufficient for now.

**The most consequential remaining change is restoring ordinary conversation history to the actual message stream.** That should improve short follow-ups, reduce unnecessary recall calls and make smaller models considerably more usable without increasing architectural complexity.

This review covers the three updated attachments: the constants header and both memory/context documents. The revised implementation files themselves were not supplied, so the checklist distinguishes documented problems from defensive implementation checks; it should not be interpreted as confirmation that every listed defect still exists.

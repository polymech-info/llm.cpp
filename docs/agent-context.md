# Agent context — multi-turn flow

Review notes for how one chat turn becomes the next prompt. This is the
path that has to explain a same-session follow-up ("as I said", "now do
that", "what did we decide") without the user repeating themselves.

Related:

- [agent-features.md](./agent-features.md) — feature pipeline
- [agent-memory-next.md](./agent-memory-next.md) — recall roadmap
- `src/llm/agent_context.cpp` — system-prompt assembly
- `src/llm/agent_memory.cpp`, `agent_memory_artifacts.cpp` — session store, turn pairs, eviction
- `src/llm/tools/memory/MemoryTools.cpp` — `memory_read` / `memory_write` / `memory_find`
- `src/android/pm_android_agent.cpp` — phone turn hydrate

---

## What the model actually receives

A normal text turn does **not** resend the chat transcript as user and
assistant messages. `AgentFactory::run` builds:

1. One `system` message: `build_system_prompt`.
2. Optional `seed_chat_messages` (OpenAI-compat daemon only). Desktop chat
   and the Android agent do not fill this.
3. One `user` message: the current prompt.

Past conversation is reconstructed into the system message from the
session store, or it is not in the prompt at all. The phone UI can show
a long thread while the model sees only the latest sentence plus whatever
the injectors kept.

`task_id` is the gate. The memory blocks in `build_system_prompt_impl`
run only when `turn.task_id` is non-empty. Desktop chat mints or reloads
that id from the chat session (`ChatWebPanel_Agent.cpp`). Android does
the same with `session_get_or_create(req.session_id)` and then
`session_load` (`pm_android_agent.cpp`). An empty or unstable
`session_id` starts a blank session even if the UI still shows the old
thread.

---

## One turn, then the next

```
UI / CLI / phone
  session_get_or_create + session_load(task_id)
  Turn { user_prompt, task_id, memory_state, recent_events, source_log_* }
        |
        v
on_prepare, phase order
  PlannerFeature          planner-prompt.md + compact memory tail
                          writes planner_meta, TurnMode, tool allowlist
  ToolsFeature / MCP      OpenAI tool schemas for this turn
  MemoryFeature           thread-local task scope (persist is on_finish)
  MemoryRecallFeature     index + enqueue pending recall for THIS prompt
  Snapshot / skills / Agent.md / workflow
  SystemPromptFeature     system-prompt.md + dynamic blocks below
        |
        v
wire: [ system, (seed history), user ]
LLM + tools
        |
        v
on_finish
  MemoryFeature           capture turn_pair, artifacts, rules; evict; save
  MemoryRecallFeature     index new items (recall for the NEXT user text
                          already ran at the start of this turn)
```

`default_agent()` order is in `agent_factory.cpp`: embed, path scope,
planner, optional browser-use, tools, host tools, optional MCP, memory,
optional tool checkpoints, memory recall, snapshot, skills, Agent.md,
workflow, system prompt, self-check.

Android uses the same factory. Prompt files resolve to `*.android.md`
first (`agent_commons.cpp`, `android_prompt_filename`). The phone also
sets `connection_context` to phone-local/cloud and `folder_hint` to the
app profile dir.

---

## Three prompts

They are different calls. None of them is the chat transcript.

| Prompt | File | Who loads it | What it is |
| --- | --- | --- | --- |
| System | `system-prompt.md` / `system-prompt.android.md` | `SystemPromptFeature` → `load_system_prompt_md` | Persona, tool policy, `${...}` substitution. Dynamic blocks are appended in `build_system_prompt_impl`, not inside the file. |
| Planner | `planner-prompt.md` / `planner-prompt.android.md` | `PlannerFeature` | Pre-pass. Picks tools and writes `intent` / `action` / `subject` / `confidence`. Sees a tighter copy of recent turns, session items, and rules (`append_planner_session_memory`). |
| Realtime | `realtime-prompt.md` / `realtime-prompt.android.md` | voice session, not the text agent | Talks. Work is delegated with `run_pm_agent`. It does not receive the text agent's `## Recent turns`. |
| Mode overlay | `mode-<name>.md` | system prompt, when `Turn.mode != Auto` | Short behavior note (`lookup`, `execute`, `create`, `followup`, …). Planner sets the mode from intent. |

`user-preferences.md` is appended only when `AgentContextFeature::Preferences`
is on. `ReplayTranscript` is **off** in `k_default_structured_memory_features`.
Loading `recent_events` does not put them in the prompt unless that bit is set,
or the turn is a scheduled tick.

Shipped sizes (not budgets): desktop `system-prompt.md` is about 6 KB,
`system-prompt.android.md` about 11 KB, mode overlays about 400 bytes.
Assembly caps live in `llm_constants.hpp` (`k_system_prompt_file_chars`
16 KiB, `k_prompt_overlay_chars` 8 KiB, `k_system_prompt_max_chars` 160 KiB).
The 160 KiB cap ellipsizes the **end** of the string. Memory blocks are
appended after the persona, so a blow-up cuts memory first.

---

## Two stores that the prompts treat as one

| Store | Write | Next turn |
| --- | --- | --- |
| Session document for `task_id` | `MemoryFeature` on finish (`turn_pair`, artifacts, instructions). `memory_write` **with** `task_id` calls `session_write_memory`. | `session_load` → `turn.memory_state` / `turn.recent_events`. Injected by the system prompt when the feature bits say so. |
| `default_memory.doc` (`pm://session-memory/default_memory.doc`) | `memory_write` **without** `task_id`. | Not loaded by `session_load`. Not pasted into the system prompt. Reachable by `memory_read`, and as a hybrid-recall candidate (`collect_default_memory_candidates`). |

`do_memory_write` tells the model, for both paths: "It will be injected
into your system prompt on the next turn." That sentence is true only for
the `task_id` path. The default path stores a long-term document and waits
for a later `memory_read` or a recall hit.

The system-prompt line added for secrets ("persist … for this session")
describes the session, not `default_memory.doc`. The Android system prompt
also says `memory_write` saves `default_memory.doc`, and that `memory_find`
searches long-term facts. The dynamic `## Recent turns` block tells the
model to call `memory_find` to recover a truncated reply. Those are three
different jobs for one tool name.

---

## What a follow-up can see

Defaults, after `session_load`, inside `build_system_prompt_impl` when
`task_id` is set and `is_scheduled_tick` is false:

| Block | Flag | Budget | Contents |
| --- | --- | --- | --- |
| `## Recent turns` | `InjectItems`, and not `ReplayTranscript` | last **4** `turn_pair`s, about **192** bytes of user text, **192** of assistant text, **520** for the whole record, **2200** for the section | Compact tail. Tool lines are hints. Full reply is not here. |
| `## Session Items` | `InjectItems` | **10** items, **4000** bytes. Planner uses half. | Artifacts and checkpoints. `role=turn_pair` is skipped here so it is not listed twice. |
| `## Relevant Session Rules` | `InjectRules` | **1500** bytes, **20** stored rules | Includes synthesized "act on the newest item, do not ask" rules. |
| `## Recalled memory` | `InjectPendingRecall` | `k_recall_inject_chars` (**1500**) at render | Whatever hybrid recall enqueued at the **start** of this turn. |
| `## Conversation History` | `ReplayTranscript` | up to **36_000** bytes (`k_session_memory_inject_chars`) | Off by default. This is the only automatic dump of `recent_events`. |
| `## Stored Memory` | none; runs whenever `memory_state` is a non-empty object | **2000** bytes if the object has no items/artifacts/instructions, else **800** bytes of leftover keys | Not `default_memory.doc`. |

At capture, `turn_pair` user and assistant strings are clamped to
`k_session_item_summary_chars` (**400**) when `TruncateSummaries` is on.
The render caps above then cut that again. `assistant_chars` and
`body_available` point at the run log. The exact reply comes back only if
something calls:

```text
memory_find(itemId=<turn id>, includeBody=true)
```

Same for one tool envelope: `toolCallId` + `includePayload=true`. The
executor advertises those lines only when `memory_find` is in the tool
schema. The planner's `append_recent_turns` call does not pass that flag,
so the planner still mentions `memory_find` by default.

`## Recent turns` now says to continue an earlier action only when the
current request refers to it. The synthesized hard rules still say a terse
follow-up **is** the newest session item and the model must not ask. A
short Android follow-up is pulled both ways.

---

## Eviction

All of this is in `agent_memory_artifacts.cpp` after the turn pair is patched.

**Items** (`EvictOldest`, on by default). One pool, cap
`k_session_items_max` (**50**). Rank key is `last_used_at`, else
`created_at` (`item_recency_at`). Oldest keys are deleted by writing
`null` into the patch. Keys written in **this** patch are skipped, so the
new turn pair survives the turn that created it.

`turn_pair` rows live in that same pool as files, images, and tool
checkpoints. A turn pair's `last_used_at` is stamped at creation. Later
bumps look for a path / index / url needle in the user text. A turn pair
usually has no such locator, so it does not get bumped. A session that
creates many artifacts can drop older conversation rows before it drops
recent files.

**Rules.** Active instructions, cap `k_session_rules_max` (**20**), oldest
`created_at`. Rules synthesized this turn are not the eviction victims.

**Events.** `session_append_event` keeps `k_max_events` (**50**).
`session_load` from chat and Android asks for the last **20**. With
`ReplayTranscript` off those 20 are loaded onto the Turn and then not
rendered. A scheduled tick injects at most **8** events and **2000** bytes
(`k_scheduled_events_inject_max`, `k_prompt_json_inject_chars`).

**Facts in `default_memory.doc`.** No LRU in the session evictor. They
are a different file. They disappear from the model's point of view when
recall does not select them and nobody calls `memory_read`.

**Hybrid recall.** `MemoryRecallFeature::on_prepare` searches the current
user text against session items and `default_memory.doc`, then may set
`pending_recall_inject` for the system prompt of **this** turn (the search
ran before the prompt was built). Hits already in the recent Session Items
window are suppressed. A fact that was truncated inside `## Recent turns`
can be treated as "already shown" and not expanded.

---

## Why a same-session follow-up still needs a re-hint

Observed on Android: the user has to restate context the thread already
contains. The code allows that without a store bug.

1. The wire has no prior user/assistant messages. Continuity is only the
   system-prompt tail.
2. That tail is four turn pairs, a few hundred bytes each, not the
   decision from two minutes ago.
3. The full reply is behind `memory_find`. The static Android prompt
   describes that tool as long-term fact search, not "reload the last
   answer". A small model can skip the call.
4. A fact saved with `memory_write` and no `task_id` is in
   `default_memory.doc`. The tool result says it will be in the next
   system prompt. The builder never reads that file. The next turn sees
   it only if hybrid recall ranks it against the new sentence.
5. Eviction can remove an older `turn_pair` once artifacts fill the 50
   slots, because conversation rows are not re-bumped.
6. If the phone's `req.session_id` changes between messages, `session_load`
   reads a different document and the tail is empty. The UI thread and the
   agent session are only the same if that id is stable.

Desktop chat uses the same prompt builder. It re-hints less when the model
is strong enough to call `memory_find`, or when the follow-up still fits
in the 192-byte lines. That is a model difference sitting on top of the
same thin tail, not a second memory implementation.

---

## Open questions

For a later pass. Each one is a behavior the current code already has.

1. Should a same-session text turn put a bounded prior-message list in the
   wire (`seed_chat_messages` or real roles), and keep session memory for
   artifacts only? The comment in `agent_factory.cpp` says the system
   prompt is what carries multi-turn recall, including for local models.
   Phone models are the ones failing that bet.

2. `memory_write`'s success text claims next-turn injection for the
   default document path. Should that sentence be removed, or should
   `default_memory.doc` actually be injected (bounded, non-secret) the
   way session extras are?

3. Is `task_id` supposed to be filled by the host only, with the tool
   writing the current session when the argument is omitted? Today an
   omitted id selects the global document, which is the opposite of the
   system line "persists for the lifetime of this chat session."

4. Should `turn_pair` rows be evicted on their own cap, or excluded from
   the artifact LRU? Right now a file-heavy session and a long chat share
   50 slots, and conversation rows lose the bump.

5. The hard rule "terse follow-up → newest item, do not ask" and the
   recent-turn line "continue only when this request refers to it"
   disagree. Which one should a follow-up on Android obey? The re-hint
   complaint means the model is asking; the rule was added because it
   asked too much. Both can be true if the tail is too small to act on.

6. Planner advertises `memory_find` even when the executor schema does
   not have it yet (`append_recent_turns` default `advertise_body_recall`).
   Should the planner receive the same capability bit the executor uses?

7. Hybrid recall suppresses hits that overlap the recent window. The
   window contains a 400-byte capture later cut to ~192. Should overlap
   mean "the full body is already in the prompt", which it is not?

8. `recent_events` is loaded (20) and stored (50) and then ignored unless
   `ReplayTranscript` is on. Is the ring still the continuity mechanism,
   or is it leftover next to `turn_pair`? Turning the 36 KB replay back
   on is a different choice from injecting the last two events under the
   recent-turn budget.

9. Realtime on the phone tells the voice model it remembers facts across
   sessions, then delegates work to `run_pm_agent`. Does that child turn
   get the text session's `task_id`, a new one, or only `default_memory.doc`?
   A voice follow-up that cannot see `## Recent turns` will ask again.

10. The final 160 KB ellipsize drops the end of the system string, which
    is where memory sits. On a small Android context window, is the
    failure "tail too small" or "tail clipped by the persona + tool
    essay in `system-prompt.android.md`"? The android file is already
    ~11 KB before any session block.

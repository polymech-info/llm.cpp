# Session context — "create it" lost the thread

Handoff for the 2026-09-27 Tanit chat failure. The agent had the prior turns
in the prompt and still answered "What would you like me to create?"

Do not treat this as "recent prompts were not injected." They were. Do not
patch it by forcing `run` when `create_command` is stripped. That guess was
tried and removed.

## Incident

Session `session-1a0e2436bc5-3`, chat session
`ad895c18-5cca-4891-8186-e34f693d9782`. Working folder
`C:\Users\zx\Desktop\tanit`. Open file: `ori.srt` (1 KiB, 132 lines). No
selection. Model `gpt-6-astra`. Each turn is a fresh request
(`previous_response_id` null on round 1).

| Turn | User | Assistant |
|------|------|-----------|
| 1 | we have a command that creates subtitles from wav files ? | Yes — "Subtitles" custom command, Whisper, writes an SRT beside the WAV. Tool: `info_lookup`. |
| 2 | how would a command for srt back to audio look like ? | `audio tts-scripted --srt … --dst …`. Tool: `info_lookup`. |
| 3 | create it | What would you like me to create? Tools: `info_lookup` twice (`audio tts-scripted`, then its schema). |

Logs (copied into the session document store):

- `sessions/resources/documents/session-memory__session-1a0e2436bc5-3.doc`
- `session-log__…__agent-09-27-11-47-44-11-47-44.doc` (turn 1)
- `session-log__…__agent-09-27-11-47-56-11-47-56.doc` (turn 2)
- `session-log__…__agent-09-27-11-48-12-11-48-12.doc` (turn 3, the failure)

Turn 3 log id `agent-09-27-11-48-12-11-48-12`. Self-check passed
(`deflection_detected: false`). The clarifying question was not flagged.

## What the prompt actually contained

Turn 3's planner user message and executor system prompt both included
`## Recent turns` with both prior user prompts in full, plus truncated
assistant replies and the tool name `info_lookup`. The executor's first call
was `info_lookup` query `audio tts-scripted`, so it used that block.

There was no `## Recalled memory` block and no `## Session Items` block.
The only stored items were `turn_pair`s and `self_check` rows. Session Items
skip turn pairs. Self-check rows are filtered out of the live window.

`## Turn Context` on the executor:

```
intent: create
action: create_custom_command
subject: SRT-to-audio command
planner note: Create the discussed SRT-to-audio custom command, looking up audio tts-scripted options for registration.
```

Then `## Behavioral Mode: create` from `dist/data/prompts/mode-create.md`:

- This is a creation or write turn.
- Before writing, briefly confirm your understanding of the target if any parameter is ambiguous.

Planner raw selection was `create_command` + `info_lookup`, confidence 0.94.
`agent_feature.cpp` then set `stripped_create_command: true` because the user
text does not ask for a ribbon / context-menu / custom command.
`selected_tools` / `valid_selected_tools` / `effective_tools` became
`info_lookup` only. The intent, action, subject, and planner note were left
as "create a custom command."

## Two history channels (the "~5" is the second one)

These are not the same list.

### 1. Recent turns — this is what carried the prompts

`append_recent_turns` in `src/llm/agent_context.cpp`, fed by `turn_pair`
items written in `src/llm/agent_memory_artifacts.cpp`.

Injected into the executor system prompt (`agent_context.cpp`, only when
`ReplayTranscript` is off) and into the planner user message
(`append_planner_session_memory` in `agent_feature.cpp`).

Caps in `src/llm/llm_constants.hpp`:

| Constant | Value | Effect on this turn |
|----------|-------|---------------------|
| `k_recent_turns_inject_max` | 4 | Both prior turns fit. Not a drop. |
| `k_recent_turns_user_chars` | 240 | Both user prompts are short. Injected whole. |
| `k_recent_turns_asst_chars` | 280 | Both assistant replies cut with `…`, including the `tts-scripted` sample, mid code fence. |
| `k_recent_turns_inject_chars` | 2200 | Not the limiter here. |

The block lists tool **names** only (`→ tools: info_lookup`). It does not
list call ids and does not paste tool result bodies.

`ReplayTranscript` is off in `k_default_structured_memory_features`. The
older `recent_events` ring (`k_session_memory_max_turns = 40` in
`src/constants.hpp`, appended by `session_append_turn`) is not rendered as
`## Conversation History`. The planner only walks `recent_events` for
`media_outputs` under `## Previous Artifacts`.

### 2. Hybrid recall — top 5, injected nothing

`k_recall_top_k = 5` in `llm_constants.hpp`. This is the "~5" channel.
`MemoryRecallFeature` scores at the end of turn N and injects a pending
`## Recalled memory` block on turn N+1 (`InjectPendingRecall`).

Turn 3's own recall telemetry (end of that turn, for the *next* turn):

```
candidates: 5
ranked_hits: 2
overlap_suppressed: 2
injected_hits: 0
injected_bytes: 0
pending_enqueued: false
```

`suppress_session_item_overlap` in `src/llm/agent_memory_recall.cpp` drops
current-session hits whose ids are already in the live window: the last 4
turn pairs plus the Session Items window. The two ranked hits were those
turn pairs, so recall added nothing on top of Recent turns. That is working
as written. It is not a second copy of the prompts.

## Tool results are stored, not injected

`CaptureTurnPairs` writes `metadata.tool_calls[]` with `id`, `name`,
`status`, `payload_chars`, `payload_available`. The body stays in the agent
log (`metadata.source_log`). Newest 8 calls only; those entries are locators,
not payloads (`agent_memory_artifacts.cpp`, comment at the call-details loop).

`## Session Items` would tell the model to recover one result with
`memory_find(itemId, toolCallId, includePayload=true)`. That hint is never
emitted for ordinary chat lookups, because `append_session_items` skips
`role == turn_pair`. Recent turns do not print the call id either.

`includeBody=true` materializes the assistant final text
(`load_agent_final_text`). `includePayload=true` materializes the tool
envelope from the log (`load_agent_tool_result`). Same-session only.
`info_lookup` is allowed. On turn 3 the model did not call `memory_find`.
It ran `info_lookup` again. `memory_find` was not in the seeded tool list.

## Why the reply was a question

History injection succeeded. The loss is after that:

1. The live user message is only `create it`. Prior turns are a system-prompt
   appendix, not prior chat messages.
2. The planner read Recent turns and chose `create_custom_command` /
   `create_command`. That is one reading of "create it" (register the command
   just described). The other reading is "run `tts-scripted` on the open
   `ori.srt`." Neither reading was disambiguated by tool payloads, because
   those were not in the prompt.
3. `user_asked_for_ribbon_command` (`src/llm/agent_commons.cpp`) is a
   substring check on the raw user prompt (`ribbon`, `custom command`,
   `create a command`, …). `create it` does not match, so
   `apply_planner_tool_selection` in `agent_feature.cpp` erases
   `create_command`. The same gate in `agent_factory.cpp` would reject the
   call if the model requested the tool later.
4. After the strip, the executor still had intent `create`, action
   `create_custom_command`, and the planner note telling it to create that
   command. Create mode then says to confirm if anything is ambiguous. The
   only tool left was `info_lookup`. It looked the command up and asked.

## Do not do this

A local reroute was added and removed in the same session: if the strip left
only `info_lookup` and `action == create_custom_command`, force intent
`execute`, seed `run`, and rewrite the planner note to "run the command
already discussed, use the open file." That picks the other guess. A session
rule `rule_terse_imperative_continues_latest` and a matching
`create_command` rejection string were removed with it. Current tree does
not contain that reroute. The ribbon-button strip itself is unchanged and
should stay.

## Code map

| Piece | File |
|-------|------|
| Recent-turns render | `src/llm/agent_context.cpp` `append_recent_turns` |
| Turn-pair capture, tool-call locators | `src/llm/agent_memory_artifacts.cpp` |
| Planner sees the same recent-turns block | `src/llm/agent_feature.cpp` `append_planner_session_memory` |
| `create_command` strip | `src/llm/agent_feature.cpp` `apply_planner_tool_selection` |
| Ribbon-button phrase list | `src/llm/agent_commons.cpp` `user_asked_for_ribbon_command` |
| Runtime `create_command` deny | `src/llm/agent_factory.cpp` (parallel and serial dispatch) |
| Intent → mode overlay | `PlannerFeature::on_prepare` in `agent_feature.cpp`; `mode-create.md` |
| Recall overlap suppression | `src/llm/agent_memory_recall.cpp` `suppress_session_item_overlap` |
| Payload / body materialization | `src/llm/tools/memory/MemoryTools.cpp` |
| Caps | `src/llm/llm_constants.hpp` (`k_recent_turns_*`, `k_recall_top_k`) |
| Broader memory plan | [agent-memory-next.md](./agent-memory-next.md) |

## Next todos

The continuation-state spec is the target behavior. It is not a fit for a new
subsystem on this stack. The pieces it asks for already exist and stop one
step short of the prompt. Do these in order. Each one is a small change to a
hook that already runs. Do not add `agent_continuation_state`, a
`PlanValidity` type, candidate-action lists, or phrase tables for "do it" /
"create it".

### Already done — do not rebuild

| Spec idea | What the code already does |
|-----------|----------------------------|
| Conversation vs lazy tool payloads | Turn pairs store prose. Payloads stay in the agent log. `memory_find(..., includePayload=true)` reads one call. |
| Local context before broad recall | Recent turns are injected on this turn. Recall top-5 is overlap-suppressed against that window and only arrives next turn. |
| Environment | Open files, selection, and folder are already on the turn and in both prompts. |
| Policy strip | `create_command` is removed unless `user_asked_for_ribbon_command`. Keep that. |
| App follow-up locator | `info_lookup` with `kind=app` already stores `query` plus up to 3 `{name, exe}` on the turn-pair call detail, specifically so "open it" does not look the app up again. |

The app locator is the pattern to copy. It is stored and then not rendered.
`append_recent_turns` prints tool names only. `append_session_items` skips
`role == turn_pair`, so `payload_calls=` never appears for these lookups.
Non-app `info_lookup` (the SRT case, `kind` default / `command`) stores id,
name, status, and `payload_available`. It drops `query`.

`needs_implicit_turn_lookup` in `apply_planner_tool_selection` seeds
`memory_find` only when the prompt matches a three-way phrase conjunction
(`earlier`/`previous` AND `answer`/`result` AND `exact`/`verbatim`).
"create it" does not match. Do not grow that phrase list.

Self-check deflection phrases are inability wording ("I cannot", "I'm
unable"). "What would you like me to create?" does not match, and a failed
check is only recorded. There is no retry.

### 1. Render locators that are already on the turn pair

`append_recent_turns` in `agent_context.cpp`.

For each of the last 4 pairs, under the existing user/assistant lines, print
the compact `metadata.tool_calls[]` fields that are already there: `name`,
`id`, `payload_available`, and any literal the capture step kept (`query`,
`command`, `path`, `apps`, `input_files`, `output_files`). One recovery line
when `payload_available` is true:

```text
recover: memory_find(itemId=<turn id>, toolCallId=<id>, includePayload=true)
```

Keep the 280-character assistant ellipsis. The footer is what must survive
it. Same block already goes to the planner via `append_planner_session_memory`.

Also store `query` and `kind` on every `info_lookup` call detail, not only
`kind=app`. The arguments are already in hand in `agent_memory_artifacts.cpp`.
That is the exact literal (`audio tts-scripted`) the truncated reply lost.
Do not copy the result body.

On the same turn-pair metadata, copy the planner fields that already exist
at end of turn (`subject`, `action`, `intent`) into a short `proposed` object.
Next turn's footer can show them without a new store. Horizon is the existing
4 turn pairs; no ranking, no confidence, no decay policy beyond current
eviction.

### 2. Seed `memory_find` from those locators

In `apply_planner_tool_selection`, after the existing tool filters.

If any turn pair in the recent window has `payload_available` and not
`payload_denied`, and `memory_find` is not already selected, push it.
Respect the schema budget the same way `ensure_exact_export_tool` does.
Gate on the stored flag, not on the user wording.

Same-session payload rules in `MemoryTools.cpp` stay as they are.

### 3. Invalidate a plan whose action tool was removed

Still inside `apply_planner_tool_selection`, at each place a selected tool
is erased (`create_command` today; the check should not name that tool).

After filtering, if the planner `action` is non-empty and every tool that
was removed was required for that action (the surviving set is only
`info_lookup` or empty, and the removed name is not a lookup), set:

```text
planner_meta.plan_invalid_after_filter = true
planner_meta.rejected_tools = [ ... ]
planner_meta.rejected_reason = <existing policy reason, e.g. ribbon-button strip>
```

Then replace `reason` with a note that keeps `subject` and the rejected
capability, and says: do not restate known subject or literals; if more than
one allowed action remains, ask which of those; do not pick one.

Clear `intent` from `create` / `write` so `PlannerFeature::on_prepare` does
not load `mode-create.md`. That overlay is what turned a known subject into
"What would you like me to create?" `followup` is the closest existing mode.
Its overlay says not to ask unless the reference is genuinely ambiguous, and
not to re-explain the prior turn.

Do not map the rejected action onto `run`, `write_file`, or any other tool.
Do not add an enum. A bool plus the rewritten note is the satisfiability
check. `agent_context.cpp` should print `rejected_tools` in `## Turn Context`
when the flag is set, so the clarification names the branch
(rejected capability vs the literals still in Recent turns) instead of asking
what the user wants.

### 4. Self-check

`SelfCheckFeature::on_finish` still records tool failures and the deflection
phrases. It does not scan the final text for a generic question. Phrases like
"what would you like" and "which file" are normal replies, and the check never
changed the answer.

### 5. Tests

Extend `tests/orchestrator/test-agent-memory.mjs`. Do not start a new harness.
Cases, all generic:

| Case | Assert |
|------|--------|
| Lookup then "do it" | Recent-turns text contains the stored `query` and `toolCallId`, not only the ellipsized assistant prose. |
| Policy removes the action tool | `plan_invalid_after_filter` is set, `intent` is not `create`, `run` was not inserted. |
| Recent call has `payload_available` | `memory_find` is in `valid_selected_tools`. |

Ambiguous PNG-vs-JPEG, topic switch, and "same operation, new file" need the
footer and the invalid-plan note in place first. They are prompt-behavior
checks, not more C++ policy. Add them only after 1 and 3 are observable in
the planner message.

### Leave for later

- A dedicated continuation module, confidence, and source tags.
- Lists of candidate actions (`execute` vs `create_reusable_definition`).
  That list is the guess the invalid-plan note is supposed to refuse.
- Retry-on-failed-self-check.
- Injecting payload bodies, or turning recall top-5 into the continuation
  channel. Recall should keep suppressing hits already in Recent turns.

## Laya / Jev compaction

Parked 2026-09-27. Not wired into the prompt. The C++ todos above are
unchanged. This was a selector experiment: score past-turn spans, including
tool rows, and keep the ones that belong in the next planner prompt.

Harness: `ref/laya-context/suite.py`, model checkout `ref/laya` (0.3.20,
`convaiinnovations/laya`). Fixture is the two prior logs for this incident,
now under `C:\Users\zx\Desktop\tanit\sessions\past-tools\documents` (the
Roaming `sessions/resources/documents` copies are gone). Turn 3 is not past
context. Two turns become 8 spans (user, assistant, one row per
`info_lookup` result). Rows are clipped to 900 characters from the front.
Flag names only, not descriptions. A second grain explodes flags (91 spans)
to see batch cost.

### Speed and memory

Machine: Ryzen 7 3700X, RTX 3060 12 GB. Weights on disk are fp16. Runtime
upcasts to fp32. One checkpoint is the budget for a 4 GB card. Multilingual
alone is about 2 GB on GPU (1228 MB of parameters, 1569 MB peak while
scoring 32 candidates). English and multilingual together reserve about
3 GB and a ~3.8 GB driver footprint. Do not preload two checkpoints,
especially when the GPU is also the display.

GPU, warmed, multilingual:

| Batch | Time |
|-------|------|
| 1 question | 34 ms |
| 8 candidates, 1 yes/no each | 39 ms |
| 32 candidates | 89 ms |
| suite: 8 spans, yes/no + score | 147 ms |

CPU multilingual is about 200 ms per question. English is about 700 ms.
GPU is the only rate that fits a turn.

### What the model was asked

The first question treated any earlier user sentence as a hit, and a choice
over spans put the passage text in the option list. On `create it` that
ranked the `tts-scripted` row last (p = 0.031) and kept the user lines.

The question that worked names two state fields, `request` and `passage`:

- yes/no: does `passage` name the command, invocation, or flags `request` needs
- score 0–2: unrelated, same topic, or names the command to run

False is label B, true is label A. State is only those two fields. Open-file
path and turn index were dropped. They were being scored as evidence.

Multilingual, `create it`, floor 1.5. One span kept: the 355-character
`tts-scripted` row.

| Span | Score | Yes/no |
|------|------:|-------:|
| turn 2 tool `tts-scripted` | 1.58 | 0.57 |
| turn 1 tool `record` | 1.46 | 0.62 |
| turn 2 tool `record` | 1.40 | 0.56 |
| turn 1 tool Subtitles | 1.24 | 0.37 |
| turn 1 user | 1.12 | 0.46 |
| turn 2 assistant | 0.87 | 0.21 |
| turn 2 user | 0.87 | 0.58 |

That is the span this incident needed. User lines fall to the bottom.

The same question fails a fact that is not a command. `which whisper model
does the wav-to-subtitle command use?` still ranks `tts-scripted` first
(1.65). The turn 1 assistant line that contains `large-v3-turbo` scores
1.42. A 1.5 floor also keeps flag dumps (50 of 64 flag spans pass). The
question rewards a command-shaped row.

### Token ceiling

`build_sequence` in `ref/laya/laya/common.py` keeps the start of the state
and drops the end (`truncate_left` is false).

| Checkpoint | Context | Question budget | State room |
|------------|--------:|----------------:|-----------:|
| multilingual (mmBERT) | 1024 | 256 | ~768 |
| english (ModernBERT-large) | 512 | 192 | ~320 |

Longest trimmed span in this fixture: 257 tokens. It fits multilingual.
English cuts it, and on `create it` English ranks the Subtitles button
first (1.88) ahead of `tts-scripted` (1.76). Use multilingual for these
rows.

### Jev fit, not adopted

[fast-jev-compaction](https://github.com/tamaratran/fast-jev-compaction)
and `pi-jev-compaction` shrink a transcript until it fits, then ask Jev
which tool calls to keep. Their ceiling is 25,000 state tokens and 30,000
for state plus questions. Jev can see the conversation. Laya's 768 cannot.
Compaction does not raise that ceiling.

`fitState` runs before the request, oldest first. The first message and the
last six stay pinned:

1. Tool inputs capped at 1000, then 200, then 60 characters.
2. Long texts abridged to the first 400 and the last 150.
3. Old messages collapse to a one-line character count.
4. Old calls collapse to one line (tool, short input, ok/error, size).
5. Old messages with no call are left out.
6. Adjacent call-only lines fold into one entry.

Kept text is verbatim. A dropped result keeps the first 300 characters.
Keep threshold is 0.5 (LiteLLM's guardrail uses 0.2 and a stub sentence).

The state Jev scores does not contain the tool body. Each call is a note
like `ok, 1045 chars (omitted)`. Copying that state would hide
`tts-scripted` from the scorer. The piece that applies here is the
head-plus-tail abridge, so an invocation at the end of a row survives the
900-character prefix clip and the end-cut inside 768 tokens. Not
implemented.

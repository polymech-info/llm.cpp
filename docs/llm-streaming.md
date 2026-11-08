# LLM streaming

How Tanit streams assistant text from the agent to the chat transcript, which endpoints support it, and what is intentionally deferred.

## Transport (normal typed agent)

The normal chat panel uses the existing thread-safe bridge:

```
Agent worker (Event::TextDelta / AssistantText)
  → EnqueueScript (PostMessageW → UI thread)
  → window.pmChat.appendAssistantDelta / completeAssistantText / resetAssistantStream
  → pmChatStore (per chatSessionId + runId)
  → ChatTranscript
```

`EnqueueScript` is **not** obsolete for this panel. It marshals worker-thread agent callbacks onto the WebView UI thread. The limitation of `appendText` is that it always creates a new transcript row; streaming uses dedicated methods that update one provisional assistant bubble in place.

The shared `CWebViewManager` bus (`pm_host_event`, `cweb_bus`) is parallel infrastructure used for consent, auth, and cross-WebView RPC. Normal agent transcript streaming does **not** go through the manager bus today. A later migration to typed `pm_host_event` is optional (Phase 1b).

## Event identity (multi-session / concurrent runs)

Every streaming event carries:

| Field | Purpose |
|---|---|
| `chatSessionId` | Chat route / autosave session id (`send.chat_session_id`). Prevents background-session output from rendering in the currently viewed session. |
| `runId` | Agent run id (`active_runs` key). Distinguishes concurrent turns in the same chat session. |
| `agentSessionId` | Structured-memory task id (agent `session_create` handle). |
| `seq` | Monotonic per-run sequence for delta ordering. |

Live state is keyed by `(chatSessionId, runId)`. When the viewed session differs, deltas accumulate in a side buffer and hydrate when that session is loaded again.

## Endpoint support

| Mode | `streaming` setting | Transport | UI streaming |
|---|---|---|---|
| `/responses` | `on` (default) | SSE via `execute_responses_stream` | Yes — `TextDelta` → live bubble |
| `/responses` | `auto` | SSE when `tanit_streaming` meta allows | Yes when enabled |
| `/responses` | `off` | Buffered | No — single `completeAssistantText` at end |
| `/chat/completions` | `on` / `auto` (default) | SSE via `execute_chat_messages` (`stream: true`) | Yes — `TextDelta` → live bubble |
| `/chat/completions` | `off` | Buffered | No — single `completeAssistantText` at end |
| Realtime voice | N/A | WebSocket | Separate path (`realtimeTranscriptDelta`) |

An empty streaming override resolves to **`auto`**. `/responses` then streams when the catalog allows it (`model_info.meta.tanit_streaming`, or OpenAI-backed Responses inference). `/chat/completions` streams for `on` and `auto` (OpenAI-compatible servers accept `stream: true`). **`off`** waits for the full body. The chat `send` payload can override with `streaming`.

## Request body fields

### Implemented

- **`stream: true`** — Set explicitly on raw `/responses` stream requests in `LLMClient::execute_responses_stream` (the typed liboai overload infers it; the raw JSON path does not). Chat completions set it by passing liboai's stream callback from `execute_chat_messages`.
- **`stream_options.include_usage`** — Sent on chat-completions streams so the final SSE chunk still carries `usage` (the buffered body does not).
- **`store` + `previous_response_id`** — Responses API stateful chaining for multi-round tool loops (distinct from prompt caching).

### Deferred (capability-gated)

Do **not** send these blindly; they are provider/endpoint-specific:

| Field | Endpoint | Notes |
|---|---|---|
| `prompt_cache_key` | Chat Completions (OpenAI) | Stable key derived from session + prompt-prefix hash, not raw user text. |
| `prompt_cache_retention` | Chat Completions (OpenAI) | e.g. `"24h"`. Requires explicit model support. |

Add these only after `tanit-models-cache` (or yaml meta) exposes support flags and the client reads them — same pattern as `tanit_api_mode` and `tanit_planner_reasoning_effort`.

## UI lifecycle

1. **`agentStarted`** — Agent pill + `busy`; includes `id` (runId) and `session_id` (agentSessionId).
2. **`appendAssistantDelta`** — First delta opens a provisional `assistant` entry (`streaming: true`). Later deltas append in place.
3. **`completeAssistantText`** — Replaces bubble text with canonical final answer; clears streaming flag. Used for both streamed and buffered turns. The kbot stream parser skips re-appending `output[].message` text when SSE deltas were already received (avoids duplicated final strings).
4. **`resetAssistantStream`** — Removes provisional bubble on tool-call round, error, or cancel (speculative text must not persist).
5. **`appendThinking`** — One collapsible muted row per run; each LLM round appends a block; collapsed header shows the latest snippet. Emitted when the provider returns reasoning summary text (`output[].summary` or SSE `response.reasoning_summary_text.delta`).
6. **`agentDone`** — Clears live-run tracking for that `runId`; `setBusy(false)` when no active runs remain.

Buffered turns skip deltas and call `completeAssistantText` directly (same final rendering path).

## Verification checklist

- `text-fast` + `responses` + `streaming=on`: bubble appears before completion; one final row.
- Two concurrent sessions: switch mid-stream; each session's text stays isolated.
- Two concurrent runs in one session: independent `runId` / `seq` state.
- Multi-tool turn: speculative text cleared before tool activity; final answer streams after last tool response.
- Stream failure / stop: no orphan live bubble.
- `streaming=off`: buffered `completeAssistantText` only.
- chat-completions + `streaming=on` or `auto`: bubble appears before completion; tool-call rounds still reset the provisional bubble.

## Tests

| Suite | Command | Covers |
|---|---|---|
| Store stream lifecycle | `npm --prefix apps/chat-next run test:stream` | delta ordering, background session buffer, hydrate, complete, reset, multi-session prune |
| Responses SSE framing | `node --test packages/kbot/test/responses_sse_parse.test.mjs` | CRLF normalization, chunked deltas, reasoning summary deltas, completed event typing |

## Diagnostics (`logger::trace`)

| Location | What is logged |
|---|---|
| `LLMClient[chat]` / `[chat-stream]` | `stream=true` on the POST line; `chat-stream` logs `text_chars`, `tool_calls`, `finish_reason` when the SSE body is consumed |
| `LLMClient[responses-stream]` | `stream=true`, round completion with `text_chars`, `reasoning_chars` / `reasoning=none` |
| `agent_factory` | `emit Thinking` or `no reasoning_text this LLM round` |
| `ChatWebPanel_Agent` | First `appendAssistantDelta`, each `appendThinking` with char count |

Thinking does **not** go through trace during SSE parse (only counts at round end). Session `agent.json` gets `kind: "thinking"` only when `Event::Thinking` fires.

## Capability layer (deferred Phase 2)

Provider-gated controls land **after** streaming is stable. Proposed shape:

1. **Catalog flags** (yaml meta or `tanit-models-cache`):
   - `supports_stream_options_include_usage` (Chat Completions)
   - `supports_prompt_cache_key` / `supports_prompt_cache_retention` (OpenAI-compatible)
2. **Client gating** in `LLMClient` / `agent_factory`:
   - Read flags from resolved model row; omit fields when unsupported.
   - `stream_options.include_usage` only when streaming is on **and** flag is set.
   - `prompt_cache_key` = `hash(session_id + stable_prompt_prefix)` — never raw user text.
   - `prompt_cache_retention` only when key is sent and model allows `"24h"` / `"in_memory"`.
3. **UI** (optional later): expose cache retention in dev settings only when catalog reports support.

Responses `store` + `previous_response_id` remain the stateful multi-round mechanism; prompt-cache fields are orthogonal and must not be conflated.

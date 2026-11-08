# Jev decisions — what we measured, and how large sets fit

`llm agent decide` sends one System One request: one state, many choice / noul / score questions. OpenRouter's async Batch API does not accept this endpoint. The batch is the questions that share a state.

Jev's hard limit is about 32k tokens of context on that one request. Output tokens are not the constraint. A value (one pair, one selected item, or one whole document) is never split across requests.

## Packing

Two caps, both applied before the HTTP call:

| Flag | Default | What it stops |
| --- | --- | --- |
| `--chunk` | 32 | Too many expanded questions in one request. One value with more questions than the cap still goes out alone. |
| `--max-tokens` | 24000 | The estimated body (`state` + `questions`) passing the ceiling. `0` turns the ceiling off. |

The estimate is the JSON body length divided by 3, plus 16 for the model field. That is deliberately high, so a packed request stays under 32k even when the guess is rough. It is not a tokenizer.

Independent values are packed until the next value would pass either cap. Then another request starts. The state of each request is only the values in that request.

A single value that does not fit is an error. The message says the value is not split. Document mode (no `--selector` / `--pairs` / `--left`) that does not fit says to pass `--selector`.

Dry-run check, four items of 6,000 characters, `--chunk 32 --max-tokens 3000`: 4 requests, 4 expanded questions. One 20,000-character item at `--max-tokens 500` is rejected at about 6,728 estimated tokens. An oversized document is rejected the same way. Suite: `npm run test:llm:decide` (82 passed).

What this does not do: concurrent requests (that only shortens wall time), rewriting a value so it fits, or OpenRouter job batch.

## What the live runs said

Same model throughout: `typesafe/jev-1.13-20260917`. Labels were not on the wire. A keep is noul ≥ 0.5. Suites do not fail on a mediocre score.

**Capability fit** (`npm run test:llm:decide:live`, 15 phrases, 1 request, 30 questions, 5199 in / 1119 out). The five-way fit label matched the deterministic classifier on 7/15 (46.7%). The binary "can this evidence do the job" noul matched on 14/15 (93.3%). Misses were mostly `composed` vs `direct`, and `editorial` vs `unsupported`, with the yes/no still right. Report: `tests/llm-decide-live__15-36.md`.

**Context stuffing is not a fit.** Six tool calls from the resize session, next ask "now as webp in that same folder" (`npm run test:llm:context:live`, 1 request, 18 questions, 2989 in / 346 out). `keepResult` stayed under 0.5 on every call, including the resize that wrote the files (0.38). `stuff` put the catalog lookup (0.53) above that resize (0.44). Usable as a blunt "drop bulky bodies" pass. Not usable to choose which line the next prompt needs. Report: `tests/llm-context-live__15-53.md`.

**Learning harvest is a fit.** Eight observations from that same turn (`npm run test:llm:learn:live`, 1 request, 8 questions, 2415 in / 373 out). Exact horizon 6/8. Session store 6/8. Long-term `memory_write` store 8/8. Output path, the working command, the ask, and the filenames stayed session. PIL missing and the screenshots folder went durable (the test had labeled those `both`). Help text and the 28,925-character catalog were dropped. Report: `tests/llm-learn-live__16-01.md`.

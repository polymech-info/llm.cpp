# liboai Roadmap

This is a living backlog of improvements and ideas as we deepen our use of the library. It is intentionally lightweight and updated as we discover new needs.

## Now

- **Modern Chat Completions tool-calling** — required for the in-app chat surface (see [`packages/media/cpp/docs/chat.md`](../media/cpp/docs/chat.md)) and for any kbot-driven agent loop talking to OpenRouter / Anthropic / OpenAI / Ollama:
  - Add `tools` (JSON array, pass-through) and `tool_choice` (string or JSON) typed parameters to `ChatCompletion::create` / `create_async`.
  - Extend the response parser in `chat.cpp` (sync + streaming) to capture `message.tool_calls[]` (an array of `{id, type:"function", function:{name, arguments}}`).
  - Add `Conversation::AddAssistantToolCalls(json)` and `Conversation::AddToolResult(call_id, content)` so multi-turn tool loops are expressible.
  - New accessors: `Conversation::GetLastToolCalls() → std::vector<{id,name,arguments}>`. Keep the legacy `LastResponseIsFunctionCall` / `GetLastFunctionCallName` / `GetLastFunctionCallArguments` working for back-compat.
- Keep all existing APIs stable and intact.

## Next

- Responses streaming helpers and SSE parsing (typed event callbacks).
- `ResponseInput` helper to build Responses `input` items (mirrors `Conversation` for the Responses API).
- `output_text` convenience helper for Responses outputs.
- Structured outputs helpers for `text.format`.
- Typed `Tools` / `ToolChoice` builders (mirror our existing `Functions` class but emitting modern `[{"type":"function","function":{…}}]`). Optional — callers can already pass raw `nlohmann::json`.

## Later

- Multimodal user content for Chat Completions (`{"role":"user","content":[{"type":"text",…},{"type":"image_url",…}]}`). Not needed for the chat-md v1 (image input flows through Explorer selection), but needed for any future drag-image-into-chat ergonomics.
- More robust testing coverage (unit + integration samples).
- Improved error messaging with request context (safe, no secrets).
- Expanded docs and cookbook-style examples.
- Performance pass on JSON construction and streaming.
- Decision: PR the P0 / Now items back to upstream `D7EAD/liboai` if upstream is reachable; otherwise keep as a vendored fork.

## Observations

- The Conversation class is useful for Chat Completions; Responses lacks an equivalent.
- The library is stable but needs modernization for new OpenAI primitives.
- Maintaining compatibility is critical for existing users.
- **Legacy `function_call` / `Functions` is preserved as-is.** All providers we target (OpenRouter, OpenAI, Anthropic-via-OR, Ollama, vLLM, llama.cpp's server) still accept the modern `tools` / `tool_choice` shape — adding it is purely additive. The legacy single-`function_call` path stays so no existing caller breaks.

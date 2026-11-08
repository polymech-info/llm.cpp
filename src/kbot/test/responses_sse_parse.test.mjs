/**
 * Regression tests for Responses SSE block splitting (mirrors LLMClient::execute_responses_stream).
 * Run: node --test packages/kbot/test/responses_sse_parse.test.mjs
 */
import assert from "node:assert/strict";
import { describe, it } from "node:test";

/** @param {string} chunk */
function feedChunk(sseBuf, chunk) {
  for (const ch of chunk) {
    if (ch !== "\r") sseBuf.push(ch);
  }
}

/**
 * @param {string[]} sseBuf
 * @returns {{ events: string[]; sseBuf: string[] }}
 */
function drainEvents(sseBuf) {
  const events = [];
  const buf = [...sseBuf];
  for (;;) {
    const joined = buf.join("");
    const sep = joined.indexOf("\n\n");
    if (sep === -1) break;
    const evt = joined.slice(0, sep);
    events.push(evt);
    const consumed = sep + 2;
    let left = consumed;
    const next = [];
    for (const ch of buf) {
      if (left > 0) {
        left -= 1;
        continue;
      }
      next.push(ch);
    }
    buf.length = 0;
    buf.push(...next);
  }
  return { events, sseBuf: buf };
}

/**
 * @param {string} evt
 * @returns {{ type: string; delta: string; reasoningDelta: string }}
 */
function parseStreamEvent(evt) {
  let data = "";
  let eventName = "";
  const lines = evt.split("\n");
  for (const line of lines) {
    if (line.startsWith("data:")) {
      const payload = line.length > 5 ? line.slice(5) : "";
      const trimmed = payload.startsWith(" ") ? payload.slice(1) : payload;
      data = data ? `${data}\n${trimmed}` : trimmed;
    } else if (line.startsWith("event:")) {
      eventName = line.length > 6 ? line.slice(6).trim() : "";
    }
  }
  if (!data || data === "[DONE]") return { type: "", delta: "", reasoningDelta: "" };
  const j = JSON.parse(data);
  const typ = j.type || eventName;
  if (typ === "response.output_text.delta") {
    return { type: typ, delta: typeof j.delta === "string" ? j.delta : "", reasoningDelta: "" };
  }
  if (typ === "response.reasoning_summary_text.delta" || typ === "response.reasoning_text.delta") {
    return { type: typ, delta: "", reasoningDelta: typeof j.delta === "string" ? j.delta : "" };
  }
  return { type: typ, delta: "", reasoningDelta: "" };
}

describe("responses SSE parse", () => {
  it("normalizes CRLF and splits on blank line", () => {
    const buf = [];
    feedChunk(buf, "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Hi\"}\r\n\r\n");
    const { events } = drainEvents(buf);
    assert.equal(events.length, 1);
    assert.deepEqual(parseStreamEvent(events[0]), {
      type: "response.output_text.delta",
      delta: "Hi",
      reasoningDelta: "",
    });
  });

  it("accumulates text across chunked frames", () => {
    const buf = [];
    feedChunk(buf, "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Hel\"}\n\n");
    feedChunk(buf, "data: {\"type\":\"response.output_text.delta\",\"delta\":\"lo\"}\n\n");
    const { events } = drainEvents(buf);
    let text = "";
    for (const evt of events) {
      const { delta } = parseStreamEvent(evt);
      text += delta;
    }
    assert.equal(text, "Hello");
  });

  it("ignores completed response until stream ends", () => {
    const buf = [];
    feedChunk(buf, "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"r1\"}}\n\n");
    const { events } = drainEvents(buf);
    assert.equal(parseStreamEvent(events[0]).type, "response.completed");
    assert.equal(parseStreamEvent(events[0]).delta, "");
  });

  it("accumulates reasoning summary deltas", () => {
    const buf = [];
    feedChunk(buf, 'data: {"type":"response.reasoning_summary_text.delta","delta":"Plan "}\n\n');
    feedChunk(buf, 'data: {"type":"response.reasoning_summary_text.delta","delta":"A"}\n\n');
    const { events } = drainEvents(buf);
    let reasoning = "";
    for (const evt of events) {
      const { reasoningDelta } = parseStreamEvent(evt);
      reasoning += reasoningDelta;
    }
    assert.equal(reasoning, "Plan A");
  });
});

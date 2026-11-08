# Realtime — speech-to-speech sessions

Gated behind `FEATURE_STT && FEATURE_REALTIME`. Covers OpenAI Realtime
(`wss://api.openai.com/v1/realtime`), the PixlWiz/LiteLLM proxy, and the
existing `pm::audio` mic/speaker layer (`src/core/audio.hpp`).

---

## 1. What exists (current)

### Stack

```
┌─ packages/kbot ─────────────────────────────────────────────────────────────┐
│ polymech::kbot::RealtimeSession   (realtime_session.h / .cpp)                │
│   WS protocol layer. No audio devices, no agent knowledge.                  │
│   in:  send_pcm · send_text · finish_input_turn · send_tool_result          │
│   out: on_audio_pcm · on_transcript · on_user_transcript(text, final)       │
│        on_tool_call · on_response_done · on_barge_in · on_state · on_error  │
└───────────────────────────────▲────────────────────────────────────────────┘
                                │
┌─ src/core/audio_realtime.cpp ─┴────────────────────────────────────────────┐
│ pm::audio::RealtimeVoiceSession                                              │
│   Owns mic (AudioInput) + speaker (PcmFifo → ma_device) + RealtimeSession. │
│   Mic  → resample 16k→24k → session.send_pcm                               │
│   session.on_audio_pcm → PcmFifo.push → play_cb → speakers                │
│   play_cb also → render_resampler 24k→16k → mic.process_render_reference   │
│      → EchoCancellationProcessor (WebRTC APM reverse stream)               │
│   PcmFifo: 200 ms prebuf, re-arm on underrun, 300 s runaway cap            │
│   on_barge_in: fifo.clear() → model stops mid-utterance                    │
└───────────────────────────────▲────────────────────────────────────────────┘
                                │
┌─ src/llm/realtime_voice_agent.cpp ┴────────────────────────────────────────┐
│ VoiceAgent                                                                   │
│   Wraps RealtimeVoiceSession + RealtimeAgentBridge + RealtimeTurnRecorder.  │
│   session_id_  — continuity/persistence across turns                        │
│   RealtimeAgentBridge — receives on_tool_call, runs run_pm_agent on worker │
│      thread, feeds result back via voice_->send_tool_result                 │
│   RealtimeTurnRecorder — commits user/assistant pairs to session store      │
│   cross-process IPC singleton (pm::audio::rt_ipc::Owner) — one live        │
│      session per machine, visible to assistant toggle / --realtime-stop     │
└───────────────────────────────▲────────────────────────────────────────────┘
                                │
┌─ src/win/ui_next/ ────────────┴────────────────────────────────────────────┐
│ ChatWebPanel_Agent.cpp  — rt_agent member; HandleAgentStop handles voice-   │
│    delegated run cancel; streams all agent events to JS via EnqueueScript   │
│ ChatWebPanel_Sessions.cpp — chat session CRUD, agent task id mapping        │
└────────────────────────────────────────────────────────────────────────────┘
```

### Session storage (roaming)

Three documents per session in `sessions/resources/documents/`:

| File | Contents |
|------|----------|
| `session__<uuid>.doc` | Flat entry log — all roles incl. tool/shell, integer ids, ms timestamps |
| `session-memory__<task-id>.doc` | Structured memory — turn pairs, instructions, artifacts, security decisions |
| `session-log__<task-id>__journal.doc` | Tool journal — per-tool-call timing, output paths |

The `session__<uuid>` id comes from the chat panel; the `session-<task-id>` is
the agent memory task id minted/rehydrated in `HandleAgentSend`.

### Audio pipeline specifics

- Mic capture: 16 kHz mono s16 (miniaudio `ma_device_type_capture`)
- Realtime model: 24 kHz in/out (resampler on both paths)
- AEC: `EchoCancellationProcessor` (WebRTC APM), created per `AudioInput::start()`
- Reference feed: `play_cb → on_render_pcm → render_resampler → process_render_reference`
  — time-aligned to actual playback, not network arrival
- Playback prebuffer: `kPlaybackPrebufFrames = kRtRate / 5` → **200 ms**

---

## 2. Known issues + next TODOs

### 2a. Ordering — STT final arrives after assistant response (MEDIUM)

**What happens:** The voice model responds and its text is committed to the flat
entry log. The STT final transcript for the *triggering* user utterance arrives
~500–700 ms later. In the log and UI, the assistant response appears before the
user message that caused it. Confirmed in `session__1fa69d8c`:

```
entry 3  assistant  "I can't access the current time..."  ts: …893918
entry 4  user       "What's the time"                     ts: …894508  (+590 ms)
```

Same inversion between entry 7 (agent handover) and entry 8 (user STT final,
+667 ms). Visible in the chat UI as the "I'm ready when you are." / "Bye."
screenshot — response to turn N appears below turn N+1.

**Root cause:** `on_user_transcript(text, final=true)` → `recorder->set_user()`
→ appended to flat log. The voice model response completes and is committed in
`on_transcript` / `on_response_done` before the STT finalizer fires.

**Fix (DONE — display only, no C++/pipeline change):**

The realtime turns never touch the flat-log `appendText` path; they reach the
UI through dedicated hooks (`realtimeTranscriptDelta` for the assistant bubble,
`realtimeUserTranscript(final)` for the user turn) in `pmChatStore.ts`. The
inversion is therefore created *and fixed* entirely in the JS store — no C++
`seq_` counter needed.

- `ChatEntry.seqId?: number` — assigned only to realtime user/assistant
  entries from a monotonic allocator (`realtimeSeq`, step 2). When the assistant
  bubble opens (`onRealtimeTranscriptDelta`) it takes `seqId = N` and reserves
  the slot `N−1` for the pending user turn (`realtimeTurnAsstSeq`). When the
  late user final lands (`onRealtimeUserTranscript`), it slots in at that
  reserved `N−1`; if STT beat the model it takes a fresh seq instead. Non-realtime
  entries never get a `seqId`.
- `orderBySeqId` in `ChatTranscript.tsx` reorders **only** the seqId-carrying
  entries among the slots they occupy, leaving every other entry in its exact
  insertion position; `buildDisplayRows` runs on the reordered array. Regular
  chat is byte-for-byte unaffected.
- `seqId` round-trips through persistence (`chatSessionsBackend`,
  `chatSessionStorage`, `applyLoadedSession`) so restored sessions keep the
  order; `startRealtime` seeds `realtimeSeq` above any restored seqIds.
- Data stays append-only. Roll back by removing the `orderBySeqId` call.

### 2b. Triple assistant entries per agent call (LOW — cosmetic)

Each agent handover produces three sequential assistant entries:

1. `[agent] <full markdown result>` — from `RealtimeAgentBridge` (`K::AssistantText`),
   prefixed `[agent] `, already rendered dimmed in `ChatTranscript.tsx`.
2. `<spoken readback>` — voice model narrates the result back (from `on_transcript`).
   Sometimes truncated for speech (confirmed: `"Screenshot saved to C drive, Users,
   zx, Desktop, pix"` — cuts off mid-path).
3. `Okay.` / `one moment` — unprompted voice model acknowledgment.

The `[agent]` prefix distinction is already in the UI (line 1139 of
`ChatTranscript.tsx`). Entries 2 and 3 are undifferentiated from real assistant
turns.

**Fix (DONE — voice/voice_ack tagging + dim; agent already dimmed by prefix):**

- `ChatEntry.assistantKind?: "voice" | "voice_ack" | "agent"` added.
- Tagged at write time in the JS store (the voice entries never go through
  `RealtimeTurnRecorder`, which is CLI-only session-memory): `onRealtimeTranscriptDelta`
  opens the bubble as `voice`; on turn close `onRealtimeResponseDone` reclassifies
  it to `voice_ack` when the whole turn is a short filler ack (`isVoiceAck` — a
  conservative whole-phrase allowlist: `Okay.`, `one moment`, `let me check`, … ≤20
  chars, so real short answers like "It's 3 PM." are never dimmed).
- `agent` entries keep the existing `[agent] ` prefix detection; the renderer also
  honours an explicit `assistantKind === "agent"`.
- Rendering (`ChatTranscript.tsx`): `voice_ack` → compact italic dimmed line at 60%
  opacity (not a second full answer); `agent` → existing dimmed border-left block.
- `assistantKind` round-trips through persistence like `seqId`.

**Still open:** the spoken-readback *truncation* ("…Desktop, pix" cut off) and
storing the full agent result vs. the spoken summary is folded into §2e
(`voice_transcript` separation), not this cosmetic pass.

### 2c. AEC — three isolated fixes (LOW risk each)

**Fix 1 — Set APM stream delay hint (one call):**

```cpp
// audio.cpp, after EchoCancellationProcessor::create()
if (m_->echo_canceller && m_->echo_canceller->active())
    m_->echo_canceller->set_stream_delay_ms(200); // match kPlaybackPrebufFrames
```

WebRTC APM auto-detects delay if the hint is 0 — setting it to 200 ms gives it
a head-start. If wrong it just falls back to auto. Zero regression risk.

**Fix 2 — Skip reference feed immediately after barge-in:**

```cpp
// audio_realtime.cpp, Impl
std::atomic<bool> skip_render_ref_{false};

// in on_barge_in:
scb.on_barge_in = [impl]() {
    impl->fifo.clear();
    impl->skip_render_ref_.store(true);
};

// in on_render_pcm, top:
if (skip_render_ref_.exchange(false)) return;
```

After `fifo.clear()`, the play_cb outputs silence-padded frames. Feeding those
to the APM reference makes it see a sudden silent reference while room echo is
still decaying → APM under-suppresses for its convergence period. Discarding one
frame fixes the false transition.

**Fix 3 — Duplex device (future, higher risk):**

Mic and speaker are on separate `ma_device` instances → independent clocks →
APM reference/capture streams can drift within a long session. Long-term fix:
open a single `ma_device_type_duplex` so miniaudio runs both streams on the
same callback with sample-accurate alignment. Not now — flag for next audio
pipeline work.

### 2d. `chat.md` duplicate turn blocks (Tanit side)

The Tanit transcript writer re-emits a complete `# Chat turn` block (including
user prompt and all tool activity verbatim) for every assistant message that
arrives within a turn. A single agent handover produces three identical blocks
differing only in the assistant reply timestamp. Fix: track turn IDs; only open
a new block when the turn ID is new; for open turns update in place or flush on
`response.done`.

### 2e. `session-memory` `user` field inconsistency

In `realtime_agent`-mode events, the `user` field stores the **agent handover
instruction** (e.g. `"Ask the system for the current time and report it back."`),
not the actual voice transcript from the user (`"Ask the system."`). `realtime`-mode
events correctly store the voice transcript. When session memory is replayed to
seed a new turn, this discrepancy causes the wrong "user said" context to be
injected. Fix: store the originating voice transcript in a separate
`voice_transcript` field on `realtime_agent` events and use that when replaying.

---

---

## 3. Virtual Inputs

Desktop / system audio ("what the speakers are playing") as an additional or
sole capture source sent to the Realtime API, with per-source gain control.

### VI-1 — `Config` API: input source + gain fields

Add to `RealtimeVoiceSession::Config` (`src/core/audio_realtime.hpp`):

```cpp
enum class InputSource { mic, desktop, mix };

InputSource input_source  = InputSource::mic;
std::string desktop_device;       // substring match; empty = default render loopback
float       mic_gain      = 1.0f; // linear multiplier, applied before mix
float       desktop_gain  = 1.0f;
```

`mic` is the unchanged default path. `desktop` replaces the mic entirely with
the system loopback. `mix` runs both sources in parallel and sums them after
per-source scaling.

### VI-2 — `DesktopAudioCapture` class

New class in `src/core/audio.hpp` / `audio.cpp`, same public contract as
`AudioInput`:

```cpp
class DesktopAudioCapture {
public:
    bool start(const std::string& device_substr,
               std::function<void(const int16_t*, size_t)> on_pcm,
               std::string& err);
    void stop();
    bool active() const;
};
```

Platform implementations:

| Platform | Mechanism | Notes |
|----------|-----------|-------|
| Windows (WASAPI) | `ma_device_type_loopback` with the render device id | Works without a virtual driver; miniaudio exposes this natively. See also `ref/obs/plugins/win-wasapi/wasapi-output.cpp` for the WASAPI `IAudioClient` loopback pattern. |
| Linux (PulseAudio) | `.monitor` suffix source | These already appear in `enumerate_capture_devices`; `DesktopAudioCapture::start` just filters for the `.monitor` entry matching `device_substr`. |
| Linux (PipeWire) | PipeWire loopback node | Presented as a PulseAudio `.monitor` source via the PW PA compatibility layer — same path. |
| macOS | CoreAudio process tap (macOS 14.2+) or BlackHole ≥0.5 | Neither works zero-install on older macOS. `audio info` should surface available tap-capable devices and note when none are present. |

Capture format is identical to `AudioInput`: 16 kHz mono s16. The loopback
device's native rate must be resampled down (render devices typically run at
44.1 kHz or 48 kHz).

### VI-3 — `AudioMixer` utility

Lightweight header-only helper `src/core/audio_mixer.hpp`. Used only in the
`mix` path; zero cost otherwise.

```cpp
// Mix up to N s16 streams with independent gains into a single s16 output.
// Float-domain: s16 → f32 → scale → sum → clip → s16.
// `sources` is a span of { pcm, frames, gain } structs; all must have
// the same frame count for this call.
void mix_s16(std::span<const MixSource> sources, int16_t* out, size_t frames);
```

The mixer accumulates into a `float` buffer per frame, sums contributions,
clamps to `[-1.0f, 1.0f]`, then converts back to s16. Thread safety: the
caller (the session impl) is responsible for aligning callback timing before
calling `mix_s16`.

### VI-4 — Wire into `RealtimeVoiceSession::Impl`

Changes in `src/core/audio_realtime.cpp`:

```
InputSource::mic     (unchanged)
  AudioInput → resample 16k→24k → session.send_pcm

InputSource::desktop
  DesktopAudioCapture → resample native→16k→24k → session.send_pcm
  (no AudioInput started)

InputSource::mix
  AudioInput on_pcm          ──┐
                               ├─ AudioMixer(mic_gain, desktop_gain)
  DesktopAudioCapture on_pcm ──┘
       → resample 16k→24k → session.send_pcm
```

`on_level` fires on the post-mix RMS, not raw mic RMS, so the UI level meter
reflects what is actually being sent to the model.

The mixer expects both callbacks to deliver frames on the same cadence (both
are 16 kHz mono after internal resampling). A small per-source ring buffer
(~20 ms) absorbs timing skew between the two device callbacks before mixing.

### VI-5 — AEC on the desktop path (interactive vs. passive)

The desktop loopback captures whatever the speakers play. In an **interactive**
session that includes the model's own TTS response — routed through the same
speakers — which loops straight back into the session as fake user speech (the
`hello`-loop). The fix is not muting or gating; it is the echo canceller doing
exactly its job. We already produce the model's **bit-exact** output PCM (the
render stream `on_audio_pcm → fifo → play_cb → on_render_pcm`), so we feed that
as the AEC reverse/reference stream and run the loopback capture through
`process_capture`. The APM subtracts the model's voice, leaving only genuinely-
new desktop audio.

Rules (implemented in `audio_realtime.cpp`):

- `InputSource::desktop` — create a dedicated `EchoCancellationProcessor`
  (`Impl::desktop_aec`, 16 kHz mono). Reference = model output via
  `on_render_pcm → desktop_aec->process_render_reference`. Capture =
  `on_desktop_pcm → desktop_aec->process_capture` (in place, before gain). The
  `set_stream_delay_ms(200)` + barge-in `skip_render_ref` behavior applies here
  too.
- `InputSource::mix` — mic branch keeps its own `AudioInput` AEC; the desktop
  branch is also run through `desktop_aec` before it enters the mixer, so the
  model's voice is stripped from both sources.
- `InputSource::mic` — unchanged (AEC on the mic by default).

**Passive exception:** for pure monitoring/transcription where the model never
speaks (e.g. "transcribe what's playing"), the loopback contains no model TTS,
so the desktop AEC is a no-op cost. It is harmless (reference is silent) but can
be skipped if a passive-only flag is ever added.

The earlier assumption — "desktop *is* the render, so any AEC cancels
everything" — only holds when the sole thing playing is the model. In the
interactive round trip the loopback is `fixture/user_audio + model_TTS`; the
reference is only `model_TTS`, so cancellation removes the feedback and keeps
the real input. This is closer to WebRTC's intended use than OBS's
`monitoring_duplicating_source` silence guard.

### VI-6 — CLI flags

Add to `audio record` in `src/cli/pm_image_cmd_audio.cpp`, and to the
`pm image realtime` entry point wherever `RealtimeVoiceSession::Config` is
populated:

```
--input-source  mic|desktop|mix   default: mic
--desktop       <device-substr>   convenience alias — sets --input-source desktop
                                  and uses the given string as desktop_device
--mic-gain      <float>           default: 1.0  (linear; 0 = mute, 2 = double)
--desktop-gain  <float>           default: 1.0
```

`--desktop` without an argument selects the default render loopback (empty
`desktop_device`). Combine with `--input` (existing mic device flag) when
`--input-source mix` is used.

### VI-7 — `audio info` device enumeration

Extend `audio info` output:

- **Windows** — call `ma_context_get_devices` with the loopback flag and list
  returned endpoints tagged `[loopback]`. Typically one entry per render device.
- **Linux** — `.monitor` sources already appear in the capture list; tag them
  `[monitor]` so they are distinguishable from real microphones.
- **macOS** — list CoreAudio devices that advertise the
  `kAudioDevicePropertyTapEnabled` property (14.2+) tagged `[tap]`; list any
  installed BlackHole virtual devices tagged `[virtual]`; if neither category
  is found emit a note: `no desktop capture available without virtual driver`.

`--json` output gains a new `"loopback": true` / `"monitor": true` boolean per
device entry.

### VI-8 — Platform matrix

| Platform | Mechanism | Zero-install? |
|----------|-----------|---------------|
| Windows (WASAPI) | `ma_device_type_loopback` | Yes |
| Linux (PulseAudio) | `.monitor` source | Yes |
| Linux (PipeWire) | PipeWire loopback (PA compat layer) | Yes (PW ≥ 0.3) |
| macOS 14.2+ | CoreAudio process tap | Yes (OS-native) |
| macOS < 14.2 | BlackHole ≥ 0.5 virtual driver | No (driver install required) |

---

---

## 4. Open / deferred

- **Voice selector UI** — settings currently picks router/model; add voice
  (`marin`, `cedar`, `alloy`, …). Config field exists in `VoiceAgent::Options`.
- **Image attachment mid-session** — `RealtimeVoiceSession::attach_image` seam
  exists but not wired to the chat context-attach (drag/paste) flow.
- **Input rate** — mic captures 16 kHz; currently resampled 16k→24k before
  `send_pcm`. Consider setting `session.audio.input.format.rate = 16000` to
  skip the upsample. Revisit when latency is otherwise optimised.
- **Max session 60 min** — no reconnect logic; session silently closes. Add
  `on_closed` → reconnect with same `session_id_` if under the limit.

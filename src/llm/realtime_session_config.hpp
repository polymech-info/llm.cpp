#pragma once
//
// Shared realtime voice-session setup — the ONE place both surfaces (chat web
// panel and CLI/headless `llm agent --realtime`) get their session config from:
//
//   • tools        : RealtimeAgentBridge schemas (run_pm_agent / stop_pm_agent)
//   • instructions : realtime-prompt.md (or the built-in fallback persona),
//                    ${VAR}-rendered against the live selection/folder, plus
//                    the live-context note ("you cannot see files — delegate
//                    to run_pm_agent", current selection/folder summary)
//   • voice        : explicit override → chat settings realtime_voice
//   • devices      : Audio & Video settings (explicit mic override wins)
//
// Surface-specific concerns (provider resolution, JS/stdout sinks, session-id
// mapping) stay at the call sites; everything the two paths must agree on for
// a session to behave and persist identically lives here. Spoken-turn
// persistence is shared separately via RealtimeTurnRecorder.

#if defined(FEATURE_STT) && FEATURE_STT && defined(FEATURE_REALTIME) && FEATURE_REALTIME

#include "core/audio_realtime.hpp"
#include "polymech_export.h"

#include <string>
#include <vector>

namespace media::llm::agent::realtime {

struct SetupOptions {
    std::vector<std::string> selection;             // live selection (utf-8 paths)
    std::string              folder_hint;           // current folder (utf-8)
    std::string              instructions_override; // explicit persona wins over realtime-prompt.md
    std::string              voice_override;        // explicit voice wins over chat settings
    std::string              mic_override;          // CLI --input style mic preference
    std::string              speaker_override;      // session-only output device override
    // Virtual input overrides (default = mic only, unity gain, back-compat).
    // When false, the persisted Audio & Video settings are used.
    bool virtual_input_override = false;
    pm::audio::RealtimeVoiceSession::Config::InputSource input_source
        = pm::audio::RealtimeVoiceSession::Config::InputSource::mic;
    std::string desktop_device; // loopback device substring; empty = default render
    float       mic_gain     = 1.0f;
    float       desktop_gain = 1.0f;
};

/// Fill cfg.tools / cfg.instructions / cfg.voice / cfg.mic_device /
/// cfg.speaker_device. base_url / api_key / model stay with the caller.
PM_CHAT_API void apply_session_defaults(pm::audio::RealtimeVoiceSession::Config& cfg,
                                         const SetupOptions& opt);

} // namespace media::llm::agent::realtime

#endif // FEATURE_STT && FEATURE_REALTIME

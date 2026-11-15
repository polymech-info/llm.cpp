#pragma once
//
// TranslationSession — OpenAI Realtime Translation WebSocket protocol.
//
// Separate from RealtimeSession (voice agent). Connects to
// /v1/realtime/translations, streams continuous PCM in, and receives translated
// PCM + transcript deltas. No tools, turns, or response.create.
//
//   in:   send_pcm() · update_language() · finish() · close()
//   out:  on_audio_pcm · on_output_transcript · on_input_transcript · on_state …
//
// Thread model matches RealtimeSession: one reader thread; curl WS I/O under one
// mutex so send_pcm() is safe from an audio-capture thread.
//
// Requires FEATURE_REALTIME=1; when disabled the methods are no-op stubs.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "kbot.h"

namespace polymech {
namespace kbot {

class POLYMECH_API TranslationSession {
public:
    struct Config {
        std::string base_url;   // https://api.openai.com/v1 OR litellm proxy base
        std::string api_key;    // Bearer token
        std::string model;      // gpt-realtime-translate / tanit-realtime-translate
        std::string language;   // target BCP-47 / ISO code, e.g. "es", "fr", "de"
        // Input noise reduction: "near_field", "far_field", or empty for none.
        std::string noise_reduction = "near_field";
        // Optional input transcription model (e.g. "gpt-realtime-whisper"); empty = server default.
        std::string transcription_model;
        int input_rate  = 24000;
        int output_rate = 24000;
        int connect_timeout_ms     = 15000;
        int close_drain_timeout_ms = 30000; // finish() waits for session.closed
    };

    struct Callbacks {
        // Translated audio (PCM16 mono @ Config::output_rate). Reader thread.
        std::function<void(const int16_t* pcm, size_t frame_count)> on_audio_pcm;
        // Target-language transcript deltas (session.output_transcript.delta).
        std::function<void(const std::string& delta)> on_output_transcript;
        // Source-language transcript deltas (session.input_transcript.delta).
        std::function<void(const std::string& delta)> on_input_transcript;
        // true after session.updated; false on close.
        std::function<void(bool connected)> on_state;
        std::function<void(const std::string& message)> on_error;
        std::function<void(const std::string& message)> on_auth_error;
        std::function<void(const std::string& message)> on_network_error;
        // Fired when session.closed is received (finish drain completed).
        std::function<void()> on_session_closed;
    };

    TranslationSession();
    ~TranslationSession();
    TranslationSession(const TranslationSession&) = delete;
    TranslationSession& operator=(const TranslationSession&) = delete;

    // Connect, send session.update (language), spawn reader. False on connect failure.
    bool open(const Config& cfg, Callbacks cb, std::string& err);

    // Append PCM16 mono @ Config::input_rate. Safe from an audio thread.
    // Uses session.input_audio_buffer.append (translation wire shape).
    void send_pcm(const int16_t* pcm, size_t frame_count);

    // Mid-session language change (session.update). No-op if closed.
    void update_language(const std::string& language);

    // Send session.close, wait for session.closed (or timeout), then tear down.
    // Prefer this over close() when the source stream ends so trailing output is kept.
    bool finish(std::string& err);

    // Hard teardown (no session.close drain). Idempotent, any-thread.
    void close();

    bool is_open() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_;
};

} // namespace kbot
} // namespace polymech

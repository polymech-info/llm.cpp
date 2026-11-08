#pragma once
//
// RealtimeSession — a persistent, bidirectional OpenAI Realtime WebSocket.
//
// Pure protocol layer: it speaks the Realtime wire format (session.update,
// input_audio_buffer.append, response.* parsing) and knows NOTHING about audio
// devices. Audio crosses the boundary only as raw PCM16 buffers; everything
// else is JSON-derived high-level callbacks.
//
//   in:   send_pcm() · send_text() · send_image() · commit() · cancel_response()
//   out:  on_audio_pcm · on_text · on_transcript · on_tool_call · on_state · on_error
//
// Thread model: a single reader thread runs the curl_ws_recv loop. All curl
// websocket I/O (send + recv) is serialized behind one mutex, so send_pcm() can
// be called from an audio-capture thread while the reader drains frames.
//
// Requires FEATURE_REALTIME=1; when disabled the methods are no-op stubs.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "kbot.h"

namespace polymech {
namespace kbot {

class POLYMECH_API RealtimeSession {
public:
    struct Config {
        std::string base_url;       // https://api.openai.com/v1 OR litellm proxy base
        std::string api_key;        // Bearer token
        std::string model;          // gpt-4o-realtime-preview / gpt-realtime
        std::string voice;          // marin / cedar / alloy … (empty → server default)
        std::string instructions;   // system prompt
        nlohmann::json tools = nlohmann::json::array(); // realtime tool defs (empty → none)
        bool   audio_output = true; // request the audio modality (vs text-only)
        bool   server_vad   = true; // turn_detection: server_vad
        // Server-VAD tuning. Without acoustic echo cancellation the model's own
        // voice (played on the speakers) can leak into the mic and trip a false
        // barge-in, chopping its reply mid-sentence. A high threshold (the OpenAI
        // realtime-agents sample uses 0.9) strongly suppresses that self-trigger.
        double vad_threshold        = 0.9; // 0..1 sensitivity (higher = less self-trigger)
        int    vad_prefix_padding_ms = 300;
        int    vad_silence_ms        = 500;
        // Input noise reduction: "near_field" (headset/close mic), "far_field"
        // (laptop/room mic), or empty for none. Cuts ambient-noise false VAD
        // triggers that would otherwise interrupt the model mid-sentence.
        std::string noise_reduction = "near_field";
        int    input_rate   = 24000;// sample rate of PCM we send (OpenAI pcm16 == 24k)
        int    output_rate  = 24000;// sample rate of PCM the model returns
        int    connect_timeout_ms = 15000;
    };

    struct Callbacks {
        // Model audio output (PCM16 mono @ Config::output_rate). Called from the reader thread.
        std::function<void(const int16_t* pcm, size_t frame_count)> on_audio_pcm;
        // Incremental assistant text (response.output_text.delta).
        std::function<void(const std::string& delta)> on_text;
        // Incremental transcript of the model's spoken audio (response.output_audio_transcript.delta).
        std::function<void(const std::string& delta)> on_transcript;
        // Transcript of the user's spoken input (conversation.item.input_audio_transcription.*).
        // `final` is false for streaming deltas and true for the completed transcript.
        std::function<void(const std::string& text, bool final)> on_user_transcript;
        // An assistant turn finished (response.done) — close the current bubble.
        std::function<void()> on_response_done;
        // The user started speaking while the model was talking (input_audio_buffer.speech_started).
        // The surface should flush any queued model audio so it stops immediately (barge-in).
        std::function<void()> on_barge_in;
        // A finalized function/tool call requested by the model.
        std::function<void(const std::string& name,
                           const std::string& call_id,
                           const nlohmann::json& args)> on_tool_call;
        // Connection state: true once the socket is open + session.updated, false on close.
        std::function<void(bool connected)> on_state;
        // Any protocol/transport error (also fired right before an unexpected close).
        std::function<void(const std::string& message)> on_error;
        // Fired in addition to on_error when the failure is specifically an
        // authentication error: HTTP 401/403 on connect, or a server-side error
        // event whose code is an auth-related value (e.g. invalid_api_key).
        // Callers can use this to show a "sign in" prompt instead of a generic toast.
        std::function<void(const std::string& message)> on_auth_error;
        // Fired in addition to on_error when the failure is a network connectivity
        // problem: DNS resolution failure, TCP connect refused, SSL handshake failure,
        // or connection timeout. Callers can use this to show a "check your internet
        // connection" prompt instead of a generic toast.
        std::function<void(const std::string& message)> on_network_error;
    };

    RealtimeSession();
    ~RealtimeSession();
    RealtimeSession(const RealtimeSession&) = delete;
    RealtimeSession& operator=(const RealtimeSession&) = delete;

    // Connect, send the initial session.update, and spawn the reader thread.
    // Returns false (and fills err) on connect failure.
    bool open(const Config& cfg, Callbacks cb, std::string& err);

    // Append one chunk of input PCM16 mono (at Config::input_rate) to the
    // server input buffer. Safe to call from an audio thread. No-op if closed.
    void send_pcm(const int16_t* pcm, size_t frame_count);

    // Inject a text user turn and request a response (manual prompt).
    void send_text(const std::string& text);

    // Return the result of a function/tool call the model requested (matches a
    // prior on_tool_call call_id). Emits a `function_call_output` conversation
    // item and then requests a response so the model speaks the outcome.
    // `output_json` is the tool's result serialized as a JSON string.
    void send_tool_result(const std::string& call_id, const std::string& output_json);

    // Attach an image to the conversation (data is raw base64, no data: prefix).
    void send_image(const std::string& base64, const std::string& mime);

    // Manual-VAD helpers (no-ops for the model under server_vad but harmless).
    void commit();           // input_audio_buffer.commit
    void create_response();  // response.create
    void cancel_response();  // response.cancel (barge-in)
    void truncate(const std::string& item_id, int audio_end_ms);

    // Idempotent, any-thread teardown: stop the reader, close the socket.
    void close();

    bool is_open() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_;
};

} // namespace kbot
} // namespace polymech

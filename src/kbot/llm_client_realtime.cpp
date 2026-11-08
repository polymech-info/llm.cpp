// llm_client_realtime.cpp — RealtimeSession: persistent bidirectional Realtime WS.
//
// See realtime_session.h for the layering contract. The entire translation unit
// is compiled to nothing unless FEATURE_REALTIME is enabled.

#include "realtime_session.h"

#if defined(FEATURE_REALTIME) && FEATURE_REALTIME

#include "curl_ca_android.hpp"
#include "logging/logger.h"
#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include <curl/curl.h>
#include <curl/websockets.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace polymech {
namespace kbot {

// ── base64 (self-contained; kbot has no shared helper) ───────────────────────
namespace {

void rt_log(const std::string& msg)
{
    logger::info(msg);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "TanitRealtime", "%s", msg.c_str());
#endif
}

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64_encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kB64[(n >> 18) & 63]);
        out.push_back(kB64[(n >> 12) & 63]);
        out.push_back(kB64[(n >> 6) & 63]);
        out.push_back(kB64[n & 63]);
    }
    if (i < len) {
        uint32_t n = data[i] << 16;
        if (i + 1 < len) n |= data[i + 1] << 8;
        out.push_back(kB64[(n >> 18) & 63]);
        out.push_back(kB64[(n >> 12) & 63]);
        out.push_back((i + 1 < len) ? kB64[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::vector<uint8_t> b64_decode(const std::string& s) {
    std::vector<uint8_t> out;
    out.reserve((s.size() / 4) * 3);
    int buf = 0, bits = 0;
    for (char c : s) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        const int v = b64_val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// Rewrite an http(s) chat base_url into the realtime wss endpoint. Mirrors
// realtime_base_url_for() in llm_client.cpp (kept local to avoid a cross-TU dep).
std::string realtime_ws_url(const std::string& base_url) {
    if (base_url.empty())
        return "wss://api.openai.com/v1/realtime";
    std::string u = base_url;
    const auto scheme_pos = u.find("://");
    if (scheme_pos != std::string::npos) {
        const std::string scheme = u.substr(0, scheme_pos);
        if (scheme == "https") u.replace(0, scheme_pos, "wss");
        else if (scheme == "http") u.replace(0, scheme_pos, "ws");
    }
    while (!u.empty() && u.back() == '/') u.pop_back();
    if (u.size() >= 3 && u.substr(u.size() - 3) == "/v1") {
        const bool is_openai_host = (u.find("api.openai.com") != std::string::npos);
        if (!is_openai_host) u.resize(u.size() - 3);
    }
    if (u.size() < 9 || u.substr(u.size() - 9) != "/realtime")
        u += "/realtime";
    return u;
}

} // namespace

// ── Impl ─────────────────────────────────────────────────────────────────────
struct RealtimeSession::Impl {
    Config    cfg;
    Callbacks cb;

    CURL*               curl = nullptr;
    struct curl_slist*  hdrs = nullptr;
    std::mutex          io_mu;             // serializes all curl_ws_send / curl_ws_recv

    std::thread         reader;
    std::atomic<bool>   open_flag{false};
    std::atomic<bool>   stop_flag{false};
    std::atomic<bool>   connected_notified{false};

    // Telemetry counters (kept for the periodic debug heartbeat).
    std::atomic<uint64_t> pcm_chunks_sent{0};
    std::atomic<uint64_t> pcm_samples_sent{0};
    std::atomic<uint64_t> audio_chunks_recv{0};
    std::atomic<uint64_t> audio_samples_recv{0};
    std::atomic<uint64_t> events_recv{0};

    // Whether the in-flight assistant response emitted any transcript delta. Lets us
    // synthesize a single transcript callback from the *.done event when the server
    // sends only the final transcript (no incremental deltas).
    std::atomic<bool> resp_saw_transcript_delta{false};

    // Active-response tracking. The realtime API allows only ONE response in flight
    // per conversation; sending `response.create` while another is active fails with
    // `conversation_already_has_active_response`. `response_active` is true between
    // response.created and response.done. When a continuation (tool result / text)
    // wants a new response while one is active, we set `pending_create` and fire the
    // deferred response.create on the next response.done instead of racing — far more
    // important behind the pixlwiz/LiteLLM proxy whose extra hop widens the window.
    std::atomic<bool> response_active{false};
    std::atomic<bool> pending_create{false};
    // True between the moment WE send a client `response.create` and the moment the
    // server acks it (response.created) or rejects it (error). Under server VAD the
    // server auto-creates responses for spoken turns; a `conversation_already_has_
    // active_response` error with NO client create in flight is a server-side VAD
    // self-conflict (a second VAD blip during an active response) — replaying it
    // would fabricate a duplicate reply, so we must only defer/replay OUR own creates.
    std::atomic<bool> client_create_inflight{false};
    // While a delegated run_pm_agent task is in flight the voice model should not
    // emit extra "still working" filler responses. `agent_run_inflight` is true
    // between dispatching a run_pm_agent tool call and receiving its tool result;
    // `user_spoke_since_toolcall` gates the exception: a server-VAD response that
    // follows fresh user speech is a genuine interjection (kept), otherwise it's a
    // redundant filler (cancelled).
    std::atomic<bool> agent_run_inflight{false};
    std::atomic<bool> user_spoke_since_toolcall{false};
    // A non-agent tool result that arrives while its response is still open.
    // If that response already spoke, the deferred response.create is a second
    // utterance of the same answer (stop_pm_agent saying "stopped" twice).
    // run_pm_agent uses reason "agent_result" and does not set this.
    std::atomic<bool> pending_drop_if_spoke{false};
    // Reader-thread playback account. A second play start before play done is
    // the duplicate spoken reply (client response.create racing server VAD).
    std::string play_id;
    std::string play_event;
    uint64_t    play_chunks = 0;
    uint64_t    play_samples = 0;
    // One accepted server-VAD reply per user speech_started. A second
    // response.created with no new speech_started is the repeated greeting.
    bool        vad_reply_open = false;
    // Server id whose audio and transcript must not reach the speaker or the chat.
    std::string drop_play_id;

    bool dropping_audio(const nlohmann::json& j) const {
        if (drop_play_id.empty()) return false;
        const std::string id = j.value("response_id", std::string());
        if (!id.empty()) return id == drop_play_id;
        return play_chunks == 0;
    }

    // Send response.create now, or defer it until the active response finishes.
    // `reason` tags the caller purely for log tracing.
    void request_response(const char* reason) {
        if (!open_flag.load()) return;
        const char* why = reason ? reason : "?";
        if (response_active.load()) {
            pending_drop_if_spoke.store(std::strcmp(why, "send_tool_result") == 0);
            pending_create.store(true);
            rt_log(std::string("RealtimeSession response.create deferred reason=") + why
                   + " active=1 id=" + play_id);
            return;
        }
        rt_log(std::string("RealtimeSession response.create send reason=") + why + " active=0");
        client_create_inflight.store(true);
        send_event({{"type", "response.create"}});
    }

    bool send_event(const nlohmann::json& j) {
        if (!curl) return false;
        const std::string wire = j.dump();
        std::lock_guard<std::mutex> lk(io_mu);
        size_t sent = 0;
        const CURLcode rc = curl_ws_send(curl, wire.data(), wire.size(), &sent, 0, CURLWS_TEXT);
        if (rc != CURLE_OK) {
            logger::error(std::string("RealtimeSession: send failed type=")
                          + j.value("type", std::string("?")) + " rc=" + curl_easy_strerror(rc));
            return false;
        }
        // Don't log per-chunk audio appends at all — they fire continuously and drown
        // the log. Other (control) events are low-volume and worth a trimmed debug line.
        const std::string typ = j.value("type", std::string("?"));
        //if (typ != "input_audio_buffer.append")
            //logger::trace("RealtimeSession >> " + wire.substr(0, 600));
        return true;
    }

    void emit_error(const std::string& msg) {
        logger::error("RealtimeSession: " + msg);
        if (cb.on_error) cb.on_error(msg);
    }

    void reader_loop();
    void dispatch_event(const nlohmann::json& j);
};

// ── ctor / dtor ──────────────────────────────────────────────────────────────
RealtimeSession::RealtimeSession() : m_(std::make_unique<Impl>()) {}
RealtimeSession::~RealtimeSession() { close(); }

bool RealtimeSession::is_open() const { return m_->open_flag.load(); }

// ── open ─────────────────────────────────────────────────────────────────────
bool RealtimeSession::open(const Config& cfg, Callbacks cb, std::string& err) {
    m_->cfg = cfg;
    m_->cb  = std::move(cb);

    const std::string base_ws = realtime_ws_url(cfg.base_url);
    const std::string model   = cfg.model.empty() ? "gpt-4o-realtime-preview" : cfg.model;
    const std::string ws_url  = base_ws + "?model=" + model;

    /*
    logger::trace("RealtimeSession.open model=" + model + " url=" + ws_url
                 + " voice=" + (cfg.voice.empty() ? "<default>" : cfg.voice)
                 + " vad=" + (cfg.server_vad ? "server" : "off")
                 + " vad_thresh=" + std::to_string(cfg.vad_threshold)
                 + " nr=" + (cfg.noise_reduction.empty() ? "off" : cfg.noise_reduction)
                 + " in_rate=" + std::to_string(cfg.input_rate)
                 + " out_rate=" + std::to_string(cfg.output_rate)
                 + " tools=" + std::to_string(cfg.tools.is_array() ? cfg.tools.size() : 0));
    */
    m_->curl = curl_easy_init();
    if (!m_->curl) { err = "curl_easy_init failed"; m_->emit_error(err); return false; }

    m_->hdrs = curl_slist_append(m_->hdrs, ("Authorization: Bearer " + cfg.api_key).c_str());
    // NOTE: do NOT send "OpenAI-Beta: realtime=v1" — that header opts into the legacy
    // beta wire shape, which OpenAI now rejects with `beta_api_shape_disabled`. The GA
    // endpoint (/v1/realtime) is selected by simply omitting the header.
    curl_easy_setopt(m_->curl, CURLOPT_URL, ws_url.c_str());
    curl_easy_setopt(m_->curl, CURLOPT_HTTPHEADER, m_->hdrs);
    curl_easy_setopt(m_->curl, CURLOPT_CONNECT_ONLY, 2L); // websocket
    curl_easy_setopt(m_->curl, CURLOPT_NOSIGNAL, 1L);
    apply_android_curl_ca(m_->curl);
    if (cfg.connect_timeout_ms > 0)
        curl_easy_setopt(m_->curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(cfg.connect_timeout_ms));

    const auto t0 = std::chrono::steady_clock::now();
    const CURLcode rc = curl_easy_perform(m_->curl);
    if (rc != CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(m_->curl, CURLINFO_RESPONSE_CODE, &http_code);
        const bool is_auth = (http_code == 401 || http_code == 403);
        const bool is_net  = !is_auth && (
            rc == CURLE_COULDNT_RESOLVE_HOST  ||
            rc == CURLE_COULDNT_RESOLVE_PROXY ||
            rc == CURLE_COULDNT_CONNECT       ||
            rc == CURLE_OPERATION_TIMEDOUT    ||
            rc == CURLE_SSL_CONNECT_ERROR     ||
            rc == CURLE_SEND_ERROR            ||
            rc == CURLE_RECV_ERROR);
        if (is_auth) {
            err = "Realtime connect failed: authentication token expired or invalid"
                  " (HTTP " + std::to_string(http_code) + ") — run `pm-image login` to sign in again";
        } else if (is_net) {
            if (rc == CURLE_COULDNT_RESOLVE_HOST || rc == CURLE_COULDNT_RESOLVE_PROXY)
                err = "Realtime connect failed: DNS lookup failed — check your internet connection";
            else if (rc == CURLE_OPERATION_TIMEDOUT)
                err = "Realtime connect failed: connection timed out — check your internet connection";
            else
                err = std::string("Realtime connect failed: no internet or server unreachable (")
                      + curl_easy_strerror(rc) + ")";
        } else if (http_code > 0) {
            err = "Realtime connect failed: HTTP " + std::to_string(http_code)
                  + " (" + curl_easy_strerror(rc) + ")";
        } else {
            err = std::string("Realtime connect failed: ") + curl_easy_strerror(rc);
        }
        m_->emit_error(err);
        if (is_auth && m_->cb.on_auth_error)   m_->cb.on_auth_error(err);
        if (is_net  && m_->cb.on_network_error) m_->cb.on_network_error(err);
        curl_slist_free_all(m_->hdrs); m_->hdrs = nullptr;
        curl_easy_cleanup(m_->curl);   m_->curl = nullptr;
        return false;
    }
    const auto connect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0).count();
    //logger::trace("RealtimeSession: WS connected in " + std::to_string(connect_ms) + " ms");

    // session.update — GA wire shape (/v1/realtime). Audio config is nested under
    // `audio.input`/`audio.output`, formats are objects ({type,rate}), and the output
    // selection uses `output_modalities` (not the beta `modalities`).
    nlohmann::json session;
    session["type"]  = "realtime";
    session["model"] = model;
    session["output_modalities"] = cfg.audio_output
        ? nlohmann::json::array({"audio"})
        : nlohmann::json::array({"text"});
    if (!cfg.instructions.empty()) session["instructions"] = cfg.instructions;

    nlohmann::json audio_in;
    audio_in["format"] = { {"type", "audio/pcm"}, {"rate", cfg.input_rate > 0 ? cfg.input_rate : 24000} };
    audio_in["turn_detection"] = cfg.server_vad
        ? nlohmann::json({ {"type", "server_vad"},
                           {"threshold", cfg.vad_threshold},
                           {"prefix_padding_ms", cfg.vad_prefix_padding_ms},
                           {"silence_duration_ms", cfg.vad_silence_ms} })
        : nlohmann::json(nullptr);
    audio_in["transcription"] = { {"model", "whisper-1"} };
    if (!cfg.noise_reduction.empty())
        audio_in["noise_reduction"] = { {"type", cfg.noise_reduction} };

    nlohmann::json audio_out;
    audio_out["format"] = { {"type", "audio/pcm"}, {"rate", cfg.output_rate > 0 ? cfg.output_rate : 24000} };
    if (!cfg.voice.empty()) audio_out["voice"] = cfg.voice;

    session["audio"] = { {"input", audio_in}, {"output", audio_out} };

    if (cfg.tools.is_array() && !cfg.tools.empty()) {
        session["tools"]       = cfg.tools;
        session["tool_choice"] = "auto";
    }
    const nlohmann::json upd{ {"type", "session.update"}, {"session", session} };

    m_->open_flag.store(true);
    m_->stop_flag.store(false);
    if (!m_->send_event(upd)) {
        err = "Realtime send failed: session.update";
        m_->open_flag.store(false);
        curl_slist_free_all(m_->hdrs); m_->hdrs = nullptr;
        curl_easy_cleanup(m_->curl);   m_->curl = nullptr;
        return false;
    }

    m_->reader = std::thread([this] { m_->reader_loop(); });
    return true;
}

// ── reader thread ────────────────────────────────────────────────────────────
void RealtimeSession::Impl::reader_loop() {
    //logger::trace("RealtimeSession: reader thread start");
    std::vector<char> tmp(65536);
    std::string frame_buf;
    auto last_hb = std::chrono::steady_clock::now();

    while (!stop_flag.load()) {
        size_t nrecvd = 0;
        const curl_ws_frame* meta = nullptr;
        CURLcode rc;
        {
            std::lock_guard<std::mutex> lk(io_mu);
            rc = curl_ws_recv(curl, tmp.data(), tmp.size(), &nrecvd, &meta);
        }

        if (rc == CURLE_AGAIN) {
            // Heartbeat so a silent session still shows it's alive in the logs.
            const auto now = std::chrono::steady_clock::now();
            if (now - last_hb >= std::chrono::seconds(5)) {
                last_hb = now;
                /*
                logger::trace("RealtimeSession: idle hb  events=" + std::to_string(events_recv.load())
                              + " pcm_sent=" + std::to_string(pcm_chunks_sent.load())
                              + " audio_recv=" + std::to_string(audio_chunks_recv.load()));
                */
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            continue;
        }
        if (rc != CURLE_OK) {
            if (!stop_flag.load()) {
                const std::string recv_err = std::string("recv failed: ") + curl_easy_strerror(rc);
                emit_error(recv_err);
                const bool is_net_drop = (rc == CURLE_OPERATION_TIMEDOUT ||
                                          rc == CURLE_SEND_ERROR         ||
                                          rc == CURLE_RECV_ERROR         ||
                                          rc == CURLE_COULDNT_CONNECT);
                if (is_net_drop && cb.on_network_error)
                    cb.on_network_error("Connection lost — check your internet connection");
            }
            break;
        }
        if (meta && (meta->flags & CURLWS_CLOSE)) {
            //logger::trace("RealtimeSession: server sent CLOSE");
            break;
        }

        if (nrecvd > 0) frame_buf.append(tmp.data(), nrecvd);
        if (meta && meta->bytesleft == 0 && !frame_buf.empty()) {
            const auto j = nlohmann::json::parse(frame_buf, nullptr, false);
            if (j.is_object()) dispatch_event(j);
            else logger::warn("RealtimeSession: non-object frame (" + std::to_string(frame_buf.size()) + " B)");
            frame_buf.clear();
        }
    }

    open_flag.store(false);
    /*
    logger::trace("RealtimeSession: reader thread exit  events=" + std::to_string(events_recv.load())
                  + " audio_chunks=" + std::to_string(audio_chunks_recv.load())
                  + " audio_samples=" + std::to_string(audio_samples_recv.load()));
                  */
    if (connected_notified.exchange(false) && cb.on_state) cb.on_state(false);
}

// ── event dispatch ───────────────────────────────────────────────────────────
void RealtimeSession::Impl::dispatch_event(const nlohmann::json& j) {
    events_recv.fetch_add(1);
    const std::string typ = j.value("type", std::string());

    // Audio output is the high-volume path — handle first and keep logs terse.
    if (typ == "response.output_audio.delta" || typ == "response.audio.delta") {
        if (dropping_audio(j)) return;
        if (j.contains("delta") && j["delta"].is_string()) {
            const std::vector<uint8_t> bytes = b64_decode(j["delta"].get<std::string>());
            const size_t frames = bytes.size() / 2;
            audio_chunks_recv.fetch_add(1);
            audio_samples_recv.fetch_add(frames);
            if (play_chunks == 0) {
                play_event = typ;
                rt_log("RealtimeSession play start id=" + (play_id.empty() ? std::string("-") : play_id)
                       + " event=" + typ + " frames=" + std::to_string(frames));
            } else if (typ != play_event) {
                rt_log("RealtimeSession play dual id=" + (play_id.empty() ? std::string("-") : play_id)
                       + " event=" + typ + " previous=" + play_event
                       + " chunks=" + std::to_string(play_chunks));
                play_event = typ;
            }
            play_chunks += 1;
            play_samples += frames;
            if (cb.on_audio_pcm && frames > 0)
                cb.on_audio_pcm(reinterpret_cast<const int16_t*>(bytes.data()), frames);
        }
        return;
    }

    if (typ == "response.output_audio_transcript.delta" || typ == "response.audio_transcript.delta") {
        if (dropping_audio(j)) return;
        resp_saw_transcript_delta.store(true);
        if (cb.on_transcript && j.contains("delta") && j["delta"].is_string())
            cb.on_transcript(j["delta"].get<std::string>());
        return;
    }

    // User speech transcription (input audio). Drives the user-side chat bubble.
    if (typ == "conversation.item.input_audio_transcription.delta") {
        if (cb.on_user_transcript && j.contains("delta") && j["delta"].is_string())
            cb.on_user_transcript(j["delta"].get<std::string>(), false);
        return;
    }
    if (typ == "conversation.item.input_audio_transcription.completed") {
        if (cb.on_user_transcript && j.contains("transcript") && j["transcript"].is_string())
            cb.on_user_transcript(j["transcript"].get<std::string>(), true);
        return;
    }

    if (typ == "response.output_text.delta" || typ == "response.text.delta") {
        if (dropping_audio(j)) return;
        if (cb.on_text && j.contains("delta") && j["delta"].is_string())
            cb.on_text(j["delta"].get<std::string>());
        return;
    }

    // Everything else is low-volume — log the (trimmed) payload.
    const std::string dump = j.dump();
    //logger::trace("RealtimeSession << " + typ + "  " + dump.substr(0, 500));

    if (typ == "session.updated") {
        if (!connected_notified.exchange(true) && cb.on_state) cb.on_state(true);
        return;
    }

    if (typ == "error") {
        std::string code;
        if (j.contains("error") && j["error"].is_object())
            code = j["error"].value("code", std::string());
        // Recoverable race: a response.create overlapped an already-active response.
        if (code == "conversation_already_has_active_response") {
            response_active.store(true); // a response IS active per the server
            // Only replay if the rejected create was OURS (a tool result / injected
            // text create that lost the race with an active response). If no client
            // create was in flight, this is a server-VAD self-conflict (the server
            // tried to auto-create a second response for a spurious/short speech blip)
            // — replaying it would fabricate a duplicate reply, so drop it.
            if (client_create_inflight.exchange(false)) {
                logger::warn("RealtimeSession: our response.create raced an active response "
                             "— deferring continuation until response.done (non-fatal)");
                pending_create.store(true);
            } else {
                logger::warn("RealtimeSession: conversation_already_has_active_response with no "
                             "client create in flight — server-VAD self-conflict, ignoring (non-fatal)");
            }
            return;
        }
        const std::string api_err = "API error: " + dump;
        emit_error(api_err);
        // Auth-specific server error codes: fire on_auth_error so the caller
        // can show a "sign in" prompt rather than a generic error toast.
        if (code == "invalid_api_key" || code == "authentication_error"
            || code == "auth_error"   || code == "authorization_error"
            || code == "access_denied") {
            if (cb.on_auth_error) cb.on_auth_error(api_err);
        }
        stop_flag.store(true);
        return;
    }
    if (typ == "response.created") {
        const bool was_ours = client_create_inflight.exchange(false);
        std::string rid;
        if (j.contains("response") && j["response"].is_object())
            rid = j["response"].value("id", std::string());
        // Suppress redundant filler responses while a delegated agent run is in
        // flight — but keep responses that follow fresh user speech (interjections).
        if (!was_ours && vad_reply_open) {
            drop_play_id = rid;
            rt_log("RealtimeSession response.cancel duplicate-vad id=" + (rid.empty() ? std::string("-") : rid));
            if (!response_active.load())
                response_active.store(true);
            send_event({{"type", "response.cancel"}});
            return;
        }
        if (!was_ours && agent_run_inflight.load()) {
            if (user_spoke_since_toolcall.exchange(false)) {
                /*
                logger::trace("RealtimeSession: response.created id=" + rid
                             + " origin=server-vad (user interjection during agent run — keeping)");
                */
            } else {
                /*
                logger::trace("RealtimeSession: suppressing server-VAD filler response id=" + rid
                             + " during agent run (no new user speech) — cancelling");
                */
                rt_log("RealtimeSession response.cancel filler id=" + (rid.empty() ? std::string("-") : rid));
                response_active.store(true);
                resp_saw_transcript_delta.store(false);
                send_event({{"type", "response.cancel"}});
                return;
            }
        } else {
            /*
            logger::trace("RealtimeSession: response.created id=" + rid
                         + " origin=" + (was_ours ? "client" : "server-vad"));
                         */
        }
        if (response_active.load() && play_chunks > 0) {
            rt_log("RealtimeSession play overlap id=" + rid
                   + " previous=" + play_id
                   + " chunks=" + std::to_string(play_chunks)
                   + " samples=" + std::to_string(play_samples)
                   + " origin=" + (was_ours ? "client" : "server-vad"));
        }
        if (!was_ours)
            vad_reply_open = true;
        play_id = rid;
        play_event.clear();
        play_chunks = 0;
        play_samples = 0;
        rt_log(std::string("RealtimeSession response.created id=") + (rid.empty() ? "-" : rid)
               + " origin=" + (was_ours ? "client" : "server-vad"));
        response_active.store(true);
        resp_saw_transcript_delta.store(false); // new assistant turn
        return;
    }
    if (typ == "response.output_audio_transcript.done" || typ == "response.audio_transcript.done") {
        if (dropping_audio(j)) return;
        // If the server streamed no incremental deltas, surface the full transcript once
        // so the assistant bubble still gets its text.
        if (!resp_saw_transcript_delta.load() && cb.on_transcript
            && j.contains("transcript") && j["transcript"].is_string()) {
            cb.on_transcript(j["transcript"].get<std::string>());
            resp_saw_transcript_delta.store(true);
        }
        return;
    }
    if (typ == "response.done") {
        std::string done_id;
        if (j.contains("response") && j["response"].is_object())
            done_id = j["response"].value("id", std::string());
        if (!drop_play_id.empty() && (done_id.empty() || done_id == drop_play_id)) {
            rt_log("RealtimeSession play dropped duplicate-vad id=" + (done_id.empty() ? drop_play_id : done_id));
            drop_play_id.clear();
            if (play_chunks == 0)
                response_active.store(false);
            return;
        }
        if (!resp_saw_transcript_delta.load() && cb.on_transcript
            && j.contains("response") && j["response"].is_object()) {
            std::string spoken;
            const auto& resp = j["response"];
            if (resp.contains("output") && resp["output"].is_array()) {
                for (const auto& item : resp["output"]) {
                    if (!item.is_object())
                        continue;
                    // Item-level transcript and content[].transcript are the same
                    // utterance. Concatenating both prints the reply twice.
                    std::string from_content;
                    if (item.contains("content") && item["content"].is_array()) {
                        for (const auto& part : item["content"]) {
                            if (part.is_object() && part.contains("transcript") && part["transcript"].is_string())
                                from_content += part["transcript"].get<std::string>();
                        }
                    }
                    if (!from_content.empty())
                        spoken += from_content;
                    else if (item.contains("transcript") && item["transcript"].is_string())
                        spoken += item["transcript"].get<std::string>();
                }
            }
            if (!spoken.empty()) {
                cb.on_transcript(spoken);
                resp_saw_transcript_delta.store(true);
            }
        }
        std::string rid;
        if (j.contains("response") && j["response"].is_object())
            rid = j["response"].value("id", std::string());
        const uint64_t spoke_chunks = play_chunks;
        const uint64_t spoke_samples = play_samples;
        rt_log("RealtimeSession play done id=" + (rid.empty() ? play_id : rid)
               + " chunks=" + std::to_string(spoke_chunks)
               + " samples=" + std::to_string(spoke_samples));
        play_chunks = 0;
        play_samples = 0;
        play_event.clear();
        response_active.store(false);
        if (cb.on_response_done) cb.on_response_done();
        // Fire any continuation that was deferred while this response was active.
        // A tool result for a reply that already spoke is the second "stopped".
        if (pending_create.exchange(false)) {
            const bool drop = pending_drop_if_spoke.exchange(false) && spoke_samples > 0;
            if (drop) {
                rt_log("RealtimeSession response.create dropped reason=send_tool_result already-spoke id="
                       + play_id
                       + " chunks=" + std::to_string(spoke_chunks)
                       + " samples=" + std::to_string(spoke_samples));
            } else {
                rt_log("RealtimeSession response.create send reason=deferred active=0 id=" + play_id);
                client_create_inflight.store(true);
                send_event({{"type", "response.create"}});
            }
        } else {
            pending_drop_if_spoke.store(false);
        }
        return;
    }
    if (typ == "input_audio_buffer.speech_started") {
        vad_reply_open = false;
        // Barge-in: with server VAD interrupt_response the model stops generating,
        // but the surface must drop already-queued playback so stale audio doesn't
        // keep talking over the user. Notify it to flush.
        //logger::trace("RealtimeSession: user speech_started (barge-in) — flushing playback");
        user_spoke_since_toolcall.store(true); // a response after this is a real interjection
        if (cb.on_barge_in) cb.on_barge_in();
        return;
    }
    if (typ == "response.function_call_arguments.done") {
        // Finalized tool call: {name, call_id, arguments(string)}.
        const std::string name    = j.value("name", std::string());
        const std::string call_id = j.value("call_id", std::string());
        nlohmann::json args = nlohmann::json::object();
        if (j.contains("arguments") && j["arguments"].is_string()) {
            args = nlohmann::json::parse(j["arguments"].get<std::string>(), nullptr, false);
            if (args.is_discarded()) args = nlohmann::json::object();
        }
        //logger::trace("RealtimeSession: tool_call name=" + name + " call_id=" + call_id);
        // A delegated run_pm_agent kicks off a background task; suppress filler
        // responses until its result comes back (see response.created handler).
        if (name == "run_pm_agent") {
            agent_run_inflight.store(true);
            user_spoke_since_toolcall.store(false);
        }
        if (cb.on_tool_call) cb.on_tool_call(name, call_id, args);
        return;
    }
}

// ── outbound ─────────────────────────────────────────────────────────────────
void RealtimeSession::send_pcm(const int16_t* pcm, size_t frame_count) {
    if (!m_->open_flag.load() || !pcm || frame_count == 0) return;
    const std::string b64 = b64_encode(reinterpret_cast<const uint8_t*>(pcm), frame_count * sizeof(int16_t));
    const nlohmann::json ev{ {"type", "input_audio_buffer.append"}, {"audio", b64} };
    if (m_->send_event(ev)) {
        m_->pcm_chunks_sent.fetch_add(1);
        m_->pcm_samples_sent.fetch_add(frame_count);
    }
}

void RealtimeSession::send_text(const std::string& text) {
    if (!m_->open_flag.load()) return;
    //logger::trace("RealtimeSession.send_text: " + text.substr(0, 200));
    const nlohmann::json item{
        {"type", "conversation.item.create"},
        {"item", {
            {"type", "message"}, {"role", "user"},
            {"content", nlohmann::json::array({ {{"type", "input_text"}, {"text", text}} })}
        }}
    };
    m_->send_event(item);
    m_->request_response("send_text");
}

void RealtimeSession::send_tool_result(const std::string& call_id, const std::string& output_json) {
    if (!m_->open_flag.load() || call_id.empty()) return;
    //logger::trace("RealtimeSession.send_tool_result call_id=" + call_id
    //             + " bytes=" + std::to_string(output_json.size()));
    // The delegated task finished — resume normal (server-VAD) responses.
    // run_pm_agent still asks for a spoken follow-up. A reply that already
    // spoke (stop_pm_agent) does not: that create is the second utterance.
    const bool agent_result = m_->agent_run_inflight.exchange(false);
    const nlohmann::json item{
        {"type", "conversation.item.create"},
        {"item", {
            {"type", "function_call_output"},
            {"call_id", call_id},
            {"output", output_json}
        }}
    };
    m_->send_event(item);
    const auto parsed = nlohmann::json::parse(output_json, nullptr, false);
    const bool suspended = parsed.is_object()
        && parsed.value("status", std::string()) == "suspended";
    if (suspended) {
        m_->pending_create.store(false);
        m_->pending_drop_if_spoke.store(false);
        rt_log("RealtimeSession response.create skipped reason=suspend");
        return;
    }
    m_->request_response(agent_result ? "agent_result" : "send_tool_result");
}

void RealtimeSession::send_image(const std::string& base64, const std::string& mime) {
    if (!m_->open_flag.load()) return;
    //logger::trace("RealtimeSession.send_image mime=" + mime + " bytes=" + std::to_string(base64.size()));
    const std::string url = "data:" + mime + ";base64," + base64;
    const nlohmann::json item{
        {"type", "conversation.item.create"},
        {"item", {
            {"type", "message"}, {"role", "user"},
            {"content", nlohmann::json::array({ {{"type", "input_image"}, {"image_url", url}} })}
        }}
    };
    m_->send_event(item);
}

void RealtimeSession::commit()          { if (m_->open_flag.load()) m_->send_event({{"type", "input_audio_buffer.commit"}}); }
void RealtimeSession::create_response() { m_->request_response("create_response"); }
void RealtimeSession::cancel_response() { if (m_->open_flag.load()) m_->send_event({{"type", "response.cancel"}}); }

void RealtimeSession::truncate(const std::string& item_id, int audio_end_ms) {
    if (!m_->open_flag.load()) return;
    m_->send_event({
        {"type", "conversation.item.truncate"},
        {"item_id", item_id}, {"content_index", 0}, {"audio_end_ms", audio_end_ms}
    });
}

// ── close (idempotent, any-thread) ───────────────────────────────────────────
void RealtimeSession::close() {
    if (m_->stop_flag.exchange(true)) {
        // Already closing — still make sure the thread is joined before returning.
        if (m_->reader.joinable() && std::this_thread::get_id() != m_->reader.get_id())
            m_->reader.join();
        return;
    }
    //logger::trace("RealtimeSession.close");
    m_->open_flag.store(false);

    if (m_->curl) {
        std::lock_guard<std::mutex> lk(m_->io_mu);
        size_t sent = 0;
        curl_ws_send(m_->curl, "", 0, &sent, 0, CURLWS_CLOSE);
    }
    if (m_->reader.joinable() && std::this_thread::get_id() != m_->reader.get_id())
        m_->reader.join();

    if (m_->hdrs) { curl_slist_free_all(m_->hdrs); m_->hdrs = nullptr; }
    if (m_->curl) { curl_easy_cleanup(m_->curl);   m_->curl = nullptr; }
}

} // namespace kbot
} // namespace polymech

#endif // FEATURE_REALTIME

// llm_client_translation.cpp — TranslationSession: /v1/realtime/translations WS.
//
// See translation_session.h. Compiled only when FEATURE_REALTIME is enabled.

#include "translation_session.h"

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
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace polymech {
namespace kbot {

namespace {

void tr_log(const std::string& msg)
{
    logger::info(msg);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "TanitTranslate", "%s", msg.c_str());
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

// Rewrite chat base_url → wss …/v1/realtime/translations.
// Keep /v1 (LiteLLM + OpenAI both expose translations under /v1/…).
std::string translation_ws_url(const std::string& base_url) {
    std::string u = base_url.empty() ? "https://api.openai.com/v1" : base_url;
    const auto scheme_pos = u.find("://");
    if (scheme_pos != std::string::npos) {
        const std::string scheme = u.substr(0, scheme_pos);
        if (scheme == "https") u.replace(0, scheme_pos, "wss");
        else if (scheme == "http") u.replace(0, scheme_pos, "ws");
    }
    while (!u.empty() && u.back() == '/') u.pop_back();

    const std::string suffix = "/realtime/translations";
    if (u.size() >= suffix.size()
        && u.compare(u.size() - suffix.size(), suffix.size(), suffix) == 0)
        return u;

    constexpr const char kRt[] = "/realtime";
    if (u.size() >= sizeof(kRt) - 1
        && u.compare(u.size() - (sizeof(kRt) - 1), sizeof(kRt) - 1, kRt) == 0)
        return u + "/translations";

    if (u.size() < 3 || u.substr(u.size() - 3) != "/v1")
        u += "/v1";
    return u + suffix;
}

nlohmann::json make_session_update(const TranslationSession::Config& cfg) {
    nlohmann::json audio = nlohmann::json::object();
    nlohmann::json output;
    output["language"] = cfg.language;
    if (cfg.output_rate > 0)
        output["format"] = { {"type", "audio/pcm"}, {"rate", cfg.output_rate} };
    audio["output"] = output;

    nlohmann::json input = nlohmann::json::object();
    if (cfg.input_rate > 0)
        input["format"] = { {"type", "audio/pcm"}, {"rate", cfg.input_rate} };
    if (!cfg.noise_reduction.empty())
        input["noise_reduction"] = { {"type", cfg.noise_reduction} };
    if (!cfg.transcription_model.empty())
        input["transcription"] = { {"model", cfg.transcription_model} };
    if (!input.empty())
        audio["input"] = input;

    nlohmann::json session;
    session["audio"] = audio;
    if (!cfg.model.empty())
        session["model"] = cfg.model;
    return { {"type", "session.update"}, {"session", session} };
}

} // namespace

struct TranslationSession::Impl {
    Config    cfg;
    Callbacks cb;

    CURL*              curl = nullptr;
    struct curl_slist* hdrs = nullptr;
    std::mutex         io_mu;

    std::thread       reader;
    std::atomic<bool> open_flag{false};
    std::atomic<bool> stop_flag{false};
    std::atomic<bool> accept_pcm{false};
    std::atomic<bool> connected_notified{false};
    std::atomic<bool> session_closed{false};
    std::atomic<bool> finish_sent{false};

    std::mutex              closed_mu;
    std::condition_variable closed_cv;

    std::atomic<uint64_t> pcm_chunks_sent{0};
    std::atomic<uint64_t> audio_chunks_recv{0};

    bool send_event(const nlohmann::json& j) {
        if (!curl) return false;
        const std::string wire = j.dump();
        std::lock_guard<std::mutex> lk(io_mu);
        size_t sent = 0;
        const CURLcode rc = curl_ws_send(curl, wire.data(), wire.size(), &sent, 0, CURLWS_TEXT);
        if (rc != CURLE_OK) {
            logger::error(std::string("TranslationSession: send failed type=")
                          + j.value("type", std::string("?")) + " rc=" + curl_easy_strerror(rc));
            return false;
        }
        return true;
    }

    void emit_error(const std::string& msg) {
        logger::error("TranslationSession: " + msg);
        if (cb.on_error) cb.on_error(msg);
    }

    void notify_session_closed() {
        session_closed.store(true);
        closed_cv.notify_all();
        if (cb.on_session_closed) cb.on_session_closed();
    }

    void reader_loop();
    void dispatch_event(const nlohmann::json& j);
};

TranslationSession::TranslationSession() : m_(std::make_unique<Impl>()) {}
TranslationSession::~TranslationSession() { close(); }

bool TranslationSession::is_open() const { return m_->open_flag.load(); }

bool TranslationSession::open(const Config& cfg, Callbacks cb, std::string& err) {
    if (cfg.language.empty()) {
        err = "TranslationSession: language is required";
        return false;
    }
    m_->cfg = cfg;
    m_->cb  = std::move(cb);
    m_->session_closed.store(false);
    m_->finish_sent.store(false);
    m_->accept_pcm.store(false);

    const std::string model = cfg.model.empty() ? "gpt-realtime-translate" : cfg.model;
    m_->cfg.model = model;
    const std::string ws_url = translation_ws_url(cfg.base_url) + "?model=" + model;

    m_->curl = curl_easy_init();
    if (!m_->curl) { err = "curl_easy_init failed"; m_->emit_error(err); return false; }

    m_->hdrs = curl_slist_append(m_->hdrs, ("Authorization: Bearer " + cfg.api_key).c_str());
    curl_easy_setopt(m_->curl, CURLOPT_URL, ws_url.c_str());
    curl_easy_setopt(m_->curl, CURLOPT_HTTPHEADER, m_->hdrs);
    curl_easy_setopt(m_->curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(m_->curl, CURLOPT_NOSIGNAL, 1L);
    apply_android_curl_ca(m_->curl);
    if (cfg.connect_timeout_ms > 0)
        curl_easy_setopt(m_->curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(cfg.connect_timeout_ms));

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
            err = "Translation connect failed: authentication token expired or invalid"
                  " (HTTP " + std::to_string(http_code) + ")";
        } else if (is_net) {
            if (rc == CURLE_COULDNT_RESOLVE_HOST || rc == CURLE_COULDNT_RESOLVE_PROXY)
                err = "Translation connect failed: DNS lookup failed — check your internet connection";
            else if (rc == CURLE_OPERATION_TIMEDOUT)
                err = "Translation connect failed: connection timed out — check your internet connection";
            else
                err = std::string("Translation connect failed: no internet or server unreachable (")
                      + curl_easy_strerror(rc) + ")";
        } else if (http_code > 0) {
            err = "Translation connect failed: HTTP " + std::to_string(http_code)
                  + " (" + curl_easy_strerror(rc) + ")";
        } else {
            err = std::string("Translation connect failed: ") + curl_easy_strerror(rc);
        }
        m_->emit_error(err);
        if (is_auth && m_->cb.on_auth_error) m_->cb.on_auth_error(err);
        if (is_net  && m_->cb.on_network_error) m_->cb.on_network_error(err);
        curl_slist_free_all(m_->hdrs); m_->hdrs = nullptr;
        curl_easy_cleanup(m_->curl);   m_->curl = nullptr;
        return false;
    }

    tr_log("TranslationSession.open model=" + model + " lang=" + cfg.language
           + " url=" + translation_ws_url(cfg.base_url));

    m_->open_flag.store(true);
    m_->stop_flag.store(false);
    if (!m_->send_event(make_session_update(m_->cfg))) {
        err = "Translation send failed: session.update";
        m_->open_flag.store(false);
        curl_slist_free_all(m_->hdrs); m_->hdrs = nullptr;
        curl_easy_cleanup(m_->curl);   m_->curl = nullptr;
        return false;
    }

    m_->reader = std::thread([this] { m_->reader_loop(); });
    return true;
}

void TranslationSession::Impl::reader_loop() {
    std::vector<char> tmp(65536);
    std::string frame_buf;

    while (!stop_flag.load()) {
        size_t nrecvd = 0;
        const curl_ws_frame* meta = nullptr;
        CURLcode rc;
        {
            std::lock_guard<std::mutex> lk(io_mu);
            rc = curl_ws_recv(curl, tmp.data(), tmp.size(), &nrecvd, &meta);
        }

        if (rc == CURLE_AGAIN) {
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
        if (meta && (meta->flags & CURLWS_CLOSE))
            break;

        if (nrecvd > 0) frame_buf.append(tmp.data(), nrecvd);
        if (meta && meta->bytesleft == 0 && !frame_buf.empty()) {
            const auto j = nlohmann::json::parse(frame_buf, nullptr, false);
            if (j.is_object()) dispatch_event(j);
            else logger::warn("TranslationSession: non-object frame ("
                              + std::to_string(frame_buf.size()) + " B)");
            frame_buf.clear();
        }
    }

    open_flag.store(false);
    accept_pcm.store(false);
    if (!session_closed.load())
        notify_session_closed();
    if (connected_notified.exchange(false) && cb.on_state) cb.on_state(false);
}

void TranslationSession::Impl::dispatch_event(const nlohmann::json& j) {
    const std::string typ = j.value("type", std::string());

    if (typ == "session.output_audio.delta") {
        if (j.contains("delta") && j["delta"].is_string()) {
            const std::vector<uint8_t> bytes = b64_decode(j["delta"].get<std::string>());
            const size_t frames = bytes.size() / 2;
            audio_chunks_recv.fetch_add(1);
            if (cb.on_audio_pcm && frames > 0)
                cb.on_audio_pcm(reinterpret_cast<const int16_t*>(bytes.data()), frames);
        }
        return;
    }

    if (typ == "session.output_transcript.delta") {
        if (cb.on_output_transcript && j.contains("delta") && j["delta"].is_string())
            cb.on_output_transcript(j["delta"].get<std::string>());
        return;
    }

    if (typ == "session.input_transcript.delta") {
        if (cb.on_input_transcript && j.contains("delta") && j["delta"].is_string())
            cb.on_input_transcript(j["delta"].get<std::string>());
        return;
    }

    if (typ == "session.created")
        return;

    if (typ == "session.updated") {
        accept_pcm.store(true);
        if (!connected_notified.exchange(true) && cb.on_state) cb.on_state(true);
        return;
    }

    if (typ == "session.closed") {
        tr_log("TranslationSession: session.closed");
        accept_pcm.store(false);
        notify_session_closed();
        stop_flag.store(true);
        return;
    }

    if (typ == "error") {
        std::string code;
        if (j.contains("error") && j["error"].is_object())
            code = j["error"].value("code", std::string());
        const std::string dump = j.dump();
        const std::string api_err = "API error: " + dump;
        emit_error(api_err);
        if (code == "invalid_api_key" || code == "authentication_error"
            || code == "auth_error"   || code == "authorization_error"
            || code == "access_denied") {
            if (cb.on_auth_error) cb.on_auth_error(api_err);
        }
        stop_flag.store(true);
        return;
    }
}

void TranslationSession::send_pcm(const int16_t* pcm, size_t frame_count) {
    if (!m_->open_flag.load() || !m_->accept_pcm.load() || !pcm || frame_count == 0) return;
    if (m_->finish_sent.load()) return;
    const std::string b64 = b64_encode(reinterpret_cast<const uint8_t*>(pcm),
                                       frame_count * sizeof(int16_t));
    const nlohmann::json ev{
        {"type", "session.input_audio_buffer.append"},
        {"audio", b64}
    };
    if (m_->send_event(ev))
        m_->pcm_chunks_sent.fetch_add(1);
}

void TranslationSession::update_language(const std::string& language) {
    if (!m_->open_flag.load() || language.empty()) return;
    m_->cfg.language = language;
    m_->send_event(make_session_update(m_->cfg));
}

bool TranslationSession::finish(std::string& err) {
    if (!m_->curl) {
        err = "TranslationSession: not open";
        return false;
    }
    if (m_->finish_sent.exchange(true)) {
        // Already finishing — wait for closed.
    } else if (m_->open_flag.load()) {
        m_->accept_pcm.store(false);
        if (!m_->send_event({{"type", "session.close"}})) {
            err = "TranslationSession: session.close send failed";
            close();
            return false;
        }
        tr_log("TranslationSession: session.close sent — draining");
    }

    const int timeout_ms = m_->cfg.close_drain_timeout_ms > 0
        ? m_->cfg.close_drain_timeout_ms : 30000;
    {
        std::unique_lock<std::mutex> lk(m_->closed_mu);
        const bool ok = m_->closed_cv.wait_for(
            lk, std::chrono::milliseconds(timeout_ms),
            [this] { return m_->session_closed.load() || m_->stop_flag.load(); });
        if (!ok && !m_->session_closed.load()) {
            err = "TranslationSession: timed out waiting for session.closed";
            close();
            return false;
        }
    }
    close();
    return true;
}

void TranslationSession::close() {
    if (m_->stop_flag.exchange(true)) {
        if (m_->reader.joinable() && std::this_thread::get_id() != m_->reader.get_id())
            m_->reader.join();
        return;
    }
    m_->open_flag.store(false);
    m_->accept_pcm.store(false);

    if (m_->curl) {
        std::lock_guard<std::mutex> lk(m_->io_mu);
        size_t sent = 0;
        curl_ws_send(m_->curl, "", 0, &sent, 0, CURLWS_CLOSE);
    }
    if (m_->reader.joinable() && std::this_thread::get_id() != m_->reader.get_id())
        m_->reader.join();

    if (m_->hdrs) { curl_slist_free_all(m_->hdrs); m_->hdrs = nullptr; }
    if (m_->curl) { curl_easy_cleanup(m_->curl);   m_->curl = nullptr; }
    m_->closed_cv.notify_all();
}

} // namespace kbot
} // namespace polymech

#endif // FEATURE_REALTIME

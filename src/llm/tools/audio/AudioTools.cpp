// Copyright PolyMech / pm-image. Internal use only.
#include "llm/tools/audio/AudioTools.hpp"
#if defined(__ANDROID__)
#include "pm_android_host.hpp"
#endif

#include "core/cli_cancel.hpp"
#include "core/settings_runtime.hpp"

#include "logging/pm_log.hpp"
#include "logging/logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <filesystem>
#include <fstream>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

#if defined(FEATURE_STT) && FEATURE_STT
#  include "core/audio.hpp"
#  include "core/mic_activity.hpp"
#  include "audio/audio_constants.hpp"
#  include "audio/tts/elevenlabs_tts.hpp"
#  include "audio/tts/proxy_tts.hpp"
#  include "audio/tts/windows_tts.hpp"
#  include "audio/stt/elevenlabs.hpp"
#  include "audio/stt/proxy_stt.hpp"
#  include "audio/stt/whisper_local.hpp"
#  include "audio/stt/windows_ai_stt.hpp"
#  include <atomic>
#  include <condition_variable>
#  include <exception>
#  include <memory>
#  include <mutex>
#  include <thread>
#  include <vector>
#  if defined(FEATURE_VIBEVOICE) && FEATURE_VIBEVOICE
#    include "audio/tts/vibevoice_tts.hpp"
#  endif
#endif

namespace media::llm::path::detail {

#if defined(FEATURE_STT) && FEATURE_STT
// Pointer to the active TTS AudioOutput on the current TTS thread. Written by
// do_speak / play_tts_cue_blocking on their own thread; read by
// abort_active_speak_impl() from any thread (CLI cancel listener).
static std::atomic<pm::audio::AudioOutput*> g_tts_out{nullptr};

// Prefer streaming (low TTFB); fall back to buffered download + play_sync when
// the ring decoder cannot init (non-MP3 payload, API error body, partial stream).
static bool speak_uses_proxy(const std::string& provider)
{
    return pm::audio::constants::is_speech_proxy_provider(provider);
}

static std::string speak_proxy_model(const std::string& provider, const std::string& model)
{
    const bool placeholder = model.empty() || model == "eleven_v3" || model == "tanit-speech";
    if (provider == "openai" && placeholder) return pm::audio::constants::k_openai_tts_model;
    if (provider == "openrouter" && placeholder) return pm::audio::constants::k_openrouter_tts_model;
    if (!model.empty()) return model;
    if (provider == "tanit") return "tanit-speech";
    return model;
}

static std::string speak_proxy_voice(const std::string& provider, const std::string& voice)
{
    return pm::audio::constants::speech_tts_voice(provider, voice);
}

static void speak_play_with_stream_fallback(
    const std::string&                 text,
    const std::string&                 provider,
    const std::string&                 base_url,
    const std::string&                 api_key,
    const std::string&                 eff_model,
    const std::string&                 voice_id,
    const pm::tts::ElevenLabsTTSConfig& el_cfg,
    pm::audio::AudioOutput&            tls_out)
{
    auto run_stream = [&]() {
        pm::audio::AudioRingBuffer ring;
        std::exception_ptr         dl_exc;
        std::thread dl_thread([&]() noexcept {
            try {
                if (speak_uses_proxy(provider))
                    pm::tts::proxy_tts_synthesize_stream(
                        text, base_url, api_key, eff_model, voice_id, ring, "mp3");
                else
                    pm::tts::elevenlabs_tts_synthesize_stream(text, el_cfg, ring);
            } catch (...) {
                dl_exc = std::current_exception();
            }
            ring.mark_done();
        });
        g_tts_out.store(&tls_out, std::memory_order_release);
        try {
            tls_out.play_ring(ring, pm::audio::constants::k_tts_ring_encoding_mp3);
        } catch (...) {
            g_tts_out.store(nullptr, std::memory_order_release);
            ring.cancel();
            dl_thread.join();
            if (dl_exc) std::rethrow_exception(dl_exc);
            throw;
        }
        g_tts_out.store(nullptr, std::memory_order_release);
        ring.cancel();
        dl_thread.join();
        if (dl_exc) std::rethrow_exception(dl_exc);
    };

    auto run_buffered = [&]() {
        std::vector<uint8_t> audio;
        if (speak_uses_proxy(provider)) {
            audio = pm::tts::proxy_tts_synthesize(
                text, base_url, api_key, eff_model, voice_id, "mp3");
        } else {
            audio = pm::tts::elevenlabs_tts_synthesize(text, el_cfg);
        }
        if (audio.empty())
            throw std::runtime_error("TTS provider returned empty audio");
        g_tts_out.store(&tls_out, std::memory_order_release);
        try {
            tls_out.play_sync(audio.data(), audio.size());
        } catch (...) {
            g_tts_out.store(nullptr, std::memory_order_release);
            throw;
        }
        g_tts_out.store(nullptr, std::memory_order_release);
    };

    try {
        run_stream();
    } catch (const std::exception& ex) {
        logger::warn(std::string("[speak] streaming playback failed, buffered fallback: ")
                       + ex.what());
        run_buffered();
    }
}

// Shared TTS-prompt helper: plays a short cue synchronously before recording.
// Mirrors do_speak's streaming path (curl → ring → play_ring) so the audio
// arrives in ~500 ms TTFB. Returns true on success (or skipped — empty text).
static bool play_tts_cue_blocking(const std::string& text,
                                  const std::string& tts_provider,
                                  const std::string& tts_model,
                                  const std::string& tts_voice_id,
                                  const std::string& tts_api_key,
                                  const std::string& tts_base_url,
                                  std::string&       err)
{
    err.clear();
    if (text.empty()) return true;
    if (pm::audio::constants::is_windows_tts_provider(tts_provider)) {
        try {
            const auto wav = pm::tts::windows_tts_synthesize(text, tts_voice_id);
            static thread_local pm::audio::AudioOutput tls_out;
            g_tts_out.store(&tls_out, std::memory_order_release);
            tls_out.play_sync(wav.data(), wav.size());
            g_tts_out.store(nullptr, std::memory_order_release);
            return true;
        } catch (const std::exception& ex) {
            g_tts_out.store(nullptr, std::memory_order_release);
            err = ex.what();
            return false;
        }
    }
    if (tts_provider.empty() || tts_api_key.empty()) {
        err = "no TTS provider/key configured for `prompt` cue";
        return false;
    }
    try {
        pm::tts::ElevenLabsTTSConfig el_cfg;
        const std::string eff_model = speak_proxy_model(tts_provider, tts_model);
        const std::string eff_voice = speak_proxy_voice(tts_provider, tts_voice_id);
        if (!speak_uses_proxy(tts_provider)) {
            el_cfg.api_key = tts_api_key;
            if (!tts_model.empty())    el_cfg.model_id = tts_model;
            if (!tts_voice_id.empty()) el_cfg.voice_id  = tts_voice_id;
        }
        static thread_local pm::audio::AudioOutput tls_out;
        speak_play_with_stream_fallback(
            text, tts_provider, tts_base_url, tts_api_key, eff_model, eff_voice, el_cfg, tls_out);
        return true;
    } catch (const std::exception& ex) {
        err = ex.what();
        return false;
    }
}

static std::string audio_tool_read_str(const nlohmann::json& args, const char* key)
{
    if (auto s = get_nonempty_string(args, key)) return *s;
    if (auto s = get_nonempty_string(options_or_empty(args), key)) return *s;
    return {};
}

struct AudioToolSttSelection {
    std::string provider;
    std::string model;
    std::string api_key;
    std::string base_url;
    bool        provider_explicit = false;
};

static ExecuteResult audio_tool_resolve_stt(const nlohmann::json& args,
                                            const char*           tool_name,
                                            bool                  prefer_local_whisper,
                                            AudioToolSttSelection& out)
{
    const std::string provider_arg = audio_tool_read_str(args, "provider");
    const std::string model_arg    = audio_tool_read_str(args, "model");
    const std::string api_key_arg  = audio_tool_read_str(args, "api_key");

    media::runtime_settings::ChatProviderSettings chat;
    {
        std::string err;
        if (!media::runtime_settings::load_chat_provider(chat, err))
            return fatal(std::string(tool_name) + ": could not load chat settings: " + err);
    }

    out.provider_explicit = !provider_arg.empty();
    if (out.provider_explicit) {
        out.provider = provider_arg;
    } else if (prefer_local_whisper) {
#if defined(FEATURE_WHISPER) && FEATURE_WHISPER
        out.provider = "whisper";
#else
        out.provider = "elevenlabs";
#endif
    } else {
        out.provider = chat.stt_provider;
    }

    if (out.provider.empty())
        return fatal(std::string(tool_name) + ": no STT provider configured");
    if (out.provider != "tanit" && out.provider != "openrouter" && out.provider != "openai"
        && out.provider != "elevenlabs" && out.provider != "whisper"
        && !pm::audio::constants::is_windows_ai_stt_provider(out.provider))
        return fatal(std::string(tool_name) + ": unsupported provider '" + out.provider
                     + "' (expected tanit | openai | openrouter | elevenlabs | whisper | windows)");

    const bool stt_is_local = (out.provider == "whisper")
        || pm::audio::constants::is_windows_ai_stt_provider(out.provider);
    if (!stt_is_local) {
        std::string ak, bu;
        media::runtime_settings::merge_provider_credentials(out.provider, false, ak, bu);
        pm::audio::constants::ensure_speech_base(out.provider, bu);
        out.api_key  = api_key_arg.empty() ? ak : api_key_arg;
        out.base_url = bu;
        if (out.api_key.empty())
            return fatal(std::string(tool_name) + ": no API key for provider '" + out.provider
                         + "' — configure it in App Settings → API Providers");
    }

    if (!model_arg.empty()) {
        out.model = model_arg;
    } else if (!prefer_local_whisper) {
        const bool saved_model_matches =
            !chat.stt_model.empty() && (chat.stt_provider.empty() || chat.stt_provider == out.provider);
        out.model = saved_model_matches
            ? chat.stt_model
            : (stt_is_local ? std::string("base.en")
               : (out.provider == "openai"
                      ? std::string(pm::audio::constants::k_openai_stt_model)
                      : (out.provider == "openrouter"
                             ? std::string(pm::audio::constants::k_openrouter_stt_model)
                             : std::string("tanit-speech-to-text"))));
    } else if (out.provider == "whisper") {
        out.model = (chat.stt_provider == "whisper" && !chat.stt_model.empty()) ? chat.stt_model : "base.en";
    } else if (out.provider == "tanit") {
        out.model = "tanit-speech-to-text";
    } else {
        out.model = (chat.stt_provider == "elevenlabs" && !chat.stt_model.empty()) ? chat.stt_model : "scribe_v2_realtime";
    }

    ExecuteResult ok;
    ok.ok = true;
    return ok;
}

static uint32_t audio_tool_rd_u32(const unsigned char* p)
{
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

static uint16_t audio_tool_rd_u16(const unsigned char* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

static std::vector<int16_t> audio_tool_read_wav_as_pcm16k(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("could not open WAV: " + path);
    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.size() < 44 || std::memcmp(buf.data(), "RIFF", 4) != 0 || std::memcmp(buf.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error("not a RIFF/WAVE file: " + path);

    uint16_t fmt = 0, channels = 0, bits = 0;
    uint32_t rate = 0, data_len = 0;
    const unsigned char* data_ptr = nullptr;

    size_t pos = 12;
    while (pos + 8 <= buf.size()) {
        const unsigned char* ck = buf.data() + pos;
        const uint32_t ck_size = audio_tool_rd_u32(ck + 4);
        if (pos + 8 + ck_size > buf.size()) break;
        const unsigned char* body = ck + 8;
        if (std::memcmp(ck, "fmt ", 4) == 0 && ck_size >= 16) {
            fmt      = audio_tool_rd_u16(body + 0);
            channels = audio_tool_rd_u16(body + 2);
            rate     = audio_tool_rd_u32(body + 4);
            bits     = audio_tool_rd_u16(body + 14);
        } else if (std::memcmp(ck, "data", 4) == 0) {
            data_ptr = body;
            data_len = ck_size;
        }
        pos += 8 + ck_size + (ck_size & 1u);
    }

    if (!data_ptr || channels == 0 || rate == 0)
        throw std::runtime_error("malformed WAV (missing fmt/data): " + path);

    std::vector<float> mono;
    if (((fmt == 3) || (fmt == 0xFFFE && bits == 32)) && bits == 32) {
        const size_t samples = data_len / 4;
        const size_t frames = samples / channels;
        mono.resize(frames);
        for (size_t i = 0; i < frames; ++i) {
            float acc = 0.0f;
            for (uint16_t c = 0; c < channels; ++c) {
                float sample = 0.0f;
                std::memcpy(&sample, data_ptr + ((i * channels + c) * 4), sizeof(sample));
                acc += sample;
            }
            mono[i] = acc / static_cast<float>(channels);
        }
    } else if ((fmt == 1 || fmt == 0xFFFE) && bits == 16) {
        const size_t samples = data_len / 2;
        const size_t frames = samples / channels;
        mono.resize(frames);
        for (size_t i = 0; i < frames; ++i) {
            int acc = 0;
            for (uint16_t c = 0; c < channels; ++c) {
                int16_t sample = 0;
                std::memcpy(&sample, data_ptr + ((i * channels + c) * 2), sizeof(sample));
                acc += sample;
            }
            mono[i] = (static_cast<float>(acc) / static_cast<float>(channels)) / 32768.0f;
        }
    } else {
        throw std::runtime_error("unsupported WAV sample format (fmt=" + std::to_string(fmt)
                                 + ", bits=" + std::to_string(bits) + ")");
    }

    constexpr uint32_t kTargetRate = 16000;
    std::vector<int16_t> pcm;
    if (mono.empty()) return pcm;

    const double ratio = static_cast<double>(kTargetRate) / static_cast<double>(rate);
    const size_t out_n = rate == kTargetRate ? mono.size() : static_cast<size_t>(mono.size() * ratio);
    pcm.resize(out_n);
    for (size_t i = 0; i < out_n; ++i) {
        float v = 0.0f;
        if (rate == kTargetRate) {
            v = mono[i];
        } else {
            const double src = static_cast<double>(i) / ratio;
            const size_t i0 = static_cast<size_t>(src);
            const size_t i1 = std::min(i0 + 1, mono.size() - 1);
            const float fr = static_cast<float>(src - static_cast<double>(i0));
            v = mono[i0] * (1.0f - fr) + mono[i1] * fr;
        }
        v = std::clamp(v, -1.0f, 1.0f);
        pcm[i] = static_cast<int16_t>(v < 0.0f ? v * 32768.0f : v * 32767.0f);
    }
    return pcm;
}

static std::string audio_tool_run_stt_on_wav(const std::string&            wav_path,
                                             const AudioToolSttSelection&  stt)
{
    if (stt.provider == "whisper") {
#if defined(FEATURE_WHISPER) && FEATURE_WHISPER
        const std::string model_path = pm::stt::whisper_resolve_model_path(stt.model);
        return pm::stt::whisper_local_transcribe(wav_path, model_path);
#else
        throw std::runtime_error("whisper provider not available (built without FEATURE_WHISPER)");
#endif
    }
    if (pm::audio::constants::is_windows_ai_stt_provider(stt.provider))
        return pm::stt::windows_ai_stt_transcribe_file(wav_path);
    if (speak_uses_proxy(stt.provider)) {
        return pm::stt::proxy_stt_transcribe(wav_path, stt.base_url, stt.api_key, stt.model);
    }

    const std::vector<int16_t> pcm = audio_tool_read_wav_as_pcm16k(wav_path);
    if (pcm.empty())
        throw std::runtime_error("WAV contains no audio samples");

    pm::stt::ElevenLabsSTT stt_ws;
    std::mutex              cb_mu;
    std::condition_variable cb_cv;
    std::string             committed;
    std::string             err_msg;
    std::atomic<bool>       done{false};
    stt_ws.on_committed = [&](const std::string& t) {
        std::lock_guard<std::mutex> lk(cb_mu);
        committed = t;
        done.store(true);
        cb_cv.notify_all();
    };
    stt_ws.on_error = [&](const std::string& e) {
        std::lock_guard<std::mutex> lk(cb_mu);
        if (err_msg.empty()) err_msg = e;
        done.store(true);
        cb_cv.notify_all();
    };
    pm::stt::ElevenLabsSTT::Config cfg;
    cfg.api_key     = stt.api_key;
    cfg.sample_rate = 16000;
    if (!stt.model.empty() && stt.model != "tanit-speech-to-text")
        cfg.model_id = stt.model;
    stt_ws.connect(cfg);
    constexpr size_t kChunk = 4000;
    for (size_t i = 0; i < pcm.size(); i += kChunk) {
        const size_t n = std::min(kChunk, pcm.size() - i);
        stt_ws.send_pcm(pcm.data() + i, n);
    }
    stt_ws.commit();
    std::unique_lock<std::mutex> lk(cb_mu);
    cb_cv.wait_for(lk, std::chrono::seconds(15), [&]{ return done.load(); });
    stt_ws.close();
    if (!err_msg.empty() && committed.empty())
        throw std::runtime_error(err_msg);
    return committed;
}

static ExecuteResult audio_tool_transcribe_uploaded_wav(const std::string& wav_path,
                                                        const nlohmann::json& args,
                                                        const char* tool_name)
{
    AudioToolSttSelection stt;
    ExecuteResult resolved = audio_tool_resolve_stt(args, tool_name, /*prefer_local_whisper=*/true, stt);
    if (!resolved.ok) return resolved;

    std::string transcript;
    std::string provider_used = stt.provider;
    std::string model_used = stt.model;
    try {
        transcript = audio_tool_run_stt_on_wav(wav_path, stt);
    } catch (const std::exception& ex) {
        if (stt.provider_explicit || stt.provider != "whisper") {
            return fatal(std::string(tool_name) + ": " + ex.what());
        }
        logger::warn(std::string("[") + tool_name + "] whisper failed, falling back to elevenlabs: " + ex.what());
        nlohmann::json fallback_args = args;
        fallback_args["provider"] = "elevenlabs";
        AudioToolSttSelection fallback;
        ExecuteResult fallback_resolved = audio_tool_resolve_stt(fallback_args, tool_name, /*prefer_local_whisper=*/false, fallback);
        if (!fallback_resolved.ok) return fallback_resolved;
        try {
            transcript = audio_tool_run_stt_on_wav(wav_path, fallback);
            provider_used = fallback.provider;
            model_used = fallback.model;
        } catch (const std::exception& fallback_ex) {
            return fatal(std::string(tool_name) + ": " + fallback_ex.what());
        }
    }

    if (const std::string text_out_raw = audio_tool_read_str(args, "text_out"); !text_out_raw.empty()) {
        const std::string text_out_abs = resolve_tool_path_string(text_out_raw);
        if (const std::string w = llm_path_fs_deny(text_out_abs); !w.empty())
            return fatal(std::string(tool_name) + ": text_out: " + w);
        if (const std::string w = llm_path_write_deny(text_out_abs); !w.empty())
            return fatal(std::string(tool_name) + ": text_out: " + w);
        const fs::path tp(text_out_abs);
        std::error_code ec;
        if (tp.has_parent_path())
            fs::create_directories(tp.parent_path(), ec);
        std::ofstream ofs(tp, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return fatal(std::string(tool_name) + ": could not open text_out for writing: " + text_out_abs);
        ofs << transcript;
        if (transcript.empty() || transcript.back() != '\n') ofs << '\n';
        ofs.close();
        if (!ofs)
            return fatal(std::string(tool_name) + ": write error on text_out: " + text_out_abs);
    }

    logger::info(std::string("[") + tool_name + "] provider=" + provider_used
                 + " chars=" + std::to_string(transcript.size()));

    ExecuteResult r;
    r.ok = true;
    r.envelope = nlohmann::json{
        {"ok",         true},
        {"transcript", transcript},
        {"provider",   provider_used},
        {"model",      model_used},
        {"src",        wav_path},
    };
    return r;
}
#endif // FEATURE_STT

ExecuteResult do_audio_transcribe(const nlohmann::json& args) {
#if !(defined(FEATURE_STT) && FEATURE_STT)
    (void)args;
    return fatal("audio_transcribe: microphone capture requires FEATURE_STT (not compiled in this build)");
#else
    namespace chr = std::chrono;
    using clk = chr::steady_clock;

    auto read_str = [&](const char* key) -> std::string {
        if (auto s = get_nonempty_string(args, key)) return *s;
        if (auto s = get_nonempty_string(options_or_empty(args), key)) return *s;
        return {};
    };

    const std::string prompt       = read_str("prompt");
    const std::string dst_raw      = read_str("dst");
    const std::string text_out_raw = read_str("text_out");
    const std::string provider_arg = read_str("provider");
    const std::string api_key_arg  = read_str("api_key");
    const std::string input_device = read_str("input_device");

    int max_ms = 30000;
    int sil_ms = 1500;
    if (args.contains("max_duration_ms") && args["max_duration_ms"].is_number_integer())
        max_ms = args["max_duration_ms"].get<int>();
    else if (const auto& o = options_or_empty(args);
             o.contains("max_duration_ms") && o["max_duration_ms"].is_number_integer())
        max_ms = o["max_duration_ms"].get<int>();
    if (args.contains("silence_ms") && args["silence_ms"].is_number_integer())
        sil_ms = args["silence_ms"].get<int>();
    else if (const auto& o = options_or_empty(args);
             o.contains("silence_ms") && o["silence_ms"].is_number_integer())
        sil_ms = o["silence_ms"].get<int>();
    if (max_ms < 500)    max_ms = 500;
    if (max_ms > 120000) max_ms = 120000;
    if (sil_ms < 0)      sil_ms = 0;
    if (sil_ms > 30000)  sil_ms = 30000;

    std::string dst_abs, text_out_abs;
    if (!dst_raw.empty()) {
        dst_abs = resolve_tool_path_string(dst_raw);
        if (const std::string w = llm_path_fs_deny(dst_abs);    !w.empty()) return fatal("audio_transcribe: dst: " + w);
        if (const std::string w = llm_path_write_deny(dst_abs); !w.empty()) return fatal("audio_transcribe: dst: " + w);
    }
    if (!text_out_raw.empty()) {
        text_out_abs = resolve_tool_path_string(text_out_raw);
        if (const std::string w = llm_path_fs_deny(text_out_abs);    !w.empty()) return fatal("audio_transcribe: text_out: " + w);
        if (const std::string w = llm_path_write_deny(text_out_abs); !w.empty()) return fatal("audio_transcribe: text_out: " + w);
    }

    media::runtime_settings::ChatProviderSettings chat;
    {
        std::string err;
        if (!media::runtime_settings::load_chat_provider(chat, err))
            return fatal("audio_transcribe: could not load chat settings: " + err);
    }
    const std::string stt_provider = !provider_arg.empty() ? provider_arg : chat.stt_provider;
    if (stt_provider.empty())
        return fatal("audio_transcribe: no STT provider configured — set one in App Settings → Chat → Voice & Audio (or pass `provider`)");
    if (stt_provider != "tanit" && stt_provider != "openrouter"
        && stt_provider != "elevenlabs" && stt_provider != "whisper"
        && !pm::audio::constants::is_windows_ai_stt_provider(stt_provider))
        return fatal("audio_transcribe: unsupported provider '" + stt_provider + "' (expected tanit | openrouter | elevenlabs | whisper | windows)");

    // Whisper and Windows AI speech need no API key.
    const bool stt_is_local = (stt_provider == "whisper")
        || pm::audio::constants::is_windows_ai_stt_provider(stt_provider);

    std::string stt_api_key = api_key_arg;
    std::string stt_base_url;
    if (!stt_is_local) {
        std::string ak, bu;
        media::runtime_settings::merge_provider_credentials(stt_provider, false, ak, bu);
        pm::audio::constants::ensure_speech_base(stt_provider, bu);
        if (stt_api_key.empty()) stt_api_key = ak;
        stt_base_url = bu;
        if (stt_api_key.empty())
            return fatal("audio_transcribe: no API key for provider '" + stt_provider
                         + "' — configure it in App Settings → API Providers");
    }

    // Only inherit chat.stt_model when it belongs to the active provider; if the
    // provider was overridden (e.g. to whisper) don't leak another provider's
    // model id (like ElevenLabs' "scribe_v2_realtime") into it.
    const bool saved_model_matches =
        !chat.stt_model.empty() && (chat.stt_provider.empty() || chat.stt_provider == stt_provider);
    const std::string stt_model = saved_model_matches
        ? chat.stt_model
        : (stt_is_local ? std::string("base.en")
           : (stt_provider == "openai"
                  ? std::string(pm::audio::constants::k_openai_stt_model)
                  : (stt_provider == "openrouter"
                         ? std::string(pm::audio::constants::k_openrouter_stt_model)
                         : std::string("tanit-speech-to-text"))));

    if (!prompt.empty()) {
        const std::string& tts_provider = chat.tts_provider;
        std::string tts_api_key, tts_base_url;
        if (!tts_provider.empty())
            media::runtime_settings::merge_provider_credentials(tts_provider, false, tts_api_key, tts_base_url);
        pm::audio::constants::ensure_speech_base(tts_provider, tts_base_url);
        std::string tts_err;
        if (!play_tts_cue_blocking(prompt, tts_provider, chat.tts_model, chat.tts_voice_id,
                                    tts_api_key, tts_base_url, tts_err))
            logger::warn("[audio_transcribe] TTS cue skipped: " + tts_err);
    }

    constexpr double k_vad_rms_threshold = 200.0;
    std::mutex                  pcm_mu;
    std::vector<int16_t>        pcm;
    std::atomic<bool>           had_speech{false};
    std::atomic<bool>           vad_stop{false};
    std::atomic<clk::time_point> last_speech_tp{clk::now()};

    pm::audio::AudioInput mic;
    pm::audio::MicActivityScope mic_scope; // fires micBusy(true) on first, micBusy(false) on last user
    auto level_last = clk::now() - chr::milliseconds(100);
    try {
        mic.start([&](const int16_t* data, size_t frames) {
            if (frames == 0) return;
            {
                std::lock_guard<std::mutex> lk(pcm_mu);
                pcm.insert(pcm.end(), data, data + frames);
            }
            // Compute RMS once — used for both level notification and VAD.
            double sum = 0.0;
            for (size_t i = 0; i < frames; ++i)
                sum += static_cast<double>(data[i]) * data[i];
            const double rms = std::sqrt(sum / static_cast<double>(frames));
            const auto   now = clk::now();

            // Level notification — throttled to ~80 ms, same cadence as HandleSttStart.
            if (chr::duration_cast<chr::milliseconds>(now - level_last).count() >= 80) {
                level_last = now;
                pm::audio::mic_notify_level(std::min(1.0, rms / 2500.0));
            }

            // VAD.
            if (sil_ms <= 0) return;
            if (rms >= k_vad_rms_threshold) {
                last_speech_tp.store(now);
                had_speech.store(true);
            } else if (had_speech.load()) {
                const auto silent_ms = chr::duration_cast<chr::milliseconds>(
                    now - last_speech_tp.load()).count();
                if (silent_ms >= sil_ms) vad_stop.store(true);
            }
        }, input_device);
    } catch (const std::exception& ex) {
        return fatal(std::string("audio_transcribe: mic.start: ") + ex.what());
    }

    const auto t0 = clk::now();
    const auto deadline = t0 + chr::milliseconds(max_ms);
    const char* stopped_by = "max_duration";
    while (clk::now() < deadline) {
        if (vad_stop.load())             { stopped_by = "vad"; break; }
        if (media::cli::cancel_requested()) { stopped_by = "cancelled"; break; }
        std::this_thread::sleep_for(chr::milliseconds(50));
    }
    mic.stop();
    const auto total_ms = chr::duration_cast<chr::milliseconds>(clk::now() - t0).count();

    std::vector<int16_t> captured;
    { std::lock_guard<std::mutex> lk(pcm_mu); captured.swap(pcm); }
    if (captured.empty())
        return fatal("audio_transcribe: no audio captured (mic produced 0 samples)");

    fs::path wav_path;
    bool wav_is_temp = dst_abs.empty();
    if (!wav_is_temp) {
        wav_path = media::path::path_from_utf8(dst_abs);
        std::error_code ec;
        if (wav_path.has_parent_path())
            fs::create_directories(wav_path.parent_path(), ec);
    } else {
        wav_path = media::runtime_settings::effective_temp_dir();
        const auto tag = clk::now().time_since_epoch().count();
        wav_path /= "pm-audio-transcribe-" + std::to_string(tag) + ".wav";
    }
    if (!pm::stt::write_pcm_wav(media::path::path_to_utf8(wav_path),
                                 captured.data(), captured.size(), /*sample_rate=*/16000))
        return fatal("audio_transcribe: failed to write WAV: " + media::path::path_to_utf8(wav_path));

    std::string transcript;
    try {
        if (stt_provider == "whisper") {
#if defined(FEATURE_WHISPER) && FEATURE_WHISPER
            const std::string model_path = pm::stt::whisper_resolve_model_path(stt_model);
            transcript = pm::stt::whisper_local_transcribe(media::path::path_to_utf8(wav_path), model_path);
#else
            throw std::runtime_error("whisper provider not available (built without FEATURE_WHISPER)");
#endif
        } else if (pm::audio::constants::is_windows_ai_stt_provider(stt_provider)) {
            transcript = pm::stt::windows_ai_stt_transcribe_file(media::path::path_to_utf8(wav_path));
        } else if (pm::audio::constants::is_speech_proxy_provider(stt_provider)) {
            transcript = pm::stt::proxy_stt_transcribe(
                media::path::path_to_utf8(wav_path), stt_base_url, stt_api_key, stt_model);
        } else {
            pm::stt::ElevenLabsSTT stt;
            std::mutex                  cb_mu;
            std::condition_variable     cb_cv;
            std::string                 committed;
            std::string                 err_msg;
            std::atomic<bool>           done{false};
            stt.on_committed = [&](const std::string& t) {
                std::lock_guard<std::mutex> lk(cb_mu);
                committed = t;
                done.store(true);
                cb_cv.notify_all();
            };
            stt.on_error = [&](const std::string& e) {
                std::lock_guard<std::mutex> lk(cb_mu);
                if (err_msg.empty()) err_msg = e;
                done.store(true);
                cb_cv.notify_all();
            };
            pm::stt::ElevenLabsSTT::Config scfg;
            scfg.api_key     = stt_api_key;
            scfg.sample_rate = 16000;
            if (!stt_model.empty() && stt_model != "tanit-speech-to-text")
                scfg.model_id = stt_model;
            stt.connect(scfg);
            constexpr size_t kChunk = 4000;
            for (size_t i = 0; i < captured.size(); i += kChunk) {
                const size_t n = std::min(kChunk, captured.size() - i);
                stt.send_pcm(captured.data() + i, n);
            }
            stt.commit();
            std::unique_lock<std::mutex> lk(cb_mu);
            cb_cv.wait_for(lk, chr::seconds(15), [&]{ return done.load(); });
            stt.close();
            if (!err_msg.empty() && committed.empty())
                throw std::runtime_error(err_msg);
            transcript = committed;
        }
    } catch (const std::exception& ex) {
        if (wav_is_temp) { std::error_code ec; fs::remove(wav_path, ec); }
        return fatal(std::string("audio_transcribe: ") + ex.what());
    }

    if (wav_is_temp) { std::error_code ec; fs::remove(wav_path, ec); }

    if (!text_out_abs.empty()) {
        const fs::path tp(text_out_abs);
        std::error_code ec;
        if (tp.has_parent_path())
            fs::create_directories(tp.parent_path(), ec);
        std::ofstream ofs(tp, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return fatal("audio_transcribe: could not open text_out for writing: " + text_out_abs);
        ofs << transcript;
        if (transcript.empty() || transcript.back() != '\n') ofs << '\n';
        ofs.close();
        if (!ofs)
            return fatal("audio_transcribe: write error on text_out: " + text_out_abs);
    }

    logger::info("[audio_transcribe] provider=" + stt_provider
                 + " duration=" + std::to_string(total_ms) + "ms"
                 + " samples=" + std::to_string(captured.size())
                 + " stopped_by=" + stopped_by
                 + " chars=" + std::to_string(transcript.size()));

    ExecuteResult r;
    r.ok       = true;
    r.envelope = nlohmann::json{
        {"ok",          true},
        {"transcript",  transcript},
        {"provider",    stt_provider},
        {"model",       stt_model},
        {"duration_ms", static_cast<uint64_t>(total_ms)},
        {"samples",     static_cast<uint64_t>(captured.size())},
        {"stopped_by",  stopped_by},
    };
    if (!dst_abs.empty())      r.envelope["dst"]      = dst_abs;
    if (!text_out_abs.empty()) r.envelope["text_out"] = text_out_abs;
    return r;
#endif
}

ExecuteResult do_audio_transcribe_file(const std::string& audio_path, const nlohmann::json& args) {
#if !(defined(FEATURE_STT) && FEATURE_STT)
    (void)audio_path;
    (void)args;
    return fatal("audio_transcribe_file: speech-to-text requires FEATURE_STT (not compiled in this build)");
#else
    if (audio_path.empty())
        return fatal("audio_transcribe_file: audio_path is required");
    return audio_tool_transcribe_uploaded_wav(audio_path, args, "audio_transcribe_file");
#endif
}

ExecuteResult do_audio_transcribe_buffer(const std::vector<unsigned char>& audio_bytes,
                                         const std::string& extension,
                                         const nlohmann::json& args) {
#if !(defined(FEATURE_STT) && FEATURE_STT)
    (void)audio_bytes;
    (void)extension;
    (void)args;
    return fatal("audio_transcribe_buffer: speech-to-text requires FEATURE_STT (not compiled in this build)");
#else
    if (audio_bytes.empty())
        return fatal("audio_transcribe_buffer: audio buffer is empty");

    std::string ext = extension.empty() ? ".wav" : extension;
    ext = media::strutil::lower_ascii(std::move(ext));
    if (ext.front() != '.')
        ext.insert(ext.begin(), '.');
    if (ext != ".wav")
        return fatal("audio_transcribe_buffer: only WAV uploads are supported for now");

    fs::path tmp_dir = media::runtime_settings::effective_temp_dir();
    const auto tag = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path tmp = tmp_dir / ("pm-audio-upload-" + std::to_string(tag) + ext);

    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return fatal("audio_transcribe_buffer: could not open temp WAV for writing: " + media::path::path_to_utf8(tmp));
        ofs.write(reinterpret_cast<const char*>(audio_bytes.data()), static_cast<std::streamsize>(audio_bytes.size()));
        ofs.close();
        if (!ofs)
            return fatal("audio_transcribe_buffer: write error on temp WAV: " + media::path::path_to_utf8(tmp));
    }

    ExecuteResult r = audio_tool_transcribe_uploaded_wav(media::path::path_to_utf8(tmp), args, "audio_transcribe_buffer");
    std::error_code ec;
    fs::remove(tmp, ec);
    if (r.ok && r.envelope.is_object()) {
        r.envelope["bytes"] = static_cast<uint64_t>(audio_bytes.size());
        r.envelope.erase("src");
    }
    return r;
#endif
}

SpeakAudioResult do_speak_audio_buffer(const std::string& text,
                                       const std::string& extension,
                                       const nlohmann::json& args) {
    SpeakAudioResult out;
#if !(defined(FEATURE_STT) && FEATURE_STT)
    (void)text;
    (void)extension;
    (void)args;
    out.error = "speak_audio: audio synthesis requires FEATURE_STT (not compiled in this build)";
    return out;
#else
    if (text.empty()) {
        out.error = "speak_audio: text is required";
        return out;
    }

    std::string ext = extension.empty() ? ".mp3" : extension;
    ext = media::strutil::lower_ascii(std::move(ext));
    if (ext.front() != '.')
        ext.insert(ext.begin(), '.');

    media::runtime_settings::ChatProviderSettings chat;
    {
        std::string err;
        if (!media::runtime_settings::load_chat_provider(chat, err)) {
            out.error = "speak_audio: could not load chat settings: " + err;
            return out;
        }
    }

    const std::string provider_arg = audio_tool_read_str(args, "provider");
    const std::string model_arg    = audio_tool_read_str(args, "model");
    const std::string voice_arg    = audio_tool_read_str(args, "voice");
    const std::string provider     = provider_arg.empty() ? chat.tts_provider : provider_arg;
    const std::string model        = model_arg.empty() ? chat.tts_model : model_arg;
    const std::string voice_id     = voice_arg.empty() ? chat.tts_voice_id : voice_arg;
    if (provider.empty()) {
        out.error = "speak_audio: no TTS provider configured";
        return out;
    }
    if (pm::audio::constants::is_windows_tts_provider(provider)) {
        try {
            out.audio = pm::tts::windows_tts_synthesize(text, voice_id);
            out.format = "wav";
            out.mime_type = "audio/wav";
            out.ok = !out.audio.empty();
            if (!out.ok) out.error = "speak_audio: windows tts returned empty audio";
        } catch (const std::exception& ex) {
            out.error = std::string("speak_audio: ") + ex.what();
        }
        return out;
    }

    out.provider = provider;
    out.model = model;

    try {
#if defined(FEATURE_VIBEVOICE) && FEATURE_VIBEVOICE
        if (provider == "vibevoice") {
            if (chat.vibevoice_model.empty())
                throw std::runtime_error("vibevoice requires 'vibevoice_model' path in settings");
            if (chat.vibevoice_tokenizer.empty())
                throw std::runtime_error("vibevoice requires 'vibevoice_tokenizer' path in settings");
            auto& eng = pm::tts::VibevoiceEngine::global();
            const int lrc = eng.ensure(chat.vibevoice_model, chat.vibevoice_tokenizer, chat.vibevoice_voice);
            if (lrc != 0)
                throw std::runtime_error("vibevoice engine load failed (rc=" + std::to_string(lrc) + ")");
            fs::path tmp_dir = media::runtime_settings::effective_temp_dir();
            const fs::path tmp = tmp_dir / ("vv_remote_speak_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
            const int rc = eng.tts(text, chat.vibevoice_voice, {}, media::path::path_to_utf8(tmp));
            if (rc != 0)
                throw std::runtime_error("vibevoice synthesis failed (rc=" + std::to_string(rc) + ")");
            std::ifstream ifs(tmp, std::ios::binary);
            out.audio.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
            std::error_code ec;
            fs::remove(tmp, ec);
            out.format = "wav";
            out.mime_type = "audio/wav";
            out.ok = !out.audio.empty();
            if (!out.ok) out.error = "speak_audio: vibevoice returned empty audio";
            return out;
        }
#endif

        std::string api_key, base_url;
        media::runtime_settings::merge_provider_credentials(provider, false, api_key, base_url);
        pm::audio::constants::ensure_speech_base(provider, base_url);
        if (api_key.empty())
            throw std::runtime_error("no API key for provider '" + provider + "'");

        std::string fmt = pm::tts::tts_format_for_ext(ext);
        if (speak_uses_proxy(provider)) {
            if (provider == "openrouter") fmt = "mp3";
            else if (ext == ".opus") fmt = "opus";
            else if (ext == ".aac") fmt = "aac";
            else if (ext == ".flac") fmt = "flac";
            else fmt = "mp3";
            const std::string eff_model = speak_proxy_model(provider, model);
            const std::string eff_voice = speak_proxy_voice(provider, voice_id);
            out.audio = pm::tts::proxy_tts_synthesize(text, base_url, api_key, eff_model, eff_voice, fmt);
            out.format = fmt;
        } else {
            pm::tts::ElevenLabsTTSConfig cfg;
            cfg.api_key = api_key;
            if (!model.empty()) cfg.model_id = model;
            if (!voice_id.empty()) cfg.voice_id = voice_id;
            cfg.output_format = fmt;
            out.audio = pm::tts::elevenlabs_tts_synthesize(text, cfg);
            out.format = fmt;
            if (pm::tts::tts_format_is_pcm(fmt)) {
                const uint32_t data_bytes = static_cast<uint32_t>(out.audio.size());
                const uint16_t channels = 1;
                const uint16_t bps = 16;
                const int sr = pm::tts::tts_pcm_sample_rate(fmt);
                std::vector<unsigned char> wav;
                wav.reserve(44 + out.audio.size());
                auto push = [&](const void* p, size_t n) {
                    const auto* b = static_cast<const unsigned char*>(p);
                    wav.insert(wav.end(), b, b + n);
                };
                auto w16 = [&](uint16_t v) { push(&v, sizeof(v)); };
                auto w32 = [&](uint32_t v) { push(&v, sizeof(v)); };
                push("RIFF", 4); w32(36 + data_bytes);
                push("WAVE", 4);
                push("fmt ", 4); w32(16); w16(1); w16(channels);
                w32(static_cast<uint32_t>(sr));
                w32(static_cast<uint32_t>(sr * channels * bps / 8));
                w16(static_cast<uint16_t>(channels * bps / 8)); w16(bps);
                push("data", 4); w32(data_bytes);
                push(out.audio.data(), out.audio.size());
                out.audio = std::move(wav);
                out.format = "wav";
            }
        }

        if (ext == ".wav" || out.format == "wav") out.mime_type = "audio/wav";
        else if (ext == ".opus" || out.format == "opus") out.mime_type = "audio/ogg; codecs=opus";
        else if (ext == ".aac" || out.format == "aac") out.mime_type = "audio/aac";
        else if (ext == ".flac" || out.format == "flac") out.mime_type = "audio/flac";
        else out.mime_type = "audio/mpeg";

        out.ok = !out.audio.empty();
        if (!out.ok) out.error = "speak_audio: provider returned empty audio";
        return out;
    } catch (const std::exception& ex) {
        out.error = std::string("speak_audio: ") + ex.what();
        return out;
    }
#endif
}

#if defined(__ANDROID__)
ExecuteResult do_speak_android(const nlohmann::json& args) {
    const std::string text = args.value("text", std::string{});
    if (text.empty())
        return fatal("speak: 'text' is required and must not be empty");

    auto read_str = [&](const char* key) -> std::string {
        if (auto s = get_nonempty_string(args, key)) return *s;
        if (auto s = get_nonempty_string(options_or_empty(args), key)) return *s;
        return {};
    };
    bool play = true;
    if (args.contains("play") && args["play"].is_boolean())
        play = args["play"].get<bool>();
    else if (const auto& o = options_or_empty(args); o.contains("play") && o["play"].is_boolean())
        play = o["play"].get<bool>();

    const std::string dst_raw = read_str("dst");
    const std::string text_out_raw = read_str("text_out");
    std::string dst_abs;
    std::string text_out_abs;
    if (!dst_raw.empty()) {
        dst_abs = resolve_tool_path_string(dst_raw);
        if (const std::string w = llm_path_fs_deny(dst_abs); !w.empty())
            return fatal("speak: dst: " + w);
        if (const std::string w = llm_path_write_deny(dst_abs); !w.empty())
            return fatal("speak: dst: " + w);
    }
    if (!text_out_raw.empty()) {
        text_out_abs = resolve_tool_path_string(text_out_raw);
        if (const std::string w = llm_path_fs_deny(text_out_abs); !w.empty())
            return fatal("speak: text_out: " + w);
        if (const std::string w = llm_path_write_deny(text_out_abs); !w.empty())
            return fatal("speak: text_out: " + w);
        std::error_code ec;
        const std::filesystem::path txt_p(text_out_abs);
        if (txt_p.has_parent_path())
            std::filesystem::create_directories(txt_p.parent_path(), ec);
        std::ofstream ofs(txt_p, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return fatal("speak: could not open text_out for writing: " + text_out_abs);
        ofs << text;
        if (text.empty() || text.back() != '\n') ofs << '\n';
    }
    if (!play && dst_abs.empty() && text_out_abs.empty())
        return fatal("speak: nothing to do (play=false and no dst / text_out)");

    if (play || !dst_abs.empty()) {
        std::string err;
        if (!pm::android::device_speak(text, play, dst_abs, err))
            return fatal(err.empty() ? "speak: phone speech failed" : err);
    }
    ExecuteResult r;
    r.ok = true;
    r.envelope = {
        {"ok", true},
        {"tool", "speak"},
        {"provider", "android"},
        {"played", play},
    };
    if (!dst_abs.empty()) r.envelope["dst"] = dst_abs;
    if (!text_out_abs.empty()) r.envelope["text_out"] = text_out_abs;
    return r;
}
#endif

ExecuteResult do_speak(const nlohmann::json& args) {
#if defined(__ANDROID__)
    return do_speak_android(args);
#elif !(defined(FEATURE_STT) && FEATURE_STT)
    (void)args;
    return fatal("speak: audio playback requires FEATURE_STT (not compiled in this build)");
#else
    const std::string text = args.value("text", std::string{});
    if (text.empty())
        return fatal("speak: 'text' is required and must not be empty");

    auto read_str = [&](const char* key) -> std::string {
        if (auto s = get_nonempty_string(args, key)) return *s;
        if (auto s = get_nonempty_string(options_or_empty(args), key)) return *s;
        return {};
    };
    const std::string dst_raw      = read_str("dst");
    const std::string text_out_raw = read_str("text_out");
    bool play = true;
    if (args.contains("play") && args["play"].is_boolean())
        play = args["play"].get<bool>();
    else if (const auto& o = options_or_empty(args); o.contains("play") && o["play"].is_boolean())
        play = o["play"].get<bool>();

    std::string dst_abs, text_out_abs;
    if (!dst_raw.empty()) {
        dst_abs = resolve_tool_path_string(dst_raw);
        if (const std::string w = llm_path_fs_deny(dst_abs); !w.empty())
            return fatal("speak: dst: " + w);
        if (const std::string w = llm_path_write_deny(dst_abs); !w.empty())
            return fatal("speak: dst: " + w);
    }
    if (!text_out_raw.empty()) {
        text_out_abs = resolve_tool_path_string(text_out_raw);
        if (const std::string w = llm_path_fs_deny(text_out_abs); !w.empty())
            return fatal("speak: text_out: " + w);
        if (const std::string w = llm_path_write_deny(text_out_abs); !w.empty())
            return fatal("speak: text_out: " + w);
    }

    if (!play && dst_abs.empty() && text_out_abs.empty())
        return fatal("speak: nothing to do (play=false and no dst / text_out)");

    if (!text_out_abs.empty()) {
        std::error_code ec;
        const fs::path txt_p(text_out_abs);
        if (txt_p.has_parent_path())
            fs::create_directories(txt_p.parent_path(), ec);
        std::ofstream ofs(txt_p, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return fatal("speak: could not open text_out for writing: " + text_out_abs);
        ofs << text;
        if (text.empty() || text.back() != '\n') ofs << '\n';
        ofs.close();
        if (!ofs)
            return fatal("speak: write error on text_out: " + text_out_abs);
    }

    media::runtime_settings::ChatProviderSettings chat;
    {
        std::string err;
        if (!media::runtime_settings::load_chat_provider(chat, err))
            return fatal("speak: could not load chat settings: " + err);
    }
    const std::string& provider  = chat.tts_provider;
    const std::string& model     = chat.tts_model;
    const std::string& voice_id  = chat.tts_voice_id;
    if (provider.empty())
        return fatal("speak: no TTS provider configured — set one in App Settings → Chat → Voice & Audio");

    // ── vibevoice local path ──────────────────────────────────────────────────
#if defined(FEATURE_VIBEVOICE) && FEATURE_VIBEVOICE
    if (provider == "vibevoice") {
        if (chat.vibevoice_model.empty())
            return fatal("speak: vibevoice requires 'vibevoice_model' path in App Settings → Chat → Voice & Audio");
        if (chat.vibevoice_tokenizer.empty())
            return fatal("speak: vibevoice requires 'vibevoice_tokenizer' path in App Settings → Chat → Voice & Audio");

        auto& eng = pm::tts::VibevoiceEngine::global();
        const int lrc = eng.ensure(chat.vibevoice_model, chat.vibevoice_tokenizer,
                                   chat.vibevoice_voice);
        if (lrc != 0)
            return fatal("speak: vibevoice engine load failed (rc=" + std::to_string(lrc) + ")");

        namespace fs  = std::filesystem;
        namespace chr = std::chrono;
        const std::string tmp = media::path::path_to_utf8(
            media::runtime_settings::effective_temp_dir() /
            ("vv_agent_speak_" + std::to_string(
                chr::steady_clock::now().time_since_epoch().count()) + ".wav"));

        const int rc = eng.tts(text, chat.vibevoice_voice, {}, tmp);
        if (rc != 0) {
            std::error_code ec; fs::remove(tmp, ec);
            return fatal("speak: vibevoice synthesis failed (rc=" + std::to_string(rc) + ")");
        }

        // Optional dst save.
        if (!dst_abs.empty()) {
            std::error_code ec;
            const fs::path dst_p(dst_abs);
            if (dst_p.has_parent_path()) fs::create_directories(dst_p.parent_path(), ec);
            fs::copy_file(tmp, dst_abs, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::error_code ec2; fs::remove(tmp, ec2);
                return fatal("speak: could not save to dst: " + dst_abs);
            }
        }

        if (play) {
            try {
                pm::audio::play_sound_file_sync(tmp, []{ return false; });
            } catch (const std::exception& ex) {
                logger::warn(std::string("[speak/vv] playback error (non-fatal): ") + ex.what());
            }
        }
        std::error_code ec; fs::remove(tmp, ec);

        ExecuteResult r;
        r.ok = true;
        r.envelope["provider"]  = "vibevoice";
        r.envelope["model"]     = chat.vibevoice_model;
        if (!dst_abs.empty())   r.envelope["dst"]        = dst_abs;
        if (!text_out_abs.empty()) r.envelope["text_out"] = text_out_abs;
        return r;
    }
#endif // FEATURE_VIBEVOICE

    if (pm::audio::constants::is_windows_tts_provider(provider)) {
        std::vector<uint8_t> wav;
        try {
            wav = pm::tts::windows_tts_synthesize(text, voice_id);
        } catch (const std::exception& ex) {
            return fatal(std::string("speak: ") + ex.what());
        }
        if (!dst_abs.empty()) {
            std::error_code ec;
            const std::filesystem::path dst_p(dst_abs);
            if (dst_p.has_parent_path())
                std::filesystem::create_directories(dst_p.parent_path(), ec);
            std::ofstream ofs(dst_p, std::ios::binary | std::ios::trunc);
            if (!ofs)
                return fatal("speak: could not save to dst: " + dst_abs);
            ofs.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
        }
        if (play) {
            try {
                static thread_local pm::audio::AudioOutput tls_out;
                g_tts_out.store(&tls_out, std::memory_order_release);
                tls_out.play_sync(wav.data(), wav.size());
                g_tts_out.store(nullptr, std::memory_order_release);
            } catch (const std::exception& ex) {
                g_tts_out.store(nullptr, std::memory_order_release);
                return fatal(std::string("speak: ") + ex.what());
            }
        }
        ExecuteResult r;
        r.ok = true;
        r.envelope["provider"] = "windows";
        if (!dst_abs.empty()) r.envelope["dst"] = dst_abs;
        if (!text_out_abs.empty()) r.envelope["text_out"] = text_out_abs;
        return r;
    }

    std::string api_key, base_url;
    media::runtime_settings::merge_provider_credentials(provider, false, api_key, base_url);
    if (api_key.empty())
        return fatal("speak: no API key for provider '" + provider
                     + "' — configure it in App Settings → API Providers");

    const std::string eff_model = speak_proxy_model(provider, model);
    const std::string eff_voice = speak_proxy_voice(provider, voice_id);

    pm::tts::ElevenLabsTTSConfig el_cfg;
    if (!speak_uses_proxy(provider)) {
        el_cfg.api_key = api_key;
        if (!model.empty())    el_cfg.model_id = model;
        if (!voice_id.empty()) el_cfg.voice_id  = voice_id;
    }

    std::string dst_fmt;
    if (!dst_abs.empty()) {
        std::string ext = media::path::path_to_utf8(media::path::path_from_utf8(dst_abs).extension());
        ext = media::strutil::lower_ascii(std::move(ext));
        dst_fmt = pm::tts::tts_format_for_ext(ext);
        if (!speak_uses_proxy(provider))
            el_cfg.output_format = dst_fmt;
    }

    try {
        const bool buffered = !dst_abs.empty();
        logger::info("[speak] provider=" + provider + " synthesising "
                     + std::to_string(text.size()) + " chars "
                     + (buffered ? "(buffered → dst)" : "(streaming)"));

        if (buffered) {
            std::vector<uint8_t> audio;
            if (speak_uses_proxy(provider)) {
                std::string proxy_fmt = dst_fmt;
                if (const auto us = proxy_fmt.find('_'); us != std::string::npos)
                    proxy_fmt = proxy_fmt.substr(0, us);
                if (provider == "openrouter" && proxy_fmt != "pcm")
                    proxy_fmt = "mp3";
                audio = pm::tts::proxy_tts_synthesize(
                    text, base_url, api_key, eff_model, eff_voice, proxy_fmt);
            } else {
                audio = pm::tts::elevenlabs_tts_synthesize(text, el_cfg);
            }
            if (audio.empty())
                return fatal("speak: provider returned empty audio");

            const fs::path dst_p(dst_abs);
            std::error_code ec;
            if (dst_p.has_parent_path())
                fs::create_directories(dst_p.parent_path(), ec);
            std::ofstream ofs(dst_p, std::ios::binary | std::ios::trunc);
            if (!ofs)
                return fatal("speak: could not open dst for writing: " + dst_abs);

            if (pm::tts::tts_format_is_pcm(dst_fmt)) {
                const int      sr         = pm::tts::tts_pcm_sample_rate(dst_fmt);
                const uint16_t channels   = 1;
                const uint16_t bps        = 16;
                const uint32_t data_bytes = static_cast<uint32_t>(audio.size());
                auto w16 = [&](uint16_t v){ ofs.write(reinterpret_cast<const char*>(&v), 2); };
                auto w32 = [&](uint32_t v){ ofs.write(reinterpret_cast<const char*>(&v), 4); };
                ofs.write("RIFF", 4); w32(36 + data_bytes);
                ofs.write("WAVE", 4);
                ofs.write("fmt ", 4); w32(16); w16(1); w16(channels);
                w32(static_cast<uint32_t>(sr));
                w32(static_cast<uint32_t>(sr * channels * bps / 8));
                w16(static_cast<uint16_t>(channels * bps / 8)); w16(bps);
                ofs.write("data", 4); w32(data_bytes);
            }
            ofs.write(reinterpret_cast<const char*>(audio.data()),
                      static_cast<std::streamsize>(audio.size()));
            ofs.close();
            if (!ofs)
                return fatal("speak: write error on dst: " + dst_abs);

            const auto audio_bytes_n = static_cast<uint64_t>(audio.size());
            if (play) {
                auto play_buf = std::make_shared<std::vector<uint8_t>>(std::move(audio));
                std::thread([play_buf]() noexcept {
                    try {
                        static thread_local pm::audio::AudioOutput tls_out;
                        g_tts_out.store(&tls_out, std::memory_order_release);
                        tls_out.play_sync(play_buf->data(), play_buf->size());
                        g_tts_out.store(nullptr, std::memory_order_release);
                    } catch (...) {
                        g_tts_out.store(nullptr, std::memory_order_release);
                        logger::warn("[speak] background dst playback error");
                    }
                }).detach();
            }

            logger::info("[speak] done (dst=" + dst_abs + ", "
                         + std::to_string(audio_bytes_n) + " bytes, play="
                         + (play ? "async" : "no") + ")");
            ExecuteResult r;
            r.ok       = true;
            r.envelope = nlohmann::json{
                {"ok",          true},
                {"provider",    provider},
                {"model",       model},
                {"chars",       static_cast<int>(text.size())},
                {"dst",         dst_abs},
                {"dst_bytes",   audio_bytes_n},
                {"dst_format",  dst_fmt},
                {"played",      play},
            };
            if (!text_out_abs.empty()) r.envelope["text_out"] = text_out_abs;
            return r;
        }

        // Non-blocking: launch playback on a detached background thread so the
        // agent can continue processing while audio plays.
        auto bg_text     = text;
        auto bg_provider = provider;
        auto bg_base_url = base_url;
        auto bg_api_key  = api_key;
        auto bg_model    = eff_model;
        auto bg_voice    = eff_voice;
        auto bg_el_cfg   = el_cfg;
        std::thread([bg_text     = std::move(bg_text),
                     bg_provider = std::move(bg_provider),
                     bg_base_url = std::move(bg_base_url),
                     bg_api_key  = std::move(bg_api_key),
                     bg_model    = std::move(bg_model),
                     bg_voice    = std::move(bg_voice),
                     bg_el_cfg   = std::move(bg_el_cfg)]() noexcept {
            try {
                static thread_local pm::audio::AudioOutput tls_out;
                speak_play_with_stream_fallback(
                    bg_text, bg_provider, bg_base_url, bg_api_key, bg_model, bg_voice, bg_el_cfg, tls_out);
                logger::info("[speak] background playback done");
            } catch (const std::exception& ex) {
                logger::warn(std::string("[speak] background playback error: ") + ex.what());
            } catch (...) {
                logger::warn("[speak] background playback unknown error");
            }
        }).detach();

        logger::info("[speak] playback launched (non-blocking)");

        ExecuteResult r;
        r.ok       = true;
        r.envelope = nlohmann::json{
            {"ok",       true},
            {"provider", provider},
            {"model",    model},
            {"chars",    static_cast<int>(text.size())},
            {"playing",  true},
            {"async",    true},
        };
        if (!text_out_abs.empty()) r.envelope["text_out"] = text_out_abs;
        return r;
    } catch (const std::exception& ex) {
        return fatal(std::string("speak: ") + ex.what());
    }
#endif
}

namespace {

bool play_file_extension_ok(const std::string& abs)
{
    std::string ext = media::path::path_to_utf8(media::path::path_from_utf8(abs).extension());
    ext = media::strutil::lower_ascii(std::move(ext));
    static const char* k_exts[] = {
        ".wav", ".mp3", ".ogg", ".oga", ".opus", ".flac",
        ".aac", ".m4a", ".mp4", ".3gp", ".amr",
    };
    for (const char* e : k_exts)
        if (ext == e) return true;
    return false;
}

} // namespace

ExecuteResult do_play_file(const nlohmann::json& args) {
    auto read_str = [&](const char* key) -> std::string {
        if (auto s = get_nonempty_string(args, key)) return *s;
        if (auto s = get_nonempty_string(options_or_empty(args), key)) return *s;
        return {};
    };
    const std::string raw = read_str("path");
    if (raw.empty())
        return fatal("play_file: 'path' is required");

    const std::string abs = resolve_tool_path_string(raw);
    if (const std::string w = llm_path_fs_deny(abs); !w.empty())
        return fatal("play_file: " + w);

    std::error_code ec;
    const fs::path file = media::path::path_from_utf8(abs);
    if (!fs::exists(file, ec) || ec)
        return fatal("play_file: path does not exist");
    if (!fs::is_regular_file(file, ec) || ec)
        return fatal("play_file: not a regular file");
    const auto bytes = fs::file_size(file, ec);
    if (ec || bytes == 0)
        return fatal("play_file: file is empty");
    constexpr std::uintmax_t k_max_bytes = 64ull * 1024ull * 1024ull;
    if (bytes > k_max_bytes)
        return fatal("play_file: file is larger than 64 MiB");
    if (!play_file_extension_ok(abs))
        return fatal("play_file: unsupported audio type "
                     "(use .wav, .mp3, .ogg, .opus, .flac, .aac, .m4a, .mp4, .3gp, .amr)");

    if (media::cli::cancel_requested())
        return fatal("play_file: cancelled");

#if defined(__ANDROID__)
    std::string err;
    if (!pm::android::device_play_file(abs, err))
        return fatal(err.empty() ? "play_file: phone playback failed" : err);
#elif defined(FEATURE_STT) && FEATURE_STT
    try {
        pm::audio::play_sound_file_sync(abs, [] { return media::cli::cancel_requested(); });
    } catch (const std::exception& ex) {
        return fatal(std::string("play_file: ") + ex.what());
    }
    if (media::cli::cancel_requested())
        return fatal("play_file: cancelled");
#else
    return fatal("play_file: audio playback is not compiled in this build");
#endif

    ExecuteResult r;
    r.ok = true;
    r.envelope = {
        {"ok", true},
        {"tool", "play_file"},
        {"path", abs},
        {"bytes", static_cast<std::uint64_t>(bytes)},
        {"played", true},
    };
    return r;
}

void abort_active_speak_impl() {
#if defined(FEATURE_STT) && FEATURE_STT
    auto* p = g_tts_out.load(std::memory_order_acquire);
    logger::trace(std::string("[cancel] abort_active_speak: tls_out=")
                  + (p ? "present → stop()" : "null (nothing to stop)"));
    if (p) p->stop();
#else
    logger::trace("[cancel] abort_active_speak: FEATURE_STT off (no-op)");
#endif
}

void pause_active_speak_impl() {
#if defined(FEATURE_STT) && FEATURE_STT
    auto* p = g_tts_out.load(std::memory_order_acquire);
    if (p) p->pause();
#endif
}

void resume_active_speak_impl() {
#if defined(FEATURE_STT) && FEATURE_STT
    auto* p = g_tts_out.load(std::memory_order_acquire);
    if (p) p->resume();
#endif
}

void set_active_speak_output(pm::audio::AudioOutput* out) noexcept {
#if defined(FEATURE_STT) && FEATURE_STT
    g_tts_out.store(out, std::memory_order_release);
#endif
    (void)out;
}

void clear_active_speak_output(pm::audio::AudioOutput* expected) noexcept {
#if defined(FEATURE_STT) && FEATURE_STT
    auto* p = g_tts_out.load(std::memory_order_acquire);
    if (p == expected)
        g_tts_out.store(nullptr, std::memory_order_release);
#endif
    (void)expected;
}

// ── Catalog schemas ───────────────────────────────────────────────────────

ToolDef make_speak() {
    return ToolDef{
        "speak",
        "Synthesise text to speech and play it on the default speaker. "
        "Use when the user asks to say, announce, read aloud, narrate, or speak text. "
        "Playback is synchronous — the tool returns after the audio finishes playing. "
        "Optional `dst` saves the synthesised audio to disk (.mp3 / .wav / .opus). "
        "Optional `text_out` writes the spoken text to a UTF-8 file alongside playback. "
        "Both accept relative paths (resolved against the current Explorer / context folder "
        "like `image_create.output_path`) or absolute paths.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"text", {
                    {"type", "string"},
                    {"description",
                     "The text to synthesise and play aloud. "
                     "Plain prose works best; keep under ~500 words for responsive playback. "
                     "Do not include SSML tags."},
                }},
                {"dst", {
                    {"type", "string"},
                    {"description",
                     "Optional output audio file path. Extension picks the format: "
                     ".mp3 (default), .wav (PCM 44.1 kHz s16le mono), .opus. "
                     "Relative paths resolve to the current Explorer / context folder; "
                     "absolute paths are accepted. Parent directories are created if missing. "
                     "When set, the streaming low-latency path is skipped in favour of a "
                     "single buffered synthesis so the same bytes can be written to disk."},
                }},
                {"text_out", {
                    {"type", "string"},
                    {"description",
                     "Optional path to write the spoken text as a UTF-8 file "
                     "(transcript/caption). Relative paths resolve to the current Explorer / "
                     "context folder; absolute paths are accepted. Parent directories are "
                     "created if missing. Independent of `dst` — set either, both, or neither."},
                }},
                {"play", {
                    {"type", "boolean"},
                    {"description",
                     "Play the audio through the default speaker (default: true). "
                     "Set to false to silently save with `dst` and/or `text_out` only. "
                     "Ignored when both `dst` and `text_out` are empty (there would be "
                     "nothing to do otherwise)."},
                    {"default", true},
                }},
            }},
            {"required", nlohmann::json::array({"text"})},
        }};
}

ToolDef make_play_file() {
    return ToolDef{
        "play_file",
        "Play an existing audio file on the default speaker. "
        "Use when the user asks to play, hear, or listen to a file already on disk "
        "(a recording, a saved `speak` `dst`, or a downloaded clip). "
        "Playback is synchronous — the tool returns after the audio finishes or the turn is stopped. "
        "`path` is relative to the current folder or absolute. "
        "This does not synthesise speech; use `speak` for text.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {
                    {"type", "string"},
                    {"description",
                     "Audio file to play. Relative paths resolve against the current "
                     "Explorer / context folder; absolute paths are accepted. "
                     "Supported types: .wav, .mp3, .ogg, .opus, .flac, .aac, .m4a, .mp4, .3gp, .amr. "
                     "The file must be a regular file of at most 64 MiB."},
                }},
            }},
            {"required", nlohmann::json::array({"path"})},
        }};
}

ToolDef make_audio_transcribe() {
    return ToolDef{
        "audio_transcribe",
        "Record from the default microphone, transcribe with the chat STT provider, and return the text. "
        "Pair with `speak` to run a Q&A turn: agent asks → user answers aloud → agent receives the transcript. "
        "Recording stops automatically on the first of: "
        "(a) `silence_ms` of quiet **after** at least one detected speech burst (auto-VAD — default 1500 ms, set 0 to disable), "
        "(b) `max_duration_ms` reached (hard cap, default 30 000 ms), "
        "(c) Ctrl+C / UI Stop cancellation. "
        "VAD uses an RMS gate (~200 / 32 767 ≈ 0.6 %% of full-scale) so quiet rooms don't false-trigger, and silence is only counted **after** the user has actually spoken — empty rooms never auto-commit. "
        "Returns transcript text + duration + `stopped_by` (`vad` | `max_duration` | `cancelled`). "
        "Optional `prompt` plays a short TTS cue **before** recording starts so the user knows they're being heard (uses the same chat TTS provider as `speak`). "
        "Optional `dst` keeps the recorded audio as a WAV file (16 kHz mono s16le). "
        "Optional `text_out` writes the transcript to a UTF-8 file. "
        "Both file paths accept relative paths (resolved against the current Explorer / context folder like `image_create.output_path`) or absolute paths. "
        "Provider/credentials are read from app Settings → Chat → Voice & Audio (override with `provider` / `api_key`).",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"prompt", {
                    {"type", "string"},
                    {"description",
                     "Optional short cue to speak before recording starts (one sentence is ideal, e.g. "
                     "'What color do you want?'). When set, the tool synthesises this via TTS and plays it "
                     "synchronously, then immediately begins recording the user's reply. "
                     "Skip when the cue was already delivered by a separate `speak` call."},
                }},
                {"max_duration_ms", {
                    {"type", "integer"},
                    {"minimum", 500},
                    {"maximum", 120000},
                    {"description",
                     "Hard cap on recording length in milliseconds (default 30 000). "
                     "Recording stops here even if the user keeps talking — pick a value matching the "
                     "expected answer length (short y/n: 5 000; sentence: 15 000; free-form: 30 000–60 000)."},
                    {"default", 30000},
                }},
                {"silence_ms", {
                    {"type", "integer"},
                    {"minimum", 0},
                    {"maximum", 30000},
                    {"description",
                     "VAD: auto-stop after this many ms of silence following at least one speech burst "
                     "(default 1 500). Set to 0 to disable VAD entirely (records the full `max_duration_ms`). "
                     "Silence is only counted **after** speech has been detected, so an empty room never "
                     "auto-commits before the user starts speaking."},
                    {"default", 1500},
                }},
                {"dst", {
                    {"type", "string"},
                    {"description",
                     "Optional WAV file path for the recorded audio (16 kHz mono s16le). "
                     "Relative paths resolve to the current Explorer / context folder; "
                     "absolute paths are accepted. Parent directories are created if missing."},
                }},
                {"text_out", {
                    {"type", "string"},
                    {"description",
                     "Optional path to write the transcript as a UTF-8 text file. "
                     "Same path semantics as `dst`."},
                }},
                {"provider", {
                    {"type", "string"},
                    {"enum", {"tanit", "openrouter", "elevenlabs", "whisper", "windows", ""}},
                    {"description",
                     "STT provider override. `pixlwiz` uses the LiteLLM proxy /audio/transcriptions "
                     "(Whisper batch, runs after recording finishes). "
                     "`elevenlabs` uses Scribe v2 Realtime (live partials, then commit). "
                     "`whisper` runs whisper.cpp fully offline on-device (no API key; ggml model from ${MODELS_DIR} or install models). "
                     "When omitted, falls back to chat.stt_provider from app Settings → Chat → Voice & Audio."},
                }},
                {"api_key", {
                    {"type", "string"},
                    {"description",
                     "Optional API key override. Falls back to the provider entry in App Settings → API Providers."},
                }},
                {"input_device", {
                    {"type", "string"},
                    {"description",
                     "Optional capture device name (case-insensitive substring; use `audio info` to list). "
                     "Empty = system default."},
                }},
            }},
            {"required", nlohmann::json::array()},
        }};
}

} // namespace media::llm::path::detail

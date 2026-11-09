#include "pm_image_cmd_includes.hpp"
#include "json/json_utils.hpp"
#include "string/string_utils.hpp"
#include "pm_image_cmd_llm_agent.hpp"
#include "pm_image_cmd_llm_agent_prompt.hpp"
#include "pm_image_cmd_llm_agent_server.hpp"
#include "pm_image_cmd_llm_call.hpp"
#include "pm_image_cmd_llm_info.hpp"
#include "pm_image_cmd_llm_list.hpp"
#include "lan/tanit_lan.hpp"
#include "pm_image_cmd_llm_external_agent.hpp"
#include "pm_image_cli_activity.hpp"

#if defined(FEATURE_JQ) && FEATURE_JQ
#  include "process/async_iterator.hpp"
#  include "llm/agent_decide.hpp"
#  include "llm/agent_each.hpp"
#  include "llm/decisions.hpp"
#  include <cctype>
#endif

#include "cli/cli_tty.hpp"
#include "html/html.h"
#include "llm/agent_feature.hpp"
#include "llm/agent_internal.hpp"
#include "core/chat_settings_schema.hpp"
#include "llm/agent_cli_launch.hpp"
#include "llm/agent_launch.hpp"
#include "llm/agent_runner_log.hpp"
#include "llm/agent_runner_session.hpp"
#include "llm/agent_turn_options.hpp"
#include "llm/agent_memory.hpp"
#include "llm/agent_memory_artifacts.hpp"
#include "llm/agent_memory_recall.hpp"
#include "llm/agent_scheduler.hpp"
#include "fs/fs_guard.hpp"
#include "fs/glob_filter.hpp"
#include "llm/llm_image_tool_defaults.hpp"
#include "llm/path_tool_executor.hpp"  // abort_active_speak / abort_active_run
#ifndef FEATURE_HOME_LLM_TOOLS
#define FEATURE_HOME_LLM_TOOLS 1
#endif
#ifndef FEATURE_HOME_LLM_SKILLS
#define FEATURE_HOME_LLM_SKILLS 1
#endif
#if FEATURE_HOME_LLM_SKILLS
#include "llm/agent_skills.hpp"
#endif
#include "core/command_variables.hpp"
#include "core/settings_runtime.hpp"
#include "win/overlay_hud.hpp"
#include "win/taskbar_status.hpp"
#if defined(FEATURE_COMMAND_IPC_NEXT) && FEATURE_COMMAND_IPC_NEXT
#include "core/agent_session_ipc.hpp"
#include "core/cmd_ipc.hpp"
#endif

#include <filesystem>
namespace fs = std::filesystem;

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <memory>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include "fs/path_sanitizer.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <conio.h>
#include <windows.h>
#endif

#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
#include "security/consent_surface.hpp"
#include "security/ui/win/consent_ai_review.hpp" // ai_review_from_json
#include "llm/security_review.hpp"               // run_review (SecurityAgent)
#include <functional>
#endif

#if defined(FEATURE_STT) && FEATURE_STT
#include "core/audio.hpp"
#include "audio_constants.hpp"
#include "stt/elevenlabs.hpp"
#include "stt/proxy_stt.hpp"
#include "stt/whisper_local.hpp"
#include "tts/elevenlabs_tts.hpp"
#include "tts/proxy_tts.hpp"
#include "tts/windows_tts.hpp"
#include "stt/windows_ai_stt.hpp"
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#  if defined(FEATURE_REALTIME) && FEATURE_REALTIME
#include "core/realtime_voice_session_ipc.hpp"
#include "plugins/chat/host_chat.hpp"
#  endif
#endif
#include "llm/cli/agent_cli_util.hpp"
#include "llm/cli/agent_cli_voice.hpp"

namespace pm_llm_agent_cli {
// ── mic conversation loop ─────────────────────────────────────────────────────
#if defined(FEATURE_STT) && FEATURE_STT

int cmd_llm_agent_mic(const PmImageCliState& st,
                              media::llm::agent::Turn         base_turn,
                              const media::llm::agent::ProviderSettings& prov,
                              const media::runtime_settings::ChatProviderSettings& chat)
{
    namespace chr = std::chrono;
    using clk     = chr::steady_clock;

    // chat is already resolved by the caller (launch_resolved.chat_settings),
    // incorporating any --stt-provider/model/--tts-* CLI overrides.

    const std::string stt_provider = chat.stt_provider;
    const std::string stt_model    = chat.stt_model;

    if (stt_provider.empty()) {
        std::cerr << "llm agent mic: no STT provider — configure one in "
                     "App Settings → Chat → Voice & Audio, or pass --stt-provider\n";
        return 1;
    }

    // whisper runs fully on-device — no API key / base URL required.
    const bool use_windows_ai_stt = pm::audio::constants::is_windows_ai_stt_provider(stt_provider);
    const bool use_local_whisper_stt = (stt_provider == "whisper");

    // CLI --stt-api-key overrides the key; base_url always comes from settings.
    std::string stt_key      = st.ag_mic_stt_api_key;
    std::string stt_base_url;
    if (stt_key.empty() && !use_local_whisper_stt && !use_windows_ai_stt)
        media::runtime_settings::merge_provider_credentials(
            stt_provider, false, stt_key, stt_base_url);
    pm::audio::constants::ensure_speech_base(stt_provider, stt_base_url);
    if (stt_key.empty() && !use_local_whisper_stt && !use_windows_ai_stt) {
        std::cerr << "llm agent mic: no API key for STT provider '" << stt_provider
                  << "' — configure it in App Settings → API Providers\n";
        return 1;
    }

    const std::string tts_provider = chat.tts_provider;
    // --tts-model-id / --voice-id on CLI override chat settings for this run.
    const std::string tts_model    = !st.ag_mic_model_id.empty() ? st.ag_mic_model_id
                                                                  : chat.tts_model;
    const std::string tts_voice_id = !st.ag_mic_voice_id.empty() ? st.ag_mic_voice_id
                                                                   : chat.tts_voice_id;
    std::string tts_key, tts_base_url;
    if (!tts_provider.empty())
        media::runtime_settings::merge_provider_credentials(
            tts_provider, false, tts_key, tts_base_url);
    pm::audio::constants::ensure_speech_base(tts_provider, tts_base_url);
    const bool use_windows_tts = pm::audio::constants::is_windows_tts_provider(tts_provider);
    const bool use_tts = !st.ag_mic_no_tts && !tts_provider.empty()
        && (use_windows_tts || !tts_key.empty());

    const bool use_elevenlabs_stt = (stt_provider == "elevenlabs");
    const bool use_whisper_stt    = !use_elevenlabs_stt; // pixlwiz / other batch

    // Whisper is batch (full utterance → REST call after silence), so a short VAD
    // window easily truncates mid-sentence.  Clamp to 2500 ms for batch providers.
    const int vad_silence_ms = use_whisper_stt
        ? std::max(st.ag_mic_silence_ms, 2500)
        : st.ag_mic_silence_ms;

    // ── thread-safe queues ────────────────────────────────────────────────────
    std::mutex              prompt_mtx;
    std::condition_variable prompt_cv;
    std::queue<std::string> prompt_queue;   // committed utterances → LLM

    std::mutex              print_mtx;
    std::queue<std::string> print_queue;    // messages for main thread to display
    std::string             last_partial;

    // Gate: suppress STT while speakers are active to avoid echo.
    std::atomic<bool> tts_playing{false};

    // ── VAD state (audio thread only) ─────────────────────────────────────────
    constexpr double k_vad_rms = 200.0;
    bool vad_has_speech    = false;
    bool vad_committed     = false;
    auto vad_last_speech   = clk::now();

    // ── ElevenLabs STT session (real-time WebSocket) ──────────────────────────
    std::unique_ptr<pm::stt::ElevenLabsSTT> el_stt;
    if (use_elevenlabs_stt) {
        el_stt = std::make_unique<pm::stt::ElevenLabsSTT>();

        el_stt->on_partial = [&](const std::string& t) {
            std::lock_guard<std::mutex> lk(print_mtx);
            last_partial = t;
        };

        std::string last_committed_text;
        el_stt->on_committed = [&, last_committed_text](const std::string& t) mutable {
            std::string segment;
            {
                std::lock_guard<std::mutex> lk(print_mtx);
                if (t.empty()) return;
                if (t == last_committed_text) return;
                if (t.size() > last_committed_text.size() &&
                    t.compare(0, last_committed_text.size(), last_committed_text) == 0) {
                    segment = t.substr(last_committed_text.size());
                    const auto p = segment.find_first_not_of(" \t");
                    if (p != std::string::npos && p > 0) segment = segment.substr(p);
                } else {
                    segment = t;
                }
                last_committed_text = t;
                if (segment.empty()) return;
                last_partial.clear();
                print_queue.push("[you] " + segment);
            }
            {
                std::lock_guard<std::mutex> lk(prompt_mtx);
                prompt_queue.push(segment);
                prompt_cv.notify_one();
            }
        };

        el_stt->on_error = [&](const std::string& e) {
            std::lock_guard<std::mutex> lk(print_mtx);
            print_queue.push("[stt error] " + e);
        };

        std::cout << "llm agent mic: connecting to ElevenLabs STT...\n" << std::flush;
        pm::stt::ElevenLabsSTT::Config scfg;
        scfg.api_key     = stt_key;
        scfg.sample_rate = 16000;

        std::string       connect_err;
        std::atomic<bool> connect_done{false};
        std::thread connect_thread([&]{
            try { el_stt->connect(scfg); }
            catch (const std::exception& ex) { connect_err = ex.what(); }
            connect_done.store(true);
        });
        while (!connect_done.load() && !media::cli::cancel_requested())
            std::this_thread::sleep_for(chr::milliseconds(100));
        if (media::cli::cancel_requested()) {
            el_stt->close();
            connect_thread.join();
            return 130;
        }
        connect_thread.join();
        if (!connect_err.empty()) {
            std::cerr << "llm agent mic: STT connect failed: " << connect_err << "\n";
            return 1;
        }
        std::cout << "llm agent mic: STT ready\n" << std::flush;
    } else {
        std::cout << "llm agent mic: Whisper STT ready (batch — transcribes on silence)\n"
                  << std::flush;
    }

    // ── PCM buffer for Whisper batch path ─────────────────────────────────────
    std::mutex           pcm_buf_mtx;
    std::vector<int16_t> pcm_buf;

    // Shared helper: enqueue a prompt as if it came from STT.
    auto enqueue_prompt_text = [&](const std::string& text) {
        if (text.empty()) return;
        {
            std::lock_guard<std::mutex> lk(print_mtx);
            last_partial.clear();
            print_queue.push("[you] " + text);
        }
        {
            std::lock_guard<std::mutex> lk(prompt_mtx);
            prompt_queue.push(text);
            prompt_cv.notify_one();
        }
    };

    auto transcribe_whisper_snapshot = [&](std::vector<int16_t> snap) {
        if (snap.empty()) return;
        std::thread([snap = std::move(snap),
                     stt_base_url, stt_key, stt_provider, use_local_whisper_stt, use_windows_ai_stt,
                     eff_model = stt_model.empty()
                         ? (use_local_whisper_stt ? std::string("base.en")
                            : (stt_provider == "openai"
                                   ? std::string(pm::audio::constants::k_openai_stt_model)
                                   : (stt_provider == "openrouter"
                                          ? std::string(pm::audio::constants::k_openrouter_stt_model)
                                          : std::string("tanit-speech-to-text"))))
                         : stt_model,
                     &print_mtx, &print_queue,
                     &enqueue_prompt_text]() mutable {
            const std::string tmp =
                media::path::path_to_utf8((std::filesystem::temp_directory_path()
                 / "pmi_agent_stt.wav"));
            try {
                if (!pm::stt::write_pcm_wav(tmp,
                        snap.data(), snap.size(), 16000))
                    throw std::runtime_error("could not write temp WAV");
                std::string text;
                if (use_local_whisper_stt) {
#if defined(FEATURE_WHISPER) && FEATURE_WHISPER
                    const auto whisper_probe = pm::stt::whisper_probe_model_path(eff_model);
                    if (whisper_probe.path.empty())
                        throw std::runtime_error(pm::stt::whisper_model_not_found_message(whisper_probe));
                    text = pm::stt::whisper_local_transcribe(tmp, whisper_probe.path);
#else
                    throw std::runtime_error("whisper provider not available (built without FEATURE_WHISPER)");
#endif
                } else if (use_windows_ai_stt) {
                    text = pm::stt::windows_ai_stt_transcribe_file(tmp);
                } else {
                    text = pm::stt::proxy_stt_transcribe(tmp, stt_base_url, stt_key, eff_model);
                }
                if (text.empty()) return;
                enqueue_prompt_text(text);
            } catch (const std::exception& ex) {
                std::lock_guard<std::mutex> lk(print_mtx);
                print_queue.push(
                    std::string("[stt error] ") + ex.what());
            }
        }).detach();
    };

    // Enter-to-commit for voice:
    // - ElevenLabs STT: send commit frame (flush current utterance now)
    // - Whisper batch STT: snapshot current PCM and transcribe immediately
    auto force_commit_stt = [&]() {
        if (use_elevenlabs_stt && el_stt) {
            el_stt->commit();
            return;
        }
        if (use_whisper_stt) {
            std::vector<int16_t> snap;
            {
                std::lock_guard<std::mutex> lk(pcm_buf_mtx);
                snap = std::move(pcm_buf);
                pcm_buf.clear();
            }
            transcribe_whisper_snapshot(std::move(snap));
        }
    };

    // Manual Enter handling (line buffered stdin):
    // - empty line + Enter => force commit current spoken buffer
    // - non-empty line + Enter => typed prompt send
    std::thread manual_input_thread([&] {
        while (!media::cli::cancel_requested()) {
            std::string line;
            if (!std::getline(std::cin, line))
                break; // EOF / stdin closed
            const auto b = line.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) {
                force_commit_stt();
                continue;
            }
            const auto e = line.find_last_not_of(" \t\r\n");
            line = line.substr(b, e - b + 1);
            enqueue_prompt_text(line);
        }
    });
    manual_input_thread.detach();

    // ── mic capture + VAD ─────────────────────────────────────────────────────
    auto on_pcm = [&](const int16_t* data, size_t frames) {
        if (tts_playing.load()) {
            vad_has_speech  = false;
            vad_committed   = false;
            vad_last_speech = clk::now();
            if (use_whisper_stt) {
                std::lock_guard<std::mutex> lk(pcm_buf_mtx);
                pcm_buf.clear(); // discard echo
            }
            return;
        }

        if (use_elevenlabs_stt && el_stt) {
            el_stt->send_pcm(data, frames);
        } else if (use_whisper_stt) {
            std::lock_guard<std::mutex> lk(pcm_buf_mtx);
            pcm_buf.insert(pcm_buf.end(), data, data + frames);
        }

        if (vad_silence_ms > 0 && frames > 0) {
            double sum = 0.0;
            for (size_t i = 0; i < frames; ++i)
                sum += static_cast<double>(data[i]) * data[i];
            const double rms = std::sqrt(sum / static_cast<double>(frames));
            const auto   now = clk::now();
            if (rms >= k_vad_rms) {
                vad_last_speech = now;
                if (!vad_has_speech) { vad_has_speech = true; vad_committed = false; }
            } else if (vad_has_speech && !vad_committed) {
                const auto silent = chr::duration_cast<chr::milliseconds>(
                    now - vad_last_speech).count();
                if (silent >= vad_silence_ms) {
                    vad_committed = true; vad_has_speech = false;

                    if (use_elevenlabs_stt && el_stt) {
                        el_stt->commit();
                    } else if (use_whisper_stt) {
                        // Snapshot PCM buffer and transcribe in a background thread.
                        std::vector<int16_t> snap;
                        {
                            std::lock_guard<std::mutex> lk(pcm_buf_mtx);
                            snap = std::move(pcm_buf);
                            pcm_buf.clear();
                        }
                        transcribe_whisper_snapshot(std::move(snap));
                    }
                }
            }
        }
    };

    // Resolve microphone device: CLI --input > App Settings audio_input_device > OS default.
    media::runtime_settings::AudioVideoDeviceSettings avs;
    { std::string averr; media::runtime_settings::load_audio_video_device_settings(avs, averr); }
    const std::string eff_mic_input = media::runtime_settings::resolve_audio_input_device(
        st.ag_mic_input, avs);

    pm::audio::AudioInput mic;
    try { mic.start(on_pcm, eff_mic_input); }
    catch (const std::exception& ex) {
        std::cerr << "llm agent mic: " << ex.what() << "\n";
        if (el_stt) el_stt->close();
        return 1;
    }

    // ── banner ────────────────────────────────────────────────────────────────
    const std::string stt_label = stt_provider + "/"
        + (stt_model.empty()
               ? (use_elevenlabs_stt ? "Scribe v2 Realtime"
                  : (stt_provider == "openai"
                         ? std::string(pm::audio::constants::k_openai_stt_model)
                         : (stt_provider == "openrouter"
                                ? std::string(pm::audio::constants::k_openrouter_stt_model)
                                : std::string("tanit-speech-to-text"))))
               : stt_model);
    std::string vad_label;
    if (vad_silence_ms <= 0) {
        vad_label = "  vad=off";
    } else {
        vad_label = "  vad=" + std::to_string(vad_silence_ms) + " ms";
        if (use_whisper_stt && st.ag_mic_silence_ms < 2500)
            vad_label += " (auto-raised for batch STT)";
    }
    std::cout << "llm agent (mic)\n"
              << "  device  : " << mic.opened_device_name() << "\n"
              << "  stt     : " << stt_label << vad_label << "\n"
              << "  llm     : " << prov.router << "  " << prov.model << "\n";
    std::string session_task_id;
    if (st.ag_multi_turn) {
        session_task_id = media::llm::agent::session_get_or_create(st.ag_session_id);
        std::cout << "  memory  : on (" << session_task_id << ")\n";
    } else {
        std::cout << "  memory  : off\n";
    }
    if (use_tts)
        std::cout << "  tts     : " << tts_provider
                  << (tts_model.empty()    ? "" : "  model=" + tts_model)
                  << (tts_voice_id.empty() ? "" : "  voice=" + tts_voice_id) << "\n";
    std::cout << "  enter   : empty line commits current speech now; typed text + Enter sends text\n"
              << "  stop    : Ctrl+C\n" << std::flush;

    auto hud = AgentCliHud::make(st.ag_hud, st.ag_hud_mode, prov.max_iterations);
    if (hud.active())
        hud.busy("listening");

    // ── TTS config ────────────────────────────────────────────────────────────
    pm::tts::ElevenLabsTTSConfig el_tts_cfg;
    el_tts_cfg.api_key  = tts_key;
    if (!tts_model.empty())    el_tts_cfg.model_id  = tts_model;
    if (!tts_voice_id.empty()) el_tts_cfg.voice_id  = tts_voice_id;

    pm::audio::AudioOutput speaker;

    // Drain pending display messages onto stdout.
    auto drain_print = [&] {
        std::lock_guard<std::mutex> lk(print_mtx);
        while (!print_queue.empty()) {
            std::cerr << "\r" << std::string(80, ' ') << "\r"; // clear partial line
            std::cout << print_queue.front() << "\n" << std::flush;
            print_queue.pop();
        }
    };

    // ── conversation loop ─────────────────────────────────────────────────────
    while (!media::cli::cancel_requested()) {
        // Device-unplug check: if the capture device was rerouted (unplugged), apply policy.
        if (mic.was_rerouted()) {
            if (avs.absent_policy == media::runtime_settings::DeviceAbsentPolicy::fail) {
                std::cerr << "\rllm agent mic: capture device unplugged (absent_policy=fail) — stopping.\n";
                media::cli::test_request_cancel();
                break;
            }
            // fallback_to_default: print once and continue on the system default.
            static bool reroute_noticed = false;
            if (!reroute_noticed) {
                reroute_noticed = true;
                std::cerr << "\rllm agent mic: capture device unplugged — fell back to system default.\n" << std::flush;
            }
        }

        std::string prompt;
        {
            std::unique_lock<std::mutex> lk(prompt_mtx);
            prompt_cv.wait_for(lk, chr::milliseconds(200),
                [&]{ return !prompt_queue.empty() || media::cli::cancel_requested(); });
            if (prompt_queue.empty()) {
                drain_print();
                // Show partial transcript in-place while listening.
                {
                    std::lock_guard<std::mutex> plk(print_mtx);
                    if (!last_partial.empty()) {
                        auto preview = last_partial.substr(0, 70);
                        std::cerr << "\r  [" << preview
                                  << (last_partial.size() > 70 ? "..." : "") << "]   "
                                  << std::flush;
                        if (hud.active())
                            hud.busy(preview);
                    }
                }
                continue;
            }
            prompt = std::move(prompt_queue.front());
            prompt_queue.pop();
        }

        drain_print(); // flush "[you] ..." before agent output

        if (hud.active())
            hud.busy("thinking");

        // Run one LLM turn with the spoken prompt.
        auto turn        = base_turn;
        turn.user_prompt = prompt;
        if (st.ag_multi_turn && !session_task_id.empty()) {
            nlohmann::json mem_state, recent_evs;
            media::llm::agent::session_load(session_task_id, mem_state, recent_evs, 20);
            turn.task_id       = session_task_id;
            turn.memory_state  = std::move(mem_state);
            turn.recent_events = std::move(recent_evs);
        }

        auto agent_cb = [&](const media::llm::agent::Event& e) -> bool {
            hud.on_event(e);
            using K = media::llm::agent::Event::Kind;
            switch (e.kind) {
            case K::LlmRound: {
                // Log compact per-round telemetry so it's visible without --log-level trace.
                std::string line = "[llm] round=" + std::to_string(e.payload.value("round", 0));
                if (e.payload.value("stateful", false))
                    line += " stateful";
                if (e.payload.contains("tokens") && e.payload["tokens"].is_object()) {
                    const auto& t = e.payload["tokens"];
                    if (t.contains("input"))
                        line += " in=" + t["input"].dump();
                    if (t.contains("output"))
                        line += " out=" + t["output"].dump();
                    if (t.contains("reasoning"))
                        line += " reasoning=" + t["reasoning"].dump();
                }
                if (e.payload.contains("model") && e.payload["model"].is_string())
                    line += " model=" + e.payload["model"].get<std::string>();
                logger::debug(line);
                break;
            }
            case K::Thinking: {
                logger::debug(std::string("[thinking] ") + e.text);
                break;
            }
            case K::ToolCall:
                std::cerr << "\r"
                          << media::llm::agent::format_agent_tool_call_cli_line(e.tool_name, e.payload)
                          << "                    \n";
                break;
            case K::ToolResult:
                std::cerr << "\r"
                          << media::llm::agent::format_agent_tool_result_cli_line(e.tool_name, e.text, e.payload)
                          << "                    \n";
                break;
            case K::Error:
                std::cerr << "[agent error] " << e.text << "\n";
                break;
            default: break;
            }
            return !media::cli::cancel_requested();
        };

        auto result = media::llm::agent::run_turn(turn, prov, agent_cb);
        append_session_turn_summary_if_enabled(st, session_task_id, turn.user_prompt, result,
                                               turn.structured_memory_features);
        if (result.cancelled) {
            if (hud.active())
                hud.busy("cancelled");
            break;
        }
        if (hud.active()) {
            if (!result.ok)
                hud.error(result.error.empty() ? "failed" : result.error);
            else
                hud.completed("done");
        }

        if (!result.final_text.empty()) {
            std::cout << "[agent] " << result.final_text << "\n" << std::flush;

            if (use_tts && !media::cli::cancel_requested()) {
                try {
                    std::vector<uint8_t> audio;
                    if (use_windows_tts) {
                        audio = pm::tts::windows_tts_synthesize(result.final_text, tts_voice_id);
                    } else if (pm::audio::constants::is_speech_proxy_provider(tts_provider)) {
                        const bool el_model = tts_model.empty() || tts_model == "eleven_v3"
                            || tts_model == "tanit-speech";
                        const std::string eff_model = tts_provider == "openai" && el_model
                            ? std::string(pm::audio::constants::k_openai_tts_model)
                            : (tts_provider == "openrouter" && el_model
                                   ? std::string(pm::audio::constants::k_openrouter_tts_model)
                                   : (tts_model.empty() ? std::string("tanit-speech") : tts_model));
                        const std::string eff_voice = pm::audio::constants::speech_tts_voice(
                            tts_provider, tts_voice_id);
                        audio = pm::tts::proxy_tts_synthesize(
                            result.final_text, tts_base_url, tts_key, eff_model, eff_voice, "mp3");
                    } else {
                        audio = pm::tts::elevenlabs_tts_synthesize(
                            result.final_text, el_tts_cfg);
                    }
                    if (!audio.empty() && !media::cli::cancel_requested()) {
                        tts_playing.store(true);
                        std::atomic<bool> play_done{false};
                        std::thread pt([&]{
                            speaker.play_sync(audio.data(), audio.size());
                            play_done.store(true);
                        });
                        // Safety timeout: never keep STT gated forever if audio playback stalls.
                        const auto play_deadline = clk::now() + chr::seconds(30);
                        while (!play_done.load() && !media::cli::cancel_requested()
                               && clk::now() < play_deadline)
                            std::this_thread::sleep_for(chr::milliseconds(30));
                        if (!play_done.load()) speaker.stop();
                        pt.join();
                        tts_playing.store(false);
                    }
                } catch (const std::exception& ex) {
                    tts_playing.store(false);
                    std::cerr << "[tts error] " << ex.what() << "\n";
                }
            }
        }
        if (hud.active())
            hud.busy("listening");
    }

    mic.stop();
    if (el_stt) el_stt->close();
    std::cerr << "\rllm agent mic: stopped                \n";
    return media::cli::cancel_requested() ? 130 : 0;
}

#if defined(FEATURE_REALTIME) && FEATURE_REALTIME
// ── Live speech-to-speech voice session (CLI) ──────────────────────────────────
// Host maps CLI flags → JSON + C callbacks. VoiceAgent lives in the chat
// plugin; lookup is FindComponent(llm.chat) only (no DELAYLOAD / dllimport).
int cmd_llm_agent_realtime(PmImageCliState& st,
                                  const media::runtime_settings::ChatProviderSettings& chat,
                                  pm::cli::CliActivityScope* cli_activity)
{
    if (!pm::plugins::chat_host::available()) {
        logger::error("llm agent --realtime: chat add-on is not installed");
        return 1;
    }

    std::string input_source = "mic";
    {
        const std::string& src = st.ag_input_source;
        if (src == "desktop" || src == "mix")
            input_source = src;
        else if (!st.ag_desktop_device.empty() && src != "mic")
            input_source = "desktop";
    }

    nlohmann::json opts{
        {"preset", st.ag_preset},
        {"router", st.ag_router},
        {"model", st.ag_model},
        {"api_key", st.ag_api_key},
        {"base_url", st.ag_base_url},
        {"timeout_ms", st.ag_timeout_ms},
        {"max_iterations", st.ag_max_iter},
        {"streaming", st.ag_streaming},
        {"planner", st.ag_planner},
        {"runner", st.ag_runner},
        {"selection", st.ag_paths},
        {"folder_hint", st.cwd},
        {"voice", chat.realtime_voice},
        {"mic", st.ag_mic_input},
        {"input_source", input_source},
        {"desktop_device", st.ag_desktop_device},
        {"mic_gain", st.ag_mic_gain},
        {"desktop_gain", st.ag_desktop_gain},
        {"virtual_input_override", true},
    };
    if (st.ag_multi_turn || !st.ag_session_id.empty())
        opts["session_id"] = media::llm::agent::session_get_or_create(st.ag_session_id);

    if (!st.ag_embed_paths.empty()) {
        std::string embed_err;
        std::string embed_suffix;
        if (!media_cli::append_agent_embedded_prompt(
                std::string{}, st.ag_embed_paths, embed_suffix, embed_err)) {
            logger::error(std::string("llm agent --realtime: ") + embed_err);
            return 1;
        }
        opts["embed_suffix"] = std::move(embed_suffix);
    }

    auto hud = AgentCliHud::make(st.ag_hud, st.ag_hud_mode, st.ag_max_iter);
    if (hud.active())
        hud.busy("connecting");

    struct RtCli {
        std::string asst;
        AgentCliHud* hud = nullptr;
    } ud;
    ud.hud = hud.active() ? &hud : nullptr;

    const PmChatRtCallbacks cb{
        [](void* p) {
            std::cout << "llm agent --realtime: live - speak now (Ctrl+C to stop)\n" << std::flush;
            if (auto* h = static_cast<RtCli*>(p)->hud)
                h->busy("listening");
        },
        nullptr,
        [](const char* d, void* p) {
            auto* s = static_cast<RtCli*>(p);
            if (d)
                s->asst += d;
            if (s->hud && d && d[0])
                s->hud->busy("speaking");
        },
        [](const char* t, int fin, void* p) {
            if (fin && t && t[0]) {
                std::cout << "[you] " << t << "\n" << std::flush;
                if (auto* h = static_cast<RtCli*>(p)->hud)
                    h->busy("thinking");
            }
        },
        [](void* p) {
            auto* s = static_cast<RtCli*>(p);
            if (!s->asst.empty()) {
                std::cout << "[assistant] " << s->asst << "\n" << std::flush;
                s->asst.clear();
            }
            if (s->hud)
                s->hud->busy("listening");
        },
        [](const char* e, void* p) {
            if (e)
                logger::error(std::string("[realtime error] ") + e);
            if (auto* h = static_cast<RtCli*>(p)->hud)
                h->error(e && e[0] ? e : "failed");
        },
        [](void* p) {
            logger::info("llm agent --realtime: session closed");
            if (auto* h = static_cast<RtCli*>(p)->hud)
                h->completed("stopped");
        },
        [](const char* kind, const char* text, const char* tool, void* p) {
            const std::string k = kind ? kind : "";
            auto* h = static_cast<RtCli*>(p)->hud;
            if (k == "tool_call") {
                logger::info(std::string("[agent tool] ") + (tool ? tool : ""));
                if (h)
                    h->busy(tool && tool[0] ? tool : "tool");
            } else if (k == "assistant_text" && text && text[0]) {
                std::cout << "[agent] " << text << "\n" << std::flush;
                if (h)
                    h->busy("speaking");
            } else if (k == "error" && text) {
                logger::error(std::string("[agent error] ") + text);
                if (h)
                    h->error(text);
            }
        },
        [](const char*, const char* goal, void* p) {
            logger::info(std::string("[agent] handover -> ") + (goal ? goal : ""));
            if (auto* h = static_cast<RtCli*>(p)->hud)
                h->busy("working");
        },
        [](void*) { return media::cli::cancel_requested() ? 1 : 0; },
        &ud,
    };

    pm::plugins::chat_host::RealtimeSession sess;
    {
        std::string ierr;
        if (!sess.open(opts, cb, ierr)) {
            logger::error("llm agent --realtime: " + ierr);
            if (hud.active())
                hud.error(ierr.empty() ? "failed" : ierr);
            return 1;
        }
    }
    const nlohmann::json cfg = sess.config();
    const std::string model = cfg.value("model", std::string{});
    const std::string voice = cfg.value("voice", std::string{});
    const std::string mic = cfg.value("mic_device", std::string{});
    const std::string speaker = cfg.value("speaker_device", std::string{});

#if defined(FEATURE_COMMAND_IPC_NEXT) && FEATURE_COMMAND_IPC_NEXT
    media::llm::agent_ipc::Registration agent_reg;
    {
        media::llm::agent_ipc::AgentInfo info;
        info.kind       = media::llm::agent_ipc::AgentKind::Realtime;
        info.cwd        = st.cwd;
        info.prompt     = st.ag_prompt;
        info.session_id = sess.session_id();
        info.model      = model;
        std::string reg_err;
        if (!agent_reg.begin(info, reg_err))
            logger::debug(std::string("[agent_ipc] register failed: ") + reg_err);
    }

    if (cli_activity) {
        cli_activity->set_extra_action_handler([&sess](const nlohmann::json& req) {
            const std::string action = req.value("action", std::string{});
            if (action != "realtime_text" && action != "realtime.send_text")
                return nlohmann::json{};

            const std::string text = req.value("text", req.value("prompt", std::string{}));
            if (text.empty()) {
                return nlohmann::json{
                    {"type", "error"},
                    {"error", "realtime_text requires text"},
                };
            }
            if (!sess.is_active()) {
                return nlohmann::json{
                    {"type", "error"},
                    {"error", "realtime voice session is not active"},
                };
            }
            sess.send_text(text);
            return nlohmann::json{
                {"type", "realtime_text_ack"},
                {"textBytes", static_cast<int>(text.size())},
            };
        });
    }
#endif

    media::cli::install_cli_interrupt_handlers();

    std::cout << "llm agent --realtime\n"
              << "  model   : " << (model.empty() ? "(default)" : model) << "\n"
              << "  voice   : " << (voice.empty() ? "(server default)" : voice) << "\n"
              << "  mic     : " << (mic.empty() ? "(system default)" : mic) << "\n"
              << "  speaker : " << (speaker.empty() ? "(system default)" : speaker) << "\n"
              << "  session : " << sess.session_id() << "\n"
              << "  connecting...\n" << std::flush;

    nlohmann::json policy{
        {"seed_prompt", st.ag_prompt},
        {"idle_auto_stop_ms", st.ag_realtime_idle_ms},
    };
    std::string run_err;
    const int result = sess.run(policy, run_err);
#if defined(FEATURE_COMMAND_IPC_NEXT) && FEATURE_COMMAND_IPC_NEXT
    if (cli_activity)
        cli_activity->set_extra_action_handler(nullptr);
#endif
    if (result == 1) {
        if (!run_err.empty())
            logger::error("llm agent --realtime: " + run_err);
        if (hud.active())
            hud.error(run_err.empty() ? "failed" : run_err);
        return 1;
    }
    if (hud.active())
        hud.completed("stopped");
    logger::info("llm agent --realtime: stopped");
    return result == 2 ? 130 : 0;
}
#endif // FEATURE_REALTIME

#endif // FEATURE_STT
} // namespace pm_llm_agent_cli

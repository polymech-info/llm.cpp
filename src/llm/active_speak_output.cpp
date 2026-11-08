// Host-local TTS playback registry for builds with no chat surface
// (Tanit Viewer: FEATURE_COMMAND_LLM / FEATURE_CHAT_WEB / FEATURE_REALTIME off).
//
// audio_blocks + the audio CLI call register/unregister so Ctrl+C and overlay
// stop can abort play_sync. Those symbols normally live in path_tool_executor
// (in-process chat) or host_chat_exports (tanit-chat-plugin). Do not compile
// this TU when a chat surface is on — you would get duplicate symbols.

#include "llm/path_tool_executor.hpp"

#include "core/cli_cancel.hpp"
#include "core/audio.hpp"
#include "logging/logger.h"

#include <atomic>

namespace media::llm::path {

namespace {
std::atomic<pm::audio::AudioOutput*> g_tts_out{nullptr};

struct CancelListenerRegistrar {
    CancelListenerRegistrar() {
        media::cli::register_cancel_listener([] { abort_active_speak(); });
    }
};
[[maybe_unused]] static const CancelListenerRegistrar g_cancel_listener_registrar;
} // namespace

void abort_active_speak() {
#if PM_HAS_AUDIO_IO
    auto* p = g_tts_out.load(std::memory_order_acquire);
    logger::trace(std::string("[cancel] abort_active_speak: tts_out=")
                  + (p ? "present → stop()" : "null (nothing to stop)"));
    if (p)
        p->stop();
#else
    logger::trace("[cancel] abort_active_speak: no audio IO (no-op)");
#endif
}

void pause_active_speak() {
#if PM_HAS_AUDIO_IO
    if (auto* p = g_tts_out.load(std::memory_order_acquire))
        p->pause();
#endif
}

void resume_active_speak() {
#if PM_HAS_AUDIO_IO
    if (auto* p = g_tts_out.load(std::memory_order_acquire))
        p->resume();
#endif
}

void register_active_speak_output(pm::audio::AudioOutput* out) noexcept {
    g_tts_out.store(out, std::memory_order_release);
}

void unregister_active_speak_output(pm::audio::AudioOutput* expected) noexcept {
    auto* p = g_tts_out.load(std::memory_order_acquire);
    if (p == expected)
        g_tts_out.store(nullptr, std::memory_order_release);
}

} // namespace media::llm::path

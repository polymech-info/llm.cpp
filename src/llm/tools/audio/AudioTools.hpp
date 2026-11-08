// Copyright PolyMech / pm-image. Internal use only.
//
// `speak` (TTS → speaker, optional .mp3/.wav/.opus dst + .txt transcript),
// `play_file` (play an existing audio file on the speaker), and
// `audio_transcribe` (mic capture → STT → text + optional .wav/.txt outputs).
//
// Both are FEATURE_STT-gated; the helper `abort_active_speak_impl()` is also
// FEATURE_STT-gated and wires into the public `abort_active_speak()` declared
// in path_tool_executor.hpp.
#pragma once

#include "llm/tools/commons/PathToolCommons.hpp"

#include <string>
#include <vector>

namespace pm::audio { class AudioOutput; }

namespace media::llm::path {
struct ToolDef;
}

namespace media::llm::path::detail {

struct SpeakAudioResult {
    bool ok = false;
    std::string error;
    std::string provider;
    std::string model;
    std::string mime_type;
    std::string format;
    std::vector<unsigned char> audio;
};

ExecuteResult do_speak(const nlohmann::json& args);
ExecuteResult do_play_file(const nlohmann::json& args);
SpeakAudioResult do_speak_audio_buffer(const std::string& text,
                                       const std::string& extension = ".mp3",
                                       const nlohmann::json& args = nlohmann::json::object());
ExecuteResult do_audio_transcribe(const nlohmann::json& args);
ExecuteResult do_audio_transcribe_file(const std::string& audio_path, const nlohmann::json& args = nlohmann::json::object());
ExecuteResult do_audio_transcribe_buffer(const std::vector<unsigned char>& audio_bytes,
                                         const std::string& extension,
                                         const nlohmann::json& args = nlohmann::json::object());

ToolDef make_speak();
ToolDef make_play_file();
ToolDef make_audio_transcribe();

/// Stop the currently-playing `speak` synthesis (any thread). No-op when
/// FEATURE_STT is off or when no TTS is in flight. Wired into the global CLI
/// cancel chain from path_tool_executor.cpp::execute_impl.
void abort_active_speak_impl();
void pause_active_speak_impl();
void resume_active_speak_impl();

void set_active_speak_output(pm::audio::AudioOutput* out) noexcept;
void clear_active_speak_output(pm::audio::AudioOutput* expected) noexcept;

} // namespace media::llm::path::detail

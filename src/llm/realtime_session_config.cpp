#include "llm/realtime_session_config.hpp"

#if defined(FEATURE_STT) && FEATURE_STT && defined(FEATURE_REALTIME) && FEATURE_REALTIME

#include "llm/agent_internal.hpp"
#include "llm/realtime_agent_bridge.hpp"
#include "core/settings_runtime.hpp"

namespace media::llm::agent::realtime {

namespace {

// Built-in persona used when prompts/realtime-prompt.md is absent. Same ${VAR}
// grammar as the file-based prompt; rendered below either way.
constexpr const char* k_default_persona =
    "You are ${APP_NAME}'s voice assistant. Keep replies short and natural. "
    "Handle greetings and small talk directly. For ANYTHING else - finding, "
    "inspecting, creating, editing, or converting files/images/video, running "
    "commands, or answering factual questions - first say a brief filler like "
    "\"one moment\", then call run_pm_agent with a single clear sentence as the "
    "goal, and read its returned summary back. Never claim you did something "
    "without calling run_pm_agent. If a task is running and the user says to "
    "stop it, call stop_pm_agent.";

// Live-context note appended to every persona (explicit override included):
// the voice model cannot see files itself, but run_pm_agent resolves the
// user's CURRENT selection/folder — so "this/these" must be delegated.
std::string build_context_note(const std::vector<std::string>& selection,
                               const std::string& folder_hint) {
    std::string note =
        "\n\nIMPORTANT: You cannot open or view file contents yourself. "
        "run_pm_agent can, and it always sees the user's CURRENT selection and "
        "folder. When the user says \"this\", \"these\", \"the file/image/video\", "
        "or refers to something on screen, do NOT say you can't see it — call "
        "run_pm_agent and let it resolve the selection.";
    if (!selection.empty()) {
        note += "\nRight now the user has " + std::to_string(selection.size())
              + (selection.size() == 1 ? " file" : " files") + " selected:";
        size_t shown = 0;
        for (const auto& p : selection) {
            if (shown++ >= 6) { note += "\n  - \u2026"; break; }
            const auto slash = p.find_last_of("/\\");
            note += "\n  - " + (slash == std::string::npos ? p : p.substr(slash + 1));
        }
    } else if (!folder_hint.empty()) {
        note += "\nCurrent folder: " + folder_hint + ".";
    }
    return note;
}

} // namespace

void apply_session_defaults(pm::audio::RealtimeVoiceSession::Config& cfg,
                            const SetupOptions& opt) {
    // ── Tools: the full agent exposed as run_pm_agent / stop_pm_agent ────────
    cfg.tools = nlohmann::json::array(
        { RealtimeAgentBridge::run_pm_agent_tool_schema(),
          RealtimeAgentBridge::stop_pm_agent_tool_schema(),
          RealtimeAgentBridge::suspend_voice_tool_schema() });

    // ── Persona: override → realtime-prompt.md → built-in, ${VAR}-rendered ───
    if (!opt.instructions_override.empty()) {
        cfg.instructions = opt.instructions_override;
    } else {
        Turn pturn;
        pturn.folder_hint = opt.folder_hint;
        pturn.selection   = opt.selection;
        std::string raw = detail::load_realtime_prompt_md_if_present();
        if (raw.empty()) raw = k_default_persona;
        cfg.instructions = detail::render_prompt_file(raw, pturn);
    }
    cfg.instructions += build_context_note(opt.selection, opt.folder_hint);

    // ── Voice: override → chat settings realtime_voice ───────────────────────
    cfg.voice = opt.voice_override;
    if (cfg.voice.empty()) {
        media::runtime_settings::ChatProviderSettings cps;
        std::string cerr;
        if (media::runtime_settings::load_chat_provider(cps, cerr)
            && !cps.realtime_voice.empty())
            cfg.voice = cps.realtime_voice;
    }

    // ── Devices: Audio & Video settings; explicit mic override wins. The
    // realtime backend validates both endpoints before startup so a wrong
    // system default cannot be used silently. ───────────────────────────────
    media::runtime_settings::AudioVideoDeviceSettings avs;
    std::string avs_err;
    media::runtime_settings::load_audio_video_device_settings(avs, avs_err);
    cfg.mic_device     = media::runtime_settings::resolve_audio_input_device(opt.mic_override, avs);
    cfg.speaker_device = media::runtime_settings::resolve_audio_output_device(opt.speaker_override, avs);

    // ── Virtual input: explicit surface override → persisted settings ─────
    if (opt.virtual_input_override) {
        cfg.input_source   = opt.input_source;
        cfg.desktop_device = opt.desktop_device;
        cfg.mic_gain       = opt.mic_gain;
        cfg.desktop_gain   = opt.desktop_gain;
    } else {
        using IS = pm::audio::RealtimeVoiceSession::Config::InputSource;
        cfg.input_source = avs.audio_input_source == "desktop" ? IS::desktop
                         : avs.audio_input_source == "mix"     ? IS::mix
                                                               : IS::mic;
        cfg.desktop_device = avs.desktop_audio_device;
        cfg.mic_gain       = avs.mic_gain;
        cfg.desktop_gain   = avs.desktop_gain;
    }
}

} // namespace media::llm::agent::realtime

#endif // FEATURE_STT && FEATURE_REALTIME

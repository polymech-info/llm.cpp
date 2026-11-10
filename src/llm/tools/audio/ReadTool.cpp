// Copyright PolyMech / pm-image. Internal use only.
//
// `read` opens a document, keeps the reader page cache, and writes one audio file.
// Long text is chunked and joined. Android uses the phone voice and still writes one wav.

#include "llm/tools/audio/AudioTools.hpp"

#include "pm/audio/tts/reader.hpp"

#include "core/settings_runtime.hpp"
#include "core/settings_store.hpp"
#include "fs/path_sanitizer.hpp"
#include "llm/agent.hpp"
#include "llm/agent_launch.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(__ANDROID__)
#include "pm_android_host.hpp"
#else
#include "html/html.h"
#include "net/url_fetch.hpp"
#if defined(FEATURE_COMMAND_MARKDOWN) && FEATURE_COMMAND_MARKDOWN
#include "markdown/markdown.hpp"
#endif
#if defined(FEATURE_STT) && FEATURE_STT
#include "audio/audio_constants.hpp"
#include "audio/speech_factory.hpp"
#include "core/audio_devices.hpp"
#include "logging/logger.h"
#include "tts/proxy_tts.hpp"
#endif
#endif

namespace media::llm::path::detail {
namespace {

namespace fs = std::filesystem;

std::string read_bytes(const fs::path& file, std::string& err) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        err = "cannot read " + media::path::path_to_utf8(file);
        return {};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

std::string read_prefix(const fs::path& file, std::size_t n) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return {};
    std::string buf(n, '\0');
    in.read(buf.data(), static_cast<std::streamsize>(n));
    buf.resize(static_cast<std::size_t>(in.gcount() > 0 ? in.gcount() : 0));
    return buf;
}

bool write_bytes(const fs::path& file, const std::string& text, std::string& err) {
    std::error_code ec;
    if (file.has_parent_path())
        fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "cannot write " + file.generic_string();
        return false;
    }
    out << text;
    if (!text.empty() && text.back() != '\n')
        out << '\n';
    if (!out) {
        err = "cannot write " + file.generic_string();
        return false;
    }
    return true;
}

std::string file_mtime(const fs::path& file) {
    std::error_code ec;
    const auto stamp = fs::last_write_time(file, ec);
    if (ec)
        return {};
    return std::to_string(stamp.time_since_epoch().count());
}

bool arg_bool(const nlohmann::json& args, const char* key, bool fallback) {
    if (args.contains(key) && args[key].is_boolean())
        return args[key].get<bool>();
    const auto& opt = options_or_empty(args);
    if (opt.contains(key) && opt[key].is_boolean())
        return opt[key].get<bool>();
    return fallback;
}

std::string arg_string(const nlohmann::json& args, const char* key) {
    if (auto s = get_nonempty_string(args, key))
        return *s;
    if (auto s = get_nonempty_string(options_or_empty(args), key))
        return *s;
    return {};
}

std::string rewrite_spoken(const std::string& page, const std::string& prompt,
                           const std::string& language, const std::string& preset,
                           std::string& err) {
    media::llm::agent::ProviderSettings provider;
    if (!preset.empty()) {
        media::llm::agent::launch::AgentLaunchSpec spec;
        spec.preset = preset;
        spec.no_tools = true;
        media::llm::agent::launch::AgentLaunchResolved resolved;
        if (!media::llm::agent::launch::resolve_agent_launch(spec, resolved, err))
            return {};
        provider = resolved.provider;
    } else {
        media::runtime_settings::ChatProviderSettings chat;
        std::string load_err;
        media::runtime_settings::load_chat_provider(chat, load_err);
        provider.router = chat.router.empty() ? "tanit" : chat.router;
        provider.model = chat.model.empty() ? "text-fast" : chat.model;
        provider.api_key = chat.api_key;
        provider.base_url = chat.base_url;
        provider.timeout_ms = chat.timeout_ms > 0 ? chat.timeout_ms : 60'000;
        if (provider.api_key.empty() || provider.base_url.empty())
            media::runtime_settings::merge_provider_credentials(
                provider.router, false, provider.api_key, provider.base_url);
    }
    provider.max_iterations = 0;
    if (provider.router.empty() || provider.model.empty()) {
        err = "no chat model configured";
        return {};
    }
    media::llm::agent::Result result;
    try {
        result = media::llm::agent::run_spoken_page(page, prompt, language, provider);
    } catch (const std::exception& ex) {
        err = ex.what();
        return {};
    }
    if (result.cancelled) {
        err = "cancelled";
        return {};
    }
    if (!result.ok || result.final_text.empty()) {
        err = result.error.empty() ? "prompt returned no text" : result.error;
        return {};
    }
    return result.final_text;
}

bool open_store(const std::string& raw, bool refresh, pm::tts::reader::Store& store, std::string& err) {
#if defined(__ANDROID__)
    if (raw.rfind("http://", 0) == 0 || raw.rfind("https://", 0) == 0) {
        err = "web pages are not available in this build";
        return false;
    }
#else
    const bool is_url = media::is_http_url(raw);
#endif
    std::string key = raw;
    std::string extension;
    std::string mtime;
    fs::path file;
#if !defined(__ANDROID__)
    if (!is_url) {
#endif
        file = fs::path(resolve_tool_path_string(raw));
        std::error_code ec;
        if (!fs::is_regular_file(file, ec) || ec) {
            err = "path is not a file";
            return false;
        }
        const std::string abs = media::path::path_to_utf8(file);
        if (const std::string deny = llm_path_fs_deny(abs); !deny.empty()) {
            err = deny;
            return false;
        }
        const fs::path canon = fs::weakly_canonical(file, ec);
        key = media::path::path_to_utf8(ec ? file : canon);
        extension = media::path::path_to_utf8(file.extension());
        mtime = file_mtime(ec ? file : canon);
#if !defined(__ANDROID__)
    }
    pm::tts::reader::Interpreter interpreter = is_url
        ? pm::tts::reader::interpreter_for_url(key)
        : pm::tts::reader::interpreter_for_extension(extension);
#else
    pm::tts::reader::Interpreter interpreter = pm::tts::reader::interpreter_for_extension(extension);
    if (interpreter == pm::tts::reader::Interpreter::Web
        || interpreter == pm::tts::reader::Interpreter::Anydoc
        || interpreter == pm::tts::reader::Interpreter::ImageUnderstand) {
        err = interpreter == pm::tts::reader::Interpreter::ImageUnderstand
            ? "images are not spoken by this tool"
            : "this build has no document loader";
        return false;
    }
#endif
    if (interpreter == pm::tts::reader::Interpreter::None) {
        err = "no interpreter for " + extension;
        return false;
    }
#if !defined(__ANDROID__)
    if (interpreter == pm::tts::reader::Interpreter::ImageUnderstand) {
        err = "images are not spoken by this tool";
        return false;
    }
#endif

    const std::string root = (media::settings::get_config_dir() / "reader").generic_string();
    const std::string dir = pm::tts::reader::store_directory(root, key);
    if (!refresh && pm::tts::reader::load_store(dir, store, err)
        && store.hash == pm::tts::reader::document_hash(key)
        && store.mtime == mtime) {
        store.cached = true;
        err.clear();
        return store.page_count > 0;
    }
    err.clear();

    std::string source;
#if !defined(__ANDROID__)
    fs::path fetched;
    struct FetchedFile {
        fs::path path;
        ~FetchedFile() {
            if (!path.empty())
                fs::remove(path);
        }
    } fetched_file;
    if (is_url) {
        std::error_code ec;
        fetched = fs::temp_directory_path(ec);
        if (ec) {
            err = "no temp directory";
            return false;
        }
        fetched /= "tanit-reader-" + pm::tts::reader::document_hash(key);
        fetched_file.path = fetched;
        if (!media::fetch_url_to_file(key, fetched, 30, 8, err)) {
            fetched_file.path.clear();
            if (err.empty())
                err = "fetch failed";
            return false;
        }
        interpreter = pm::tts::reader::interpreter_for_url(key, read_prefix(fetched, 4096));
        if (interpreter == pm::tts::reader::Interpreter::None) {
            err = "no interpreter for " + key;
            return false;
        }
        if (interpreter == pm::tts::reader::Interpreter::ImageUnderstand) {
            err = "images are not spoken by this tool";
            return false;
        }
    }
    const fs::path source_file = fetched.empty() ? file : fetched;
    if (interpreter == pm::tts::reader::Interpreter::Web) {
        source = read_bytes(source_file, err);
        if (!err.empty())
            return false;
        source = html::to_markdown(html::clean_for_markdown(source));
        if (source.empty()) {
            err = "page has no readable text";
            return false;
        }
    } else if (interpreter == pm::tts::reader::Interpreter::Anydoc) {
#if defined(FEATURE_COMMAND_MARKDOWN) && FEATURE_COMMAND_MARKDOWN
        const media::markdown::Result converted =
            media::markdown::to_markdown(source_file, media::markdown::Format::Detect);
        if (!converted.ok()) {
            err = converted.error.empty() ? "conversion failed" : converted.error;
            if (converted.status == media::markdown::Status::NeedsOcr)
                err += "; scanned pages need a page renderer";
            return false;
        }
        source = converted.text;
#else
        err = "this build has no document loader";
        return false;
#endif
    } else {
        source = read_bytes(source_file, err);
        if (!err.empty())
            return false;
    }
#else
    if (interpreter != pm::tts::reader::Interpreter::Text) {
        err = "this build has no document loader";
        return false;
    }
    source = read_bytes(file, err);
    if (!err.empty())
        return false;
#endif

    store = {};
    store.root = root;
    store.path = key;
    store.hash = pm::tts::reader::document_hash(key);
    store.interpreter = pm::tts::reader::interpreter_name(interpreter);
    store.mtime = mtime;
    store.page = 1;
    if (!pm::tts::reader::write_pages(dir, store, source, err))
        return false;
    return store.page_count > 0;
}

} // namespace

ExecuteResult do_read(const nlohmann::json& args) {
    const std::string raw = arg_string(args, "path");
    if (raw.empty())
        return fatal("read: 'path' is required");
    const std::string prompt = arg_string(args, "prompt");
    const std::string preset = arg_string(args, "chat_preset");
    if (!preset.empty() && prompt.empty())
        return fatal("read: prompt is required with chat_preset");

    pm::tts::reader::Store store;
    std::string err;
    if (!open_store(raw, arg_bool(args, "refresh", false), store, err) || store.page_count < 1) {
        if (err.empty())
            err = "no pages";
        return fatal("read: " + err);
    }
    std::string text = pm::tts::reader::document_speak_text(store, err);
    if (text.empty())
        return fatal("read: " + (err.empty() ? "no speakable text" : err));
    if (!prompt.empty()) {
        text = rewrite_spoken(text, prompt, arg_string(args, "language"), preset, err);
        if (text.empty())
            return fatal("read: " + (err.empty() ? "prompt returned no text" : err));
    }
    const fs::path script = fs::path(store.dir) / "spoken.md";
    if (!write_bytes(script, text, err))
        return fatal("read: " + err);

    const bool play = arg_bool(args, "play", false);
    std::string dst = arg_string(args, "dst");
#if defined(__ANDROID__)
    if (dst.empty())
        dst = (fs::path(store.dir) / "document.wav").generic_string();
    else {
        dst = resolve_tool_path_string(dst);
        if (const std::string deny = llm_path_fs_deny(dst); !deny.empty())
            return fatal("read: dst: " + deny);
        if (const std::string deny = llm_path_write_deny(dst); !deny.empty())
            return fatal("read: dst: " + deny);
    }
    if (!pm::android::device_speak(text, play, dst, err))
        return fatal(err.empty() ? "read: phone speech failed" : err);
    return simple_ok({
        {"tool", "read"},
        {"provider", "android"},
        {"path", store.path},
        {"interpreter", store.interpreter},
        {"pages", store.page_count},
        {"chars", static_cast<int>(text.size())},
        {"dst", dst},
        {"format", "wav"},
        {"played", play},
        {"script", script.generic_string()},
    });
#elif !(defined(FEATURE_STT) && FEATURE_STT)
    (void)play;
    (void)dst;
    return fatal("read: speech is not built");
#else
    if (dst.empty())
        dst = (fs::path(store.dir) / "document.mp3").generic_string();
    else {
        dst = resolve_tool_path_string(dst);
        if (const std::string deny = llm_path_fs_deny(dst); !deny.empty())
            return fatal("read: dst: " + deny);
        if (const std::string deny = llm_path_write_deny(dst); !deny.empty())
            return fatal("read: dst: " + deny);
    }

    media::runtime_settings::ChatProviderSettings chat;
    media::runtime_settings::ProviderMap providers;
    std::string load_err;
    media::runtime_settings::load_chat_provider(chat, load_err);
    media::runtime_settings::load_providers(providers, load_err);
    const std::string settings_provider = chat.tts_provider.empty() ? "tanit" : chat.tts_provider;
    const pm::audio::SpeechProvider provider = pm::audio::resolve_speech_provider(
        {}, {}, settings_provider, providers);
    if (provider.api_key.empty() || provider.base_url.empty())
        return fatal("read: no voice provider key");
    if (!pm::audio::constants::is_speech_proxy_provider(provider.provider))
        return fatal("read: speech uses the voice proxy");
    std::string model = chat.tts_model;
    if (model.empty() || model == "eleven_v3" || model == "tanit-speech")
        model = "tanit-speech-v4";
    std::string voice = chat.tts_voice_id;
    if (voice.empty() || voice == pm::audio::constants::k_elevenlabs_voice_id)
        voice = pm::audio::constants::k_proxy_default_voice;

    const pm::tts::DocumentSpeech speech = pm::tts::proxy_tts_document(
        text, provider.base_url, provider.api_key, model, voice, true, 400, 800,
        [](const pm::tts::SpeechProgress& step) {
            pm::tts::reader::WorkProgress progress;
            progress.phase = "speak";
            progress.index = static_cast<std::int64_t>(step.index);
            progress.total = static_cast<std::int64_t>(step.count);
            progress.done = static_cast<std::int64_t>(step.chars_done);
            progress.whole = static_cast<std::int64_t>(step.chars_total);
            progress.elapsed_ms = step.elapsed_ms;
            logger::info(std::string("[read] ") + pm::tts::reader::format_progress(progress));
        });
    if (speech.cancelled)
        return fatal("read: cancelled");
    if (!speech.error.empty() || speech.audio.empty())
        return fatal("read: " + (speech.error.empty() ? "server returned empty audio" : speech.error));

    fs::path dst_path(dst);
    if (speech.format == "wav" && dst_path.extension() == ".mp3")
        dst_path.replace_extension(".wav");
    {
        std::error_code ec;
        if (dst_path.has_parent_path())
            fs::create_directories(dst_path.parent_path(), ec);
        std::ofstream out(dst_path, std::ios::binary | std::ios::trunc);
        if (!out)
            return fatal("read: cannot write " + dst_path.generic_string());
        out.write(reinterpret_cast<const char*>(speech.audio.data()),
                  static_cast<std::streamsize>(speech.audio.size()));
        if (!out)
            return fatal("read: cannot write " + dst_path.generic_string());
    }
    if (play) {
        pm::audio::AudioOutput player;
        try {
            set_active_speak_output(&player);
            player.play_sync(speech.audio.data(), speech.audio.size());
            clear_active_speak_output(&player);
        } catch (const std::exception& ex) {
            clear_active_speak_output(&player);
            return fatal(std::string("read: saved ") + dst_path.generic_string() + " playback: " + ex.what());
        }
    }
    return simple_ok({
        {"tool", "read"},
        {"provider", provider.provider},
        {"model", model},
        {"path", store.path},
        {"interpreter", store.interpreter},
        {"pages", store.page_count},
        {"chars", static_cast<int>(text.size())},
        {"dst", dst_path.generic_string()},
        {"format", speech.format},
        {"played", play},
        {"script", script.generic_string()},
    });
#endif
}

ToolDef make_read() {
    return ToolDef{
        "read",
        "Read a document or web page and write one audio file. "
        "Use when the user asks to read a file, book, pdf, or page aloud as one recording. "
        "Long text is split, spoken in order, and joined. One chunk stays mp3. "
        "Several chunks, and the phone voice, are one wav. "
        "`speak` is for a short line. `read` is for a file.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {
                    {"type", "string"},
                    {"description",
                     "Local document or http(s) URL. Text and markdown are spoken directly. "
                     "PDF, office, and epub are converted first. The original pages stay in the reader cache."},
                }},
                {"prompt", {
                    {"type", "string"},
                    {"description",
                     "Optional rewrite of the whole document before speech. "
                     "Example: summarize, in german. One text turn, no tools."},
                }},
                {"chat_preset", {
                    {"type", "string"},
                    {"description", "Saved chat preset for the rewrite. Requires prompt."},
                }},
                {"language", {
                    {"type", "string"},
                    {"description", "Spoken language passed to the rewrite."},
                }},
                {"dst", {
                    {"type", "string"},
                    {"description",
                     "Audio file to write. Empty writes document.mp3 in the reader cache, "
                     "or document.wav when the speech is joined."},
                }},
                {"play", {
                    {"type", "boolean"},
                    {"description", "Also play the file. Default false."},
                    {"default", false},
                }},
                {"refresh", {
                    {"type", "boolean"},
                    {"description", "Convert the document again and replace the cached pages."},
                    {"default", false},
                }},
            }},
            {"required", nlohmann::json::array({"path"})},
        }};
}

} // namespace media::llm::path::detail

// Copyright PolyMech / pm-image. Internal use only.
//
// Path-mode tool dispatcher. After the May 2026 refactor the per-tool bodies
// + JSON Schemas live under `src/llm/tools/<category>/`:
//
//   commons/    PathToolCommons.cpp  – helpers, fatal/simple_*, thread-locals
//   fs/         FsTools.cpp          – list_images, file_glob, file_read,
//                                       file_search, write_file, file_compose,
//                                       file_str_replace, file_delete
//   pictures/   PictureTools.cpp     – image_resize, image_crop, image_transform,
//                                       image_create, image_understand, image_from_camera
//                                       + internal image_compress / image_meta
//                                       + the transform file-progress hook
//   video/      VideoTools.cpp       – create_video (+ replicate enrichment)
//   audio/      AudioTools.cpp       – speak, play_file, audio_transcribe (FEATURE_STT)
//                                       + abort_active_speak_impl + g_tts_out
//   memory/     MemoryTools.cpp      – memory_read / write / append_event / find
//   scheduler/  SchedulerTools.cpp   – schedule_at / in / every / cancel / list
//   service/    ServiceTools.cpp     – service_* Tanit CMS / VFS API (FEATURE_INSTALLER_VFS)
//   inquire/    InquireTools.cpp     – ask_user (FTXUI + reply-file backend)
//   mcp/        McpBridge.cpp        – MCP bridge (loaded by agent_feature)
//
// This file is now the wiring layer:
//   - public setters / clearers (set_agent_path_base, set_agent_tool_blocklist, …)
//   - the tool name → category dispatcher (execute_impl / execute)
//   - cancel-listener bootstrap (abort_active_speak / abort_active_run)
//
#include "llm/path_tool_executor.hpp"

#include "llm/change_store.hpp"
#include "llm/tools/commons/PathToolCommons.hpp"
#include "llm/tools/audio/AudioTools.hpp"
#include "llm/tools/fs/FsTools.hpp"
#include "llm/tools/inquire/InquireTools.hpp"
#include "llm/tools/memory/MemoryTools.hpp"
#include "llm/tools/pictures/PictureTools.hpp"
#if defined(FEATURE_SCHEDULER_TOOL) && FEATURE_SCHEDULER_TOOL
#include "llm/tools/scheduler/SchedulerTools.hpp"
#endif
#include "llm/tools/video/VideoTools.hpp"
#if defined(FEATURE_INSTALLER_VFS) && FEATURE_INSTALLER_VFS
#include "llm/tools/service/ServiceTools.hpp"
#endif

#include "llm/tools/computer_use/Tool_ComputerUse.hpp"
#include "llm/tools/browser_use/Tool_BrowserUse.hpp"
#include "llm/agent_tool_provider.hpp"
#include "core/host_backends.hpp"
#include "llm/tools/app_command/Tool_AppCommand.hpp"
#include "llm/tools/info/InfoTool.hpp"
#include "llm/tools/network/NetworkTools.hpp"
#include "llm/tools/run/RunTool.hpp"
#if defined(FEATURE_XBLOX) && FEATURE_XBLOX
#include "llm/tools/sequencer/SequencerTool.hpp"
#endif
#if defined(FEATURE_TOOLS_COMMAND_CREATION) && FEATURE_TOOLS_COMMAND_CREATION
#include "llm/tools/command_creation/CreateCommandTool.hpp"
#endif

#include "core/cli_cancel.hpp"
#include "string/string_sanitizers.hpp"
#include "logging/logger.h"
#include "logging/pm_log.hpp"
#if defined(__ANDROID__)
#include "android/pm_android_realtime.hpp"
#endif
#if defined(FEATURE_STT) && FEATURE_STT && defined(FEATURE_REALTIME) && FEATURE_REALTIME && !defined(__ANDROID__)
#include "core/realtime_voice_session_ipc.hpp"
#endif

#include <functional>
#include "string/string_utils.hpp"

namespace {

media::llm::path::ExecuteResult chrome_host_browser(
    const std::string& name, const nlohmann::json& arguments)
{
#if !defined(_WIN32) || !defined(FEATURE_BROWSER) || !FEATURE_BROWSER
    (void)name;
    (void)arguments;
    return {};
#else
    if (pm::host::browser_use::enabled())
        return {};
    const auto providers = media::llm::agent::tool_providers::list();
    media::llm::agent::IAgentToolProvider* host =
        media::llm::agent::find_host_tool(providers, name);
    if (!host) {
        for (const auto& provider : providers) {
            if (provider && provider->id() == "chrome") {
                host = provider.get();
                break;
            }
        }
    }
    if (!host)
        return {};
    media::llm::agent::ToolContext ctx;
    const media::llm::agent::ToolResult host_result = host->invoke(name, arguments, ctx);
    media::llm::path::ExecuteResult result;
    result.ok = host_result.ok;
    result.envelope = host_result.envelope.is_object()
        ? host_result.envelope
        : nlohmann::json{{"ok", host_result.ok}};
    if (!result.envelope.contains("tool"))
        result.envelope["tool"] = name;
    if (!result.ok) {
        const auto& error = result.envelope["error"];
        if (error.is_string())
            result.error = error.get<std::string>();
        else if (error.is_object())
            result.error = error.value("message", std::string{"browser command failed"});
        else
            result.error = "browser command failed";
    }
    return result;
#endif
}

// A registered host provider wins when the voice session is outside this
// process (desktop UI, LAN peer). The catalog name still belongs to the
// builtin, so HostToolsFeature will not also advertise it.
bool invoke_registered_host_tool(const std::string& name,
                                 const nlohmann::json& arguments,
                                 media::llm::path::ExecuteResult& out)
{
    const auto providers = media::llm::agent::tool_providers::list();
    media::llm::agent::IAgentToolProvider* host =
        media::llm::agent::find_host_tool(providers, name);
    if (!host)
        return false;
    media::llm::agent::ToolContext ctx;
    const media::llm::agent::ToolResult host_result = host->invoke(name, arguments, ctx);
    out.ok = host_result.ok;
    out.envelope = host_result.envelope.is_object()
        ? host_result.envelope
        : nlohmann::json{{"ok", host_result.ok}};
    if (!out.envelope.contains("tool"))
        out.envelope["tool"] = name;
    if (!out.envelope.contains("ok"))
        out.envelope["ok"] = out.ok;
    if (!out.ok) {
        const auto& error = out.envelope["error"];
        if (error.is_string())
            out.error = error.get<std::string>();
        else
            out.error = "suspend_voice failed";
    }
    return true;
}

media::llm::path::ExecuteResult do_suspend_voice(const nlohmann::json& arguments)
{
    media::llm::path::ExecuteResult via_host;
    if (invoke_registered_host_tool("suspend_voice", arguments, via_host))
        return via_host;

#if defined(__ANDROID__)
    if (!pm::android::realtime_request_suspend())
        return media::llm::path::detail::simple_error("no active live voice session");
    return media::llm::path::detail::simple_ok({
        {"tool", "suspend_voice"},
        {"status", "suspended"},
        {"note", "Voice session is ending. Say nothing further."},
    });
#elif defined(FEATURE_STT) && FEATURE_STT && defined(FEATURE_REALTIME) && FEATURE_REALTIME
    std::string err;
    if (!pm::audio::rt_ipc::request_stop(err))
        return media::llm::path::detail::simple_error(
            err.empty() ? "no active live voice session" : err);
    return media::llm::path::detail::simple_ok({
        {"tool", "suspend_voice"},
        {"status", "suspended"},
        {"note", "Voice session is ending. Say nothing further."},
    });
#else
    (void)arguments;
    return media::llm::path::detail::simple_error(
        "suspend_voice is not available on this host");
#endif
}

} // namespace

namespace media::llm::path {

// ── Public state setters / accessors ──────────────────────────────────────
// Thin wrappers around the thread-locals owned by PathToolCommons.cpp.

void set_image_transform_file_progress_hook(
    std::function<void(const nlohmann::json& one_file_envelope)> f)
{
    detail::set_image_transform_file_progress(std::move(f));
}

void clear_image_transform_file_progress_hook()
{
    detail::clear_image_transform_file_progress();
}

void set_download_url_progress_hook(
    std::function<void(std::uint64_t received, std::uint64_t total)> f)
{
    detail::set_download_url_progress_hook(std::move(f));
}

void clear_download_url_progress_hook()
{
    detail::clear_download_url_progress_hook();
}

void set_agent_tool_blocklist(const std::vector<std::string>& disabled_names) {
    detail::set_agent_tool_blocklist_impl(disabled_names);
}

void clear_agent_tool_blocklist() { detail::clear_agent_tool_blocklist_impl(); }

void set_agent_path_base(const std::string& raw) {
    detail::set_agent_path_base_impl(raw);
}

void clear_agent_path_base() { detail::clear_agent_path_base_impl(); }

void set_agent_fs_jail(bool enabled) { detail::set_agent_fs_jail_impl(enabled); }

void clear_agent_fs_jail() { detail::clear_agent_fs_jail_impl(); }

bool agent_fs_jail_enabled() { return detail::agent_fs_jail_enabled(); }

void set_change_tool_call_id(const std::string& id) {
    media::llm::change::set_active_tool_call_id(id);
}

void clear_change_tool_call_id() {
    media::llm::change::clear_active_tool_call_id();
}

namespace detail {

ExecuteResult execute_impl(const std::string& name_in, const nlohmann::json& arguments) {
    constexpr const char k_ch[] = "llm_path_tools";

    const std::string name = strip_template_prefix(name_in);
    if (name != name_in) {
        pm::log::warn_lazy(k_ch, [&] {
            return std::string("template-prefixed tool name; using \"") + name
                 + "\" instead of \"" + name_in + "\"";
        });
    }

    auto log_done = [k_ch, &name](ExecuteResult r) -> ExecuteResult {
        if (r.ok)
            pm::log::debug_lazy(k_ch, [&] { return std::string("tool=") + name + " ok"; });
        else
            pm::log::warn_lazy(k_ch, [&] { return std::string("tool=") + name + " failed: " + r.error; });
        return r;
    };

    try {
        if (!agent_tool_blocklist().empty()) {
            std::string nlow = name;
            nlow = media::strutil::lower_ascii(nlow);
            if (agent_tool_blocklist().count(nlow)) {
                pm::log::warn_lazy(k_ch, [&] { return std::string("tool blocked name=") + name; });
                return fatal("tool is disabled: " + name);
            }
        }
        log_path_tool_invoke_context(name, arguments);

        // FS
        if (name == "list_images")     return log_done(do_list_images(arguments));
        if (name == "file_glob")       return log_done(do_file_glob(arguments));
        if (name == "file_read")       return log_done(do_file_read_path(arguments));
        if (name == "file_search")     return log_done(do_file_search(arguments));
        if (name == "search_index")    return log_done(do_search_index(arguments));
        if (name == "search_query")    return log_done(do_search_query(arguments));
        if (name == "search_detail")   return log_done(do_search_detail(arguments));
        if (name == "write_file")      return log_done(do_write_file(arguments));
        if (name == "file_compose")    return log_done(do_file_compose(arguments));
        if (name == "download_url")    return log_done(do_download_url(arguments));
        if (name == "file_str_replace") return log_done(do_file_str_replace(arguments));
        if (name == "file_delete")      return log_done(do_file_delete(arguments));

        // Pictures
        if (name == "image_resize")    return log_done(do_resize(arguments));
        if (name == "image_crop")      return log_done(do_crop(arguments));
        if (name == "image_transform") return log_done(do_transform(arguments));
        if (name == "image_create")    return log_done(do_create_image(arguments));
        if (name == "image_understand")   return log_done(do_understand(arguments));
        if (name == "ocr_text")           return log_done(do_ocr(arguments));
        if (name == "image_from_camera")  return log_done(do_camera(arguments));

        // image_meta and image_find removed from the agent catalog; the
        // internal worker code still lives in PictureTools / FsTools for
        // sidecar generation / CLI find. If a model somehow still calls
        // them, return a clear error.
        if (name == "image_meta")
            return fatal("image_meta is no longer available; use image_understand with a prompt instead");
        if (name == "image_find")
            return fatal("image_find is no longer available as an agent tool");

        // Video
        if (name == "create_video")    return log_done(do_create_video(arguments, name));

        // Audio
        if (name == "speak")               return log_done(do_speak(arguments));
        if (name == "play_file")           return log_done(do_play_file(arguments));
        if (name == "suspend_voice")       return log_done(do_suspend_voice(arguments));
        if (name == "audio_transcribe")    return log_done(do_audio_transcribe(arguments));

        // Inquire
        if (name == "ask_user")            return log_done(do_ask_user(arguments));

#if defined(FEATURE_SCHEDULER_TOOL) && FEATURE_SCHEDULER_TOOL
        // Scheduler
        if (name == "schedule_at")     return log_done(do_schedule_at(arguments));
        if (name == "schedule_in")     return log_done(do_schedule_in(arguments));
        if (name == "schedule_every")  return log_done(do_schedule_every(arguments));
        if (name == "schedule_cancel") return log_done(do_schedule_cancel(arguments));
        if (name == "schedule_list")   return log_done(do_schedule_list(arguments));
#endif

        // Memory
        if (name == "memory_read")          return log_done(do_memory_read(arguments));
        if (name == "memory_write")         return log_done(do_memory_write(arguments));
        if (name == "memory_append_event")  return log_done(do_memory_append_event(arguments));
        if (name == "memory_find")          return log_done(do_memory_find(arguments));

        // Shell + info + custom-command authoring
        if (name == "run")                 return log_done(media::llm::run::execute(arguments, agent_path_base()));
#if defined(FEATURE_XBLOX) && FEATURE_XBLOX
        if (name == "run_sequence")        return log_done(detail::do_run_sequence(arguments));
#endif
        if (name == "info_lookup")         return log_done(media::llm::info_tool::execute(arguments, agent_path_base()));
        if (detail::is_network_tool(name))
            return log_done(detail::do_network_tool(name, arguments));
#if defined(FEATURE_INSTALLER_VFS) && FEATURE_INSTALLER_VFS
        if (detail::is_service_tool(name))
            return log_done(detail::do_service_tool(name, arguments));
#endif
#if defined(FEATURE_TOOLS_COMMAND_CREATION) && FEATURE_TOOLS_COMMAND_CREATION
        if (name == "create_command")      return log_done(media::llm::create_command::execute(arguments));
#endif

        // Computer use / app inspection
        if (media::llm::computer_use::is_computer_use_tool(name))
            return log_done(media::llm::computer_use::execute(name, arguments, agent_path_base()));
        if (media::llm::browser_use::is_browser_use_tool(name)) {
            ExecuteResult via_chrome = chrome_host_browser(name, arguments);
            if (via_chrome.envelope.is_object() && !via_chrome.envelope.empty())
                return log_done(std::move(via_chrome));
            return log_done(media::llm::browser_use::execute(name, arguments));
        }
        if (media::llm::app_command::is_app_command_tool(name))
            return log_done(media::llm::app_command::execute(name, arguments));

        pm::log::warn_lazy(k_ch, [&] { return std::string("unknown tool name=") + name; });
        return fatal("unknown tool: " + name);
    } catch (const std::exception& e) {
        pm::log::error_lazy(k_ch, [&] { return std::string("tool=") + name + " exception: " + e.what(); });
        return fatal(std::string("path tool '") + name + "' exception: " + e.what());
    } catch (...) {
        pm::log::error_lazy(k_ch, [&] { return std::string("tool=") + name + " exception: unknown"; });
        return fatal("path tool '" + name + "' exception: unknown");
    }
}

} // namespace detail

ExecuteResult execute(const std::string& name, const nlohmann::json& arguments) {
    struct ClearCallId {
        ~ClearCallId() { media::llm::change::clear_active_tool_call_id(); }
    } clear;
    return detail::execute_impl(name, arguments);
}

// Wire abort_active_speak / abort_active_run into the global CLI cancel chain.
// install_cli_interrupt_handlers() only flips an atomic; without these
// listeners a Ctrl+C during a blocking `speak` (play_ring → wait_until_done)
// or `run` (subprocess wait) would not stop until the operation completed
// naturally. Registered exactly once on first call to execute().
namespace {
struct CancelListenerRegistrar {
    CancelListenerRegistrar() {
        media::cli::register_cancel_listener([] { abort_active_speak(); });
        media::cli::register_cancel_listener([] { abort_active_run();   });
    }
};
[[maybe_unused]] static const CancelListenerRegistrar g_cancel_listener_registrar;
} // namespace

void abort_active_speak() {
    detail::abort_active_speak_impl();
}

void pause_active_speak() {
    detail::pause_active_speak_impl();
}

void resume_active_speak() {
    detail::resume_active_speak_impl();
}

void register_active_speak_output(pm::audio::AudioOutput* out) noexcept {
    detail::set_active_speak_output(out);
}

void unregister_active_speak_output(pm::audio::AudioOutput* expected) noexcept {
    detail::clear_active_speak_output(expected);
}

void abort_active_run() {
    logger::trace("[cancel] abort_active_run → run::abort_active_run()");
    media::llm::run::abort_active_run();
}

namespace {

void sanitize_envelope_json_strings(nlohmann::json& j)
{
    if (j.is_string()) {
        j = media::str_san::normalise_text_for_json_storage(j.get<std::string>());
        return;
    }
    if (j.is_array()) {
        for (auto& el : j)
            sanitize_envelope_json_strings(el);
        return;
    }
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it)
            sanitize_envelope_json_strings(it.value());
    }
}

std::string safe_envelope_dump(const nlohmann::json& envelope)
{
    try {
        nlohmann::json copy = envelope;
        sanitize_envelope_json_strings(copy);
        return copy.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (...) {
        return "{\"ok\":false,\"error\":\"envelope serialization failed\"}";
    }
}

} // namespace

std::string envelope_text(const ExecuteResult& r) {
    return safe_envelope_dump(r.envelope);
}

} // namespace media::llm::path

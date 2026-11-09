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
#include "llm/cli/agent_cli_batch.hpp"

using namespace pm_llm_agent_cli;

int pm_image_cmd_llm_agent(CLI::App& app, PmImageCliState& st,
                           pm::cli::CliActivityScope* cli_activity) {
    (void)app;

#if defined(FEATURE_JQ) && FEATURE_JQ
    if (st.llm_agent_dedupe_cmd && st.llm_agent_dedupe_cmd->parsed()) {
#if defined(FEATURE_LLAMA) && FEATURE_LLAMA
        return cmd_llm_agent_dedupe(st);
#else
        std::cerr << "llm agent dedupe: this build was compiled without FEATURE_LLAMA.\n";
        return 1;
#endif
    }
    if (st.llm_agent_decide_cmd && st.llm_agent_decide_cmd->parsed())
        return cmd_llm_agent_decide(st);
    if (st.llm_agent_each_cmd && st.llm_agent_each_cmd->parsed())
        return cmd_llm_agent_each(st);
#endif

    if ((st.ag_serve_lan || st.ag_serve_pair) && !st.ag_serve) {
        std::cerr << "llm agent: --lan and --pair require --serve\n";
        return 1;
    }
    if (st.ag_serve_lan_devices || !st.ag_serve_lan_revoke.empty()) {
        if (st.ag_serve_lan_devices) {
            std::string text;
            std::string err;
            if (!media_cli::lan::list_paired_devices(text, err)) {
                std::cerr << "llm agent --lan-devices: " << err << "\n";
                return 1;
            }
            std::cout << text;
        }
        if (!st.ag_serve_lan_revoke.empty()) {
            std::string err;
            if (!media_cli::lan::revoke_paired_device(st.ag_serve_lan_revoke, err)) {
                std::cerr << "llm agent --lan-revoke: " << err << "\n";
                return 1;
            }
            std::cout << "revoked " << st.ag_serve_lan_revoke << "\n";
        }
        if (!st.ag_serve)
            return 0;
    }

    if (st.ag_serve)
        return media_cli::run_llm_agent_openai_server(
            media_cli::agent_server_options_from_cli(st));

#if !FEATURE_HOME_LLM_TOOLS
    st.ag_no_tools = true;
    st.ag_disable_tools.clear();
#endif

    // ── Live voice session control (start handled below; stop/status here) ─────
    // These never need a provider/prompt — they only touch the cross-process
    // session-file singleton, mirroring `video record stop` / `video record status`.
#if defined(FEATURE_STT) && FEATURE_STT && defined(FEATURE_REALTIME) && FEATURE_REALTIME
    if (st.ag_realtime_stop) {
        std::string err;
        if (!pm::audio::rt_ipc::request_stop(err)) {
            logger::error("llm agent --realtime-stop: " + err);
            return 1;
        }
        std::cout << "llm agent --realtime-stop: signal sent\n";
        return 0;
    }
    if (st.ag_realtime_status) {
        pm::audio::rt_ipc::Status info;
        std::string err;
        if (!pm::audio::rt_ipc::read_status(info, err)) {
            logger::error("llm agent --realtime-status: " + err);
            return 1;
        }
        if (st.ag_json) {
            nlohmann::json j;
            j["active"] = info.active;
            if (info.active) {
                j["pid"]   = info.pid;
                j["model"] = info.model;
                j["voice"] = info.voice;
                j["session_id"] = info.session_id;
                j["session_dir"] = info.session_dir;
            }
            std::cout << j.dump(2) << "\n";
        } else if (!info.active) {
            std::cout << "llm agent --realtime-status: no live voice session\n";
        } else {
            std::cout << "llm agent --realtime-status: live\n"
                      << "  pid   : " << info.pid << "\n"
                      << "  model : " << (info.model.empty() ? "(default)" : info.model) << "\n"
                      << "  voice : " << (info.voice.empty() ? "(server default)" : info.voice) << "\n"
                      << "  session : " << (info.session_id.empty() ? "(none)" : info.session_id) << "\n"
                      << "  session_dir : " << (info.session_dir.empty() ? "(default)" : info.session_dir) << "\n";
        }
        return 0;
    }
#else
    if (st.ag_realtime_stop || st.ag_realtime_status) {
        std::cerr << "llm agent: realtime voice session control requires a build with "
                     "FEATURE_STT + FEATURE_REALTIME.\n";
        return 1;
    }
#endif

    // Recovery for `--disable-tools --flag`: if a flag-shaped token is consumed
    // as the disable-tools "value", treat it as "no tool ids provided".
    // In that case disable all tools (same as --no-tools) and recover common
    // mode flags that users often place after --disable-tools.
    if (!st.ag_disable_tools.empty() && st.ag_disable_tools.rfind("--", 0) == 0) {
        const std::string swallowed_flag = st.ag_disable_tools;
        st.ag_disable_tools.clear();
        st.ag_no_tools = true;
        if (swallowed_flag == "--single-turn")
            st.ag_multi_turn = false;
        else if (swallowed_flag == "--multi-turn")
            st.ag_multi_turn = true;
        if (swallowed_flag == "--single-turn" || swallowed_flag == "--multi-turn")
            st.ag_multi_turn_explicit = true;
    }
    // Discard flag-shaped values accidentally swallowed by --enable-tools.
    if (!st.ag_enable_tools.empty() && st.ag_enable_tools.rfind("--", 0) == 0)
        st.ag_enable_tools.clear();

    // --cwd is the folder context for the agent (processed globally in dispatch)
    std::string folder_context = st.cwd;

    // --realtime is sugar for --type realtime; it wins over an explicit --type.
    if (st.ag_realtime)
        st.ag_api_mode = "realtime";

    const media::llm::agent::launch::AgentLaunchSpec launch_spec =
        media::llm::agent::cliopt::launch_spec_from(agent_cli_options_from(st));

    media::llm::agent::launch::AgentLaunchResolved launch_resolved;
    {
        std::string launch_err;
        if (!media::llm::agent::launch::resolve_agent_launch(launch_spec, launch_resolved, launch_err)) {
            std::cerr << "llm agent: " << launch_err << "\n";
            std::cout << launch_err << "\n";
            return 1;
        }
    }
    if (st.ag_runner.empty()) {
        if (const char* env = std::getenv("PM_LLM_AGENT_RUNNER")) {
            if (*env)
                st.ag_runner = env;
        }
    }
    if (st.ag_runner.empty())
        st.ag_runner = launch_resolved.effective_runner;
    // Install tool-provider overrides for the duration of this run.
    // All tool dispatch paths (ocr_text, image_understand, image_create, create_video)
    // read from resolved_default_chat_settings() which checks this override first.
    media::llm::ChatSettingsOverrideGuard tool_settings_guard(launch_resolved.chat_settings);

    if (st.ag_prompt.empty() && !cli_tty::stdin_is_tty()) {
        std::ostringstream buf;
        buf << std::cin.rdbuf();
        st.ag_prompt = buf.str();
        while (!st.ag_prompt.empty() && std::isspace(static_cast<unsigned char>(st.ag_prompt.back())))
            st.ag_prompt.pop_back();
    }

    {
        std::string prep_err;
        media_cli::AgentPromptPrepareParams prep{
            "llm agent", st.ag_prompt, st.ag_paths, st.ag_embed_paths, folder_context,
            /*append_embed=*/!st.ag_realtime};
        if (!media_cli::prepare_agent_cli_prompt(prep, prep_err)) {
            std::cerr << prep_err << "\n";
            return 1;
        }
    }
    if (!folder_context.empty()) {
        std::error_code ec;
        fs::path fp = media::path::path_from_utf8(folder_context);
        if (!fp.is_absolute())
            fp = fs::absolute(fp, ec);
        if (ec || !fs::exists(fp)) {
            logger::error(std::string("llm agent: --cwd folder does not exist: ") + folder_context);
            return 1;
        }
        folder_context = media::path::path_to_utf8(fp);
    }

#if defined(FEATURE_STT) && FEATURE_STT
#  if defined(FEATURE_REALTIME) && FEATURE_REALTIME
    // Live voice session: `--realtime` with no prompt-run intent starts a
    // speech-to-speech session (mic+speaker) instead of a one-shot text turn.
    // A --prompt is optional here (injected as a spoken-turn seed).
    if (st.ag_realtime && !st.ag_dry) {
#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
        // A live voice session runs tools (e.g. app_screenshot) via the realtime
        // bridge, which triggers the security gateway's consent path. The normal
        // run installs the consent surface far below; the realtime path returns
        // early, so install it here too — otherwise the win32/cli surface is
        // never registered and the request hangs waiting on an absent delegate.
        ConsentSurfaceScope::ReviewFn rt_review_fn =
            make_consent_review_fn(st, launch_resolved.provider);
        const ConsentSurfaceScope rt_consent_surface(
            resolve_consent_ui_for_launch(st.ag_consent_ui, launch_resolved),
            st.ag_consent_owner, std::move(rt_review_fn));
#endif
        return cmd_llm_agent_realtime(st, launch_resolved.chat_settings, cli_activity);
    }
#  endif
    // Mic mode: --prompt is used as optional system context, not the user prompt.
    if (st.ag_mic) {
        // Resolve provider settings first (same waterfall as normal mode).
        media::llm::agent::ProviderSettings prov = launch_resolved.provider;

        if (prov.api_key.empty()) {
            std::cerr << "llm agent mic: no LLM API key. Pass --api-key or configure app settings.\n";
            return 1;
        }

        media::llm::agent::Turn base_turn;
        base_turn.system_extra = st.ag_prompt; // --prompt = optional system context
        base_turn.selection    = st.ag_paths;
        base_turn.folder_hint  = folder_context;
        base_turn.structured_memory_features =
            resolve_structured_memory_features(base_turn.structured_memory_features, st);
        media::llm::agent::launch::apply_launch_policy_to_turn(launch_spec, launch_resolved, base_turn);
        apply_agent_capability_opt_outs(base_turn, st);
        {
            std::string sys_err;
            if (!apply_agent_system_prompt_option(st.ag_system_prompt, base_turn, sys_err)) {
                std::cerr << "llm agent mic: " << sys_err << "\n";
                return 1;
            }
            std::string planner_err;
            if (!apply_agent_planner_prompt_option(st.ag_planner_prompt, base_turn, planner_err)) {
                std::cerr << "llm agent mic: " << planner_err << "\n";
                return 1;
            }
        }
        media_cli::parse_agent_disable_tools(st.ag_disable_tools, st.ag_no_tools,
                                             base_turn.disabled_path_tools, &base_turn.disabled_mcp_tools);
        media_cli::parse_agent_enable_tools(st.ag_enable_tools, st.ag_no_tools,
                                            base_turn.allowed_path_tools,
                                            base_turn.planner_path_tool_allowlist_set);
        base_turn.parallel_tool_dispatch = !st.ag_no_parallel_tools;
        base_turn.load_snapshot_name     = st.ag_load_snapshot;
        base_turn.save_snapshot_name     = st.ag_save_snapshot;

        return cmd_llm_agent_mic(st, std::move(base_turn), prov,
                                 launch_resolved.chat_settings);
    }
#endif // FEATURE_STT

    // Normal (text) mode — prompt can come from --prompt or piped stdin.
    if (st.ag_prompt.empty()) {
        std::cerr << "llm agent: --prompt is required (or pipe prompt via stdin, or use --mic for voice input)\n";
        return 1;
    }

    bool ag_log_path_explicit = false;
    try {
        if (st.llm_agent_cmd) {
            if (const auto* opt = st.llm_agent_cmd->get_option_no_throw("--log"))
                ag_log_path_explicit = opt->count() > 0;
        }
    } catch (...) {}
    if (!ag_log_path_explicit) {
        st.ag_log_path = media::path::path_to_utf8((media::runtime_settings::default_logging_dir() / "agent.json"));
    } else if (!st.ag_log_path.empty()) {
        std::error_code ec;
        fs::path p = media::path::path_from_utf8(st.ag_log_path);
        if (!p.is_absolute()) {
            p = fs::absolute(p, ec);
            if (!ec)
                st.ag_log_path = media::path::path_to_utf8(p);
        }
    }

    media::llm::agent::Turn turn;
    turn.user_prompt = st.ag_prompt;
    turn.selection   = st.ag_paths;
    turn.folder_hint = folder_context;
    turn.structured_memory_features = resolve_structured_memory_features(turn.structured_memory_features, st);
    media::llm::agent::launch::apply_launch_policy_to_turn(launch_spec, launch_resolved, turn);
    apply_agent_capability_opt_outs(turn, st);
    {
        std::string sys_err;
        if (!apply_agent_system_prompt_option(st.ag_system_prompt, turn, sys_err)) {
            std::cerr << "llm agent: " << sys_err << "\n";
            return 1;
        }
        std::string planner_err;
        if (!apply_agent_planner_prompt_option(st.ag_planner_prompt, turn, planner_err)) {
            std::cerr << "llm agent: " << planner_err << "\n";
            return 1;
        }
    }
    media_cli::parse_agent_disable_tools(st.ag_disable_tools, st.ag_no_tools,
                                         turn.disabled_path_tools, &turn.disabled_mcp_tools);
    media_cli::parse_agent_enable_tools(st.ag_enable_tools, st.ag_no_tools,
                                        turn.allowed_path_tools,
                                        turn.planner_path_tool_allowlist_set);
    turn.parallel_tool_dispatch = !st.ag_no_parallel_tools;
    turn.load_snapshot_name     = st.ag_load_snapshot;
    turn.save_snapshot_name     = st.ag_save_snapshot;

    std::string session_task_id;
    // Open a session whenever multi-turn is on, OR the user explicitly named
    // one via --session-id (so single-turn invocations sharing an id read
    // each other's persisted memory).
    if (st.ag_multi_turn || !st.ag_session_id.empty()) {
        session_task_id = media::llm::agent::session_get_or_create(st.ag_session_id);
        nlohmann::json mem_state, recent_evs;
        media::llm::agent::session_load(session_task_id, mem_state, recent_evs, 20);
        turn.task_id       = session_task_id;
        turn.memory_state  = std::move(mem_state);
        turn.recent_events = std::move(recent_evs);
    }
    if (!st.ag_log_path.empty()) {
        const fs::path log_path = media::path::path_from_utf8(st.ag_log_path);
        turn.source_log_session_id = session_task_id;
        turn.source_log_id         = media::path::path_to_utf8(log_path.stem());
        turn.source_log_path       = media::path::path_to_utf8(log_path);
    }

    // Base: load Chat Provider Settings from the app config store (Windows only).
    // CLI args overlay on top of whatever is saved there.
    media::llm::agent::ProviderSettings prov = launch_resolved.provider;

    auto agent_log_utc_now = []() -> std::string {
        std::time_t t = std::time(nullptr);
#if defined(_WIN32)
        std::tm tm_buf{};
        if (gmtime_s(&tm_buf, &t) != 0) return {};
        const std::tm* g = &tm_buf;
#else
        const std::tm* g = std::gmtime(&t);
#endif
        if (!g) return {};
        char buf[40];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", g);
        return buf;
    };

    auto agent_append_log_event = [](nlohmann::json& events,
                                     const media::llm::agent::Event& e) {
        nlohmann::json j;
        using K = media::llm::agent::Event::Kind;
        switch (e.kind) {
        case K::TurnStarted:
            j["kind"] = "turn_started";
            j["detail"] = e.payload;
            break;
        case K::LlmRound:
            j["kind"] = "llm_round";
            j["detail"] = e.payload;  // {round, stateful, response_id?, tokens, model}
            break;
        case K::ToolCall:
            j["kind"] = "tool_call";
            j["tool"] = e.tool_name;
            j["detail"] = e.payload;
            break;
        case K::ToolResult:
            j["kind"] = "tool_result";
            j["tool"] = e.tool_name;
            j["detail"] = e.payload;  // includes full envelope
            break;
        case K::ToolFileProgress:
            j["kind"] = "tool_file_progress";
            j["tool"] = e.tool_name;
            j["detail"] = e.payload;  // single-file envelope during image_transform
            break;
        case K::Thinking:
            j["kind"] = "thinking";
            j["text"] = e.text;
            break;
        case K::AssistantText:
            j["kind"] = "assistant_text";
            j["text"] = e.text;
            break;
        case K::MemoryUpdate:
            j["kind"] = "memory_update";
            j["detail"] = media::llm::agent::truncate_memory_update_for_log(e.payload);
            break;
        case K::ClarificationNeeded:
            j["kind"] = "clarification_needed";
            j["detail"] = e.payload;  // {confidence, intent, action, subject, reason}
            break;
        case K::ErrorReflexion:
            j["kind"] = "error_reflexion";
            j["detail"] = media::llm::agent::truncate_memory_update_for_log(e.payload);
            break;
        case K::SelfCheckResult:
            j["kind"] = "self_check";
            j["detail"] = e.payload;  // {passed, tool_ok_rate, deflection_detected, ...}
            break;
        case K::Error:
            j["kind"] = "error";
            j["text"] = e.text;
            break;
        case K::Done:
            j["kind"] = "done";
            j["text"] = e.text;
            break;
        default:
            j["kind"] = "unknown";
            break;
        }
        events.push_back(std::move(j));
    };

    auto write_agent_log = [&](const nlohmann::json& root) {
        if (st.ag_log_path.empty()) return;
        std::ofstream f(st.ag_log_path, std::ios::out | std::ios::trunc);
        if (!f) {
            std::cerr << "llm agent: could not open --log file: " << st.ag_log_path << "\n";
            return;
        }
        f << root.dump(2) << "\n";
    };

    if (st.ag_dry) {
        media::llm::agent::Turn dry_turn = turn;
        if (prov.max_iterations > 0 && !dry_turn.tool_policy_override_set) {
            std::string policy_err;
            media::runtime_settings::apply_global_tool_policy_to_values(
                dry_turn.disabled_path_tools,
                dry_turn.mcp_tools_enabled,
                dry_turn.disabled_mcp_servers,
                policy_err);
            if (!policy_err.empty())
                logger::warn(std::string("agent tools: global policy load: ") + policy_err);
        }
        if (dry_turn.planner_enabled) {
            if (prov.api_key.empty() && prov.router != "llama") {
                dry_turn.planner_meta = {
                    {"enabled", true},
                    {"budget", dry_turn.planner_schema_budget},
                    {"fallback", true},
                    {"reason", "no API key configured for planner dry-run"},
                };
            } else {
#if FEATURE_HOME_LLM_TOOLS
                media::llm::agent::apply_planner_tool_selection(dry_turn, prov);
#endif
            }
        }
        const bool system_prompt_null = turn.system_prompt_override_set && turn.system_prompt_is_null;
        nlohmann::json dry_tools = nlohmann::json::array();
#if FEATURE_HOME_LLM_TOOLS
        if (prov.max_iterations > 0) {
            if (dry_turn.planner_path_tool_allowlist_set) {
                dry_tools = dry_turn.allowed_path_tools.empty()
                    ? nlohmann::json::array()
                    : media::llm::path::tool_catalog_openai_allowing(
                        dry_turn.allowed_path_tools, dry_turn.disabled_path_tools);
            } else {
                dry_tools = dry_turn.disabled_path_tools.empty()
                    ? media::llm::path::tool_catalog_openai()
                    : media::llm::path::tool_catalog_openai_excluding(dry_turn.disabled_path_tools);
            }
        }
#endif
        const std::string sys = turn.system_prompt_override_set
            ? turn.system_prompt_override
            : media::llm::agent::build_system_prompt(dry_turn, dry_tools);
        const std::string api_mode_str =
            prov.api_mode == media::llm::agent::LlmApiMode::Responses
                ? "responses (`/v1/responses`)"
                : (prov.api_mode == media::llm::agent::LlmApiMode::Realtime
                    ? "realtime (experimental WebSocket `/v1/realtime`, text-only)"
                    : "completion (`/v1/chat/completions`)");
        const std::string stream_mode_str =
            prov.streaming_mode == media::llm::agent::LlmStreamingMode::On  ? "on" :
            prov.streaming_mode == media::llm::agent::LlmStreamingMode::Off ? "off" : "auto";

        std::ostringstream md;
        md << "# llm agent — dry run\n\n";

        // ── Provider ─────────────────────────────────────────────────────────
        md << "## Provider\n\n";
        md << "| Setting | Value |\n";
        md << "| :------ | :---- |\n";
        md << "| preset | `" << (st.ag_preset.empty() ? "(none)" : st.ag_preset) << "` |\n";
        md << "| router | `" << (prov.router.empty() ? "(default)" : prov.router) << "` |\n";
        md << "| model | `"  << (prov.model.empty()  ? "(default)" : prov.model)  << "` |\n";
        md << "| base_url | "
           << (prov.base_url.empty() ? "*(router default)*" : "`" + prov.base_url + "`")
           << " |\n";
        md << "| api_key | " << (prov.api_key.empty() ? "**missing**" : "set") << " |\n";
        md << "| api_mode | " << api_mode_str << " |\n";
        md << "| streaming | `" << stream_mode_str << "` |\n";
        md << "| timeout | " << prov.timeout_ms << " ms |\n";
        md << "| max_iterations | " << prov.max_iterations << " |\n";
        md << "| planner | " << (dry_turn.planner_enabled ? "on" : "off") << " |\n";
        md << "\n";

        // ── Context ───────────────────────────────────────────────────────────
        md << "## Context\n\n";
        md << "| Setting | Value |\n";
        md << "| :------ | :---- |\n";
        md << "| folder | `" << (turn.folder_hint.empty() ? "(none)" : turn.folder_hint) << "` |\n";
        md << "| included files | " << turn.selection.size() << " |\n";
        md << "| embedded files | " << st.ag_embed_paths.size() << " |\n";
        md << "\n";
        if (!turn.selection.empty()) {
            for (const auto& p : turn.selection)
                md << "- `" << p << "`\n";
            md << "\n";
        }

        // ── Tools ─────────────────────────────────────────────────────────────
        md << "## Tools\n\n";
#if !FEATURE_HOME_LLM_TOOLS
        md << "*Built-in path tools and MCP tools are disabled in this build "
              "(`FEATURE_HOME_LLM_TOOLS=OFF`).*\n\n";
#else
        if (st.ag_no_tools) {
            md << "*All path tools disabled via `--no-tools`.*\n\n";
        } else if (!st.ag_enable_tools.empty() && dry_turn.planner_path_tool_allowlist_set) {
            md << "*`--enable-tools` allowlist active; "
               << dry_turn.allowed_path_tools.size() << " tool(s) allowed: ";
            for (std::size_t i = 0; i < dry_turn.allowed_path_tools.size(); ++i) {
                if (i) md << ", ";
                md << "`" << dry_turn.allowed_path_tools[i] << "`";
            }
            md << ".*\n\n";
        } else if (dry_turn.planner_enabled && dry_turn.planner_path_tool_allowlist_set) {
            md << "*Planner selected " << dry_turn.allowed_path_tools.size()
               << " path-tool schemas; " << (dry_tools.is_array() ? dry_tools.size() : 0)
               << " tools enabled after policy.*\n\n";
        } else if (!dry_turn.disabled_path_tools.empty() || !dry_turn.disabled_mcp_tools.empty()) {
            md << "*Tool policy applied; " << (dry_tools.is_array() ? dry_tools.size() : 0)
               << " tools enabled. See the system prompt below for the effective tool list.*\n\n";
        } else if (!st.ag_disable_tools.empty()) {
                md << "*`--disable-tools` value `" << st.ag_disable_tools
                   << "` did not match any known tool — full catalog active.*\n\n";
        } else {
            md << "*Full catalog enabled.*\n\n";
        }
        if (!dry_turn.mcp_tools_enabled)
            md << "*MCP-backed tools suppressed for this turn (`--no-mcp`).*\n\n";
        if (dry_turn.planner_enabled) {
            md << "## Planner\n\n";
            md << "| Setting | Value |\n";
            md << "| :------ | :---- |\n";
            md << "| budget | " << dry_turn.planner_meta.value("budget", dry_turn.planner_schema_budget) << " |\n";
            md << "| fallback | " << (dry_turn.planner_meta.value("fallback", true) ? "yes" : "no") << " |\n";
            md << "| reason | `" << dry_turn.planner_meta.value("reason", std::string{"(none)"}) << "` |\n";
            md << "| input chars | " << dry_turn.planner_meta.value("input_chars", 0) << " |\n";
            md << "\n";
            if (!dry_turn.allowed_path_tools.empty()) {
                md << "Selected tools:\n";
                for (const auto& name : dry_turn.allowed_path_tools)
                    md << "- `" << name << "`\n";
                md << "\n";
            }
        }
#endif

        // ── Skills ────────────────────────────────────────────────────────────
#if FEATURE_HOME_LLM_SKILLS
        md << "## Skills\n\n";
        if (!turn.skills_enabled) {
            md << "*Skill discovery/injection disabled for this turn (`--no-skills`).*\n\n";
        } else {
        const auto snap = load_skill_snapshot_for_cwd(turn.folder_hint, &turn);
        md << "| Root | Path |\n";
        md << "| :--- | :--- |\n";
        md << "| roaming | `" << media::path::path_to_utf8(snap.roaming_root) << "` |\n";
        md << "| workspace | `" << media::path::path_to_utf8(snap.workspace_root) << "` |\n\n";
        md << "| Name | Source | Available | Active |\n";
        md << "| :--- | :--- | :---: | :---: |\n";
        if (snap.entries.empty()) {
            md << "| *(none)* |  |  |  |\n\n";
        } else {
            for (const auto& e : snap.entries) {
                md << "| `" << e.name << "` | "
                   << (e.source == media::llm::skills::SkillSource::Roaming ? "roaming" : "workspace")
                   << " | " << (e.available ? "✓" : "✗")
                   << " | " << (e.active ? "✓" : "✗")
                   << " |\n";
            }
            md << "\n";
        }
        }
#else
        md << "## Skills\n\n";
        md << "*Skills are disabled in this build (`FEATURE_HOME_LLM_SKILLS=OFF`).*\n\n";
#endif

        // ── Render metadata (tables / lists) through the markdown renderer ───
        const bool want_render =
            st.ag_markdown == "plain"  ? false :
            st.ag_markdown == "render" ? true  :
            cli_tty::stdout_is_tty();
        const bool use_color =
            st.ag_color == "never"  ? false :
            st.ag_color == "always" ? true  :
            cli_tty::stdout_is_tty() && !cli_tty::env_requests_plain_output();
#if defined(_WIN32)
        if (want_render && use_color)
            cli_tty::win_enable_virtual_terminal_processing_stdout();
#endif
        // ANSI helpers (no-ops when color is off)
        const char* bold  = use_color ? "\033[1m"  : "";
        const char* dim   = use_color ? "\033[2m"  : "";
        const char* reset = use_color ? "\033[0m"  : "";
        const char* rule  = use_color ? "\033[2m"  : "";

        {
            std::string meta = md.str();
            if (want_render) {
                html::MdOptions mdopts{};
                int w = cli_tty::terminal_width_columns();
                if (w <= 0) w = 80;
                meta = html::markdown_to_terminal(meta, mdopts, use_color, w);
            }
            std::cout << meta;
            if (meta.empty() || meta.back() != '\n') std::cout << '\n';
        }

        // ── Prompts printed verbatim — the system prompt is prose, not markdown ─
        auto print_rule = [&]() {
            int w = cli_tty::terminal_width_columns();
            if (w <= 0 || w > 220) w = 72;
            std::cout << rule;
            for (int i = 0; i < w; ++i) std::cout << '-';
            std::cout << reset << '\n';
        };

        print_rule();
        std::cout << bold << "System prompt" << reset << "\n\n";
        if (system_prompt_null)
            std::cout << dim << "(null - no system message will be sent)" << reset << "\n";
        else
            std::cout << dim << sys << reset << "\n";

        print_rule();
        std::cout << bold << "User prompt" << reset << "\n\n";
        std::cout << turn.user_prompt << "\n";

        if (!st.ag_log_path.empty()) {
            nlohmann::json paths = nlohmann::json::array();
            for (const auto& p : turn.selection) paths.push_back(p);
            nlohmann::json logj{
                {"schema_version", "1"},
                {"generated_at",   agent_log_utc_now()},
                {"command",        "llm agent"},
                {"dry_run",        true},
                {"cli", {
                    {"prompt", st.ag_prompt},
                    {"preset", st.ag_preset.empty() ? nlohmann::json(nullptr) : nlohmann::json(st.ag_preset)},
                    {"paths",  std::move(paths)},
                    {"embed_paths", nlohmann::json(st.ag_embed_paths)},
                    {"folder", turn.folder_hint},
                    {"no_tools", st.ag_no_tools},
                    {"planner", st.ag_planner},
                    {"planner_budget", st.ag_planner_budget},
                    {"multi_turn", st.ag_multi_turn},
                    {"session_id", session_task_id.empty() ? nlohmann::json(nullptr) : nlohmann::json(session_task_id)},
                    {"system_prompt", st.ag_system_prompt.empty() ? nlohmann::json(nullptr) : nlohmann::json(st.ag_system_prompt)},
                    {"planner_prompt", st.ag_planner_prompt.empty() ? nlohmann::json(nullptr) : nlohmann::json(st.ag_planner_prompt)},
                    {"disable_tools", st.ag_disable_tools.empty() ? nlohmann::json(nullptr) : nlohmann::json(st.ag_disable_tools)},
                    {"enable_tools", st.ag_enable_tools.empty() ? nlohmann::json(nullptr) : nlohmann::json(st.ag_enable_tools)},
                    {"json_stdout", st.ag_json},
                }},
                {"provider", {
                    {"router", prov.router},
                    {"model", prov.model},
                    {"base_url", prov.base_url.empty()
                        ? nlohmann::json(nullptr) : nlohmann::json(prov.base_url)},
                    {"timeout_ms", prov.timeout_ms},
                    {"max_iterations", prov.max_iterations},
                    {"api_mode", prov.api_mode == media::llm::agent::LlmApiMode::Responses ? "responses"
                                  : prov.api_mode == media::llm::agent::LlmApiMode::Realtime ? "realtime"
                                  : "completion"},
                    {"streaming", prov.streaming_mode == media::llm::agent::LlmStreamingMode::On ? "on"
                                  : prov.streaming_mode == media::llm::agent::LlmStreamingMode::Off ? "off"
                                  : "auto"},
                    {"api_key_configured", !prov.api_key.empty()},
                }},
                {"turn", {
                    {"user_prompt", turn.user_prompt},
                    {"selection", nlohmann::json(turn.selection)},
                    {"folder_hint", turn.folder_hint},
                    {"disabled_path_tools", nlohmann::json(turn.disabled_path_tools)},
                    {"disabled_mcp_tools", nlohmann::json(turn.disabled_mcp_tools)},
                    {"allowed_path_tools", nlohmann::json(dry_turn.allowed_path_tools)},
                    {"planner", dry_turn.planner_meta},
                    {"context_reduction", {
                        {"mode",
                            turn.context_reduction_mode == pm::llm::ContextReductionMode::ToolResults ? "tool-results"
                          : turn.context_reduction_mode == pm::llm::ContextReductionMode::All          ? "all"
                                                                                                       : "off"},
                        {"ratio",     turn.context_reduction_ratio},
                        {"min_bytes", turn.context_reduction_min_bytes},
                        {"objective", turn.context_reduction_objective.empty()
                                          ? nlohmann::json(nullptr)
                                          : nlohmann::json(turn.context_reduction_objective)},
                    }},
                }},
                {"system_prompt", system_prompt_null ? nlohmann::json(nullptr) : nlohmann::json(sys)},
            };
            write_agent_log(logj);
        }
        return 0;
    }

    {
        std::optional<media::llm::agent::AgentRunnerKind> external_runner;
        if (!st.ag_runner.empty()) {
            external_runner = media::llm::agent::parse_agent_runner_kind(st.ag_runner);
            if (!external_runner) {
                std::cerr << "llm agent: unknown --runner '" << st.ag_runner
                          << "' (use native | claude-code | codex)\n";
                return 1;
            }
        }

        if (external_runner
            && *external_runner != media::llm::agent::AgentRunnerKind::Native) {
#if FEATURE_AGENT_CODEX || FEATURE_AGENT_CLAUDE || FEATURE_AGENT_CURSOR
            return run_llm_agent_external_runner(st, std::move(turn), session_task_id,
                                               *external_runner,
                                               !launch_resolved.has_preset
                                                   || launch_resolved.selected_preset.add_tanit_mcp_server);
#else
            std::cerr << "llm agent: external runners are disabled in this build\n";
            return 1;
#endif
        }
    }

    if (prov.api_key.empty() && prov.router != "llama"
            && prov.router != "loopback" && prov.router != "mock") {
        if (!st.ag_log_path.empty()) {
            nlohmann::json paths = nlohmann::json::array();
            for (const auto& p : turn.selection) paths.push_back(p);
            write_agent_log(nlohmann::json{
                {"schema_version", "1"},
                {"generated_at", agent_log_utc_now()},
                {"command", "llm agent"},
                {"error", "no_api_key"},
                {"message", "Pass --api-key or configure Chat / provider API keys in app settings."},
                {"cli", {{"prompt", st.ag_prompt},
                         {"preset", st.ag_preset.empty() ? nlohmann::json(nullptr) : nlohmann::json(st.ag_preset)},
                         {"paths", paths},
                         {"embed_paths", nlohmann::json(st.ag_embed_paths)},
                         {"folder", turn.folder_hint},
                         {"no_tools", st.ag_no_tools},
                         {"disable_tools", st.ag_disable_tools.empty() ? nlohmann::json(nullptr) : nlohmann::json(st.ag_disable_tools)},
                         {"enable_tools", st.ag_enable_tools.empty() ? nlohmann::json(nullptr) : nlohmann::json(st.ag_enable_tools)}}},
                {"provider", {{"router", prov.router}, {"model", prov.model}}},
            });
        }
        std::cerr << "llm agent: no API key supplied. Pass --api-key, or set the key in app settings "
                     "(Chat provider and/or API Keys / providers in settings.json).\n";
        return 1;
    }

    const bool interactive_multi_turn =
        st.ag_multi_turn_explicit
        && st.ag_multi_turn
        && !st.ag_json
        && cli_tty::stdout_is_tty();

    // Resolve --dst path variables once (same VariableContext as --prompt above).
    std::string resolved_dst_path;
    if (!st.ag_dst.empty()) {
        media::commands::VariableContext dst_vc;
        dst_vc.cwd = folder_context;
        if (!st.ag_paths.empty())
            dst_vc.source_file = st.ag_paths.front();
        else if (!st.ag_embed_paths.empty())
            dst_vc.source_file = st.ag_embed_paths.front();
        dst_vc.selection_paths = st.ag_paths;
        const auto dst_vars = media::commands::make_variable_map(dst_vc);
        std::string dst_var_err;
        resolved_dst_path = media::commands::resolve_variables(st.ag_dst, dst_vars, &dst_var_err);
        if (!dst_var_err.empty())
            logger::warn(std::string("llm agent: --dst variable warning: ") + dst_var_err);
        // Make absolute relative to folder_context / cwd
        if (!resolved_dst_path.empty()) {
            std::error_code abs_ec;
            fs::path dp = media::path::path_from_utf8(resolved_dst_path);
            if (!dp.is_absolute()) {
                const fs::path base = folder_context.empty()
                    ? fs::current_path(abs_ec)
                    : media::path::path_from_utf8(folder_context);
                dp = (base / dp).lexically_normal();
            }
            resolved_dst_path = media::path::path_to_utf8(dp);
        }
    }

    std::string current_prompt = turn.user_prompt;
    if (interactive_multi_turn) {
        const std::string mode =
            prov.api_mode == media::llm::agent::LlmApiMode::Realtime ? "realtime"
            : prov.api_mode == media::llm::agent::LlmApiMode::Responses ? "responses"
            : "completion";
        std::cerr << "llm agent: " << mode
                  << " multi-turn interactive mode (Enter prompt, Ctrl+C to exit)\n";
    }

#if defined(_WIN32)
    const ScopedConsoleTitle agent_console_title(L"PM-Image CLI - llm agent");
#endif

#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
    // Build the SecurityAgent review callback for the win32 surface. It reuses
    // the run's provider, overridden by --ai-consent-router/--ai-consent-model
    // when set (mirrors agent_factory's CliAiReviewConsentDelegate). Wiring it
    // here (rather than letting agent_factory auto-install) is required because
    // the ConsentSurfaceScope installs an active delegate, which suppresses the
    // factory's fallback reviewer.
    ConsentSurfaceScope::ReviewFn consent_review_fn = make_consent_review_fn(st, prov);
    // Select + install the consent surface (--consent-ui) for the whole run.
    // The security gateway picks it up from the registry on each turn.
    const ConsentSurfaceScope consent_surface(
        resolve_consent_ui_for_launch(st.ag_consent_ui, launch_resolved),
        st.ag_consent_owner, std::move(consent_review_fn));
#endif

    auto hud = AgentCliHud::make(st.ag_hud, st.ag_hud_mode, prov.max_iterations);

#if defined(FEATURE_COMMAND_IPC_NEXT) && FEATURE_COMMAND_IPC_NEXT
    // Publish this run into the cross-process agent registry for the lifetime of
    // the command (covers single + interactive multi-turn). The descriptor is
    // removed by the RAII dtor on every return path. cmdline is auto-filled.
    media::llm::agent_ipc::Registration agent_reg;
    {
        media::llm::agent_ipc::AgentInfo info;
        info.kind       = media::llm::agent_ipc::AgentKind::Regular;
        info.cwd        = turn.folder_hint.empty() ? st.cwd : turn.folder_hint;
        info.prompt     = turn.user_prompt;
        info.session_id = session_task_id;
        info.model      = prov.router.empty() ? prov.model
                                              : (prov.router + ":" + prov.model);
        std::string reg_err;
        if (!agent_reg.begin(info, reg_err))
            logger::debug(std::string("[agent_ipc] register failed: ") + reg_err);
    }
#endif

    for (;;) {
        turn.user_prompt = current_prompt;
        if (st.ag_multi_turn && !session_task_id.empty()) {
            nlohmann::json mem_state, recent_evs;
            media::llm::agent::session_load(session_task_id, mem_state, recent_evs, 20);
            turn.task_id       = session_task_id;
            turn.memory_state  = std::move(mem_state);
            turn.recent_events = std::move(recent_evs);
        }

        bool saw_text_delta = false;
        nlohmann::json log_events = nlohmann::json::array();

        // Stream events to stderr in human-readable form unless --json is set.
        media::llm::agent::EventCallback cb_stderr;
        if (!st.ag_json) {
            cb_stderr = [&](const media::llm::agent::Event& e) -> bool {
                using K = media::llm::agent::Event::Kind;
                switch (e.kind) {
                    case K::TurnStarted:
                        logger::debug("-> turn started");
                        break;
                    case K::LlmRound: {
                        std::string line = "[llm] round=" + std::to_string(e.payload.value("round", 0));
                        if (e.payload.value("stateful", false)) line += " stateful";
                        if (e.payload.contains("tokens") && e.payload["tokens"].is_object()) {
                            const auto& t = e.payload["tokens"];
                            if (t.contains("input"))     line += " in=" + t["input"].dump();
                            if (t.contains("output"))    line += " out=" + t["output"].dump();
                            if (t.contains("reasoning")) line += " reasoning=" + t["reasoning"].dump();
                        }
                        logger::debug(line);
                        break;
                    }
                    case K::ToolCall:
                        std::cerr << media::llm::agent::format_agent_tool_call_cli_line(e.tool_name, e.payload)
                                  << "\n";
                        break;
                    case K::ToolFileProgress:
                        if (e.payload.contains("envelope")) {
                            std::cerr << media::llm::agent::format_agent_tool_result_cli_line(
                                             e.tool_name, {}, e.payload)
                                      << "\n";
                        }
                        break;
                    case K::ToolResult:
                        std::cerr << media::llm::agent::format_agent_tool_result_cli_line(
                                         e.tool_name, e.text, e.payload)
                                  << "\n";
                        break;
                    case K::Thinking: {
                        logger::info(std::string("[thinking] ") + e.text);
                        break;
                    }
                    case K::TextDelta:
                        saw_text_delta = true;
                        std::cout << e.text << std::flush;
                        break;
                    case K::AssistantText:
                        logger::debug(std::string("<- assistant: ") + e.text);
                        break;
                    case K::Error:
                        logger::error(std::string("llm agent: ") + e.text);
                        break;
                    case K::Done:
                        break;
                    default: break;
            }
                return true;
            };
        }

        media::llm::agent::EventCallback cb;
        if (!st.ag_log_path.empty()) {
            cb = [&, cb_stderr](const media::llm::agent::Event& e) -> bool {
                agent_append_log_event(log_events, e);
                if (cb_stderr) return cb_stderr(e);
                return true;
            };
        } else {
            cb = std::move(cb_stderr);
        }
        if (hud.active()) {
            auto inner = std::move(cb);
            cb = [&hud, inner](const media::llm::agent::Event& e) -> bool {
                hud.on_event(e);
                if (inner)
                    return inner(e);
                return true;
            };
        }

        const auto turn_started_at = std::chrono::steady_clock::now();
        auto result = media::llm::agent::run_turn(turn, prov, cb);
        const auto turn_wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - turn_started_at).count();
        append_session_turn_summary_if_enabled(st, session_task_id, turn.user_prompt, result,
                                               turn.structured_memory_features);

        nlohmann::json result_json{
            {"ok",         result.ok},
            {"final_text", result.final_text},
            {"iterations", result.iterations},
            {"transcript", result.transcript},
        };
        if (result.transcript.is_object() && result.transcript.contains("planner"))
            result_json["planner"] = result.transcript["planner"];
        if (result.cancelled) result_json["cancelled"] = true;
        if (!result.error.empty()) result_json["error"] = result.error;
        if (result.llm_usage_aggregate.is_object()) {
            const int tt = result.llm_usage_aggregate.value("total_tokens", 0);
            const int pr = result.llm_usage_aggregate.value("prompt_tokens", 0);
            const int co = result.llm_usage_aggregate.value("completion_tokens", 0);
            const auto rounds = result.llm_usage_aggregate.value("llm_rounds", nlohmann::json::array());
            const bool has_cost = result.llm_usage_aggregate.contains("cost")
                                  && result.llm_usage_aggregate["cost"].is_number();
            if ((rounds.is_array() && !rounds.empty()) || tt > 0 || pr > 0 || co > 0 || has_cost)
                result_json["llm_usage"] = result.llm_usage_aggregate;
        }

        if (!st.ag_log_path.empty()) {
            nlohmann::json paths = nlohmann::json::array();
            for (const auto& p : turn.selection) paths.push_back(p);
            nlohmann::json logj{
                {"schema_version", "1"},
                {"generated_at",   agent_log_utc_now()},
                {"command",        "llm agent"},
                {"cli", {
                    {"prompt", turn.user_prompt},
                    {"preset", st.ag_preset.empty() ? nlohmann::json(nullptr) : nlohmann::json(st.ag_preset)},
                    {"paths",  std::move(paths)},
                    {"embed_paths", nlohmann::json(st.ag_embed_paths)},
                    {"folder", turn.folder_hint},
                    {"no_tools", st.ag_no_tools},
                    {"planner", st.ag_planner},
                    {"planner_budget", st.ag_planner_budget},
                    {"multi_turn", st.ag_multi_turn},
                    {"session_id", session_task_id.empty() ? nlohmann::json(nullptr) : nlohmann::json(session_task_id)},
                    {"system_prompt", st.ag_system_prompt.empty() ? nlohmann::json(nullptr) : nlohmann::json(st.ag_system_prompt)},
                    {"planner_prompt", st.ag_planner_prompt.empty() ? nlohmann::json(nullptr) : nlohmann::json(st.ag_planner_prompt)},
                    {"disable_tools", st.ag_disable_tools.empty() ? nlohmann::json(nullptr) : nlohmann::json(st.ag_disable_tools)},
                    {"enable_tools", st.ag_enable_tools.empty() ? nlohmann::json(nullptr) : nlohmann::json(st.ag_enable_tools)},
                    {"json_stdout", st.ag_json},
                }},
                {"provider", {
                    {"router", prov.router},
                    {"model", prov.model},
                    {"base_url", prov.base_url.empty()
                        ? nlohmann::json(nullptr) : nlohmann::json(prov.base_url)},
                    {"timeout_ms", prov.timeout_ms},
                    {"max_iterations", prov.max_iterations},
                    {"api_mode", prov.api_mode == media::llm::agent::LlmApiMode::Responses ? "responses"
                                  : prov.api_mode == media::llm::agent::LlmApiMode::Realtime ? "realtime"
                                  : "completion"},
                    {"streaming", prov.streaming_mode == media::llm::agent::LlmStreamingMode::On ? "on"
                                  : prov.streaming_mode == media::llm::agent::LlmStreamingMode::Off ? "off"
                                  : "auto"},
                    {"api_key_configured", true},
                }},
                {"turn", {
                    {"user_prompt", turn.user_prompt},
                    {"selection", nlohmann::json(turn.selection)},
                    {"folder_hint", turn.folder_hint},
                    {"disabled_path_tools", nlohmann::json(turn.disabled_path_tools)},
                    {"disabled_mcp_tools", nlohmann::json(turn.disabled_mcp_tools)},
                    {"system_prompt_null", turn.system_prompt_override_set && turn.system_prompt_is_null},
                }},
                {"events", std::move(log_events)},
                {"result", result_json},
            };
            write_agent_log(logj);
        }

        if (hud.active()) {
            if (result.cancelled)
                hud.busy("cancelled");
            else if (!result.ok)
                hud.error(result.error.empty() ? "failed" : result.error);
            else
                hud.completed("done");
        }

        if (st.ag_json) {
            std::cout << result_json.dump(2) << "\n";
        } else if (result.ok) {
            if (!saw_text_delta) {
                bool want_render = false;
                if (st.ag_markdown == "plain")
                    want_render = false;
                else if (st.ag_markdown == "render")
                    want_render = true;
                else
                    want_render = cli_tty::stdout_is_tty();

                bool use_color = false;
                if (st.ag_color == "never")
                    use_color = false;
                else if (st.ag_color == "always")
                    use_color = true;
                else
                    use_color = cli_tty::stdout_is_tty() && !cli_tty::env_requests_plain_output();

#if defined(_WIN32)
                if (want_render && use_color)
                    cli_tty::win_enable_virtual_terminal_processing_stdout();
#endif

                std::string out = result.final_text;
                if (want_render) {
                    // Default MdOptions: GFM on (tables=true, etc.). Pipe tables → padded | grid in md_terminal.cpp.
                    html::MdOptions mdopts{};
                    int w = cli_tty::terminal_width_columns();
                    if (w <= 0)
                        w = 80;
                    out = html::markdown_to_terminal(result.final_text, mdopts, use_color, w);
                }
                std::cout << out;
                if (out.empty() || out.back() != '\n')
                    std::cout << '\n';
            } else if (result.final_text.empty() || result.final_text.back() != '\n') {
                std::cout << '\n';
            }
        } else if (result.cancelled) {
            std::cerr << "llm agent: interrupted (Ctrl+C)\n";
        }

        // Write response to --dst file (raw markdown text, or JSON when --json).
        // ANSI rendering is not applied to file output; the file always receives
        // clean text so it is usable as markdown or as plain input to other tools.
        if (!resolved_dst_path.empty() && result.ok) {
            std::string dst_content;
            if (st.ag_json) {
                dst_content = result_json.dump(2) + "\n";
            } else {
                dst_content = result.final_text;
                if (dst_content.empty() || dst_content.back() != '\n')
                    dst_content += '\n';
            }
            std::error_code dst_mk_ec;
            const fs::path dst_p = media::path::path_from_utf8(resolved_dst_path);
            if (dst_p.has_parent_path())
                fs::create_directories(dst_p.parent_path(), dst_mk_ec);
            std::ofstream dst_f(dst_p, std::ios::out | std::ios::trunc);
            if (!dst_f) {
                logger::warn(std::string("llm agent: --dst: could not write to ") + resolved_dst_path);
            } else {
                dst_f << dst_content;
                logger::debug(std::string("llm agent: --dst written -> ") + resolved_dst_path);
                media::commands::emit_run_artifact(resolved_dst_path, "llm");
            }
        }

        log_llm_usage_aggregate_debug(result.llm_usage_aggregate, turn_wall_ms);

        if (result.cancelled)
            return 130;
        if (!result.ok)
            return 1;
        if (!interactive_multi_turn) {
            if (st.ag_scheduler)
                return run_scheduler_blocking(prov, st.ag_scheduler_timeout_s,
                                              st.ag_scheduler_exit_when_idle);
            return 0;
        }

        std::cout << "> " << std::flush;
        std::string next_prompt;
        if (!std::getline(std::cin, next_prompt)) {
            if (st.ag_scheduler)
                return run_scheduler_blocking(prov, st.ag_scheduler_timeout_s,
                                              st.ag_scheduler_exit_when_idle);
            return 0;
        }
        if (next_prompt.find_first_not_of(" \t\r\n") == std::string::npos)
            continue;
        if (hud.active())
            hud.busy("working");
        current_prompt = std::move(next_prompt);
    }
}

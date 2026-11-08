// OpenAI HTTP server for `llm agent --serve`.
// Compiled on the Tanit host lists. pm-llm does not link this translation unit:
// routes still call the agent loop, video, LAN, and IPC.

#include "pm_image_cmd_llm_agent_server.hpp"
#include "json/json_utils.hpp"
#include "cli/llm_vfs_layer.hpp"
#include "lan/tanit_lan.hpp"

#include "pm_image_cli_helpers.hpp"

#include "core/cli_cancel.hpp"
#include "core/cmd_ipc.hpp"
#include "core/command_runner.hpp"
#include "core/launcher_commands.hpp"
#include "llm/chat_host_events.hpp"
#include "core/settings_runtime.hpp"
#include "llm/agent_skills.hpp"
#if defined(_WIN32)
#include "win/settings_store.hpp"
#endif
#include "llm/agent.hpp"
#include "llm/agent_launch.hpp"
#include "llm/agent_tool_provider.hpp"
#include "llm/agent_memory.hpp"
#include "llm/agent_memory_artifacts.hpp"
#include "llm/agent_memory_recall.hpp"
#include "llm/agent_runner.hpp"
#include "llm/agent_runner_session.hpp"
#include "llm/agent_turn_options.hpp"
#include "llm/agent_each.hpp"
#include "llm/agent_decide.hpp"
#include "llm/llm_image_tool_defaults.hpp"
#include "llm/path_tool_executor.hpp"
#include "llm/tools/browser_use/browser_use_page_model.hpp"
#include "logging/logger.h"
#if defined(_WIN32)
#include "win/ui_next/ChatWebPanel_TypeaheadPresets.hpp"
#endif
#include "security/consent_surface.hpp"
#include "llm/tools/inquire/InquireTools.hpp"
#include "llm/chat_session_store.hpp"
#include "lib/pm_zitadel_oauth.hpp"
#include "core/pixlwiz_user_budget.hpp"
#include "plugins/chat/host_chat.hpp"
#include "audio/session/audio_events.hpp"
#if defined(FEATURE_VIDEO) && FEATURE_VIDEO && defined(FEATURE_VIDEO_STREAM) && FEATURE_VIDEO_STREAM
#include "video_stream_host.hpp"
#endif

#if defined(_WIN32)
#include "performance/counters.hpp"
#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>
#endif

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

namespace fs = std::filesystem;

namespace media_cli {

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO && defined(FEATURE_VIDEO_STREAM) && FEATURE_VIDEO_STREAM
static std::shared_ptr<VideoStreamHost> g_llm_video_host;
#endif

static void stop_llm_video_host()
{
#if defined(FEATURE_VIDEO) && FEATURE_VIDEO && defined(FEATURE_VIDEO_STREAM) && FEATURE_VIDEO_STREAM
    if (g_llm_video_host)
        g_llm_video_host->stop();
#endif
}

namespace {

class InflightGate {
public:
    struct Stats {
        int concurrency = 1;
        int inflight    = 0;
        int waiting     = 0;
        int max_waiting = 0; ///< 0 = unbounded waiters
        int peak_inflight = 0;
        int peak_waiting  = 0;
        std::uint64_t accepted = 0;
        std::uint64_t waited   = 0; // accepted turns that blocked on the gate
        std::uint64_t rejected = 0; // turned away because the waiter cap was full
    };

    /// @param max_n max concurrent turns.
    /// @param max_waiting cap on parkable HTTP workers waiting for a slot.
    ///        0 = unbounded (legacy). Prefer http_workers - concurrency - reserve
    ///        so consent / control routes keep free workers.
    explicit InflightGate(int max_n, int max_waiting = 0)
        : max_n_(max_n < 1 ? 1 : max_n)
        , max_waiting_(max_waiting < 0 ? 0 : max_waiting)
    {
    }

    Stats stats() const
    {
        std::lock_guard lock(mu_);
        Stats s;
        s.concurrency   = max_n_;
        s.inflight      = inflight_;
        s.waiting       = waiting_;
        s.max_waiting   = max_waiting_;
        s.peak_inflight = peak_inflight_;
        s.peak_waiting  = peak_waiting_;
        s.accepted      = accepted_;
        s.waited        = waited_;
        s.rejected      = rejected_;
        return s;
    }

    class Slot {
    public:
        /// Acquire a turn slot. When the concurrency limit is hit, park until a
        /// slot frees — unless the waiter cap is already full, then @ref ok is
        /// false and the caller should return HTTP 503.
        explicit Slot(InflightGate& g) : g_(&g)
        {
            const auto t0 = std::chrono::steady_clock::now();
            std::unique_lock lock(g_->mu_);
            if (g_->inflight_ >= g_->max_n_) {
                if (g_->max_waiting_ > 0 && g_->waiting_ >= g_->max_waiting_) {
                    ++g_->rejected_;
                    g_ = nullptr;
                    return;
                }
                ++g_->waiting_;
                if (g_->waiting_ > g_->peak_waiting_)
                    g_->peak_waiting_ = g_->waiting_;
                ++g_->waited_;
                g_->cv_.wait(lock, [&] {
                    return g_->inflight_ < g_->max_n_;
                });
                --g_->waiting_;
            }
            queue_wait_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
            ++g_->inflight_;
            ++g_->accepted_;
            if (g_->inflight_ > g_->peak_inflight_)
                g_->peak_inflight_ = g_->inflight_;
        }
        ~Slot()
        {
            if (!g_)
                return;
            {
                std::lock_guard lock(g_->mu_);
                --g_->inflight_;
            }
            g_->cv_.notify_one();
        }
        Slot(const Slot&)            = delete;
        Slot& operator=(const Slot&) = delete;

        bool ok() const { return g_ != nullptr; }
        explicit operator bool() const { return ok(); }
        long long queue_wait_ms() const { return queue_wait_ms_; }

    private:
        InflightGate* g_             = nullptr;
        long long     queue_wait_ms_ = 0;
    };

private:
    int                     max_n_         = 1;
    int                     max_waiting_   = 0;
    int                     inflight_      = 0;
    int                     waiting_       = 0;
    int                     peak_inflight_ = 0;
    int                     peak_waiting_  = 0;
    std::uint64_t           accepted_      = 0;
    std::uint64_t           waited_        = 0;
    std::uint64_t           rejected_      = 0;
    mutable std::mutex      mu_;
    std::condition_variable cv_;
};

using media::llm::agent::cliopt::BrowserEnvGuard;
using media::llm::agent::cliopt::ParsedChatRequest;
using media::llm::agent::cliopt::StreamEmitFn;
using media::llm::agent::cliopt::TurnOutcome;
using media::llm::agent::cliopt::VictoriaTarget;
using media::llm::agent::cliopt::apply_provider_defaults;
using media::llm::agent::cliopt::clip_text;
using media::llm::agent::cliopt::completion_success_json;
using media::llm::agent::cliopt::is_video_route;
using media::llm::agent::cliopt::log_level_name;
using media::llm::agent::cliopt::looks_like_loopback_script;
using media::llm::agent::cliopt::openai_completion_id;
using media::llm::agent::cliopt::openai_created_unix;
using media::llm::agent::cliopt::openai_error;
using media::llm::agent::cliopt::parse_chat_request;
using media::llm::agent::cliopt::parse_host_tool_provider_spec;
using media::llm::agent::cliopt::resolve_request_selection;
using media::llm::agent::cliopt::sse_data_line;
using media::llm::agent::cliopt::stage_message_images;
using media::llm::agent::cliopt::stage_mobile_attachments;
using media::llm::agent::cliopt::trim_ascii;
using media::llm::agent::cliopt::usage_fields;
using media::llm::agent::cliopt::utc_now_iso;
using media::llm::agent::cliopt::victoria_target;

bool looks_like_preset(const std::string& model_or_preset)
{
    if (model_or_preset.empty() || looks_like_loopback_script(model_or_preset))
        return false;
    media::runtime_settings::ChatProviderPreset p;
    std::string err;
    return media::runtime_settings::find_chat_provider_preset(model_or_preset, p, err);
}

nlohmann::json models_list_json()
{
    nlohmann::json data = nlohmann::json::array();
    std::vector<media::runtime_settings::ChatProviderPreset> presets;
    std::string err;
    (void)media::runtime_settings::load_chat_provider_presets(presets, err);

    const long created = openai_created_unix();
    if (presets.empty()) {
        data.push_back({
            {"id", "default"},
            {"object", "model"},
            {"created", created},
            {"owned_by", "tanit"},
        });
    } else {
        for (const auto& p : presets) {
            const std::string id = !p.name.empty() ? p.name : (!p.id.empty() ? p.id : "preset");
            data.push_back({
                {"id", id},
                {"object", "model"},
                {"created", created},
                {"owned_by", "tanit"},
                {"tanit_preset_id", p.id},
                {"tanit_is_default", p.is_default},
            });
        }
    }
    return nlohmann::json{{"object", "list"}, {"data", std::move(data)}};
}


std::string resolve_request_cwd(const httplib::Request& req,
                                const nlohmann::json& body,
                                const AgentServerOptions& opt)
{
    const std::string header =
        req.has_header("X-Tanit-Cwd") ? req.get_header_value("X-Tanit-Cwd") : std::string{};
    return media::llm::agent::cliopt::resolve_body_cwd(body, header, opt);
}

std::string resolve_request_session(const httplib::Request& req, const nlohmann::json& body)
{
    const std::string header =
        req.has_header("X-Tanit-Session") ? req.get_header_value("X-Tanit-Session") : std::string{};
    return media::llm::agent::cliopt::resolve_body_session(body, header);
}

void apply_request_provider_overrides(const httplib::Request& req,
                                      const nlohmann::json& body,
                                      std::string& api_key,
                                      std::string& base_url)
{
    const std::string key = req.has_header("X-Tanit-Provider-Key")
        ? req.get_header_value("X-Tanit-Provider-Key") : std::string{};
    const std::string base = req.has_header("X-Tanit-Provider-Base-Url")
        ? req.get_header_value("X-Tanit-Provider-Base-Url") : std::string{};
    media::llm::agent::cliopt::apply_body_provider_overrides(body, key, base, api_key, base_url);
}

void attach_session_to_turn(media::llm::agent::Turn& turn, const std::string& session_id)
{
    if (session_id.empty())
        return;
    nlohmann::json mem_state, recent_evs;
    media::llm::agent::session_load(session_id, mem_state, recent_evs, 20);
    turn.task_id       = session_id;
    turn.memory_state  = std::move(mem_state);
    turn.recent_events = std::move(recent_evs);
}

bool bearer_authorized(const httplib::Request& req, const std::string& expected_key)
{
    return media::llm::agent::cliopt::bearer_token_authorized(
        req.get_header_value("Authorization"),
        req.get_header_value("x-api-key"),
        req.get_header_value("X-Api-Key"),
        req.get_header_value("api-key"),
        expected_key);
}

void apply_capability_flags(media::llm::agent::Turn& turn, const AgentServerOptions& opt)
{
    const auto caps = media::llm::agent::cliopt::server_turn_caps(opt);
    if (caps.clear_mcp) {
        turn.mcp_tools_enabled = false;
        turn.tool_policy_override_set = true;
    }
    if (caps.skills_on) {
        turn.skills_enabled = true;
        turn.skills_roaming_enabled = true;
        turn.skills_workspace_enabled = true;
        turn.skill_policy_override_set = true;
    } else if (caps.skills_off) {
        turn.skills_enabled = false;
        turn.skill_policy_override_set = true;
    }
    if (caps.planner_off) {
        turn.planner_enabled = false;
        turn.planner_strategy = media::llm::agent::PlannerStrategy::Disabled;
    }
    if (caps.fs_jail)
        turn.fs_jail = true;
}

TurnOutcome run_one_completion(const AgentServerOptions& opt,
                               const nlohmann::json&     body,
                               const ParsedChatRequest&  parsed,
                               const std::string&        request_cwd,
                               const std::string&        request_session,
                               const std::string&        request_id,
                               const StreamEmitFn&       stream_emit,
                               const std::string&        req_provider_key = {},
                               const std::string&        req_provider_base = {})
{
    TurnOutcome out;
    out.session_id = request_session;
    logger::info("llm-server turn phase=resolve id=" + request_id
                 + " prompt_chars=" + std::to_string(parsed.user_prompt.size()));

    media::llm::agent::launch::AgentLaunchSpec launch_spec;
    launch_spec.preset         = opt.preset;
    launch_spec.router         = opt.router;
    launch_spec.model          = opt.model;
    // Resolve with process/profile credentials first. Per-request JWT (below) is
    // applied only for router=tanit so OpenAI presets are not fed a Zitadel bearer.
    launch_spec.api_key        = opt.api_key;
    launch_spec.base_url       = opt.base_url;
    launch_spec.timeout_ms     = opt.timeout_ms;
    launch_spec.max_iterations = opt.max_iterations;
    launch_spec.no_tools       = opt.no_tools;
    launch_spec.api_mode       = opt.api_mode;
    launch_spec.streaming      = opt.streaming;
    launch_spec.planner_enabled = opt.planner;
    launch_spec.planner_schema_budget = opt.planner_budget;
    launch_spec.runner         = opt.runner;

    const std::string req_model = body.value("model", "");
    if (!req_model.empty()) {
        if (looks_like_loopback_script(req_model)) {
            // OpenAI `model` carries a loopback script (inline JSON / @file).
            launch_spec.model = req_model;
            if (launch_spec.router.empty())
                launch_spec.router = "loopback";
        } else if (looks_like_preset(req_model)) {
            launch_spec.preset = req_model;
            // The named preset owns its model. A process-level --model from the
            // previous chat (text-fast on an OpenAI preset) must not stick.
            launch_spec.model.clear();
        } else {
            launch_spec.model = req_model;
        }
    }

    media::llm::agent::launch::AgentLaunchResolved resolved;
    std::string launch_err;
    if (!media::llm::agent::launch::resolve_agent_launch(launch_spec, resolved, launch_err)) {
        out.error = launch_err.empty() ? "resolve_agent_launch failed" : launch_err;
        logger::warn("llm-server turn phase=resolve_error id=" + request_id
                     + " err=" + out.error);
        return out;
    }

    // LiteLLM only: caller's Zitadel JWT as provider key (pm-pics X-Tanit-Provider-Key).
    if (resolved.provider.router == "tanit") {
        if (!req_provider_key.empty())
            resolved.provider.api_key = req_provider_key;
        if (!req_provider_base.empty())
            resolved.provider.base_url = req_provider_base;
    }

    out.model = !resolved.provider.model.empty()
                    ? resolved.provider.model
                    : (!req_model.empty() ? req_model
                                          : (!launch_spec.preset.empty() ? launch_spec.preset
                                                                         : "default"));
    // Don't leak giant loopback scripts into the OpenAI response model field.
    if (looks_like_loopback_script(out.model))
        out.model = "loopback";
    logger::info("llm-server turn phase=resolved id=" + request_id
                 + " router=" + resolved.provider.router
                 + " model=" + out.model);

    media::llm::ChatSettingsOverrideGuard tool_settings_guard(resolved.chat_settings);

    std::string folder = !request_cwd.empty() ? request_cwd : opt.cwd;
    if (!folder.empty()) {
        std::error_code ec;
        fs::path fp = media::path::path_from_utf8(folder);
        if (!fp.is_absolute())
            fp = fs::absolute(fp, ec);
        if (ec || !fs::exists(fp)) {
            out.error = std::string("cwd does not exist: ") + folder;
            return out;
        }
        folder = media::path::path_to_utf8(fp);
    }

    media::llm::agent::Turn turn;
    turn.user_prompt        = parsed.user_prompt;
    turn.seed_chat_messages = parsed.seed_chat_messages;
    turn.folder_hint        = folder;
    turn.selection          = resolve_request_selection(body);
    {
        std::string stage_err = stage_mobile_attachments(body, folder, turn.selection);
        if (!stage_err.empty()) {
            out.error = std::move(stage_err);
            return out;
        }
    }
    {
        std::string stage_err = stage_message_images(body, folder, turn.selection);
        if (!stage_err.empty()) {
            out.error = std::move(stage_err);
            return out;
        }
    }
    if (turn.user_prompt.empty() && !turn.selection.empty())
        turn.user_prompt = "Use the selected context.";
    if (body.contains("tanit") && body["tanit"].is_object()
        && body["tanit"].contains("tool_providers")
        && body["tanit"]["tool_providers"].is_array()) {
        for (const auto& v : body["tanit"]["tool_providers"]) {
            if (v.is_string()) {
                const std::string id = v.get<std::string>();
                if (!id.empty())
                    turn.tool_provider_ids.push_back(id);
            }
        }
    }
    media::llm::agent::launch::apply_launch_policy_to_turn(launch_spec, resolved, turn);
    apply_capability_flags(turn, opt);
    attach_session_to_turn(turn, request_session);
    // Same precedence as `llm agent`: settings profile, then
    // PM_LLM_STRUCTURED_MEMORY_FEATURES, then PM_LLM_MEMORY_RECALL_PROFILE.
    // Loopback benches set the feature mask to none so a long client ramp does
    // not reindex every prior session on each request.
    media::llm::agent::apply_agent_memory_settings_to_turn(turn);
    turn.structured_memory_features =
        media::llm::agent::apply_structured_memory_features_env(
            turn.structured_memory_features, "PM_LLM_STRUCTURED_MEMORY_FEATURES");
    if (const char* recall_profile = std::getenv("PM_LLM_MEMORY_RECALL_PROFILE");
        recall_profile && *recall_profile) {
        media::llm::agent::apply_memory_recall_profile(
            turn.structured_memory_features, recall_profile);
    }
    {
        // Request system message wins over CLI --system-prompt when present.
        const std::string sys_raw =
            !parsed.system_prompt.empty() ? parsed.system_prompt : opt.system_prompt;
        std::string sys_err;
        if (!media::llm::agent::options::apply_system_prompt_option(sys_raw, turn, sys_err)) {
            out.error = sys_err;
            return out;
        }
        std::string planner_err;
        if (!media::llm::agent::options::apply_planner_prompt_option(opt.planner_prompt, turn,
                                                                    planner_err)) {
            out.error = planner_err;
            return out;
        }
    }
    media_cli::parse_agent_disable_tools(opt.disable_tools, opt.no_tools, turn.disabled_path_tools,
                                         &turn.disabled_mcp_tools);
    media_cli::parse_agent_enable_tools(opt.enable_tools, opt.no_tools, turn.allowed_path_tools,
                                        turn.planner_path_tool_allowlist_set);
    turn.parallel_tool_dispatch = !opt.no_parallel_tools;

#if !FEATURE_HOME_LLM_TOOLS
    turn.disabled_path_tools.clear();
#endif

    const auto& prov = resolved.provider;
    if (prov.api_key.empty() && prov.router != "llama" && prov.router != "loopback"
        && prov.router != "mock") {
        out.error = "no API key configured for router '" + prov.router
                    + "' (pass --api-key or configure Chat / provider keys)";
        return out;
    }

#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
    const std::string consent_mode = pm::security::resolve_consent_ui_mode(
        opt.consent_ui,
        resolved.has_preset ? resolved.selected_preset.consent_ui : std::string{},
        "auto-deny");
    const pm::security::ConsentSurfaceScope consent_surface(consent_mode, opt.consent_owner);
    // HTTP hosts (pm-pics TanitChat) answer ask_user via SSE + POST /v1/chat/ask.
    if (consent_surface.resolved() == "stream")
        pm::llm::inquire::ensure_stream_ask_ui_backend();
#endif

    media::llm::agent::AgentRunnerOptions runner_opt;
    runner_opt.kind = media::llm::agent::AgentRunnerKind::Native;
    if (!opt.runner.empty() || !resolved.effective_runner.empty()) {
        const std::string r =
            !opt.runner.empty() ? opt.runner : resolved.effective_runner;
        if (auto k = media::llm::agent::parse_agent_runner_kind(r))
            runner_opt.kind = *k;
    }

    std::string streamed;
    const auto t0 = std::chrono::steady_clock::now();
#if defined(_WIN32)
    pm::perf::emit_event("agent.completion.start", "llm-server",
                         std::string("model=") + out.model);
#endif
    const std::string sse_id      = openai_completion_id();
    const long        sse_created = openai_created_unix();
    out.stream_id      = sse_id;
    out.stream_created = sse_created;
    bool              role_sent        = false;
    bool              stream_had_delta = false;
    std::size_t       stream_delta_count = 0;

    auto write_oai_delta = [&](const std::string& content_piece) -> bool {
        if (!stream_emit)
            return true;
        nlohmann::json delta = nlohmann::json::object();
        if (!role_sent) {
            delta["role"] = "assistant";
            role_sent     = true;
        }
        if (!content_piece.empty())
            delta["content"] = content_piece;
        nlohmann::json chunk{
            {"id", sse_id},
            {"object", "chat.completion.chunk"},
            {"created", sse_created},
            {"model", out.model},
            {"choices",
             nlohmann::json::array(
                 {{{"index", 0}, {"delta", std::move(delta)}, {"finish_reason", nullptr}}})},
        };
        const std::string line = sse_data_line(chunk);
        return stream_emit(line);
    };

    auto write_tanit_event = [&](const std::string& type, const nlohmann::json& payload) -> bool {
        if (!stream_emit)
            return true;
        nlohmann::json ev{{"object", "tanit.event"}, {"type", type}, {"model", out.model}};
        for (auto it = payload.begin(); it != payload.end(); ++it)
            ev[it.key()] = it.value();
        const std::string line = "event: tanit." + type + "\ndata: " + ev.dump() + "\n\n";
        return stream_emit(line);
    };

    // Short transcript fields. The model already has the full envelope in-process;
    // streaming it stalls the side panel.
    struct ToolNote {
        std::string text;
        std::string id;
        long long ms = -1;
    };
    auto tool_note = [](const media::llm::agent::Event& ev) -> ToolNote {
        auto clip = [](std::string s, std::size_t n) {
            for (char& c : s) {
                if (c == '\n' || c == '\r' || c == '\t' || c == '"')
                    c = ' ';
            }
            if (s.size() <= n)
                return s;
            // Do not split a UTF-8 sequence: walk back past continuation bytes.
            std::size_t cut = n;
            while (cut > 0
                   && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
                --cut;
            s.resize(cut);
            return s;
        };
        auto cryptic = [](const std::string& s) {
            if (s.empty() || s.find(' ') != std::string::npos)
                return false;
            return s.find_first_not_of(
                       "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789")
                       == std::string::npos
                   && s.size() >= 6;
        };
        auto arg_detail = [&](const nlohmann::json& args, const std::string& tool) -> std::string {
            if (!args.is_object())
                return {};
            const char* keys[] = {
                "command", "cmd", "query", "text", "url", "path", "prompt",
                "pattern", "instruction",
            };
            if (tool == "run") {
                for (const char* key : {"command", "cmd"}) {
                    if (!args.contains(key) || !args[key].is_string())
                        continue;
                    std::string s = args[key].get<std::string>();
                    if (!s.empty())
                        return clip(std::move(s), 80);
                }
            }
            for (const char* key : keys) {
                if (!args.contains(key) || !args[key].is_string())
                    continue;
                std::string s = args[key].get<std::string>();
                if (s.empty() || cryptic(s))
                    continue;
                return clip(std::move(s), 80);
            }
            return {};
        };
        auto friendly = [](const std::string& name) -> std::string {
            if (name == "browser_read") return "Read page";
            if (name == "browser_find") return "Find";
            if (name == "browser_click") return "Click";
            if (name == "browser_type") return "Type";
            if (name == "browser_select") return "Select";
            if (name == "browser_navigate") return "Open page";
            if (name == "browser_scope") return "Scope page";
            if (name == "browser_batch") return "Page actions";
            if (name == "run") return "Run";
            if (name == "info_lookup") return "Look up";
            if (name == "write_file") return "Write file";
            if (name == "download_url") return "Download";
            if (name == "file_read") return "Read file";
            if (name == "file_glob") return "Find files";
            if (name == "app_batch") return "App actions";
            if (name == "app_inspect_dump") return "Inspect app";
            if (name == "app_command") return "App command";
            if (name == "memory_find") return "Search memory";
            if (name == "service_page_get") return "Open service page";
            std::string s = name;
            for (char& c : s)
                if (c == '_')
                    c = ' ';
            return s;
        };
        ToolNote note;
        std::string name = ev.tool_name.empty() ? "tool" : ev.tool_name;
        std::string detail;
        std::string err;
        if (ev.payload.is_object()) {
            if (ev.payload.contains("id") && ev.payload["id"].is_string())
                note.id = ev.payload["id"].get<std::string>();
            if (ev.payload.contains("duration_ms") && ev.payload["duration_ms"].is_number())
                note.ms = ev.payload["duration_ms"].get<long long>();
            if (ev.kind == media::llm::agent::Event::ToolCall
                && ev.payload.contains("arguments") && ev.payload["arguments"].is_object())
                detail = arg_detail(ev.payload["arguments"], ev.tool_name);
            const nlohmann::json* env = nullptr;
            if (ev.payload.contains("envelope") && ev.payload["envelope"].is_object())
                env = &ev.payload["envelope"];
            if (env && env->contains("error")) {
                const auto& e = (*env)["error"];
                if (e.is_string())
                    err = e.get<std::string>();
                else if (e.is_object())
                    err = e.value("message", std::string{});
            }
        }
        note.text = friendly(name);
        if (!detail.empty())
            note.text += " " + detail;
        else if (!err.empty() && !cryptic(err))
            note.text += " " + clip(std::move(err), 80);
        return note;
    };

#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
    std::uint64_t consent_ticket = 0;
    std::uint64_t ask_ticket = 0;
    if (stream_emit) {
        auto emit = [&](const std::string& type, const std::string& payload_json) {
            const auto payload = media::json::try_parse(payload_json);
            if (!payload.is_object())
                return false;
            return write_tanit_event(type, payload);
        };
        consent_ticket = pm::security::bind_stream_consent_emit(emit);
        ask_ticket = pm::llm::inquire::bind_stream_ask_emit(emit);
    }
    struct ConsentEmitRestore {
        std::uint64_t consent_ticket = 0;
        std::uint64_t ask_ticket = 0;
        ~ConsentEmitRestore()
        {
            // Unblock any ask_user still waiting if the SSE client dropped.
            if (ask_ticket)
                pm::llm::inquire::cancel_stream_ask();
            if (consent_ticket)
                pm::security::restore_stream_consent_emit(consent_ticket);
            if (ask_ticket)
                pm::llm::inquire::restore_stream_ask_emit(ask_ticket);
        }
    } consent_restore{consent_ticket, ask_ticket};
#endif

    logger::info("llm-server turn phase=agent_start id=" + request_id);
    auto result = media::llm::agent::run_agent_turn(
        turn, prov, runner_opt, [&](const media::llm::agent::Event& ev) {
            if (ev.kind == media::llm::agent::Event::TextDelta) {
                if (!ev.text.empty()) {
                    streamed += ev.text;
                    stream_had_delta = true;
                    stream_delta_count += 1;
                    if (!write_oai_delta(ev.text))
                        return false;
                }
            } else if (ev.kind == media::llm::agent::Event::AssistantText) {
                // Deltas already streamed the reply — AssistantText is the final snapshot only.
                if (!stream_had_delta && !ev.text.empty()) {
                    streamed += ev.text;
                    if (!write_oai_delta(ev.text))
                        return false;
                }
            } else if (ev.kind == media::llm::agent::Event::Thinking) {
                if (!write_tanit_event("thinking", {{"text", ev.text}}))
                    return false;
            } else if (ev.kind == media::llm::agent::Event::ToolCall
                       || ev.kind == media::llm::agent::Event::ToolResult) {
                const bool starting = ev.kind == media::llm::agent::Event::ToolCall;
                const ToolNote note = tool_note(ev);
                logger::info("llm-server turn phase="
                             + std::string(starting ? "tool_start" : "tool_done")
                             + " id=" + request_id + " tool="
                             + (ev.tool_name.empty() ? std::string("?") : ev.tool_name)
                             + (note.ms >= 0 ? " duration_ms=" + std::to_string(note.ms)
                                             : std::string{}));
                nlohmann::json p{{"name", ev.tool_name}, {"text", note.text}};
                if (!note.id.empty())
                    p["id"] = note.id;
                if (note.ms >= 0)
                    p["duration_ms"] = note.ms;
                if (!starting && ev.payload.is_object()
                    && ev.payload.contains("envelope") && ev.payload["envelope"].is_object()) {
                    const auto& env = ev.payload["envelope"];
                    if (env.contains("results") && env["results"].is_array()) {
                        for (const auto& row : env["results"]) {
                            if (!row.is_object() || !row.value("ok", false))
                                continue;
                            std::string produced = row.value("output_path", std::string{});
                            if (produced.empty())
                                continue;
                            p["path"] = std::move(produced);
                            break;
                        }
                    }
                }
                if (!write_tanit_event(starting ? "tool_call" : "tool_result", p))
                    return false;
            } else if (ev.kind == media::llm::agent::Event::ToolFileProgress
                       && (ev.tool_name == "run" || ev.tool_name == "download_url")) {
                nlohmann::json p = ev.payload.is_object() ? ev.payload : nlohmann::json::object();
                p["name"] = ev.tool_name;
                if (!write_tanit_event("tool_file_progress", p))
                    return false;
            }
            return !media::cli::cancel_requested();
        });
    const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    logger::info("llm-server turn phase=agent_done id=" + request_id
                 + " ok=" + (result.ok ? std::string("1") : std::string("0"))
                 + " wall_ms=" + std::to_string(wall_ms)
                 + " streamed_chars=" + std::to_string(streamed.size()));
#if defined(_WIN32)
    pm::perf::emit_event(
        result.ok ? "agent.completion.done" : "agent.completion.error", "llm-server",
        std::string("wall_ms=") + std::to_string(wall_ms)
            + (result.ok ? "" : (std::string(" err=") + (result.error.empty() ? "?" : result.error))));
    // Do not call sample() here — that would reset CPU deltas to a post-idle
    // window and poison the next /v1/sysprobe (benchmarks sample mid-flight).
#endif
    (void)wall_ms;

    if (!result.ok) {
        out.error = result.error.empty() ? "agent turn failed" : result.error;
        return out;
    }
    out.ok         = true;
    out.final_text = !result.final_text.empty() ? result.final_text : streamed;
    out.streamed_chars = streamed.size();
    out.stream_deltas  = stream_delta_count;
    out.finish_reason  = !result.llm_finish_reason.empty() ? result.llm_finish_reason : "stop";
    if (result.llm_usage_aggregate.is_object())
        out.usage = result.llm_usage_aggregate;

    logger::trace("llm-server completion: streamed_chars=" + std::to_string(out.streamed_chars)
                  + " final_chars=" + std::to_string(out.final_text.size())
                  + " deltas=" + std::to_string(out.stream_deltas)
                  + " finish_reason=" + out.finish_reason
                  + " had_delta=" + (stream_had_delta ? "1" : "0"));
    if (out.finish_reason == "length")
        logger::warn("llm-server completion: provider finish_reason=length (output likely truncated)");

    // Live stream: flush final text if no deltas were emitted. Caller writes stop + [DONE]
    // after filling queue_wait_ms / exec_ms.
    if (stream_emit && !role_sent && !out.final_text.empty())
        (void)write_oai_delta(out.final_text);
    return out;
}



/// Inherit server provider credentials only when the request is not redirecting
/// to a foreign base URL (avoids attaching the daemon key to a caller URL).

void respond_queue_full(httplib::Response& res)
{
    res.status = 503;
    res.set_header("Retry-After", "1");
    res.set_content(
        openai_error("server busy: turn concurrency queue is full", "server_error",
                     "queue_full")
            .dump(),
        "application/json");
}

struct PathBaseGuard {
    explicit PathBaseGuard(const std::string& cwd, bool fs_jail = false)
    {
        if (!cwd.empty()) {
            media::llm::path::set_agent_path_base(cwd);
            active_ = true;
        }
        if (fs_jail) {
            media::llm::path::set_agent_fs_jail(true);
            jail_ = true;
        }
    }
    ~PathBaseGuard()
    {
        if (jail_)
            media::llm::path::clear_agent_fs_jail();
        if (active_)
            media::llm::path::clear_agent_path_base();
    }
    PathBaseGuard(const PathBaseGuard&)            = delete;
    PathBaseGuard& operator=(const PathBaseGuard&) = delete;

private:
    bool active_ = false;
    bool jail_   = false;
};

struct PathBlocklistGuard {
    explicit PathBlocklistGuard(const std::vector<std::string>& disabled)
    {
        if (!disabled.empty()) {
            media::llm::path::set_agent_tool_blocklist(disabled);
            active_ = true;
        }
    }
    ~PathBlocklistGuard()
    {
        if (active_)
            media::llm::path::clear_agent_tool_blocklist();
    }
    PathBlocklistGuard(const PathBlocklistGuard&)            = delete;
    PathBlocklistGuard& operator=(const PathBlocklistGuard&) = delete;

private:
    bool active_ = false;
};


bool is_loopback_addr(std::string_view addr)
{
    return media::llm::agent::cliopt::is_loopback_host(addr);
}

/// Set while `llm agent --serve --lan` is running. Proxied phones are not loopback.
lan::Service* g_lan = nullptr;

bool lan_hop_present(const httplib::Request& req)
{
    return req.has_header(lan::k_hop_header);
}

bool lan_hop_ok(const httplib::Request& req)
{
    if (!g_lan || !lan_hop_present(req))
        return false;
    return g_lan->hop_matches(req.get_header_value(lan::k_hop_header));
}

/// True for a client that dialed this process directly on loopback.
/// A phone forwarded by the TLS proxy is loopback at the TCP layer and is not local.
bool machine_local_request(const httplib::Request& req)
{
    if (lan_hop_present(req))
        return false;
    return is_loopback_addr(req.remote_addr);
}

std::vector<std::string> request_secrets(const httplib::Request& req)
{
    std::vector<std::string> out;
    auto add = [&](std::string s) {
        s = trim_ascii(std::move(s));
        if (!s.empty())
            out.push_back(std::move(s));
    };
    const std::string auth = trim_ascii(req.get_header_value("Authorization"));
    if (auth.size() >= 7) {
        bool bearer = true;
        const char pfx[] = "bearer ";
        for (int i = 0; i < 7; ++i) {
            const auto c = static_cast<unsigned char>(auth[static_cast<size_t>(i)]);
            if (std::tolower(c) != static_cast<unsigned char>(pfx[i])) {
                bearer = false;
                break;
            }
        }
        if (bearer)
            add(auth.substr(7));
    }
    for (const char* h : {"x-api-key", "api-key"})
        add(req.get_header_value(h));
    return out;
}

bool forward_victoria_ndjson(const std::string& ndjson)
{
    const VictoriaTarget target = victoria_target();
    if (!target.enabled || ndjson.empty())
        return false;
    httplib::Client cli(target.host, target.port);
    cli.set_connection_timeout(0, 400000);
    cli.set_read_timeout(0, 800000);
    cli.set_write_timeout(0, 800000);
    const auto res = cli.Post(
        "/insert/jsonline?_stream_fields=source,device,build,session_id"
        "&_msg_field=message&_time_field=_time",
        ndjson, "application/stream+json");
    return res && res->status >= 200 && res->status < 300;
}

bool device_scope_ok(const lan::DeviceAuth& dev, const httplib::Request& req)
{
    const std::string& path = req.path;
    if (path == "/v1/models" || path == "/v1/lan/info")
        return true;
    if (path == "/v1/logs")
        return dev.has("chat");
    if (is_video_route(path))
        return dev.has("video");
    if (path.rfind("/v1/vfs", 0) == 0)
        return dev.has("files");
    if (path == "/v1/llm/path-tools/call" || path == "/v1/ui/open-file")
        return dev.has("files") || dev.has("tools");
    if (path == "/v1/chat/commands/run" || path == "/v1/llm/each" || path == "/v1/llm/decide"
        || path == "/v1/ui/open" || path == "/v1/realtime/cancel-agent")
        return dev.has("tools");
    if (path == "/v1/realtime/start")
        return dev.has("chat") && dev.has("tools");
    if (path == "/v1/host-tool-providers" || path == "/v1/browser/page-model/rank")
        return false;
    return dev.has("chat");
}

bool lan_completion_without_tools(const httplib::Request& req, const std::string& serve_api_key)
{
    if (!g_lan)
        return false;
    const auto secrets = request_secrets(req);
    if (!serve_api_key.empty()) {
        for (const auto& secret : secrets) {
            if (lan::constant_time_equal(secret, serve_api_key))
                return false;
        }
    }
    for (const auto& secret : secrets) {
        lan::DeviceAuth dev;
        if (g_lan->authorize(secret, dev))
            return !dev.has("tools");
    }
    return false;
}


bool try_register_host_provider(const std::string& id, const std::string& url, std::string& err)
{
    if (!media::llm::agent::register_http_host_tool_provider(id, url, &err)) {
        if (err.empty())
            err = "invalid host tool provider (need id=http://127.0.0.1/...)";
        return false;
    }
    std::string shown = url;
    const auto qmark = shown.find('?');
    if (qmark != std::string::npos)
        shown.resize(qmark);
    logger::info(std::string("host tool provider registered: ") + id + " -> " + shown);
    return true;
}

void register_cli_host_tool_providers(const AgentServerOptions& opt)
{
    for (const auto& spec : opt.host_tool_providers) {
        std::string id;
        std::string url;
        std::string err;
        if (!parse_host_tool_provider_spec(spec, id, url)
            || !try_register_host_provider(id, url, err)) {
            logger::warn(std::string("llm agent --serve: ignored --host-tool-provider '")
                         + spec + "'" + (err.empty() ? "" : (": " + err)));
        }
    }
}

struct RtLive {
    std::uint64_t epoch = 0;
    std::atomic<bool> stop{false};
    std::atomic<int> tool_n{0};
    std::shared_ptr<pm::plugins::chat_host::RealtimeSession> session;
#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
    // Lives as long as the voice session. A spoken run happens after this
    // HTTP handler returns, so the session keeps the same consent surface
    // the process was started with. `stream` posts the card on the realtime
    // event queue. consent_ticket stays bound until the session stops —
    // start() returns as soon as the socket is up, before any tool ask.
    std::unique_ptr<pm::security::ConsentSurfaceScope> consent;
    std::atomic<std::uint64_t> consent_ticket{0};
    std::atomic<std::uint64_t> ask_ticket{0};
#endif
};

struct RtHub {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<nlohmann::json> events;
    std::uint64_t base = 0;
    std::uint64_t epoch = 0;
    std::uint64_t level_gen = 0;
    double level = 0;
    bool closed = false;
    std::shared_ptr<RtLive> live;
};

RtHub g_rt;

constexpr std::int64_t k_chat_keep_ms = 5 * 60 * 1000;

std::int64_t chat_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct ChatHub {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<nlohmann::json> events;
    std::uint64_t base = 0;
    std::uint64_t next_id = 1;
    std::vector<std::pair<std::uint64_t, std::string>> origins;
    std::vector<std::int64_t> origin_seen_ms;
};

ChatHub g_chat_hub;
thread_local std::string tls_chat_sender;

void prune_chat_hub_locked(std::int64_t now)
{
    while (!g_chat_hub.events.empty()) {
        const std::int64_t ts = g_chat_hub.events.front().value("ts", std::int64_t{0});
        if (now - ts < k_chat_keep_ms)
            break;
        g_chat_hub.events.erase(g_chat_hub.events.begin());
        ++g_chat_hub.base;
    }
    std::vector<std::pair<std::uint64_t, std::string>> origins;
    std::vector<std::int64_t> seen;
    for (std::size_t i = 0; i < g_chat_hub.origins.size(); ++i) {
        if (now - g_chat_hub.origin_seen_ms[i] >= k_chat_keep_ms)
            continue;
        origins.push_back(g_chat_hub.origins[i]);
        seen.push_back(g_chat_hub.origin_seen_ms[i]);
    }
    g_chat_hub.origins.swap(origins);
    g_chat_hub.origin_seen_ms.swap(seen);
}

void remember_chat_origin(std::uint64_t pid, const std::string& sender)
{
    if (pid == 0 || sender.empty())
        return;
    const std::int64_t now = chat_now_ms();
    std::lock_guard lock(g_chat_hub.mu);
    prune_chat_hub_locked(now);
    for (std::size_t i = 0; i < g_chat_hub.origins.size(); ++i) {
        if (g_chat_hub.origins[i].first == pid) {
            g_chat_hub.origins[i].second = sender;
            g_chat_hub.origin_seen_ms[i] = now;
            return;
        }
    }
    g_chat_hub.origins.emplace_back(pid, sender);
    g_chat_hub.origin_seen_ms.push_back(now);
}

std::string chat_sender_for_pid(std::uint64_t pid)
{
    if (pid == 0)
        return {};
    std::lock_guard lock(g_chat_hub.mu);
    for (const auto& row : g_chat_hub.origins) {
        if (row.first == pid)
            return row.second;
    }
    return {};
}

#if defined(_WIN32)
std::uint64_t parent_pid_of(std::uint64_t pid)
{
    if (pid == 0)
        return 0;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    std::uint64_t parent = 0;
    if (::Process32FirstW(snap, &pe)) {
        do {
            if (static_cast<std::uint64_t>(pe.th32ProcessID) == pid) {
                parent = pe.th32ParentProcessID;
                break;
            }
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);
    return parent;
}
#endif

std::string chat_sender_for_process(std::uint64_t pid)
{
    for (int hop = 0; hop < 8 && pid > 4; ++hop) {
        const std::string sender = chat_sender_for_pid(pid);
        if (!sender.empty())
            return sender;
#if defined(_WIN32)
        pid = parent_pid_of(pid);
#else
        break;
#endif
    }
    return {};
}

bool chat_event_visible(const nlohmann::json& ev, const std::string& subscriber)
{
    const std::string sender = ev.value("sender", std::string{});
    return sender.empty() || subscriber.empty() || sender == subscriber;
}

void chat_emit(const std::string& method, nlohmann::json args, const std::string& sender = {})
{
    const std::int64_t now = chat_now_ms();
    {
        std::lock_guard lock(g_chat_hub.mu);
        prune_chat_hub_locked(now);
        const std::uint64_t id = g_chat_hub.next_id++;
        nlohmann::json ev{{"id", id}, {"ts", now}, {"method", method}, {"args", std::move(args)}};
        if (!sender.empty())
            ev["sender"] = sender;
        g_chat_hub.events.push_back(std::move(ev));
    }
    g_chat_hub.cv.notify_all();
}

void install_chat_event_sources()
{
    static std::once_flag once;
    std::call_once(once, [] {
        media::llm::set_chat_host_emit([](const std::string& method, nlohmann::json args) {
            chat_emit(method, std::move(args));
        });
        media::cmd_run::Runner::instance().subscribe([](const media::cmd_run::RunEvent& ev) {
            const char* phase = "started";
            switch (ev.phase) {
            case media::cmd_run::RunPhase::Started: phase = "started"; break;
            case media::cmd_run::RunPhase::Exited: phase = "exited"; break;
            case media::cmd_run::RunPhase::Failed: phase = "failed"; break;
            case media::cmd_run::RunPhase::Cancelled: phase = "cancelled"; break;
            }
            std::string sender = tls_chat_sender;
            if (ev.phase == media::cmd_run::RunPhase::Started)
                remember_chat_origin(ev.pid, sender);
            else if (sender.empty())
                sender = chat_sender_for_pid(ev.pid);
            chat_emit("commandRunEvent", nlohmann::json::array({nlohmann::json{
                {"phase", phase},
                {"runId", ev.runId},
                {"cmdId", ev.cmdId},
                {"commandId", ev.commandId},
                {"pid", ev.pid},
                {"exitCode", ev.exitCode},
                {"error", ev.error},
                {"running", ev.phase == media::cmd_run::RunPhase::Started},
            }}), sender);
        });
    });
}

void rt_emit_for(std::uint64_t epoch, const char* method, nlohmann::json args)
{
    {
        std::lock_guard lock(g_rt.mu);
        if (epoch != g_rt.epoch)
            return;
        const char* level_method =
            pm::audio::session::audio_method_name(pm::audio::session::AudioMethod::Level);
        if (method && std::strcmp(method, level_method) == 0) {
            double level = 0;
            if (args.is_array() && !args.empty() && args[0].is_number())
                level = args[0].get<double>();
            g_rt.level = std::clamp(level, 0.0, 1.0);
            ++g_rt.level_gen;
            g_rt.cv.notify_all();
            return;
        }
        if (g_rt.events.size() >= 4000) {
            g_rt.events.erase(g_rt.events.begin(), g_rt.events.begin() + 1000);
            g_rt.base += 1000;
        }
        g_rt.events.push_back({{"method", method}, {"args", std::move(args)}});
    }
    g_rt.cv.notify_all();
}

void rt_fail(std::uint64_t epoch, const std::string& err)
{
    rt_emit_for(epoch, pm::audio::session::audio_method_name(pm::audio::session::AudioMethod::Error),
                nlohmann::json::array({err.empty() ? "realtime start failed" : err}));
    rt_emit_for(epoch, pm::audio::session::audio_method_name(pm::audio::session::AudioMethod::Done),
                nlohmann::json::array());
    {
        std::lock_guard lock(g_rt.mu);
        if (epoch == g_rt.epoch)
            g_rt.closed = true;
    }
    g_rt.cv.notify_all();
}

#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
void release_rt_consent_emit(RtLive* live)
{
    if (!live)
        return;
    const std::uint64_t ticket = live->consent_ticket.exchange(0);
    if (ticket)
        pm::security::restore_stream_consent_emit(ticket);
    const std::uint64_t ask = live->ask_ticket.exchange(0);
    if (ask)
        pm::llm::inquire::restore_stream_ask_emit(ask);
}
#endif

std::string rt_text(const char* s)
{
    return s ? s : "";
}

PmChatRtCallbacks rt_callbacks(RtLive* live)
{
    using pm::audio::session::AudioMethod;
    using pm::audio::session::audio_method_name;
    PmChatRtCallbacks cb{};
    cb.userdata = live;
    cb.on_ready = [](void* ud) {
        auto* live = static_cast<RtLive*>(ud);
        if (live)
            rt_emit_for(live->epoch, audio_method_name(AudioMethod::Ready), nlohmann::json::array());
    };
    cb.on_level = [](double level, void* ud) {
        auto* live = static_cast<RtLive*>(ud);
        if (live)
            rt_emit_for(live->epoch, audio_method_name(AudioMethod::Level),
                        nlohmann::json::array({std::clamp(level, 0.0, 1.0)}));
    };
    cb.on_text = [](const char* text, void* ud) {
        auto* live = static_cast<RtLive*>(ud);
        if (live)
            rt_emit_for(live->epoch, audio_method_name(AudioMethod::TextDelta), nlohmann::json::array({rt_text(text)}));
    };
    cb.on_transcript = [](const char* text, void* ud) {
        auto* live = static_cast<RtLive*>(ud);
        if (live)
            rt_emit_for(live->epoch, audio_method_name(AudioMethod::TranscriptDelta), nlohmann::json::array({rt_text(text)}));
    };
    cb.on_user_transcript = [](const char* text, int is_final, void* ud) {
        auto* live = static_cast<RtLive*>(ud);
        if (!live)
            return;
        if (is_final) {
            const char* spoken = text ? text : "";
            logger::info(std::string("realtime user transcript chars=")
                         + std::to_string(std::char_traits<char>::length(spoken)));
        }
        rt_emit_for(live->epoch, audio_method_name(AudioMethod::UserTranscript),
                    nlohmann::json::array({rt_text(text), is_final != 0}));
    };
    cb.on_response_done = [](void* ud) {
        auto* live = static_cast<RtLive*>(ud);
        if (live)
            rt_emit_for(live->epoch, audio_method_name(AudioMethod::ResponseDone), nlohmann::json::array());
    };
    cb.on_error = [](const char* text, void* ud) {
        auto* live = static_cast<RtLive*>(ud);
        if (live)
            rt_emit_for(live->epoch, audio_method_name(AudioMethod::Error), nlohmann::json::array({rt_text(text)}));
    };
    cb.on_auth_error = [](const char* text, void* ud) {
        auto* live = static_cast<RtLive*>(ud);
        if (live)
            rt_emit_for(live->epoch, audio_method_name(AudioMethod::AuthError), nlohmann::json::array({rt_text(text)}));
    };
    cb.on_network_error = [](const char* text, void* ud) {
        auto* live = static_cast<RtLive*>(ud);
        if (live)
            rt_emit_for(live->epoch, audio_method_name(AudioMethod::NetworkError), nlohmann::json::array({rt_text(text)}));
    };
    // Spoken run_pm_agent tools never ride the chat-completion SSE. Without
    // this the side panel only hears the voice summary.
    cb.on_agent_event = [](const char* kind, const char* /*text*/, const char* tool_name, void* ud) {
        auto* live = static_cast<RtLive*>(ud);
        if (!live)
            return;
        const std::string k = rt_text(kind);
        if (k != "tool_call" && k != "tool_result")
            return;
        std::string name = rt_text(tool_name);
        if (name.empty())
            name = "tool";
        std::string id;
        const bool starting = k == "tool_call";
        if (starting)
            id = std::to_string(live->tool_n.fetch_add(1) + 1);
        else {
            const int n = live->tool_n.load();
            id = std::to_string(n > 0 ? n : 1);
        }
        nlohmann::json row{{"role", "tool"}, {"text", std::move(name)}, {"toolCallId", "rt-" + id}};
        if (starting)
            row["toolRunning"] = true;
        rt_emit_for(live->epoch, audio_method_name(AudioMethod::AppendText), nlohmann::json::array({std::move(row)}));
    };
    cb.on_closed = [](void* ud) {
        auto* live = static_cast<RtLive*>(ud);
        if (!live)
            return;
        rt_emit_for(live->epoch, audio_method_name(AudioMethod::Done), nlohmann::json::array());
        {
            std::lock_guard lock(g_rt.mu);
            if (live->epoch == g_rt.epoch)
                g_rt.closed = true;
        }
        g_rt.cv.notify_all();
    };
    cb.on_agent_started = [](const char* call_id, const char* goal, void* ud) {
        auto* live = static_cast<RtLive*>(ud);
        if (!live)
            return;
        const std::string name = rt_text(goal);
        nlohmann::json agent{{"id", rt_text(call_id)}, {"name", name}, {"display_name", name}};
        rt_emit_for(live->epoch, audio_method_name(AudioMethod::AgentStarted), nlohmann::json::array({std::move(agent)}));
    };
    cb.on_agent_result = [](const char* call_id, const char* output_json, void* ud) {
        auto* live = static_cast<RtLive*>(ud);
        if (!live)
            return;
        bool ok = false;
        std::string err;
        const auto parsed = media::json::try_parse(output_json ? output_json : "{}");
        if (parsed.is_object()) {
            const std::string status = parsed.value("status", std::string{});
            ok = status == "completed";
            if (!ok)
                err = parsed.value("error", parsed.value("note", status));
        }
        rt_emit_for(live->epoch, audio_method_name(AudioMethod::AgentDone), nlohmann::json::array({rt_text(call_id), ok, err}));
    };
    return cb;
}

#if defined(_WIN32)
std::wstring wide_from_utf8(const std::string& text)
{
    if (text.empty())
        return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

std::string utf8_from_wide(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

bool filename_has_query(std::wstring name, std::wstring query)
{
    auto lower = [](std::wstring s) {
        for (wchar_t& c : s)
            c = static_cast<wchar_t>(::towlower(c));
        return s;
    };
    name = lower(std::move(name));
    query = lower(std::move(query));
    return !query.empty() && name.find(query) != std::wstring::npos;
}

bool skip_search_dir(const std::wstring& name)
{
    return name == L".git" || name == L"node_modules" || name == L"dist"
        || name == L".vs" || name == L"target" || name == L"__pycache__";
}

std::string scope_id_for_path(const std::string& path, const nlohmann::json& scopes)
{
    std::string best_id;
    std::size_t best_len = 0;
    if (!scopes.is_array())
        return best_id;
    std::string norm = path;
    for (char& c : norm)
        if (c == '/')
            c = '\\';
    norm = media::strutil::lower_ascii(std::move(norm));
    for (const auto& scope : scopes) {
        if (!scope.is_object())
            continue;
        std::string root = scope.value("path", std::string{});
        if (root.empty())
            continue;
        for (char& c : root)
            if (c == '/')
                c = '\\';
        while (root.size() > 3 && (root.back() == '\\' || root.back() == '/'))
            root.pop_back();
        root = media::strutil::lower_ascii(std::move(root));
        const bool hit = norm == root
            || (norm.size() > root.size() && norm.compare(0, root.size(), root) == 0 && norm[root.size()] == '\\');
        if (hit && root.size() >= best_len) {
            best_len = root.size();
            best_id = scope.value("id", std::string{});
        }
    }
    return best_id;
}

nlohmann::json search_bookmark_names(const nlohmann::json& req)
{
    std::string query = req.value("query", std::string{});
    int limit = req.value("headLimit", 40);
    if (limit < 1)
        limit = 1;
    if (limit > 60)
        limit = 60;
    const nlohmann::json scopes = req.contains("scopes") && req["scopes"].is_array()
        ? req["scopes"]
        : nlohmann::json::array();
    std::vector<std::wstring> roots;
    auto add_root = [&](const std::string& raw) {
        const std::wstring wide = wide_from_utf8(raw);
        if (wide.empty())
            return;
        std::error_code ec;
        if (!fs::is_directory(wide, ec))
            return;
        roots.push_back(wide);
    };
    if (req.contains("inputs") && req["inputs"].is_array()) {
        for (const auto& row : req["inputs"])
            if (row.is_string())
                add_root(row.get<std::string>());
    }
    if (roots.empty()) {
        std::vector<media::settings::BookmarkItem> bookmarks;
        std::string err;
        if (media::settings::load_bookmarks(bookmarks, err)) {
            for (const auto& item : bookmarks) {
                if (item.searchable && item.kind == "node" && item.node_kind == "folder")
                    add_root(item.path);
            }
        }
    }
    const std::wstring needle = wide_from_utf8(query);
    nlohmann::json hits = nlohmann::json::array();
    int scanned = 0;
    const int scan_cap = 12000;
    const std::function<void(const fs::path&, int)> walk = [&](const fs::path& dir, int depth) {
        if (depth > 12 || static_cast<int>(hits.size()) >= limit || scanned >= scan_cap)
            return;
        std::error_code ec;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
             it != end && static_cast<int>(hits.size()) < limit && scanned < scan_cap;
             it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            ++scanned;
            std::error_code fec;
            const fs::path child = it->path();
            if (it->is_symlink(fec))
                continue;
            if (it->is_directory(fec)) {
                if (!skip_search_dir(child.filename().wstring()))
                    walk(child, depth + 1);
                continue;
            }
            if (!it->is_regular_file(fec))
                continue;
            const std::wstring name = child.filename().wstring();
            if (!filename_has_query(name, needle))
                continue;
            const std::string full = utf8_from_wide(child.wstring());
            hits.push_back({
                {"path", full},
                {"title", utf8_from_wide(name)},
                {"scope_id", scope_id_for_path(full, scopes)},
            });
        }
    };
    for (const std::wstring& root : roots) {
        if (static_cast<int>(hits.size()) >= limit || scanned >= scan_cap)
            break;
        walk(std::filesystem::path(root), 0);
    }
    const auto matches = hits.size();
    return {
        {"accepted", true},
        {"taskId", "chrome-names"},
        {"hits", std::move(hits)},
        {"matches", matches},
    };
}

std::wstring tanit_gui_exe()
{
    wchar_t self[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring path(self, n);
        const auto slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            const std::wstring sibling = path.substr(0, slash) + L"\\tanit.exe";
            if (::GetFileAttributesW(sibling.c_str()) != INVALID_FILE_ATTRIBUTES)
                return sibling;
        }
    }
    wchar_t local[MAX_PATH]{};
    if (SUCCEEDED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local))) {
        const std::wstring alias = std::wstring(local) + L"\\Microsoft\\WindowsApps\\tanit.exe";
        if (::GetFileAttributesW(alias.c_str()) != INVALID_FILE_ATTRIBUTES)
            return alias;
    }
    return {};
}

bool settings_section_ok(const std::string& section)
{
    if (section.empty() || section.size() > 200)
        return false;
    for (unsigned char c : section) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '_' || c == '-' || c == '?' || c == '=' || c == '&' || c == '%' || c == '.';
        if (!ok)
            return false;
    }
    return true;
}

#if defined(FEATURE_COMMAND_IPC_NEXT) && FEATURE_COMMAND_IPC_NEXT
std::optional<media::cmd_ipc::InstanceDescriptor> newest_ui_instance()
{
    std::optional<media::cmd_ipc::InstanceDescriptor> best;
    for (const auto& inst : media::cmd_ipc::list_instances()) {
        if (inst.kind != media::cmd_ipc::ProcessKind::Ui)
            continue;
        if (!best || inst.started_unix_ms > best->started_unix_ms)
            best = inst;
    }
    return best;
}

nlohmann::json spawn_tanit_ui(const std::wstring& args)
{
    const std::wstring exe = tanit_gui_exe();
    if (exe.empty())
        return {{"ok", false}, {"error", "tanit.exe was not found"}};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(exe.c_str(), mutable_cmd.data(), nullptr, nullptr, FALSE,
                          CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) {
        return {{"ok", false}, {"error", "tanit.exe did not start"}};
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (!newest_ui_instance() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (!newest_ui_instance())
        return {{"ok", false}, {"error", "tanit ui did not register"}};
    return {{"ok", true}};
}

nlohmann::json send_ui_app_cmd(const std::string& command, const nlohmann::json& args)
{
    const auto inst = newest_ui_instance();
    if (!inst)
        return {{"ok", false}, {"error", "tanit ui did not register"}};
    nlohmann::json reply;
    std::string err;
    const bool sent = media::cmd_ipc::send_action(*inst, "app_cmd", {
        {"command", command},
        {"args", args},
    }, reply, err, 8000);
    if (!sent || reply.value("type", std::string{}) != "app_cmd_ack") {
        return {{"ok", false}, {"error", err.empty() ? command + " was not accepted" : err}};
    }
    return {{"ok", true}};
}
#endif

// Same sequence as scripts/screenshots.mjs settings: start `tanit --ui-preset main`,
// then `run-ipc app-cmd settings|<section>` (section may be `chat?preset=<id>`).
nlohmann::json open_settings_ui(const std::string& section)
{
#if !defined(FEATURE_COMMAND_IPC_NEXT) || !FEATURE_COMMAND_IPC_NEXT
    return {{"ok", false}, {"error", "ui ipc is not available"}};
#else
    const std::string route = settings_section_ok(section) ? section : std::string("chat");
    if (!newest_ui_instance()) {
        const nlohmann::json spawned = spawn_tanit_ui(L"--ui-preset=main");
        if (!spawned.value("ok", false))
            return spawned;
    }
    nlohmann::json sent = send_ui_app_cmd("settings", {{"path", route}});
    if (sent.value("ok", false))
        sent["section"] = route;
    return sent;
#endif
}

bool ui_file_path_ok(const std::string& path)
{
    if (path.empty() || path.size() > 1024)
        return false;
    for (unsigned char c : path) {
        if (c < 0x20 || c == '"' || c == ';')
            return false;
    }
    return true;
}

// Centre file viewer: `tanit --ui-preset=viewer` when no window is up, then `open|<path>`.
nlohmann::json open_file_ui(const std::string& path, const nlohmann::json& paths, bool fullscreen)
{
#if !defined(FEATURE_COMMAND_IPC_NEXT) || !FEATURE_COMMAND_IPC_NEXT
    (void)path;
    (void)paths;
    (void)fullscreen;
    return {{"ok", false}, {"error", "ui ipc is not available"}};
#else
    if (!ui_file_path_ok(path))
        return {{"ok", false}, {"error", "path is not usable"}};
    std::string joined = path;
    if (paths.is_array()) {
        for (const auto& item : paths) {
            if (!item.is_string())
                continue;
            const std::string one = item.get<std::string>();
            if (!ui_file_path_ok(one) || one == path)
                continue;
            if (joined.size() + 1 + one.size() > 32000)
                break;
            joined += ";";
            joined += one;
        }
    }
    if (!newest_ui_instance()) {
        std::wstring src = wide_from_utf8(path);
        for (wchar_t& c : src) {
            if (c == L'/')
                c = L'\\';
        }
        const nlohmann::json spawned = spawn_tanit_ui(L"--ui-preset=viewer --src \"" + src + L"\"");
        if (!spawned.value("ok", false))
            return spawned;
    }
    nlohmann::json sent = send_ui_app_cmd("open", {{"path", joined}});
    if (!sent.value("ok", false))
        return sent;
    if (fullscreen) {
        const nlohmann::json full = send_ui_app_cmd("startfullscreen", nlohmann::json::object());
        if (!full.value("ok", false))
            return full;
    }
    return {{"ok", true}, {"path", path}};
#endif
}
#endif

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO && defined(FEATURE_VIDEO_STREAM) && FEATURE_VIDEO_STREAM
bool fill_video_update(const nlohmann::json& body, VideoStreamCaptureUpdate& upd, std::string& err)
{
    auto fail = [&](std::string msg) {
        err = std::move(msg);
        return false;
    };
    if (body.contains("input")) {
        if (!body["input"].is_string())
            return fail("input must be a string");
        upd.input = body["input"].get<std::string>();
    }
    auto take_int = [&](const char* primary, const char* alias, std::optional<int>& slot) -> bool {
        const char* key = nullptr;
        const nlohmann::json* value = nullptr;
        if (body.contains(primary)) {
            key = primary;
            value = &body.at(primary);
        } else if (alias && body.contains(alias)) {
            key = alias;
            value = &body.at(alias);
        }
        if (!value)
            return true;
        if (!value->is_number())
            return fail(std::string(key) + " must be a number");
        const double n = value->get<double>();
        if (!std::isfinite(n) || n < -1000000.0 || n > 1000000.0)
            return fail(std::string(key) + " is out of range");
        slot = static_cast<int>(n);
        return true;
    };
    if (!take_int("fps", nullptr, upd.fps))
        return false;
    if (!take_int("mjpegQuality", "mjpeg_quality", upd.mjpeg_quality))
        return false;
    if (!take_int("bitrateKbps", "bitrate_kbps", upd.bitrate_kbps))
        return false;
    auto take_bool = [&](const char* primary, const char* alias, std::optional<bool>& slot) -> bool {
        const char* key = nullptr;
        const nlohmann::json* value = nullptr;
        if (body.contains(primary)) {
            key = primary;
            value = &body.at(primary);
        } else if (alias && body.contains(alias)) {
            key = alias;
            value = &body.at(alias);
        }
        if (!value)
            return true;
        if (!value->is_boolean())
            return fail(std::string(key) + " must be a boolean");
        slot = value->get<bool>();
        return true;
    };
    if (!take_bool("includeCursor", "include_cursor", upd.include_cursor))
        return false;
    if (body.contains("encoder")) {
        if (!body["encoder"].is_string())
            return fail("encoder must be a string");
        upd.encoder = body["encoder"].get<std::string>();
    }
    return true;
}

void write_video_json(httplib::Response& res, const nlohmann::json& body, int status = 200)
{
    res.status = status;
    res.set_content(body.dump(2) + "\n", "application/json; charset=utf-8");
}
#endif

void register_routes(httplib::Server& svr, const AgentServerOptions& opt, InflightGate& gate)
{
    install_chat_event_sources();
    auto require_v1_auth = [&opt](const httplib::Request& req, httplib::Response& res) -> bool {
        if (lan_hop_present(req) && !lan_hop_ok(req)) {
            res.status = 400;
            res.set_content(openai_error("invalid LAN hop").dump(), "application/json");
            return false;
        }
        const bool via_tls = lan_hop_ok(req);
        const bool control = req.path == "/v1/lan/pair/begin" || req.path == "/v1/lan/devices"
                             || req.path == "/v1/lan/devices/revoke";
        if (control) {
            if (!machine_local_request(req)) {
                res.status = 403;
                res.set_content(openai_error("LAN device admin is local-only").dump(),
                                "application/json");
                return false;
            }
            return true;
        }

        const auto secrets = request_secrets(req);
        if (!opt.serve_api_key.empty()) {
            for (const auto& secret : secrets) {
                if (lan::constant_time_equal(secret, opt.serve_api_key))
                    return true;
            }
        }
        if (g_lan) {
            for (const auto& secret : secrets) {
                lan::DeviceAuth dev;
                if (!g_lan->authorize(secret, dev))
                    continue;
                if (!device_scope_ok(dev, req)) {
                    res.status = 403;
                    res.set_content(
                        openai_error("this paired phone is not allowed to call that route",
                                     "invalid_request_error", "insufficient_scope")
                            .dump(),
                        "application/json");
                    return false;
                }
                return true;
            }
        }
        // Direct loopback with no serve key stays open for local tools. A phone on the
        // TLS proxy always needs a paired device token or the serve key.
        if (!via_tls && opt.serve_api_key.empty())
            return true;
        res.status = 401;
        res.set_content(
            openai_error("missing or invalid Authorization Bearer token", "invalid_request_error",
                         "unauthorized")
                .dump(),
            "application/json");
        return false;
    };

    register_llm_vfs_routes(
        svr,
        [&opt](const httplib::Request& req, httplib::Response& res, LlmVfsAccess& access) {
            access = {};
            if (lan_hop_present(req) && !lan_hop_ok(req)) {
                res.status = 400;
                res.set_content(openai_error("invalid LAN hop").dump(), "application/json");
                return false;
            }
            const bool via_tls = lan_hop_ok(req);
            const auto secrets = request_secrets(req);
            if (!opt.serve_api_key.empty()) {
                for (const auto& secret : secrets) {
                    if (lan::constant_time_equal(secret, opt.serve_api_key)) {
                        access.full = true;
                        return true;
                    }
                }
            }
            if (g_lan) {
                for (const auto& secret : secrets) {
                    lan::DeviceAuth dev;
                    if (!g_lan->authorize(secret, dev))
                        continue;
                    if (!dev.has("files")) {
                        res.status = 403;
                        res.set_content(
                            openai_error("this paired phone is not allowed to call that route",
                                         "invalid_request_error", "insufficient_scope")
                                .dump(),
                            "application/json");
                        return false;
                    }
                    access.device_id = dev.id;
                    access.files = true;
                    return true;
                }
            }
            if (!via_tls && opt.serve_api_key.empty()) {
                access.full = true;
                return true;
            }
            res.status = 401;
            res.set_content(
                openai_error("missing or invalid Authorization Bearer token", "invalid_request_error",
                             "unauthorized")
                    .dump(),
                "application/json");
            return false;
        });

    svr.Get("/health", [&gate, &opt](const httplib::Request&, httplib::Response& res) {
        const auto gs = gate.stats();
        nlohmann::json j{
            {"ok", true},
            {"ready", true},
            {"service", "tanit-llm-agent"},
            {"auth_required", !opt.serve_api_key.empty()},
            {"concurrency",
             {{"limit", gs.concurrency},
              {"inflight", gs.inflight},
              {"waiting", gs.waiting},
              {"max_waiting", gs.max_waiting},
              {"peak_inflight", gs.peak_inflight},
              {"peak_waiting", gs.peak_waiting},
              {"accepted", gs.accepted},
              {"waited", gs.waited},
              {"rejected", gs.rejected},
              // Excess turns block until a slot frees, up to max_waiting parkers.
              // Beyond that: HTTP 503 queue_full (keeps workers free for consent).
              {"policy", "queue-block"},
             }},
            {"preset", opt.preset},
        };
        if (g_lan) {
            const lan::PublicInfo info = g_lan->info();
            j["lan"] = {
                {"enabled", true},
                {"protocol", lan::k_protocol_version},
                {"name", info.name},
                {"fingerprint", info.fingerprint},
                {"tls_port", info.tls_port},
                {"pairing", info.pairing_open},
                {"mdns", info.mdns},
                {"addresses", info.addresses},
            };
        }
#if defined(_WIN32)
        // Cheap host WS peek without advancing CPU deltas (use /v1/sysprobe for full sample).
        try {
            const auto snap = pm::perf::last_sample();
            if (snap.tick_ms != 0) {
                j["hostWsMb"] = snap.host_ws_mb;
                j["hostCpu"]  = snap.host_cpu_pct;
                j["sysCpu"]   = snap.sys_cpu_pct;
                j["sysMem"]   = snap.sys_mem_pct;
            }
        } catch (...) {
        }
#endif
        res.status = 200;
        res.set_content(j.dump(), "application/json");
    });

    svr.Get("/ready", [&gate](const httplib::Request&, httplib::Response& res) {
        const auto gs = gate.stats();
        nlohmann::json j{
            {"ok", true},
            {"ready", true},
            {"inflight", gs.inflight},
            {"waiting", gs.waiting},
            {"limit", gs.concurrency},
        };
        res.status = 200;
        res.set_content(j.dump(), "application/json");
    });

    svr.Get("/v1/host-tool-providers",
            [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
                if (!require_v1_auth(req, res))
                    return;
                if (!machine_local_request(req)) {
                    res.status = 403;
                    res.set_content(
                        openai_error("host tool providers are loopback-only").dump(),
                        "application/json");
                    return;
                }
                res.status = 200;
                res.set_content(
                    media::llm::agent::list_registered_host_tool_providers().dump(),
                    "application/json");
            });

    svr.Post("/v1/host-tool-providers",
             [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
                 if (!require_v1_auth(req, res))
                     return;
                 if (!machine_local_request(req)) {
                     res.status = 403;
                     res.set_content(
                         openai_error("host tool providers are loopback-only").dump(),
                         "application/json");
                     return;
                 }
                 nlohmann::json body;
                 try {
                     body = nlohmann::json::parse(req.body);
                 } catch (...) {
                     res.status = 400;
                     res.set_content(openai_error("invalid JSON body").dump(),
                                     "application/json");
                     return;
                 }
                 const std::string id  = body.value("id", std::string{});
                 const std::string url = body.value("url", std::string{});
                 const std::string token = body.value("token", std::string{});
                 std::string       clean = url;
                 const auto        qmark = clean.find('?');
                 if (qmark != std::string::npos && clean.find("token=") != std::string::npos)
                     clean.resize(qmark);
                 std::string registered = url;
                 if (!token.empty() && registered.find("token=") == std::string::npos) {
                     registered += registered.find('?') == std::string::npos ? "?" : "&";
                     registered += "token=";
                     registered += token;
                 }
                 std::string       err;
                 if (id.empty() || url.empty()
                     || !try_register_host_provider(id, registered, err)) {
                     res.status = 400;
                     res.set_content(
                         openai_error(err.empty()
                                          ? "need {id, url} with loopback http URL"
                                          : err)
                             .dump(),
                         "application/json");
                     return;
                 }
                 res.status = 200;
                 res.set_content(
                     nlohmann::json{{"ok", true}, {"id", id}, {"url", clean}}.dump(),
                     "application/json");
             });

    // Pure BM25 over a page model the caller already holds. Loopback only:
    // the body is live page text. Does not open a tab.
    svr.Post("/v1/browser/page-model/rank",
             [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
                 auto fail = [&res](const std::string& error) {
                     res.status = 200;
                     res.set_content(nlohmann::json{{"ok", false}, {"error", error}}.dump(),
                                     "application/json");
                 };
                 if (!require_v1_auth(req, res))
                     return;
                 if (!machine_local_request(req)) {
                     res.status = 403;
                     res.set_content(
                         openai_error("page model rank is loopback-only").dump(),
                         "application/json");
                     return;
                 }
                 nlohmann::json body;
                 try {
                     body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
                 } catch (...) {
                     fail("invalid JSON body");
                     return;
                 }
                 if (!body.is_object()) {
                     fail("JSON body must be an object");
                     return;
                 }
                 std::string query;
                 if (body.contains("query") && body["query"].is_string())
                     query = body["query"].get<std::string>();
                 if (query.empty()) {
                     fail("query is required");
                     return;
                 }
                 nlohmann::json model;
                 if (body.contains("model") && body["model"].is_object())
                     model = body["model"];
                 else if (body.contains("data") && body["data"].is_object()
                          && body["data"].contains("model") && body["data"]["model"].is_object())
                     model = body["data"]["model"];
                 else {
                     fail("model object is required");
                     return;
                 }
                 std::size_t top_k = 5;
                 if (body.contains("topK") && body["topK"].is_number()) {
                     const double raw = body["topK"].get<double>();
                     if (std::isfinite(raw) && raw >= 1.0 && raw <= 16.0)
                         top_k = static_cast<std::size_t>(raw);
                 }
                 const std::size_t zone_count =
                     model.contains("zones") && model["zones"].is_array() ? model["zones"].size() : 0;
#if defined(FEATURE_BROWSER_PAGE_MODEL) && FEATURE_BROWSER_PAGE_MODEL
                 media::llm::browser_use::rank_page_model_zones(model, query, top_k);
                 nlohmann::json ranked = nlohmann::json::array();
                 if (model.contains("rankedZones") && model["rankedZones"].is_array())
                     ranked = model["rankedZones"];
                 res.status = 200;
                 res.set_content(
                     nlohmann::json{{"ok", true},
                                    {"data",
                                     {{"rankedZones", std::move(ranked)},
                                      {"zoneCount", zone_count},
                                      {"topK", top_k}}}}
                         .dump(),
                     "application/json");
#else
                 (void)top_k;
                 fail("page model ranking is not built");
                 (void)zone_count;
#endif
             });

#if defined(_WIN32)
    // Same counters as Statusbar / perfui (`pm::perf::sample`).
    // ?mode=last → last sample without advancing CPU deltas (post-workload WS/GPU).
    // Full sample also attaches peak host/sys CPU + WS over recent ring.
    svr.Get("/v1/sysprobe", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        try {
            const std::string mode = req.get_param_value("mode");
            const bool peek_last   = (mode == "last" || mode == "peek");
            const auto snap =
                peek_last ? pm::perf::last_sample() : pm::perf::sample();
            nlohmann::json j = nlohmann::json::parse(pm::perf::sample_to_json_object(snap));
            int      peak_host_cpu = snap.host_cpu_pct;
            int      peak_sys_cpu  = snap.sys_cpu_pct;
            unsigned peak_ws_mb    = snap.host_ws_mb;
            for (const auto& s : pm::perf::recent_samples(60)) {
                peak_host_cpu = (std::max)(peak_host_cpu, s.host_cpu_pct);
                peak_sys_cpu  = (std::max)(peak_sys_cpu, s.sys_cpu_pct);
                peak_ws_mb    = (std::max)(peak_ws_mb, s.host_ws_mb);
            }
            j["hostCpuPeak"] = peak_host_cpu;
            j["sysCpuPeak"]  = peak_sys_cpu;
            j["hostWsMbPeak"] = peak_ws_mb;
            j["sampleMode"]  = peek_last ? "last" : "interval";
            res.status = 200;
            res.set_content(j.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(openai_error(std::string("sysprobe: ") + e.what()).dump(),
                            "application/json");
        }
    });
#else
    svr.Get("/v1/sysprobe", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        res.status = 501;
        res.set_content(openai_error("sysprobe unavailable on this platform", "server_error",
                                     "sysprobe_unsupported")
                            .dump(),
                        "application/json");
    });
#endif

    svr.Get("/v1/models", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        // Discovery stays open on the direct HTTP port. Phones on the TLS proxy authenticate.
        if (lan_hop_present(req) && !require_v1_auth(req, res))
            return;
        res.status = 200;
        res.set_content(models_list_json().dump(), "application/json");
    });

    svr.Get("/v1/credit", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        // Prefer per-request Zitadel bearer (pm-pics), else process `tanit-cli login`.
        std::string token;
        std::string token_err;
        if (req.has_header("X-Tanit-Provider-Key"))
            token = trim_ascii(req.get_header_value("X-Tanit-Provider-Key"));
        if (token.empty()
            && (!pm_zitadel_oauth_read_access_token(token, token_err) || token.empty())) {
            res.status = 200;
            res.set_content(nlohmann::json{{"ok", false},
                                           {"error", token_err.empty() ? "not signed in" : token_err}}
                                .dump(),
                            "application/json");
            return;
        }
        std::string api_key;
        std::string base_url;
        media::runtime_settings::merge_provider_credentials("tanit", false, api_key, base_url);
        if (req.has_header("X-Tanit-Provider-Base-Url")) {
            const std::string h = trim_ascii(req.get_header_value("X-Tanit-Provider-Base-Url"));
            if (!h.empty())
                base_url = h;
        }
        media::pixlwiz_cli::PixlWizBudgetInfo info;
        std::string err;
        if (!media::pixlwiz_cli::pixlwiz_get_budget_info(token, base_url, info, err)) {
            res.status = 200;
            res.set_content(nlohmann::json{{"ok", false},
                                           {"error", err.empty() ? "budget lookup failed" : err}}
                                .dump(),
                            "application/json");
            return;
        }
        nlohmann::json data{{"spend", info.spend}};
        if (info.max_budget >= 0.0)
            data["max_budget"] = info.max_budget;
        else
            data["max_budget"] = nullptr;
        res.status = 200;
        res.set_content(nlohmann::json{{"ok", true}, {"data", std::move(data)}}.dump(),
                        "application/json");
    });

    svr.Get("/v1/chat/commands", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        res.status = 200;
        res.set_content(nlohmann::json{{"ok", true}, {"data", media::launcher::chat_commands_catalog_json()}}.dump(),
                        "application/json");
    });

    svr.Post("/v1/chat/commands/run", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        nlohmann::json body = nlohmann::json::object();
        try {
            if (!req.body.empty())
                body = nlohmann::json::parse(req.body);
        } catch (...) {
            res.status = 200;
            res.set_content(nlohmann::json{{"ok", false}, {"error", "invalid JSON body"}}.dump(),
                            "application/json");
            return;
        }
        const std::string id = body.value("id", std::string{});
        tls_chat_sender = body.value("sender", std::string{});
        const nlohmann::json started = media::launcher::run_chat_command(id);
        tls_chat_sender.clear();
        res.status = 200;
        res.set_content(started.dump(), "application/json");
    });

    svr.Get("/v1/chat/events", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_chunked_content_provider(
            "text/event-stream; charset=utf-8",
            [sender = req.has_param("sender") ? req.get_param_value("sender") : std::string{},
             after = req.has_param("after") ? std::strtoull(req.get_param_value("after").c_str(), nullptr, 10) : 0ULL](
                size_t /*offset*/, httplib::DataSink& sink) -> bool {
                std::uint64_t pos = 0;
                std::vector<nlohmann::json> backlog;
                {
                    std::lock_guard lock(g_chat_hub.mu);
                    prune_chat_hub_locked(chat_now_ms());
                    for (const auto& ev : g_chat_hub.events) {
                        if (ev.value("id", std::uint64_t{0}) <= after)
                            continue;
                        if (!chat_event_visible(ev, sender))
                            continue;
                        backlog.push_back(ev);
                    }
                    pos = g_chat_hub.base + g_chat_hub.events.size();
                }
                for (const auto& ev : backlog) {
                    const std::string line = "data: " + ev.dump() + "\n\n";
                    if (!sink.write(line.data(), line.size()))
                        return false;
                }
                for (;;) {
                    std::vector<nlohmann::json> batch;
                    {
                        std::unique_lock lock(g_chat_hub.mu);
                        g_chat_hub.cv.wait_for(lock, std::chrono::seconds(15), [&] {
                            return (g_chat_hub.base + g_chat_hub.events.size()) > pos;
                        });
                        prune_chat_hub_locked(chat_now_ms());
                        if (pos < g_chat_hub.base)
                            pos = g_chat_hub.base;
                        const std::uint64_t end = g_chat_hub.base + g_chat_hub.events.size();
                        while (pos < end) {
                            const auto& ev = g_chat_hub.events[static_cast<size_t>(pos - g_chat_hub.base)];
                            if (chat_event_visible(ev, sender))
                                batch.push_back(ev);
                            ++pos;
                        }
                    }
                    for (const auto& ev : batch) {
                        const std::string line = "data: " + ev.dump() + "\n\n";
                        if (!sink.write(line.data(), line.size()))
                            return false;
                    }
                    if (batch.empty()) {
                        static const char ping[] = ": keepalive\n\n";
                        if (!sink.write(ping, sizeof(ping) - 1u))
                            return false;
                    }
                }
            });
    });

    svr.Post("/v1/chat/consent", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        nlohmann::json body = nlohmann::json::object();
        if (!req.body.empty()) {
            body = media::json::try_parse(req.body);
            if (!body.is_object()) {
                res.status = 200;
                res.set_content(nlohmann::json{{"ok", false}, {"error", "invalid JSON body"}}.dump(),
                                "application/json");
                return;
            }
        }
        if (body.value("cancel", false))
            pm::security::cancel_stream_consent();
        else
            pm::security::resolve_stream_consent(req.body);
        res.status = 200;
        res.set_content(nlohmann::json{{"ok", true}}.dump(), "application/json");
    });

    // Unblocks ask_user when `--consent-ui stream` (SSE `tanit.ask`).
    svr.Post("/v1/chat/ask", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        nlohmann::json body = nlohmann::json::object();
        if (!req.body.empty()) {
            body = media::json::try_parse(req.body);
            if (!body.is_object()) {
                res.status = 200;
                res.set_content(nlohmann::json{{"ok", false}, {"error", "invalid JSON body"}}.dump(),
                                "application/json");
                return;
            }
        }
        if (body.value("cancel", false))
            pm::llm::inquire::cancel_stream_ask();
        else
            pm::llm::inquire::resolve_stream_ask(req.body);
        res.status = 200;
        res.set_content(nlohmann::json{{"ok", true}}.dump(), "application/json");
    });

    svr.Get("/v1/chat/skills", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        const std::string cwd = req.has_param("cwd") ? req.get_param_value("cwd") : std::string{};
        try {
            res.status = 200;
            res.set_content(nlohmann::json{
                {"ok", true},
                {"data", media::llm::skills::list_skills_data_json(cwd)},
            }.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 200;
            res.set_content(nlohmann::json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
        }
    });

    svr.Get("/v1/chat/recent-paths", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        nlohmann::json files = nlohmann::json::array();
        nlohmann::json folders = nlohmann::json::array();
#if defined(_WIN32)
        media::layout::WindowLayout layout;
        std::string err;
        if (!media::settings::load_window_layout(layout, err) && !err.empty()) {
            res.status = 200;
            res.set_content(nlohmann::json{{"ok", false}, {"error", err}}.dump(), "application/json");
            return;
        }
        for (const auto& path : layout.recent_files) {
            if (!path.empty())
                files.push_back(path);
        }
        for (const auto& path : layout.recent_folders) {
            if (!path.empty())
                folders.push_back(path);
        }
#endif
        res.status = 200;
        res.set_content(nlohmann::json{{"ok", true}, {"data", {{"files", std::move(files)}, {"folders", std::move(folders)}}}}.dump(),
                        "application/json");
    });

#if defined(_WIN32)
    svr.Get("/v1/chat/typeahead-presets", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        std::vector<media::settings::BookmarkItem> bookmarks;
        std::string err;
        if (!media::settings::load_bookmarks(bookmarks, err)) {
            res.status = 200;
            res.set_content(nlohmann::json{{"ok", false}, {"error", err.empty() ? "bookmarks unavailable" : err}}.dump(),
                            "application/json");
            return;
        }
        const nlohmann::json presets = media::chat_web::typeahead::build_typeahead_presets_json(bookmarks, {});
        res.status = 200;
        res.set_content(nlohmann::json{{"ok", true}, {"data", {{"presets", presets}}}}.dump(), "application/json");
    });

    svr.Post("/v1/chat/search-names", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        const nlohmann::json body = media::json::parse_or(req.body, nlohmann::json::object());
        res.status = 200;
        res.set_content(nlohmann::json{{"ok", true}, {"data", search_bookmark_names(body)}}.dump(), "application/json");
    });
#endif

    svr.Get("/v1/chat/xblox-artifacts", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        nlohmann::json doc;
        std::string err;
        if (!media::runtime_settings::load_xblox_artifacts(doc, err)) {
            res.status = 200;
            res.set_content(nlohmann::json{{"ok", false},
                                           {"error", err.empty() ? "xblox artifacts unavailable" : err}}
                                .dump(),
                            "application/json");
            return;
        }
        nlohmann::json items = (doc.contains("items") && doc["items"].is_array())
            ? doc["items"]
            : nlohmann::json::array();
        int limit = 0;
        if (req.has_param("limit")) {
            try {
                limit = std::stoi(req.get_param_value("limit"));
            } catch (...) {
                limit = 0;
            }
        }
        if (limit > 0 && static_cast<std::size_t>(limit) < items.size()) {
            nlohmann::json trimmed = nlohmann::json::array();
            for (std::size_t i = 0; i < static_cast<std::size_t>(limit); ++i)
                trimmed.push_back(items[i]);
            items = std::move(trimmed);
        }
        for (auto& item : items) {
            if (!item.is_object())
                continue;
            const std::string path = item.value("path", std::string{});
            const auto shared = shared_file_for_path(path);
            if (shared && !shared->rel.empty())
                item["uri"] = shared->uri;
        }
        res.status = 200;
        res.set_content(nlohmann::json{{"ok", true},
                                       {"data", {{"version", doc.value("version", 1)}, {"items", std::move(items)}}}}
                            .dump(),
                        "application/json");
    });

    svr.Get("/v1/chat/sessions", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        std::string err;
        nlohmann::json sessions = nlohmann::json::array();
        for (auto& row : media::llm::chat_sessions::list_meta(err))
            sessions.push_back(std::move(row));
        res.status = 200;
        if (!err.empty())
            res.set_content(nlohmann::json{{"ok", false}, {"error", err}}.dump(), "application/json");
        else
            res.set_content(nlohmann::json{{"ok", true}, {"data", {{"sessions", std::move(sessions)}}}}.dump(),
                            "application/json");
    });

    svr.Post("/v1/chat/sessions", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        nlohmann::json session;
        try {
            session = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(nlohmann::json{{"ok", false}, {"error", e.what()}}.dump(), "application/json");
            return;
        }
        std::string err;
        const bool ok = session.is_object() && !session.value("id", std::string{}).empty()
                        && media::llm::chat_sessions::save(session, err);
        res.status = 200;
        if (!ok)
            res.set_content(nlohmann::json{{"ok", false},
                                           {"error", err.empty() ? "saveChatSession" : err}}
                                .dump(),
                            "application/json");
        else
            res.set_content(nlohmann::json{{"ok", true}}.dump(), "application/json");
    });

    svr.Get("/v1/chat/session", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        const std::string id = req.get_param_value("id");
        std::string err;
        nlohmann::json session;
        const bool ok = !id.empty() && media::llm::chat_sessions::load(id, session, err);
        res.status = 200;
        if (!ok)
            res.set_content(nlohmann::json{{"ok", false},
                                           {"error", err.empty() ? "loadChatSession" : err}}
                                .dump(),
                            "application/json");
        else
            res.set_content(nlohmann::json{{"ok", true}, {"data", {{"session", std::move(session)}}}}.dump(),
                            "application/json");
    });

    svr.Delete("/v1/chat/session", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        const std::string id = req.get_param_value("id");
        std::string err;
        if (id.empty()) {
            res.status = 200;
            res.set_content(nlohmann::json{{"ok", false}, {"error", "missing id"}}.dump(),
                            "application/json");
            return;
        }
        const auto removed = media::llm::chat_sessions::remove(id, err);
        res.status = 200;
        if (!err.empty())
            res.set_content(nlohmann::json{{"ok", false}, {"error", err}}.dump(), "application/json");
        else
            res.set_content(nlohmann::json{{"ok", true}, {"removed", removed.removed}}.dump(),
                            "application/json");
    });

    svr.Post("/v1/speak", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        nlohmann::json body = media::json::parse_object_or(req.body);
        const auto result = media::llm::path::execute("speak", {
            {"text", body.value("text", std::string{})},
        });
        nlohmann::json out = {{"ok", result.ok}};
        if (!result.ok)
            out["error"] = result.error.empty() ? std::string("speak failed") : result.error;
        res.status = 200;
        res.set_content(out.dump(), "application/json");
    });

    svr.Post("/v1/speak/stop", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        media::llm::path::abort_active_speak();
        res.status = 200;
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/v1/speak/pause", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        media::llm::path::pause_active_speak();
        res.status = 200;
        res.set_content("{\"ok\":true}", "application/json");
    });

    svr.Post("/v1/speak/resume", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        media::llm::path::resume_active_speak();
        res.status = 200;
        res.set_content("{\"ok\":true}", "application/json");
    });

#if defined(_WIN32)
    svr.Post("/v1/ui/open", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        std::string section = "chat";
        if (!req.body.empty()) {
            const auto body = media::json::try_parse(req.body);
            if (body.is_object())
                section = body.value("section", section);
        }
        const nlohmann::json out = open_settings_ui(section);
        res.status = 200;
        res.set_content(out.dump(), "application/json");
    });

    svr.Post("/v1/ui/open-file", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        std::string path;
        nlohmann::json paths = nlohmann::json::array();
        bool fullscreen = false;
        if (!req.body.empty()) {
            const auto body = media::json::try_parse(req.body);
            if (body.is_object()) {
                path = body.value("path", std::string{});
                if (body.contains("paths") && body["paths"].is_array())
                    paths = body["paths"];
                fullscreen = body.value("fullscreen", false);
            }
        }
        const nlohmann::json out = open_file_ui(path, paths, fullscreen);
        res.status = 200;
        res.set_content(out.dump(), "application/json");
    });
#endif

    svr.Post("/v1/realtime/start", [require_v1_auth, &opt](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        nlohmann::json body = media::json::parse_object_or(req.body);
        std::shared_ptr<RtLive> prev;
        std::shared_ptr<RtLive> live;
        {
            std::lock_guard lock(g_rt.mu);
            prev = g_rt.live;
            if (prev)
                prev->stop.store(true);
            ++g_rt.epoch;
            g_rt.events.clear();
            g_rt.base = 0;
            g_rt.level_gen = 0;
            g_rt.level = 0;
            g_rt.closed = false;
            live = std::make_shared<RtLive>();
            live->epoch = g_rt.epoch;
            live->session = std::make_shared<pm::plugins::chat_host::RealtimeSession>();
            g_rt.live = live;
        }
        g_rt.cv.notify_all();
        if (prev) {
            prev->stop.store(true);
#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
            release_rt_consent_emit(prev.get());
#endif
        }
        if (prev && prev->session)
            prev->session->stop();
        prev.reset();
#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
        live->consent = std::make_unique<pm::security::ConsentSurfaceScope>(
            opt.consent_ui.empty() ? std::string("auto-deny") : opt.consent_ui,
            opt.consent_owner);
#endif

        nlohmann::json opts{
            {"preset", body.value("preset", opt.preset)},
            {"chat_session_id", body.value("chat_session_id", std::string{})},
            {"mic_send", body.value("mic_send", true)},
            {"acquire_timeout_ms", body.value("acquire_timeout_ms", 1500)},
            {"router", opt.router},
            {"model", opt.model},
            {"api_key", opt.api_key},
            {"base_url", opt.base_url},
            {"streaming", opt.streaming},
            {"timeout_ms", opt.timeout_ms},
            {"max_iterations", opt.max_iterations},
            {"no_tools", opt.no_tools},
            {"runner", opt.runner},
        };
        if (body.contains("instructions") && body["instructions"].is_string())
            opts["instructions"] = body["instructions"];
        if (body.contains("voice") && body["voice"].is_string())
            opts["voice"] = body["voice"];

#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
        const bool stream_consent = live->consent && live->consent->resolved() == "stream";
#else
        const bool stream_consent = false;
#endif
        std::thread([live, opts = std::move(opts), stream_consent] {
            if (!live || !live->session)
                return;
            if (live->stop.load()) {
                rt_fail(live->epoch, "realtime stopped");
                return;
            }
            if (stream_consent) {
#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
                pm::llm::inquire::ensure_stream_ask_ui_backend();
                auto emit = [epoch = live->epoch](const std::string& type,
                                                  const std::string& payload_json) {
                    auto payload = media::json::try_parse(payload_json);
                    if (!payload.is_object())
                        return false;
                    const char* method =
                        type == "consent" ? pm::audio::session::audio_method_name(pm::audio::session::AudioMethod::ConsentRequest)
                        : type == "ask"   ? pm::audio::session::audio_method_name(pm::audio::session::AudioMethod::AskUser)
                                          : pm::audio::session::audio_method_name(pm::audio::session::AudioMethod::ConsentDismiss);
                    rt_emit_for(epoch, method, nlohmann::json::array({std::move(payload)}));
                    return true;
                };
                const std::uint64_t ticket = pm::security::bind_stream_consent_emit(emit);
                const std::uint64_t ask = pm::llm::inquire::bind_stream_ask_emit(emit);
                live->consent_ticket.store(ticket);
                live->ask_ticket.store(ask);
                if (live->stop.load()) {
                    release_rt_consent_emit(live.get());
                    rt_fail(live->epoch, "realtime stopped");
                    return;
                }
#endif
            }
            const PmChatRtCallbacks cb = rt_callbacks(live.get());
            std::string err;
            if (!live->session->open(opts, cb, err)) {
#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
                release_rt_consent_emit(live.get());
#endif
                rt_fail(live->epoch, err);
                return;
            }
            if (live->stop.load()) {
#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
                release_rt_consent_emit(live.get());
#endif
                live->session->stop();
                return;
            }
            std::string serr;
            if (!live->session->start(serr)) {
#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
                release_rt_consent_emit(live.get());
#endif
                rt_emit_for(live->epoch, pm::audio::session::audio_method_name(pm::audio::session::AudioMethod::Error),
                            nlohmann::json::array(
                                {serr.empty() ? std::string("realtime start failed") : serr}));
                live->session->stop();
                return;
            }
            if (live->stop.load()) {
#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
                release_rt_consent_emit(live.get());
#endif
                live->session->stop();
            }
        }).detach();

        res.status = 200;
        res.set_content(nlohmann::json{{"ok", true}}.dump(), "application/json");
    });

    svr.Post("/v1/realtime/stop", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        std::shared_ptr<RtLive> live;
        {
            std::lock_guard lock(g_rt.mu);
            live = g_rt.live;
        }
        if (live) {
            live->stop.store(true);
#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
            release_rt_consent_emit(live.get());
#endif
        }
        if (live && live->session)
            live->session->stop();
        else if (live)
            rt_fail(live->epoch, "realtime stopped");
        res.status = 200;
        res.set_content(nlohmann::json{{"ok", true}}.dump(), "application/json");
    });

    svr.Post("/v1/realtime/ptt", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        nlohmann::json body = media::json::parse_object_or(req.body);
        std::shared_ptr<RtLive> live;
        {
            std::lock_guard lock(g_rt.mu);
            live = g_rt.live;
        }
        if (live && live->session) {
            const bool held = body.value("held", true);
            live->session->set_mic_send(held);
            if (!held && body.value("finishInputTurn", false))
                live->session->finish_input();
        }
        res.status = 200;
        res.set_content(nlohmann::json{{"ok", true}}.dump(), "application/json");
    });

    svr.Post("/v1/realtime/speaker", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        nlohmann::json body = media::json::parse_object_or(req.body);
        std::shared_ptr<RtLive> live;
        {
            std::lock_guard lock(g_rt.mu);
            live = g_rt.live;
        }
        if (live && live->session)
            live->session->set_speaker_send(body.value("enabled", true));
        res.status = 200;
        res.set_content(nlohmann::json{{"ok", true}}.dump(), "application/json");
    });

    svr.Post("/v1/realtime/cancel-agent",
             [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        std::shared_ptr<RtLive> live;
        {
            std::lock_guard lock(g_rt.mu);
            live = g_rt.live;
        }
        const bool cancelled = live && live->session && live->session->cancel_agent({});
        res.status = cancelled ? 200 : 409;
        res.set_content(
            nlohmann::json{
                {"ok", cancelled},
                {"error", cancelled ? std::string{} : "no delegated agent is running"},
            }.dump(),
            "application/json");
    });

    svr.Get("/v1/realtime/events", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        const std::uint64_t epoch = [&] {
            std::lock_guard lock(g_rt.mu);
            return g_rt.epoch;
        }();
        logger::info(std::string("realtime events client epoch=") + std::to_string(epoch));
        res.set_chunked_content_provider(
            "text/event-stream; charset=utf-8",
            [epoch](size_t /*offset*/, httplib::DataSink& sink) -> bool {
                std::uint64_t pos = 0;
                std::uint64_t seen_level = 0;
                for (;;) {
                    std::vector<nlohmann::json> batch;
                    nlohmann::json level_event;
                    bool done = false;
                    bool superseded = false;
                    {
                        std::unique_lock lock(g_rt.mu);
                        g_rt.cv.wait_for(lock, std::chrono::seconds(15), [&] {
                            return g_rt.epoch != epoch || g_rt.closed || g_rt.level_gen != seen_level
                                || (g_rt.base + g_rt.events.size()) > pos;
                        });
                        if (g_rt.epoch != epoch)
                            superseded = true;
                        else {
                            if (g_rt.level_gen != seen_level) {
                                seen_level = g_rt.level_gen;
                                level_event = {
                                    {"method", pm::audio::session::audio_method_name(
                                                   pm::audio::session::AudioMethod::Level)},
                                    {"args", nlohmann::json::array({g_rt.level})},
                                };
                            }
                            if (pos < g_rt.base)
                                pos = g_rt.base;
                            const std::uint64_t end = g_rt.base + g_rt.events.size();
                            while (pos < end) {
                                batch.push_back(g_rt.events[static_cast<size_t>(pos - g_rt.base)]);
                                ++pos;
                            }
                            done = g_rt.closed && pos >= end && g_rt.level_gen == seen_level;
                        }
                    }
                    if (superseded)
                        return false;
                    if (!level_event.is_null()) {
                        const std::string line = "data: " + level_event.dump() + "\n\n";
                        if (!sink.write(line.data(), line.size()))
                            return false;
                    }
                    for (const auto& ev : batch) {
                        const std::string line = "data: " + ev.dump() + "\n\n";
                        if (!sink.write(line.data(), line.size()))
                            return false;
                    }
                    if (done) {
                        sink.done();
                        return false;
                    }
                    if (batch.empty() && level_event.is_null()) {
                        static const char ping[] = ": keepalive\n\n";
                        if (!sink.write(ping, sizeof(ping) - 1u))
                            return false;
                    }
                }
            });
    });

    svr.Post("/v1/llm/path-tools/call",
             [&opt, &gate, require_v1_auth](const httplib::Request& req, httplib::Response& res) {
                 if (!require_v1_auth(req, res))
                     return;
                 if (opt.no_tools) {
                     res.status = 403;
                     res.set_content(
                         openai_error("path tools are disabled on this daemon (--no-tools)",
                                      "invalid_request_error", "tools_disabled")
                             .dump(),
                         "application/json");
                     return;
                 }
                 nlohmann::json body;
                 try {
                     body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
                 } catch (const std::exception& e) {
                     res.status = 400;
                     res.set_content(
                         openai_error(std::string("invalid JSON: ") + e.what(),
                                      "invalid_request_error", "invalid_json")
                             .dump(),
                         "application/json");
                     return;
                 }
                 if (!body.contains("name") || !body["name"].is_string()) {
                     res.status = 400;
                     res.set_content(
                         openai_error("'name' (string) is required", "invalid_request_error",
                                      "missing_name")
                             .dump(),
                         "application/json");
                     return;
                 }
                 const std::string          name = body["name"].get<std::string>();
                 const nlohmann::json       args =
                     body.contains("arguments") && body["arguments"].is_object()
                         ? body["arguments"]
                         : nlohmann::json::object();

                 std::vector<std::string> disabled;
                 std::vector<std::string> allowed;
                 bool                     allowlist_set = false;
                 media_cli::parse_agent_disable_tools(opt.disable_tools, opt.no_tools, disabled,
                                                     nullptr);
                 media_cli::parse_agent_enable_tools(opt.enable_tools, opt.no_tools, allowed,
                                                    allowlist_set);
                 if (allowlist_set) {
                     bool ok = false;
                     for (const auto& a : allowed) {
                         if (a == name) {
                             ok = true;
                             break;
                         }
                     }
                     if (!ok) {
                         res.status = 403;
                         res.set_content(
                             openai_error("tool is not in the daemon enable-tools allowlist: " + name,
                                          "invalid_request_error", "tool_not_allowed")
                                 .dump(),
                             "application/json");
                         return;
                     }
                 }

                 InflightGate::Slot slot(gate);
                 if (!slot) {
                     respond_queue_full(res);
                     return;
                 }
                 const std::string req_cwd = resolve_request_cwd(req, body, opt);
                 PathBaseGuard     path_base(req_cwd, opt.fs_jail);
                 PathBlocklistGuard block(disabled);
                 const media::llm::path::ExecuteResult out = media::llm::path::execute(name, args);
                 res.status = 200;
                 res.set_content(out.envelope.dump(), "application/json");
             });

    // jq leaf transform (same engine as `llm agent each`). Inline `document`
    // or server-side `source` path; response includes `output` (+ optional dst write).
    svr.Post("/v1/llm/each",
             [&opt, &gate, require_v1_auth](const httplib::Request& req, httplib::Response& res) {
                 if (!require_v1_auth(req, res))
                     return;
                 nlohmann::json body;
                 try {
                     body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
                 } catch (const std::exception& e) {
                     res.status = 400;
                     res.set_content(
                         openai_error(std::string("invalid JSON: ") + e.what(),
                                      "invalid_request_error", "invalid_json")
                             .dump(),
                         "application/json");
                     return;
                 }
                 media::llm::agent::each::EachOptions each_opt;
                 std::string parse_err;
                 if (!media::llm::agent::each::parse_each_options(body, each_opt, parse_err)) {
                     res.status = 400;
                     res.set_content(
                         openai_error(parse_err, "invalid_request_error", "invalid_each_options")
                             .dump(),
                         "application/json");
                     return;
                 }
                 InflightGate::Slot slot(gate);
                 if (!slot) {
                     respond_queue_full(res);
                     return;
                 }
                 const std::string req_cwd = resolve_request_cwd(req, body, opt);
                 if (each_opt.cwd.empty())
                     each_opt.cwd = req_cwd;
                 if (each_opt.preset.empty())
                     each_opt.preset = opt.preset;
                 if (each_opt.router.empty())
                     each_opt.router = opt.router;
                 if (each_opt.model.empty())
                     each_opt.model = opt.model;
                 apply_request_provider_overrides(req, body, each_opt.api_key, each_opt.base_url);
                 apply_provider_defaults(each_opt.api_key, each_opt.base_url, opt.api_key,
                                         opt.base_url);
                 if (each_opt.timeout_ms <= 0 && opt.timeout_ms > 0)
                     each_opt.timeout_ms = opt.timeout_ms;
                 if (each_opt.api_mode.empty() && !opt.api_mode.empty())
                     each_opt.api_mode = opt.api_mode;
                 if (each_opt.system_prompt.empty() && !opt.system_prompt.empty())
                     each_opt.system_prompt = opt.system_prompt;

                 media::llm::agent::each::EachResult result;
                 (void)media::llm::agent::each::run_each(each_opt, result);
                 if (!result.ok && result.document.is_null()) {
                     const bool bad_req =
                         result.error.find("required") != std::string::npos
                         || result.error.find("must be") != std::string::npos
                         || result.error.find("invalid JSON") != std::string::npos
                         || result.error.find("could not open") != std::string::npos;
                     res.status = bad_req ? 400 : 500;
                     res.set_content(
                         openai_error(result.error.empty() ? "each failed" : result.error,
                                      bad_req ? "invalid_request_error" : "server_error",
                                      "each_error")
                             .dump(),
                         "application/json");
                     return;
                 }
                 res.status = 200;
                 res.set_content(result.to_json().dump(), "application/json");
             });

    // System One decisions (same engine as `llm agent decide`).
    svr.Post("/v1/llm/decide",
             [&opt, &gate, require_v1_auth](const httplib::Request& req, httplib::Response& res) {
                 if (!require_v1_auth(req, res))
                     return;
                 nlohmann::json body;
                 try {
                     body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
                 } catch (const std::exception& e) {
                     res.status = 400;
                     res.set_content(
                         openai_error(std::string("invalid JSON: ") + e.what(),
                                      "invalid_request_error", "invalid_json")
                             .dump(),
                         "application/json");
                     return;
                 }
                 media::llm::agent::decide::DecideOptions decide_opt;
                 std::string parse_err;
                 if (!media::llm::agent::decide::parse_decide_options(body, decide_opt, parse_err)) {
                     res.status = 400;
                     res.set_content(
                         openai_error(parse_err, "invalid_request_error", "invalid_decide_options")
                             .dump(),
                         "application/json");
                     return;
                 }
                 InflightGate::Slot slot(gate);
                 if (!slot) {
                     respond_queue_full(res);
                     return;
                 }
                 const std::string req_cwd = resolve_request_cwd(req, body, opt);
                 if (decide_opt.cwd.empty())
                     decide_opt.cwd = req_cwd;
                 if (decide_opt.model.empty())
                     decide_opt.model = opt.model;
                 apply_request_provider_overrides(req, body, decide_opt.api_key, decide_opt.base_url);
                 apply_provider_defaults(decide_opt.api_key, decide_opt.base_url, opt.api_key,
                                         opt.base_url);
                 if (decide_opt.timeout_ms <= 0 && opt.timeout_ms > 0)
                     decide_opt.timeout_ms = opt.timeout_ms;

                 media::llm::agent::decide::DecideResult result;
                 const bool ok = media::llm::agent::decide::run_decide(decide_opt, result);
                 if (!ok) {
                     const bool bad_req =
                         result.error.find("required") != std::string::npos
                         || result.error.find("must be") != std::string::npos
                         || result.error.find("cannot be combined") != std::string::npos
                         || result.error.find("matched nothing") != std::string::npos
                         || result.error.find("invalid JSON") != std::string::npos;
                     res.status = bad_req ? 400 : 500;
                     res.set_content(
                         openai_error(result.error.empty() ? "decide failed" : result.error,
                                      bad_req ? "invalid_request_error" : "server_error",
                                      "decide_error")
                             .dump(),
                         "application/json");
                     return;
                 }
                 res.status = 200;
                 res.set_content(result.to_json().dump(), "application/json");
             });

    svr.Post("/v1/chat/completions",
             [&opt, &gate, require_v1_auth](const httplib::Request& req, httplib::Response& res) {
                 if (!require_v1_auth(req, res))
                     return;

                 nlohmann::json body;
                 try {
                     body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
                 } catch (const std::exception& e) {
                     res.status = 400;
                     res.set_content(openai_error(std::string("invalid JSON: ") + e.what(),
                                                  "invalid_request_error", "invalid_json")
                                         .dump(),
                                     "application/json");
                     return;
                 }

                 ParsedChatRequest parsed;
                 std::string       parse_err;
                 if (!parse_chat_request(body, parsed, parse_err)) {
                     res.status = 400;
                     res.set_content(
                         openai_error(parse_err, "invalid_request_error", "invalid_messages").dump(),
                         "application/json");
                     return;
                 }

                 const std::string req_cwd = resolve_request_cwd(req, body, opt);
                 const std::string req_session =
                     media::llm::agent::session_get_or_create(resolve_request_session(req, body));
                 const bool        stream  = body.value("stream", false);
                 const std::string request_id = openai_completion_id();
                 logger::info("llm-server request phase=received id=" + request_id
                              + " stream=" + (stream ? std::string("1") : std::string("0"))
                              + " prompt_chars=" + std::to_string(parsed.user_prompt.size()));

                 // Stash per-request provider creds; applied in run_one_completion only
                 // when resolved router is tanit (LiteLLM). OpenAI presets keep profile keys.
                 AgentServerOptions turn_opt = opt;
                 if (lan_completion_without_tools(req, opt.serve_api_key))
                     turn_opt.no_tools = true;
                 std::string        req_provider_key;
                 std::string        req_provider_base;
                 apply_request_provider_overrides(req, body, req_provider_key, req_provider_base);

                 if (stream) {
                     // Live SSE: hold the gate slot for the lifetime of the provider.
                     {
                         const auto gs = gate.stats();
                         logger::info("llm-server request phase=gate_wait id=" + request_id
                                      + " inflight=" + std::to_string(gs.inflight)
                                      + " waiting=" + std::to_string(gs.waiting));
                     }
                     auto slot = std::make_shared<InflightGate::Slot>(gate);
                     if (!*slot) {
                         logger::warn("llm-server request phase=gate_rejected id=" + request_id);
                         respond_queue_full(res);
                         return;
                     }
                     logger::info("llm-server request phase=gate_acquired id=" + request_id
                                  + " wait_ms=" + std::to_string(slot->queue_wait_ms()));
                     // Browser env after gate so waiters do not stamp process env early.
                     auto browser = std::make_shared<BrowserEnvGuard>(body);
                     res.status = 200;
                     res.set_header("Cache-Control", "no-cache");
                     res.set_header("Connection", "keep-alive");
                     res.set_header("X-Tanit-Session", req_session);
                     res.set_header("X-Tanit-Queue-Wait-Ms", std::to_string(slot->queue_wait_ms()));
                     res.set_chunked_content_provider(
                         "text/event-stream; charset=utf-8",
                         [turn_opt, body, parsed, req_cwd, req_session, request_id, slot, browser,
                          req_provider_key, req_provider_base](
                             size_t /*offset*/, httplib::DataSink& sink) -> bool {
                             (void)browser;
                             const auto t_exec0 = std::chrono::steady_clock::now();
                             bool client_connected = true;
                             auto emit = [&sink, &client_connected, &request_id](
                                             std::string_view wire) -> bool {
                                 const bool ok = sink.write(wire.data(), wire.size());
                                 if (!ok && client_connected) {
                                     client_connected = false;
                                     logger::warn("llm-server request phase=client_closed id="
                                                  + request_id);
                                 }
                                 return ok;
                             };
                             TurnOutcome outcome =
                                 run_one_completion(turn_opt, body, parsed, req_cwd, req_session,
                                                    request_id, emit, req_provider_key,
                                                    req_provider_base);
                             outcome.queue_wait_ms = slot->queue_wait_ms();
                             outcome.exec_ms =
                                 std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t_exec0)
                                     .count();
                             if (!outcome.ok) {
                                 const std::string line = sse_data_line(openai_error(outcome.error));
                                 (void)sink.write(line.data(), line.size());
                             } else {
                                 nlohmann::json usage = usage_fields(outcome.usage);
                                 if (outcome.exec_ms > 0)
                                     usage["wall_ms"] = outcome.exec_ms;
                                 nlohmann::json stop{
                                     {"id",
                                      outcome.stream_id.empty() ? openai_completion_id()
                                                                : outcome.stream_id},
                                     {"object", "chat.completion.chunk"},
                                     {"created",
                                      outcome.stream_created != 0 ? outcome.stream_created
                                                                  : openai_created_unix()},
                                     {"model", outcome.model},
                                     {"choices",
                                      nlohmann::json::array(
                                          {{{"index", 0},
                                            {"delta", nlohmann::json::object()},
                                            {"finish_reason",
                                             outcome.finish_reason.empty() ? "stop"
                                                                           : outcome.finish_reason}}})},
                                     {"tanit",
                                      {{"queue_wait_ms", outcome.queue_wait_ms},
                                       {"exec_ms", outcome.exec_ms},
                                       {"session_id", outcome.session_id},
                                       {"streamed_chars", outcome.streamed_chars},
                                       {"stream_deltas", outcome.stream_deltas}}},
                                     {"usage", std::move(usage)},
                                 };
                                 const std::string stop_line = sse_data_line(stop);
                                 (void)sink.write(stop_line.data(), stop_line.size());
                             }
                             static const char done[] = "data: [DONE]\n\n";
                             (void)sink.write(done, sizeof(done) - 1u);
                             sink.done();
                             logger::info("llm-server request phase=finished id=" + request_id
                                          + " ok="
                                          + (outcome.ok ? std::string("1") : std::string("0"))
                                          + " exec_ms=" + std::to_string(outcome.exec_ms));
                             return false;
                         });
                     return;
                 }

                 {
                     const auto gs = gate.stats();
                     logger::info("llm-server request phase=gate_wait id=" + request_id
                                  + " inflight=" + std::to_string(gs.inflight)
                                  + " waiting=" + std::to_string(gs.waiting));
                 }
                 InflightGate::Slot slot(gate);
                 if (!slot) {
                     logger::warn("llm-server request phase=gate_rejected id=" + request_id);
                     respond_queue_full(res);
                     return;
                 }
                 logger::info("llm-server request phase=gate_acquired id=" + request_id
                              + " wait_ms=" + std::to_string(slot.queue_wait_ms()));
                 BrowserEnvGuard browser(body);
                 const auto      t_exec0 = std::chrono::steady_clock::now();
                 TurnOutcome     outcome =
                     run_one_completion(turn_opt, body, parsed, req_cwd, req_session, request_id, {},
                                        req_provider_key, req_provider_base);
                 outcome.queue_wait_ms = slot.queue_wait_ms();
                 outcome.exec_ms       = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t_exec0)
                                     .count();
                 res.set_header("X-Tanit-Session", req_session);
                 res.set_header("X-Tanit-Queue-Wait-Ms", std::to_string(outcome.queue_wait_ms));
                 res.set_header("X-Tanit-Exec-Ms", std::to_string(outcome.exec_ms));

                 if (!outcome.ok) {
                     logger::warn("llm-server request phase=finished id=" + request_id
                                  + " ok=0 exec_ms=" + std::to_string(outcome.exec_ms));
                     res.status = 500;
                     res.set_content(openai_error(outcome.error).dump(), "application/json");
                     return;
                 }
                 logger::info("llm-server request phase=finished id=" + request_id
                              + " ok=1 exec_ms=" + std::to_string(outcome.exec_ms));
                 res.status = 200;
                 res.set_content(completion_success_json(outcome).dump(), "application/json");
             });

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO && defined(FEATURE_VIDEO_STREAM) && FEATURE_VIDEO_STREAM
    {
        VideoStreamHostConfig cfg;
        cfg.path_prefix = "/v1/video";
        cfg.require_stream_token = false;
        cfg.input = "screen:0";
        cfg.stream_id = "llm";
        cfg.stop_when_last_viewer = true;
        g_llm_video_host = std::make_shared<VideoStreamHost>(std::move(cfg));
        g_llm_video_host->mount(svr, require_v1_auth);
    }
    svr.Post("/v1/video/start", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        const auto host = g_llm_video_host;
        if (!host) {
            res.status = 501;
            res.set_content(openai_error("video streaming is not built into this binary",
                                         "server_error", "video_unavailable")
                                .dump(),
                            "application/json");
            return;
        }
        nlohmann::json body = nlohmann::json::object();
        if (!req.body.empty()) {
            body = media::json::try_parse(req.body);
            if (!body.is_object()) {
                write_video_json(res, {{"ok", false}, {"error", "invalid JSON body"}}, 400);
                return;
            }
        }
        VideoStreamCaptureUpdate update;
        std::string err;
        if (!fill_video_update(body, update, err)) {
            write_video_json(res, {{"ok", false}, {"error", err}}, 400);
            return;
        }
        const VideoStreamStartResult started = host->start(update);
        if (!started.ok) {
            write_video_json(res, {{"ok", false}, {"error", started.error}}, 400);
            return;
        }
        nlohmann::json status = host->status();
        status["accepted"] = true;
        status["already"] = started.already;
        status["switched"] = started.switched;
        write_video_json(res, status);
    });
    svr.Post("/v1/video/stop", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        const auto host = g_llm_video_host;
        if (!host) {
            res.status = 501;
            res.set_content(openai_error("video streaming is not built into this binary",
                                         "server_error", "video_unavailable")
                                .dump(),
                            "application/json");
            return;
        }
        host->stop();
        nlohmann::json status = host->status();
        status["accepted"] = true;
        status["stopped"] = true;
        write_video_json(res, status);
    });
#else
    auto video_unavailable = [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        res.status = 501;
        res.set_content(openai_error("video streaming is not built into this binary", "server_error",
                                     "video_unavailable")
                            .dump(),
                        "application/json");
    };
    for (const char* path : {"/v1/video/status", "/v1/video/sources", "/v1/video/snapshot.jpg",
                             "/v1/video/mjpeg", "/v1/video/signal"})
        svr.Get(path, video_unavailable);
    for (const char* path : {"/v1/video/start", "/v1/video/stop", "/v1/video/source",
                             "/v1/video/control", "/v1/video/signal", "/v1/video/transcribe",
                             "/v1/video/speak"})
        svr.Post(path, video_unavailable);
#endif

    auto lan_off = [](httplib::Response& res) {
        res.status = 404;
        res.set_content(openai_error("LAN discovery is off; restart with --lan").dump(),
                        "application/json");
    };

    svr.Post("/v1/logs", [require_v1_auth](const httplib::Request& req, httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        if (req.body.size() > 512 * 1024) {
            res.status = 413;
            res.set_content(openai_error("log batch is too large").dump(), "application/json");
            return;
        }
        const nlohmann::json body = media::json::try_parse(req.body.empty() ? "{}" : req.body);
        if (!body.is_object() || !body.contains("lines") || !body["lines"].is_array()) {
            res.status = 400;
            res.set_content(openai_error("body must be {\"lines\":[...]}").dump(), "application/json");
            return;
        }
        std::string device = "lan";
        std::string device_name;
        if (g_lan) {
            for (const auto& secret : request_secrets(req)) {
                lan::DeviceAuth dev;
                if (!g_lan->authorize(secret, dev))
                    continue;
                if (!dev.id.empty())
                    device = clip_text(dev.id, 64);
                device_name = clip_text(dev.name, 80);
                break;
            }
        }
        auto text_field = [](const nlohmann::json& obj, const char* key) -> std::string {
            if (!obj.contains(key) || !obj[key].is_string())
                return {};
            return obj[key].get<std::string>();
        };
        std::string session = text_field(body, "session_id");
        if (session.empty())
            session = "lan";
        session = clip_text(std::move(session), 80);
        const std::string model = clip_text(text_field(body, "device_model"), 80);
        const std::string now = utc_now_iso();
        std::string ndjson;
        int accepted = 0;
        for (const auto& line : body["lines"]) {
            if (accepted >= 200)
                break;
            if (!line.is_object())
                continue;
            std::string message = text_field(line, "message");
            if (message.empty())
                continue;
            message = clip_text(std::move(message), 8192);
            std::string when = text_field(line, "_time");
            if (when.empty())
                when = text_field(line, "time");
            if (when.size() < 19 || when.size() > 40 || when.find('T') == std::string::npos)
                when = now;
            std::string level = text_field(line, "level");
            if (level.empty())
                level = "info";
            std::string component = text_field(line, "component");
            if (component.empty())
                component = "logcat";
            nlohmann::json event{
                {"_time", when},
                {"source", "tanit-android"},
                {"device", device},
                {"device_kind", "phone"},
                {"device_model", model},
                {"device_name", device_name},
                {"build", "android"},
                {"session_id", session},
                {"level", log_level_name(std::move(level))},
                {"component", clip_text(std::move(component), 64)},
                {"message", message},
                {"via", "pair"},
            };
            ndjson += event.dump();
            ndjson.push_back('\n');
            ++accepted;
        }
        const bool stored = accepted > 0 && forward_victoria_ndjson(ndjson);
        if (accepted > 0 && !stored) {
            res.status = 503;
            res.set_content(nlohmann::json{{"ok", false},
                                           {"accepted", accepted},
                                           {"stored", false},
                                           {"error", "log store unavailable"}}
                                .dump(),
                            "application/json");
            return;
        }
        res.status = 200;
        res.set_content(nlohmann::json{{"ok", true}, {"accepted", accepted}, {"stored", stored}}.dump(),
                        "application/json");
    });

    svr.Get("/v1/lan/info", [](const httplib::Request&, httplib::Response& res) {
        if (!g_lan) {
            res.status = 200;
            res.set_content(nlohmann::json{{"ok", true}, {"enabled", false}}.dump(),
                            "application/json");
            return;
        }
        const lan::PublicInfo info = g_lan->info();
        res.status = 200;
        res.set_content(nlohmann::json{
                            {"ok", true},
                            {"enabled", true},
                            {"protocol", lan::k_protocol_version},
                            {"name", info.name},
                            {"fingerprint", info.fingerprint},
                            {"tls_port", info.tls_port},
                            {"pairing", info.pairing_open},
                            {"mdns", info.mdns},
                            {"addresses", info.addresses},
                        }
                            .dump(),
                        "application/json");
    });

    svr.Post("/v1/lan/pair/begin", [require_v1_auth, lan_off](const httplib::Request& req,
                                                             httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        if (!g_lan) {
            lan_off(res);
            return;
        }
        nlohmann::json body = nlohmann::json::object();
        if (!req.body.empty()) {
            body = media::json::try_parse(req.body);
            if (!body.is_object()) {
                res.status = 400;
                res.set_content(openai_error("invalid JSON body").dump(), "application/json");
                return;
            }
        }
        std::vector<std::string> scopes;
        std::string scope_err;
        std::string csv;
        if (body.contains("scopes")) {
            if (!body["scopes"].is_array() || body["scopes"].empty()) {
                res.status = 400;
                res.set_content(openai_error("scopes must list chat, files, tools, or video").dump(),
                                "application/json");
                return;
            }
            for (const auto& s : body["scopes"]) {
                if (s.is_string()) {
                    if (!csv.empty())
                        csv += ",";
                    csv += s.get<std::string>();
                }
            }
        }
        if (!lan::parse_scopes(csv, scopes, scope_err)) {
            res.status = 400;
            res.set_content(openai_error(scope_err).dump(), "application/json");
            return;
        }
        int ttl = body.value("ttl_sec", 300);
        lan::PairingTicket ticket;
        std::string err;
        if (!g_lan->open_pairing(scopes, ttl, ticket, err)) {
            res.status = 500;
            res.set_content(openai_error(err.empty() ? "could not open pairing" : err).dump(),
                            "application/json");
            return;
        }
        ticket.image_path = g_lan->reveal_qr(ticket);
        res.status = 200;
        res.set_content(nlohmann::json{
                            {"ok", true},
                            {"uri", ticket.uri},
                            {"secret", ticket.secret},
                            {"expires_at", ticket.expires_at},
                            {"host", ticket.host},
                            {"port", ticket.port},
                            {"fingerprint", ticket.fingerprint},
                            {"scopes", ticket.scopes},
                            {"qr", ticket.qr_ascii},
                            {"image", ticket.image_path},
                            {"qr_error", ticket.qr_error},
                        }
                            .dump(),
                        "application/json");
    });

    svr.Post("/v1/lan/pair", [lan_off](const httplib::Request& req, httplib::Response& res) {
        if (lan_hop_present(req) && !lan_hop_ok(req)) {
            res.status = 400;
            res.set_content(openai_error("invalid LAN hop").dump(), "application/json");
            return;
        }
        const bool via_tls = lan_hop_ok(req);
        if (!via_tls && !machine_local_request(req)) {
            res.status = 403;
            res.set_content(
                openai_error("send the pairing secret over TLS after checking the fingerprint")
                    .dump(),
                "application/json");
            return;
        }
        if (!g_lan) {
            lan_off(res);
            return;
        }
        const nlohmann::json body = media::json::try_parse(req.body.empty() ? "{}" : req.body);
        if (!body.is_object()) {
            res.status = 400;
            res.set_content(openai_error("invalid JSON body").dump(), "application/json");
            return;
        }
        std::vector<std::string> scopes;
        if (body.contains("scopes") && body["scopes"].is_array()) {
            for (const auto& s : body["scopes"]) {
                if (!s.is_string())
                    continue;
                std::string name = s.get<std::string>();
                name = media::strutil::lower_ascii(std::move(name));
                scopes.push_back(std::move(name));
            }
        }
        lan::PairGrant grant;
        std::string err;
        const lan::PairError st = g_lan->complete_pairing(
            body.value("secret", std::string{}), body.value("device_id", std::string{}),
            body.value("device_name", std::string{}), scopes, grant, err);
        if (st != lan::PairError::Ok) {
            res.status = st == lan::PairError::BadSecret ? 401
                         : st == lan::PairError::Expired ? 410
                         : st == lan::PairError::Locked  ? 429
                         : st == lan::PairError::NoSession ? 404
                                                           : 403;
            if (st == lan::PairError::Locked)
                res.set_header("Retry-After", "30");
            res.set_content(openai_error(err.empty() ? "pairing refused" : err).dump(),
                            "application/json");
            return;
        }
        res.status = 200;
        res.set_content(nlohmann::json{
                            {"ok", true},
                            {"token", grant.token},
                            {"device_id", grant.device_id},
                            {"fingerprint", grant.fingerprint},
                            {"scopes", grant.scopes},
                            {"protocol", lan::k_protocol_version},
                        }
                            .dump(),
                        "application/json");
    });

    svr.Get("/v1/lan/devices", [require_v1_auth, lan_off](const httplib::Request& req,
                                                         httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        if (!g_lan) {
            lan_off(res);
            return;
        }
        nlohmann::json rows = nlohmann::json::array();
        for (const auto& d : g_lan->devices()) {
            rows.push_back({
                {"id", d.id},
                {"name", d.name},
                {"scopes", d.scopes},
                {"created", d.created},
                {"last_seen", d.last_seen},
                {"revoked", d.revoked},
            });
        }
        res.status = 200;
        res.set_content(nlohmann::json{{"ok", true}, {"devices", std::move(rows)}}.dump(),
                        "application/json");
    });

    svr.Post("/v1/lan/devices/revoke", [require_v1_auth, lan_off](const httplib::Request& req,
                                                                  httplib::Response& res) {
        if (!require_v1_auth(req, res))
            return;
        if (!g_lan) {
            lan_off(res);
            return;
        }
        const nlohmann::json body = media::json::try_parse(req.body.empty() ? "{}" : req.body);
        if (!body.is_object()) {
            res.status = 400;
            res.set_content(openai_error("invalid JSON body").dump(), "application/json");
            return;
        }
        std::string err;
        if (!g_lan->revoke(body.value("id", std::string{}), err)) {
            res.status = 404;
            res.set_content(openai_error(err.empty() ? "device not found" : err).dump(),
                            "application/json");
            return;
        }
        res.status = 200;
        res.set_content(nlohmann::json{{"ok", true}}.dump(), "application/json");
    });
}

#if defined(FEATURE_COMMAND_IPC_NEXT) && FEATURE_COMMAND_IPC_NEXT
void attach_chat_host_ipc(media::cmd_ipc::InstanceServer& ipc)
{
    ipc.set_extra_action_handler([](const nlohmann::json& req) {
        if (req.value("action", std::string{}) != "chat_host")
            return nlohmann::json();
        const std::string method = req.value("method", std::string{});
        if (method.empty() || !req.contains("args") || !req["args"].is_array()) {
            return nlohmann::json{
                {"type", "error"},
                {"error", "chat_host requires method and args"},
            };
        }
        const auto pid = req.value("pid", std::uint64_t{0});
        const std::string sender = chat_sender_for_process(pid);
        chat_emit(method, req["args"], sender);
        nlohmann::json ack{{"type", "chat_host_ack"}, {"method", method}};
        if (!sender.empty())
            ack["sender"] = sender;
        return ack;
    });
}
#endif

} // namespace

int run_llm_agent_openai_server(const AgentServerOptions& opt)
{
    AgentServerOptions options = opt;
    if (const std::string err = media::llm::agent::cliopt::validate_server_options(options); !err.empty()) {
        std::cerr << "llm agent --serve: " << err << "\n";
        if (err.find("non-loopback") != std::string::npos)
            std::cerr << "  phones should use --host 127.0.0.1 --lan (TLS), not cleartext HTTP\n";
        return 1;
    }
    const int http_workers = media::llm::agent::cliopt::resolved_http_workers(options);
    std::vector<std::string> pair_scopes;
    if (options.lan && options.pair_on_start) {
        std::string scope_err;
        if (!lan::parse_scopes(options.pair_scopes, pair_scopes, scope_err)) {
            std::cerr << "llm agent --serve: " << scope_err << "\n";
            return 1;
        }
    }

    std::unique_ptr<lan::Service> lan_service;
    if (options.lan) {
        lan_service = std::make_unique<lan::Service>();
        std::string lan_err;
        if (!lan_service->prepare(options.lan_name, lan_err)) {
            std::cerr << "llm agent --serve: " << lan_err << "\n";
            return 1;
        }
        g_lan = lan_service.get();
    }
    if (options.serve_api_key.empty()) {
        logger::warn(std::string("llm agent --serve: no API key — /v1 is open on ")
                     + options.host + ":" + std::to_string(options.port)
                     + " (dev/loopback only; treat as full desktop trust)");
    }

    register_cli_host_tool_providers(options);

    // Keep a few HTTP workers free for consent / health / stop while turns wait.
    const int reserve_control = (std::min)(4, (std::max)(1, http_workers / 8));
    const int max_waiting =
        (std::max)(0, http_workers - options.concurrency - reserve_control);
    InflightGate gate(options.concurrency, max_waiting);
    auto         svr = std::make_shared<httplib::Server>();
    svr->new_task_queue = [http_workers] {
        return new httplib::ThreadPool(static_cast<size_t>(http_workers));
    };
    register_routes(*svr, options, gate);

    if (!svr->bind_to_port(options.host, options.port)) {
        std::cerr << "llm agent --serve: could not bind " << options.host << ":" << options.port
                  << "\n";
        g_lan = nullptr;
        return 1;
    }

#if defined(FEATURE_COMMAND_IPC_NEXT) && FEATURE_COMMAND_IPC_NEXT
    media::cmd_ipc::InstanceServer ipc_server;
    ipc_server.set_stop_hook([] { media::cli::request_cancel(); });
    attach_chat_host_ipc(ipc_server);
    std::error_code cwd_ec;
    std::string cwd = media::path::path_to_utf8(std::filesystem::current_path(cwd_ec));
    if (cwd.empty())
        cwd = ".";
    std::string ipc_err;
    const std::string label = "llm agent :" + std::to_string(options.port);
    if (!ipc_server.start(media::cmd_ipc::ProcessKind::LlmAgent, cwd, ipc_err, label))
        logger::info(std::string("llm agent server: activity registration skipped: ") + ipc_err);
#endif

    const std::string base = "http://" + options.host + ":" + std::to_string(options.port);
    std::thread server_thread([svr] { (void)svr->listen_after_bind(); });

    if (lan_service) {
        const int tls_port = options.lan_port > 0 ? options.lan_port : options.port + 1;
        std::string lan_err;
        if (tls_port > 65535 || !lan_service->listen(options.host, options.port, tls_port, lan_err)) {
            std::cerr << "llm agent --serve: "
                      << (lan_err.empty() ? "could not start LAN discovery" : lan_err) << "\n";
            stop_llm_video_host();
            svr->stop();
            if (server_thread.joinable())
                server_thread.join();
            g_lan = nullptr;
            return 1;
        }
        const lan::PublicInfo info = lan_service->info();
        std::cout << "Tanit LAN\n"
                  << "  name: " << info.name << "\n"
                  << "  tls:  "
                  << (info.addresses.empty() ? std::string(info.name) : info.addresses.front())
                  << ":" << info.tls_port << "\n";
        if (info.addresses.size() > 1) {
            std::cout << "  also:";
            for (size_t i = 1; i < info.addresses.size(); ++i)
                std::cout << " " << info.addresses[i];
            std::cout << "\n";
        }
        std::cout << "  fingerprint: " << info.fingerprint_colon << "\n"
                  << "  mdns: " << (info.mdns ? "_tanit._tcp" : "unavailable — type the address")
                  << "\n"
                  << "  trust the fingerprint, not the advertised name\n"
                  << std::flush;
        if (options.pair_on_start) {
            lan::PairingTicket ticket;
            std::string pair_err;
            if (!lan_service->open_pairing(pair_scopes, 300, ticket, pair_err)) {
                std::cerr << "llm agent --serve: " << pair_err << "\n";
            } else {
                ticket.image_path = lan_service->reveal_qr(ticket);
                std::cout << "\nPair phone (one-time, expires " << ticket.expires_at << ")\n"
                          << "  scopes:";
                for (const auto& scope : ticket.scopes)
                    std::cout << " " << scope;
                std::cout << "\n";
                if (!ticket.qr_ascii.empty())
                    std::cout << ticket.qr_ascii;
                if (!ticket.qr_error.empty())
                    std::cout << "  QR: " << ticket.qr_error << "\n";
                if (!ticket.image_path.empty())
                    std::cout << "  image: " << ticket.image_path << "\n";
                std::cout << "  uri: " << ticket.uri << "\n" << std::flush;
            }
        }
    }

    std::cout << "llm agent --serve: " << base
              << "  (GET /health, GET /v1/models, POST /v1/chat/completions,"
              << " POST /v1/llm/each, POST /v1/llm/decide, GET /v1/video/status; concurrency="
              << options.concurrency << "; max_waiting=" << max_waiting
              << "; http_workers=" << http_workers
              << "; auth=" << (options.serve_api_key.empty() ? "off" : "on")
              << "; consent=" << options.consent_ui;
    if (options.lan)
        std::cout << "; lan=on";
    std::cout << ")\n  stop: Ctrl+C\n" << std::flush;
    logger::info(std::string("llm agent server listening on ") + base
                 + " concurrency=" + std::to_string(options.concurrency)
                 + " max_waiting=" + std::to_string(max_waiting)
                 + " http_workers=" + std::to_string(http_workers)
                 + " auth=" + (options.serve_api_key.empty() ? "off" : "on"));

    while (!media::cli::cancel_requested())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (lan_service)
        lan_service->stop();
    stop_llm_video_host();
    svr->stop();
    if (server_thread.joinable())
        server_thread.join();
    g_lan = nullptr;
    return 0;
}

} // namespace media_cli

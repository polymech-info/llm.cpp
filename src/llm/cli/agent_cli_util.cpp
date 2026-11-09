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
#include <iomanip>

namespace pm_llm_agent_cli {
/// Parse a feature-mask spec (decimal / 0xHEX / comma-separated names with
/// optional +/- deltas) into a mask. Returns the fallback on empty or
/// fully-unparseable input. Delegates to the library helper so the CLI, env
/// var, and any library embedder share the same grammar.
pm::llm::StructuredMemoryFeatures parse_memory_features_string(
    const std::string& s, pm::llm::StructuredMemoryFeatures fallback, const char* origin)
{
    if (s.empty()) return fallback;
    auto parsed = media::llm::agent::parse_structured_memory_features(s);
    if (!parsed) {
        logger::warn(std::string("llm agent: ignoring malformed ") + origin + " = " + s);
        return fallback;
    }
    logger::info(std::string("llm agent: structured_memory_features ") + origin + " = " + s
                 + " (resolved 0x" + [&] {
                     std::ostringstream os;
                     os << std::hex << *parsed;
                     return os.str();
                   }() + ")");
    return *parsed;
}

/// Resolve the structured-memory mask for a Turn. Precedence (highest first):
///   1. `--memory-features=<mask>` CLI option
///   2. `--no-replay` CLI flag (clears the ReplayTranscript bit)
///   3. `PM_LLM_STRUCTURED_MEMORY_FEATURES` env var
///   4. the existing `Turn::structured_memory_features` default
pm::llm::StructuredMemoryFeatures resolve_structured_memory_features(
    pm::llm::StructuredMemoryFeatures fallback, const PmImageCliState& st)
{
    // settings profile → env feature mask → CLI --memory-features → env profile
    media::llm::agent::Turn probe;
    probe.structured_memory_features = fallback;
    media::llm::agent::apply_agent_memory_settings_to_turn(probe);
    pm::llm::StructuredMemoryFeatures mask =
        media::llm::agent::apply_structured_memory_features_env(
            probe.structured_memory_features, "PM_LLM_STRUCTURED_MEMORY_FEATURES");
    if (!st.ag_memory_features.empty())
        mask = parse_memory_features_string(st.ag_memory_features, mask, "--memory-features");
    if (st.ag_no_replay) {
        mask &= ~static_cast<pm::llm::StructuredMemoryFeatures>(
            pm::llm::StructuredMemoryFeature::ReplayTranscript);
        logger::info("llm agent: --no-replay cleared ReplayTranscript bit");
    }
    if (const char* recall_profile = std::getenv("PM_LLM_MEMORY_RECALL_PROFILE");
        recall_profile && *recall_profile) {
        media::llm::agent::apply_memory_recall_profile(mask, recall_profile);
    }
    return mask;
}

#if FEATURE_HOME_LLM_SKILLS
media::llm::skills::SkillSnapshot load_skill_snapshot_for_cwd(
    const std::string& cwd_hint,
    const media::llm::agent::Turn* turn_override) {
    media::llm::skills::SkillPolicy policy;
    if (turn_override && turn_override->skill_policy_override_set) {
        policy.enabled = turn_override->skills_enabled;
        policy.roaming_enabled = turn_override->skills_roaming_enabled;
        policy.workspace_enabled = turn_override->skills_workspace_enabled;
        policy.pinned = turn_override->pinned_skills;
        policy.disabled = turn_override->disabled_skills;
    } else {
        media::runtime_settings::AgentSkillsSettings cfg;
        std::string err;
        (void)media::runtime_settings::load_agent_skills_settings(cfg, err);
        policy.enabled = cfg.enabled;
        policy.roaming_enabled = cfg.roaming_enabled;
        policy.workspace_enabled = cfg.workspace_enabled;
        policy.pinned = cfg.pinned;
        policy.disabled = cfg.disabled;
    }
    return media::llm::skills::discover_skills(policy, cwd_hint);
}
#endif

void log_llm_usage_aggregate_debug(const nlohmann::json& agg, long long wall_ms) {
    if (!agg.is_object())
        return;
    auto num_ll = [](const nlohmann::json& j, const char* k) -> long long {
        if (!j.contains(k) || !j[k].is_number())
            return 0;
        if (j[k].is_number_unsigned())
            return static_cast<long long>(j[k].get<std::uint64_t>());
        if (j[k].is_number_integer())
            return j[k].get<long long>();
        return static_cast<long long>(j[k].get<double>());
    };
    const long long pr = num_ll(agg, "prompt_tokens");
    const long long co = num_ll(agg, "completion_tokens");
    const long long tt = num_ll(agg, "total_tokens");
    const auto rounds = agg.value("llm_rounds", nlohmann::json::array());
    const bool has_cost = agg.contains("cost") && agg["cost"].is_number();
    const bool has = (rounds.is_array() && !rounds.empty()) || tt > 0 || pr > 0 || co > 0 || has_cost;
    if (!has)
        return;

    std::ostringstream os;
    os << "llm agent: usage — prompt=" << pr << " completion=" << co << " total=" << tt;
    if (has_cost) {
        const double c = agg["cost"].get<double>();
        os << std::fixed << std::setprecision(6) << " cost_usd≈" << c << std::defaultfloat;
    }
    if (rounds.is_array() && !rounds.empty())
        os << " api_rounds=" << rounds.size();
    if (wall_ms >= 0)
        os << " wall_ms=" << wall_ms;
    logger::info(os.str());
}

std::string trim_agent_cli_value(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool read_agent_system_prompt_file(const std::string& value,
                                   std::string& text,
                                   std::string& resolved_path,
                                   std::string& err) {
    err.clear();
    text.clear();
    resolved_path.clear();
    std::string path_value = value;
    if (!path_value.empty() && path_value.front() == '@')
        path_value.erase(path_value.begin());
    else if (path_value.rfind("file:", 0) == 0)
        path_value.erase(0, 5);
    path_value = trim_agent_cli_value(path_value);
    if (path_value.empty()) {
        err = "empty system prompt file path";
        return false;
    }

    std::error_code ec;
    fs::path fp = media::path::path_from_utf8(path_value);
    if (!fp.is_absolute())
        fp = fs::absolute(fp, ec);
    if (ec) {
        err = "could not resolve system prompt file: " + path_value + ": " + ec.message();
        return false;
    }
    if (!fs::is_regular_file(fp, ec)) {
        err = "system prompt file does not exist: " + media::path::path_to_utf8(fp);
        return false;
    }
    std::ifstream in(fp, std::ios::binary);
    if (!in) {
        err = "could not open system prompt file: " + media::path::path_to_utf8(fp);
        return false;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    text = buf.str();
    resolved_path = media::path::path_to_utf8(fp);
    return true;
}

bool apply_agent_system_prompt_option(const std::string& raw,
                                      media::llm::agent::Turn& turn,
                                      std::string& err) {
    return media::llm::agent::options::apply_system_prompt_option(raw, turn, err);
}

// Mirror of apply_agent_system_prompt_option for the planner. Same file /
// @path / file: / literal / null grammar; the loaded text is then run through
// `${builtin}` template substitution by the planner itself, so callers can
// extend the baked-in instructions instead of replacing them.
bool apply_agent_planner_prompt_option(const std::string& raw,
                                       media::llm::agent::Turn& turn,
                                       std::string& err) {
    return media::llm::agent::options::apply_planner_prompt_option(raw, turn, err);
}

// Per-turn capability opt-outs (--no-mcp / --no-skills / --no-planner). Applied
// AFTER apply_launch_policy_to_turn so they win over the resolved chat preset —
// mirrors the xblox llmAgent block's mcp/skills/planner params. mcp needs
// tool_policy_override_set so the persisted global tools policy does not reload
// over mcp_tools_enabled; planner uses PlannerStrategy::Disabled to beat
// planner_enabled (and --planner).

media::llm::agent::cliopt::AgentCliOptions agent_cli_options_from(const PmImageCliState& st)
{
    media::llm::agent::cliopt::AgentCliOptions o;
    o.prompt = st.ag_prompt;
    o.system_prompt = st.ag_system_prompt;
    o.planner_prompt = st.ag_planner_prompt;
    o.include_paths = st.ag_paths;
    o.embed_paths = st.ag_embed_paths;
    o.preset = st.ag_preset;
    o.runner = st.ag_runner;
    o.save_preset = st.ag_save_preset;
    o.router = st.ag_router;
    o.model = st.ag_model;
    o.api_key = st.ag_api_key;
    o.base_url = st.ag_base_url;
    o.api_mode = st.ag_api_mode;
    o.streaming = st.ag_streaming;
    o.timeout_ms = st.ag_timeout_ms;
    o.max_iterations = st.ag_max_iter;
    o.no_tools = st.ag_no_tools;
    o.no_mcp = st.ag_no_mcp;
    o.no_skills = st.ag_no_skills;
    o.skills = st.ag_skills;
    o.no_planner = st.ag_no_planner;
    o.planner = st.ag_planner;
    o.planner_budget = st.ag_planner_budget;
    o.no_parallel_tools = st.ag_no_parallel_tools;
    o.ai_consent_review = st.ag_ai_consent_review;
    o.ai_consent_router = st.ag_ai_consent_router;
    o.ai_consent_model = st.ag_ai_consent_model;
    o.consent_ui = st.ag_consent_ui;
    o.ocr_provider = st.ag_ocr_provider;
    o.ocr_model = st.ag_ocr_model;
    o.recognition_provider = st.ag_recognition_provider;
    o.recognition_model = st.ag_recognition_model;
    o.image_provider = st.ag_image_provider;
    o.image_model = st.ag_image_model;
    o.video_provider = st.ag_video_provider;
    o.video_model = st.ag_video_model;
    o.stt_provider = st.ag_stt_provider;
    o.stt_model = st.ag_stt_model;
    o.tts_provider = st.ag_tts_provider;
    o.tts_model = st.ag_tts_model;
    o.tts_voice_id = st.ag_tts_voice_id;
    o.context_reduction = st.ag_context_reduction;
    o.context_reduction_ratio = st.ag_context_reduction_ratio;
    o.context_reduction_min_bytes = st.ag_context_reduction_min_bytes;
    o.context_reduction_objective = st.ag_context_reduction_objective;
    o.session_id = st.ag_session_id;
    o.load_snapshot = st.ag_load_snapshot;
    o.save_snapshot = st.ag_save_snapshot;
    o.multi_turn = st.ag_multi_turn;
    return o;
}

void apply_agent_capability_opt_outs(media::llm::agent::Turn& turn, const PmImageCliState& st)
{
    media::llm::agent::cliopt::apply_capability_opt_outs(turn, agent_cli_options_from(st));
}

void append_session_turn_summary_if_enabled(const PmImageCliState& /*st*/,
                                            const std::string&     session_task_id,
                                            const std::string&     user_prompt,
                                            const media::llm::agent::Result& result,
                                            pm::llm::StructuredMemoryFeatures features)
{
    // Gate purely on whether a session is open — single-turn + --session-id
    // (cross-process A2A pattern) also benefits from the persisted event log.
    if (session_task_id.empty() || !result.ok)
        return;
    // Skip the legacy chat-style turn log when the caller has turned off
    // transcript replay; structured memory now carries the conversational
    // context (kind=conversation/role=turn_pair items).
    if (!pm::llm::has_feature(features, pm::llm::StructuredMemoryFeature::ReplayTranscript))
        return;

    nlohmann::json entry = nlohmann::json::object();
    entry["ts"]   = media::llm::agent::now_iso8601();
    entry["user"] = user_prompt;
    if (!result.final_text.empty())
        entry["assistant"] = result.final_text;

    nlohmann::json tools = nlohmann::json::array();
    try {
        if (result.transcript.is_object() && result.transcript.contains("messages")
            && result.transcript["messages"].is_array()) {
            for (const auto& m : result.transcript["messages"]) {
                if (!m.is_object())
                    continue;
                if (m.value("role", std::string()) != "tool")
                    continue;
                const std::string tn = m.value("name", std::string());
                if (!tn.empty())
                    tools.push_back(tn);
            }
        }
    } catch (...) {}
    if (!tools.empty())
        entry["tools"] = std::move(tools);

    media::llm::agent::session_append_turn(session_task_id, entry);
}

// ── Scheduler daemon helper for `llm agent --scheduler` ─────────────────────
//
// SIGINT is intentionally handled via a single global atomic flag (and not the
// per-call `prov` capture) because std::signal cannot accept a closure on
// POSIX/Windows. The flag also doubles as a ctrl-c sentinel for the wait loop.
std::atomic<bool> g_scheduler_stop_requested{false};

void scheduler_sigint_handler(int) {
    g_scheduler_stop_requested.store(true, std::memory_order_release);
    // Best-effort wake the scheduler thread out of any in-progress speak/run.
    media::llm::path::abort_active_speak();
    media::llm::path::abort_active_run();
}

/// Configure and run the global AgentScheduler in-process, blocking the CLI
/// process so scheduled ticks (`schedule_every` / `schedule_in` / `schedule_at`
/// created during the agent's turn(s)) actually fire. Returns when:
///   * SIGINT is received (Ctrl+C)             → exit code 0
///   * timeout_s > 0 and that many seconds pass → exit code 0
///   * exit_when_idle and no enabled tasks remain → exit code 0
int run_scheduler_blocking(const media::llm::agent::ProviderSettings& prov,
                           int  timeout_s,
                           bool exit_when_idle)
{
    auto& sched = media::llm::agent::global_scheduler();

    // Re-snapshot the provider settings on every tick. Capturing prov by
    // value here is intentional: settings.json changes mid-loop are NOT
    // hot-reloaded (a CLI session is short-lived).
    sched.configure([prov] { return prov; },
                    /*on_event=*/nullptr);

    g_scheduler_stop_requested.store(false, std::memory_order_release);

    using SigHandler = void (*)(int);
    SigHandler prev_sigint = std::signal(SIGINT, &scheduler_sigint_handler);

    {
        auto tasks = media::llm::agent::task_store_list();
        size_t enabled = 0;
        for (const auto& t : tasks) if (t.enabled) ++enabled;
        std::cerr << "[scheduler] starting -- enabled tasks=" << enabled
                  << " (total=" << tasks.size() << ")";
        if (timeout_s > 0) std::cerr << " timeout=" << timeout_s << "s";
        if (exit_when_idle) std::cerr << " exit-when-idle=on";
        std::cerr << " router=" << prov.router << " model=" << prov.model
                  << " -- press Ctrl+C to stop\n";
    }

    sched.start();

    const auto deadline = (timeout_s > 0)
        ? std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s)
        : std::chrono::steady_clock::time_point::max();

    while (!g_scheduler_stop_requested.load(std::memory_order_acquire)) {
        if (timeout_s > 0 && std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "[scheduler] timeout reached -- stopping\n";
            break;
        }
        if (exit_when_idle) {
            auto tasks = media::llm::agent::task_store_list();
            bool any_enabled = false;
            for (const auto& t : tasks) {
                if (t.enabled) { any_enabled = true; break; }
            }
            if (!any_enabled) {
                std::cerr << "[scheduler] no enabled tasks remain -- stopping\n";
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    sched.stop();
    std::signal(SIGINT, prev_sigint);
    g_scheduler_stop_requested.store(false, std::memory_order_release);
    return 0;
}
} // namespace pm_llm_agent_cli

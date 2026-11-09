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

void pm_image_register_llm(CLI::App& app, PmImageCliState& s) {
#if FEATURE_HOME_LLM_TOOLS
    const char* llm_desc = "LLM: tools-list / tools-call / info (saved chat + image defaults) / agent (path tools)";
#else
    const char* llm_desc = "LLM: info (saved chat defaults) / agent (text-only; built-in tools disabled)";
#endif
    s.llm_cmd = app.add_subcommand("llm", llm_desc);
    s.llm_cmd->require_subcommand(1);

#if FEATURE_HOME_LLM_TOOLS
    const char* llm_info_desc =
        "Show Chat router/model and image provider/model from app settings, effective defaults "
        "for path tools, and which CLI flags override per command (find/transform/meta/duplicates, …)";
#else
    const char* llm_info_desc =
        "Show Chat router/model and configured LLM providers from app settings.";
#endif
    s.llm_info_cmd = s.llm_cmd->add_subcommand(
        "info",
        llm_info_desc);
    s.llm_info_cmd->add_flag("--json", s.llm_info_json, "Print machine-readable JSON on stdout");
    s.llm_info_cmd->add_flag("--compress-md", s.llm_info_compress_md,
        "Print raw compact Markdown for router/planner context: tools, skills, XBlox blocks. "
        "Ignored with --json.");
    s.llm_info_cmd->add_option("--preset", s.llm_info_preset,
        "Chat settings preset name or id. If omitted, uses preset `Default`, or the lone saved preset.");
    s.llm_info_cmd
        ->add_option("--markdown", s.llm_info_markdown,
                     "Human stdout: render (default — always pretty-print), auto (only on TTY), or plain (raw UTF-8). "
                     "Ignored with --json.")
        ->check(CLI::IsMember({"auto", "plain", "render"}))
        ->default_val("render");
    s.llm_info_cmd
        ->add_option("--color", s.llm_info_color,
                     "When markdown rendering is used: auto (color on TTY unless NO_COLOR/TERM=dumb), never, or always.")
        ->check(CLI::IsMember({"auto", "never", "always"}))
        ->default_val("auto");
    s.llm_info_cmd->add_flag(
        "--no-mcp-probe", s.llm_info_no_mcp_probe,
        "Do not run live MCP profile probes (stdio/HTTP handshakes). JSON/text output still lists settings; "
        "the `mcp` object notes that the probe was skipped.");
    s.llm_info_cmd->add_flag_function(
        "--mcp-probe",
        [&](std::int64_t) { s.llm_info_no_mcp_probe = false; },
        "Run live MCP profile probes (stdio/HTTP handshakes).");

    s.llm_info_providers_cmd = s.llm_info_cmd->add_subcommand(
        "providers",
        "List enabled LLM providers from the provider registry and app settings.");
    s.llm_info_providers_cmd->add_flag("--json", s.llm_info_json, "Output JSON array");
    s.llm_info_providers_cmd->add_flag(
        "--all", s.llm_info_providers_all,
        "Include providers disabled by FEATURE_PROVIDER_* or Group Policy (for GPO generation).");

    s.llm_info_models_cmd = s.llm_info_cmd->add_subcommand(
        "models",
        "List models for a given provider. "
        "local / llama: text/planner GGUF models. "
        "vlm: local VLM models (multimodal / vision / OCR) with mmproj paths. "
        "whisper: local whisper.cpp ggml models. "
        "pixlwiz / openrouter / replicate: fetches live catalog (disk-cached; replicate 3 d, others 24 h). "
        "Other names: shows configured default_model from app settings.");
    s.llm_info_models_cmd->add_option(
        "--provider", s.llm_info_models_provider,
        "Provider to query: local | llama | vlm | whisper | pixlwiz | openrouter | replicate | <name from `llm info providers`>")->required(true);
    s.llm_info_models_cmd->add_flag("--no-cache", s.llm_info_models_no_cache,
        "Bypass the disk cache and force a live HTTP fetch (pixlwiz / openrouter).");
    s.llm_info_models_cmd->add_flag("--json", s.llm_info_json, "Output JSON");

#if FEATURE_HOME_LLM_TOOLS
    s.llm_info_tools_cmd = s.llm_info_cmd->add_subcommand(
        "tools",
        "List all built-in path-mode agent tools (name + description). "
        "These are the tools available to the chat agent in every session.");
    s.llm_info_tools_cmd->add_flag("--json", s.llm_info_json, "Output JSON array {name, description}");
    s.llm_info_tools_cmd->add_flag(
        "--all", s.llm_info_tools_all,
        "Show the full built-in registry, ignoring user/GPO filtering (for GPO template generation).");

#endif

#if FEATURE_HOME_LLM_SKILLS
    s.llm_info_skills_cmd = s.llm_info_cmd->add_subcommand(
        "skills",
        "List discovered agent skills from roaming and workspace roots with availability/active state.");
    s.llm_info_skills_cmd->add_flag("--json", s.llm_info_json, "Output JSON");
#endif

#if defined(FEATURE_WINDOWS_GPO) && FEATURE_WINDOWS_GPO
    s.llm_info_policy_cmd = s.llm_info_cmd->add_subcommand(
        "policy",
        "Dump the full GPO policy catalog (keys, categories, types, defaults, ADMX strings). "
        "Single source consumed by scripts/generate-admx.mjs.");
    s.llm_info_policy_cmd->add_flag("--json", s.llm_info_json, "Output JSON");
#endif

#if FEATURE_HOME_LLM_TOOLS
    const std::string llm_tools_list_desc = std::string("Print the JSON-Schema tool catalog (one entry per ") + pm::brand::k_app_id_u8 + " op)";
    s.llm_list_cmd = s.llm_cmd->add_subcommand("tools-list", llm_tools_list_desc.c_str());
    s.llm_list_cmd->add_flag("--path", s.llm_tools_list_path,
        "List path-mode chat agent tools (default catalog for llm agent). Omit for in-buffer REST/MCP tools.");

    s.llm_call_cmd = s.llm_cmd->add_subcommand("tools-call",
        "Invoke a tool by name with a JSON arguments envelope");
    s.llm_call_cmd->add_option("--name", s.llm_call_name,
        "Tool name (image_resize|image_compress|image_transform|image_create|image_meta|image_find|file_read)")->required(true);
    s.llm_call_cmd->add_option("--args", s.llm_call_args_path,
        "Path to JSON arguments file ('-' or '@-' for stdin; omit = empty {}).");
    s.llm_call_cmd->add_option("--image-file", s.llm_call_image_file,
        "Convenience: read this file, base64-encode it, and inject as arguments.image.b64.");
#endif

#if FEATURE_HOME_LLM_TOOLS
    s.llm_agent_cmd = s.llm_cmd->add_subcommand("agent",
        "Run a single chat-agent turn: LLM picks tools (image_resize / compress / "
        "transform / meta / find), runs them on the supplied paths, and writes "
        "outputs to disk. Use --no-tools for a plain one-shot text reply (no path tools). "
        "Use --disable-tools=a,b to omit specific path tools from the catalog. "
        "Provider is router-aware (OpenAI-compatible client).");
#else
    s.llm_agent_cmd = s.llm_cmd->add_subcommand("agent",
        "Run a single text-only chat-agent turn. Built-in path tools and MCP tools "
        "are disabled in this build (FEATURE_HOME_LLM_TOOLS=OFF).");
#endif
    s.llm_agent_cmd->add_option("-p,--prompt", s.ag_prompt,
        "User prompt (required unless piped via stdin or --mic is used, e.g. 'compress these as MozJPEG quality 70'). "
        "Supports ${VAR} variable substitution (CWD, SRC_FILE, YYYY, …) and file references: "
        "`@path`, `file:path`, or an absolute path to a UTF-8 text file.")
        ->group("Input");
    s.llm_agent_cmd->add_option(
        "--logging-dir", s.logging_dir,
        "Directory for pm-image.log and per-run agent JSON artifacts. Relative paths are resolved from cwd. "
        "Same as the global --logging-dir.")
        ->group("Input");
    s.llm_agent_cmd->add_option("--system-prompt", s.ag_system_prompt,
        "Override the full system prompt. Use `null`/`none` to send no system message, "
        "`@path` or `file:path` to read a UTF-8 file (relative or absolute), or literal text.")
        ->group("Prompts");
    s.llm_agent_cmd->add_option("--planner-prompt", s.ag_planner_prompt,
        "Override the planner system prompt. Same grammar as --system-prompt "
        "(literal text, `@path`, `file:path`, or `null`/`none`). "
        "The loaded text is run through `${...}` template substitution; "
        "`${builtin}` resolves to the baked-in planner instructions so you "
        "can extend rather than fully replace. A `planner-prompt.md` file "
        "in the exe folder or the config dir is used automatically when no "
        "CLI override is given.")
        ->group("Prompts");
    s.llm_agent_cmd->add_option("--include", s.ag_paths,
        "One or more file paths to put in the agent's selection context. "
        "Repeatable. When omitted, --cwd is used as the folder context.")->expected(-1)
        ->group("Input");
    s.llm_agent_cmd->add_option("--embed", s.ag_embed_paths,
        "One or more text file paths to inline into the user prompt under 'Embedded Content'. "
        "Uses the same text read policy as file_read. Repeatable; works even with --system-prompt null.")->expected(-1)
        ->group("Input");
    s.llm_agent_cmd->add_option("--preset", s.ag_preset,
        "Chat settings preset name or id. Loads saved router/model/max-iter/planner defaults; explicit CLI flags override it.")
        ->group("Provider");
    s.llm_agent_cmd->add_option("--runner", s.ag_runner,
        "Agent backend: native (default) | claude-code | codex. "
        "Also reads PM_LLM_AGENT_RUNNER when unset.")
        ->group("Provider");
    s.llm_agent_cmd->add_option("--router", s.ag_router,
        "LLM router (llama|openrouter|openai|deepseek|gemini|ollama|fireworks|xai|huggingface). "
        "Default: from Chat Provider Settings in app.")
        ->group("Provider");
    s.llm_agent_cmd->add_option("--model", s.ag_model,
        "Model id (router-specific, e.g. openai/gpt-4o-mini). "
        "Default: from Chat Provider Settings in app.")
        ->group("Provider");
    s.llm_agent_cmd->add_flag("--ai-consent-review", s.ag_ai_consent_review,
        "Run the SecurityAgent as an advisory reviewer before CLI consent prompts. "
        "The review is shown on stderr; human approval is still required.")
        ->group("Provider");
    s.llm_agent_cmd->add_option("--ai-consent-router", s.ag_ai_consent_router,
        "Router for the advisory SecurityAgent reviewer. Implies --ai-consent-review. "
        "Default: Chat security router, then planner router, then text router.")
        ->group("Provider");
    s.llm_agent_cmd->add_option("--ai-consent-model", s.ag_ai_consent_model,
        "Model/script for the advisory SecurityAgent reviewer. Implies --ai-consent-review. "
        "Default: Chat security model, then planner model, then text model.")
        ->group("Provider");
    s.llm_agent_cmd->add_option("--consent-ui", s.ag_consent_ui,
        "Consent surface for security-gated tools: cli | win32 | owner | auto-deny | auto-allow | auto | preset. "
        "cli = stdio prompt (stderr/stdin); win32 = native modal dialog; "
        "auto-deny = deny every Ask (headless); "
        "auto-allow (alias: yolo) = allow every Ask with a session grant; "
        "owner = route to owning surface (not yet wired -> auto). "
        "When omitted: chat preset `consent_ui` (Yolo) if set, else win32 "
        "(native dialog). Pass cli for a stdio prompt.")
        ->group("Security");
    s.llm_agent_cmd->add_option("--consent-owner", s.ag_consent_owner,
        "Target descriptor (ipc id) for --consent-ui owner routing. Reserved; owner "
        "delegation is not wired yet.")
        ->group("Security");
    s.llm_agent_cmd->add_option("--ocr-provider", s.ag_ocr_provider,
        "Override the OCR tool provider for this run (e.g. llama, onnx, replicate). "
        "Wins over Chat → OCR settings and the active preset.")
        ->group("Tool Overrides");
    s.llm_agent_cmd->add_option("--ocr-model", s.ag_ocr_model,
        "Override the OCR tool model id for this run (e.g. paddleocr-vl-1-5). "
        "Wins over Chat → OCR settings and the active preset.")
        ->group("Tool Overrides");
    s.llm_agent_cmd->add_option("--recognition-provider", s.ag_recognition_provider,
        "Override the image recognition (image_understand) provider for this run.")
        ->group("Tool Overrides");
    s.llm_agent_cmd->add_option("--recognition-model", s.ag_recognition_model,
        "Override the image recognition model id for this run.")
        ->group("Tool Overrides");
    s.llm_agent_cmd->add_option("--image-provider", s.ag_image_provider,
        "Override the image creation (image_create) provider for this run.")
        ->group("Tool Overrides");
    s.llm_agent_cmd->add_option("--image-model", s.ag_image_model,
        "Override the image creation model id for this run.")
        ->group("Tool Overrides");
    s.llm_agent_cmd->add_option("--video-provider", s.ag_video_provider,
        "Override the video creation (create_video) provider for this run.")
        ->group("Tool Overrides");
    s.llm_agent_cmd->add_option("--video-model", s.ag_video_model,
        "Override the video creation model id for this run.")
        ->group("Tool Overrides");
    s.llm_agent_cmd->add_option("--api-key", s.ag_api_key,
        "API key (optional; default from app chat / API Keys in settings.json)")
        ->group("Provider");
    s.llm_agent_cmd->add_option("--base-url", s.ag_base_url,
        "Override the router's default base URL (OpenAI-compatible endpoints). "
        "Default: from app API Providers settings.")
        ->group("Provider");
    s.llm_agent_cmd->add_option("--timeout-ms", s.ag_timeout_ms,
        "HTTP timeout per LLM round (ms). Default: from Chat Provider Settings (or 60000).")
        ->group("Provider");
    s.llm_agent_cmd->add_option("--max-iter", s.ag_max_iter,
        "Maximum tool-call iterations before forcing a final response. "
        "Default: from Chat Provider Settings (or 8).")
        ->group("Agent");
#if FEATURE_HOME_LLM_TOOLS
    s.llm_agent_cmd->add_flag("--no-tools", s.ag_no_tools,
        "Do not register path tools (no list_images, image_resize, image_compress, …) — one LLM text turn only")
        ->group("Agent");
    s.llm_agent_cmd->add_flag("--no-mcp", s.ag_no_mcp,
        "Suppress MCP-backed tools for this turn, overriding the chat preset (path/run tools still register).")
        ->group("Agent");
    s.llm_agent_cmd->add_flag("--no-skills", s.ag_no_skills,
        "Skip agent skill discovery/injection for this turn, overriding the chat preset.")
        ->group("Agent");
    s.llm_agent_cmd->add_flag("--skills", s.ag_skills,
        "Force roaming/workspace skill discovery for this turn, overriding disabled chat/global skill policy.")
        ->group("Agent");
    s.llm_agent_cmd->add_flag("--no-planner", s.ag_no_planner,
        "Force the planner pre-pass off for this turn, overriding --planner and any chat-preset default.")
        ->group("Agent");
    s.llm_agent_cmd->add_flag("--planner", s.ag_planner,
        "Run a compact planner pass first to select a budgeted subset of tool schemas, then execute the normal agent.")
        ->group("Agent");
    s.llm_agent_cmd->add_option("--planner-budget", s.ag_planner_budget,
        "Maximum tool schemas the planner may pre-expand before falling back to the full catalog.")
        ->default_val(8)
        ->group("Agent");
    s.llm_agent_cmd->add_flag("--no-parallel-tools", s.ag_no_parallel_tools,
        "Disable concurrent tool dispatch and fall back to serial execution. "
        "Parallel dispatch (P6) is on by default; use this flag to opt out.")
        ->group("Agent");
    s.llm_agent_cmd->add_flag("--parallel-tools", s.ag_parallel_tools,
        "Dispatch all tool calls in a single LLM response concurrently (std::async). "
        "On by default — this flag is accepted for compatibility but is a no-op unless "
        "--no-parallel-tools was previously applied.")
        ->group("Agent");
#endif
    s.llm_agent_cmd->add_flag(
        "--multi-turn", s.ag_multi_turn,
        "Enable session memory across turns (default: on).")
        ->each([&](const std::string&) { s.ag_multi_turn_explicit = true; })
        ->group("Session");
    s.llm_agent_cmd->add_flag_function(
        "--single-turn",
        [&](std::int64_t) { s.ag_multi_turn = false; s.ag_multi_turn_explicit = true; },
        "Disable session memory and run as one-shot only.")
        ->group("Session");
    s.llm_agent_cmd->add_option(
        "--session-id", s.ag_session_id,
        "Optional session id used when --multi-turn is enabled (allows continuity across CLI invocations).")
        ->group("Session");
    s.llm_agent_cmd->add_option(
        "--load-snapshot", s.ag_load_snapshot,
        "P12: Load a named snapshot before the agent run. Merges the snapshot's memory_state / recent_events "
        "into the session context so the agent starts with that knowledge baseline. "
        "Name is sanitized to a filename; file lives under <config>/snapshots/<name>.json.")
        ->group("Session");
    s.llm_agent_cmd->add_option(
        "--save-snapshot", s.ag_save_snapshot,
        "P12: Save the current session state to a named snapshot after the agent run. "
        "Requires --multi-turn (needs an active session). The snapshot captures memory_state + "
        "recent_events so the workflow can be restored later via --load-snapshot.")
        ->group("Session");
    s.llm_agent_cmd->add_option(
        "--memory-features", s.ag_memory_features,
        "Override structured-memory feature mask. Accepts a decimal/0xHEX literal "
        "or comma-separated flag names with optional +/- deltas. Special tokens: "
        "all, default, none. Examples: --memory-features=0x7FF | "
        "--memory-features=CaptureWriteArtifacts,InjectItems | "
        "--memory-features=default,-ReplayTranscript. Overrides "
        "PM_LLM_STRUCTURED_MEMORY_FEATURES. See pm::llm::StructuredMemoryFeature.")
        ->group("Session");
    s.llm_agent_cmd->add_flag(
        "--no-replay", s.ag_no_replay,
        "Clear the ReplayTranscript bit from the structured-memory mask "
        "(skip raw chat-history injection; rely on session items/rules only).")
        ->group("Session");
    s.llm_agent_cmd->add_flag(
        "--scheduler", s.ag_scheduler,
        "After the first turn completes, start the agent scheduler in this "
        "process so schedule_every / schedule_in / schedule_at tasks created "
        "by the agent actually fire. Blocks until Ctrl+C, --scheduler-timeout "
        "expires, or --scheduler-exit-when-idle and no tasks remain.")
        ->group("Scheduler");
    s.llm_agent_cmd->add_option(
        "--scheduler-timeout", s.ag_scheduler_timeout_s,
        "When --scheduler is set, auto-exit after N seconds (0 = run until Ctrl+C / idle).")
        ->default_val(0)
        ->group("Scheduler");
    s.llm_agent_cmd->add_flag(
        "--scheduler-exit-when-idle", s.ag_scheduler_exit_when_idle,
        "When --scheduler is set, exit cleanly once every scheduled task is "
        "disabled (one-shots done, every-tasks hit max_runs / cancelled).")
        ->group("Scheduler");
#if FEATURE_HOME_LLM_TOOLS
    s.llm_agent_cmd->add_option("--disable-tools", s.ag_disable_tools,
        media_cli::build_agent_disable_tools_help(/*include_mcp=*/false))
        ->group("Agent");
    s.llm_agent_cmd->add_option("--enable-tools", s.ag_enable_tools,
        media_cli::build_agent_enable_tools_help())
        ->group("Agent");
#endif
#if defined(FEATURE_LLM_COMPRESSION) && FEATURE_LLM_COMPRESSION
    s.llm_agent_cmd
        ->add_option("--context-reduction", s.ag_context_reduction,
                     "Pre-API context reduction (attention reduction layer). Shrinks the wire "
                     "payload sent to the model each round; the transcript keeps full fidelity. "
                     "off (default), tool-results (reduce large tool-result messages only), or "
                     "all (also reduce older assistant/user context — never the system message "
                     "or the live user turn). JSON/code tool envelopes take the structural path; "
                     "free text takes the deterministic attention pipeline.")
        ->check(CLI::IsMember({"off", "none", "tool-results", "tools", "all"}))
        ->group("Context Reduction");
    s.llm_agent_cmd
        ->add_option("--context-reduction-ratio", s.ag_context_reduction_ratio,
                     "Target output/input byte ratio for reduced messages (0..1). "
                     "Only used when --context-reduction names a non-off mode.")
        ->default_val(0.5)
        ->check(CLI::Range(0.0, 1.0))
        ->group("Context Reduction");
    s.llm_agent_cmd
        ->add_option("--context-reduction-min-bytes", s.ag_context_reduction_min_bytes,
                     "Minimum message size (bytes) before the pre-API filter reduces it. "
                     "Only used when --context-reduction names a non-off mode.")
        ->default_val(2048)
        ->group("Context Reduction");
    s.llm_agent_cmd->add_option(
        "--context-reduction-objective", s.ag_context_reduction_objective,
        "Relevance objective for text reduction (empty = use the user prompt). "
        "Only used when --context-reduction names a non-off mode.")
        ->group("Context Reduction");
#endif
    s.llm_agent_cmd->add_flag("--json", s.ag_json,
        "Emit the full transcript as JSON on stdout (instead of plain-text events)")
        ->group("Output");
    s.llm_agent_cmd->add_flag(
        "--hud", s.ag_hud,
        "Show the overlay HUD over the clock. Stop cancels the run. Default off.")
        ->group("Output");
    s.llm_agent_cmd->add_option(
        "--hud-mode", s.ag_hud_mode,
        "HUD mode: off, taskbar, overlay, or both. Empty + --hud = overlay. "
        "taskbar/overlay/both enable the HUD even without --hud.")
        ->check(CLI::IsMember({"off", "taskbar", "overlay", "both"}, CLI::ignore_case))
        ->group("Output");
    s.llm_agent_cmd->add_flag("--dry-run", s.ag_dry,
        "Resolve provider + tools + selection; print context as Markdown to stdout. "
        "With --planner, runs only the planner LLM pass to preview selected schemas.")
        ->group("Output");
    s.llm_agent_cmd->add_option("--log", s.ag_log_path,
        "Write a JSON run log (provider, per-event tool calls with full envelopes, transcript) to this file (truncates). "
        "Default: <logging-dir>/agent.json.")->default_val("agent.json")
        ->group("Output");
    s.llm_agent_cmd->add_option("--dst", s.ag_dst,
        "Write the agent response to this file path (truncates on each turn). "
        "Without --json: writes result.final_text (raw markdown, no ANSI). "
        "With --json: writes the full result JSON object. "
        "Supports ${VAR} variable substitution (CWD, SRC_FILE, YYYY, MM, DD, …) "
        "resolved against --cwd / --include paths. Relative paths are resolved from --cwd. "
        "Parent directories are created automatically.")
        ->group("Output");
    s.llm_agent_cmd
        ->add_option("--type", s.ag_api_mode,
                     "LLM API type: completion (POST /chat/completions) or "
                     "responses (POST /responses — OpenAI Responses API; supported by "
                     "OpenAI, OpenRouter, and LiteLLM proxy), or realtime "
                     "(experimental WebSocket /realtime PoC; text-only; best with router=openai). "
                     "Default: " + std::string(pm::llm::k_default_api_mode) + ".")
        ->check(CLI::IsMember({"completion", "responses", "realtime"}))
        ->default_val(pm::llm::k_default_api_mode)
        ->group("API Mode");
    s.llm_agent_cmd->add_flag(
        "--realtime", s.ag_realtime,
        "Start a live speech-to-speech voice session over the WebSocket /v1/realtime "
        "endpoint (mic + speaker; the full agent is exposed as the run_pm_agent tool). "
        "No --prompt is required; if given it seeds the conversation. Runs until Ctrl+C "
        "or `--realtime-stop`. Best with router=openai / a realtime model.")
        ->group("API Mode");
    s.llm_agent_cmd->add_flag(
        "--realtime-stop", s.ag_realtime_stop,
        "Signal a running `--realtime` live voice session (this or another process) to "
        "stop cooperatively, then exit. Uses the cross-process session file.")
        ->group("API Mode");
    s.llm_agent_cmd->add_flag(
        "--realtime-status", s.ag_realtime_status,
        "Print whether a live `--realtime` voice session is running (add --json for "
        "machine-readable output), then exit.")
        ->group("API Mode");
    s.llm_agent_cmd->add_flag(
        "--serve", s.ag_serve,
        "Run an OpenAI-compatible HTTP daemon (GET /health, GET /v1/models, "
        "POST /v1/chat/completions). Turns inherit the same preset/tools/MCP/skills "
        "pipeline as a normal `llm agent` run. Default consent is auto-deny. "
        "See docs/llm/llm-server.md.")
        ->group("Server");
    s.llm_agent_cmd->add_option("--host", s.ag_serve_host,
        "Bind host for --serve (default 127.0.0.1).")
        ->default_val("127.0.0.1")
        ->group("Server");
    s.llm_agent_cmd->add_option("--port", s.ag_serve_port,
        "Bind port for --serve (default 8090).")
        ->default_val(8090)
        ->group("Server");
    s.llm_agent_cmd->add_option("--concurrency", s.ag_serve_concurrency,
        "Max concurrent agent turns for --serve (default 2).")
        ->default_val(2)
        ->group("Server");
    s.llm_agent_cmd->add_option("--http-workers", s.ag_serve_http_workers,
        "httplib worker threads for --serve (default: max(32, concurrency*2); "
        "raise for stress with many waiting clients).")
        ->group("Server");
    s.llm_agent_cmd->add_option("--serve-api-key", s.ag_serve_api_key,
        "Require Authorization: Bearer <key> on /v1/* when set. "
        "Empty = open (dev). Overrides TANIT_SERVE_API_KEY when passed.")
        ->group("Server");
    s.llm_agent_cmd->add_flag(
        "--fs-jail", s.ag_serve_fs_jail,
        "Hard-deny FS / search / pictures / service path tools outside the "
        "request workspace cwd (X-Tanit-Cwd / --cwd). Does not jail `run` or MCP. "
        "Also TANIT_SERVE_FS_JAIL=1.")
        ->group("Server");
    s.llm_agent_cmd->add_flag(
        "--lan", s.ag_serve_lan,
        "Advertise this daemon as _tanit._tcp and accept paired phones on a TLS port "
        "(HTTP stays on --host). No credentials in the discovery record. "
        "Also TANIT_SERVE_LAN=1. Requires --serve.")
        ->group("Server");
    s.llm_agent_cmd->add_option(
        "--lan-port", s.ag_serve_lan_port,
        "TLS port for --lan (default: --port + 1). Phones connect here, not to cleartext HTTP.")
        ->default_val(0)
        ->group("Server");
    s.llm_agent_cmd->add_option(
        "--lan-name", s.ag_serve_lan_name,
        "Display name in mDNS and the pairing QR. Default: this computer's name.")
        ->group("Server");
    s.llm_agent_cmd->add_flag(
        "--pair", s.ag_serve_pair,
        "With --serve, open a one-time pairing window and show a QR "
        "(implies --lan). The URI contains a short-lived secret.")
        ->group("Server");
    s.llm_agent_cmd->add_option(
        "--pair-scopes", s.ag_serve_pair_scopes,
        "Comma list granted by --pair: chat, files, tools, video. "
        "Default: chat, files, tools. video is the remote desktop viewer.")
        ->group("Server");
    s.llm_agent_cmd->add_flag(
        "--lan-devices", s.ag_serve_lan_devices,
        "List paired phones (id, name, scopes). Does not require --serve.")
        ->group("Server");
    s.llm_agent_cmd->add_option(
        "--lan-revoke", s.ag_serve_lan_revoke,
        "Revoke one paired phone by id. Does not require --serve. "
        "A running --lan daemon picks the change up from the device file.")
        ->group("Server");
    s.llm_agent_cmd->add_option(
        "--host-tool-provider", s.ag_host_tool_providers,
        "Register an HTTP IAgentToolProvider as id=http://127.0.0.1:port/path "
        "(repeatable). Loopback HTTP only. Also TANIT_HOST_TOOL_PROVIDER.")
        ->expected(1)
        ->group("Server");
    s.llm_agent_cmd->add_option(
        "--realtime-idle-ms", s.ag_realtime_idle_ms,
        "Self-stop a live `--realtime` session after this many ms of genuine idle "
        "time — no speech, no reply, and no delegated run_pm_agent in flight (a "
        "running agent task always keeps the session alive). Stops headless "
        "sessions from lingering. 0 = never auto-stop. Default: 120000 (2 min).")
        ->default_val(pm::llm::k_realtime_idle_timeout_ms)
        ->group("API Mode");
    s.llm_agent_cmd
        ->add_option("--streaming", s.ag_streaming,
                     "Streaming mode: on (default), auto (catalog-gated), off. "
                     "Implemented now for --type responses; completion remains non-streaming.")
        ->check(CLI::IsMember({"auto", "on", "off"}))
        ->default_val("on")
        ->group("Output");
    s.llm_agent_cmd
        ->add_option("--markdown", s.ag_markdown,
                     "Human stdout: auto (render when stdout is a TTY), plain (raw model UTF-8), or render (always run "
                     "the terminal markdown pass). Ignored with --json.")
        ->check(CLI::IsMember({"auto", "plain", "render"}))
        ->default_val("auto")
        ->group("Output");
    s.llm_agent_cmd
        ->add_option("--color", s.ag_color,
                     "When markdown rendering is used: auto (color on TTY unless NO_COLOR/TERM=dumb), never, or always.")
        ->check(CLI::IsMember({"auto", "never", "always"}))
        ->default_val("auto")
        ->group("Output");
    s.llm_agent_cmd->positionals_at_end(true);

#if defined(FEATURE_STT) && FEATURE_STT
    s.llm_agent_cmd->add_flag(
        "--mic", s.ag_mic,
        "Use the microphone as prompt input (continuous STT → LLM → TTS loop). "
        "Replaces --prompt for user input; --prompt may still be given as context. "
        "STT/TTS provider, model, and voice come from Chat → Voice & Audio "
        "(elevenlabs, tanit, openrouter, whisper). Press Ctrl+C to stop.")
        ->group("Voice");
    s.llm_agent_cmd->add_option(
        "--stt-api-key", s.ag_mic_stt_api_key,
        "API key override for the selected STT provider. "
        "Falls back to the provider entry in App Settings.")
        ->group("Voice");
    s.llm_agent_cmd->add_option(
        "--voice-id", s.ag_mic_voice_id,
        "TTS voice for this mic run. Empty uses Chat settings. "
        "OpenRouter defaults to eve when the saved voice is an ElevenLabs id.")
        ->group("Voice");
    s.llm_agent_cmd->add_flag(
        "--no-tts", s.ag_mic_no_tts,
        "Mic mode: disable TTS playback entirely (keep listening continuously after each response).")
        ->group("Voice");
    s.llm_agent_cmd->add_option(
        "--tts-model-id", s.ag_mic_model_id,
        "TTS model for this mic run. Empty uses Chat settings "
        "(eleven_v3 for ElevenLabs, x-ai/grok-voice-tts-1.0 for OpenRouter).")
        ->group("Voice");
    s.llm_agent_cmd->add_option(
        "--input", s.ag_mic_input,
        "Microphone device name (case-insensitive substring; use `audio info` to list). "
        "Omit to use the system default input device.")
        ->group("Voice");
    s.llm_agent_cmd->add_option(
        "--input-source", s.ag_input_source,
        "Input source for the realtime session: mic (default), desktop (system loopback), "
        "or mix (mic + desktop summed). Use `audio info` to list available devices.")
        ->check(CLI::IsMember({"mic", "desktop", "mix"}, CLI::ignore_case))
        ->default_val("mic")
        ->group("Voice");
    s.llm_agent_cmd->add_option(
        "--desktop", s.ag_desktop_device,
        "Loopback device name substring for --input-source desktop or mix "
        "(case-insensitive; empty = default render device). Sets --input-source desktop "
        "when --input-source is not explicitly provided.")
        ->group("Voice");
    s.llm_agent_cmd->add_option(
        "--mic-gain", s.ag_mic_gain,
        "Mic input level multiplier (linear; 0 = mute, 1 = unity, 2 = double). "
        "Applied before mixing when --input-source mix is set.")
        ->default_val(1.0f)
        ->check(CLI::NonNegativeNumber)
        ->group("Voice");
    s.llm_agent_cmd->add_option(
        "--desktop-gain", s.ag_desktop_gain,
        "Desktop/loopback input level multiplier (linear; 0 = mute, 1 = unity). "
        "Applied before mixing when --input-source desktop or mix is set.")
        ->default_val(1.0f)
        ->check(CLI::NonNegativeNumber)
        ->group("Voice");
    s.llm_agent_cmd->add_option(
        "--silence-ms", s.ag_mic_silence_ms,
        "Silence duration in ms after which speech is auto-committed to the LLM "
        "(0 = disabled; default 1500).")
        ->default_val(1500)
        ->group("Voice");
    // Provider overrides for STT/TTS (apply to --mic, audio_transcribe tool, and
    // any future run that reads chat.stt_* / tts_* via the launch waterfall).
    s.llm_agent_cmd->add_option(
        "--stt-provider", s.ag_stt_provider,
        "Override the STT provider for this run. "
        "Default: from Chat → Voice & Audio settings. "
        "whisper is local whisper.cpp (no API key). "
        "windows is Windows AI speech when the packaged runtime registers it (no API key).")
        ->check(CLI::IsMember({"elevenlabs", "tanit", "openrouter", "whisper", "windows"}, CLI::ignore_case))
        ->group("Voice");
    s.llm_agent_cmd->add_option(
        "--stt-model", s.ag_stt_model,
        "Override the STT model for this run. "
        "whisper: base.en. elevenlabs: scribe_v2_realtime. "
        "openrouter: openai/whisper-large-v3-turbo. Default: from settings.")
        ->group("Voice");
    s.llm_agent_cmd->add_option(
        "--tts-provider", s.ag_tts_provider,
        "Override the TTS provider for this run. Default: from settings.")
        ->check(CLI::IsMember(
            {"elevenlabs", "tanit", "openrouter", "vibevoice", "moss", "windows"}, CLI::ignore_case))
        ->group("Voice");
    s.llm_agent_cmd->add_option(
        "--tts-model", s.ag_tts_model,
        "Override the TTS model for this run "
        "(eleven_v3, x-ai/grok-voice-tts-1.0, …). "
        "Also applies as --tts-model-id when --voice-id is used.")
        ->group("Voice");
    s.llm_agent_cmd->add_option(
        "--tts-voice-id", s.ag_tts_voice_id,
        "Override the TTS voice id for this run. "
        "Alias for --voice-id; wins if both are set.")
        ->group("Voice");
#endif

#if defined(FEATURE_JQ) && FEATURE_JQ
    // ── llm agent decide ──────────────────────────────────────────────────────
    // System One questions on one JSON document. Pair mode is the batch:
    // many questions, one state, chunked by --chunk.
    s.llm_agent_decide_cmd = s.llm_agent_cmd->add_subcommand(
        "decide",
        "Run choice / noul / score questions on a JSON document. "
        "--selector, --pairs, or --left with --right packs each match into one "
        "state and expands each question once per value. That is the batch the "
        "decisions API supports. --chunk caps questions per request, and "
        "--max-tokens caps the estimated body so a large set stays under "
        "Jev's 32k context. One value is never split across requests. "
        "Omit those flags to judge the whole document as one state. "
        "OpenRouter is one provider; jev is the same JSON body on --base-url.");
    s.llm_agent_decide_cmd->add_option("-i,--source", s.ag_decide_source,
        "Input JSON file.")
        ->required()
        ->group("Decide");
    s.llm_agent_decide_cmd->add_option("--questions", s.ag_decide_questions,
        "JSON object of questions, keyed by id. Each value is "
        "{type: choice|noul|score, instructions, criteria?}. "
        "Instructions may name `item` / `item.field` (--selector) or "
        "`pair.left`, `pair.right`, and `pair` (--pairs / --left / --right).")
        ->required()
        ->group("Decide");
    s.llm_agent_decide_cmd->add_option("--selector", s.ag_decide_selector,
        "jq filter selecting values to judge together. Each value becomes "
        "state.items[i]. Object matches are written on the object (id from "
        "a string \"id\", else i0, i1, ...). Other values are written on the parent.")
        ->group("Decide");
    s.llm_agent_decide_cmd->add_option("--pairs", s.ag_decide_pairs,
        "jq filter selecting pair values. Each value is an object with "
        "--left-key/--right-key (default left/right) or a 2-element array. "
        "Answers are written onto that object under --target.")
        ->group("Decide");
    s.llm_agent_decide_cmd->add_option("--left", s.ag_decide_left,
        "jq filter for the left side of each pair. Requires --right. "
        "Answers are written onto the parent object of each left value.")
        ->group("Decide");
    s.llm_agent_decide_cmd->add_option("--right", s.ag_decide_right,
        "jq filter for the right side of each pair. Zipped with --left by match order.")
        ->group("Decide");
    s.llm_agent_decide_cmd->add_option("--left-key", s.ag_decide_left_key,
        "Object field used as the left side when --pairs selects objects.")
        ->default_val("left")
        ->group("Decide");
    s.llm_agent_decide_cmd->add_option("--right-key", s.ag_decide_right_key,
        "Object field used as the right side when --pairs selects objects.")
        ->default_val("right")
        ->group("Decide");
    s.llm_agent_decide_cmd->add_option("--target", s.ag_decide_target,
        "Key that receives the answer object (question id -> answer). "
        "Default: decisions.")
        ->default_val("decisions")
        ->group("Decide");
    s.llm_agent_decide_cmd->add_option("-o,--dst", s.ag_decide_dst,
        "Output path for the full JSON document with answers written in. "
        "Required unless --json.")
        ->group("Decide");
    s.llm_agent_decide_cmd->add_option("--chunk", s.ag_decide_chunk,
        "Max questions per HTTP request when batching selected values (default 32). "
        "One value's questions stay in one request even when that exceeds the cap.")
        ->default_val(32)
        ->check(CLI::Range(1, 4096))
        ->group("Decide");
    s.llm_agent_decide_cmd->add_option("--max-tokens", s.ag_decide_max_tokens,
        "Estimated token ceiling for one request (default 24000, under Jev's 32k). "
        "0 disables the ceiling. Independent values are packed until the next one "
        "would pass this or --chunk. A single value over the ceiling is rejected.")
        ->default_val(24000)
        ->check(CLI::Range(0, 32000))
        ->group("Decide");
    s.llm_agent_decide_cmd->add_flag("--dry-run", s.ag_decide_dry_run,
        "Use the stub provider (no network). Pair objects use left==right "
        "(choice same/different, noul 1 or 0). Other items: choice matches a "
        "criteria key found in the value, noul is 1 when the value is present.")
        ->group("Decide");
    s.llm_agent_decide_cmd->add_flag("--json", s.ag_decide_json,
        "Print machine-readable JSON (stats + full output document) to stdout.")
        ->group("Decide");
    s.llm_agent_decide_cmd->add_option("--provider", s.ag_decide_provider,
        "Decisions provider: openrouter (/api/alpha/decisions), "
        "systemone (/api/v1/systemone on OpenRouter), "
        "jev (same JSON body; requires --base-url), "
        "openai (reserved, not implemented), stub.")
        ->default_val("openrouter")
        ->check(CLI::IsMember({"openrouter", "systemone", "jev", "openai", "stub"}))
        ->group("Provider");
    s.llm_agent_decide_cmd->add_option("--path", s.ag_decide_path,
        "Override the HTTP path for the decisions endpoint.")
        ->group("Provider");
    s.llm_agent_decide_cmd->add_option("--model", s.ag_model,
        "Model id. Default for openrouter/systemone/jev: ~typesafe/jev-latest.")
        ->group("Provider");
    s.llm_agent_decide_cmd->add_option("--api-key", s.ag_api_key,
        "API key (optional for openrouter/systemone; taken from app settings when omitted).")
        ->group("Provider");
    s.llm_agent_decide_cmd->add_option("--base-url", s.ag_base_url,
        "Origin, or an OpenAI-style base (the /api/v1 suffix is stripped). "
        "Required for --provider jev.")
        ->group("Provider");
    s.llm_agent_decide_cmd->add_option("--timeout-ms", s.ag_timeout_ms,
        "HTTP timeout for the decisions call (ms). Default 60000.")
        ->group("Provider");

    // ── llm agent each ────────────────────────────────────────────────────────
    // Iterate string leaves selected by jq and transform them (LLM or dry-run).
    // --dst writes the whole document; --target (optional) writes a sibling key.
    s.llm_agent_each_cmd = s.llm_agent_cmd->add_subcommand(
        "each",
        "Transform selected JSON string fields with a prompt (async iterator). "
        "--source/--dst are files; --selector is a jq filter; "
        "--target is an optional sibling key (omit = in-place); "
        "--merge-json merges a checked JSON object onto the parent (hard overwrite).");
    s.llm_agent_each_cmd->add_option("-i,--source", s.ag_each_source,
        "Input JSON file (e.g. dist/data/commands.json)")
        ->required()
        ->group("Each");
    s.llm_agent_each_cmd->add_option("--selector,--path", s.ag_each_selector,
        "jq filter selecting string leaves to transform "
        "(JSONPath-ish accepted: $.a[*].b -> .a[].b). "
        "Example: .ribbon.groups[].items[].label")
        ->required()
        ->group("Each");
    s.llm_agent_each_cmd->add_option("--target,--as", s.ag_each_target,
        "Optional sibling field name for the transformed value on the parent object. "
        "Omit to overwrite the selected leaf in place. "
        "Ignored when --merge-json is set. "
        "Example: --selector '.items[].label' --target label_de -> writes items[i].label_de")
        ->group("Each");
    s.llm_agent_each_cmd->add_flag("--merge-json", s.ag_each_merge_json,
        "Expect a JSON object from the transformer/LLM (checked; optional ```json fences ok). "
        "Merge object keys onto the selected leaf's parent. "
        "HARD OVERWRITE: existing keys with the same name are replaced (dev-tool semantics). "
        "Use for multi-language replies in one shot, e.g. "
        "{\"label_de\":\"…\",\"label_fr\":\"…\"}. Incompatible with --target.")
        ->group("Each");
    s.llm_agent_each_cmd->add_option("--merge-keys", s.ag_each_merge_keys,
        "With --merge-json: require these object keys as non-empty strings before merge. "
        "Missing or empty keys retry the leaf (--max-retries). Comma list, e.g. "
        "label_de,label_es,label_it,label_fr. Omit to accept any non-empty object.")
        ->delimiter(',')
        ->group("Each");
    s.llm_agent_each_cmd->add_option("-o,--dst", s.ag_each_dst,
        "Output path for the full mutated JSON document (not a single field). "
        "Required unless --json (stdout payload includes output).")
        ->group("Each");
    s.llm_agent_each_cmd->add_option("-p,--prompt", s.ag_each_prompt,
        "Prompt applied to each selected value "
        "(shaped as: <prompt>\\n\\nText to transform: \"…\"). "
        "Required unless --dry-run. With --merge-json, ask for a JSON object only.")
        ->group("Each");
    s.llm_agent_each_cmd->add_flag("--dry-run", s.ag_each_dry_run,
        "Skip live LLM; use --transform (default upper) for offline/fixture runs.")
        ->group("Each");
    s.llm_agent_each_cmd->add_option("--transform", s.ag_each_transform,
        "Dry-run transformer: upper | prefix | identity | llm-stub | json-stub "
        "(json-stub emits {label_de,label_fr} for --merge-json tests)")
        ->default_val("upper")
        ->check(CLI::IsMember({"upper", "prefix", "identity", "llm-stub", "json-stub"}))
        ->group("Each");
    s.llm_agent_each_cmd->add_option("--prefix", s.ag_each_prefix,
        "Prefix string when --transform prefix")
        ->default_val("x")
        ->group("Each");
    s.llm_agent_each_cmd->add_option("--concurrency", s.ag_each_concurrency,
        "Max concurrent leaf transforms (0 = auto: 1 dry-run, 4 live)")
        ->default_val(0)
        ->check(CLI::Range(0, 64))
        ->group("Each");
    s.llm_agent_each_cmd->add_option("--max-retries", s.ag_each_max_retries,
        "Per-leaf retries on transform failure (0 = auto: 1 dry-run, 3 live)")
        ->default_val(0)
        ->check(CLI::Range(0, 16))
        ->group("Each");
    s.llm_agent_each_cmd->add_option("--throttle-ms", s.ag_each_throttle_ms,
        "Minimum delay between transform starts (rate limit)")
        ->default_val(0)
        ->check(CLI::Range(0, 60'000))
        ->group("Each");
    s.llm_agent_each_cmd->add_flag("--no-cache", s.ag_each_no_cache,
        "Disable identical-input result cache (default: cache on).")
        ->group("Each");
    s.llm_agent_each_cmd->add_flag("--json", s.ag_each_json,
        "Print machine-readable JSON (stats + full output document) to stdout.")
        ->group("Each");
    s.llm_agent_each_cmd->add_option("--abort-after-consecutive-errors", s.ag_each_abort_consecutive,
        "Abort the run after N consecutive leaf failures (0 = off). "
        "A run that keeps failing is unlikely to recover — the model is "
        "probably unsuited for the prompt/shape.")
        ->default_val(0)
        ->check(CLI::Range(0, 1024))
        ->group("Each");
    s.llm_agent_each_cmd->add_flag("--partial-commit", s.ag_each_partial_commit,
        "On abort, write --dst with the successfully transformed leaves "
        "merged (failed/unprocessed left as-is). Makes an aborted run "
        "resumable via --only-missing. No effect without "
        "--abort-after-consecutive-errors.")
        ->group("Each");
    // Provider knobs bind the same PmImageCliState fields as parent `llm agent`.
    // Re-register here so `… each --router …` works: root allow_extras() would
    // otherwise swallow unknown flags after the subcommand without binding them.
    // Parent-before form still works: `llm agent --router openai each …`.
    s.llm_agent_each_cmd->add_option("--preset", s.ag_preset,
        "Chat settings preset name or id (same as parent llm agent).")
        ->group("Provider");
    s.llm_agent_each_cmd->add_option("--router", s.ag_router,
        "LLM router (llama|openrouter|openai|…). Default: Chat Provider Settings.")
        ->group("Provider");
    s.llm_agent_each_cmd->add_option("--model", s.ag_model,
        "Model id (router-specific). Default: Chat Provider Settings.")
        ->group("Provider");
    s.llm_agent_each_cmd->add_option("--api-key", s.ag_api_key,
        "API key (optional; default from app chat / API Keys).")
        ->group("Provider");
    s.llm_agent_each_cmd->add_option("--base-url", s.ag_base_url,
        "Override the router's default base URL (OpenAI-compatible).")
        ->group("Provider");
    s.llm_agent_each_cmd->add_option("--api-mode", s.ag_each_api_mode,
        "API mode for leaf transforms: completion|responses|realtime. "
        "Default: completion (/chat/completions). Non-OpenAI-backed tanit "
        "aliases are downgraded off responses (see resolve_agent_launch).")
        ->group("Provider")
        ->default_val("completion")
        ->check(CLI::IsMember({"completion", "responses", "realtime"}));
    s.llm_agent_each_cmd->add_option("--timeout-ms", s.ag_timeout_ms,
        "HTTP timeout per LLM round (ms). Default: Chat Provider Settings (or 60000).")
        ->group("Provider");

#if defined(FEATURE_LLAMA) && FEATURE_LLAMA
    // ── llm agent dedupe ──────────────────────────────────────────────────────
    s.llm_agent_dedupe_cmd = s.llm_agent_cmd->add_subcommand(
        "dedupe",
        "Cluster jq-selected string leaves by local embedding cosine similarity. "
        "Annotates duplicates with keep/duplicateOf/clusterId; optional keptTexts output.");
    s.llm_agent_dedupe_cmd->add_option("-i,--source", s.ag_dedupe_source,
        "Input JSON file (e.g. changelog leaf bundle)")
        ->required()
        ->group("Dedupe");
    s.llm_agent_dedupe_cmd->add_option("--selector,--path", s.ag_dedupe_selector,
        "jq filter selecting string leaves to compare (e.g. .items[].text)")
        ->required()
        ->group("Dedupe");
    s.llm_agent_dedupe_cmd->add_option("--embed-model,-m", s.ag_dedupe_embed_model,
        "Path to GGUF embedding model (e.g. ${MODELS_DIR}/all-MiniLM-L6-v2.Q5_K_M.gguf).")
        ->required()
        ->group("Dedupe");
    s.llm_agent_dedupe_cmd->add_option("-o,--dst", s.ag_dedupe_dst,
        "Output JSON report path (required unless --json).")
        ->group("Dedupe");
    s.llm_agent_dedupe_cmd->add_option("--threshold", s.ag_dedupe_threshold,
        "Cosine similarity threshold to treat two entries as duplicates (default 0.78).")
        ->default_val(0.78f)
        ->check(CLI::Range(0.0f, 1.0f))
        ->group("Dedupe");
    s.llm_agent_dedupe_cmd->add_option("--margin", s.ag_dedupe_margin,
        "Required lead over the runner-up cluster score (default 0.05).")
        ->default_val(0.05f)
        ->check(CLI::Range(0.0f, 1.0f))
        ->group("Dedupe");
    s.llm_agent_dedupe_cmd->add_option("-j,--threads", s.ag_dedupe_threads,
        "CPU threads for embedding inference (default 4).")
        ->default_val(4)
        ->check(CLI::Range(1, 256))
        ->group("Dedupe");
    s.llm_agent_dedupe_cmd->add_option("--ctx", s.ag_dedupe_ctx,
        "Embedding model context length (default 512).")
        ->default_val(512)
        ->check(CLI::Range(32, 8192))
        ->group("Dedupe");
    s.llm_agent_dedupe_cmd->add_flag("--drop", s.ag_dedupe_drop,
        "Include output.keptTexts in the report (kept strings only).")
        ->group("Dedupe");
    s.llm_agent_dedupe_cmd->add_flag("--json", s.ag_dedupe_json,
        "Print the full JSON report to stdout.")
        ->group("Dedupe");
#endif
#endif

    pm_image_register_llm_external_agents(app, s);
}

int pm_image_dispatch_llm(CLI::App& app, PmImageCliState& st,
                          pm::cli::CliActivityScope* cli_activity)
{
    if (st.llm_list_cmd && st.llm_list_cmd->parsed())
        return pm_image_cmd_llm_list(app, st);
    if (st.llm_call_cmd && st.llm_call_cmd->parsed())
        return pm_image_cmd_llm_call(app, st);
    if (st.llm_info_cmd && st.llm_info_cmd->parsed())
        return pm_image_cmd_llm_info(app, st);
    if (st.llm_agent_cmd && st.llm_agent_cmd->parsed())
        return pm_image_cmd_llm_agent(app, st, cli_activity);
#if FEATURE_AGENT_CODEX
    if (st.llm_codex_cmd && st.llm_codex_cmd->parsed())
        return pm_image_cmd_llm_codex(app, st);
#endif
#if FEATURE_AGENT_CLAUDE
    if (st.llm_claude_cmd && st.llm_claude_cmd->parsed())
        return pm_image_cmd_llm_claude(app, st);
#endif
#if 0 // Cursor runner incomplete; keep for later
#if FEATURE_AGENT_CURSOR
    if (st.llm_cursor_cmd && st.llm_cursor_cmd->parsed())
        return pm_image_cmd_llm_cursor(app, st);
#endif
#endif
    std::cerr << "llm: no subcommand\n";
    return 2;
}

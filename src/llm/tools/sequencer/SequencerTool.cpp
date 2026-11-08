// Copyright PolyMech / pm-image. Internal use only.
#include "SequencerTool.hpp"

#include "llm/tools/commons/PathToolCommons.hpp"
#include "core/cli_cancel.hpp"
#include "xblox_commands.hpp"
#include "utils/conv.hpp"
#include "logging/logger.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "fs/path_sanitizer.hpp"

namespace media::llm::path::detail {

namespace {

// Summarise the xblox event list into a compact representation the LLM can
// read without drowning in boilerplate.  Each entry keeps the step index
// (path), kind, status, and a short message or the stdout/stderr lines when
// the step failed.
nlohmann::json summarise_events(const std::vector<media::xblox::ExecutionEvent>& events)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& ev : events) {
        if (ev.status == "running") continue; // skip "about to execute" noise
        nlohmann::json entry = {
            {"step",    ev.path},
            {"kind",    ev.kind},
            {"status",  ev.status},
            {"message", ev.message},
        };
        if (!ev.stdout_lines.empty())
            entry["stdout"] = ev.stdout_lines;
        if (!ev.stderr_lines.empty() && ev.status != "ok")
            entry["stderr"] = ev.stderr_lines;
        if (ev.exit_code != 0)
            entry["exit_code"] = ev.exit_code;
        arr.push_back(std::move(entry));
    }
    return arr;
}

// Write two artefacts into `cwd` after execution:
//
//   run_sequence_<unix_ms>.xblox
//     Proper xblox document — same format as hand-authored .xblox files.
//     Runnable via `pm-image xblox run --src <file>`, editable, repeatable.
//     Contains only { version, context, roots } — no execution noise.
//
//   run_sequence_<unix_ms>_events.json
//     Execution log — per-step status, stdout/stderr, exit codes.
//     Helpful for post-mortem debugging; not needed to re-run the sequence.
//
// Returns the stem timestamp so the caller can surface the paths.
std::string write_sequence_artifacts(const std::filesystem::path& cwd,
                                     const nlohmann::json& roots,
                                     const nlohmann::json& context,
                                     const media::xblox::ExecutionResult& xr)
{
    if (cwd.empty()) return {};
    namespace chr = std::chrono;
    const auto now_ms = chr::duration_cast<chr::milliseconds>(
        chr::system_clock::now().time_since_epoch()).count();
    const std::string stem = "run_sequence_" + std::to_string(now_ms);

    std::error_code ec;
    std::filesystem::create_directories(cwd, ec);

    // ── 1. Proper .xblox document ─────────────────────────────────────────
    const nlohmann::json xblox_doc = {
        {"version", 1},
        {"context", context.is_object() ? context : nlohmann::json::object()},
        {"roots",   roots},
    };
    {
        std::ofstream f(cwd / (stem + ".xblox"), std::ios::binary);
        if (f) f << xblox_doc.dump(2) << "\n";
    }

    // ── 2. Execution event log ────────────────────────────────────────────
    nlohmann::json events_j = nlohmann::json::array();
    for (const auto& ev : xr.events)
        events_j.push_back(media::xblox::conv::event_to_json(ev));

    const nlohmann::json events_doc = {
        {"xblox_file", stem + ".xblox"},
        {"result", {
            {"ok",          xr.ok},
            {"exit_code",   xr.exit_code},
            {"event_count", static_cast<int>(xr.event_count)},
            {"error_count", static_cast<int>(xr.error_count)},
        }},
        {"events", events_j},
    };
    {
        std::ofstream f(cwd / (stem + "_events.json"), std::ios::binary);
        if (f) f << events_doc.dump(2) << "\n";
    }

    return stem;
}

} // namespace

ExecuteResult do_run_sequence(const nlohmann::json& args)
{
    if (!args.contains("steps"))
        return simple_error("run_sequence: 'steps' (array) is required");

    // LLMs occasionally JSON-stringify the array instead of passing it inline.
    // Accept both the proper array and a string that parses to one.
    nlohmann::json roots_storage;
    if (args["steps"].is_string()) {
        try {
            roots_storage = nlohmann::json::parse(args["steps"].get<std::string>());
        } catch (...) {
            return simple_error("run_sequence: 'steps' is a string that could not be parsed as JSON array");
        }
        if (!roots_storage.is_array())
            return simple_error("run_sequence: 'steps' string did not parse to an array");
    } else if (args["steps"].is_array()) {
        roots_storage = args["steps"];
    } else {
        return simple_error("run_sequence: 'steps' must be a JSON array");
    }
    const nlohmann::json& roots = roots_storage;
    const nlohmann::json  context = args.contains("context") && args["context"].is_object()
                                    ? args["context"]
                                    : nlohmann::json::object();

    if (roots.empty())
        return simple_error("run_sequence: 'steps' must not be empty");

    media::xblox::ExecutionOptions opts;
    opts.default_cwd            = agent_path_base_empty() ? std::filesystem::path{} : media::path::path_from_utf8(agent_path_base());
    opts.execute_external_commands = true;
    opts.wait_blocks            = true;
    opts.collect_events         = true;
    opts.max_loop_iterations    = 1; // loops not exposed to LLM for safety
    // Agent sequences frequently chain slow AI operations (image transform /
    // create hit Replicate/Gemini and routinely take 30-90s each). xblox's
    // 30s shell default would TerminateProcess them mid-flight (exit 124),
    // producing no output file and no captured path. Give sequenced steps a
    // generous default; individual steps can still override via `timeoutMs`.
    opts.default_timeout_ms     = 300000; // 5 min per step

    // Wire cancellation so Ctrl-C / agent stop propagates into waiting steps.
    opts.cancel_requested = [] { return media::cli::cancel_requested(); };

    // App / ribbon / URL / path actions are not reachable from the LLM-facing
    // schema (only `shell` and `wait` and `log` are described).  Stub them
    // out so any accidentally submitted `command` block doesn't crash.
    auto noop = [](const media::xblox::CommandInvocation&, std::string* m) {
        if (m) *m = "stubbed";
        return 0;
    };
    opts.app_command_handler     = noop;
    opts.ribbon_command_handler  = noop;
    opts.open_url_handler        = noop;
    opts.open_path_handler       = noop;

    const std::filesystem::path debug_dir = agent_path_base_empty()
        ? std::filesystem::path{}
        : media::path::path_from_utf8(agent_path_base());

    logger::info("[run_sequence] executing " + std::to_string(roots.size()) + " step(s)");
    const auto xr = media::xblox::run_block_roots(roots, context, opts);

    const std::string artifact_stem = write_sequence_artifacts(debug_dir, roots, context, xr);

    // Collect the final context (all storeAs-captured variables) so the agent
    // can reference output paths in follow-up tool calls.
    nlohmann::json captured_context = nlohmann::json::object();
    // run_block_roots works on a mutable copy of context; we read back the
    // stored values from the event data (each command event carries the stored
    // result under the target key).
    for (const auto& ev : xr.events) {
        if (ev.kind == "command" && ev.status == "ok" && ev.data.is_object()) {
            const auto& d = ev.data;
            // The executor stores the result under the key named in storeAs /
            // target; we surface the stdout_lines as the primary captured value.
            if (d.contains("id") && d["id"].is_string() && !d["id"].get<std::string>().empty()) {
                const std::string id = d["id"].get<std::string>();
                if (d.contains("stdout") && d["stdout"].is_array() && !d["stdout"].empty())
                    captured_context[id] = d["stdout"];
            }
        }
    }

    nlohmann::json payload = {
        {"ok",           xr.ok},
        {"step_count",   static_cast<int>(roots.size())},
        {"error_count",  static_cast<int>(xr.error_count)},
        {"context",      captured_context},
        {"steps",        summarise_events(xr.events)},
    };
    if (!artifact_stem.empty()) {
        const std::string base = media::path::path_to_utf8(debug_dir);
        payload["xblox_file"]  = base + "/" + artifact_stem + ".xblox";
        payload["events_file"] = base + "/" + artifact_stem + "_events.json";
    }

    if (!xr.ok)
        logger::warn("[run_sequence] finished with " + std::to_string(xr.error_count) + " error(s)");
    else
        logger::info("[run_sequence] all steps ok");

    return simple_ok(payload);
}

nlohmann::json parse_schema(const char* schema)
{
    return nlohmann::json::parse(schema);
}

ToolDef make_run_sequence()
{
    return ToolDef{
        "run_sequence",

        "Execute a deterministic sequence of shell commands and timed waits "
        "without spending extra LLM round-trips between steps.  Use this "
        "whenever you need to chain multiple CLI operations with precise "
        "timing — for example: speak a cue, capture a camera frame, wait "
        "N seconds, repeat — without spawning a child agent.\n\n"
        "Each element of `steps` is one of:\n"
        "  shell — run a CLI command string in the system shell (PowerShell "
        "on Windows, /bin/sh on Unix).  Set `storeAs` to a variable name to "
        "capture stdout lines for later reference.  Set `background: true` "
        "to fire-and-forget (does not wait for completion).\n"
        "    {\"kind\": \"shell\", \"command\": \"pm-image video image --dst cam_001.jpg\", \"storeAs\": \"cap1\"}\n\n"
        "  wait — sleep for the given number of milliseconds (respects "
        "user cancel).\n"
        "    {\"kind\": \"wait\", \"ms\": 5000}\n\n"
        "  log  — emit a trace message (does not block).\n"
        "    {\"kind\": \"log\", \"level\": \"info\", \"message\": \"step 1 done\"}\n\n"
        "All steps run sequentially in C++ with cancel support.  The result "
        "contains `ok` (bool), `context` (storeAs captures), `steps` "
        "(per-step status / stdout), and `xblox_file` (path to the saved "
        ".xblox file — re-runnable via `pm-image xblox run --src <path>`).\n\n"
        "Use the returned context paths in subsequent image_transform, "
        "file_read, or other tool calls.\n\n"
        "TIMING ACCURACY: `wait` blocks are wall-clock precise (C++ "
        "sleep_for) — suitable for photo-burst intervals, countdown "
        "announcements, or any sequence where millisecond accuracy matters.\n\n"
        "DISCOVER CLI COMMANDS: before building shell steps, call "
        "info_lookup with kind=\"command\" and the relevant sub-command name "
        "(e.g. query=\"video image\" to get the camera still-capture syntax, "
        "query=\"audio tts\" for TTS, query=\"transform\" for image transform). "
        "The full flag surface is returned; embed the resolved command string "
        "directly into the shell step.\n\n"
        "PREFER ONE SEQUENCE END-TO-END: chain the whole job here rather than "
        "splitting capture and post-processing across separate LLM rounds. AI "
        "image steps are first-class shell commands: "
        "`pm-image transform <in> <out> -p \"<style>\" --json` edits an image "
        "and prints the new path under outputs[].path (add `--json` so you can "
        "parse the result); `pm-image create -p \"...\" <out> --json` generates "
        "one. A timed photo-to-sketch burst is therefore a single run_sequence: "
        "capture -> wait -> capture -> transform -> transform. Slow AI steps "
        "default to a 5-min timeout, so they will not be killed mid-flight.",

        parse_schema(R"json({
            "type": "object",
            "properties": {
                "steps": {
                    "type": "array",
                    "minItems": 1,
                    "description": "Ordered list of steps to execute. Each step must have a `kind` field. Supported kinds: \"shell\", \"wait\", \"log\".",
                    "items": {
                        "type": "object",
                        "properties": {
                            "kind": {
                                "type": "string",
                                "enum": ["shell", "wait", "log"],
                                "description": "\"shell\" - run a command. \"wait\" - sleep ms. \"log\" - trace message."
                            },
                            "command": {
                                "type": "string",
                                "description": "(shell only) Full command string, e.g. \"pm-image video image --dst C:/work/cam_001.jpg\". Use absolute paths where possible. Runs in PowerShell on Windows, /bin/sh on Unix."
                            },
                            "ms": {
                                "type": "integer",
                                "minimum": 0,
                                "maximum": 300000,
                                "description": "(wait only) Milliseconds to sleep. 5000 = 5 seconds."
                            },
                            "message": {
                                "type": "string",
                                "description": "(log only) Human-readable trace message."
                            },
                            "level": {
                                "type": "string",
                                "enum": ["trace", "debug", "info", "warn", "error"],
                                "description": "(log only) Log level. Defaults to \"info\"."
                            },
                            "storeAs": {
                                "type": "string",
                                "description": "(shell only) Variable name under which stdout lines are captured and returned in the result `context` map. Use to carry output paths into follow-up tool calls."
                            },
                            "background": {
                                "type": "boolean",
                                "description": "(shell only) Fire-and-forget: launch without waiting for completion. Useful for starting a recorder before a timed wait. Defaults to false."
                            },
                            "timeoutMs": {
                                "type": "integer",
                                "minimum": 1000,
                                "maximum": 600000,
                                "description": "(shell only) Max milliseconds to wait for this command. Defaults to 300000 (5 min). Raise for slow AI steps such as `pm-image transform`/`create` (Replicate/Gemini calls take 30-90s each)."
                            }
                        },
                        "required": ["kind"]
                    }
                },
                "context": {
                    "type": "object",
                    "description": "Optional initial key-value context passed to the first step. Use to inject dynamic paths or flags resolved before calling this tool."
                }
            },
            "required": ["steps"]
        })json")
    };
}

} // namespace media::llm::path::detail

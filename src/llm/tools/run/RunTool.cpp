#include "RunTool.hpp"

#include "constants.hpp"
#include "core/app_exe_directory.hpp"
#include "core/cli_cancel.hpp"
#include "process/process_runner.hpp"
#include "core/settings_store.hpp"
#include "ui_defaults.hpp"
#include "fs/fs_guard.hpp"
#include "llm/llm_constants.hpp"
#include "llm/tools/run/run_capture.hpp"
#include "logging/pm_log.hpp"
#include "pm_branding_config.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"
#include <string>

#if defined(_WIN32)
#include "security/security_default.hpp"
#if FEATURE_SECURITY_AGENT && FEATURE_SECURITY_SANDBOX
#include "security/backends/win/SandboxAppContainer.hpp"
#define PM_RUN_TOOL_HAS_APPCONTAINER 1
#endif
#endif

namespace fs = std::filesystem;

namespace media::llm::run {

// ═══════════════════════════════════════════════════════════════════════════════
// §1  Streaming output hook (set by agent_factory; forwarded to ProcessRunner)
// ═══════════════════════════════════════════════════════════════════════════════

static std::mutex       g_hook_mtx;
static OutputChunkHook  g_chunk_hook;

void set_output_chunk_hook(OutputChunkHook hook) {
    std::lock_guard<std::mutex> lk(g_hook_mtx);
    g_chunk_hook = std::move(hook);
}

void clear_output_chunk_hook() {
    std::lock_guard<std::mutex> lk(g_hook_mtx);
    g_chunk_hook = nullptr;
}

namespace {

constexpr const char k_ch[] = "run_tool";
constexpr int    k_timeout_default_ms = 60000;   // raised from 30s — AI ops routinely need >30s
constexpr int    k_timeout_max_ms     = 300000;  // raised to 5 min — video gen / slow Replicate

using media::strutil::lower_ascii;

std::string normalize_nested_agent_command(std::string command) {
    // Branded CLI basename/stem from branding constants (already lowercase; lowered anyway).
    const std::string cli_exe  = lower_ascii(std::string(pm::brand::k_exe_basename_cli_u8));
    const std::string cli_stem = lower_ascii(std::string(pm::brand::k_exe_stem_cli_u8));

    std::string low = lower_ascii(command);
    std::size_t exe_pos = low.find(cli_exe + " llm agent");
    std::size_t exe_len = cli_exe.size();
    if (exe_pos == std::string::npos) {
        exe_pos = low.find(cli_stem + " llm agent");
        exe_len = cli_stem.size();
    }
    if (exe_pos == std::string::npos)
        return command;

    const std::string agent_needle = "llm agent";
    std::size_t agent_pos = low.find(agent_needle, exe_pos);
    if (agent_pos == std::string::npos)
        return command;

    const bool has_top_level_log_level = low.substr(0, agent_pos).find("--log-level") != std::string::npos;
    std::size_t misplaced_log_level = low.find("--log-level", agent_pos + agent_needle.size());
    if (misplaced_log_level != std::string::npos) {
        std::size_t erase_end = command.find_first_not_of(" \t", misplaced_log_level + std::string("--log-level").size());
        if (erase_end != std::string::npos)
            erase_end = command.find_first_of(" \t", erase_end);
        if (erase_end == std::string::npos)
            erase_end = command.size();
        command.erase(misplaced_log_level, erase_end - misplaced_log_level);
        low = lower_ascii(command);
    }

    if (!has_top_level_log_level) {
        command.insert(exe_pos + exe_len, " --log-level error");
        low = lower_ascii(command);
        agent_pos = low.find(agent_needle, exe_pos);
        if (agent_pos == std::string::npos)
            return command;
    }

    std::string extra;
    if (!extra.empty())
        command.insert(agent_pos + agent_needle.size(), extra);
    return command;
}

std::string log_preview(std::string_view text, std::size_t max_chars = 160) {
    std::string out;
    out.reserve(std::min<std::size_t>(text.size(), max_chars) + 8);
    std::size_t count = 0;
    for (char ch : text) {
        if (count >= max_chars) {
            out += "...";
            break;
        }
        switch (ch) {
        case '\r': out += "\\r"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        default:   out.push_back(ch); break;
        }
        ++count;
    }
    return out;
}

bool truthy_env(const char* name) {
    const char* v = std::getenv(name);
    if (!v) return false;
    const std::string s = lower_ascii(v);
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

// Whether this run must be offloaded to the AppContainer sandbox. In the agent
// path the SecurityGateway is authoritative: it injects args["sandbox"] from
// PolicyDecision::sandboxBackend (Phase B routing). The env var / explicit flag
// remain a direct-invocation seam for the CLI tools-call path and tests.
bool wants_sandbox_offload(const nlohmann::json& args) {
    if (args.contains("sandbox")) {
        const auto& v = args.at("sandbox");
        if (v.is_boolean()) return v.get<bool>();
        if (v.is_string()) {
            const std::string s = lower_ascii(v.get<std::string>());
            return s == "1" || s == "true" || s == "yes" || s == "on"
                || s == "appcontainer";
        }
    }
    return truthy_env("PM_RUN_SANDBOX");
}

std::string sandbox_moniker_for_run(const std::string& cwd) {
    const auto h = std::hash<std::string>{}(cwd.empty() ? std::string{"default"} : cwd);
    return "pm-run-tool-" + std::to_string(static_cast<unsigned long long>(h));
}

std::string normalize_sandbox_path_for_run(const std::string& path, const std::string& cwd) {
    if (path.empty()) return {};
    std::error_code ec;
    fs::path p(path);
    if (p.is_relative())
        p = cwd.empty() ? fs::absolute(p, ec) : media::path::path_from_utf8(cwd) / p;
    if (ec) return media::path::path_to_utf8(p);
    fs::path norm = fs::weakly_canonical(p, ec);
    if (ec) {
        ec.clear();
        norm = fs::absolute(p, ec);
    }
    return media::path::path_to_utf8((ec ? p : norm));
}

media::llm::path::ExecuteResult fatal(const std::string& msg) {
    media::llm::path::ExecuteResult r;
    r.ok = false;
    r.error = msg;
    r.envelope = nlohmann::json{{"ok", false}, {"error", msg}};
    return r;
}

const char* validation_tier_token(ValidationTier tier) noexcept {
    switch (tier) {
        case ValidationTier::Allow:      return "allow";
        case ValidationTier::Warn:       return "warn";
        case ValidationTier::WarnStrict: return "warn_strict";
        case ValidationTier::Deny:       return "deny";
    }
    return "allow";
}

const char* validation_risk_token(ValidationTier tier) noexcept {
    switch (tier) {
        case ValidationTier::Allow:      return "low";
        case ValidationTier::Warn:       return "medium";
        case ValidationTier::WarnStrict: return "high";
        case ValidationTier::Deny:       return "critical";
    }
    return "low";
}

nlohmann::json run_security_audit_json(const ValidationResult& vr) {
    return {
        {"required", vr.tier != ValidationTier::Allow},
        {"scope", "run.validation"},
        {"tier", validation_tier_token(vr.tier)},
        {"risk", validation_risk_token(vr.tier)},
        {"reason", vr.reason},
    };
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════════
// §2  Public API — detect_shell (delegates to process_helpers)
// ═══════════════════════════════════════════════════════════════════════════════

ShellSpec detect_shell(const std::string& hint) {
    auto s = media::process::detect_shell(hint);
    return ShellSpec{ std::move(s.exe), std::move(s.flag), s.is_pwsh };
}

// ═══════════════════════════════════════════════════════════════════════════════
// §3  Public API — execute (the full do_run lifecycle via ProcessRunner)
// ═══════════════════════════════════════════════════════════════════════════════

media::llm::path::ExecuteResult
execute(const nlohmann::json& args,
        const std::string& agent_path_base) {
    namespace chr = std::chrono;

    std::string command = normalize_nested_agent_command(args.value("command", std::string{}));
    const std::string shell_hint = args.value("shell", std::string{"auto"});

    int timeout_ms = k_timeout_default_ms;
    if (args.contains("timeout_ms")) {
        const auto& v = args.at("timeout_ms");
        if (v.is_number_integer())      timeout_ms = v.get<int>();
        else if (v.is_number_float())   timeout_ms = static_cast<int>(v.get<double>());
        else if (v.is_string()) {
            try { timeout_ms = std::stoi(v.get<std::string>()); }
            catch (...) { timeout_ms = k_timeout_default_ms; }
        }
    }
    if (timeout_ms < 1)              timeout_ms = k_timeout_default_ms;
    if (timeout_ms > k_timeout_max_ms) timeout_ms = k_timeout_max_ms;

    // ── 1. Validate ──
    const auto vr = validate_command(command, shell_hint);
    if (vr.tier == ValidationTier::Deny) {
        pm::log::warn_lazy(k_ch, [&] { return "deny: " + vr.reason + " cmd=\"" + command.substr(0, 80) + "\""; });
        auto r = fatal("run: blocked \xe2\x80\x94 " + vr.reason);
        r.envelope["security_audit"] = run_security_audit_json(vr);
        return r;
    }
    if (vr.tier == ValidationTier::WarnStrict && pm::llm::LLM_RUN_STRICT) {
        pm::log::warn_lazy(k_ch, [&] { return "warn-strict blocked: " + vr.reason + " cmd=\"" + command.substr(0, 80) + "\""; });
        auto r = fatal("run: blocked \xe2\x80\x94 " + vr.reason);
        r.envelope["security_audit"] = run_security_audit_json(vr);
        return r;
    }
    if (vr.tier == ValidationTier::Warn || vr.tier == ValidationTier::WarnStrict) {
        pm::log::warn_lazy(k_ch, [&] { return "warn (allowed): " + vr.reason; });
    }

    // ── 2. CWD ──
    // A pinned agent folder wins. Otherwise the command runs in this process's
    // working directory (the CLI spawn cwd). The saved UI work folder is only
    // a last resort — granting that tree would miss the caller's directory.
    std::string cwd = agent_path_base;
    if (cwd.empty()) {
        std::error_code cwd_ec;
        const fs::path cur = fs::current_path(cwd_ec);
        if (!cwd_ec && !cur.empty())
            cwd = media::path::path_to_utf8(cur);
        else
            cwd = media::path::path_to_utf8(media::ui_defaults::fallback_work_folder());
    }

    if (!cwd.empty()) {
        const std::string deny = media::llm::llm_fs_guard_deny_reason(media::path::path_from_utf8(cwd));
        if (!deny.empty())
            return fatal("run: cwd blocked \xe2\x80\x94 " + deny);
    }

#if PM_RUN_TOOL_HAS_APPCONTAINER
    if (wants_sandbox_offload(args)) {
        const ShellSpec shell = detect_shell(shell_hint);
        pm::security::SandboxSpawnOptions sb;
        sb.moniker   = sandbox_moniker_for_run(cwd);
        sb.run       = shell.exe;
        sb.args      = {shell.flag, command};
        sb.cwd       = cwd;
        sb.timeoutMs = static_cast<uint32_t>(timeout_ms);
        sb.retain    = false;

        // Map a per-entry "propagation" string to the enum. Explicit grants
        // default to ImmediateFiles so existing files in the folder are readable.
        const auto parse_propagation = [](const nlohmann::json& entry) {
            const std::string s = entry.value("propagation", std::string{"immediate"});
            if (s == "node")      return pm::security::GrantPropagation::NodeOnly;
            if (s == "recursive") return pm::security::GrantPropagation::Recursive;
            return pm::security::GrantPropagation::ImmediateFiles;
        };

        // Dedupe across all grant sources (explicit args, cwd, user settings).
        // Critical: a user-configured sandbox path that resolves to the cwd must
        // NOT be granted again with an inheriting mode — that would re-stamp the
        // whole workspace subtree (O(files), tens of seconds on a repo root) on
        // top of the O(1) NodeOnly cwd grant. Windows paths compare case-
        // insensitively.
        std::set<std::string> seen;
        const auto key_of = [](std::string s) {
            return media::strutil::lower_ascii(std::move(s));
        };
        const auto add_grant = [&](const std::string& p, const std::string& access,
                                   pm::security::GrantPropagation prop) {
            if (p.empty() || access == "forbidden") return false;
            if (!seen.insert(key_of(p)).second) return false;  // duplicate
            sb.paths.push_back({p, access, prop, false});
            return true;
        };

        bool has_explicit_paths = false;
        if (args.contains("sandbox_paths") && args["sandbox_paths"].is_array()) {
            for (const auto& entry : args["sandbox_paths"]) {
                if (!entry.is_object()) continue;
                const std::string p = normalize_sandbox_path_for_run(
                    entry.value("path", std::string{}), cwd);
                const std::string m = entry.value("access", entry.value("mode", std::string{"read"}));
                if (add_grant(p, m, parse_propagation(entry)))
                    has_explicit_paths = true;
            }
        }
        // The cwd/workspace auto-grant is NodeOnly: O(1) and never triggers a
        // subtree ACL re-propagation on large workspaces. It still allows the
        // command to list/traverse the dir and create/write new files (e.g.
        // `dir *.md > dir.md`); reading pre-existing files requires an explicit
        // sandbox_paths grant. Added before user paths so a user entry equal to
        // the cwd is deduped away (keeping the fast NodeOnly grant).
        if (!cwd.empty())
            add_grant(normalize_sandbox_path_for_run(cwd, cwd),
                      has_explicit_paths ? "read" : "read-write",
                      pm::security::GrantPropagation::NodeOnly);

        // User-configured sandbox path policy (settings > Security > Sandbox paths).
        // Each entry is {path, mode, propagation?}. These are explicit, so they
        // default to ImmediateFiles (existing files readable) — but an entry that
        // collides with the cwd is skipped by the dedupe above.
        {
            nlohmann::json user_paths_arr = nlohmann::json::array();
            std::string sp_err;
            media::settings::load_subtree("security.sandboxUserPaths", user_paths_arr, sp_err);
            for (const auto& entry : user_paths_arr) {
                if (!entry.is_object()) continue;
                const std::string p = normalize_sandbox_path_for_run(
                    entry.value("path", std::string{}), cwd);
                const std::string m = entry.value("mode", std::string{"read-write"});
                add_grant(p, m, parse_propagation(entry));
            }
        }

        // Lifecycle parity with the ProcessRunner path: stream output live and
        // honour cooperative cancellation (UI Stop button / Ctrl-C set this flag).
        sb.onOutputChunk = [](const std::string& stream, const std::string& chunk) {
            std::lock_guard<std::mutex> lk(g_hook_mtx);
            if (g_chunk_hook) g_chunk_hook(stream, chunk);
        };
        sb.onCancelCheck = [] { return media::cli::cancel_requested(); };

        const auto search_dirs = media::ui_defaults::app_search_path_dirs();
        {
            std::string joined;
            const auto bin = media::app::exe_bin_directory();
            if (!bin.empty()) {
                joined = media::path::path_to_utf8(bin);
                add_grant(normalize_sandbox_path_for_run(joined, cwd), "read",
                          pm::security::GrantPropagation::ImmediateFiles);
            }
            for (const auto& dir : search_dirs) {
                if (dir.empty())
                    continue;
                if (!joined.empty())
                    joined += ';';
                joined += dir;
                add_grant(normalize_sandbox_path_for_run(dir, cwd), "read",
                          pm::security::GrantPropagation::ImmediateFiles);
            }
            if (!joined.empty()) {
                if (shell.is_pwsh)
                    command = "$env:Path='" + joined + ";' + $env:Path; " + command;
                else
                    command = "set \"PATH=" + joined + ";%PATH%\" && " + command;
                sb.args = {shell.flag, command};
            }
        }

        pm::log::info_lazy(k_ch, [&] {
            return "sandbox spawn cmd=\"" + command.substr(0, 120) + "\""
                + " shell=\"" + shell.exe + "\" cwd=\"" + cwd + "\""
                + " timeout=" + std::to_string(timeout_ms) + "ms";
        });

        const auto t0 = chr::steady_clock::now();
        // Output streams live via sb.onOutputChunk during spawn(); nothing to
        // replay afterwards.
        static pm::security::SandboxAppContainer backend;
        const pm::security::SandboxResult res = backend.spawn(sb);
        const auto wall_ms = chr::duration_cast<chr::milliseconds>(chr::steady_clock::now() - t0).count();

        pm::log::info_lazy(k_ch, [&, wall_ms] {
            return "sandbox done launched=" + std::string(res.launched ? "true" : "false")
                + " exit=" + std::to_string(res.exitCode)
                + " stdout=" + std::to_string(res.stdoutText.size()) + "B"
                + " stderr=" + std::to_string(res.stderrText.size()) + "B"
                + " wall_ms=" + std::to_string(wall_ms)
                + (res.timedOut ? " TIMEOUT" : "")
                + (res.cancelled ? " CANCELLED" : "");
        });

        media::llm::path::ExecuteResult r;
        r.ok = res.launched && res.exitCode == 0 &&
               !res.timedOut && !res.cancelled && res.error.empty();
        r.envelope = nlohmann::json{
            {"ok", r.ok},
            {"exit_code", res.exitCode},
            {"security_audit", run_security_audit_json(vr)},
            {"sandboxed", true},
            {"sandbox_backend", "appcontainer"},
            {"containerSid", res.containerSid},
        };
        attach_run_captured_output(r.envelope, res.stdoutText, res.stderrText);
        if (res.timedOut) {
            r.error = "run: timed out after " + std::to_string(timeout_ms) + "ms";
            r.envelope["error"] = r.error;
            r.envelope["timed_out"] = true;
        }
        if (res.cancelled) {
            r.error = "run: cancelled";
            r.envelope["error"] = r.error;
            r.envelope["cancelled"] = true;
        }
        if (!res.error.empty()) {
            r.error = "run: " + res.error;
            r.envelope["error"] = r.error;
        }
        return r;
    }
#endif

    // ── 3. Build ProcessRunner options ──
    media::process::RunOptions opts;
    opts.mode           = media::process::RunMode::Captured;
    opts.use_shell      = true;
    opts.shell_hint     = shell_hint;
    opts.shell_command  = command;
    opts.cwd            = cwd;
    opts.timeout_ms     = timeout_ms;
    opts.augment_path   = true;

    // Streaming hook
    opts.on_output_chunk = [](const std::string& stream, const std::string& chunk) {
        std::lock_guard<std::mutex> lk(g_hook_mtx);
        if (g_chunk_hook) g_chunk_hook(stream, chunk);
    };

    // Cancel check
    opts.on_cancel_check = [] { return media::cli::cancel_requested(); };

    // ── 4. Spawn via ProcessRunner ──
    pm::log::info_lazy(k_ch, [&] {
        return "spawn cmd=\"" + command.substr(0, 120) + "\""
            + " shell_hint=\"" + shell_hint + "\""
            + " cwd=\"" + cwd + "\" timeout=" + std::to_string(timeout_ms) + "ms";
    });

    const auto t0 = chr::steady_clock::now();
    auto res = media::process::run(opts);
    const auto wall_ms = chr::duration_cast<chr::milliseconds>(chr::steady_clock::now() - t0).count();

    pm::log::info_lazy(k_ch, [&, wall_ms] {
        std::string line = "done exit=" + std::to_string(res.exit_code)
            + " stdout=" + std::to_string(res.stdout_text.size()) + "B"
            + " stderr=" + std::to_string(res.stderr_text.size()) + "B"
            + " wall_ms=" + std::to_string(wall_ms)
            + (res.timed_out ? " TIMEOUT" : "")
            + (res.cancelled ? " CANCELLED" : "");
        if (!res.stdout_text.empty() && res.stdout_text.size() <= 512)
            line += " stdout_preview=\"" + log_preview(res.stdout_text) + "\"";
        if (!res.stderr_text.empty() && res.stderr_text.size() <= 512)
            line += " stderr_preview=\"" + log_preview(res.stderr_text) + "\"";
        return line;
    });

    // ── 5. Envelope ──
    media::llm::path::ExecuteResult r;
    r.ok = res.exit_code == 0 &&
           !res.timed_out && !res.cancelled && res.error.empty();
    r.envelope = nlohmann::json{
        {"ok", r.ok},
        {"exit_code", res.exit_code},
        {"security_audit", run_security_audit_json(vr)},
    };
    attach_run_captured_output(r.envelope, res.stdout_text, res.stderr_text);
    if (res.timed_out) {
        r.error = "run: timed out after " + std::to_string(timeout_ms) + "ms";
        r.envelope["error"] = r.error;
        r.envelope["timed_out"] = true;
    }
    if (res.cancelled) {
        r.error = "run: cancelled";
        r.envelope["error"] = r.error;
        r.envelope["cancelled"] = true;
    }
    if (!res.ok && !res.timed_out && !res.cancelled && !res.error.empty()) {
        r.error = "run: " + res.error;
        r.envelope["error"] = r.error;
    }
    return r;
}

// ═══════════════════════════════════════════════════════════════════════════════
// §5  Public API — abort_active_run (delegates to ProcessRunner)
// ═══════════════════════════════════════════════════════════════════════════════

void abort_active_run() {
    media::process::abort_captured_run();
#if PM_RUN_TOOL_HAS_APPCONTAINER
    // Also stop a sandboxed run, if one is in flight. Either path is a no-op
    // when nothing is running, so calling both is safe.
    pm::security::abort_active_sandbox();
#endif
}

} // namespace media::llm::run

#include "agent_child_launch.hpp"
#include "pm_branding_config.hpp"

#include <sstream>
#include <string>

namespace media::llm::agent {

// ── helpers ───────────────────────────────────────────────────────────────────

static std::string exe_name(ShellDialect dialect) {
    // PowerShell needs the .exe suffix; POSIX uses the extension-free stem.
    const std::string full = pm::brand::k_exe_basename_cli_u8;  // e.g. "tanit-cli.exe"
    if (dialect == ShellDialect::PowerShell)
        return full;
    // Strip ".exe" for POSIX.
    if (full.size() > 4 && full.compare(full.size() - 4, 4, ".exe") == 0)
        return full.substr(0, full.size() - 4);
    return full;
}

/// Escape a prompt string for embedding inside a double-quoted shell argument.
/// PowerShell: escape backtick, dollar, double-quote.
/// Posix: escape single-quote by ending the string, inserting \', re-opening.
static std::string escape_prompt(const std::string& prompt, ShellDialect dialect) {
    std::string out;
    out.reserve(prompt.size() + 8);
    if (dialect == ShellDialect::PowerShell) {
        for (char c : prompt) {
            if (c == '`' || c == '$' || c == '"') out += '`';
            out += c;
        }
    } else {
        // Posix: wrap in single quotes, escape embedded single-quotes as '\''
        for (char c : prompt) {
            if (c == '\'') out += "'\\''";
            else           out += c;
        }
    }
    return out;
}

// ── public API ────────────────────────────────────────────────────────────────

std::string build_child_shell_command(const AgentChildLaunchSpec& spec,
                                      ShellDialect dialect)
{
    std::ostringstream s;
    s << exe_name(dialect);
    if (!spec.log_level.empty())
        s << " --log-level " << spec.log_level;
    s << " llm agent";
    if (!spec.preset.empty())
        s << " --preset " << spec.preset;
    if (spec.single_turn)
        s << " --single-turn";
    if (!spec.session_id.empty())
        s << " --session-id " << spec.session_id;
    if (!spec.extra_args.empty())
        s << " " << spec.extra_args;

    // Prompt last — it may contain spaces.
    if (!spec.prompt.empty()) {
        if (dialect == ShellDialect::PowerShell)
            s << " --prompt \"" << escape_prompt(spec.prompt, dialect) << "\"";
        else
            s << " --prompt '" << escape_prompt(spec.prompt, dialect) << "'";
    }
    return s.str();
}

std::string child_launch_canonical_example(ShellDialect dialect)
{
    // Keep this in sync with AgentChildLaunchSpec defaults.
    // The placeholder "<child prompt>" signals to readers that the real prompt
    // should be substituted; it is intentionally not a valid shell string.
    std::ostringstream s;
    s << "`" << exe_name(dialect)
      << " --log-level error"
         " llm agent"
         " --single-turn"
         " --prompt \"<child prompt>\"`";
    return s.str();
}

} // namespace media::llm::agent

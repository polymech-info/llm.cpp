#pragma once
//
// AgentChildLaunchSpec — canonical specification for spawning a child
// PM-Image agent via the `run` tool.
//
// Motivation (P7): the canonical child-agent command line was duplicated in
// four places (agent_context.cpp, path_tool_catalog.cpp, llm_info_compact.cpp,
// the test harness).  A single authoritative struct ensures they stay in sync.
//
// Usage:
//   // Build a complete shell command string for a known prompt:
//   AgentChildLaunchSpec spec;
//   spec.prompt = "What is 1+1? Reply with only the number.";
//   std::string cmd = build_child_shell_command(spec);
//
//   // Embed the canonical example (with "<prompt>" placeholder) in system text:
//   std::string example = child_launch_canonical_example();
//
#include "polymech_export.h"
#include <cstdint>
#include <string>

namespace media::llm::agent {

enum class ShellDialect : uint8_t {
    PowerShell,  // pm-image-cli.exe … (Windows default)
    Posix,       // pm-image-cli … (Linux / macOS)
};

struct AgentChildLaunchSpec {
    /// Log verbosity for the child process. Default "error" keeps stderr clean.
    std::string log_level  = "error";
    /// Chat preset to load. "superfast" selects the smallest/fastest model.
    std::string preset     = "superfast";
    /// Whether the child exits after the first assistant response.
    bool        single_turn = true;
    /// The prompt sent to the child agent. Required for build_child_shell_command.
    std::string prompt;
    /// Optional session-id to carry conversation context across child calls.
    std::string session_id;
    /// Extra flags appended verbatim at the end of the arg list (e.g. "--no-tools").
    std::string extra_args;
};

/// Build the full inline shell command for a child agent invocation.
/// The returned string is ready to embed directly in a `run` tool `command` arg.
/// On PowerShell, quotes inside `prompt` are escaped; on Posix, single-quoted.
PM_CHAT_API std::string build_child_shell_command(
    const AgentChildLaunchSpec& spec,
    ShellDialect dialect = ShellDialect::PowerShell);

/// Returns the canonical example string (with a `"<child prompt>"` placeholder)
/// used in system prompts and tool descriptions to instruct the LLM on the
/// correct invocation shape.  Changes here propagate to all injection sites.
PM_CHAT_API std::string child_launch_canonical_example(
    ShellDialect dialect = ShellDialect::PowerShell);

} // namespace media::llm::agent

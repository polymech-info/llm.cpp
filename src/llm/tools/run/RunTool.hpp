#pragma once
//
// media::llm::run — Run Tool for LLM agent shell execution.
//
// Provides:
//   validate_command()      — tiered (Allow/Warn/WarnStrict/Deny) command security  [RunValidators.hpp]
//   detect_shell()          — cross-platform shell resolution (delegates to process_helpers)
//   execute()               — full do_run lifecycle (validate→spawn→poll→drain→envelope)
//   abort_active_run()      — cross-thread cancellation of the running child
//
// Process spawning is handled by media::process::run() in process_runner.cpp.
//
#include "polymech_export.h"
#include "llm/tools/run/RunValidators.hpp"   // ValidationTier, ValidationResult, validate_command
#include "llm/path_tool_executor.hpp"        // ExecuteResult

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace media::llm::run {

// ── Shell detection (thin wrapper around media::process::detect_shell) ────────

struct ShellSpec {
    std::string exe;
    std::string flag;
    bool        is_pwsh = false;
};

POLYMECH_API ShellSpec detect_shell(const std::string& hint = "auto");

// ── Tool execution (called from path_tool_executor dispatch) ──────────────────

/// Full lifecycle: validate → resolve CWD → detect shell → spawn via
/// ProcessRunner(Captured) → build envelope.  Returns an ExecuteResult that
/// path_tool_executor forwards to the agent loop.
POLYMECH_API media::llm::path::ExecuteResult
execute(const nlohmann::json& args,
        const std::string& agent_path_base);

/// Kill the child process of a currently-running `run` tool call.
/// Safe to call from any thread; no-op when idle.
POLYMECH_API void abort_active_run();

// ── Streaming progress hook ──────────────────────────────────────────────────

/// Callback receives (stream, chunk) where stream is "stdout" or "stderr".
using OutputChunkHook = std::function<void(const std::string& stream,
                                           const std::string& chunk)>;

POLYMECH_API void set_output_chunk_hook(OutputChunkHook hook);
POLYMECH_API void clear_output_chunk_hook();

} // namespace media::llm::run

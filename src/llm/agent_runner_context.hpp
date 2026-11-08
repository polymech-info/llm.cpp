#pragma once

#include "agent.hpp"
#include "process/process_runner.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace media::llm::agent {

struct ExternalContextBundle {
    std::string append_system;
    std::string user_prompt;
    std::filesystem::path working_root;
    std::vector<std::filesystem::path> image_attachments;
};

ExternalContextBundle build_external_context(const Turn& turn);

/// `Agent.md` / `AGENTS.md` from the turn's current folder, wrapped for a
/// system prompt. Empty when the preset toggle is off or no file is present.
std::string agent_md_prompt_block(const Turn& turn);

/// Resolve an external CLI executable name (e.g. codex, claude) to a concrete
/// path, honoring `override_path` when set. On Windows this prefers `.exe` /
/// `.cmd` npm shims and skips extensionless shell-script stubs.
std::string resolve_external_cli_executable(const std::string& default_name,
                                            const std::string& override_path = {});

/// Populate `proc.executable`/`proc.args` or a cmd.exe shell wrapper for npm
/// `.cmd` shims on Windows.
void apply_external_cli_command(media::process::RunOptions& proc,
                              const std::string& default_name,
                              const std::string& override_path,
                              std::vector<std::string> args);

/// One Tanit MCP tool call observed on an external runner stream.
/// `name` is the path-tool name (`image_create`), not `mcp__tanit__image_create`.
struct ExternalMcpToolRecord {
    std::string id;
    std::string name;
    nlohmann::json args = nlohmann::json::object();
    nlohmann::json envelope = nlohmann::json::object();
};

/// Bare path-tool name when `tool`/`server` is Tanit's MCP server. Empty otherwise.
std::string tanit_native_tool_name(const std::string& tool, const std::string& server);

/// Tanit path-tool envelope (`ok` / `results`) buried in an MCP result payload.
nlohmann::json tanit_envelope_from_mcp_value(const nlohmann::json& value);

/// Write session artifacts from external-runner MCP calls using the same
/// transcript shape the native runner persists.
void persist_external_mcp_artifacts(const Turn& turn,
                                    const std::string& final_text,
                                    const std::vector<ExternalMcpToolRecord>& calls);

} // namespace media::llm::agent

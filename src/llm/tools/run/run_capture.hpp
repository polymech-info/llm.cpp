#pragma once
//
// Bound one captured stdout/stderr stream before it enters an LLM tool result.
// The live on_output_chunk path is untouched; only the final envelope is.
//

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace media::llm::run {

namespace fs = std::filesystem;

/// {config}/agent-temp/run — Tanit-owned, outside the install dir, and not a
/// sensitive path, so file_read can open files written here.
fs::path agent_run_temp_directory();

/// Copy of the current agent task id. The task-id functions live in the chat
/// host, which pm-media cannot call. set/clear_current_task_id forward here.
void set_capture_session_id(const std::string& id);
void clear_capture_session_id();

/// Collision-safe path for one stream. Empty session_id uses the current
/// agent task id, or "unscoped" when no session is active.
/// stream_name must be "stdout" or "stderr".
fs::path create_run_capture_path(std::string_view stream_name,
                                 std::string_view session_id = {});

struct RunCapturedOutput {
    std::string inline_text;
    bool clipped = false;
    /// False only when a future capture ceiling drops bytes. Phase 1 keeps
    /// the whole ProcessRunner string, so this stays true.
    bool capture_complete = true;
    std::size_t captured_bytes = 0;
    std::size_t inline_bytes = 0;
    std::size_t line_count = 0;
    fs::path full_output_path;
    std::string file_error;
};

/// Return the stream unchanged when it fits. Otherwise persist the complete
/// text, then return head + clipping marker + tail.
RunCapturedOutput prepare_run_output(std::string_view stream_name,
                                     std::string_view captured);

/// Write stdout_* and stderr_* fields. Command ok/exit_code are left alone.
void attach_run_captured_output(nlohmann::json& envelope,
                                std::string_view stdout_text,
                                std::string_view stderr_text);

} // namespace media::llm::run

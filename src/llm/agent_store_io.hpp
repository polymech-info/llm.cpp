#pragma once
//
// Shared disk and clock helpers for session memory and the schedule store.
// This file does not own either store.
//
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace media::llm::agent {

std::filesystem::path session_store_dir();
std::filesystem::path schedule_store_dir();

/// Override the session directory for this process. An empty path clears it.
/// A real change stops the in-process scheduler when one is linked, then
/// drops both stores so the next read reloads the new profile.
void set_session_store_dir(const std::filesystem::path& dir);

/// Called once from the scheduler translation unit. Security builds that do
/// not link the scheduler leave this unset.
void set_session_dir_hook(void (*hook)());

std::string sanitize_session_id(const std::string& id);

std::string now_iso8601();
std::string format_iso8601(const std::chrono::system_clock::time_point& tp);
std::optional<std::chrono::system_clock::time_point> parse_iso8601(const std::string& s);

bool atomic_write_json(const std::filesystem::path& path, const nlohmann::json& j);
bool read_json_file(const std::filesystem::path& path, nlohmann::json& out);

void merge_memory_state(nlohmann::json& target, const nlohmann::json& patch);

int schedule_generation();
void schedule_generation_bump();

bool debug_store_dumps_enabled();

std::filesystem::path session_file_path_for(const std::string& sanitized_id);

} // namespace media::llm::agent

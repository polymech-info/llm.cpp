#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace media::llm {

// Set by `tanit-cli llm agent --serve`. Desktop hosts leave this empty and
// keep using `app_command_handler`.
void set_chat_host_emit(std::function<void(const std::string& method, nlohmann::json args)> fn);

// Returns false when no serve process installed a sink.
bool emit_chat_host(const std::string& method, nlohmann::json args);

} // namespace media::llm

#pragma once

#include "kbot.h"
#include "llm_client.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace media::llm::agent::llama_client {

bool is_llama_router(std::string_view router);

/// Run a chat completion via llama.cpp's native engine (no HTTP / kbot).
///
/// Mirrors polymech::kbot::LLMClient::execute_chat_messages: the OpenAI-style
/// `tools` catalog and conversation are fed to the model's own chat template
/// (common_chat_*), and the response carries either `text` or structured
/// `tool_calls` for the shared agent loop to dispatch.
///
/// cancel_check  : optional predicate polled after each generated token and
///                 against the elapsed-time deadline (when timeout_ms > 0).
///                 Return true to abort; response will have success=false.
polymech::kbot::LLMResponse execute_chat_messages(
    const std::vector<polymech::kbot::ChatMessage>& messages,
    const std::string& model_id_or_path,
    const nlohmann::json& tools       = nlohmann::json(),
    const nlohmann::json& tool_choice = nlohmann::json(),
    int timeout_ms                    = 0,
    std::function<bool()> cancel_check = {});

/// Process-local diagnostics for the cached text GGUF model.
nlohmann::json loaded_models_json();

/// Unload the cached text GGUF model. Returns false when it is busy or absent.
bool unload_loaded_model(std::string_view key, std::string& err);

} // namespace media::llm::agent::llama_client

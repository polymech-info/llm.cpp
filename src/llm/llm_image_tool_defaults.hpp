#pragma once

#include "core/settings_runtime.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace media::llm {

/// Install a per-run override for all chat tool defaults.  The supplied
/// `ChatProviderSettings` wins over anything read from settings.json or the
/// active preset for the duration of the current agent run.  Call
/// pop_chat_settings_override() when the run finishes (RAII guard recommended).
/// Thread-local: safe for concurrent runs in separate threads.
void push_chat_settings_override(const media::runtime_settings::ChatProviderSettings& cs);
void pop_chat_settings_override();

/// RAII guard that calls pop on destruction.
struct ChatSettingsOverrideGuard {
    explicit ChatSettingsOverrideGuard(const media::runtime_settings::ChatProviderSettings& cs)
    { push_chat_settings_override(cs); }
    ~ChatSettingsOverrideGuard() { pop_chat_settings_override(); }
    ChatSettingsOverrideGuard(const ChatSettingsOverrideGuard&)            = delete;
    ChatSettingsOverrideGuard& operator=(const ChatSettingsOverrideGuard&) = delete;
};

/// `settings.json` key `chat` (`image_provider` / `image_model`) only.
/// Does not use top-level `active_provider` or provider-row `default_model`.
void apply_chat_image_defaults_for_tool_options(std::string&           provider,
                                                std::string&           model,
                                                const nlohmann::json&  options_json);

/// `settings.json` key `chat` (`image_recognition_provider` / `image_recognition_model`).
/// Does not use provider-row `default_model`; vision/meta model selection must be explicit.
void apply_chat_image_recognition_defaults_for_tool_options(std::string&          provider,
                                                            std::string&          model,
                                                            const nlohmann::json& options_json);

/// `chat.video_provider` / `chat.video_model` only. Video model selection must be explicit.
void apply_chat_video_defaults_for_tool_options(std::string&          provider,
                                                std::string&          model,
                                                const nlohmann::json& options_json);

/// `chat.ocr_provider` / `chat.ocr_model`.
/// When both fields are empty falls back to `image_recognition_provider/model` so that
/// a preset without an explicit OCR slot still produces a usable default.
void apply_chat_ocr_defaults_for_tool_options(std::string&          provider,
                                              std::string&          model,
                                              const nlohmann::json& options_json);

void ensure_api_key_for_image_provider(const std::string& provider, std::string& api_key);
void ensure_base_url_for_image_provider(const std::string& provider, std::string& base_url);

void repair_meta_google_model_if_replicate_slug(std::string& provider, std::string& model);

} // namespace media::llm

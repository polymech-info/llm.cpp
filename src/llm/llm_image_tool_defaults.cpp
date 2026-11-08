#include "llm_image_tool_defaults.hpp"

#include "core/settings_runtime.hpp"

#include <string>

namespace media::llm {
namespace {

// Thread-local override set by push_chat_settings_override() for the duration
// of one agent run (CLI --ocr-provider/model etc.).
thread_local bool tl_has_chat_override = false;
thread_local media::runtime_settings::ChatProviderSettings tl_chat_override;

media::runtime_settings::ChatProviderSettings resolved_default_chat_settings()
{
    if (tl_has_chat_override)
        return tl_chat_override;

    std::string err;
    media::runtime_settings::ChatProviderPreset preset;
    if (media::runtime_settings::resolve_chat_provider_preset({}, preset, err))
        return preset.chat;

    media::runtime_settings::ChatProviderSettings cs;
    if (media::runtime_settings::load_chat_provider(cs, err))
        return cs;
    return {};
}

} // namespace

void push_chat_settings_override(const media::runtime_settings::ChatProviderSettings& cs)
{
    tl_chat_override     = cs;
    tl_has_chat_override = true;
}

void pop_chat_settings_override()
{
    tl_has_chat_override = false;
    tl_chat_override     = {};
}

void apply_chat_image_defaults_for_tool_options(std::string& provider, std::string& model,
                                                const nlohmann::json& options_json) {
    const bool have_provider = options_json.contains("provider") && options_json["provider"].is_string();
    const bool have_model    = options_json.contains("model") && options_json["model"].is_string();
    const bool user_set_provider = have_provider;
    const bool user_set_model    = have_model;

    media::runtime_settings::ChatProviderSettings cs = resolved_default_chat_settings();
    if (!user_set_provider && provider.empty() && !cs.image_provider.empty())
        provider = cs.image_provider;
    if (!user_set_model && model.empty() && !cs.image_model.empty())
        model = cs.image_model;
}

void apply_chat_image_recognition_defaults_for_tool_options(std::string& provider, std::string& model,
                                                            const nlohmann::json& options_json) {
    const bool have_provider = options_json.contains("provider") && options_json["provider"].is_string();
    const bool have_model    = options_json.contains("model") && options_json["model"].is_string();
    const bool user_set_provider = have_provider;
    const bool user_set_model    = have_model;

    media::runtime_settings::ChatProviderSettings cs = resolved_default_chat_settings();
    if (!user_set_provider && provider.empty() && !cs.image_recognition_provider.empty())
        provider = cs.image_recognition_provider;
    if (!user_set_model && model.empty() && !cs.image_recognition_model.empty())
        model = cs.image_recognition_model;
}

void apply_chat_video_defaults_for_tool_options(std::string& provider, std::string& model,
                                                const nlohmann::json& options_json) {
    const bool have_provider = options_json.contains("provider") && options_json["provider"].is_string();
    const bool have_model    = options_json.contains("model") && options_json["model"].is_string();
    const bool user_set_provider = have_provider;
    const bool user_set_model    = have_model;

    media::runtime_settings::ChatProviderSettings cs = resolved_default_chat_settings();
    if (!user_set_provider && provider.empty() && !cs.video_provider.empty())
        provider = cs.video_provider;
    if (!user_set_model && model.empty() && !cs.video_model.empty())
        model = cs.video_model;
}

void apply_chat_ocr_defaults_for_tool_options(std::string& provider, std::string& model,
                                              const nlohmann::json& options_json) {
    const bool have_provider = options_json.contains("provider") && options_json["provider"].is_string();
    const bool have_model    = options_json.contains("model")    && options_json["model"].is_string();

    media::runtime_settings::ChatProviderSettings cs = resolved_default_chat_settings();

    if (!have_provider && provider.empty()) {
        if (!cs.ocr_provider.empty())
            provider = cs.ocr_provider;
        else if (!cs.image_recognition_provider.empty())
            provider = cs.image_recognition_provider;
    }
    if (!have_model && model.empty()) {
        if (!cs.ocr_model.empty())
            model = cs.ocr_model;
        else if (!cs.image_recognition_model.empty())
            model = cs.image_recognition_model;
    }
}

void repair_meta_google_model_if_replicate_slug(std::string& provider, std::string& model) {
    (void)provider;
    (void)model;
}

void ensure_api_key_for_image_provider(const std::string& provider, std::string& api_key) {
    if (!api_key.empty())
        return;
    std::string base_url;
    media::runtime_settings::merge_provider_credentials(provider, false, api_key, base_url);
}

void ensure_base_url_for_image_provider(const std::string& provider, std::string& base_url) {
    if (!base_url.empty())
        return;
    if (provider.empty())
        return;
    std::string api_key;
    media::runtime_settings::merge_provider_credentials(provider, false, api_key, base_url);
}

} // namespace media::llm

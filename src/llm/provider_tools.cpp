#include "provider_tools.hpp"

#include "constants.hpp"

#include <cctype>

#ifndef FEATURE_PROVIDER_ELEVENLABS
#  define FEATURE_PROVIDER_ELEVENLABS 1
#endif
#ifndef FEATURE_PROVIDER_GOOGLE
#  define FEATURE_PROVIDER_GOOGLE 1
#endif
#ifndef FEATURE_PROVIDER_OPENAI
#  define FEATURE_PROVIDER_OPENAI 1
#endif
#ifndef FEATURE_PROVIDER_OPENROUTER
#  define FEATURE_PROVIDER_OPENROUTER 1
#endif
#ifndef FEATURE_PROVIDER_TANIT
#  define FEATURE_PROVIDER_TANIT 1
#endif
#ifndef FEATURE_PROVIDER_REPLICATE
#  define FEATURE_PROVIDER_REPLICATE 1
#endif
#ifndef FEATURE_PROVIDER_HUGGINGFACE
#  define FEATURE_PROVIDER_HUGGINGFACE 1
#endif
#ifndef FEATURE_PROVIDER_CUSTOM
#  define FEATURE_PROVIDER_CUSTOM 1
#endif

namespace pm::llm {
namespace {

static constexpr const char* k_google_models[] = {
    "gemini-3-pro-image-preview",
    "gemini-3.1-flash-image-preview",
    "gemini-2.0-flash-exp",
};

static constexpr const char* k_openai_models[] = {
    "gpt-image-1",
    "dall-e-3",
    "gpt-4o",
};

static constexpr const char* k_replicate_models[] = {
    "google/nano-banana-pro",
    "google/nano-banana",
    "black-forest-labs/flux-schnell",
};

static constexpr const char* k_openrouter_models[] = {
    "openai/gpt-4o-mini",
    "anthropic/claude-3.5-sonnet",
    "google/gemini-2.0-flash-001",
};

static constexpr ProviderInfo k_registry[] = {
    { "google",     "Google / Gemini", "https://generativelanguage.googleapis.com/v1beta",
      k_google_models,     sizeof(k_google_models)     / sizeof(k_google_models[0]),
      "Providers.EnableGoogle",     "EnableGoogle",     FEATURE_PROVIDER_GOOGLE != 0 },
    { "openai",     "OpenAI",          "https://api.openai.com/v1",
      k_openai_models,     sizeof(k_openai_models)     / sizeof(k_openai_models[0]),
      "Providers.EnableOpenai",     "EnableOpenai",     FEATURE_PROVIDER_OPENAI != 0 },
    { "replicate",  "Replicate",       "https://api.replicate.com/v1",
      k_replicate_models,  sizeof(k_replicate_models)  / sizeof(k_replicate_models[0]),
      "Providers.EnableReplicate",  "EnableReplicate",  FEATURE_PROVIDER_REPLICATE != 0 },
    { "openrouter", "OpenRouter",      "https://openrouter.ai/api/v1",
      k_openrouter_models, sizeof(k_openrouter_models) / sizeof(k_openrouter_models[0]),
      "Providers.EnableOpenrouter", "EnableOpenrouter", FEATURE_PROVIDER_OPENROUTER != 0 },
    { "tanit",    "Tanit",           PM_TANIT_LLM_BASE_U8 "/v1",
      nullptr, 0,
      "Providers.EnablePixlwiz",    "EnablePixlwiz",    FEATURE_PROVIDER_TANIT != 0 },
    { "elevenlabs", "ElevenLabs",      "https://api.elevenlabs.io/v1",
      nullptr, 0,
      "Providers.EnableElevenlabs", "EnableElevenlabs", FEATURE_PROVIDER_ELEVENLABS != 0 },
    // Hub origin only (downloads / search / probe). Do not point this at
    // api-inference.huggingface.co — that is the separate chat-router default.
    { "huggingface", "Hugging Face",   "https://huggingface.co",
      nullptr, 0,
      "Providers.EnableHuggingface", "EnableHuggingface", FEATURE_PROVIDER_HUGGINGFACE != 0 },
    { "custom",     "Custom (OpenAI-compatible)", "",
      nullptr, 0,
      "Providers.EnableCustom",     "EnableCustom",     FEATURE_PROVIDER_CUSTOM != 0 },
};

static constexpr std::size_t k_registry_size =
    sizeof(k_registry) / sizeof(k_registry[0]);

} // namespace

std::size_t provider_registry_size() noexcept { return k_registry_size; }

const ProviderInfo* provider_registry_data() noexcept { return k_registry; }

std::size_t provider_enabled_registry_size() noexcept
{
    std::size_t n = 0;
    for (const auto& e : k_registry)
        if (e.build_enabled)
            ++n;
    return n;
}

const ProviderInfo* provider_find_by_name(std::string_view name) noexcept
{
    for (const auto& e : k_registry) {
        const char* n = e.name;
        std::size_t i = 0;
        for (; i < name.size() && n[i]; ++i) {
            if (std::tolower(static_cast<unsigned char>(name[i])) !=
                std::tolower(static_cast<unsigned char>(n[i])))
                break;
        }
        if (i == name.size() && n[i] == '\0')
            return &e;
    }
    return nullptr;
}

} // namespace pm::llm

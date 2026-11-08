#include "agent_launch.hpp"

#include "constants.hpp"
#include "core/chat_settings_schema.hpp"
#if defined(FEATURE_MS_STORE_ENTITLEMENTS) && FEATURE_MS_STORE_ENTITLEMENTS
#include "licensing/features.hpp"
#endif
#include "core/local_model_catalog.hpp"
#include "core/pixlwiz_provider_models_cli.hpp"
#include "lib/provider_oauth.hpp"
#include "logging/logger.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <vector>

namespace media::llm::agent::launch {
namespace {

LlmApiMode api_mode_from_string(const std::string& raw)
{
    if (raw == "responses" || raw == "response")
        return LlmApiMode::Responses;
    if (raw == "realtime")
        return LlmApiMode::Realtime;
    return LlmApiMode::ChatCompletion;
}

LlmStreamingMode streaming_mode_from_string(const std::string& raw)
{
    if (raw == "on")
        return LlmStreamingMode::On;
    if (raw == "off")
        return LlmStreamingMode::Off;
    return LlmStreamingMode::Auto;
}

} // namespace

bool resolve_agent_launch(const AgentLaunchSpec& spec,
                          AgentLaunchResolved& out,
                          std::string& err)
{
    out = AgentLaunchResolved{};
    std::string load_err;
    (void)media::runtime_settings::load_chat_provider(out.chat_settings, load_err);

    {
        std::string preset_err;
        if (media::runtime_settings::resolve_chat_provider_preset(spec.preset, out.selected_preset, preset_err)) {
            out.chat_settings = out.selected_preset.chat;
            out.has_preset = true;
        } else if (!spec.preset.empty()) {
            err = preset_err;
            return false;
        }
    }

    // CLI per-run tool provider/model overrides win over preset/global settings.
    if (!spec.ocr_provider.empty())        out.chat_settings.ocr_provider                  = spec.ocr_provider;
    if (!spec.ocr_model.empty())           out.chat_settings.ocr_model                     = spec.ocr_model;
    if (!spec.recognition_provider.empty()) out.chat_settings.image_recognition_provider   = spec.recognition_provider;
    if (!spec.recognition_model.empty())   out.chat_settings.image_recognition_model       = spec.recognition_model;
    if (!spec.image_provider.empty())      out.chat_settings.image_provider                = spec.image_provider;
    if (!spec.image_model.empty())         out.chat_settings.image_model                   = spec.image_model;
    if (!spec.video_provider.empty())      out.chat_settings.video_provider                = spec.video_provider;
    if (!spec.video_model.empty())         out.chat_settings.video_model                   = spec.video_model;
    if (!spec.stt_provider.empty())        out.chat_settings.stt_provider                  = spec.stt_provider;
    if (!spec.stt_model.empty())           out.chat_settings.stt_model                     = spec.stt_model;
    if (!spec.tts_provider.empty())        out.chat_settings.tts_provider                  = spec.tts_provider;
    if (!spec.tts_model.empty())           out.chat_settings.tts_model                     = spec.tts_model;
    if (!spec.tts_voice_id.empty())        out.chat_settings.tts_voice_id                  = spec.tts_voice_id;

    const auto& cs = out.chat_settings;
    out.provider.router = spec.router.empty() ? cs.router : spec.router;
    out.provider.model = spec.model.empty() ? cs.model : spec.model;
    // Dedicated planner overrides are propagated as-is from the resolved chat settings; the launch
    // spec does not yet expose CLI knobs for these. Empty strings keep the legacy fallback
    // (planner reuses the main router/model). `apply_planner_tool_selection` re-resolves
    // credentials when planner_router differs from the main router.
    out.provider.planner_router = cs.planner_router;
    out.provider.planner_model  = cs.planner_model;
    {
        const std::string eff_planner_router =
            out.provider.planner_router.empty() ? out.provider.router : out.provider.planner_router;
        const std::string eff_planner_model =
            out.provider.planner_model.empty() ? out.provider.model : out.provider.planner_model;
        out.provider.ai_consent_review =
            spec.ai_consent_review
            || !spec.ai_consent_router.empty()
            || !spec.ai_consent_model.empty();
        out.provider.ai_consent_router = spec.ai_consent_router.empty()
            ? (cs.security_router.empty() ? eff_planner_router : cs.security_router)
            : spec.ai_consent_router;
        out.provider.ai_consent_model = spec.ai_consent_model.empty()
            ? (cs.security_model.empty() ? eff_planner_model : cs.security_model)
            : spec.ai_consent_model;
    }
    out.provider.custom_base_url = cs.base_url;
    out.provider.base_url = spec.base_url;
    media::settings_schema::apply_custom_router_base(
        out.provider.router, cs.base_url, out.provider.base_url);
    out.provider.api_key = spec.api_key;
    out.provider.timeout_ms = spec.timeout_ms > 0 ? spec.timeout_ms : cs.timeout_ms;
    out.provider.max_iterations = spec.max_iterations > 0 ? spec.max_iterations : cs.max_iterations;
    if (out.provider.timeout_ms <= 0)
        out.provider.timeout_ms = 180'000;
    if (!spec.no_tools && out.provider.max_iterations <= 0)
        out.provider.max_iterations = 8;
    if (spec.no_tools)
        out.provider.max_iterations = 0;

    const std::string mode_raw = !spec.api_mode.empty() ? spec.api_mode : cs.api_mode;
    const bool spec_wants_realtime = !spec.api_mode.empty()
        && api_mode_from_string(spec.api_mode) == LlmApiMode::Realtime;
    out.provider.api_mode = api_mode_from_string(mode_raw);
    // chat.api_mode=realtime is the voice-session flag (realtime_router / realtime_model).
    // The text agent is text-only on that transport and cannot dispatch tools, so a
    // stored/preset value must not hijack normal chat turns. Voice / --realtime set
    // spec.api_mode explicitly.
    if (out.provider.api_mode == LlmApiMode::Realtime && !spec_wants_realtime) {
        logger::trace("agent: ignoring stored api_mode=realtime for a text turn; using responses");
        out.provider.api_mode = LlmApiMode::Responses;
    }
    out.provider.streaming_mode = streaming_mode_from_string(spec.streaming);
    // llama.cpp / Ollama / other OpenAI-compat hosts expose /v1/chat/completions,
    // not /v1/responses. Explicit realtime (voice) stays on the WS transport.
    if ((out.provider.router == "llama"
         || media::settings_schema::is_custom_chat_router(out.provider.router))
        && out.provider.api_mode != LlmApiMode::Realtime)
        out.provider.api_mode = LlmApiMode::ChatCompletion;

    // The pixlwiz umbrella proxies text models through LiteLLM. LiteLLM mishandles
    // provider-native `reasoning` items on the /responses path for non-OpenAI
    // backends (Anthropic et al.), corrupting multi-round tool loops. OpenAI-backed
    // models must keep /responses when they use reasoning + function tools (e.g.
    // `text-fast` -> openai/gpt-5.6-luna). Opaque aliases resolve via the cached
    // Tanit model catalog (`tanit-models-cache.json`): explicit
    // `model_info.meta.tanit_api_mode` wins, else infer from litellm_provider.
    if (out.provider.api_mode == LlmApiMode::Responses && out.provider.router == "tanit") {
        const std::string& m = out.provider.model;
        if (!media::pixlwiz_cli::is_openai_backed_tanit_alias(m)) {
            out.provider.api_mode = LlmApiMode::ChatCompletion;
            logger::trace("agent: pixlwiz model '" + m + "' is not OpenAI-backed; using "
                         "/chat/completions instead of /responses (LiteLLM mishandles "
                         "non-OpenAI reasoning items in multi-round tool loops)");
        }
    }

    // Realtime WS sessions need a /v1/realtime-capable model. Dedicated
    // chat.realtime_* overrides win; otherwise fall back to built-in defaults —
    // never the text chat model (gpt-5.6-sol et al. reject invalid_model).
    if (out.provider.api_mode == LlmApiMode::Realtime) {
        auto is_realtime_router = [](const std::string& router) {
            return router == "openai" || router == "tanit" || router == "custom";
        };
        if (spec.router.empty()) {
            if (!cs.realtime_router.empty())
                out.provider.router = cs.realtime_router;
            else if (!is_realtime_router(out.provider.router))
                out.provider.router = pm::llm::k_default_realtime_router;
        }
        if (spec.model.empty()) {
            if (!cs.realtime_model.empty())
                out.provider.model = cs.realtime_model;
            else
                out.provider.model = pm::llm::k_default_realtime_model;
        }
    }

    auto validate_llama_model = [&](const std::string& router,
                                    const std::string& model,
                                    const char* field_name) -> bool {
        if (router != "llama")
            return true;
        media::local_models::LocalModelEntry entry;
        std::string model_err;
        if (!media::local_models::resolve_llama_text_model(model, entry, model_err)) {
            err = std::string(field_name) + ": " + model_err;
            return false;
        }
        return true;
    };
    if (media::settings_schema::is_custom_chat_router(out.provider.router)
        && out.provider.model.empty()) {
        err = "custom router: set a model (id from GET /v1/models, e.g. llama3.2)";
        return false;
    }
    if (!validate_llama_model(out.provider.router, out.provider.model, "model"))
        return false;
    if (!cs.planner_router.empty()) {
        const std::string planner_model = cs.planner_model.empty() ? out.provider.model : cs.planner_model;
        if (!validate_llama_model(cs.planner_router, planner_model, "planner_model"))
            return false;
    }

    media::runtime_settings::merge_provider_credentials(
        out.provider.router, spec.dry_run_credentials, out.provider.api_key, out.provider.base_url);
    // ChatGPT plan tokens are not Platform API keys. They call /v1/responses.
    // An sk- key stays on whatever api_mode the preset already selected.
    if (out.provider.router == "openai"
        && out.provider.api_mode == LlmApiMode::ChatCompletion
        && media::provider_oauth::is_chatgpt_plan_token(out.provider.api_key)) {
        logger::trace("agent: openai credential is a ChatGPT plan token; using /responses");
        out.provider.api_mode = LlmApiMode::Responses;
    }
    media::settings_schema::apply_custom_router_base(
        out.provider.router, out.provider.custom_base_url, out.provider.base_url,
        /*overwrite=*/spec.base_url.empty());

    out.effective_runner = spec.runner;
    if (out.effective_runner.empty() && out.has_preset)
        out.effective_runner = out.selected_preset.runner;
    out.effective_runner = media::settings_schema::normalize_chat_preset_runner(out.effective_runner);

#if defined(FEATURE_MS_STORE_ENTITLEMENTS) && FEATURE_MS_STORE_ENTITLEMENTS
    auto base_for_router = [&](const std::string& router) -> std::string {
        if (router == out.provider.router)
            return out.provider.base_url;
        if (media::settings_schema::is_custom_chat_router(router))
            return cs.base_url;
        return {};
    };
    if (!media::licensing::enforce_llm_router_allowed(out.provider.router, out.provider.base_url, err))
        return false;
    if (!cs.planner_router.empty()
        && !media::licensing::enforce_llm_router_allowed(cs.planner_router,
                                                         base_for_router(cs.planner_router), err))
        return false;
    if (!cs.security_router.empty()
        && !media::licensing::enforce_llm_router_allowed(cs.security_router,
                                                         base_for_router(cs.security_router), err))
        return false;
    if (out.provider.api_mode == LlmApiMode::Realtime
        && !cs.realtime_router.empty()
        && !media::licensing::enforce_llm_router_allowed(cs.realtime_router,
                                                         base_for_router(cs.realtime_router), err))
        return false;
    if (out.provider.ai_consent_review
        && !media::licensing::enforce_llm_router_allowed(out.provider.ai_consent_router,
                                                         base_for_router(out.provider.ai_consent_router),
                                                         err))
        return false;
#endif

    err.clear();
    return true;
}

void apply_launch_policy_to_turn(const AgentLaunchSpec& spec,
                                 const AgentLaunchResolved& resolved,
                                 Turn& turn)
{
    turn.planner_enabled = spec.planner_override_set
        ? spec.planner_enabled
        : (spec.planner_enabled
           || (spec.use_chat_planner_default && resolved.chat_settings.planner_enabled)
           || (resolved.has_preset && resolved.chat_settings.planner_enabled));
    turn.planner_schema_budget = spec.planner_schema_budget;

    if (resolved.has_preset && resolved.selected_preset.has_tools) {
        turn.tool_policy_override_set = true;
        turn.disabled_path_tools  = resolved.selected_preset.tools.disabled_path_tools;
        turn.mcp_tools_enabled    = resolved.selected_preset.tools.mcp_tools_enabled;
        turn.disabled_mcp_servers = resolved.selected_preset.tools.disabled_mcp_servers;
        turn.disabled_mcp_tools   = resolved.selected_preset.tools.disabled_mcp_tools;
    }
    if (resolved.has_preset)
        turn.load_agent_md = resolved.selected_preset.load_agent_md;
    if (resolved.has_preset && resolved.selected_preset.has_skills) {
        turn.skill_policy_override_set = true;
        turn.skills_enabled = resolved.selected_preset.skills.enabled;
        turn.skills_roaming_enabled = resolved.selected_preset.skills.roaming_enabled;
        turn.skills_workspace_enabled = resolved.selected_preset.skills.workspace_enabled;
        turn.pinned_skills = resolved.selected_preset.skills.pinned;
        turn.disabled_skills = resolved.selected_preset.skills.disabled;
    }
}

AgentRunName make_timestamp_agent_run_name()
{
    std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
#if defined(_WIN32)
    if (localtime_s(&tm_buf, &t) != 0)
        return {"agent", "agent"};
    const std::tm* local = &tm_buf;
#else
    const std::tm* local = std::localtime(&t);
    if (!local)
        return {"agent", "agent"};
#endif
    char display[32];
    std::strftime(display, sizeof(display), "%m:%d:%H:%M:%S", local);
    AgentRunName out;
    out.display_name = display;
    out.safe_id = make_safe_agent_run_id(out.display_name);
    return out;
}

std::string make_safe_agent_run_id(const std::string& display_name)
{
    std::string stem;
    for (unsigned char ch : display_name) {
        if (std::isalnum(ch)) {
            stem.push_back(static_cast<char>(std::tolower(ch)));
        } else if (ch == '-' || ch == '_' || ch == ':' || ch == '.' || ch == ' ') {
            if (stem.empty() || stem.back() != '-')
                stem.push_back('-');
        }
    }
    while (!stem.empty() && stem.back() == '-')
        stem.pop_back();
    return stem.empty() ? std::string{"agent"} : "agent-" + stem;
}

} // namespace media::llm::agent::launch

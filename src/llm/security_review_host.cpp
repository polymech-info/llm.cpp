#include "llm/security_review.hpp"

#include "core/chat_settings_schema.hpp"
#include "core/settings_runtime.hpp"

#include <string>
#include <utility>
#include "fs/path_sanitizer.hpp"

namespace media::llm::agent::security_review {

nlohmann::json consent_request_payload(const pm::security::ConsentRequest& cr)
{
    nlohmann::json j{
        {"title", cr.title},
        {"risk", std::string(pm::security::to_string(cr.risk))},
        {"userGoal", cr.userGoal},
        {"whyNeeded", cr.whyNeeded},
        {"possibleRisk", cr.possibleRisk},
        {"resourcePreview", cr.resourcePreview},
        {"choices", cr.choices},
        {"sandboxBackends", cr.sandboxBackends},
        {"suggestedScope", cr.suggestedScope},
        {"aiAutoAvailable", cr.aiAutoAvailable},
        {"tool", {
            {"toolId", cr.request.tool.toolId},
            {"provider", cr.request.tool.provider},
            {"serverId", cr.request.tool.serverId},
        }},
        {"action", std::string(pm::security::to_string(cr.request.action))},
        {"profile", std::string(pm::security::to_string(cr.request.profile))},
        {"sessionId", cr.request.sessionId},
        {"taskId", cr.request.taskId},
    };
    if (cr.request.target) {
        const auto& t = *cr.request.target;
        j["target"] = {
            {"uri", t.uri},
            {"path", t.path.empty() ? std::string{} : media::path::path_to_generic_utf8(t.path)},
        };
    }
    return j;
}

nlohmann::json error_review(std::string message)
{
    return {
        {"decision", "ask"},
        {"assessedRisk", "medium"},
        {"riskRaised", false},
        {"reason", std::move(message)},
        {"notes", nlohmann::json::array({"AI review failed; human approval remains required."})},
    };
}

ProviderSettings resolve_provider_from_chat_settings()
{
    ProviderSettings p;
    media::runtime_settings::ChatProviderSettings cs;
    std::string err;
    (void)media::runtime_settings::load_chat_provider(cs, err);
    const std::string planner_router = cs.planner_router.empty() ? cs.router : cs.planner_router;
    const std::string planner_model = cs.planner_model.empty() ? cs.model : cs.planner_model;
    p.router = cs.security_router.empty() ? planner_router : cs.security_router;
    p.model = cs.security_model.empty() ? planner_model : cs.security_model;
    p.timeout_ms = cs.timeout_ms > 0 ? cs.timeout_ms : 10'000;
    p.max_iterations = 0;
    p.api_mode = LlmApiMode::ChatCompletion;
    media::runtime_settings::merge_provider_credentials(p.router, false, p.api_key, p.base_url);
    media::settings_schema::apply_custom_router_base(
        p.router, cs.base_url, p.base_url, /*overwrite=*/true);
    p.custom_base_url = cs.base_url;
    return p;
}

ReviewResult run_review(const pm::security::ConsentRequest& cr,
                        ProviderSettings provider)
{
    std::string folder_hint;
    if (cr.request.target && !cr.request.target->path.empty())
        folder_hint = media::path::path_to_generic_utf8(cr.request.target->path.parent_path());
    return run_json_review(consent_request_payload(cr), std::move(provider), std::move(folder_hint));
}

} // namespace media::llm::agent::security_review

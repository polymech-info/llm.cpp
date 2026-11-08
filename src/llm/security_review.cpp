#include "llm/security_review.hpp"

#include "core/cli_cancel.hpp"
#include "core/chat_settings_schema.hpp"
#include "core/settings_runtime.hpp"
#include "llm/agent_factory.hpp"

#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>
#include "string/string_utils.hpp"

namespace media::llm::agent::security_review {

nlohmann::json parse_review_text(std::string text)
{
    auto trim = [](std::string& s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
            s.erase(s.begin());
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
            s.pop_back();
    };
    trim(text);
    if (text.rfind("```", 0) == 0) {
        const auto first_nl = text.find('\n');
        const auto last = text.rfind("```");
        if (first_nl != std::string::npos && last != std::string::npos && last > first_nl)
            text = text.substr(first_nl + 1, last - first_nl - 1);
        trim(text);
    }
    try {
        return nlohmann::json::parse(text);
    } catch (...) {
        const auto b = text.find('{');
        const auto e = text.rfind('}');
        if (b != std::string::npos && e != std::string::npos && e > b)
            return nlohmann::json::parse(text.substr(b, e - b + 1));
        throw;
    }
}

using media::strutil::lower_ascii;

std::string normalize_risk_token(const nlohmann::json& v)
{
    if (!v.is_string())
        return {};
    std::string s = lower_ascii(v.get<std::string>());
    if (s == "low" || s == "medium" || s == "high" || s == "critical")
        return s;
    if (s == "med" || s == "moderate")
        return "medium";
    if (s == "severe")
        return "critical";
    return {};
}

int risk_rank(const std::string& risk)
{
    if (risk == "low") return 0;
    if (risk == "medium") return 1;
    if (risk == "high") return 2;
    if (risk == "critical") return 3;
    return 1;
}

std::string infer_risk_from_reason(const std::string& reason)
{
    const std::string s = lower_ascii(reason);
    if (s.find("critical") != std::string::npos ||
        s.find("credential") != std::string::npos ||
        s.find("secret") != std::string::npos) {
        return "critical";
    }
    if (s.find("exceeds medium-risk") != std::string::npos ||
        s.find("persistent") != std::string::npos ||
        s.find("autorun") != std::string::npos ||
        s.find("scheduled task") != std::string::npos ||
        s.find("at every logon") != std::string::npos) {
        return "high";
    }
    return {};
}

std::string normalize_decision(const nlohmann::json& review)
{
    std::string s;
    if (review.contains("decision") && review["decision"].is_string())
        s = review["decision"].get<std::string>();
    else if (review.contains("kind") && review["kind"].is_string())
        s = review["kind"].get<std::string>();
    s = lower_ascii(std::move(s));
    if (s == "allow" || s == "deny" || s == "ask")
        return s;
    return "ask";
}

nlohmann::json normalize_review_json(nlohmann::json review, const nlohmann::json& consent_request)
{
    if (!review.is_object())
        review = nlohmann::json::object();

    const std::string input_risk =
        normalize_risk_token(review.value("inputRisk", nlohmann::json{})).empty()
            ? (normalize_risk_token(consent_request.value("risk", nlohmann::json{})).empty()
                ? std::string{"medium"}
                : normalize_risk_token(consent_request.value("risk", nlohmann::json{})))
            : normalize_risk_token(review.value("inputRisk", nlohmann::json{}));

    std::string assessed = normalize_risk_token(review.value("assessedRisk", nlohmann::json{}));
    if (assessed.empty())
        assessed = normalize_risk_token(review.value("risk", nlohmann::json{}));
    if (assessed.empty())
        assessed = normalize_risk_token(review.value("riskLevel", nlohmann::json{}));

    const std::string reason = review.value("reason", std::string{});
    if (assessed.empty())
        assessed = infer_risk_from_reason(reason);
    if (assessed.empty())
        assessed = input_risk;

    const std::string decision = normalize_decision(review);
    review["decision"] = decision;
    review["inputRisk"] = input_risk;
    review["assessedRisk"] = assessed;
    review["riskRaised"] = review.value("riskRaised", false) || risk_rank(assessed) > risk_rank(input_risk);
    if (!review.contains("reason") || !review["reason"].is_string())
        review["reason"] = "SecurityAgent returned no rationale.";
    if (!review.contains("notes") || !review["notes"].is_array())
        review["notes"] = nlohmann::json::array();

    nlohmann::json recommendation = review.value("recommendation", nlohmann::json::object());
    if (!recommendation.is_object())
        recommendation = nlohmann::json::object();
    recommendation["decision"] = decision;
    for (const char* key : {"lifetime", "scope", "sandboxBackend"}) {
        if (!recommendation.contains(key) && review.contains(key))
            recommendation[key] = review[key];
    }
    if (!recommendation.empty())
        review["recommendation"] = std::move(recommendation);

    review.erase("kind");
    review.erase("risk");
    review.erase("riskLevel");
    return review;
}

ReviewResult run_json_review(nlohmann::json consent_request,
                             ProviderSettings provider,
                             std::string folder_hint)
{
    ReviewResult out;
    out.provider = provider;

#if defined(FEATURE_COMMAND_TEST) && FEATURE_COMMAND_TEST
    if (const char* mock = std::getenv("PM_AI_CONSENT_REVIEW_JSON")) {
        out.provider.router = "mock";
        out.provider.model = "PM_AI_CONSENT_REVIEW_JSON";
        out.mock = true;
        try {
            out.review = normalize_review_json(nlohmann::json::parse(mock), consent_request);
            out.ok = true;
        } catch (const std::exception& ex) {
            out.error = ex.what();
        }
        return out;
    }
#endif

    if (provider.router.empty() || provider.model.empty()) {
        out.error = "security reviewer router/model not configured";
        return out;
    }

    ProviderSettings rp = provider;
    rp.max_iterations = 0;
    if (rp.timeout_ms <= 0)
        rp.timeout_ms = 10'000;
    if (rp.api_key.empty() || rp.base_url.empty())
        media::runtime_settings::merge_provider_credentials(
            rp.router, false, rp.api_key, rp.base_url);
    media::settings_schema::apply_custom_router_base(
        rp.router, provider.custom_base_url, rp.base_url, /*overwrite=*/true);
    out.provider = rp;

    Turn turn;
    turn.user_prompt = nlohmann::json{
        {"kind", "security_consent_review"},
        {"instruction", "Review this ConsentRequest. Return compact JSON only with keys: decision, assessedRisk, riskRaised, reason, lifetime, scope, sandboxBackend, notes."},
        {"consentRequest", consent_request},
    }.dump();
    turn.folder_hint = std::move(folder_hint);
    turn.planner_enabled = false;

    try {
        Agent sec_agent = AgentFactory::security_agent();
        Result r = sec_agent.run(turn, rp, [](const Event&) {
            return !media::cli::cancel_requested();
        });
        if (!r.ok) {
            out.error = r.error.empty() ? "security reviewer failed" : r.error;
            return out;
        }
        out.review = normalize_review_json(parse_review_text(r.final_text), consent_request);
        out.ok = true;
        return out;
    } catch (const std::exception& ex) {
        out.error = ex.what();
        return out;
    }
}

} // namespace media::llm::agent::security_review

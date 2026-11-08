#pragma once

#include "llm/agent.hpp"
#include "security/types.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace media::llm::agent::security_review {

struct ReviewResult {
    bool ok = false;
    nlohmann::json review = nlohmann::json::object();
    std::string error;
    ProviderSettings provider;
    bool mock = false;
};

nlohmann::json consent_request_payload(const pm::security::ConsentRequest& cr);
nlohmann::json error_review(std::string message);

ProviderSettings resolve_provider_from_chat_settings();

ReviewResult run_json_review(nlohmann::json consent_request,
                             ProviderSettings provider,
                             std::string folder_hint = {});

ReviewResult run_review(const pm::security::ConsentRequest& cr,
                        ProviderSettings provider);

} // namespace media::llm::agent::security_review

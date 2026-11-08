#include "agent_internal.hpp"

#include "string/string_sanitizers.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace media::llm::agent::detail {

void sanitize_json_utf8_strings(nlohmann::json& j)
{
    if (j.is_string()) {
        const std::string repaired =
            media::str_san::normalise_text_for_json_storage(j.get<std::string>());
        j = repaired;
        return;
    }
    if (j.is_array()) {
        for (auto& el : j)
            sanitize_json_utf8_strings(el);
        return;
    }
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it)
            sanitize_json_utf8_strings(it.value());
    }
}

std::string safe_json_dump(const nlohmann::json& j, int indent)
{
    try {
        nlohmann::json copy = j;
        sanitize_json_utf8_strings(copy);
        return copy.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (...) {
        if (j.is_array())
            return "[]";
        return "{}";
    }
}

} // namespace media::llm::agent::detail

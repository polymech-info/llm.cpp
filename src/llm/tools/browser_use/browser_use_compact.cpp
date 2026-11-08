#include "llm/tools/browser_use/browser_use_compact.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>
#include "string/string_utils.hpp"

namespace media::llm::browser_use {
namespace {

constexpr int k_max_items_kept = 12;
constexpr int k_max_items_kept_find = 8;
constexpr std::size_t k_max_url_kept = 120;

void copy_if_present(const nlohmann::json& src, nlohmann::json& dst, const char* key)
{
    if (src.contains(key))
        dst[key] = src[key];
}

void shorten_url_field(nlohmann::json& envelope)
{
    if (!envelope.contains("url") || !envelope["url"].is_string())
        return;
    std::string url = envelope["url"].get<std::string>();
    if (url.size() <= k_max_url_kept)
        return;
    // Keep origin + path prefix; drop long query strings (Google SERPs).
    const auto q = url.find('?');
    if (q != std::string::npos && q > 32)
        url.resize(q);
    if (url.size() > k_max_url_kept) {
        url.resize(k_max_url_kept);
        url += "...";
    }
    envelope["url"] = std::move(url);
}

nlohmann::json slim_index_item(const nlohmann::json& item)
{
    if (!item.is_object())
        return item;
    nlohmann::json slim = nlohmann::json::object();
    static constexpr const char* k_keys[] = {
        "ref", "role", "name", "disabled", "checked", "type", "value",
    };
    for (const char* key : k_keys)
        copy_if_present(item, slim, key);
    return slim;
}

bool is_formish_item(const nlohmann::json& item)
{
    if (!item.is_object())
        return false;
    const std::string role = item.value("role", std::string{});
    if (role == "textbox" || role == "combobox" || role == "checkbox" || role == "radio"
        || role == "searchbox" || role == "spinbutton" || role == "switch")
        return true;
    if (item.contains("type") && item["type"].is_string()) {
        const std::string type = item["type"].get<std::string>();
        if (type == "text" || type == "email" || type == "password" || type == "search"
            || type == "url" || type == "number" || type == "tel")
            return true;
    }
    const std::string name = item.value("name", std::string{});
    if (name.empty())
        return false;
    // Save / Apply / Submit / Next style actions matter for form fills.
    static constexpr const char* k_verbs[] = {"save", "submit", "apply", "next", "continue", "ok"};
    std::string lower = name;
    lower = media::strutil::lower_ascii(std::move(lower));
    for (const char* verb : k_verbs) {
        if (lower.find(verb) != std::string::npos)
            return true;
    }
    return false;
}

nlohmann::json prefer_form_items(const nlohmann::json& src, std::size_t limit)
{
    nlohmann::json form = nlohmann::json::array();
    nlohmann::json other = nlohmann::json::array();
    for (const auto& item : src) {
        if (!item.is_object())
            continue;
        if (is_formish_item(item))
            form.push_back(slim_index_item(item));
        else
            other.push_back(slim_index_item(item));
    }
    nlohmann::json out = nlohmann::json::array();
    for (const auto& item : form) {
        if (out.size() >= limit)
            break;
        out.push_back(item);
    }
    for (const auto& item : other) {
        if (out.size() >= limit)
            break;
        out.push_back(item);
    }
    return out;
}

nlohmann::json slim_zone_hint(const nlohmann::json& hint)
{
    if (!hint.is_object())
        return hint;
    nlohmann::json slim = nlohmann::json::object();
    static constexpr const char* k_keys[] = {
        "ref", "role", "name", "interactiveCount", "confidence", "roles",
    };
    for (const char* key : k_keys)
        copy_if_present(hint, slim, key);
    return slim;
}

nlohmann::json slim_scope_field(const nlohmann::json& scope)
{
    if (!scope.is_object())
        return scope;
    nlohmann::json slim = nlohmann::json::object();
    copy_if_present(scope, slim, "active");
    copy_if_present(scope, slim, "ref");
    copy_if_present(scope, slim, "name");
    copy_if_present(scope, slim, "source");
    return slim;
}

#if defined(FEATURE_BROWSER_PAGE_MODEL) && FEATURE_BROWSER_PAGE_MODEL
/// Keep actionable refs in history: ranked/blocking zones first, collectionItems
/// with liveRef (not keys-only). Caps size so we don't re-inflate page dumps.
nlohmann::json slim_page_model(const nlohmann::json& src)
{
    nlohmann::json model = nlohmann::json::object();
    if (!src.is_object())
        return model;
    copy_if_present(src, model, "v");
    copy_if_present(src, model, "feature");
    copy_if_present(src, model, "revision");
    copy_if_present(src, model, "addedZones");
    copy_if_present(src, model, "changedZones");
    copy_if_present(src, model, "removedZones");
    copy_if_present(src, model, "interactionImpact");
    copy_if_present(src, model, "events");
    copy_if_present(src, model, "rankedZones");

    std::vector<std::string> priority_ids;
    if (src.contains("rankedZones") && src["rankedZones"].is_array()) {
        for (const auto& rz : src["rankedZones"]) {
            if (rz.is_object())
                priority_ids.push_back(rz.value("zoneId", std::string{}));
        }
    }

    if (src.contains("zones") && src["zones"].is_array()) {
        const auto& src_zones = src["zones"];
        std::vector<std::size_t> order;
        order.reserve(src_zones.size());
        auto already = [&](std::size_t idx) {
            return std::find(order.begin(), order.end(), idx) != order.end();
        };
        for (const auto& id : priority_ids) {
            if (id.empty())
                continue;
            for (std::size_t i = 0; i < src_zones.size(); ++i) {
                if (!src_zones[i].is_object())
                    continue;
                if (src_zones[i].value("zoneId", std::string{}) == id && !already(i)) {
                    order.push_back(i);
                    break;
                }
            }
        }
        for (std::size_t i = 0; i < src_zones.size(); ++i) {
            if (!src_zones[i].is_object())
                continue;
            if (src_zones[i].value("blocking", false) && !already(i))
                order.push_back(i);
        }
        for (std::size_t i = 0; i < src_zones.size(); ++i) {
            if (!already(i) && src_zones[i].is_object())
                order.push_back(i);
        }

        nlohmann::json zones = nlohmann::json::array();
        const std::size_t limit = std::min(order.size(), static_cast<std::size_t>(8));
        for (std::size_t n = 0; n < limit; ++n) {
            const auto& z = src_zones[order[n]];
            nlohmann::json slim = nlohmann::json::object();
            copy_if_present(z, slim, "zoneId");
            copy_if_present(z, slim, "kind");
            copy_if_present(z, slim, "purpose");
            copy_if_present(z, slim, "name");
            copy_if_present(z, slim, "liveRef");
            copy_if_present(z, slim, "doc");
            copy_if_present(z, slim, "blocking");
            copy_if_present(z, slim, "dismissCandidates");
            copy_if_present(z, slim, "blockedZones");
            copy_if_present(z, slim, "durable");
            if (z.contains("collectionItems") && z["collectionItems"].is_array()) {
                const auto& items = z["collectionItems"];
                slim["collectionItemCount"] = items.size();
                nlohmann::json kept = nlohmann::json::array();
                // Prefer actionable refs over key-only — this is the yield win.
                const bool prioritized =
                    z.value("blocking", false)
                    || std::find(priority_ids.begin(), priority_ids.end(),
                                 z.value("zoneId", std::string{}))
                        != priority_ids.end();
                const std::size_t klimit = prioritized
                    ? std::min(items.size(), static_cast<std::size_t>(16))
                    : std::min(items.size(), static_cast<std::size_t>(8));
                for (std::size_t k = 0; k < klimit; ++k) {
                    if (!items[k].is_object())
                        continue;
                    nlohmann::json row = nlohmann::json::object();
                    copy_if_present(items[k], row, "key");
                    copy_if_present(items[k], row, "liveRef");
                    copy_if_present(items[k], row, "name");
                    if (!row.empty())
                        kept.push_back(std::move(row));
                }
                if (!kept.empty())
                    slim["collectionItems"] = std::move(kept);
            }
            zones.push_back(std::move(slim));
        }
        model["zones"] = std::move(zones);
    }
    return model;
}
#endif

} // namespace

bool is_browser_history_tool(const std::string& tool)
{
    return tool == "browser_read" || tool == "browser_find" || tool == "browser_batch"
        || tool == "browser_click" || tool == "browser_type" || tool == "browser_select"
        || tool == "browser_scope" || tool == "browser_navigate";
}

nlohmann::json compact_read_data(const nlohmann::json& data, bool keep_items)
{
    if (!data.is_object())
        return data;

    nlohmann::json out = nlohmann::json::object();
    copy_if_present(data, out, "count");
    copy_if_present(data, out, "truncated");
    copy_if_present(data, out, "pageTextOmitted");
    copy_if_present(data, out, "ref");
    copy_if_present(data, out, "role");
    copy_if_present(data, out, "name");
    copy_if_present(data, out, "value");

    if (data.contains("scope"))
        out["scope"] = slim_scope_field(data["scope"]);

    auto append_files = [&]() {
        if (!data.contains("files") || !data["files"].is_array())
            return;
        nlohmann::json files = nlohmann::json::array();
        for (const auto& file : data["files"]) {
            if (!file.is_object() || files.size() >= 8)
                break;
            nlohmann::json row = nlohmann::json::object();
            copy_if_present(file, row, "kind");
            copy_if_present(file, row, "url");
            copy_if_present(file, row, "name");
            copy_if_present(file, row, "ref");
            if (row.contains("url"))
                files.push_back(std::move(row));
        }
        if (!files.empty())
            out["files"] = std::move(files);
    };

    if (!keep_items) {
        if (data.contains("items") && data["items"].is_array())
            out["itemsReturned"] = data["items"].size();
        append_files();
        out["compacted"] = true;
#if defined(FEATURE_BROWSER_PAGE_MODEL) && FEATURE_BROWSER_PAGE_MODEL
        if (data.contains("model") && data["model"].is_object())
            out["model"] = slim_page_model(data["model"]);
#endif
        return out;
    }

    // History keeps refs/actions only — pageText/pageInfo are too large and rarely needed.
    copy_if_present(data, out, "options");
    copy_if_present(data, out, "matchCount");

    if (data.contains("element") && data["element"].is_object()) {
        const auto& element = data["element"];
        nlohmann::json slim = nlohmann::json::object();
        static constexpr const char* k_el_keys[] = {
            "role", "name", "value", "options",
        };
        for (const char* key : k_el_keys)
            copy_if_present(element, slim, key);
        out["element"] = std::move(slim);
    }

    if (data.contains("items") && data["items"].is_array()) {
        const auto& src = data["items"];
        const bool find_payload = data.contains("matchCount");
        const std::size_t limit = static_cast<std::size_t>(
            find_payload ? k_max_items_kept_find : k_max_items_kept);
        nlohmann::json items;
        if (!find_payload)
            items = prefer_form_items(src, limit);
        else {
            items = nlohmann::json::array();
            const std::size_t n = std::min(src.size(), limit);
            for (std::size_t i = 0; i < n; ++i)
                items.push_back(slim_index_item(src[i]));
        }
        out["items"] = std::move(items);
        if (src.size() > out["items"].size()) {
            out["truncated"] = true;
            out["itemsOmitted"] = src.size() - out["items"].size();
        }
    }

    if (data.contains("zoneHints") && data["zoneHints"].is_array()) {
        nlohmann::json hints = nlohmann::json::array();
        const auto& src = data["zoneHints"];
        const std::size_t limit = std::min(src.size(), static_cast<std::size_t>(2));
        for (std::size_t i = 0; i < limit; ++i)
            hints.push_back(slim_zone_hint(src[i]));
        out["zoneHints"] = std::move(hints);
    }

#if defined(FEATURE_BROWSER_PAGE_MODEL) && FEATURE_BROWSER_PAGE_MODEL
    if (data.contains("model") && data["model"].is_object())
        out["model"] = slim_page_model(data["model"]);
#endif

    if (data.contains("pageText") && data["pageText"].is_string()
        && !data["pageText"].get<std::string>().empty()) {
        out["pageTextOmitted"] = true;
    }

    append_files();
    return out;
}

nlohmann::json compact_batch_envelope(nlohmann::json envelope, bool keep_steps)
{
    if (!envelope.is_object())
        return envelope;

    nlohmann::json out = nlohmann::json::object();
    copy_if_present(envelope, out, "ok");
    copy_if_present(envelope, out, "tool");
    copy_if_present(envelope, out, "epoch");
    copy_if_present(envelope, out, "revision");
    copy_if_present(envelope, out, "error");
    copy_if_present(envelope, out, "batch_error");
    copy_if_present(envelope, out, "prompt_injection_detected");
    copy_if_present(envelope, out, "prompt_injection_kind");
    copy_if_present(envelope, out, "prompt_injection_reason");
    out["all_ok"] = envelope.value("all_ok", false);
    if (envelope.contains("first_failure"))
        out["first_failure"] = envelope["first_failure"];
    shorten_url_field(envelope);
    copy_if_present(envelope, out, "url");
    if (keep_steps)
        copy_if_present(envelope, out, "title");

    if (!keep_steps) {
        if (envelope.contains("steps") && envelope["steps"].is_array()) {
            out["stepsReturned"] = envelope["steps"].size();
            // Keep a one-line trail of actions so the model remembers the turn.
            nlohmann::json trail = nlohmann::json::array();
            for (const auto& step : envelope["steps"]) {
                if (!step.is_object())
                    continue;
                nlohmann::json row{{"action", step.value("action", std::string{})},
                                   {"ok", step.value("ok", false)}};
                if (step.value("auto", false))
                    row["auto"] = true;
                if (step.contains("data") && step["data"].is_object()
                    && step["data"].contains("ref"))
                    row["ref"] = step["data"]["ref"];
                trail.push_back(std::move(row));
                if (trail.size() >= 8)
                    break;
            }
            if (!trail.empty())
                out["actionTrail"] = std::move(trail);
        }
#if defined(FEATURE_BROWSER_PAGE_MODEL) && FEATURE_BROWSER_PAGE_MODEL
        if (envelope.contains("model") && envelope["model"].is_object())
            out["model"] = slim_page_model(envelope["model"]);
#endif
        out["compacted"] = true;
        return out;
    }

    if (envelope.contains("steps") && envelope["steps"].is_array()) {
        nlohmann::json steps = nlohmann::json::array();
        for (const auto& step : envelope["steps"]) {
            if (!step.is_object())
                continue;
            nlohmann::json slim = nlohmann::json::object();
            copy_if_present(step, slim, "index");
            copy_if_present(step, slim, "action");
            copy_if_present(step, slim, "ok");
            copy_if_present(step, slim, "error");
            copy_if_present(step, slim, "waited_ms");
            copy_if_present(step, slim, "auto");
            copy_if_present(step, slim, "note");
            if (step.contains("data")) {
                if (step.value("action", std::string{}) == "read" && step["data"].is_object())
                    slim["data"] = compact_read_data(step["data"], true);
                else
                    slim["data"] = step["data"];
            }
            steps.push_back(std::move(slim));
        }
        out["steps"] = std::move(steps);
    }
#if defined(FEATURE_BROWSER_PAGE_MODEL) && FEATURE_BROWSER_PAGE_MODEL
    if (envelope.contains("model") && envelope["model"].is_object())
        out["model"] = slim_page_model(envelope["model"]);
#endif
    return out;
}

nlohmann::json compact_action_envelope(nlohmann::json envelope, bool keep_detail)
{
    if (!envelope.is_object())
        return envelope;
    nlohmann::json out = nlohmann::json::object();
    copy_if_present(envelope, out, "ok");
    copy_if_present(envelope, out, "tool");
    copy_if_present(envelope, out, "epoch");
    copy_if_present(envelope, out, "revision");
    copy_if_present(envelope, out, "error");
    copy_if_present(envelope, out, "prompt_injection_detected");
    copy_if_present(envelope, out, "prompt_injection_kind");
    copy_if_present(envelope, out, "prompt_injection_reason");
    shorten_url_field(envelope);
    copy_if_present(envelope, out, "url");
    if (envelope.contains("scope"))
        out["scope"] = slim_scope_field(envelope["scope"]);
    if (!keep_detail) {
        if (envelope.contains("data") && envelope["data"].is_object()) {
            nlohmann::json data = nlohmann::json::object();
            copy_if_present(envelope["data"], data, "ref");
            copy_if_present(envelope["data"], data, "action");
            if (envelope["data"].contains("scope"))
                data["scope"] = slim_scope_field(envelope["data"]["scope"]);
#if defined(FEATURE_BROWSER_PAGE_MODEL) && FEATURE_BROWSER_PAGE_MODEL
            // Keep post-mutation model so dismiss/board loops don't lose events in history.
            if (envelope["data"].contains("model") && envelope["data"]["model"].is_object())
                data["model"] = slim_page_model(envelope["data"]["model"]);
#endif
            if (!data.empty())
                out["data"] = std::move(data);
        }
        out["compacted"] = true;
        return out;
    }
    if (envelope.contains("data") && envelope["data"].is_object()) {
        nlohmann::json data = envelope["data"];
#if defined(FEATURE_BROWSER_PAGE_MODEL) && FEATURE_BROWSER_PAGE_MODEL
        if (data.contains("model") && data["model"].is_object())
            data["model"] = slim_page_model(data["model"]);
#endif
        out["data"] = std::move(data);
    }
    copy_if_present(envelope, out, "title");
    return out;
}

nlohmann::json compact_tool_envelope_for_history(nlohmann::json envelope)
{
    if (!envelope.is_object())
        return envelope;

    shorten_url_field(envelope);

    const std::string tool = envelope.value("tool", std::string{});
    if (tool == "browser_read" || tool == "browser_find") {
        if (envelope.contains("data"))
            envelope["data"] = compact_read_data(envelope["data"], true);
        if (envelope.contains("scope"))
            envelope["scope"] = slim_scope_field(envelope["scope"]);
        envelope.erase("title");
        return envelope;
    }
    if (tool == "browser_batch")
        return compact_batch_envelope(std::move(envelope), true);
    if (tool == "browser_click" || tool == "browser_type" || tool == "browser_select"
        || tool == "browser_scope" || tool == "browser_navigate")
        return compact_action_envelope(std::move(envelope), true);
    return envelope;
}

} // namespace media::llm::browser_use

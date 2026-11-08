#include "llm/tools/browser_use/Tool_BrowserUse.hpp"
#include "llm/tools/browser_use/browser_use_compact.hpp"
#include "llm/tools/browser_use/browser_use_inject.hpp"
#include "llm/tools/browser_use/browser_use_page_model.hpp"

#include "logging/logger.h"
#include "policy/app_policy.hpp"
#if defined(_WIN32) && defined(FEATURE_BROWSER) && FEATURE_BROWSER
#include "win/web/browser_batch.hpp"
#include "win/web/browser_navigate.hpp"
#include "core/host_backends.hpp"
#endif

#include <atomic>
#include <optional>
#include <regex>
#include <string>

namespace media::llm::browser_use {
namespace {

using media::llm::path::ExecuteResult;
using media::llm::path::ToolDef;

constexpr const char* k_names[] = {
    "browser_read",
    "browser_find",
    "browser_click",
    "browser_type",
    "browser_select",
    "browser_scope",
    "browser_batch",
    "browser_navigate",
};

ExecuteResult fatal(const std::string& message)
{
    ExecuteResult result;
    result.ok = false;
    result.error = message;
    result.envelope = nlohmann::json{{"ok", false}, {"error", message}};
    return result;
}

ExecuteResult tool_ok(const std::string& tool, nlohmann::json payload)
{
    payload["ok"] = true;
    payload["tool"] = tool;
    ExecuteResult result;
    result.ok = true;
    result.envelope = std::move(payload);
    return result;
}

bool looks_like_epoch(const std::string& value)
{
    static const std::regex pattern(
        R"(^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$)",
        std::regex::icase);
    return std::regex_match(value, pattern);
}

bool looks_like_ref(const std::string& value)
{
    static const std::regex pattern(R"(^e\d+$)");
    return std::regex_match(value, pattern);
}

std::optional<std::string> validate_target_args(const std::string& tool, const nlohmann::json& args)
{
    const bool has_ref = args.contains("ref") && args["ref"].is_string()
        && !args["ref"].get<std::string>().empty();
    const bool has_selector = args.contains("selector") && args["selector"].is_string()
        && !args["selector"].get<std::string>().empty();
    if (!has_ref && !has_selector)
        return tool + ": ref or selector is required";
    if (!has_ref)
        return std::nullopt;
    const std::string ref = args["ref"].get<std::string>();
    if (!looks_like_ref(ref))
        return tool + ": invalid ref — use ref from browser_read items (e.g. e42), not page text";
    // Epoch is optional (live page default). Near-miss typos are recovered in the WebView runtime.
    if (args.contains("epoch") && args["epoch"].is_string()) {
        const std::string epoch = args["epoch"].get<std::string>();
        if (!epoch.empty() && !looks_like_epoch(epoch))
            return tool + ": invalid epoch — omit it or copy the uuid from browser_read";
    }
    return std::nullopt;
}

nlohmann::json tab_property()
{
    return {
        {"type", "string"},
        {"description",
         "Centre browser tab id from the open-tabs list (dock id, e.g. \"12\"). "
         "Omit to use the current tab."},
    };
}

nlohmann::json target_properties()
{
    return {
        {"tab", tab_property()},
        {"ref", {
            {"type", "string"},
            {"description", "Opaque ref from browser_read/browser_find, for example e42."},
        }},
        {"epoch", {
            {"type", "string"},
            {"description",
             "Optional page epoch from browser_read. Omit to use the live page epoch; "
             "still required conceptually after navigation (re-read first)."},
        }},
        {"selector", {
            {"type", "string"},
            {"description", "CSS selector alternative to ref. Prefer refs returned by browser_read."},
        }},
        {"highlight", {
            {"type", "boolean"},
            {"description",
             "Show a brief on-page flash (corner brackets) at the target. Default true. "
             "Set false for invisible automation."},
        }},
    };
}

nlohmann::json scope_index_properties()
{
    return {
        {"tab", tab_property()},
        {"within_ref", {
            {"type", "string"},
            {"description", "Limit index/find to descendants of this container ref."},
        }},
        {"within_selector", {
            {"type", "string"},
            {"description", "Limit index/find to descendants of this CSS selector root."},
        }},
        {"scope", {
            {"type", "string"},
            {"enum", nlohmann::json::array({"set", "clear", "full"})},
            {"description",
             "set=persist within_ref/within_selector (or ref/selector) as the session zone; "
             "clear=drop zone; full=one-shot full-page bypass. set without a root fails."},
        }},
    };
}

void merge_scope_properties(nlohmann::json& props)
{
    const nlohmann::json scope_props = scope_index_properties();
    for (auto it = scope_props.begin(); it != scope_props.end(); ++it)
        props[it.key()] = it.value();
}

nlohmann::json batch_action(const char* action)
{
    return {
        {"type", "string"},
        {"enum", nlohmann::json::array({action})},
    };
}

nlohmann::json batch_variant(const char* action, nlohmann::json properties)
{
    properties["action"] = batch_action(action);
    nlohmann::json required = nlohmann::json::array();
    for (auto it = properties.begin(); it != properties.end(); ++it)
        required.push_back(it.key());
    return {
        {"type", "object"},
        {"properties", std::move(properties)},
        {"required", std::move(required)},
        {"additionalProperties", false},
    };
}

nlohmann::json batch_step_schema()
{
    const nlohmann::json compact{
        {"type", "boolean"},
        {"description", "For read: false returns slim refs; true returns summary only."},
    };
    const nlohmann::json ref{
        {"type", "string"},
        {"description", "Element ref from the latest read."},
    };
    const nlohmann::json selector{
        {"type", "string"},
        {"description", "CSS selector alternative to ref."},
    };
    const nlohmann::json text{{"type", "string"}};
    const nlohmann::json scope_set{
        {"type", "string"},
        {"enum", nlohmann::json::array({"set"})},
    };
    const nlohmann::json scope_full{
        {"type", "string"},
        {"enum", nlohmann::json::array({"full"})},
    };

    return {
        {"anyOf", nlohmann::json::array({
            batch_variant("click", {{"ref", ref}}),
            batch_variant("click", {{"selector", selector}}),
            batch_variant("wait", {{"ms", {
                {"type", "integer"}, {"minimum", 0}, {"maximum", 5000},
                {"description", "Sleep up to 5000ms without blocking the page (async)."},
            }}}),
            batch_variant("wait", {
                {"ms", {
                    {"type", "integer"}, {"minimum", 0}, {"maximum", 5000},
                    {"description", "Timeout while waiting for a condition."},
                }},
                {"until_revision_gt", {
                    {"type", "integer"},
                    {"description", "Resolve when page revision becomes greater than this value."},
                }},
            }),
            batch_variant("wait", {
                {"ms", {
                    {"type", "integer"}, {"minimum", 0}, {"maximum", 5000},
                    {"description", "Timeout while waiting for a selector."},
                }},
                {"until_selector", {
                    {"type", "string"},
                    {"description", "Resolve when this CSS selector matches a visible element."},
                }},
            }),
            batch_variant("read", {{"compact", compact}}),
            batch_variant("read", {
                {"compact", compact}, {"within_ref", ref}, {"scope", scope_set},
            }),
            batch_variant("read", {
                {"compact", compact}, {"within_selector", selector}, {"scope", scope_set},
            }),
            batch_variant("read", {{"compact", compact}, {"scope", scope_full}}),
            batch_variant("find", {
                {"role", text}, {"name", text}, {"limit", {
                    {"type", "integer"}, {"minimum", 1}, {"maximum", 100},
                }},
            }),
            batch_variant("find", {
                {"selector", selector}, {"limit", {
                    {"type", "integer"}, {"minimum", 1}, {"maximum", 100},
                }},
            }),
            batch_variant("type", {
                {"ref", ref}, {"text", text}, {"clear", {{"type", "boolean"}}},
                {"submit", {{"type", "boolean"}}},
            }),
            batch_variant("type", {
                {"selector", selector}, {"text", text}, {"clear", {{"type", "boolean"}}},
                {"submit", {{"type", "boolean"}}},
            }),
            batch_variant("select", {{"ref", ref}, {"value", text}}),
            batch_variant("select", {{"selector", selector}, {"value", text}}),
        })},
    };
}

ToolDef make_read()
{
    nlohmann::json properties = target_properties();
    properties["limit"] = {
        {"type", "integer"}, {"minimum", 1}, {"maximum", 40},
        {"description", "Maximum index rows. Default 24. Keep small after pinning a zone."},
    };
    properties["viewport_only"] = {
        {"type", "boolean"},
        {"description", "Filter to viewport-near elements (default true)."},
    };
    properties["compact"] = {
        {"type", "boolean"},
        {"description", "Summary only (omit item arrays). Use true for the first scout read."},
    };
#if defined(FEATURE_BROWSER_PAGE_MODEL) && FEATURE_BROWSER_PAGE_MODEL
    properties["query"] = {
        {"type", "string"},
        {"description",
         "Optional: BM25-rank model.zones into model.rankedZones (task / zone name). "
         "Use to pick a pin target without re-scouting the full index."},
    };
#endif
    merge_scope_properties(properties);
    return {
        "browser_read",
        "Read the current (or tab=) in-app browser tab index (ref, role, name). "
        "files[] lists visible images and links whose path looks like a filename; "
        "save one with download_url. "
        "Scout once with compact=true, then pin: within_ref=<zoneHint.ref|model.rankedZones[0].liveRef> scope=set. "
        "Later reads inherit the zone — do not scope=full to rediscover fields. "
        "scope=set without a root fails. Re-read after navigate / model.changedZones only. "
        "When model{} is present (including on click/type/select), prefer events/dismissCandidates "
        "over a full re-read.",
        {{"type", "object"}, {"properties", std::move(properties)}},
    };
}

ToolDef make_find()
{
    nlohmann::json properties = scope_index_properties();
    properties["selector"] = {{"type", "string"}, {"description", "Optional CSS selector."}};
    properties["role"] = {{"type", "string"}, {"description", "Optional role such as button, link, textbox."}};
    properties["name"] = {{"type", "string"}, {"description", "Optional case-insensitive accessible-name substring."}};
    properties["limit"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 20}};
    properties["viewport_only"] = {
        {"type", "boolean"},
        {"description", "Default true. Set false to search off-screen elements."},
    };
    return {
        "browser_find",
        "Find by selector or role/name only when you lack a ref from a scoped read. "
        "Do not use scope=full to re-scan the page — pin a zone with browser_read instead. "
        "Respects session zone.",
        {
            {"type", "object"},
            {"properties", std::move(properties)},
        },
    };
}

ToolDef make_click()
{
    return {
        "browser_click",
        "Click an element in the current (or tab=) in-app browser tab by ref (preferred) or CSS selector. "
        "Shows a brief visual flash on the target (highlight=true by default). "
        "Epoch is optional and defaults to the live page; re-read after navigation. "
        "Returns ref, epoch, revision, and post-mutation model when available.",
        {
            {"type", "object"},
            {"properties", target_properties()},
            {"anyOf", nlohmann::json::array({
                nlohmann::json{{"required", nlohmann::json::array({"ref"})}},
                nlohmann::json{{"required", nlohmann::json::array({"selector"})}},
            })},
        },
    };
}

ToolDef make_type()
{
    nlohmann::json properties = target_properties();
    properties["text"] = {{"type", "string"}, {"description", "Text to enter."}};
    properties["clear"] = {{"type", "boolean"}, {"description", "Replace existing text. Default true."}};
    properties["submit"] = {{"type", "boolean"}, {"description", "Submit the containing form afterwards."}};
    return {
        "browser_type",
        "Enter text into an input, textarea, or contenteditable element by ref or selector. "
        "Shows a brief visual flash on the target (highlight=true by default). "
        "Epoch is optional (live page default).",
        {
            {"type", "object"},
            {"properties", std::move(properties)},
            {"required", nlohmann::json::array({"text"})},
        },
    };
}

ToolDef make_select()
{
    nlohmann::json properties = target_properties();
    properties["value"] = {{"type", "string"}, {"description", "Option value."}};
    properties["label"] = {{"type", "string"}, {"description", "Exact option label alternative."}};
    properties["index"] = {{"type", "integer"}, {"description", "Zero-based option index alternative."}};
    return {
        "browser_select",
        "Select an option in a select element by ref or selector. "
        "Shows a brief visual flash on the target (highlight=true by default). "
        "Epoch is optional (live page default).",
        {
            {"type", "object"},
            {"properties", std::move(properties)},
        },
    };
}

ToolDef make_navigate()
{
    return {
        "browser_navigate",
        "Navigate a centre browser tab: goto/back/reload, list/switch/close tabs, or open a new tab. "
        "goto reuses an open tab on the same URL / origin+path / origin (unpinned) instead of stacking copies. "
        "Default target is the current real tab (skips about:blank). Pass tab=<id> for another. "
        "action=tabs lists open tabs. action=switch focuses tab=. action=close closes tab= or the current unpinned tab. "
        "action=new always opens a new tab. Close scratch tabs as you go. "
        "After goto/new/reload/switch the page epoch resets — call browser_read before click/type.",
        {
            {"type", "object"},
            {"properties", {
                {"action", {
                    {"type", "string"},
                    {"enum", nlohmann::json::array({"goto", "back", "reload", "tabs", "switch", "new", "close"})},
                    {"description",
                     "Default goto when url is set. tabs=list. switch=focus tab=. "
                     "close=close tab= or current unpinned. new=force a new tab."},
                }},
                {"url", {
                    {"type", "string"},
                    {"description", "For goto/new: absolute http(s) URL or CMS-relative path such as /user/…"},
                }},
                {"tab", tab_property()},
            }},
        },
    };
}

ToolDef make_scope()
{
    return {
        "browser_scope",
        "Inspect or clear the session zone. Prefer pinning via browser_read "
        "(within_ref + scope=set) so you get the zone index in the same call. "
        "action=status (default); action=clear drops the zone; action=set with ref/selector "
        "still works if you only need to pin without reading. Cleared on navigation/epoch reset.",
        {
            {"type", "object"},
            {"properties", {
                {"tab", tab_property()},
                {"action", {
                    {"type", "string"},
                    {"enum", nlohmann::json::array({"set", "clear", "status"})},
                    {"description", "Default status. Prefer browser_read for set+index."},
                }},
                {"ref", {{"type", "string"}, {"description", "Container ref for action=set."}}},
                {"epoch", {
                    {"type", "string"},
                    {"description", "Optional page epoch for ref on action=set; defaults to live page."},
                }},
                {"selector", {{"type", "string"}, {"description", "CSS root for action=set."}}},
                {"within_selector", {{"type", "string"}, {"description", "Alias for selector on action=set."}}},
            }},
        },
    };
}

ToolDef make_batch()
{
    return {
        "browser_batch",
        "Execute a sequence of WebView actions in ONE tool call (read, find, click, type, select, wait). "
        "Do not mix navigation. Use steps only (aliases are not needed). "
        "After click/type/select, a compact read is auto-appended if the batch has no read step. "
        "wait sleeps on the host (page keeps running); optional until_revision_gt. "
        "Pin a zone with a read step: within_ref + scope=set. "
        "Terminal read item lists are capped. The batch stops on first failure. "
        "Epoch is optional (live page default); hard mismatch means the page navigated — re-read.",
        {
            {"type", "object"},
            {"properties", {
                {"tab", tab_property()},
                {"steps", {
                    {"type", "array"},
                    {"description", "Ordered action-specific step objects."},
                    {"minItems", 1},
                    {"items", batch_step_schema()},
                }},
                {"epoch", {
                    {"type", "string"},
                    {"description",
                     "Optional page epoch from browser_read; omit to use the live page. "
                     "Near-miss typos are recovered; a true page change still aborts."},
                }},
            }},
            {"required", nlohmann::json::array({"steps"})},
            {"additionalProperties", false},
        },
    };
}

nlohmann::json make_request(const std::string& name, const nlohmann::json& args)
{
    static std::atomic<unsigned long long> next_id{1};
    std::string command;
    if (name == "browser_read") command = "read";
    else if (name == "browser_find") command = "find";
    else if (name == "browser_scope") command = "scope";
    else if (name == "browser_click") command = "click";
    else if (name == "browser_type") command = "type";
    else if (name == "browser_select") command = "select";

    const bool ref_read = args.contains("ref") && args["ref"].is_string()
        && !args["ref"].get<std::string>().empty();
    const bool selector_read = args.contains("selector") && args["selector"].is_string()
        && !args["selector"].get<std::string>().empty();
    const bool index_scope = (command == "read" || command == "find") && !ref_read && !selector_read;

    nlohmann::json request{
        {"v", 2},
        {"id", "agent-" + std::to_string(next_id.fetch_add(1))},
        {"cmd", command},
        {"args", nlohmann::json::object()},
    };
    if (args.contains("epoch") && args["epoch"].is_string())
        request["epoch"] = args["epoch"];
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (it.key() == "epoch" || it.key() == "tab")
            continue;
#if defined(FEATURE_BROWSER_PAGE_MODEL) && FEATURE_BROWSER_PAGE_MODEL
        // Host-only BM25 query — do not forward into the inject script.
        if (it.key() == "query")
            continue;
#endif
        std::string key = it.key();
        if (key == "include_geometry" && index_scope)
            continue;
        if (key == "viewport_only") key = "viewportOnly";
        else if (key == "text_max_chars") key = "textMaxChars";
        else if (key == "viewport_threshold") key = "viewportThreshold";
        else if (key == "include_geometry") key = "includeGeometry";
        else if (key == "include_html") key = "includeHtml";
        else if (key == "max_index_chars") key = "maxIndexChars";
        else if (key == "within_ref") key = "withinRef";
        else if (key == "within_selector") key = "withinSelector";
        else if (key == "persist_scope") key = "persistScope";
        else if (key == "clear_scope") key = "clearScope";
        else if (key == "full_page") key = "fullPage";
        request["args"][key] = it.value();
    }
    if (index_scope) {
        request["args"]["includeGeometry"] = false;
        request["args"]["includeHtml"] = false;
    }
    return request;
}

#if defined(_WIN32) && defined(FEATURE_BROWSER) && FEATURE_BROWSER
std::string tab_arg(const nlohmann::json& args)
{
    if (args.contains("tab") && args["tab"].is_string())
        return args["tab"].get<std::string>();
    if (args.contains("tab") && args["tab"].is_number_integer())
        return std::to_string(args["tab"].get<int>());
    return {};
}

nlohmann::json tabs_payload()
{
    nlohmann::json tabs = nlohmann::json::array();
    std::string current;
    for (const auto& tab : pm::host::browser_use::list_tabs()) {
        tabs.push_back({
            {"id", tab.id},
            {"url", tab.url},
            {"title", tab.title},
            {"current", tab.current},
            {"pinned", tab.pinned},
        });
        if (tab.current)
            current = tab.id;
    }
    nlohmann::json payload{
        {"action", "tabs"},
        {"tabs", std::move(tabs)},
        {"count", 0},
    };
    payload["count"] = payload["tabs"].size();
    if (!current.empty())
        payload["tab"] = current;
    return payload;
}

ExecuteResult do_browser_navigate(const nlohmann::json& args)
{
    std::string action;
    if (args.contains("action") && args["action"].is_string())
        action = args["action"].get<std::string>();
    else if (args.contains("url") && args["url"].is_string() && !args["url"].get<std::string>().empty())
        action = "goto";
    else
        return fatal("browser_navigate: action is required (goto, back, reload, tabs, switch, new, close)");

    if (action == "tabs" || action == "list")
        return tool_ok("browser_navigate", tabs_payload());

    std::string url;
    if (args.contains("url") && args["url"].is_string())
        url = args["url"].get<std::string>();
    const std::string tab = tab_arg(args);

    media::win::browser_navigate::NavigateResult nav;
    std::string err;
    if (!media::win::browser_navigate::navigate(action, url, nav, err, tab))
        return fatal(err.empty() ? "browser_navigate failed" : ("browser_navigate: " + err));

    nlohmann::json payload{
        {"action", nav.action},
        {"url", nav.url},
        {"title", nav.title},
        {"epoch", nav.epoch},
    };
    if (!nav.tab.empty())
        payload["tab"] = nav.tab;
    else if (!tab.empty())
        payload["tab"] = tab;
    if (nav.reused)
        payload["reused"] = true;
    return tool_ok("browser_navigate", std::move(payload));
}

ExecuteResult do_browser_batch(const nlohmann::json& args)
{
    media::win::browser_batch::RunOptions opts;
    if (args.contains("default_delay_ms") && args["default_delay_ms"].is_number_integer())
        opts.default_delay_ms = args["default_delay_ms"].get<int>();
    if (args.contains("continue_on_error") && args["continue_on_error"].is_boolean())
        opts.stop_on_error = !args["continue_on_error"].get<bool>();

    nlohmann::json report;
    std::string err;
    if (!media::win::browser_batch::run_json_batch(args, opts, report, err))
        return fatal(err.empty() ? "browser_batch failed" : ("browser_batch: " + err));

    nlohmann::json payload = report.is_object() ? std::move(report) : nlohmann::json::object();
    bool all_ok = payload.value("all_ok", false);
    if (!err.empty())
        payload["batch_error"] = err;
    if (!all_ok && !payload.contains("first_failure")) {
        for (const auto& step : payload.value("steps", nlohmann::json::array())) {
            if (step.is_object() && !step.value("ok", true)) {
                payload["first_failure"] = step;
                break;
            }
        }
    }
    std::string failure = !err.empty()
        ? err
        : std::string{"browser_batch: one or more steps failed"};
    payload["ok"] = all_ok;
    payload["tool"] = "browser_batch";
    if (!all_ok)
        payload["error"] = failure;
    nlohmann::json envelope = std::move(payload);
    // Scan before history compaction — compact drops pageText/items that carry bait.
    if (auto hit = detect_prompt_injection_in_envelope(envelope)) {
        apply_prompt_injection_guard(envelope, *hit);
        all_ok = false;
        failure = envelope.value("error", failure);
    }
    envelope = compact_tool_envelope_for_history(std::move(envelope));
    ExecuteResult result;
    result.ok = all_ok;
    if (!all_ok)
        result.error = failure;
    result.envelope = std::move(envelope);
    return result;
}
#endif

} // namespace

std::vector<ToolDef> tool_defs()
{
#if defined(_WIN32) && defined(FEATURE_BROWSER) && FEATURE_BROWSER \
    && defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT \
    && defined(FEATURE_TOOLS_APP_USE) && FEATURE_TOOLS_APP_USE \
    && defined(FEATURE_AGENT_COMPUTER_USE) && FEATURE_AGENT_COMPUTER_USE
    return {make_read(), make_find(), make_scope(), make_click(), make_type(), make_select(), make_navigate(), make_batch()};
#else
    return {};
#endif
}

bool is_browser_use_tool(const std::string& name)
{
    for (const char* candidate : k_names)
        if (name == candidate)
            return true;
    return false;
}

ExecuteResult execute(const std::string& name, const nlohmann::json& args)
{
    if (!is_browser_use_tool(name))
        return fatal("unknown browser-use tool: " + name);
    if (!pm::policy::browser_use_allowed())
        return fatal(name + ": browser-use is disabled by policy.");
    if (!args.is_object())
        return fatal(name + ": arguments must be an object");
#if !defined(_WIN32) || !defined(FEATURE_BROWSER) || !FEATURE_BROWSER
    return fatal(name + ": browser-use requires Windows WebView2 support");
#else
    if (!pm::host::browser_use::enabled())
        return fatal(name + ": browser-use is unavailable in this build");

    if (name == "browser_batch") {
        if (args.contains("epoch") && args["epoch"].is_string()) {
            const std::string epoch = args["epoch"].get<std::string>();
            if (!epoch.empty() && !looks_like_epoch(epoch))
                return fatal("browser_batch: invalid epoch — omit it or copy the uuid from browser_read");
        }
        return do_browser_batch(args);
    }
    if (name == "browser_navigate")
        return do_browser_navigate(args);

    if (name == "browser_click" || name == "browser_type" || name == "browser_select") {
        if (const auto validation_err = validate_target_args(name, args))
            return fatal(*validation_err);
    }

    std::string raw;
    std::string err;
    const nlohmann::json request = make_request(name, args);
    if (!pm::host::browser_use::rpc_for_tab(tab_arg(args), request.dump(), raw, 15000, err))
        return fatal(name + ": " + (err.empty() ? "WebView RPC failed" : err));

    try {
        nlohmann::json envelope = nlohmann::json::parse(raw);
        if (envelope.is_string())
            envelope = nlohmann::json::parse(envelope.get<std::string>());
        if (!envelope.is_object())
            return fatal(name + ": empty WebView response (script missing or returned non-object)");
        if (!envelope.contains("ok"))
            return fatal(name + ": malformed WebView response (missing ok)");
        envelope["tool"] = name;
        if (const std::string tab = tab_arg(args); !tab.empty())
            envelope["tab"] = tab;
#if defined(FEATURE_BROWSER_PAGE_MODEL) && FEATURE_BROWSER_PAGE_MODEL
        if (args.contains("query") && args["query"].is_string()) {
            const std::string q = args["query"].get<std::string>();
            if (!q.empty())
                enrich_envelope_page_model(envelope, q);
        }
#endif
        // Scan before history compaction — compact drops pageText/items that carry bait.
        if (auto hit = detect_prompt_injection_in_envelope(envelope))
            apply_prompt_injection_guard(envelope, *hit);
        envelope = compact_tool_envelope_for_history(std::move(envelope));
        ExecuteResult result;
        result.ok = envelope.value("ok", false);
        result.envelope = std::move(envelope);
        if (!result.ok) {
            const auto& error = result.envelope["error"];
            result.error = error.is_object()
                ? error.value("message", std::string{"browser-use command failed"})
                : error.is_string()
                    ? error.get<std::string>()
                    : std::string{"browser-use command failed"};
        }
        return result;
    } catch (const std::exception& ex) {
        logger::warn(std::string("[browser-use] invalid RPC response: ") + ex.what());
        return fatal(name + ": invalid WebView response");
    }
#endif
}

} // namespace media::llm::browser_use

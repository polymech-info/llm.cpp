#include "InfoTool.hpp"
#include "fs/path_sanitizer.hpp"

#include "AppFinder.hpp"
#include "constants.hpp"
#include "core/app_exe_directory.hpp"
#include "cli/pm_image_command_info.hpp"
#include "commands/command_registry.hpp"
#if defined(_WIN32)
#include "core/host_backends.hpp"
#include "win/app_state.hpp"
#endif
#include "llm/llm_info_compact.hpp"
#include "llm/path_tool_catalog.hpp"
#include "llm/agent_memory.hpp"
#include "llm/agent_resources.hpp"
#include "string/string_utils.hpp"
#if defined(FEATURE_COMMAND_LLAMA_INFERENCE) && FEATURE_COMMAND_LLAMA_INFERENCE
#include "lib/llm/llama/EmbedInference.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace media::llm::info_tool {
namespace {

#if defined(_WIN32)
namespace catalog = pm::host::cli;
#else
namespace catalog = pm::cli;
#endif

// Text + fuzzy-matching helpers moved to src/string_utils.hpp.
using media::strutil::lower_ascii;
using media::strutil::trim;
using media::strutil::match_score;
using media::strutil::matches_query;

// Branded CLI basename (e.g. "tanit-cli.exe") for agent-facing invocation hints.
const std::string& cli_exe()
{
    static const std::string s = pm::brand::k_exe_basename_cli_u8;
    return s;
}

std::string str_arg(const nlohmann::json& args, const char* key, const char* fallback = "")
{
    if (args.contains(key) && args[key].is_string())
        return args[key].get<std::string>();
    return fallback;
}

bool bool_arg(const nlohmann::json& args, const char* key, bool fallback = false)
{
    if (args.contains(key) && args[key].is_boolean())
        return args[key].get<bool>();
    return fallback;
}

media::llm::path::ExecuteResult ok(nlohmann::json payload)
{
    media::llm::path::ExecuteResult r;
    r.ok = true;
    payload["ok"] = true;
    r.envelope = std::move(payload);
    return r;
}

media::llm::path::ExecuteResult err(const std::string& msg)
{
    media::llm::path::ExecuteResult r;
    r.ok = false;
    r.error = msg;
    r.envelope = nlohmann::json{{"ok", false}, {"error", msg}};
    return r;
}

nlohmann::json command_options_summary(const std::vector<pm::cli::CommandOptionInfo>& options)
{
    nlohmann::json flags = nlohmann::json::array();
    nlohmann::json positionals = nlohmann::json::array();
    for (const auto& opt : options) {
        if (opt.name.empty())
            continue;
        nlohmann::json item{
            {"name", opt.name},
            {"type", opt.type_name},
            {"description", opt.description},
            {"default", opt.default_value},
            {"required", opt.required},
        };
        if (opt.positional)
            positionals.push_back(std::move(item));
        else
            flags.push_back(std::move(item));
    }
    return {
        {"flags", std::move(flags)},
        {"positionals", std::move(positionals)},
        {"source", "runtime CLI11 command registry"},
        {"rule", "Only options with positional=false are CLI flags. Positional arguments are written by position, not as --flags."},
    };
}

nlohmann::json command_usage_summary(const pm::cli::CommandDetailInfo& command, const nlohmann::json& options)
{
    nlohmann::json flags = nlohmann::json::array();
    nlohmann::json positionals = nlohmann::json::array();
    nlohmann::json must_not = nlohmann::json::array();

    if (options.contains("flags") && options["flags"].is_array()) {
        for (const auto& item : options["flags"]) {
            const std::string name = item.value("name", std::string{});
            if (!name.empty())
                flags.push_back(name);
        }
    }
    if (options.contains("positionals") && options["positionals"].is_array()) {
        for (const auto& item : options["positionals"]) {
            const std::string name = item.value("name", std::string{});
            if (name.empty())
                continue;
            positionals.push_back(name);
            must_not.push_back("--" + name);
        }
    }

    return {
        {"usage", command.usage.empty() ? (cli_exe() + " " + command.id + " [OPTIONS]") : command.usage},
        {"flags", std::move(flags)},
        {"positionals", std::move(positionals)},
        {"must_not_use_flags", std::move(must_not)},
        {"rule", "Use exactly this usage shape. Anything in positionals is written by position, not as a --flag."},
    };
}

nlohmann::json lookup_commands(const std::string& query, bool detail)
{
    const std::string q = trim(query);
    const std::string q_lower = lower_ascii(q);
    std::vector<std::pair<int, nlohmann::json>> matches;
    const auto command_details = catalog::registered_cli_command_details();
    std::unordered_set<std::string> custom_ids;
    for (const auto& c : catalog::visible_custom_commands()) {
        if (!c.id.empty())
            custom_ids.insert(lower_ascii(c.id));
    }

    for (const auto& c : command_details) {
        if (custom_ids.count(lower_ascii(c.id)))
            continue;
        const std::string hay = c.id + " " + c.label + " " + c.description;
        const std::string id_lower = lower_ascii(c.id);
        const std::string label_lower = lower_ascii(c.label);
        // A command id appearing verbatim in the query (e.g. "...pencil sketch
        // transform" -> id "transform") is a decisive signal even when the rest
        // of the query is descriptive noise that no command text contains.
        const bool id_in_query = !id_lower.empty() && q_lower.find(id_lower) != std::string::npos;
        const double coverage = match_score(hay, q);
        bool passed_threshold = (coverage >= 0.5 || id_in_query);
#if defined(FEATURE_COMMAND_LLAMA_INFERENCE) && FEATURE_COMMAND_LLAMA_INFERENCE
        // Semantic fallback. The glossary does not equate camera/webcam with
        // video, so "webcam capture" reaches a video command only through this
        // path. Paid only when the token scorer already failed.
        float embed_coverage = 0.f;
        if (!passed_threshold && media::strutil::has_embed_backend()) {
            embed_coverage = media::strutil::embed_match_score(q, hay);
            if (embed_coverage >= 0.50f)
                passed_threshold = true;
        }
#endif
        if (!passed_threshold)
            continue;
        int score = 10;
        if (!q_lower.empty() && (id_lower == q_lower || label_lower == q_lower))
            score = 100;
        else if (id_in_query || (!q_lower.empty() && (id_lower.find(q_lower) != std::string::npos
                || label_lower.find(q_lower) != std::string::npos)))
            score = 80;
        else {
#if defined(FEATURE_COMMAND_LLAMA_INFERENCE) && FEATURE_COMMAND_LLAMA_INFERENCE
            // Blend fuzzy + embed into the score bucket (10–60 range).
            const double best = (embed_coverage > coverage) ? embed_coverage : coverage;
            score = 10 + static_cast<int>(best * 50.0);
#else
            score = 10 + static_cast<int>(coverage * 50.0); // 10-60 by coverage
#endif
        }
        nlohmann::json row{
            {"kind", "command"},
            {"id", c.id},
            {"label", c.label},
            {"description", c.description},
            {"available", c.available},
            {"invocation", cli_exe() + " " + c.id + " [args]"},
            {"help_command", cli_exe() + " " + c.id + " --help"},
        };
        if (detail) {
            nlohmann::json options = command_options_summary(c.options);
            row["usage_summary"] = command_usage_summary(c, options);
            row["options_summary"] = std::move(options);
        }
        matches.push_back({score, std::move(row)});
    }

    if (command_details.empty()) {
        for (const auto& c : catalog::registered_cli_commands()) {
            const std::string id = c.id ? c.id : "";
            const std::string label = c.label ? c.label : "";
            const std::string hay = id + " " + label;
            if (!matches_query(hay, q))
                continue;
            int score = 10;
            const std::string id_lower = lower_ascii(id);
            const std::string label_lower = lower_ascii(label);
            if (!q_lower.empty() && (id_lower == q_lower || label_lower == q_lower))
                score = 100;
            else if (!q_lower.empty() && (id_lower.find(q_lower) != std::string::npos
                    || label_lower.find(q_lower) != std::string::npos))
                score = 80;
            matches.push_back({score, {
                {"kind", "command"},
                {"id", id},
                {"label", label},
                {"available", c.available},
                {"invocation", cli_exe() + " " + id + " [args]"},
                {"help_command", cli_exe() + " " + id + " --help"},
            }});
        }
    }

    for (const auto& c : catalog::visible_custom_commands()) {
        const std::string hay = c.id + " " + c.label + " " + c.description + " "
            + c.command_name + " " + c.type + " " + c.action + " " + c.group;
        if (!matches_query(hay, q))
            continue;
        int score = 20;
        const std::string id_lower = lower_ascii(c.id);
        const std::string label_lower = lower_ascii(c.label);
        if (!q_lower.empty() && (id_lower == q_lower || label_lower == q_lower))
            score = 100;
        else if (!q_lower.empty() && (id_lower.find(q_lower) != std::string::npos
                || label_lower.find(q_lower) != std::string::npos))
            score = 90;
        else
            score = 55 + static_cast<int>(match_score(hay, q) * 40.0);
        const std::string blob = c.command_name + " " + [&] {
            std::string joined;
            for (const auto& a : c.args) {
                if (!joined.empty()) joined.push_back(' ');
                joined += a;
            }
            return joined;
        }();
        const bool binds_file = blob.find("${CURRENT_FILE}") != std::string::npos
            || blob.find("${SRC_") != std::string::npos
            || blob.find("${PATH_") != std::string::npos;
        std::string invocation = cli_exe() + " " + c.id;
        if (binds_file)
            invocation += " --CURRENT_FILE <path>";
        nlohmann::json row{
            {"kind", "custom_command"},
            {"id", c.id},
            {"label", c.label},
            {"group", c.group},
            {"type", c.type},
            {"action", c.action},
            {"command", c.command_name},
            {"args", c.args},
            {"invocation", std::move(invocation)},
        };
        if (!c.description.empty())
            row["description"] = c.description;
        if (binds_file)
            row["file_flag"] = "--CURRENT_FILE";
        matches.push_back({score, std::move(row)});
    }
    std::stable_sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
        return a.first > b.first;
    });
    nlohmann::json rows = nlohmann::json::array();
    for (auto& m : matches)
        rows.push_back(std::move(m.second));
    return rows;
}

#if defined(_WIN32)
bool app_command_available(const media::commands::CommandDescriptor& d)
{
    const auto cmd = pm::host::app_cmd::parse(d.id);
    return cmd != pm::host::app_cmd::Command::Unknown
        && pm::host::app_cmd::is_available(cmd);
}
#else
bool app_command_available(const media::commands::CommandDescriptor&)
{
    return false;
}
#endif

std::string app_command_haystack(const media::commands::CommandDescriptor& d)
{
    std::string hay = d.id + " " + d.label + " " + d.description + " "
        + media::commands::registry().group_label(d);
    for (const auto& alias : d.aliases) {
        hay.push_back(' ');
        hay += alias;
    }
    return hay;
}

nlohmann::json lookup_app_commands(const std::string& query, bool detail, bool list_all)
{
    const std::string q = trim(query);
    const std::string q_lower = lower_ascii(q);
    std::vector<std::pair<int, nlohmann::json>> matches;
    auto& reg = media::commands::registry();
    for (const auto* d : reg.list_in_group_order()) {
        if (!d || d->implementation != "app" || !app_command_available(*d))
            continue;
        const std::string hay = app_command_haystack(*d);
        const std::string id_lower = lower_ascii(d->id);
        const std::string label_lower = lower_ascii(d->label);
        const bool id_in_query = !id_lower.empty() && !q_lower.empty()
            && q_lower.find(id_lower) != std::string::npos;
        if (!list_all && !id_in_query && !matches_query(hay, q))
            continue;
        int score = list_all ? 10 : 20;
        if (!q_lower.empty() && (id_lower == q_lower || label_lower == q_lower))
            score = 100;
        else if (id_in_query || (!q_lower.empty() && (id_lower.find(q_lower) != std::string::npos
                || label_lower.find(q_lower) != std::string::npos)))
            score = 80;
        matches.push_back({score, reg.project_lookup_item(*d, detail)});
    }
    std::stable_sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
        return a.first > b.first;
    });
    nlohmann::json rows = nlohmann::json::array();
    for (auto& m : matches)
        rows.push_back(std::move(m.second));
    return rows;
}

bool is_mcp_list_query(const std::string& query)
{
    const std::string q = lower_ascii(trim(query));
    return q.empty()
        || q == "mcp" || q == "mcps" || q == "mcp_server" || q == "mcp_servers"
        || q == "mcp_server__tool" || q == "list" || q == "servers"
        || q == "all" || q == "tools";
}

nlohmann::json lookup_mcp_tools(const std::string& query, bool detail)
{
    const bool list_all = is_mcp_list_query(query);
    nlohmann::json rows = nlohmann::json::array();
    for (const auto& t : media::llm::info::mcp_tool_inventory(true)) {
        const std::string hay = t.function_name + " " + t.server_name + " " + t.remote_name + " " + t.description;
        if (!list_all && !matches_query(hay, query))
            continue;
        nlohmann::json row{
            {"kind", "mcp_tool"},
            {"name", t.function_name},
            {"server", t.server_name},
            {"remote_tool", t.remote_name},
            {"description", t.description},
            {"policy_labels", t.policy_labels},
        };
        if (detail && !t.input_schema_json.empty()) {
            try {
                row["input_schema"] = nlohmann::json::parse(t.input_schema_json);
            } catch (...) {
                row["input_schema_json"] = t.input_schema_json;
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

nlohmann::json lookup_tools(const std::string& query, bool detail)
{
    const std::string q = trim(query);
    const std::string q_lower = lower_ascii(q);
    nlohmann::json rows = nlohmann::json::array();

    for (const auto& t : media::llm::path::tool_catalog()) {
        const std::string hay = lower_ascii(t.name + " " + t.description);
        if (!q_lower.empty()) {
            const bool substr_hit = (hay.find(q_lower) != std::string::npos);
#if defined(FEATURE_COMMAND_LLAMA_INFERENCE) && FEATURE_COMMAND_LLAMA_INFERENCE
            // A1: semantic fallback — catch synonyms the substring misses
            // (e.g. "speak" → audio_transcribe, "detect faces" → image_understand).
            // Only evaluated when the backend is warm; cheap guard avoids the
            // embed call for the common case where substring already matched.
            const bool embed_hit = !substr_hit
                && media::strutil::has_embed_backend()
                && media::strutil::embed_match_score(q, t.name + " " + t.description) >= 0.50f;
#else
            constexpr bool embed_hit = false;
#endif
            if (!substr_hit && !embed_hit)
                continue;
        }
        nlohmann::json row{
            {"kind", "tool"},
            {"name", t.name},
            {"description", t.description},
        };
        if (detail) {
            row["input_schema"] = t.input_schema;
            row["schema_source"] = "runtime path tool catalog";
        }
        rows.push_back(std::move(row));
    }

    for (auto& row : lookup_mcp_tools(q, detail))
        rows.push_back(std::move(row));
    return rows;
}

nlohmann::json lookup_apps(const std::string& query, int max_results)
{
    nlohmann::json rows = nlohmann::json::array();
    auto matches = media::llm::info::find_installed_apps(query, max_results);
    for (auto& m : matches) {
        rows.push_back({
            {"kind",        "app"},
            {"name",        m.name},
            {"exe",         m.exe},
            {"install_dir", m.install_dir},
            {"source",      m.source},
            {"confidence",  m.confidence},
        });
    }
    const std::string want = media::strutil::lower_ascii(query);
    bool exact = false;
    for (const auto& row : rows) {
        const std::string exe = row.value("exe", std::string{});
        const auto slash = exe.find_last_of("\\/");
        std::string base = slash == std::string::npos ? exe : exe.substr(slash + 1);
        const auto dot = base.find_last_of('.');
        if (dot != std::string::npos)
            base.resize(dot);
        if (media::strutil::lower_ascii(base) == want) {
            exact = true;
            break;
        }
    }
    std::string command = query;
    auto resolved = media::app::resolve_third_party_executable(query);
    if (resolved.empty() && query.find('_') != std::string::npos) {
        std::string hyphens = query;
        for (char& c : hyphens)
            if (c == '_') c = '-';
        resolved = media::app::resolve_third_party_executable(hyphens);
        if (!resolved.empty())
            command = std::move(hyphens);
    }
    if (resolved.empty() && query.find('-') != std::string::npos) {
        std::string underscores = query;
        for (char& c : underscores)
            if (c == '-') c = '_';
        resolved = media::app::resolve_third_party_executable(underscores);
        if (!resolved.empty())
            command = std::move(underscores);
    }
    const bool bundled = !resolved.empty()
        && !media::app::exe_bin_directory().empty()
        && resolved.parent_path() == media::app::exe_bin_directory();
    if (bundled) {
        for (auto it = rows.begin(); it != rows.end();) {
            const std::string exe = it->value("exe", std::string{});
            const auto slash = exe.find_last_of("\\/");
            std::string base = slash == std::string::npos ? exe : exe.substr(slash + 1);
            const auto dot = base.find_last_of('.');
            if (dot != std::string::npos)
                base.resize(dot);
            if (media::strutil::lower_ascii(base) == want)
                it = rows.erase(it);
            else
                ++it;
        }
        rows.insert(rows.begin(), {
            {"kind",       "app"},
            {"name",       command},
            {"exe",        media::path::path_to_utf8(resolved)},
            {"command",    command},
            {"install_dir", ""},
            {"source",     "tanit-bin"},
            {"confidence", 1.0},
            {"next_step",  "Run command=\"" + command + "\". Do not embed the absolute path. Do not create a command."},
        });
        return rows;
    }
    if (!exact && !resolved.empty()) {
        rows.push_back({
            {"kind",       "app"},
            {"name",       command},
            {"exe",        media::path::path_to_utf8(resolved)},
            {"command",    command},
            {"install_dir", ""},
            {"source",     "path"},
            {"confidence", 1.0},
            {"next_step",  "Run command=\"" + command + "\". Do not embed the absolute path. Do not create a command."},
        });
    }
    return rows;
}

media::llm::path::ExecuteResult lookup_resources(
    const std::string& query,
    const std::string& kind,
    int max_results,
    const std::string& cwd_hint,
    const nlohmann::json& args)
{
    skills::SkillPolicy policy;
    const auto snapshot = resources::discover(policy, cwd_hint);
    resources::index_resources(snapshot.entries);
    if (const auto exact = resources::resolve(query, snapshot.entries)) {
        const auto& resource = snapshot.entries[*exact];
        if (kind == "skill" && resource.kind != resources::ResourceKind::Skill)
            return err("info_lookup: exact resource is not a skill");
        const std::size_t byte_limit = static_cast<std::size_t>(
            std::max(1024, std::min(96 * 1024, args.value("max_bytes", 24 * 1024))));
        const int depth = std::max(0, std::min(4, args.value("depth", 0)));
        auto payload = resources::materialize(
            resource, byte_limit, depth, agent::get_current_task_id());
        if (!payload.value("ok", false))
            return err("info_lookup: " + payload.value("error", std::string{"resource unavailable"}));
        return ok({
            {"tool", "info_lookup"},
            {"query", query},
            {"kind", kind},
            {"materialized", true},
            {"resource", std::move(payload)},
        });
    }

    nlohmann::json rows = nlohmann::json::array();
    const auto ranked = resources::rank(query, snapshot.entries,
                                        static_cast<std::size_t>(max_results));
    for (const std::size_t index : ranked) {
        const auto& resource = snapshot.entries[index];
        if (kind == "skill" && resource.kind != resources::ResourceKind::Skill)
            continue;
        rows.push_back(resources::metadata_json(resource));
    }
    if (rows.empty())
        return err("info_lookup: no resource matches for query: " + query);
    return ok({
        {"tool", "info_lookup"},
        {"query", query},
        {"kind", kind},
        {"materialized", false},
        {"instruction", "Call info_lookup again with an exact uri or qualified_name to materialize one body."},
        {"results", std::move(rows)},
    });
}

media::llm::path::ExecuteResult lookup_app_state(const std::string& query)
{
#if defined(_WIN32)
    nlohmann::json state;
    std::string err;
    if (!media::win::app_state::query_from_primary(state, err)) {
        nlohmann::json payload{
            {"running", false},
            {"error", err.empty() ? std::string{"no running UI instance"} : err},
        };
        if (state.is_object()) {
            for (auto it = state.begin(); it != state.end(); ++it)
                payload[it.key()] = it.value();
        }
        return ok({
            {"tool", "info_lookup"},
            {"kind", "app_state"},
            {"query", query},
            {"state", std::move(payload)},
        });
    }

    const std::string q = lower_ascii(trim(query));
    if (!q.empty() && state.contains("commands") && state["commands"].is_array()) {
        nlohmann::json filtered = nlohmann::json::array();
        for (const auto& cmd : state["commands"]) {
            if (!cmd.is_object())
                continue;
            const std::string hay = lower_ascii(
                cmd.value("name", std::string{}) + " " + cmd.value("label", std::string{}) + " "
                + cmd.value("group", std::string{}));
            if (hay.find(q) != std::string::npos)
                filtered.push_back(cmd);
        }
        state["commands"] = std::move(filtered);
        state["commands_filtered"] = true;
    }

    return ok({
        {"tool", "info_lookup"},
        {"kind", "app_state"},
        {"query", query},
        {"state", std::move(state)},
    });
#else
    (void)query;
    return err("info_lookup kind=app_state requires Windows");
#endif
}

} // namespace

media::llm::path::ExecuteResult execute(const nlohmann::json& args,
                                        const std::string& cwd_hint)
{
    std::string query = str_arg(args, "query", "");
    if (query.empty())
        query = str_arg(args, "name", "");
    query = trim(std::move(query));
    const std::string kind = lower_ascii(trim(str_arg(args, "kind", "auto")));
    const bool detail = bool_arg(args, "detail", true);
    const int max_results = std::max(1, std::min(50, args.value("max_results", 8)));

    if (kind == "resource" || kind == "resources" || kind == "skill" || kind == "skills")
        return lookup_resources(query, kind == "skills" ? "skill" :
            (kind == "resources" ? "resource" : kind), max_results, cwd_hint, args);

    if (kind == "app_state" || kind == "appstate" || kind == "ui")
        return lookup_app_state(query);

    nlohmann::json results = nlohmann::json::array();
    const bool mcp_only = (kind == "mcp" || kind == "mcps");
    if (mcp_only) {
        for (auto& row : lookup_mcp_tools(query, detail)) {
            if (static_cast<int>(results.size()) >= max_results) break;
            results.push_back(std::move(row));
        }
    }
    if (!mcp_only && (kind == "auto" || kind == "all" || kind == "command" || kind == "commands"
                      || kind == "app_command" || kind == "app_commands")) {
        if (kind != "app_command" && kind != "app_commands") {
            for (auto& row : lookup_commands(query, detail)) {
                if (static_cast<int>(results.size()) >= max_results) break;
                results.push_back(std::move(row));
            }
        }
        const bool app_only = (kind == "app_command" || kind == "app_commands");
        for (auto& row : lookup_app_commands(query, detail, app_only && query.empty())) {
            if (static_cast<int>(results.size()) >= max_results) break;
            results.push_back(std::move(row));
        }
    }
    if (!mcp_only && (kind == "auto" || kind == "all" || kind == "tool" || kind == "tools")) {
        for (auto& row : lookup_tools(query, detail)) {
            if (static_cast<int>(results.size()) >= max_results) break;
            results.push_back(std::move(row));
        }
    }
    if (kind == "app" || kind == "apps") {
        for (auto& row : lookup_apps(query, max_results)) {
            if (static_cast<int>(results.size()) >= max_results) break;
            results.push_back(std::move(row));
        }
    }
    // kind=tool/auto searches the path-tool catalog, not ui.appPathDirectories.
    // A short local model asks kind=tool for an executable name, gets
    // matched=false, and reports the program as missing. If the name resolves
    // through the same folders `run` prepends to PATH, return that hit here.
    if (results.empty() && !query.empty() && !mcp_only &&
        (kind == "auto" || kind == "all" || kind == "tool" || kind == "tools" ||
         kind == "command" || kind == "commands")) {
        for (auto& row : lookup_apps(query, max_results)) {
            const std::string source = row.value("source", std::string{});
            if (source != "path" && source != "tanit-bin")
                continue;
            results.push_back(std::move(row));
            break;
        }
    }

    if (results.empty()) {
        nlohmann::json mcp_names = nlohmann::json::array();
        for (const auto& t : media::llm::info::mcp_tool_inventory(true))
            mcp_names.push_back(t.function_name);
        nlohmann::json payload{
            {"tool", "info_lookup"},
            {"query", query},
            {"kind", kind},
            {"matched", false},
            {"results", nlohmann::json::array()},
            {"mcp_tools", mcp_names},
            {"answer_rules", nlohmann::json::array({
                "info_lookup is a local Tanit catalog. It does not search the web.",
                "A miss means the name is not in Tanit, not 'not on the internet'.",
            })},
        };
        if (mcp_names.empty()) {
            payload["next_step"] =
                "No catalog match. info_lookup is not a web search.";
            if (mcp_only)
                return err("info_lookup: no MCP tools matched '" + query
                           + "'. Retry with kind=mcp and query=mcp. "
                             "This is a local catalog, not a web search.");
        } else if (kind == "app" || kind == "apps") {
            payload["next_step"] =
                "No installed app matched. Run an exact executable name that is on PATH "
                "(ffmpeg, magick). Do not invent a path. Do not create a command.";
        } else {
            payload["next_step"] =
                "No catalog tool matched. If the name is an executable, call run "
                "with command set to that name. Otherwise request_tools one "
                "mcp_tools name, then CALL it. Do not create_command. Do not say "
                "the program is missing.";
        }
        return ok(std::move(payload));
    }

    bool has_mcp_row = mcp_only;
    if (!has_mcp_row) {
        for (const auto& row : results) {
            if (row.is_object() && row.value("kind", std::string{}) == "mcp_tool") {
                has_mcp_row = true;
                break;
            }
        }
    }

    nlohmann::json answer_rules = nlohmann::json::array({
        "info_lookup is a local Tanit catalog. It does not search the web.",
        "Treat returned runtime schemas/options_summary as source of truth.",
        "Do not invent CLI flags. Positional arguments are not flags.",
        "If examples are shown, use only flags listed in options_summary.flags.",
        "If custom_command results appear before built-in commands, run that custom invocation. Do not create_command.",
        "When a custom_command row has file_flag, pass the selected path as that flag (--CURRENT_FILE <path>).",
        "If kind=app_command, run the verb with the app_command tool (not tanit-cli).",
    });
    nlohmann::json payload{
        {"tool", "info_lookup"},
        {"query", query},
        {"kind", kind},
        {"detail", detail},
        {"matched", true},
        {"results", std::move(results)},
    };
    if (has_mcp_row) {
        answer_rules.push_back(
            "To use an mcp_* tool: request_tools with that exact name, then CALL it. "
            "Do not create_command.");
        payload["next_step"] =
            "request_tools the exact mcp_* name, then CALL that tool. Do not create_command.";
    } else if (payload.contains("results") && payload["results"].is_array()) {
        for (const auto& row : payload["results"]) {
            if (!row.is_object() || row.value("kind", std::string{}) != "app")
                continue;
            const std::string step = row.value("next_step", std::string{});
            if (step.empty())
                continue;
            answer_rules.push_back(
                "A resolved executable is invoked with the run tool. Use the command "
                "field. Do not request an mcp_* tool for it.");
            payload["next_step"] = step;
            break;
        }
    }
    payload["answer_rules"] = std::move(answer_rules);
    return ok(std::move(payload));
}

} // namespace media::llm::info_tool

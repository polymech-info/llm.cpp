#include "agent_internal.hpp"
#include "json/json_utils.hpp"

#include "constants.hpp"
#include "app_exe_directory.hpp"

#include "string/string_sanitizers.hpp"
#include "core/settings_store.hpp"
#include "core/command_variables.hpp"
#include "policy/app_policy.hpp"
#if defined(FEATURE_SECURE_STORAGE) && FEATURE_SECURE_STORAGE
#include "storage/secure_storage.hpp"
#endif

#include "path_tool_catalog.hpp"
#include "path_tool_executor.hpp"  // set_agent_tool_blocklist / clear / set_agent_path_base
#include "llm_constants.hpp"
#include "fs/glob_filter.hpp"
#if defined(FEATURE_HOME_LLM_SKILLS) && FEATURE_HOME_LLM_SKILLS
#include "agent_skills.hpp"
#endif
#if defined(FEATURE_MCP_CLIENT) && FEATURE_MCP_CLIENT
#include "mcp_config.hpp"
#endif
#include "agent_memory_recall.hpp"
#include "logging/logger.h"
#include "ui_defaults.hpp"
#include "fs/path_sanitizer.hpp"
#if defined(FEATURE_COMMAND_LLAMA_INFERENCE) && FEATURE_COMMAND_LLAMA_INFERENCE
#include "lib/llm/llama/EmbedInference.hpp"
#endif
#if !defined(PM_PLUGIN_KBOT) || !PM_PLUGIN_KBOT
#include "llm_client.h"
#endif
#if defined(FEATURE_LLAMA) && FEATURE_LLAMA
#include "lib/llm/local_vector_store/VectorStore.hpp"
#endif

#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include "string/string_utils.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace media::llm::agent::detail {

namespace {
#ifndef FEATURE_SECURE_STORAGE
#define FEATURE_SECURE_STORAGE 0
#endif
} // namespace

// ── String utilities ──────────────────────────────────────────────────────────


bool user_asked_for_ribbon_command(std::string_view prompt)
{
    std::string q(prompt);
    q = media::strutil::lower_ascii(q);
    static constexpr const char* k_needles[] = {
        "ribbon",
        "context menu",
        "context-menu",
        "contextmenu",
        "create_command",
        "custom command",
        "add a command",
        "add a button",
        "add a shortcut",
        "create a button",
        "create a shortcut",
        "create a command",
        "create command",
        "make a command",
        "make command",
        "add command",
        "make a button",
        "new ribbon",
        "ribbon button",
    };
    for (const char* n : k_needles) {
        if (q.find(n) != std::string::npos)
            return true;
    }
    return false;
}

// ── Event emission ────────────────────────────────────────────────────────────

bool emit(const EventCallback& cb, const Event& ev) {
    if (!cb) return true;
    // Callbacks dump this event (often off the agent thread, e.g. process
    // stdout). Strict nlohmann dump abort()s on invalid UTF-8 there.
    Event safe = ev;
    safe.text = media::str_san::normalise_text_for_json_storage(safe.text);
    safe.tool_name = media::str_san::normalise_text_for_json_storage(safe.tool_name);
    sanitize_json_utf8_strings(safe.payload);
    return cb(safe);
}

// ── System-prompt file loading ────────────────────────────────────────────────

std::string trim_system_prompt_utf8(std::string s) {
    while (!s.empty() && static_cast<unsigned char>(s.front()) <= 32U) s.erase(0, 1);
    while (!s.empty() && static_cast<unsigned char>(s.back())  <= 32U) s.pop_back();
    return s;
}

std::filesystem::path system_prompt_config_parent() {
    namespace fs = std::filesystem;
    try {
        return media::settings::get_config_dir();
    } catch (...) {
        return {};
    }
}

// Shared loader used for system/planner/realtime/mode prompts so the secure
// document URI mapping stays in lockstep.
static std::string prompt_md_uri_for(const std::string& filename_utf8)
{
    if (filename_utf8 == "system-prompt.md")
        return "pm://config/prompts/system-prompt.md";
    if (filename_utf8 == "planner-prompt.md")
        return "pm://config/prompts/planner-prompt.md";
    if (filename_utf8 == "realtime-prompt.md")
        return "pm://config/prompts/realtime-prompt.md";
    if (filename_utf8 == "user-preferences.md")
        return "pm://config/prompts/user-preferences.md";
    if (filename_utf8 == "security-prompt.md")
        return "pm://config/prompts/security-prompt.md";
    constexpr std::string_view prefix = "mode-";
    constexpr std::string_view suffix = ".md";
    if (filename_utf8.size() > prefix.size() + suffix.size() &&
        filename_utf8.rfind(prefix, 0) == 0 &&
        filename_utf8.compare(filename_utf8.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return "pm://config/prompts/" + filename_utf8;
    }
    return {};
}

#if defined(__ANDROID__)
// system-prompt.md → system-prompt.android.md (same directory). Desktop names stay the fallback.
static std::string android_prompt_filename(const std::filesystem::path& filename)
{
    const std::string s = media::path::path_to_generic_utf8(filename);
    constexpr std::string_view ext = ".md";
    constexpr std::string_view android_ext = ".android.md";
    if (s.size() <= ext.size() ||
        s.compare(s.size() - ext.size(), ext.size(), ext) != 0)
        return {};
    if (s.size() >= android_ext.size() &&
        s.compare(s.size() - android_ext.size(), android_ext.size(), android_ext) == 0)
        return s;
    return s.substr(0, s.size() - ext.size()) + std::string(android_ext);
}
#endif

struct PromptMdCacheEntry {
    PromptFileLoad load;
    std::optional<std::filesystem::file_time_type> mtime;
};
std::mutex g_prompt_md_mu;
std::unordered_map<std::string, PromptMdCacheEntry> g_prompt_md;

static std::optional<std::filesystem::file_time_type>
prompt_md_write_time(const std::string& uri, const std::filesystem::path& filename)
{
    namespace fs = std::filesystem;
#if FEATURE_SECURE_STORAGE
    (void)filename;
    if (uri.empty())
        return std::nullopt;
    try {
        const auto paths = pm::storage::resolve_storage_paths(media::settings::get_config_dir());
        return pm::storage::document_write_time(paths, uri);
    } catch (...) {
        return std::nullopt;
    }
#else
    auto probe_one = [](const fs::path& file) -> std::optional<fs::file_time_type> {
        std::error_code ec;
        if (!fs::is_regular_file(file, ec))
            return std::nullopt;
        const auto ft = fs::last_write_time(file, ec);
        if (ec)
            return std::nullopt;
        return ft;
    };
    auto probe = [&](const fs::path& dir) -> std::optional<fs::file_time_type> {
        if (dir.empty())
            return std::nullopt;
#if defined(__ANDROID__)
        const std::string alias = android_prompt_filename(filename);
        if (!alias.empty()) {
            if (auto t = probe_one(dir / alias))
                return t;
        }
#endif
        return probe_one(dir / filename);
    };
    if (auto t = probe(media::app::exe_parent_directory()))
        return t;
    return probe(system_prompt_config_parent() / "prompts");
#endif
}

static PromptFileLoad load_prompt_md_uncached(const std::filesystem::path& filename,
                                              const std::string& uri)
{
    namespace fs = std::filesystem;
#if FEATURE_SECURE_STORAGE
    if (!uri.empty()) {
        try {
            pm::storage::DocumentLoadPolicy policy;
            policy.require_signature = true;
            policy.require_encryption = true;
#if defined(FEATURE_WINDOWS_GPO) && FEATURE_WINDOWS_GPO
            // Admin may require encryption/signing even when FEATURE_SECURE_STORAGE
            // would otherwise accept plaintext vendor seeds.
            if (pm::policy::policy_require_encryption()) policy.require_encryption = true;
            if (pm::policy::policy_require_signing())    policy.require_signature  = true;
#endif
            policy.allowed_trust = {
                pm::model::TrustLevel::Builtin,
                pm::model::TrustLevel::SignedVendor,
                pm::model::TrustLevel::UserLocal,
            };
            const auto paths = pm::storage::resolve_storage_paths(media::settings::get_config_dir());
            const auto loaded = pm::storage::load_or_import_seed_document(paths, uri, policy);
            if (loaded.ok) {
                PromptFileLoad out;
                out.content.assign(reinterpret_cast<const char*>(loaded.document.data.data()), loaded.document.data.size());
                out.content = trim_system_prompt_utf8(std::move(out.content));
                out.path = media::path::path_from_utf8(uri);
                return out;
            }
        } catch (...) {}
    }
#else
    auto read_md = [](const fs::path& file) -> PromptFileLoad {
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) return {};
        std::ifstream in(file, std::ios::binary);
        if (!in) return {};
        std::ostringstream oss;
        oss << in.rdbuf();
        PromptFileLoad out;
        out.content = trim_system_prompt_utf8(std::move(oss.str()));
        out.path = file;
        return out;
    };
    auto try_dir = [&](const fs::path& dir) -> PromptFileLoad {
        if (dir.empty()) return {};
#if defined(__ANDROID__)
        const std::string alias = android_prompt_filename(filename);
        if (!alias.empty()) {
            if (PromptFileLoad p = read_md(dir / alias); !p.content.empty())
                return p;
        }
#endif
        return read_md(dir / filename);
    };
    if (PromptFileLoad p = try_dir(media::app::exe_parent_directory()); !p.content.empty()) return p;
    if (PromptFileLoad p = try_dir(system_prompt_config_parent() / "prompts"); !p.content.empty()) return p;
#endif
    return {};
}

static PromptFileLoad load_prompt_md(const std::filesystem::path& filename) {
    const std::string filename_utf8 = media::path::path_to_generic_utf8(filename);
    const std::string uri = prompt_md_uri_for(filename_utf8);
    if (const auto wt = prompt_md_write_time(uri, filename)) {
        std::lock_guard<std::mutex> lock(g_prompt_md_mu);
        const auto it = g_prompt_md.find(filename_utf8);
        if (it != g_prompt_md.end() && it->second.mtime && *it->second.mtime == *wt)
            return it->second.load;
    }
    PromptFileLoad out = load_prompt_md_uncached(filename, uri);
    if (const auto wt = prompt_md_write_time(uri, filename)) {
        std::lock_guard<std::mutex> lock(g_prompt_md_mu);
        g_prompt_md[filename_utf8] = PromptMdCacheEntry{out, wt};
    }
    return out;
}

PromptFileLoad load_system_prompt_md() {
    return load_prompt_md(std::filesystem::path{"system-prompt.md"});
}

PromptFileLoad load_planner_prompt_md() {
    return load_prompt_md(std::filesystem::path{"planner-prompt.md"});
}

PromptFileLoad load_realtime_prompt_md() {
    return load_prompt_md(std::filesystem::path{"realtime-prompt.md"});
}

PromptFileLoad load_user_preferences_md() {
    return load_prompt_md(std::filesystem::path{"user-preferences.md"});
}

PromptFileLoad load_security_prompt_md() {
    return load_prompt_md(std::filesystem::path{"security-prompt.md"});
}

std::string load_system_prompt_md_if_present() {
    return load_system_prompt_md().content;
}

std::string load_planner_prompt_md_if_present() {
    return load_planner_prompt_md().content;
}

std::string load_realtime_prompt_md_if_present() {
    return load_realtime_prompt_md().content;
}

std::string load_security_prompt_md_if_present() {
    return load_security_prompt_md().content;
}

std::string_view turn_mode_name(TurnMode mode) noexcept {
    switch (mode) {
        case TurnMode::Lookup:      return "lookup";
        case TurnMode::Execute:     return "execute";
        case TurnMode::Research:    return "research";
        case TurnMode::Orchestrate: return "orchestrate";
        case TurnMode::Create:      return "create";
        case TurnMode::FollowUp:    return "followup";
        default:                    return "auto";
    }
}

PromptFileLoad load_mode_overlay_md(TurnMode mode) {
    if (mode == TurnMode::Auto) return {};
    std::string name{"mode-"};
    name += turn_mode_name(mode);
    name += ".md";
    return load_prompt_md(std::filesystem::path{name});
}

std::string load_mode_overlay_md_if_present(TurnMode mode) {
    return load_mode_overlay_md(mode).content;
}

// ── Prompt variable map + renderer ────────────────────────────────────────────

namespace {

const nlohmann::json* object_field(const nlohmann::json& j, const char* key)
{
    if (!j.is_object()) return nullptr;
    auto it = j.find(key);
    return (it != j.end() && it->is_object()) ? &*it : nullptr;
}

std::string string_field(const nlohmann::json& j, const char* key)
{
    if (!j.is_object()) return {};
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

std::string compact_or_pretty_json(const nlohmann::json& j, int indent = 2)
{
    try { return j.dump(indent); } catch (...) { return {}; }
}

std::string canonical_catalog_tool_name(std::string tool_id)
{
    if (tool_id.rfind("builtin.", 0) == 0)
        tool_id = tool_id.substr(std::string{"builtin."}.size());
    if (tool_id == "shell.exec" || tool_id == "shell.run")
        return "run";
    return tool_id;
}

const media::llm::path::ToolDef* find_path_tool_def(const std::string& name)
{
    static const std::vector<media::llm::path::ToolDef> catalog = media::llm::path::tool_catalog();
    for (const auto& def : catalog)
        if (def.name == name)
            return &def;
    return nullptr;
}

std::size_t skip_json_leading_ws(const std::string& s)
{
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
    return i;
}

bool looks_like_security_consent_json(const std::string& prompt)
{
    const std::size_t i = skip_json_leading_ws(prompt);
    if (i >= prompt.size() || prompt[i] != '{')
        return false;
    // Host consent payloads (security.consent.request) carry these keys.
    return prompt.find("\"tool\"") != std::string::npos
        || prompt.find("\"toolId\"") != std::string::npos
        || prompt.find("\"request\"") != std::string::npos;
}

void append_security_consent_prompt_vars(media::commands::VariableMap& vars,
                                         const std::string& prompt)
{
    vars["CONSENT_REQUEST_JSON"] = "";
    vars["TOOL_NAME"] = "";
    vars["TOOL_ID"] = "";
    vars["TOOL_PROVIDER"] = "";
    vars["TOOL_ACTION"] = "";
    vars["TOOL_TARGET_URI"] = "";
    vars["TOOL_PARAMETERS"] = "";
    vars["TOOL_SCHEMA"] = "";
    vars["TOOL_DESCRIPTION"] = "";
    vars["REQUEST_RISK"] = "";
    vars["REQUEST_REASON"] = "";
    vars["REQUEST_SCOPE"] = "";

    if (!looks_like_security_consent_json(prompt))
        return;

    vars["CONSENT_REQUEST_JSON"] = prompt;

    const nlohmann::json root = media::json::try_parse(prompt);
    if (root.is_discarded() || !root.is_object())
        return;

    const nlohmann::json* request = object_field(root, "request");
    if (!request) request = &root;

    const nlohmann::json* tool = object_field(*request, "tool");
    const std::string tool_id = tool ? string_field(*tool, "toolId") : string_field(*request, "tool");
    const std::string provider = tool ? string_field(*tool, "provider") : string_field(*request, "provider");
    const std::string catalog_name = canonical_catalog_tool_name(tool_id);

    vars["TOOL_ID"] = tool_id;
    vars["TOOL_NAME"] = catalog_name.empty() ? tool_id : catalog_name;
    vars["TOOL_PROVIDER"] = provider;
    vars["TOOL_ACTION"] = string_field(*request, "action");
    vars["REQUEST_RISK"] = string_field(root, "risk");
    vars["REQUEST_REASON"] = string_field(root, "whyNeeded");
    vars["REQUEST_SCOPE"] = string_field(root, "suggestedScope");
    if (vars["REQUEST_SCOPE"].empty())
        vars["REQUEST_SCOPE"] = string_field(*request, "suggestedScope");

    if (const nlohmann::json* target = object_field(*request, "target"))
        vars["TOOL_TARGET_URI"] = string_field(*target, "uri");

    std::string arguments = string_field(*request, "argumentsJson");
    if (!arguments.empty()) {
        const nlohmann::json parsed = media::json::try_parse(arguments);
        if (!parsed.is_discarded())
            arguments = compact_or_pretty_json(parsed);
    } else if (auto it = request->find("arguments"); it != request->end()) {
        arguments = compact_or_pretty_json(*it);
    } else if (auto it = root.find("arguments"); it != root.end()) {
        arguments = compact_or_pretty_json(*it);
    }
    vars["TOOL_PARAMETERS"] = arguments;

    if (const auto* def = find_path_tool_def(catalog_name)) {
        vars["TOOL_DESCRIPTION"] = def->description;
        vars["TOOL_SCHEMA"] = compact_or_pretty_json(def->input_schema);
    }
}

} // namespace

namespace {

std::string ext_lower_dot(const std::filesystem::path& p)
{
    std::string ext = media::path::path_to_utf8(p.extension());
    ext = media::strutil::lower_ascii(std::move(ext));
    return ext;
}

std::string format_byte_size(std::uintmax_t n)
{
    std::ostringstream o;
    if (n < 1024)
        o << n << " B";
    else if (n < 1024ull * 1024ull)
        o << (n / 1024) << " KiB";
    else
        o << (n / (1024ull * 1024ull)) << " MiB";
    return o.str();
}

int count_text_file_lines(const std::filesystem::path& p, std::uintmax_t sz)
{
    if (sz == 0)
        return 0;
    constexpr std::uintmax_t k_scan_cap = 2ull * 1024ull * 1024ull;
    if (sz > k_scan_cap)
        return -1;
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return -1;
    int lines = 0;
    char buf[4096];
    char last = 0;
    bool any = false;
    while (in) {
        in.read(buf, sizeof(buf));
        const auto n = in.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            any = true;
            last = buf[i];
            if (buf[i] == '\n')
                ++lines;
        }
    }
    if (!any)
        return 0;
    if (last != '\n')
        ++lines;
    return lines;
}

bool looks_like_text_path(const std::filesystem::path& p)
{
    const std::string ext = ext_lower_dot(p);
    return ext.empty() || !media::llm::glob_file_extension_blocked_like_file_read(ext);
}

} // namespace

std::string format_path_with_stats(std::string_view path)
{
    const std::string raw(path);
    if (raw.empty())
        return {};
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(raw);
    if (!fs::is_regular_file(p, ec) || ec)
        return raw;
    const auto sz = fs::file_size(p, ec);
    if (ec)
        return raw;

    int lines = -1;
    if (looks_like_text_path(p))
        lines = count_text_file_lines(p, sz);
    const bool large = (sz > pm::llm::k_file_read_large_bytes)
        || (lines > pm::llm::k_file_read_window_lines);

    std::ostringstream o;
    o << raw << " (" << format_byte_size(sz);
    if (lines >= 0)
        o << ", " << lines << " lines";
    if (large)
        o << ", large";
    o << ")";
    return o.str();
}

std::string format_paths_with_stats(const std::vector<std::string>& paths)
{
    std::ostringstream o;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (i)
            o << "\n";
        o << format_path_with_stats(paths[i]);
    }
    return o.str();
}

bool path_is_existing_text_file(std::string_view path)
{
    if (path.empty())
        return false;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path file_path{std::string{path}};
    if (!fs::is_regular_file(file_path, ec) || ec)
        return false;
    return looks_like_text_path(file_path);
}

bool path_is_large_text_file(std::string_view path)
{
    if (!path_is_existing_text_file(path))
        return false;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path file_path{std::string{path}};
    const auto sz = fs::file_size(file_path, ec);
    if (ec)
        return false;
    if (sz > pm::llm::k_file_read_large_bytes)
        return true;
    const int lines = count_text_file_lines(file_path, sz);
    return lines > pm::llm::k_file_read_window_lines;
}

static media::commands::VariableMap build_agent_prompt_vars(const Turn& turn,
                                                             const std::string& builtin_text)
{
    // Base: standard command_variables (CWD, YYYY, MM, DD, HH, SS,
    //       CURRENT_PATH, CURRENT_SELECTION, PATH_SEP, …).
    media::commands::VariableContext ctx;
    ctx.cwd             = turn.folder_hint;
    ctx.selection_paths = turn.selection;
    auto vars = media::commands::make_variable_map(ctx);

    // Agent-specific additions.
    vars["APP_NAME"]        = pm::brand::k_app_display_u8;
    vars["APP_ID"]          = pm::brand::k_app_id_u8;
    vars["DATETIME"]        = local_datetime_with_offset();
    vars["FOLDER"]          = turn.folder_hint;
    vars["SELECTION_COUNT"] = std::to_string(turn.selection.size());
    vars["OPEN_COUNT"]      = std::to_string(turn.open_files.size());
    vars["TASK_ID"]         = turn.task_id;
    vars["PLATFORM"]        =
#if defined(_WIN32)
        "Windows";
#elif defined(__APPLE__)
        "macOS";
#elif defined(__ANDROID__)
        "Android";
#else
        "Linux";
#endif
    vars["CONNECTION_CONTEXT"] = turn.connection_context.empty()
#if defined(__ANDROID__)
        ? "Connection mode: phone-local. Tools act on this Android device or its app data unless a tool explicitly names a remote service."
#else
        ? "Connection mode: local. Tools act on this computer unless a tool explicitly names a remote service."
#endif
        : turn.connection_context;

    // Guarantee selection/path keys are always present so templates never
    // leave a literal ${...} placeholder when nothing is selected / no folder.
    // Agent prompts get size/line annotations; CLI CURRENT_SELECTION stays a path list.
    vars["CURRENT_SELECTION"] = format_paths_with_stats(turn.selection);
    if (!vars.count("CURRENT_PATH"))
        vars["CURRENT_PATH"] = turn.folder_hint;
    vars["CURRENT_OPEN_FILES"] = format_paths_with_stats(turn.open_files);

    if (!turn.editor_path.empty() && turn.editor_line > 0) {
        std::ostringstream caret;
        caret << turn.editor_path << ':' << turn.editor_line << ':' << turn.editor_column;
        const bool ranged =
            turn.editor_sel_start_line > 0
            && (turn.editor_sel_start_line != turn.editor_sel_end_line
                || turn.editor_sel_start_col != turn.editor_sel_end_col);
        if (ranged) {
            caret << " (sel " << turn.editor_sel_start_line << ':' << turn.editor_sel_start_col
                  << '-' << turn.editor_sel_end_line << ':' << turn.editor_sel_end_col << ')';
        }
        vars["CURRENT_CURSOR"] = caret.str();
    } else {
        vars["CURRENT_CURSOR"] = "(none)";
    }
    if (!turn.editor_selected_text.empty()) {
        vars["CURRENT_INNER_SELECTION"] = media::str_san::utf8_safe_ellipsize(
            turn.editor_selected_text, 400);
        vars["INNER_SELECTION_CHARS"] = std::to_string(turn.editor_selected_text.size());
    } else {
        vars["CURRENT_INNER_SELECTION"] = "(none)";
        vars["INNER_SELECTION_CHARS"] = "0";
    }

    if (!builtin_text.empty())
        vars["builtin"] = builtin_text;

    append_security_consent_prompt_vars(vars, turn.user_prompt);

    return vars;
}

std::string render_prompt_file(const std::string& raw,
                               const Turn& turn,
                               const std::string& builtin_text)
{
    if (raw.empty()) return {};
    const auto vars = build_agent_prompt_vars(turn, builtin_text);
    return media::commands::resolve_variables(raw, vars);
}

// ── Date / time ───────────────────────────────────────────────────────────────

std::string local_datetime_with_offset() {
    using namespace std::chrono;
    const auto     now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);

    std::tm ltm{};
    std::tm utm{};
#if defined(_WIN32)
    localtime_s(&ltm, &t);
    gmtime_s(&utm, &t);
#else
    localtime_r(&t, &ltm);
    gmtime_r(&t, &utm);
#endif

    int local_m = ltm.tm_hour * 60 + ltm.tm_min;
    int utc_m   = utm.tm_hour * 60 + utm.tm_min;
    int day_d   = ltm.tm_yday - utm.tm_yday;
    if (day_d >  1) day_d = -1;
    if (day_d < -1) day_d =  1;
    const int off_min = local_m - utc_m + day_d * 1440;
    const char sign   = off_min >= 0 ? '+' : '-';
    const int aoff    = std::abs(off_min);

    std::ostringstream oss;
    oss << std::put_time(&ltm, "%Y-%m-%dT%H:%M:%S")
        << sign
        << std::setw(2) << std::setfill('0') << (aoff / 60) << ':'
        << std::setw(2) << std::setfill('0') << (aoff % 60);

#if defined(_WIN32)
    TIME_ZONE_INFORMATION tzi{};
    if (GetTimeZoneInformation(&tzi) != TIME_ZONE_ID_INVALID) {
        const WCHAR* wname = (ltm.tm_isdst > 0) ? tzi.DaylightName : tzi.StandardName;
        if (wname && wname[0]) {
            const int len = ::WideCharToMultiByte(CP_UTF8, 0, wname, -1,
                                                  nullptr, 0, nullptr, nullptr);
            if (len > 1) {
                std::string tz(static_cast<std::size_t>(len - 1), '\0');
                ::WideCharToMultiByte(CP_UTF8, 0, wname, -1,
                                      tz.data(), len, nullptr, nullptr);
                oss << " (" << tz << ')';
            }
        }
    }
#endif
    return oss.str();
}

// ── RAII scopes ───────────────────────────────────────────────────────────────

PathToolBlocklistScope::PathToolBlocklistScope(const std::vector<std::string>& disabled, int max_iter) {
    if (!disabled.empty() && max_iter > 0) {
        media::llm::path::set_agent_tool_blocklist(disabled);
        active = true;
    }
}
PathToolBlocklistScope::~PathToolBlocklistScope() {
    if (active) media::llm::path::clear_agent_tool_blocklist();
}

AgentPathBaseScope::AgentPathBaseScope(const Turn& t) {
    std::string base;
    if (!t.folder_hint.empty()) base = t.folder_hint;
    if (!base.empty()) {
        media::llm::path::set_agent_path_base(base);
        active = true;
    }
    if (!t.selection.empty()) {
        media::llm::path::set_agent_selection(t.selection);
        selection_active = true;
    }
    // Serve PathBaseGuard may already own the jail for the HTTP request — do not
    // nest clear on scope exit or tools after the turn would lose the jail.
    if (t.fs_jail && !media::llm::path::agent_fs_jail_enabled()) {
        media::llm::path::set_agent_fs_jail(true);
        jail_active = true;
    }
}
AgentPathBaseScope::~AgentPathBaseScope() {
    if (jail_active) media::llm::path::clear_agent_fs_jail();
    if (selection_active) media::llm::path::clear_agent_selection();
    if (active) media::llm::path::clear_agent_path_base();
}

// ── Runtime environment block ─────────────────────────────────────────────────

void append_runtime_environment_context(std::ostringstream& s) {
    s << "Runtime context (built in, always true for this session):\n"
         "  - Host OS: ";
#if defined(_WIN32)
    s << "Microsoft Windows (desktop). User paths often use drive letters and backslashes; "
         "the app normalises paths for tools.\n";
#elif defined(__APPLE__)
    s << "Apple macOS (desktop). Paths are POSIX-style.\n";
#elif defined(__ANDROID__)
    s << "Android. Paths are POSIX-style.\n";
    s << "  - Current working directory (cwd): **"
      << media::path::path_to_utf8(media::settings::get_config_dir())
      << "** — the app data folder. When the user asks for your cwd, working "
         "directory, or current folder, answer with this exact path.\n";
#elif defined(__linux__)
    s << "Linux (desktop or server). Paths are POSIX-style.\n";
#else
    s << "Other or unknown; paths follow the host platform.\n";
#endif
    s << "  - Application: **" << pm::brand::k_app_display_u8
      << "** (technical id `" << pm::brand::k_app_id_u8 << "`)";
#if defined(_WIN32)
    s << " \u2014 includes Windows Explorer / shell integration.\n";
#else
    s << ".\n";
#endif
    s << "  - Current local date/time: **" << local_datetime_with_offset() << "**"
         " \u2014 use this when the user says 'now', 'today', 'tonight', or a clock time without a timezone.\n"
         "  - Chat interface: the user reads your replies in an **in-app chat panel** with **rich Markdown** "
         "(headings, lists, emphasis, fenced code blocks, links when allowed by the renderer). "
         "It is not a plain-text terminal; use Markdown when it clarifies structure or code.\n\n";
}

// ── LLM usage parsing ─────────────────────────────────────────────────────────

std::int64_t json_usage_int64(const nlohmann::json& usage, const char* key) {
    if (!usage.contains(key) || !usage[key].is_number()) return 0;
    const auto& v = usage[key];
    if (v.is_number_integer()) return v.get<std::int64_t>();
    return static_cast<std::int64_t>(v.get<double>());
}

const nlohmann::json* provider_meta_usage_object(const nlohmann::json& meta) {
    if (!meta.is_object()) return nullptr;
    if (meta.contains("usage") && meta["usage"].is_object())
        return &meta["usage"];
    if (meta.contains("usage_object") && meta["usage_object"].is_object())
        return &meta["usage_object"];
    if (meta.contains("additional_usage_values") && meta["additional_usage_values"].is_object())
        return &meta["additional_usage_values"];
    return nullptr;
}

void merge_llm_usage_from_provider_meta(nlohmann::json& agg,
                                        const std::string& provider_meta_json,
                                        const std::string& router)
{
    if (provider_meta_json.empty()) return;

    if (!agg.is_object()) {
        agg                          = nlohmann::json::object();
        agg["llm_rounds"]            = nlohmann::json::array();
        agg["prompt_tokens"]         = 0;
        agg["completion_tokens"]     = 0;
        agg["total_tokens"]          = 0;
    }
    if (!agg.contains("llm_rounds") || !agg["llm_rounds"].is_array())
        agg["llm_rounds"] = nlohmann::json::array();

    nlohmann::json meta = media::json::try_parse(provider_meta_json);
    if (meta.is_discarded() || !meta.is_object()) return;
    sanitize_json_utf8_strings(meta);

    nlohmann::json round_entry = nlohmann::json::object();
    if (!router.empty())        round_entry["provider"] = router;
    if (meta.contains("model")) round_entry["model"]    = meta["model"];
    else if (meta.contains("model_map_information") && meta["model_map_information"].is_object()) {
        const auto& mi = meta["model_map_information"];
        if (mi.contains("model_map_key") && mi["model_map_key"].is_string())
            round_entry["model"] = mi["model_map_key"];
    }
    if (meta.contains("id"))    round_entry["id"]       = meta["id"];

    std::int64_t add_prompt     = 0;
    std::int64_t add_completion = 0;
    std::int64_t add_total      = 0;
    double       add_cost       = 0;
    bool         have_usage     = false;
    bool         have_cost      = false;

    nlohmann::json synthesized_usage;
    const nlohmann::json* usage = provider_meta_usage_object(meta);
    if (!usage) {
        // Local llama.cpp writes prompt/completion counts at the meta root
        // (and historically omitted a nested `usage` object). Treat those
        // fields as usage so the chat transcript can show token details.
        const bool have_top_level =
            meta.contains("prompt_tokens") || meta.contains("completion_tokens")
            || meta.contains("input_tokens") || meta.contains("output_tokens")
            || meta.contains("promptTokens") || meta.contains("completionTokens");
        if (have_top_level) {
            synthesized_usage = nlohmann::json::object();
            auto copy_num = [&](const char* from, const char* to) {
                if (meta.contains(from) && meta[from].is_number())
                    synthesized_usage[to] = meta[from];
            };
            copy_num("prompt_tokens", "prompt_tokens");
            copy_num("promptTokens", "prompt_tokens");
            copy_num("input_tokens", "prompt_tokens");
            copy_num("completion_tokens", "completion_tokens");
            copy_num("completionTokens", "completion_tokens");
            copy_num("output_tokens", "completion_tokens");
            copy_num("total_tokens", "total_tokens");
            copy_num("tokens_per_second", "tokens_per_second");
            copy_num("tokensPerSecond", "tokens_per_second");
            usage = &synthesized_usage;
        }
    }
    if (usage) {
        const nlohmann::json& u = *usage;
        round_entry["usage"]    = u;
        have_usage              = true;
        add_prompt     = json_usage_int64(u, "prompt_tokens")     + json_usage_int64(u, "input_tokens");
        add_completion = json_usage_int64(u, "completion_tokens") + json_usage_int64(u, "output_tokens");
        add_total      = json_usage_int64(u, "total_tokens");
        if (add_total <= 0 && (add_prompt > 0 || add_completion > 0))
            add_total = add_prompt + add_completion;
        if (u.contains("cost") && u["cost"].is_number()) {
            have_cost = true;
            add_cost  = u["cost"].get<double>();
        }
    }

    if (!round_entry.empty())
        agg["llm_rounds"].push_back(std::move(round_entry));

    if (!have_usage) return;

    agg["prompt_tokens"]     = json_usage_int64(agg, "prompt_tokens")     + add_prompt;
    agg["completion_tokens"] = json_usage_int64(agg, "completion_tokens") + add_completion;
    agg["total_tokens"]      = json_usage_int64(agg, "total_tokens")      + add_total;

    if (have_cost) {
        double prev = 0;
        if (agg.contains("cost") && agg["cost"].is_number())
            prev = agg["cost"].get<double>();
        agg["cost"] = prev + add_cost;
    }

    sanitize_json_utf8_strings(agg);
}

} // namespace media::llm::agent::detail

namespace media::llm::agent {

void warmup_host_io_async()
{
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            try {
                const auto t0 = std::chrono::steady_clock::now();
                (void)detail::load_system_prompt_md();
                (void)detail::load_planner_prompt_md();
                (void)detail::load_security_prompt_md();
                (void)detail::load_realtime_prompt_md();
#if defined(FEATURE_HOME_LLM_SKILLS) && FEATURE_HOME_LLM_SKILLS
                {
                    const auto policy = media::llm::skills::load_policy_from_settings();
                    const auto cwd = media::path::path_to_utf8(media::ui_defaults::fallback_work_folder());
                    (void)media::llm::skills::discover_skills(policy, cwd);
                }
#endif
#if defined(FEATURE_MCP_CLIENT) && FEATURE_MCP_CLIENT
                {
                    std::string note;
                    (void)media::llm::mcp::load_or_probe_mcp_tools(false, note, {}, true);
                }
#endif
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                logger::info(std::string("agent warmup: host io ") + std::to_string(ms) + "ms");
            } catch (const std::exception& e) {
                logger::warn(std::string("agent warmup: host io ") + e.what());
            } catch (...) {
                logger::warn("agent warmup: host io failed");
            }
        }).detach();
#if !defined(PM_PLUGIN_KBOT) || !PM_PLUGIN_KBOT
        polymech::kbot::warmup_llm_http_async();
#endif
#if defined(FEATURE_COMMAND_LLAMA_INFERENCE) && FEATURE_COMMAND_LLAMA_INFERENCE
        std::thread([] {
            try {
                const auto t0 = std::chrono::steady_clock::now();
                const bool ready = media::llama::embed_inference_init();
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                logger::info(std::string("agent warmup: embed ")
                             + (ready ? "ready " : "skipped ")
                             + std::to_string(ms) + "ms");
#if defined(FEATURE_LLAMA) && FEATURE_LLAMA
                if (ready) {
                    const auto t1 = std::chrono::steady_clock::now();
                    std::string err;
                    (void)media::llm::vector_store_with(
                        memory_recall_index_dir(), {},
                        [](media::llm::VectorStore&) {}, &err);
                    const auto vs_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t1).count();
                    logger::info(std::string("agent warmup: recall index ")
                                 + (err.empty() ? "open " : "skip ")
                                 + std::to_string(vs_ms) + "ms");
                }
#endif
            } catch (const std::exception& e) {
                logger::warn(std::string("agent warmup: embed ") + e.what());
            } catch (...) {
                logger::warn("agent warmup: embed failed");
            }
        }).detach();
#endif
    });
}

} // namespace media::llm::agent

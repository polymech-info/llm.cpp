#include "CreateCommandTool.hpp"
#include "json/json_utils.hpp"

#include "cli/pm_image_command_info.hpp"
#if defined(_WIN32)
#include "core/host_backends.hpp"
#endif
#include "core/command_variables.hpp"
#include "fs/path_sanitizer.hpp"
#include "core/settings_runtime.hpp"
#include "llm/tools/info/AppFinder.hpp"
#include "llm/tools/run/RunValidators.hpp"
#include "logging/pm_log.hpp"
#include "pm_branding_config.hpp"
#include "string/string_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

// Pipeline (creation side):
//   LLM proposal → strict schema parse → canonical intent
//   → internal: dry-parse against the live CLI grammar
//   → external: exact executable resolution (or return candidates)
//   → shell:    interpreter allow-list + Deny validators on the line
//   → exposure gating → serialised load/validate/insert/atomic save.
//
// Execution-side validation (after `${…}` expansion) is done by
// media::cmd_run::validate_expanded_command for entries stamped
// `userData.createdBy = "create_command"`.
//
// Rule: normalisation may remove harmless syntax differences; it must never
// invent intent. Anything ambiguous is an error, not a guess.

namespace media::llm::create_command {
namespace {

#if defined(_WIN32)
namespace catalog = pm::host::cli;
#else
namespace catalog = pm::cli;
#endif

constexpr const char k_ch[] = "create_command";
constexpr const char k_default_group[] = "AI Commands";

// ─── Result helpers ────────────────────────────────────────────────────────

media::llm::path::ExecuteResult tool_ok(nlohmann::json payload)
{
    media::llm::path::ExecuteResult r;
    r.ok      = true;
    payload["ok"] = true;
    r.envelope = std::move(payload);
    return r;
}

media::llm::path::ExecuteResult tool_err(const std::string& msg, nlohmann::json extra = {})
{
    media::llm::path::ExecuteResult r;
    r.ok    = false;
    r.error = "create_command: " + msg;
    r.envelope = nlohmann::json{{"ok", false}, {"error", r.error}};
    if (extra.is_object())
        for (auto& [k, v] : extra.items())
            r.envelope[k] = v;
    return r;
}

// ─── Strict argument access ───────────────────────────────────────────────
//
// A present-but-wrong-typed field is an error, never a silent default.

struct ArgError {
    std::string message;
};

std::optional<ArgError> want_string(const nlohmann::json& args, const char* key, std::string& out)
{
    if (!args.contains(key) || args[key].is_null())
        return std::nullopt;
    if (!args[key].is_string())
        return ArgError{std::string(key) + " must be a string"};
    out = media::strutil::trim(args[key].get<std::string>());
    return std::nullopt;
}

std::optional<ArgError> want_bool(const nlohmann::json& args, const char* key, bool& out)
{
    if (!args.contains(key) || args[key].is_null())
        return std::nullopt;
    if (!args[key].is_boolean())
        return ArgError{std::string(key) + " must be true or false"};
    out = args[key].get<bool>();
    return std::nullopt;
}

std::optional<ArgError> want_string_array(const nlohmann::json& args, const char* key,
                                          std::vector<std::string>& out, size_t max_items)
{
    if (!args.contains(key) || args[key].is_null())
        return std::nullopt;
    const auto& v = args[key];
    if (!v.is_array())
        return ArgError{std::string(key) + " must be a JSON array of strings (not a string)"};
    if (v.size() > max_items)
        return ArgError{std::string(key) + ": at most " + std::to_string(max_items) + " entries"};
    out.clear();
    for (size_t i = 0; i < v.size(); ++i) {
        if (!v[i].is_string())
            return ArgError{std::string(key) + "[" + std::to_string(i) + "] must be a string"};
        out.push_back(v[i].get<std::string>());
    }
    return std::nullopt;
}

// ─── ID generation ────────────────────────────────────────────────────────
//
// 128 random bits from the OS CSPRNG; collision is not a practical concern and
// the id no longer leaks creation time or label.

std::string generate_id()
{
    std::random_device rd;
    char buf[40];
    const uint32_t a = rd(), b = rd(), c = rd(), d = rd();
    std::snprintf(buf, sizeof(buf), "%08x%08x%08x%08x", a, b, c, d);
    return std::string("custom.command-") + buf;
}

bool id_exists(const nlohmann::json& node, const std::string& id)
{
    if (node.is_object()) {
        if (node.value("id", std::string{}) == id)
            return true;
        for (const auto& [k, v] : node.items())
            if (id_exists(v, id))
                return true;
    } else if (node.is_array()) {
        for (const auto& v : node)
            if (id_exists(v, id))
                return true;
    }
    return false;
}

// ─── Text validators ──────────────────────────────────────────────────────

// Decode one UTF-8 code point; returns 0 and advances by one on malformed input.
uint32_t next_code_point(const std::string& s, size_t& i)
{
    const auto b0 = static_cast<unsigned char>(s[i]);
    size_t len = 1;
    uint32_t cp = b0;
    if (b0 >= 0xF0)      { len = 4; cp = b0 & 0x07; }
    else if (b0 >= 0xE0) { len = 3; cp = b0 & 0x0F; }
    else if (b0 >= 0xC0) { len = 2; cp = b0 & 0x1F; }
    if (i + len > s.size()) { ++i; return 0; }
    for (size_t k = 1; k < len; ++k) {
        const auto bk = static_cast<unsigned char>(s[i + k]);
        if ((bk & 0xC0) != 0x80) { ++i; return 0; }
        cp = (cp << 6) | (bk & 0x3F);
    }
    i += len;
    return cp;
}

// Exactly the direction-control / invisible-format code points that can make a
// label read differently from what executes. Curly quotes, dashes and the
// ellipsis (U+2018…U+2026) are legitimate and pass.
bool is_spoofing_code_point(uint32_t cp)
{
    switch (cp) {
    case 0x061C:                   // ARABIC LETTER MARK
    case 0x200B: case 0x200C: case 0x200D: // zero-width space / non-joiner / joiner
    case 0x200E: case 0x200F:     // LRM RLM
    case 0x202A: case 0x202B: case 0x202C: case 0x202D: case 0x202E: // LRE RLE PDF LRO RLO
    case 0x2060:                   // WORD JOINER
    case 0x2066: case 0x2067: case 0x2068: case 0x2069: // LRI RLI FSI PDI
    case 0xFEFF:                   // ZWNBSP / BOM
        return true;
    default:
        return false;
    }
}

bool contains_spoofing_chars(const std::string& s)
{
    for (size_t i = 0; i < s.size();) {
        if (is_spoofing_code_point(next_code_point(s, i)))
            return true;
    }
    return false;
}

std::string validate_display_text(const std::string& s, const char* field, size_t max_len,
                                  bool allow_newline = false)
{
    if (s.size() > max_len)
        return std::string(field) + " must be " + std::to_string(max_len) + " characters or fewer";
    for (unsigned char c : s) {
        if (c < 0x20 && c != '\t' && !(allow_newline && c == '\n'))
            return std::string(field) + " contains a control character";
    }
    if (contains_spoofing_chars(s))
        return std::string(field) + " contains an invisible or bidirectional control character";
    return {};
}

std::string validate_tint(const std::string& s)
{
    if (s.empty()) return {};
    if (s.size() != 7 || s[0] != '#')
        return "tint must be in #RRGGBB format";
    for (int i = 1; i < 7; ++i)
        if (!std::isxdigit(static_cast<unsigned char>(s[i])))
            return "tint must be in #RRGGBB format";
    return {};
}

// ─── Variable grammar ─────────────────────────────────────────────────────
//
// Fails closed: with no palette there is no way to tell a real variable from a
// typo, so any `${` is rejected. Namespaces are validated by prefix against the
// resolver's grammar (command_variables.cpp); `${ENV:…}` is not available to
// agent-created commands; `|transform` is not part of the grammar.

const std::unordered_set<std::string>& variable_palette_names()
{
    static const std::unordered_set<std::string> names = [] {
        std::unordered_set<std::string> out;
        const auto rows = media::commands::command_variable_palette_json();
        if (!rows.is_array())
            return out;
        for (const auto& row : rows) {
            if (!row.is_object())
                continue;
            const std::string name = row.value("name", std::string{});
            if (!name.empty())
                out.insert(name);
        }
        return out;
    }();
    return names;
}

bool is_identifier(const std::string& s)
{
    if (s.empty())
        return false;
    for (unsigned char c : s)
        if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.'))
            return false;
    return true;
}

bool is_indexed_slot(const std::string& token, const char* prefix)
{
    const std::size_t prefix_len = std::char_traits<char>::length(prefix);
    if (token.size() <= prefix_len || token.compare(0, prefix_len, prefix) != 0)
        return false;
    for (std::size_t i = prefix_len; i < token.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(token[i])))
            return false;
    }
    return true;
}

std::string validate_variable_token(const std::string& token, const std::unordered_set<std::string>& names)
{
    if (token.empty())
        return "empty ${} variable";
    if (token.find('|') != std::string::npos)
        return "${" + token + "}: |transforms are not supported; use ${PATH_DIR:…}, ${PATH_NAME:…}, "
               "${PATH_EXT:…}, ${PATH_BASE:…} or ${PATH_ABS:…}";
    if (names.count(token)
        || is_indexed_slot(token, "CURRENT_SELECTION_")
        || is_indexed_slot(token, "BROWSER_TAB_")
        || is_indexed_slot(token, "BROWSER_TAB_TITLE_"))
        return {};

    const auto colon = token.find(':');
    if (colon == std::string::npos)
        return "unknown variable ${" + token + "}. Use a name from the command variable palette "
               "(CURRENT_FILE, CURRENT_PATH, CURRENT_SELECTION, KNOWNFOLDER:Desktop, …).";

    const std::string ns   = token.substr(0, colon);
    const std::string name = token.substr(colon + 1);
    if (!is_identifier(name))
        return "${" + token + "}: \"" + name + "\" is not a valid variable name";

    if (ns == "ENV")
        return "${" + token + "}: environment variables are not available to agent-created commands. "
               "Use ${KNOWNFOLDER:…} or an explicit path.";
    if (ns == "KNOWNFOLDER")
        return "unknown known folder ${" + token + "}. Use one of the KNOWNFOLDER:* entries from the palette.";
    if (ns == "PATH_DIR" || ns == "PATH_NAME" || ns == "PATH_EXT" || ns == "PATH_BASE" || ns == "PATH_ABS") {
        if (!names.count(name) && !is_indexed_slot(name, "CURRENT_SELECTION_"))
            return "${" + token + "}: \"" + name + "\" is not a palette variable";
        return {};
    }
    if (ns == "USER") {
        if (!media::commands::user_variable_value(name).has_value())
            return "${" + token + "}: no user variable named \"" + name + "\" is defined";
        return {};
    }
    return "unknown variable namespace \"" + ns + ":\" in ${" + token + "}";
}

std::string validate_variable_text(const std::string& text)
{
    if (!media::strutil::has_variable_tokens(text))
        return {};
    const auto& names = variable_palette_names();
    if (names.empty())
        return "command variable registry unavailable; cannot validate ${…} references";
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '$' || i + 1 >= text.size() || text[i + 1] != '{')
            continue;
        const auto end = text.find('}', i + 2);
        if (end == std::string::npos)
            return "unclosed ${ in \"" + text + "\"";
        const std::string token = media::strutil::trim(text.substr(i + 2, end - (i + 2)));
        if (auto e = validate_variable_token(token, names); !e.empty())
            return e;
        i = end;
    }
    return {};
}

// ─── Canonical intent ─────────────────────────────────────────────────────

enum class Kind { Internal, External, Shell };

struct Intent {
    Kind kind = Kind::External;
    // internal
    std::vector<std::string> cli_argv;   // subcommand path + options, no exe token
    std::string cli_path;                // "audio record" (from the dry parse)
    // external
    std::string exe;                     // resolved executable (path or exact basename)
    std::vector<std::string> args;
    // shell
    std::string interpreter;             // cmd|powershell|pwsh|bash|sh|zsh|fish
    std::string shell_line;
};


std::string basename_no_ext(std::string t)
{
    const auto slash = t.find_last_of("/\\");
    if (slash != std::string::npos)
        t = t.substr(slash + 1);
    t = media::strutil::lower_ascii(std::move(t));
    for (const char* ext : {".exe", ".cmd", ".bat", ".com"}) {
        const size_t el = std::char_traits<char>::length(ext);
        if (t.size() > el && t.compare(t.size() - el, el, ext) == 0) {
            t.resize(t.size() - el);
            break;
        }
    }
    return t;
}

bool is_cli_exe_token(const std::string& tok)
{
    const std::string t0 = basename_no_ext(tok);
    const std::string branded = basename_no_ext(pm::brand::k_exe_basename_cli_u8);
    return t0 == "pm-image-cli" || t0 == "pm-image" || t0 == "pm_image_cli" || t0 == "pm_image"
        || t0 == media::strutil::lower_ascii(pm::brand::k_app_id_u8)
        || t0 == media::strutil::lower_ascii(std::string(pm::brand::k_app_id_u8) + "-cli")
        || t0 == branded;
}

bool is_cli_root_slug(const std::string& low)
{
    for (const auto& c : catalog::registered_cli_commands())
        if (c.id && low == c.id)
            return true;
    return false;
}

// A single program launch, even one that computes an output path, is argv.
// Shell is for a pipe, a redirect, or a loop.
bool shell_line_needs_shell(const std::string& line)
{
    bool in_single = false;
    bool in_double = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\'' && !in_double)
            in_single = !in_single;
        else if (c == '"' && !in_single)
            in_double = !in_double;
        if (in_single || in_double)
            continue;
        if (c == '|' || c == '>' || c == '<')
            return true;
    }
    const std::string low = media::strutil::lower_ascii(line);
    for (const char* kw : {"foreach", "for(", "for ", "while ", "while(", "switch "}) {
        if (low.find(kw) != std::string::npos)
            return true;
    }
    return false;
}

const std::unordered_set<std::string>& shell_interpreters()
{
    static const std::unordered_set<std::string> s{
        "cmd", "powershell", "pwsh", "bash", "sh", "zsh", "fish"};
    return s;
}

const std::unordered_set<std::string>& denied_executables()
{
    // Script hosts and LOLBins that are never needed for a stored button.
    static const std::unordered_set<std::string> s{
        "wscript", "cscript", "mshta", "rundll32", "regsvr32", "certutil", "bitsadmin",
        "msiexec", "reg", "schtasks", "at", "wmic", "installutil", "msbuild"};
    return s;
}

const std::unordered_set<std::string>& agent_tool_names()
{
    static const std::unordered_set<std::string> s{
        "run", "write_file", "file_str_replace", "file_delete", "file_read", "file_glob",
        "file_search", "image_resize", "image_crop", "image_transform", "image_create",
        "image_understand", "image_from_camera", "create_video", "speak", "play_file", "suspend_voice", "audio_transcribe",
        "ask_user", "memory_read", "memory_write", "memory_append_event", "schedule_at",
        "schedule_in", "schedule_every", "schedule_cancel", "schedule_list", "info_lookup",
        "create_command", "list_images"};
    return s;
}

bool has_path_separator(const std::string& s)
{
    return s.find('\\') != std::string::npos || s.find('/') != std::string::npos;
}

// Exact-or-nothing app resolution. Returns the executable path, or empty and
// fills `candidates` so the caller can ask the user.
std::string resolve_external_exe(const std::string& command, nlohmann::json& candidates, std::string& err)
{
    namespace fs = std::filesystem;
    if (has_path_separator(command) || command.find(':') != std::string::npos) {
        // Explicit path: must be an absolute local path to an existing file.
        if (!media::path::looks_like_local_path(command)) {
            err = "\"" + command + "\" is not a local executable path";
            return {};
        }
        const std::string norm = media::path::normalize_local_path(command, true);
        fs::path p = media::path::path_from_utf8(norm);
        if (!p.is_absolute()) {
            err = "executable path must be absolute: \"" + command + "\"";
            return {};
        }
        std::error_code ec;
        if (!fs::is_regular_file(p, ec) || ec) {
            err = "executable not found: \"" + norm + "\"";
            return {};
        }
        // A PATH program is stored as its bare name so the button stays portable.
        const std::string bare = basename_no_ext(norm);
        if (const char* path_env = std::getenv("PATH"); path_env && *path_env && !bare.empty()) {
            std::string_view pv(path_env);
#if defined(_WIN32)
            const char sep = ';';
            const char* exts[] = {".exe", ".cmd", ".bat", ".com"};
#else
            const char sep = ':';
            const char* exts[] = {""};
#endif
            size_t start = 0;
            while (start <= pv.size()) {
                const auto end = pv.find(sep, start);
                const std::string_view dir = pv.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
                if (!dir.empty()) {
                    for (const char* ext : exts) {
                        std::error_code pec;
                        const fs::path hit = media::path::path_from_utf8(std::string(dir)) / (bare + ext);
                        if (fs::is_regular_file(hit, pec) && !pec)
                            return bare + ext;
                    }
                }
                if (end == std::string_view::npos)
                    break;
                start = end + 1;
            }
        }
        return norm;
    }

    const std::string want = basename_no_ext(command);
    if (!is_identifier(want)) {
        err = "\"" + command + "\" is not a valid executable name";
        return {};
    }
    const auto hits = media::llm::info::find_installed_apps(command, 8, true);

    // 1) exact executable basename match
    std::vector<const media::llm::info::AppMatch*> exact_exe;
    std::vector<const media::llm::info::AppMatch*> exact_name;
    for (const auto& h : hits) {
        if (h.exe.empty())
            continue;
        if (basename_no_ext(h.exe) == want)
            exact_exe.push_back(&h);
        else if (media::strutil::lower_ascii(h.name) == media::strutil::lower_ascii(command))
            exact_name.push_back(&h);
    }
    // Same executable listed by several sources (App Paths + Start Menu) is one
    // identity; two different files with the same basename are ambiguous.
    auto unique_exe = [](const std::vector<const media::llm::info::AppMatch*>& v) {
        std::unordered_set<std::string> keys;
        for (const auto* m : v)
            keys.insert(media::strutil::lower_ascii(media::path::normalize_local_path(m->exe, true)));
        return keys.size() == 1;
    };
    if (!exact_exe.empty() && unique_exe(exact_exe))
        return exact_exe.front()->exe;
    if (exact_exe.empty() && !exact_name.empty() && unique_exe(exact_name))
        return exact_name.front()->exe;

    // 2) on PATH by exact basename → store the bare name; the runner augments PATH.
    if (const char* path_env = std::getenv("PATH"); path_env && *path_env) {
        std::string_view pv(path_env);
#if defined(_WIN32)
        const char sep = ';';
        const char* exts[] = {".exe", ".cmd", ".bat", ".com"};
#else
        const char sep = ':';
        const char* exts[] = {""};
#endif
        size_t start = 0;
        while (start <= pv.size()) {
            const auto end = pv.find(sep, start);
            const std::string_view dir = pv.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
            if (!dir.empty()) {
                for (const char* ext : exts) {
                    std::error_code ec;
                    const fs::path p = media::path::path_from_utf8(std::string(dir)) / (want + ext);
                    if (fs::is_regular_file(p, ec) && !ec)
                        return want + ext;
                }
            }
            if (end == std::string_view::npos)
                break;
            start = end + 1;
        }
    }

    candidates = nlohmann::json::array();
    for (const auto& h : hits) {
        if (h.exe.empty())
            continue;
        candidates.push_back({{"name", h.name}, {"exe", h.exe}, {"source", h.source}});
        if (candidates.size() >= 6)
            break;
    }
    err = candidates.empty()
        ? "could not find an installed program named \"" + command + "\". Pass the full executable path."
        : "\"" + command + "\" is ambiguous. Pick one candidate and pass its exe path as command.";
    return {};
}

// ─── Persistence helpers ──────────────────────────────────────────────────

int find_group_index(const nlohmann::json& groups, const std::string& label_needle)
{
    if (!groups.is_array() || label_needle.empty()) return -1;
    const std::string needle = media::strutil::lower_ascii(label_needle);
    for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
        if (!groups[i].is_object()) continue;
        if (media::strutil::lower_ascii(groups[i].value("label", std::string{})) == needle) return i;
    }
    return -1;
}

std::string normalize_phrase(const std::string& s)
{
    std::string out;
    bool space = false;
    for (unsigned char c : media::strutil::lower_ascii(s)) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(c));
            space = false;
        } else if (!out.empty() && !space) {
            out.push_back(' ');
            space = true;
        }
    }
    return media::strutil::trim(out);
}

struct ExistingIndex {
    std::unordered_set<std::string> labels;          // lower
    std::unordered_set<std::string> voice_phrases;   // normalized
    std::vector<std::string> group_labels;
};

void index_items(const nlohmann::json& node, ExistingIndex& idx)
{
    if (node.is_object()) {
        if (node.contains("label") && node["label"].is_string() && node.contains("id"))
            idx.labels.insert(media::strutil::lower_ascii(node["label"].get<std::string>()));
        if (node.value("asVoiceCommand", false) && node.contains("voiceCommand") && node["voiceCommand"].is_string()) {
            const auto n = normalize_phrase(node["voiceCommand"].get<std::string>());
            if (!n.empty())
                idx.voice_phrases.insert(n);
        }
        for (const auto& [k, v] : node.items())
            index_items(v, idx);
    } else if (node.is_array()) {
        for (const auto& v : node)
            index_items(v, idx);
    }
}

// Document shape check. Empty → fresh document. Parseable-but-wrong → corruption,
// never "repaired".
std::string validate_document_shape(const nlohmann::json& doc)
{
    if (!doc.is_object())
        return "commands document root is not an object";
    if (doc.contains("ribbon")) {
        if (!doc["ribbon"].is_object())
            return "commands document: \"ribbon\" is not an object";
        if (doc["ribbon"].contains("groups") && !doc["ribbon"]["groups"].is_array())
            return "commands document: \"ribbon.groups\" is not an array";
        if (doc["ribbon"].contains("groups"))
            for (const auto& g : doc["ribbon"]["groups"]) {
                if (!g.is_object())
                    return "commands document: a ribbon group is not an object";
                if (g.contains("items") && !g["items"].is_array())
                    return "commands document: a ribbon group's \"items\" is not an array";
            }
    }
    return {};
}

} // namespace

// ─── Public execute ────────────────────────────────────────────────────────

media::llm::path::ExecuteResult execute(const nlohmann::json& args)
{
    if (!args.is_object())
        return tool_err("arguments must be an object");

    // ── 1. Strict parse ─────────────────────────────────────────────────────
    std::string label, command, description, surface = "ribbon", group_hint, icon = "run", tint;
    std::string shell, shell_line, selection_mode, voice_command, cwd;
    bool cwd_from_selection = false, create_group = false;
    bool as_llm_tool = false, as_voice_command = false, show_in_launcher = false;
    bool show_in_assistant_bar = false, show_parameter_dialog = false;
    std::vector<std::string> cmd_args, explorer_types;

    for (auto e : {
             want_string(args, "label", label), want_string(args, "command", command),
             want_string(args, "description", description), want_string(args, "surface", surface),
             want_string(args, "group", group_hint), want_string(args, "icon", icon),
             want_string(args, "tint", tint), want_string(args, "shell", shell),
             want_string(args, "shell_line", shell_line), want_string(args, "selection_mode", selection_mode),
             want_string(args, "voice_command", voice_command), want_string(args, "cwd", cwd),
             want_bool(args, "cwd_from_selection", cwd_from_selection), want_bool(args, "create_group", create_group),
             want_bool(args, "as_llm_tool", as_llm_tool), want_bool(args, "as_voice_command", as_voice_command),
             want_bool(args, "show_in_launcher", show_in_launcher),
             want_bool(args, "show_in_assistant_bar", show_in_assistant_bar),
             want_bool(args, "show_parameter_dialog", show_parameter_dialog),
             want_string_array(args, "args", cmd_args, 40),
             want_string_array(args, "explorer_file_types", explorer_types, 5)}) {
        if (e) return tool_err(e->message);
    }

    surface        = media::strutil::lower_ascii(surface);
    selection_mode = media::strutil::lower_ascii(selection_mode);
    shell          = basename_no_ext(shell);

    // ── 2. Field validation ─────────────────────────────────────────────────
    if (label.empty()) return tool_err("label is required");
    if (auto e = validate_display_text(label, "label", 80); !e.empty()) return tool_err(e);
    if (auto e = validate_display_text(description, "description", 300, true); !e.empty()) return tool_err(e);
    if (auto e = validate_display_text(group_hint, "group", 60); !e.empty()) return tool_err(e);
    if (auto e = validate_display_text(voice_command, "voice_command", 80); !e.empty()) return tool_err(e);
    if (auto e = validate_tint(tint); !e.empty()) return tool_err(e);
    if (surface != "ribbon" && surface != "context_menu" && surface != "both")
        return tool_err("surface must be \"ribbon\", \"context_menu\", or \"both\"");
    if (!selection_mode.empty() && selection_mode != "aggregate" && selection_mode != "peritem")
        return tool_err("selection_mode must be \"aggregate\" or \"perItem\"");
    for (auto& t : explorer_types) {
        t = media::strutil::lower_ascii(t);
        if (t != "image" && t != "video" && t != "audio" && t != "text" && t != "folder")
            return tool_err("explorer_file_types: \"" + t + "\" is not image, video, audio, text, or folder");
    }
    if (!cwd.empty() && cwd_from_selection)
        return tool_err("pass either cwd or cwd_from_selection, not both");
    if (cwd_from_selection)
        cwd = "${CURRENT_PATH}";
    if (!cwd.empty()) {
        if (auto e = validate_variable_text(cwd); !e.empty()) return tool_err(e);
        if (!media::strutil::has_variable_tokens(cwd)) {
            if (!media::path::looks_like_local_path(cwd))
                return tool_err("cwd must be an absolute local directory or a ${…} variable");
            cwd = media::path::normalize_local_path(cwd, true);
            std::error_code ec;
            if (!std::filesystem::is_directory(media::path::path_from_utf8(cwd), ec) || ec)
                return tool_err("cwd directory does not exist: \"" + cwd + "\"");
        }
    }

    // {command,args} and {shell,shell_line} are mutually exclusive inputs.
    const bool has_cmd   = !command.empty() || !cmd_args.empty();
    const bool has_shell = !shell.empty() || !shell_line.empty();
    if (has_cmd && has_shell)
        return tool_err("pass either command (+args) or shell (+shell_line), not both");
    if (!has_cmd && !has_shell)
        return tool_err("command is required (or shell + shell_line for a shell one-liner)");
    if (has_shell && (shell.empty() || shell_line.empty()))
        return tool_err("shell one-liners need both shell (cmd|powershell|pwsh|bash|sh|zsh|fish) and shell_line");
    if (has_cmd && command.empty())
        return tool_err("command is required when args are given");

    for (const auto& a : cmd_args) {
        if (a.size() > 512) return tool_err("args: each argument must be 512 characters or fewer");
        if (a.find('\0') != std::string::npos) return tool_err("args: argument contains a null byte");
    }
    if (command.size() > 512) return tool_err("command must be 512 characters or fewer");
    if (shell_line.size() > 2048) return tool_err("shell_line must be 2048 characters or fewer");
    if (command.find('\0') != std::string::npos || shell_line.find('\0') != std::string::npos)
        return tool_err("command contains a null byte");

    // ── 3. Canonical intent ─────────────────────────────────────────────────
    Intent intent;
    nlohmann::json candidates;

    if (has_shell) {
        if (!shell_interpreters().count(shell))
            return tool_err("shell must be one of cmd, powershell, pwsh, bash, sh, zsh, fish "
                            "(\"" + shell + "\" is not a shell; pass interpreters like python as command + args)");
        if (auto e = validate_variable_text(shell_line); !e.empty()) return tool_err(e);
        if (!shell_line_needs_shell(shell_line))
            return tool_err(
                "shell is only for a pipeline, redirection, or a loop. "
                "Launch one program with command and args. "
                "A sibling output is ${PATH_DIR:CURRENT_FILE}${PATH_SEP}${PATH_NAME:CURRENT_FILE}.ext "
                "(also ${SRC_DIR}, ${SRC_NAME}, ${PATH_EXT:CURRENT_FILE}). "
                "Pass a PATH program by name (ffmpeg), not its install path.");
        intent.kind        = Kind::Shell;
        intent.interpreter = shell;
        intent.shell_line  = shell_line;
        const auto vr = media::llm::run::validate_command(shell_line, shell);
        if (vr.tier == media::llm::run::ValidationTier::Deny)
            return tool_err("blocked: " + vr.reason);
    } else {
        // Tokenise `command` only when it is not a path: "tanit-cli audio record" is
        // a convenience spelling; "C:\Program Files\x.exe" must stay one token.
        std::vector<std::string> tokens;
        if (has_path_separator(command) || command.find(':') != std::string::npos)
            tokens.push_back(command);
        else
            tokens = media::strutil::tokenize(command);
        if (tokens.empty())
            return tool_err("command is required");

        const std::string head = media::strutil::lower_ascii(tokens.front());
        if (agent_tool_names().count(head))
            return tool_err("\"" + tokens.front() + "\" is an agent tool name, not a command. For "
                            + pm::brand::k_exe_basename_cli_u8
                            + " use a CLI path (audio record, llm agent, …); for programs use the executable name or path.");

        size_t start = 0;
        if (is_cli_exe_token(tokens.front()))
            start = 1;
        const bool internal = start == 1
            || (tokens.size() >= 1 && is_cli_root_slug(head));

        if (internal) {
            if (start >= tokens.size())
                return tool_err("\"" + command + "\" names the CLI but no subcommand. Use e.g. command=\"audio record\".");
            intent.kind = Kind::Internal;
            intent.cli_argv.assign(tokens.begin() + static_cast<std::ptrdiff_t>(start), tokens.end());
            intent.cli_argv.insert(intent.cli_argv.end(), cmd_args.begin(), cmd_args.end());
            for (const auto& a : intent.cli_argv)
                if (auto e = validate_variable_text(a); !e.empty()) return tool_err(e);

            // Dry-parse against the live grammar. `${…}` values are replaced by a
            // plausible literal so filesystem validators do not reject the token
            // itself; the real path is validated at execution time.
            std::vector<std::string> probe = intent.cli_argv;
            for (auto& a : probe)
                if (media::strutil::has_variable_tokens(a))
                    a = media::strutil::substitute_variables(a, [](std::string_view) {
                        return std::optional<std::string>(".");
                    });
            const auto chk = catalog::validate_cli_invocation(probe);
            if (!chk.ok)
                return tool_err("invalid " + std::string(pm::brand::k_exe_basename_cli_u8) + " invocation: "
                                + chk.error + ". Use info_lookup for the live subcommands and flags.");
            intent.cli_path = chk.command_path;
        } else {
            if (tokens.size() > 1)
                return tool_err("command must be a single executable name or path; put arguments in args");
            if (shell_interpreters().count(head))
                return tool_err("\"" + command + "\" is a shell. Use shell=\"" + head
                                + "\" and shell_line=\"…\" for one-liners.");
            if (denied_executables().count(head))
                return tool_err("\"" + command + "\" is a Windows script host / system utility and cannot be stored as a command.");
            for (const auto& a : cmd_args)
                if (auto e = validate_variable_text(a); !e.empty()) return tool_err(e);
            std::string err;
            intent.kind = Kind::External;
            intent.exe  = resolve_external_exe(command, candidates, err);
            if (intent.exe.empty())
                return tool_err(err, nlohmann::json{{"needs_resolution", true}, {"candidates", candidates}});
            if (denied_executables().count(basename_no_ext(intent.exe)))
                return tool_err("resolved executable \"" + intent.exe + "\" is not allowed");
            intent.args = cmd_args;

            std::string line = intent.exe;
            for (const auto& a : intent.args) { line += ' '; line += a; }
            const auto vr = media::llm::run::validate_command(line, "");
            if (vr.tier == media::llm::run::ValidationTier::Deny)
                return tool_err("blocked: " + vr.reason);
        }
    }

    // ── 4. Exposure gating ──────────────────────────────────────────────────
    const bool explorer = surface == "context_menu" || surface == "both";
    if (!explorer && !explorer_types.empty())
        return tool_err("explorer_file_types requires surface \"context_menu\" or \"both\"");
    if (as_voice_command && voice_command.empty())
        return tool_err("as_voice_command requires voice_command (the spoken phrase)");
    if (!voice_command.empty() && !as_voice_command)
        return tool_err("voice_command given but as_voice_command is false");
    const std::string voice_norm = normalize_phrase(voice_command);
    if (as_voice_command) {
        if (voice_norm.size() < 3)
            return tool_err("voice_command must contain at least 3 letters or digits");
        if (intent.kind == Kind::Shell)
            return tool_err("shell one-liners cannot be exposed as voice commands");
    }
    if (as_llm_tool) {
        if (intent.kind == Kind::Shell)
            return tool_err("shell one-liners cannot be exposed as agent tools");
        if (description.empty())
            return tool_err("as_llm_tool requires a description (the agent uses it to decide when to call the command)");
    }

    // ── 5. Build the entry (canonical representation) ───────────────────────
    nlohmann::json entry = nlohmann::json::object();
    entry["label"]    = label;
    entry["type"]     = "button";
    entry["enabled"]  = true;
    entry["visible"]  = true;
    entry["icon"]     = icon.empty() ? std::string("run") : icon;
    entry["source"]   = nlohmann::json{{"kind", "selection"}};
    if (selection_mode == "peritem")
        entry["source"]["selectionMode"] = "perItem";
    else if (selection_mode == "aggregate")
        entry["source"]["selectionMode"] = "aggregate";
    entry["output"]   = nlohmann::json::object();
    entry["userData"] = nlohmann::json{{"createdBy", "create_command"}};

    const char* type_tag = "external";
    switch (intent.kind) {
    case Kind::Internal: {
        type_tag = "internal";
        entry["cliCommand"] = intent.cli_argv.front();
        entry["args"] = nlohmann::json(std::vector<std::string>(intent.cli_argv.begin() + 1, intent.cli_argv.end()));
        if (!cwd.empty())
            entry["cwd"] = cwd;
        break;
    }
    case Kind::Shell: {
        type_tag = "shell";
        entry["externalCommand"] = nlohmann::json{
            {"mode",        "shell"},
            {"interpreter", intent.interpreter},
            {"shellLine",   intent.shell_line},
            {"command",     ""},
            {"args",        nlohmann::json::array()},
            {"cwd",         cwd},
        };
        break;
    }
    case Kind::External: {
        entry["externalCommand"] = nlohmann::json{
            {"mode",    "argv"},
            {"command", intent.exe},
            {"args",    nlohmann::json(intent.args)},
            {"cwd",     cwd},
        };
        break;
    }
    }

    if (!description.empty())        entry["description"] = description;
    if (!tint.empty())               entry["tint"] = tint;
    if (explorer)                    entry["registerInExplorer"] = true;
    if (!explorer_types.empty())     entry["explorerFileTypes"] = explorer_types;
    if (as_llm_tool)                 entry["asLlmTool"] = true;
    if (as_voice_command)          { entry["asVoiceCommand"] = true; entry["voiceCommand"] = voice_command; }
    if (show_in_launcher)            entry["showInLauncher"] = true;
    if (show_in_assistant_bar)       entry["showInAssistantBar"] = true;
    if (show_parameter_dialog)       entry["showParameterDialog"] = true;

    // ── 6. Serialised load → validate → insert → atomic save ────────────────
    std::string new_id, effective_group, save_err;
    bool created_group = false;
    std::optional<media::llm::path::ExecuteResult> failure;

    const bool saved = media::runtime_settings::update_command_json_utf8(
        [&](std::string& raw, std::string& err) -> bool {
            nlohmann::json doc;
            if (media::strutil::trim(raw).empty()) {
                doc = nlohmann::json{{"version", 1}, {"ribbon", {{"groups", nlohmann::json::array()}}}};
            } else {
                doc = media::json::try_parse(raw);
                if (doc.is_discarded()) {
                    err = "commands document is not valid JSON; refusing to overwrite it";
                    return false;
                }
                if (auto e = validate_document_shape(doc); !e.empty()) {
                    err = e + "; refusing to modify a corrupt document";
                    return false;
                }
                if (!doc.contains("ribbon"))
                    doc["ribbon"] = nlohmann::json{{"groups", nlohmann::json::array()}};
                if (!doc["ribbon"].contains("groups"))
                    doc["ribbon"]["groups"] = nlohmann::json::array();
            }

            ExistingIndex idx;
            index_items(doc, idx);
            if (idx.labels.count(media::strutil::lower_ascii(label))) {
                err = "a command labelled \"" + label + "\" already exists; choose a distinct label";
                return false;
            }
            if (as_voice_command && idx.voice_phrases.count(voice_norm)) {
                err = "voice phrase \"" + voice_command + "\" collides with an existing voice command";
                return false;
            }

            auto& groups = doc["ribbon"]["groups"];
            int target = -1;
            if (group_hint.empty()) {
                target = find_group_index(groups, k_default_group);
                if (target < 0) {
                    groups.push_back(nlohmann::json{{"label", k_default_group}, {"items", nlohmann::json::array()}});
                    target = static_cast<int>(groups.size()) - 1;
                    created_group = true;
                }
            } else {
                target = find_group_index(groups, group_hint);
                if (target < 0) {
                    if (!create_group) {
                        std::string names;
                        for (const auto& g : groups) {
                            if (!names.empty()) names += ", ";
                            names += "\"" + g.value("label", std::string{}) + "\"";
                        }
                        err = "group \"" + group_hint + "\" does not exist. Existing groups: "
                            + (names.empty() ? std::string("(none)") : names)
                            + ". Pass create_group=true to add it, or omit group for \"" + k_default_group + "\".";
                        return false;
                    }
                    groups.push_back(nlohmann::json{{"label", group_hint}, {"items", nlohmann::json::array()}});
                    target = static_cast<int>(groups.size()) - 1;
                    created_group = true;
                }
            }
            if (!groups[target].contains("items"))
                groups[target]["items"] = nlohmann::json::array();

            do {
                new_id = generate_id();
            } while (id_exists(doc, new_id));
            entry["id"] = new_id;

            groups[target]["items"].push_back(entry);
            effective_group = groups[target].value("label", std::string{});
            raw = doc.dump(2);
            return true;
        },
        save_err);

    if (!saved)
        return tool_err(save_err.empty() ? std::string("failed to save the commands document") : save_err);

    pm::log::info_lazy(k_ch, [&] {
        return "created id=" + new_id + " label=\"" + label + "\" type=" + type_tag
             + " group=\"" + effective_group + "\" surface=" + surface;
    });

    nlohmann::json out{
        {"tool",        "create_command"},
        {"command_id",  new_id},
        {"label",       label},
        {"type",        type_tag},
        {"surface",     surface},
        {"group",       effective_group},
        {"group_created", created_group},
        {"answer_rules", nlohmann::json::array({
            "The command has been written to the commands document.",
            std::string("Tell the user the exact command id so they can invoke it: ")
                + pm::brand::k_exe_basename_cli_u8 + " " + new_id,
            "Advise the user to reload the app or ribbon to see the new button.",
            "If surface includes context_menu / both, the user may need to re-run register-explorer.",
        })},
    };
    switch (intent.kind) {
    case Kind::Internal:
        out["cli_path"] = intent.cli_path;
        out["argv"]     = intent.cli_argv;
        break;
    case Kind::External:
        out["exe"]  = intent.exe;
        out["args"] = intent.args;
        break;
    case Kind::Shell:
        out["interpreter"] = intent.interpreter;
        out["shell_line"]  = intent.shell_line;
        break;
    }
    return tool_ok(std::move(out));
}

} // namespace media::llm::create_command

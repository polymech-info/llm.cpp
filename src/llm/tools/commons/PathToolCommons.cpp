// Copyright PolyMech / pm-image. Internal use only.
//
// Implementation of shared internals for the path-mode agent tools.
// Mirrors what `path_tool_executor.cpp::detail::*` used to hold before the
// split into per-category translation units (commons / fs / pictures / video
// / audio / memory / scheduler / inquire / mcp).
//
// Bodies are pasted verbatim from the pre-refactor file so behaviour is bit-
// for-bit identical — see git log path_tool_executor.cpp for the original.
#include "llm/tools/commons/PathToolCommons.hpp"
#include "json/json_utils.hpp"

#include "fs/fs_guard.hpp"
#include "core/meta.hpp"
#include "fs/path_sanitizer.hpp"
#include "core/settings_store.hpp"
#include "core/settings_runtime.hpp"
#include "ui_defaults.hpp"

#include "logging/pm_log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <system_error>
#include "string/string_utils.hpp"

namespace media::llm::path::detail {

// ── Thread-local agent state ───────────────────────────────────────────────
namespace {

thread_local std::unordered_set<std::string> tls_agent_tool_blocklist;
thread_local std::string                     tls_agent_path_base;
thread_local bool                            tls_agent_fs_jail = false;

/// True when @p child is the same as or under @p root (after lexically_normal).
bool path_is_under_root(const fs::path& child, const fs::path& root) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(child, ec);
    if (ec || c.empty())
        c = child.lexically_normal();
    fs::path r = fs::weakly_canonical(root, ec);
    if (ec || r.empty())
        r = root.lexically_normal();
    if (c.empty() || r.empty())
        return false;
    const fs::path rel = c.lexically_relative(r);
    if (rel.empty())
        return false;
    for (const auto& part : rel) {
        if (part == "..")
            return false;
    }
    return true;
}

/// Drop glob wildcards so jail checks the concrete root / parent of a pattern.
fs::path strip_glob_for_jail(fs::path p) {
    const auto s = media::path::path_to_utf8(p);
    const auto star = s.find_first_of("*?");
    if (star == std::string::npos)
        return p.lexically_normal();
    const auto slash = s.find_last_of("/\\", star == 0 ? 0 : star - 1);
    if (slash == std::string::npos)
        return fs::path{};
    return media::path::path_from_utf8(s.substr(0, slash)).lexically_normal();
}

} // namespace

const std::string& agent_path_base() noexcept                   { return tls_agent_path_base; }
bool               agent_path_base_empty() noexcept             { return tls_agent_path_base.empty(); }
const std::unordered_set<std::string>& agent_tool_blocklist() noexcept { return tls_agent_tool_blocklist; }

void set_agent_path_base_impl(const std::string& raw) {
    tls_agent_path_base.clear();
    if (raw.empty()) return;
    std::error_code ec;
    fs::path p = media::path::path_from_utf8(raw);
    if (!p.is_absolute()) p = fs::absolute(p, ec);
    if (ec) p = media::path::path_from_utf8(raw);
    p = p.lexically_normal();
    tls_agent_path_base = media::path::path_to_utf8(p);
    if (tls_agent_path_base.empty() && !raw.empty()) tls_agent_path_base = raw;
}

void clear_agent_path_base_impl() { tls_agent_path_base.clear(); }

void set_agent_tool_blocklist_impl(const std::vector<std::string>& disabled_names) {
    tls_agent_tool_blocklist.clear();
    tls_agent_tool_blocklist.reserve(disabled_names.size() * 2);
    for (const auto& d : disabled_names) {
        if (d.empty()) continue;
        std::string k = d;
        k = media::strutil::lower_ascii(std::move(k));
        const auto b = k.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        const auto e = k.find_last_not_of(" \t\r\n");
        k = k.substr(b, e - b + 1);
        if (!k.empty()) tls_agent_tool_blocklist.insert(std::move(k));
    }
}

void clear_agent_tool_blocklist_impl() { tls_agent_tool_blocklist.clear(); }

void set_agent_fs_jail_impl(bool enabled) { tls_agent_fs_jail = enabled; }

void clear_agent_fs_jail_impl() { tls_agent_fs_jail = false; }

bool agent_fs_jail_enabled() noexcept { return tls_agent_fs_jail; }

// ── Small string helpers ──────────────────────────────────────────────────

void trim_in_place(std::string& s) {
    const auto* ws = " \t\r\n";
    const auto  b  = s.find_first_not_of(ws);
    if (b == std::string::npos) { s.clear(); return; }
    s.erase(0, b);
    const auto e = s.find_last_not_of(ws);
    s.erase(e + 1);
}

// ── FS guard wrappers ─────────────────────────────────────────────────────
std::string llm_path_fs_deny(const std::string& abs_str) {
    const fs::path target = media::path::path_from_utf8(abs_str);
    if (const std::string sens = media::llm::llm_fs_guard_deny_reason(target); !sens.empty())
        return sens;
    if (!tls_agent_fs_jail)
        return {};
    if (tls_agent_path_base.empty())
        return "fs-jail: workspace cwd is required (set X-Tanit-Cwd / folder)";
    fs::path check = strip_glob_for_jail(target);
    if (check.empty())
        check = media::path::path_from_utf8(tls_agent_path_base);
    if (!path_is_under_root(check, media::path::path_from_utf8(tls_agent_path_base)))
        return "fs-jail: path outside workspace cwd";
    return {};
}

std::string llm_path_write_deny(const std::string& abs_str) {
    // Write deny includes the read/jail check so callers that only call write_deny
    // still cannot escape the workspace when --fs-jail is on.
    if (const std::string r = llm_path_fs_deny(abs_str); !r.empty())
        return r;
    return media::llm::llm_fs_guard_write_deny_reason(media::path::path_from_utf8(abs_str));
}

// ── JSON-arg accessors ────────────────────────────────────────────────────
const nlohmann::json& options_or_empty(const nlohmann::json& args) {
    static const nlohmann::json empty = nlohmann::json::object();
    if (args.contains("options") && args["options"].is_object()) return args["options"];
    return empty;
}

std::vector<std::string> get_string_array(const nlohmann::json& args, const char* key) {
    std::vector<std::string> out;
    if (!args.contains(key) || !args[key].is_array()) return out;
    for (const auto& v : args[key]) if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

std::optional<std::string> get_nonempty_string(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object() || !obj.contains(key) || !obj[key].is_string())
        return std::nullopt;
    std::string s = obj[key].get<std::string>();
    if (s.empty())
        return std::nullopt;
    return s;
}

std::optional<std::string> get_output_path_argument(const nlohmann::json& args) {
    if (auto s = get_nonempty_string(args, "output_path"))
        return s;
    if (auto s = get_nonempty_string(args, "output"))
        return s;
    const nlohmann::json& opts = options_or_empty(args);
    if (auto s = get_nonempty_string(opts, "output_path"))
        return s;
    if (auto s = get_nonempty_string(opts, "output"))
        return s;
    return std::nullopt;
}

// ── Path resolution (internal helpers, then public entry point) ───────────
namespace {

bool starts_with_ci(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size())
        return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(s[i]);
        const unsigned char b = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(a) != std::tolower(b))
            return false;
    }
    return true;
}

std::optional<fs::path> try_resolve_known_path_alias(const std::string& raw,
                                                     const fs::path&    cwd,
                                                     const fs::path&    /*base_eff*/,
                                                     const fs::path&    global_root)
{
    std::string s = raw;
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return std::nullopt;
    const auto e = s.find_last_not_of(" \t\r\n");
    s = s.substr(b, e - b + 1);
    if (s.empty())
        return std::nullopt;

    auto join_under = [](const fs::path& root, std::string tail) -> fs::path {
        while (!tail.empty() && (tail.front() == '/' || tail.front() == '\\'))
            tail.erase(tail.begin());
        if (tail.empty())
            return root.lexically_normal();
        return (root / media::path::path_from_utf8(tail)).lexically_normal();
    };

    auto resolve_for = [&](const fs::path& root, std::string_view token) -> std::optional<fs::path> {
        if (s.size() == token.size() && starts_with_ci(s, token))
            return root.lexically_normal();
        if (starts_with_ci(s, token)) {
            const std::string tail = s.substr(token.size());
            return join_under(root, tail);
        }
        return std::nullopt;
    };

    for (const std::string_view t : {
             std::string_view("cwd:"),
             std::string_view("cwd/"),
             std::string_view("cwd\\"),
             std::string_view("$cwd"),
             std::string_view("${cwd}"),
             std::string_view("%cwd%"),
             std::string_view("%cd%"),
         }) {
        if (auto p = resolve_for(cwd, t))
            return p;
    }

    for (const std::string_view t : {
             std::string_view("global:"),
             std::string_view("global/"),
             std::string_view("global\\"),
             std::string_view("$global"),
             std::string_view("${global}"),
             std::string_view("%global%"),
         }) {
        if (auto p = resolve_for(global_root, t))
            return p;
    }

    return std::nullopt;
}

bool ends_with_ci(const std::string& s, const char* suffix) {
    size_t sl = 0;
    while (suffix[sl])
        ++sl;
    if (s.size() < sl)
        return false;
    for (size_t i = 0; i < sl; ++i) {
        const unsigned char a = static_cast<unsigned char>(s[s.size() - sl + i]);
        const unsigned char b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b))
            return false;
    }
    return true;
}

/// `coll.jpg` in the agent cwd / folder — no slash, still a resolvable local path for tools + Replicate.
bool bare_media_filename_no_separators(const std::string& s) {
    if (s.empty())
        return false;
    if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos)
        return false;
    if (s.size() >= 2 && s[1] == ':')
        return false;
    const size_t dot = s.rfind('.');
    if (dot == std::string::npos || dot == 0)
        return false;
    static const char* suf[] = {".jpg",  ".jpeg", ".png",  ".webp", ".gif", ".bmp", ".tif", ".tiff",
                                ".heic", ".avif", ".jxl",  ".mp4",  ".webm", ".mov", ".m4v"};
    for (const char* e : suf) {
        if (ends_with_ci(s, e))
            return true;
    }
    return false;
}

/// Same idea as transform.cpp `string_might_be_local_file_path`: do not treat
/// short enum tokens (e.g. "16:9", "1080p") or prose prompts as relative paths.
bool tool_string_looks_like_path_for_resolve(const std::string& s) {
    if (s.empty() || s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0 || s.rfind("data:", 0) == 0)
        return false;
    if (s.size() >= 2 && s[1] == ':')
        return true;
    if (!s.empty() && (s[0] == '/' || s[0] == '\\'))
        return true;
    if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos)
        return true;
    return bare_media_filename_no_separators(s);
}

} // namespace

std::string resolve_tool_path_string(const std::string& raw) {
    const std::string cleaned = media::path::normalize_local_path(raw, true);
    if (cleaned.empty()) return cleaned;
    const bool has_glob =
        cleaned.find('*') != std::string::npos || cleaned.find('?') != std::string::npos;
    std::error_code ec_cwd;
    const fs::path cwd = fs::current_path(ec_cwd);
    if (ec_cwd) return cleaned;

    const fs::path fallback_base = media::ui_defaults::fallback_work_folder();
    const fs::path base_eff = tls_agent_path_base.empty()
        ? (fallback_base.empty() ? cwd : fallback_base)
        : media::path::path_from_utf8(tls_agent_path_base);
    const fs::path global_root = media::settings::get_config_dir();
    if (const auto aliased = try_resolve_known_path_alias(cleaned, cwd, base_eff, global_root); aliased.has_value()) {
        std::error_code ec_alias;
        const fs::path p = aliased->lexically_normal();
        if (has_glob) return media::path::path_to_utf8(p);
        const fs::path c = fs::weakly_canonical(p, ec_alias);
        if (!ec_alias) return media::path::path_to_utf8(c);
        return media::path::path_to_utf8(p);
    }

    std::error_code ec;
    fs::path p_in = media::path::path_from_utf8(cleaned);
    if (p_in.is_absolute()) {
        fs::path p = p_in.lexically_normal();
        if (has_glob) return media::path::path_to_utf8(p);
        const fs::path c = fs::weakly_canonical(p, ec);
        if (!ec) return media::path::path_to_utf8(c);
        return media::path::path_to_utf8(p);
    }

    const fs::path rel = p_in.lexically_normal();
    fs::path p = (base_eff / rel).lexically_normal();

    if (!tls_agent_path_base.empty()) {
        const fs::path p_cwd = (cwd / rel).lexically_normal();
        if (p_cwd != p) {
            std::error_code e_base, e_cwd;
            const bool ok_base = fs::exists(p, e_base);
            const bool ok_cwd = fs::exists(p_cwd, e_cwd);
            if (ok_cwd && !ok_base)
                p = p_cwd;
        }
    }

    if (has_glob) return media::path::path_to_utf8(p);
    const fs::path c = fs::weakly_canonical(p, ec);
    if (!ec) return media::path::path_to_utf8(c);
    return media::path::path_to_utf8(p);
}

void resolve_tool_paths_strings_in_json(nlohmann::json& j, const std::string* parent_obj_key) {
    if (j.is_string()) {
        if (parent_obj_key != nullptr) {
            const std::string& k = *parent_obj_key;
            if (k == "prompt" || k == "negative_prompt")
                return;
        }
        std::string s = j.get<std::string>();
        if (s.empty() || s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0 || s.rfind("data:", 0) == 0)
            return;
        if (!tool_string_looks_like_path_for_resolve(s))
            return;
        j = resolve_tool_path_string(s);
    } else if (j.is_array()) {
        for (auto& el : j)
            resolve_tool_paths_strings_in_json(el, nullptr);
    } else if (j.is_object()) {
        for (auto& kv : j.items()) {
            const std::string keyCopy = kv.key();
            resolve_tool_paths_strings_in_json(kv.value(), &keyCopy);
        }
    }
}

std::string make_sibling_path(const std::string& input,
                              const std::string& out_dir,
                              const std::string& suffix,
                              const std::string& new_ext_with_dot) {
    fs::path p = media::path::path_from_utf8(input);
    std::string stem = media::path::path_to_utf8(p.stem());
    if (!suffix.empty()) stem += suffix;
    const std::string ext = new_ext_with_dot.empty() ? media::path::path_to_utf8(p.extension()) : new_ext_with_dot;
    const std::string filename = stem + ext;
    fs::path dir = out_dir.empty() ? p.parent_path() : media::path::path_from_utf8(out_dir);
    if (!out_dir.empty()) {
        std::error_code ec;
        fs::create_directories(dir, ec); // best-effort; worker will surface I/O errors
    }
    return media::path::path_to_utf8(dir / media::path::path_from_utf8(filename));
}

// ── Result envelope helpers ───────────────────────────────────────────────
ExecuteResult build_envelope(const std::string& tool, std::vector<PerFileResult> per_file) {
    ExecuteResult r;
    r.ok = true;
    int succeeded = 0, failed = 0;
    nlohmann::json results = nlohmann::json::array();
    for (const auto& f : per_file) {
        nlohmann::json e;
        e["path"] = f.path;
        e["ok"]   = f.ok;
        if (!f.ok) {
            e["error"] = f.error;
            if (!f.result.is_null() && !f.result.empty())
                e["result"] = f.result;
            ++failed;
        } else {
            ++succeeded;
            if (!f.output_path.empty()) e["output_path"] = f.output_path;
            if (!f.result.is_null())    e["result"]      = f.result;
        }
        results.push_back(std::move(e));
    }
    r.envelope = nlohmann::json{
        {"ok",      true},
        {"tool",    tool},
        {"summary", {
            {"total",     (int)per_file.size()},
            {"succeeded", succeeded},
            {"failed",    failed},
        }},
        {"results", std::move(results)},
    };
    r.per_file = std::move(per_file);
    return r;
}

ExecuteResult fatal(const std::string& msg) {
    ExecuteResult r;
    r.ok = false;
    r.error = msg;
    r.envelope = nlohmann::json{{"ok", false}, {"error", msg}};
    return r;
}

ExecuteResult simple_error(const std::string& msg) {
    ExecuteResult r;
    r.ok       = false;
    r.error    = msg;
    r.envelope = nlohmann::json{{"ok", false}, {"error", msg}};
    return r;
}

ExecuteResult simple_ok(nlohmann::json payload) {
    ExecuteResult r;
    r.ok = true;
    if (payload.is_object()) {
        r.envelope = std::move(payload);
    } else if (payload.is_array()) {
        r.envelope = nlohmann::json{{"items", std::move(payload)}};
        r.envelope["count"] = r.envelope["items"].size();
    } else {
        r.envelope = nlohmann::json{{"data", std::move(payload)}};
    }
    r.envelope["ok"] = true;
    return r;
}

// ── Sidecar cache lookup ──────────────────────────────────────────────────
bool try_image_meta_from_sidecar(const std::string&    in_abs,
                                 const media::MetaOptions& opt,
                                 const nlohmann::json&  tool_options,
                                 PerFileResult&         r)
{
    if (opt.dry_run) return false;
    if (opt.update_exif) return false;  // needs a fresh model payload to write EXIF
    if (tool_options.value("regenerate", false)) return false;

    if (!opt.out_md && !opt.out_json) return false;

    const fs::path    src  = media::path::path_from_utf8(in_abs);
    const fs::path    dir  = opt.out_dir.empty() ? src.parent_path() : media::path::path_from_utf8(opt.out_dir);
    const std::string stem = media::path::path_to_utf8(src.stem());
    const fs::path    md_p   = dir / (stem + ".md");
    const fs::path    json_p = dir / (stem + ".json");

    auto nonempty = [](const fs::path& p) {
        std::error_code ec;
        if (!fs::exists(p, ec) || !fs::is_regular_file(p, ec)) return false;
        return fs::file_size(p, ec) > 0;
    };

    if (nonempty(json_p)) {
        std::ifstream      ifs(json_p, std::ios::binary);
        if (ifs) {
            std::ostringstream ss;
            ss << ifs.rdbuf();
            nlohmann::json parsed = media::json::try_parse(ss.str());
            if (!parsed.is_discarded()) {
                parsed["_from_sidecar_cache"] = true;
                r.result        = std::move(parsed);
                r.ok            = true;
                r.output_path   = media::path::path_to_utf8(json_p);
                return true;
            }
        }
    }

    if (opt.out_md && nonempty(md_p)) {
        std::ifstream      ifs(md_p, std::ios::binary);
        if (!ifs) return false;
        std::ostringstream ss;
        ss << ifs.rdbuf();
        r.result = nlohmann::json{
            {"markdown",          ss.str()},
            {"_from_sidecar_cache", true},
        };
        r.ok          = true;
        r.output_path = media::path::path_to_utf8(md_p);
        return true;
    }

    return false;
}

// ── Dispatcher-side helpers ───────────────────────────────────────────────
void log_path_tool_invoke_context(const std::string& tool, const nlohmann::json& args) {
    constexpr const char k_ch[] = "llm_path_tools";
    std::error_code ec;
    const fs::path cwd_p = fs::current_path(ec);
    const std::string cwd_s = ec ? std::string("(unavailable)") : media::path::path_to_generic_utf8(cwd_p);
    const std::string base_s = tls_agent_path_base.empty()
        ? std::string("(unset)")
        : media::path::path_to_generic_utf8(media::path::path_from_utf8(tls_agent_path_base));

    std::string slim;
    try {
        if (!args.is_object()) {
            slim = args.dump();
        } else if (tool == "file_glob") {
            nlohmann::json j;
            if (args.contains("pattern")) j["pattern"] = args["pattern"];
            if (args.contains("options")) j["options"] = args["options"];
            slim = j.dump();
        } else if (tool == "list_images") {
            nlohmann::json j;
            if (args.contains("inputs")) j["inputs"] = args["inputs"];
            if (args.contains("options")) j["options"] = args["options"];
            slim = j.dump();
        } else if (tool == "file_read") {
            nlohmann::json j;
            if (args.contains("path")) j["path"] = args["path"];
            if (args.contains("offset")) j["offset"] = args["offset"];
            if (args.contains("limit")) j["limit"] = args["limit"];
            if (args.contains("options")) j["options"] = args["options"];
            slim = j.dump();
        } else if (tool == "write_file") {
            nlohmann::json j;
            if (args.contains("path")) j["path"] = args["path"];
            if (args.contains("content") && args["content"].is_string())
                j["content_bytes"] = args["content"].get<std::string>().size();
            if (args.contains("options")) j["options"] = args["options"];
            if (args.contains("expected_version")) j["expected_version"] = true;
            slim = j.dump();
        } else if (tool == "file_compose") {
            nlohmann::json j;
            if (args.contains("output")) j["output"] = args["output"];
            if (args.contains("overwrite")) j["overwrite"] = args["overwrite"];
            if (args.contains("expected_version")) j["expected_version"] = true;
            if (args.contains("parts") && args["parts"].is_array()) {
                j["parts"] = args["parts"].size();
                std::size_t literal_bytes = 0;
                std::size_t file_parts = 0;
                for (const auto& part : args["parts"]) {
                    if (!part.is_object()) continue;
                    const std::string type = part.value("type", std::string{});
                    if (type == "literal" && part.contains("text") && part["text"].is_string())
                        literal_bytes += part["text"].get<std::string>().size();
                    else if (type == "file")
                        ++file_parts;
                }
                j["file_parts"] = file_parts;
                j["literal_bytes"] = literal_bytes;
            }
            slim = j.dump();
        } else if (tool == "file_str_replace") {
            nlohmann::json j;
            if (args.contains("path")) j["path"] = args["path"];
            if (args.contains("old_string") && args["old_string"].is_string())
                j["old_bytes"] = args["old_string"].get<std::string>().size();
            if (args.contains("new_string") && args["new_string"].is_string())
                j["new_bytes"] = args["new_string"].get<std::string>().size();
            if (args.contains("replace_all")) j["replace_all"] = args["replace_all"];
            if (args.contains("start_line")) j["start_line"] = args["start_line"];
            if (args.contains("end_line")) j["end_line"] = args["end_line"];
            if (args.contains("expected_version")) j["expected_version"] = true;
            slim = j.dump();
        } else if (tool == "file_delete") {
            nlohmann::json j;
            if (args.contains("path")) j["path"] = args["path"];
            if (args.contains("expected_version")) j["expected_version"] = true;
            slim = j.dump();
        } else if (tool == "image_find") {
            nlohmann::json j;
            if (args.contains("inputs")) j["inputs"] = args["inputs"];
            if (args.contains("options")) j["options"] = args["options"];
            slim = j.dump();
        } else if (tool == "file_search") {
            nlohmann::json j;
            if (args.contains("pattern")) j["pattern"] = args["pattern"];
            if (args.contains("path"))    j["path"]    = args["path"];
            if (args.contains("glob"))    j["glob"]    = args["glob"];
            if (args.contains("type"))    j["type"]    = args["type"];
            if (args.contains("output_mode")) j["output_mode"] = args["output_mode"];
            slim = j.dump();
        } else if (tool == "run") {
            nlohmann::json j;
            if (args.contains("command") && args["command"].is_string()) {
                std::string cmd = args["command"].get<std::string>();
                if (cmd.size() > 500)
                    cmd = cmd.substr(0, 500) + "...";
                j["command"] = cmd;
            }
            if (args.contains("shell"))      j["shell"]      = args["shell"];
            if (args.contains("timeout_ms")) j["timeout_ms"] = args["timeout_ms"];
            slim = j.dump();
        } else {
            nlohmann::json j = nlohmann::json::object();
            for (auto it = args.begin(); it != args.end(); ++it) {
                const std::string& k = it.key();
                const auto&      v = it.value();
                if (k == "image" && v.is_object()) {
                    nlohmann::json im = nlohmann::json::object();
                    if (v.contains("mime")) im["mime"] = v["mime"];
                    if (v.contains("b64") && v["b64"].is_string())
                        im["b64_bytes"] = v["b64"].get<std::string>().size();
                    j["image"] = std::move(im);
                } else if (k == "references" && v.is_array()) {
                    nlohmann::json arr = nlohmann::json::array();
                    for (const auto& el : v) {
                        if (!el.is_object()) continue;
                        nlohmann::json one;
                        if (el.contains("mime")) one["mime"] = el["mime"];
                        if (el.contains("b64") && el["b64"].is_string())
                            one["b64_bytes"] = el["b64"].get<std::string>().size();
                        arr.push_back(std::move(one));
                    }
                    j["references"] = std::move(arr);
                } else if (k == "paths" || k == "inputs" || k == "options" || k == "path")
                    j[k] = v;
            }
            slim = j.empty() ? std::string("{...}") : j.dump();
        }
    } catch (...) {
        slim = "<args summary error>";
    }

    pm::log::info_lazy(k_ch, [&tool, cwd_s, base_s, slim]() -> std::string {
        return std::string("invoke ctx tool=") + tool + " cwd=" + cwd_s + " path_base=" + base_s + " " + slim;
    });
}

std::string strip_template_prefix(const std::string& name) {
    if (name.empty() || name[0] != '$') return name;
    size_t i = 1;
    size_t last_underscore = std::string::npos;
    while (i < name.size()) {
        unsigned char ch = static_cast<unsigned char>(name[i]);
        if (std::isupper(ch) || ch == '_') {
            if (ch == '_') last_underscore = i;
            ++i;
        } else {
            break;
        }
    }
    if (last_underscore == std::string::npos) return name;
    std::string tail = name.substr(last_underscore + 1);
    return tail.empty() ? name : tail;
}

} // namespace media::llm::path::detail

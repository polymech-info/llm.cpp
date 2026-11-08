// Copyright PolyMech / pm-image. Internal use only.
//
// Bodies pasted verbatim from the pre-split path_tool_executor.cpp — see
// `git log src/llm/path_tool_executor.cpp` for prior history.
#include "llm/tools/fs/FsTools.hpp"

#include "fs/path_sanitizer.hpp"
#include "core/settings_store.hpp"
#include "lib/net/http_client.hpp"
#include "media/probe.hpp"

#include "llm/change_store.hpp"
#include "llm/agent_memory.hpp"
#include "core/cli_cancel.hpp"
#include "fs/file_extensions.hpp"
#include "core/find.hpp"
#include "fs/glob_paths.hpp"
#include "core/meta.hpp"
#include "core/search.hpp"
#include "core/app_exe_directory.hpp"
#include "search/search_facade.hpp"
#include "search/search_presets.hpp"
#include "search/search_request.hpp"
#include "fs/glob_filter.hpp"
#include "llm/llm_constants.hpp"
#if defined(FEATURE_COMMAND_MARKDOWN) && FEATURE_COMMAND_MARKDOWN
#include "markdown/markdown.hpp"
#endif
#include "llm/llm_image_tool_defaults.hpp"
#include "ui_defaults.hpp"

#include "logging/pm_log.hpp"
#include "llm/tools/commons/PathToolCommons.hpp"
#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <vector>
#include "string/string_utils.hpp"

namespace media::llm::path::detail {

namespace {

thread_local std::function<void(std::uint64_t, std::uint64_t)> tls_download_progress;

} // namespace

void set_download_url_progress_hook(std::function<void(std::uint64_t, std::uint64_t)> f) {
    tls_download_progress = std::move(f);
}

void clear_download_url_progress_hook() {
    tls_download_progress = {};
}

namespace {

void attach_history(nlohmann::json& result, const char* tool, const fs::path& path,
                    std::string_view before, std::string_view after) {
    media::llm::change::RecordInput in;
    in.session_id = media::llm::agent::get_current_task_id();
    in.tool = tool;
    in.path = media::path::path_to_utf8(path);
    in.before = std::string(before);
    in.after = std::string(after);
    in.meta = {
        {"source", "agent"},
        {"tool", tool},
    };
    const auto rec = media::llm::change::record_mutation(in);
    media::llm::change::attach_record_fields(result, rec);
}

bool stale_version(const char* tool, std::string_view before, const nlohmann::json& args,
                   ExecuteResult& out) {
    std::string err;
    if (media::llm::change::check_expected_version(before, args, err))
        return false;
    out = fatal(std::string(tool) + ": " + err);
    return true;
}

} // namespace

// ── fs tool helpers (TU-local) ─────────────────────────────────────────────
namespace {

std::wstring lower_ext_w(const fs::path& p) {
    std::wstring e = p.extension().wstring();
    for (auto& c : e)
        c = static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(c)));
    return e;
}

std::string narrow_ext_lower(const fs::path& p) {
    std::string e = media::path::path_to_utf8(p.extension());
    e = media::strutil::lower_ascii(std::move(e));
    return e;
}

bool path_is_listable_image(const fs::path& p, bool include_raw) {
    const std::wstring ext = lower_ext_w(p);
    return pmui::is_standard_image_ext(ext) || (include_raw && pmui::is_raw_ext(ext));
}

void append_probe_metadata(const fs::path& p, nlohmann::json& entry) {
    const auto probe = media::probe::probe_file(p);
    if (!probe.ok) return;
    entry["contentType"] = probe.content_type;
    entry["kind"] = std::string(media::probe::media_kind_name(probe.kind));
    if (!probe.preferred_extension.empty())
        entry["preferredExt"] = probe.preferred_extension;
}

bool file_glob_append_one_file(const fs::path& p, bool skip_dev, nlohmann::json& files,
                               int& skipped_dev, int& skipped_policy,
                               std::string& policy_error) {
    std::error_code ec2;
    fs::path canon = fs::weakly_canonical(p, ec2);
    if (ec2 || canon.empty()) canon = fs::absolute(p, ec2);
    if (skip_dev && media::llm::glob_path_has_excluded_component(canon)) {
        ++skipped_dev;
        return true;
    }
    if (const std::string w = llm_path_fs_deny(media::path::path_to_utf8(canon)); !w.empty()) {
        if (pm::llm::LLM_FS_GUARD_STRICT) {
            policy_error = w;
            return false;
        }
        ++skipped_policy;
        return true;
    }
    const std::string ext = narrow_ext_lower(canon);
    std::error_code ecs;
    const auto sz = fs::file_size(canon, ecs);
    nlohmann::json e;
    e["path"] = media::path::path_to_generic_utf8(canon);
    e["ext"]  = ext;
    e["size"] = ecs ? 0 : static_cast<std::uint64_t>(sz);
    append_probe_metadata(canon, e);
    files.push_back(std::move(e));
    return true;
}

bool glob_star_match(std::string_view text, std::string_view pat)
{
    std::size_t i = 0, j = 0, back = 0;
    std::size_t star = std::string_view::npos;
    while (i < text.size()) {
        if (j < pat.size() && (pat[j] == '?' || pat[j] == text[i])) {
            ++i;
            ++j;
        } else if (j < pat.size() && pat[j] == '*') {
            star = j++;
            back = i;
        } else if (star != std::string_view::npos) {
            j = star + 1;
            i = ++back;
        } else {
            return false;
        }
    }
    while (j < pat.size() && pat[j] == '*')
        ++j;
    return j == pat.size();
}

// p-ranav rglob follows directory symlinks and materializes every directory
// before it filters. On Android a relative `**/*.png` starts that walk at `/`
// (symlink cycles under /proc and /storage) and never returns.
struct FileGlobQuery {
    fs::path root;
    std::string name_pat = "*";
    bool recursive = false;
};

bool build_file_glob_query(const std::string& resolved, bool opt_recursive, FileGlobQuery& q,
                           std::string& err)
{
    std::string s = resolved;
    for (char& c : s)
        if (c == '\\')
            c = '/';
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    const bool absolute = !s.empty() && (s.front() == '/' || (s.size() >= 2 && s[1] == ':'));

    std::vector<std::string> segs;
    std::string cur;
    for (char c : s) {
        if (c == '/') {
            if (!cur.empty()) {
                segs.push_back(std::move(cur));
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        segs.push_back(std::move(cur));

    std::size_t magic = segs.size();
    for (std::size_t i = 0; i < segs.size(); ++i) {
        if (segs[i] == "**" || segs[i].find('*') != std::string::npos ||
            segs[i].find('?') != std::string::npos) {
            magic = i;
            break;
        }
    }

    // Join the concrete prefix as a generic string, then parse once.
    // `path("C:") / "Users"` is the drive-relative path "C:Users", not "C:\Users",
    // so a shared folder such as C:\Users\...\pixlwiz fails is_directory.
    std::string root_utf8;
    if (absolute && !s.empty() && s.front() == '/')
        root_utf8 = "/";
    bool any_doublestar = false;
    std::string name_pat = "*";
    for (std::size_t i = 0; i < segs.size(); ++i) {
        if (i < magic) {
            if (!root_utf8.empty() && root_utf8.back() != '/')
                root_utf8.push_back('/');
            root_utf8 += segs[i];
        } else if (segs[i] == "**")
            any_doublestar = true;
        else
            name_pat = segs[i];
    }
    fs::path root = root_utf8.empty() ? fs::path{} : media::path::path_from_utf8(root_utf8);
    if (!absolute) {
        std::string base = agent_path_base();
        if (base.empty()) {
            const fs::path fb = media::ui_defaults::fallback_work_folder();
            if (!fb.empty())
                base = media::path::path_to_utf8(fb);
        }
        if (!base.empty())
            root = media::path::path_from_utf8(base) / root;
    }
    q.root = root.lexically_normal();
    q.name_pat = name_pat.empty() ? "*" : name_pat;
    q.recursive = any_doublestar || opt_recursive;

    std::error_code ec;
    fs::path canon = fs::weakly_canonical(q.root, ec);
    if (!ec && !canon.empty())
        q.root = std::move(canon);
    if (q.root.empty() || q.root == q.root.root_path()) {
        err = "refusing to scan from the filesystem root. Anchor the pattern under the workspace.";
        return false;
    }
    if (!fs::is_directory(q.root, ec)) {
        err = "not a directory: " + media::path::path_to_utf8(q.root);
        return false;
    }
    return true;
}

bool collect_files_file_glob(const fs::path& root, bool recursive, int max_results, bool skip_dev,
                             nlohmann::json& files, int& skipped_dev, int& skipped_policy,
                             std::string& policy_error) {
    std::error_code ec;
    const auto opts = fs::directory_options::skip_permission_denied;
    if (!recursive) {
        for (const auto& ent : fs::directory_iterator(root, opts, ec)) {
            media::cli::yield_to_interrupt();
            if (media::cli::cancel_requested()) return true;
            if (!ent.is_regular_file(ec)) continue;
            if (static_cast<int>(files.size()) >= max_results) return true;
            if (!file_glob_append_one_file(ent.path(), skip_dev, files, skipped_dev, skipped_policy, policy_error))
                return false;
        }
        return true;
    }
    for (fs::recursive_directory_iterator it(root, opts, ec), end; it != end; it.increment(ec)) {
        media::cli::yield_to_interrupt();
        if (media::cli::cancel_requested()) return true;
        if (ec) {
            ec.clear();
            continue;
        }
        const fs::directory_entry& ent = *it;
        if (ent.is_directory(ec)) {
            if (skip_dev && media::llm::glob_exclude_default_path_component(media::path::path_to_utf8(ent.path().filename())))
                it.disable_recursion_pending();
            continue;
        }
        if (!ent.is_regular_file(ec)) continue;
        if (static_cast<int>(files.size()) >= max_results) return true;
        if (!file_glob_append_one_file(ent.path(), skip_dev, files, skipped_dev, skipped_policy, policy_error))
            return false;
    }
    return true;
}

bool collect_images_from(const fs::path& root, bool recursive, bool include_raw,
                         std::vector<fs::path>& out, std::size_t cap,
                         std::string& policy_error) {
    if (cap > 0 && out.size() >= cap) return true;
    std::error_code ec;
    auto check = [&](const fs::path& p) -> bool {
        if (cap > 0 && out.size() >= cap) return true;
        if (const std::string w = llm_path_fs_deny(media::path::path_to_utf8(p)); !w.empty()) {
            if (pm::llm::LLM_FS_GUARD_STRICT) {
                policy_error = w;
                return false;
            }
            return true;
        }
        if (path_is_listable_image(p, include_raw)) {
            out.push_back(p);
        }
        return true;
    };
    if (fs::is_regular_file(root, ec)) return check(root);
    if (!fs::is_directory(root, ec)) return true;
    if (recursive) {
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (!it->is_regular_file(ec)) continue;
            if (!check(it->path())) return false;
            if (cap > 0 && out.size() >= cap) return true;
        }
    } else {
        for (auto& entry : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (!check(entry.path())) return false;
            if (cap > 0 && out.size() >= cap) return true;
        }
    }
    return true;
}

constexpr std::size_t k_write_file_max_bytes = 16 * 1024 * 1024;
constexpr std::size_t k_file_read_window_bytes = pm::llm::k_file_read_window_bytes;
constexpr std::uintmax_t k_file_read_max_file_bytes = 16 * 1024 * 1024;
constexpr int k_file_read_max_lines = pm::llm::k_file_read_window_lines;

bool json_get_int(const nlohmann::json& args, const char* key, int& out) {
    if (!args.contains(key)) return false;
    const auto& v = args[key];
    if (v.is_number_integer()) {
        out = v.get<int>();
        return true;
    }
    if (v.is_number_unsigned()) {
        const auto u = v.get<unsigned>();
        if (u > static_cast<unsigned>(std::numeric_limits<int>::max()))
            return false;
        out = static_cast<int>(u);
        return true;
    }
    return false;
}

bool buffer_looks_binary_text(const std::string& buf) {
    if (buf.find('\0') != std::string::npos) return true;
    const std::size_t scan = std::min(buf.size(), static_cast<std::size_t>(65536));
    std::size_t ctrl = 0;
    for (std::size_t i = 0; i < scan; ++i) {
        const auto c = static_cast<unsigned char>(buf[i]);
        if (c < 32 && c != '\t' && c != '\n' && c != '\r') ++ctrl;
    }
    return scan > 8000 && ctrl * 10 > scan;
}

std::string ext_lower_dot(const fs::path& p) {
    std::string ext = media::path::path_to_utf8(p.extension());
    ext = media::strutil::lower_ascii(std::move(ext));
    return ext;
}

bool ext_blocked_text_tool(const fs::path& p) {
    return media::llm::glob_file_extension_blocked_like_file_read(ext_lower_dot(p));
}

bool ext_is_markdown_document(const fs::path& p) {
    const std::string ext = ext_lower_dot(p);
    static const char* k[] = {
        ".doc", ".docx", ".docm", ".odt", ".pdf",
        ".ppt", ".pps", ".pot", ".pptx", ".pptm", ".ppsx", ".ppsm",
        ".rtf", ".epub",
        ".xls", ".xlsx", ".xlsm", ".xlsb", ".ods", ".odp", ".csv",
    };
    for (const char* e : k) {
        if (ext == e)
            return true;
    }
    return false;
}

struct LineSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
};

void collect_line_spans(const std::string& buf, std::vector<LineSpan>& out) {
    out.clear();
    const std::size_t n = buf.size();
    std::size_t i = 0;
    while (i < n) {
        const std::size_t start = i;
        while (i < n && buf[i] != '\n' && buf[i] != '\r')
            ++i;
        out.push_back({start, i});
        if (i >= n) break;
        if (buf[i] == '\r' && i + 1 < n && buf[i + 1] == '\n')
            i += 2;
        else
            ++i;
    }
}

std::string to_lf(std::string s) {
    std::string o;
    o.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') {
            o += '\n';
            ++i;
        } else if (s[i] == '\r') {
            o += '\n';
        } else {
            o += s[i];
        }
    }
    return o;
}

std::string apply_file_eol(const std::string& s, bool crlf) {
    std::string lf = to_lf(s);
    if (!crlf) return lf;
    std::string o;
    o.reserve(lf.size() + lf.size() / 8);
    for (char c : lf) {
        if (c == '\n') {
            o += '\r';
            o += '\n';
        } else {
            o += c;
        }
    }
    return o;
}

int line_number_at(const std::string& buf, std::size_t pos) {
    int line = 1;
    const std::size_t n = std::min(pos, buf.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (buf[i] == '\n')
            ++line;
    }
    return line;
}

bool read_file_bytes(const fs::path& p, std::string& out, std::string& err) {
    std::error_code ec;
    const auto sz = fs::file_size(p, ec);
    if (ec) {
        err = "could not read file size: " + ec.message();
        return false;
    }
    if (sz > k_file_read_max_file_bytes) {
        err = "file exceeds 16 MiB limit";
        return false;
    }
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        err = "could not open file for reading";
        return false;
    }
    out.assign(static_cast<std::size_t>(sz), '\0');
    if (sz > 0) {
        in.read(out.data(), static_cast<std::streamsize>(sz));
        if (!in || static_cast<std::uintmax_t>(in.gcount()) != sz) {
            err = "could not read full file";
            return false;
        }
    }
    return true;
}

bool atomic_write_utf8(const fs::path& path, const std::string& data, std::string& err) {
    const fs::path tmp = path.native() + fs::path(".tmp~").native();
    {
        std::ofstream f(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "cannot open tmp: " + media::path::path_to_generic_utf8(tmp);
            return false;
        }
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) {
            err = "write failed: " + media::path::path_to_generic_utf8(tmp);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        err = "rename failed: " + ec.message();
        return false;
    }
    return true;
}

std::vector<std::size_t> find_all_nonoverlap(const std::string& hay, const std::string& needle) {
    std::vector<std::size_t> hits;
    if (needle.empty()) return hits;
    for (std::size_t p = 0;;) {
        p = hay.find(needle, p);
        if (p == std::string::npos) break;
        hits.push_back(p);
        p += needle.size();
    }
    return hits;
}

} // namespace

ExecuteResult do_list_images(const nlohmann::json& args) {
    auto inputs = get_string_array(args, "inputs");
    if (inputs.empty()) return fatal("list_images: 'inputs' (string array) is required and non-empty");

    {
        std::string resolved_inputs;
        for (const auto& spec : inputs) {
            if (!resolved_inputs.empty()) resolved_inputs += "; ";
            resolved_inputs += media::path::path_to_generic_utf8(media::path::path_from_utf8(resolve_tool_path_string(spec)));
        }
        pm::log::info_lazy("llm_path_tools", [resolved_inputs]() -> std::string {
            return std::string("list_images inputs_resolved=") + resolved_inputs;
        });
    }

    const auto& opts = options_or_empty(args);
    const bool        recursive    = opts.value("recursive",    true);
    const bool        include_raw  = opts.value("include_raw",  true);
    const std::size_t max_results  = static_cast<std::size_t>(std::max(0, opts.value("max_results", 0)));

    std::vector<fs::path> hits;
    int scanned_inputs = 0;
    std::string policy_error;
    for (const auto& spec : inputs) {
        media::cli::yield_to_interrupt();
        if (media::cli::cancel_requested()) {
            nlohmann::json files = nlohmann::json::array();
            for (const auto& p : hits) {
                std::error_code ec2;
                const auto sz = fs::file_size(p, ec2);
                nlohmann::json e;
                e["path"] = media::path::path_to_utf8(p);
                e["ext"]  = narrow_ext_lower(p);
                append_probe_metadata(p, e);
                e["size"] = ec2 ? 0 : static_cast<std::uint64_t>(sz);
                files.push_back(std::move(e));
            }
            PerFileResult agg;
            agg.path = "<aggregate>";
            agg.ok   = true;
            agg.result = nlohmann::json{
                {"count",         (int)files.size()},
                {"files",         std::move(files)},
                {"scanned_inputs",scanned_inputs},
                {"recursive",     recursive},
                {"include_raw",   include_raw},
                {"max_results",   max_results},
                {"interrupted",   "Ctrl+C (partial list)"},
            };
            std::vector<PerFileResult> pout; pout.push_back(std::move(agg));
            return build_envelope("list_images", std::move(pout));
        }
        ++scanned_inputs;
        const std::string spec_r = resolve_tool_path_string(spec);
        std::error_code ec;
        if (spec_r.find('*') != std::string::npos || spec_r.find('?') != std::string::npos) {
            std::string err;
            auto expanded = media::expand_input_paths(spec_r, err);
            for (const auto& s : expanded) {
                const std::string s_abs = resolve_tool_path_string(s);
                if (!collect_images_from(media::path::path_from_utf8(s_abs), recursive, include_raw, hits, max_results, policy_error))
                    return fatal("list_images: " + policy_error);
                if (max_results > 0 && hits.size() >= max_results) break;
            }
        } else {
            if (!collect_images_from(media::path::path_from_utf8(spec_r), recursive, include_raw, hits, max_results, policy_error))
                return fatal("list_images: " + policy_error);
        }
        if (max_results > 0 && hits.size() >= max_results) break;
    }

    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());

    nlohmann::json files = nlohmann::json::array();
    for (const auto& p : hits) {
        std::error_code ec;
        const auto sz = fs::file_size(p, ec);
        nlohmann::json e;
        e["path"] = media::path::path_to_utf8(p);
        e["ext"]  = narrow_ext_lower(p);
        e["size"] = ec ? 0 : static_cast<std::uint64_t>(sz);
        append_probe_metadata(p, e);
        files.push_back(std::move(e));
    }

    PerFileResult agg;
    agg.path = "<aggregate>";
    agg.ok = true;
    agg.result = nlohmann::json{
        {"count",         (int)files.size()},
        {"files",         std::move(files)},
        {"scanned_inputs",scanned_inputs},
        {"recursive",     recursive},
        {"include_raw",   include_raw},
        {"max_results",   max_results},
    };
    std::vector<PerFileResult> out; out.push_back(std::move(agg));
    return build_envelope("list_images", std::move(out));
}

ExecuteResult do_file_glob(const nlohmann::json& args) {
    if (!args.contains("pattern") || !args["pattern"].is_string())
        return fatal("file_glob: 'pattern' (string) is required");
    std::string pattern = args["pattern"].get<std::string>();
    while (!pattern.empty() && std::isspace(static_cast<unsigned char>(pattern.front())))
        pattern.erase(0, 1);
    while (!pattern.empty() && std::isspace(static_cast<unsigned char>(pattern.back())))
        pattern.pop_back();
    if (pattern.empty()) return fatal("file_glob: pattern is empty");

    const nlohmann::json& opts = options_or_empty(args);
    int max_results = opts.value("max_results", 500);
    if (max_results < 1) max_results = 1;
    if (max_results > 5000) max_results = 5000;
    const bool skip_dev        = opts.value("skip_dev_folders", true);
    const bool recursive_dir   = opts.value("recursive", true);

    const std::string resolved = resolve_tool_path_string(pattern);
    pm::log::info_lazy("llm_path_tools", [&pattern, &resolved]() -> std::string {
        return std::string("file_glob pattern_resolved: ") + pattern + " -> "
            + media::path::path_to_generic_utf8(media::path::path_from_utf8(resolved));
    });
    const bool has_glob = media::has_glob_tokens(resolved) || resolved.find("**") != std::string::npos;

    int          skipped_dev = 0, skipped_policy = 0, skipped_ext = 0;
    std::string  policy_error;
    nlohmann::json files     = nlohmann::json::array();
    bool         truncated   = false;

    if (!has_glob) {
        std::error_code ec;
        fs::path p = media::path::path_from_utf8(resolved).lexically_normal();
        fs::path abs = fs::absolute(p, ec);
        if (ec) return fatal("file_glob: could not resolve path");
        fs::path canon = fs::weakly_canonical(abs, ec);
        if (!ec && !canon.empty()) abs = std::move(canon);
        if (const std::string w = llm_path_fs_deny(media::path::path_to_utf8(abs)); !w.empty())
            return fatal(std::string("file_glob: ") + w);
        if (fs::is_directory(abs, ec)) {
            if (!collect_files_file_glob(abs, recursive_dir, max_results, skip_dev, files, skipped_dev, skipped_policy, policy_error))
                return fatal("file_glob: " + policy_error);
        } else if (fs::is_regular_file(abs, ec)) {
            if (max_results > 0
                && !file_glob_append_one_file(abs, skip_dev, files, skipped_dev, skipped_policy, policy_error))
                return fatal("file_glob: " + policy_error);
        } else {
            return fatal("file_glob: not a file or directory: " + resolved);
        }
    } else {
        FileGlobQuery query;
        std::string gerr;
        if (!build_file_glob_query(resolved, recursive_dir, query, gerr))
            return fatal(std::string("file_glob: ") + gerr);
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_INFO, "pm-chat", "file_glob root=%s pat=%s recursive=%d",
                            media::path::path_to_utf8(query.root).c_str(), query.name_pat.c_str(),
                            query.recursive ? 1 : 0);
#endif
        constexpr int k_max_dirs = 4000;
        int visited = 0;
        bool cancelled = false;
        const auto walk = [&](auto&& self, const fs::path& dir) -> bool {
            if (media::cli::cancel_requested()) {
                cancelled = true;
                return false;
            }
            if (++visited > k_max_dirs) {
                truncated = true;
                return true;
            }
            std::error_code ec;
            const auto opts = fs::directory_options::skip_permission_denied;
            for (fs::directory_iterator it(dir, opts, ec), end; it != end; it.increment(ec)) {
                if (media::cli::cancel_requested()) {
                    cancelled = true;
                    return false;
                }
                if (ec) {
                    ec.clear();
                    continue;
                }
                const fs::directory_entry& ent = *it;
                std::error_code iec;
                if (ent.is_symlink(iec))
                    continue;
                if (ent.is_directory(iec)) {
                    if (skip_dev && media::llm::glob_exclude_default_path_component(
                                        media::path::path_to_utf8(ent.path().filename())))
                        continue;
                    if (query.recursive && !self(self, ent.path()))
                        return false;
                    if (truncated || static_cast<int>(files.size()) >= max_results)
                        return true;
                    continue;
                }
                if (!ent.is_regular_file(iec))
                    continue;
                const std::string name = media::path::path_to_utf8(ent.path().filename());
                if (!glob_star_match(name, query.name_pat))
                    continue;
                if (!file_glob_append_one_file(ent.path(), skip_dev, files, skipped_dev, skipped_policy,
                                               policy_error))
                    return false;
                if (static_cast<int>(files.size()) >= max_results)
                    return true;
            }
            return true;
        };
        if (!walk(walk, query.root)) {
            if (cancelled || media::cli::cancel_requested())
                return fatal("file_glob: interrupted");
            return fatal(std::string("file_glob: ") +
                         (policy_error.empty() ? std::string("scan failed") : policy_error));
        }
    }

    PerFileResult agg;
    agg.path = "<aggregate>";
    agg.ok   = true;
    agg.result = nlohmann::json{
        {"count",               static_cast<int>(files.size())},
        {"files",               std::move(files)},
        {"pattern_resolved",    resolved},
        {"max_results",         max_results},
        {"recursive",           recursive_dir},
        {"skip_dev_folders",    skip_dev},
        {"skipped_dev_folders", skipped_dev},
        {"skipped_policy",      skipped_policy},
        {"skipped_extension",   skipped_ext},
        {"truncated",           truncated},
    };
    std::vector<PerFileResult> out;
    out.push_back(std::move(agg));
    return build_envelope("file_glob", std::move(out));
}

ExecuteResult do_find(const nlohmann::json& args) {
    auto inputs = get_string_array(args, "inputs");
    if (inputs.empty()) return fatal("image_find: 'inputs' (string array) is required and non-empty");
    for (auto& in : inputs)
        in = resolve_tool_path_string(in);
    for (const auto& in : inputs) {
        if (in.find('*') != std::string::npos || in.find('?') != std::string::npos) continue;
        if (const std::string w = llm_path_fs_deny(in); !w.empty())
            return fatal(std::string("image_find: ") + w);
    }

    const nlohmann::json& tool_options = options_or_empty(args);
    media::FindOptions     opt;
    media::apply_find_options_from_json(tool_options, opt);
    if (!tool_options.contains("mode") || tool_options["mode"].is_null()
        || (tool_options["mode"].is_string() && tool_options["mode"].get<std::string>().empty()))
        opt.mode = media::FindMode::Llm;
    if (opt.mode == media::FindMode::Llm) {
        opt.find_semantic_judge = false;
        opt.generate = true;
        opt.bypass_cache = false;
        opt.meta.resize_first = true;
        opt.meta.resize_width  = 512;
    }
    opt.meta.provider.clear();
    opt.meta.model.clear();
    opt.meta.api_key.clear();
    opt.meta.base_url.clear();
    apply_chat_image_recognition_defaults_for_tool_options(
        opt.meta.provider, opt.meta.model, nlohmann::json::object());
    const bool prefill_find_provider = opt.mode == media::FindMode::Llm && !opt.dry_run
        && (opt.find_semantic_judge || !opt.reference_images.empty() || opt.generate);
    if (prefill_find_provider) {
        ensure_api_key_for_image_provider(opt.meta.provider, opt.meta.api_key);
        ensure_base_url_for_image_provider(opt.meta.provider, opt.meta.base_url);
    }

    auto fr = media::find_images(inputs, opt);
    if (!fr.ok) return fatal(std::string("image_find: ") + fr.error);

    nlohmann::json matches = nlohmann::json::array();
    for (const auto& m : fr.matches) {
        if (const std::string w = llm_path_fs_deny(m.path); !w.empty()) {
            if (pm::llm::LLM_FS_GUARD_STRICT)
                return fatal("image_find: " + w);
            continue;
        }
        matches.push_back({{"path", m.path}, {"score", m.score},
                           {"source", m.source}, {"reason", m.reason}});
    }

    PerFileResult agg;
    agg.path = "<aggregate>";
    agg.ok = true;
    agg.result = nlohmann::json{
        {"matches",    std::move(matches)},
        {"scanned",    fr.scanned},
        {"considered", fr.considered},
        {"generated",  fr.generated},
        {"cache_hits", fr.cache_hits},
    };
    std::vector<PerFileResult> out; out.push_back(std::move(agg));
    return build_envelope("image_find", std::move(out));
}

namespace {

std::vector<std::string> file_search_preset_inputs()
{
    media::search::presets::Options options;
    options.policy = media::search::presets::InputPolicy::FileSearch;
    options.reject_dist_win_x64 = true;
    options.install_dir_utf8 = media::search::presets::normalize_path_utf8(
        media::path::path_to_utf8(media::app::exe_parent_directory()));

    const std::string cwd = agent_path_base_empty() ? std::string{} : agent_path_base();
    const media::search::presets::Resolved resolved =
        media::search::presets::resolve_from_settings(cwd, options, nullptr);

    std::vector<std::string> inputs;
    inputs.reserve(resolved.inputs.size());
    for (const std::string& path : resolved.inputs) {
        if (path.empty())
            continue;
        if (const std::string deny = llm_path_fs_deny(path); !deny.empty()) {
            pm::log::info_lazy("llm_path_tools", [&path, &deny]() -> std::string {
                return "file_search preset skip " + path + ": " + deny;
            });
            continue;
        }
        inputs.push_back(path);
    }
    pm::log::info_lazy("llm_path_tools", [&cwd, &inputs]() -> std::string {
        std::string line = "file_search preset cwd=";
        line += cwd.empty() ? "(none)" : cwd;
        line += " inputs=" + std::to_string(inputs.size());
        for (const std::string& path : inputs) {
            line += " ";
            line += path;
        }
        return line;
    });
    return inputs;
}

} // namespace

// Tool-only bounds. CLI and the UI keep an unlimited scan.
constexpr int k_file_search_context_lines = 8;
constexpr std::uint32_t k_file_search_hits_per_file = 40;
constexpr std::uint64_t k_file_search_max_file_bytes = 8ull * 1024ull * 1024ull;

ExecuteResult do_file_search(const nlohmann::json& args) {
    // query: prefer "query", fall back to "pattern" for backward compat
    std::string query;
    if (args.contains("query") && args["query"].is_string())
        query = args["query"].get<std::string>();
    else if (args.contains("pattern") && args["pattern"].is_string())
        query = args["pattern"].get<std::string>();
    if (query.empty())
        return fatal("file_search: 'query' (string) is required and non-empty");

    // inputs: prefer "inputs" array, fall back to "path" string.
    // When both are omitted, FileSearch preset: current directory, then bookmarks.
    std::vector<std::string> inputs;
    if (args.contains("inputs") && args["inputs"].is_array()) {
        for (const auto& v : args["inputs"]) {
            if (!v.is_string())
                continue;
            std::string path = resolve_tool_path_string(v.get<std::string>());
            if (!path.empty())
                inputs.push_back(std::move(path));
        }
    }
    if (inputs.empty() && args.contains("path") && args["path"].is_string()) {
        const std::string r = args["path"].get<std::string>();
        if (!r.empty()) inputs.push_back(resolve_tool_path_string(r));
    }
    if (inputs.empty())
        inputs = file_search_preset_inputs();
    if (inputs.empty()) {
        if (!agent_path_base_empty()) {
            if (const std::string w = llm_path_fs_deny(agent_path_base()); !w.empty())
                return fatal("file_search: " + w);
            return fatal("file_search: no searchable directories");
        }
        if (const auto fb = media::ui_defaults::fallback_work_folder(); !fb.empty())
            inputs.push_back(media::path::path_to_utf8(fb));
        else
            inputs.push_back(".");
    }

    for (const auto& p : inputs) {
        if (p.find('*') != std::string::npos || p.find('?') != std::string::npos) continue;
        if (const std::string w = llm_path_fs_deny(p); !w.empty())
            return fatal("file_search: " + w);
    }

    media::search::Request req;
    req.query   = query;
    req.inputs  = inputs;
    req.backend = media::search::Backend::Exact;
    req.content = media::search::Content::Any;

    // mode: "mode" param wins, legacy "grep" bool as fallback
    if (args.contains("mode") && args["mode"].is_string()) {
        media::search::Mode m = media::search::Mode::Grep;
        media::search::parse_mode(args["mode"].get<std::string>(), m);
        req.mode = m;
    } else if (args.contains("grep") && args["grep"].is_boolean() && !args["grep"].get<bool>()) {
        req.mode = media::search::Mode::Name;
    } else {
        req.mode = media::search::Mode::Grep;
    }
    // Filename edit-distance runs only for name mode, and only after a literal
    // miss. Grep and semantic search stay off this path.
    if (req.mode == media::search::Mode::Name) {
        req.fuzzy_names = true;
        if (args.contains("fuzzy") && args["fuzzy"].is_boolean())
            req.fuzzy_names = args["fuzzy"].get<bool>();
    }

    // indexer
    if (args.contains("indexer") && args["indexer"].is_string())
        req.exact_indexer = args["indexer"].get<std::string>();

    // search options
    req.recursive      = true;
    req.case_sensitive = false;
    req.head_limit     = 250;
    req.cancel_requested = [] { return media::cli::cancel_requested(); };

    if (args.contains("-i") && args["-i"].is_boolean())
        req.case_sensitive = !args["-i"].get<bool>();
    if (args.contains("case_sensitive") && args["case_sensitive"].is_boolean())
        req.case_sensitive = args["case_sensitive"].get<bool>();
    if (args.contains("regex") && args["regex"].is_boolean())
        req.regex = args["regex"].get<bool>();
    if (args.contains("whole_word") && args["whole_word"].is_boolean())
        req.whole_word = args["whole_word"].get<bool>();
    if (args.contains("multiline") && args["multiline"].is_boolean())
        req.multiline = args["multiline"].get<bool>();
    if (args.contains("head_limit") && args["head_limit"].is_number_integer())
        req.head_limit = args["head_limit"].get<int>();
    if (args.contains("offset") && args["offset"].is_number_integer())
        req.offset = args["offset"].get<int>();

    // context lines
    if (args.contains("-C") && args["-C"].is_number_integer()) {
        const int c = args["-C"].get<int>();
        req.context_before = c; req.context_after = c;
    }
    if (args.contains("-B") && args["-B"].is_number_integer())
        req.context_before = args["-B"].get<int>();
    if (args.contains("-A") && args["-A"].is_number_integer())
        req.context_after = args["-A"].get<int>();

    // glob filter — comma/space-separated, braced patterns kept whole
    if (args.contains("glob") && args["glob"].is_string()) {
        const std::string g = args["glob"].get<std::string>();
        std::istringstream iss(g);
        std::string tok;
        while (iss >> tok) {
            if (tok.find('{') != std::string::npos && tok.find('}') != std::string::npos) {
                req.include_globs.push_back(tok);
            } else {
                std::string part;
                for (char ch : tok + ",") {
                    if (ch == ',') {
                        if (!part.empty()) { req.include_globs.push_back(part); part.clear(); }
                    } else part += ch;
                }
            }
        }
    }
    if (args.contains("type") && args["type"].is_string())
        req.type_filter = args["type"].get<std::string>();

    // output mode
    if (args.contains("output_mode") && args["output_mode"].is_string())
        req.output_mode = args["output_mode"].get<std::string>();

    req.exclude_dirs = media::default_search_exclude_dirs();

    // Content search is the path that reads whole trees. Cap the bytes, the
    // hits inside one file, and the context window. head_limit 0 stays an
    // unlimited hit count; ripgrep stops only when that count is positive,
    // and it reads req.limit rather than head_limit.
    if (req.mode == media::search::Mode::Grep) {
        auto clamp_ctx = [](int n) {
            if (n < 0)
                return 0;
            return n > k_file_search_context_lines ? k_file_search_context_lines : n;
        };
        req.max_file_size_bytes = k_file_search_max_file_bytes;
        req.max_matches_per_file = k_file_search_hits_per_file;
        req.context_lines = clamp_ctx(req.context_lines);
        req.context_before = clamp_ctx(req.context_before);
        req.context_after = clamp_ctx(req.context_after);
        if (req.head_limit > 0)
            req.limit = req.head_limit;
    }

    media::cli::yield_to_interrupt();
    if (media::cli::cancel_requested()) {
        PerFileResult r; r.path = inputs[0]; r.ok = false; r.error = "interrupted";
        std::vector<PerFileResult> one; one.push_back(std::move(r));
        return build_envelope("file_search", std::move(one));
    }

    const media::search::Result sr = media::search::unified_search(req);

    if (!sr.ok)
        return fatal("file_search: " + sr.error);

    nlohmann::json result = nlohmann::json::object();
    result["mode"] = req.output_mode;
    result["inputs"] = req.inputs;

    if (req.output_mode == "files_with_matches") {
        // deduplicate paths preserving first-seen order
        nlohmann::json fn_arr = nlohmann::json::array();
        std::vector<std::string> seen_paths;
        for (const auto& h : sr.hits) {
            if (std::find(seen_paths.begin(), seen_paths.end(), h.path) == seen_paths.end()) {
                seen_paths.push_back(h.path);
                fn_arr.push_back(h.path);
            }
        }
        result["filenames"] = std::move(fn_arr);
        result["num_files"] = static_cast<int>(seen_paths.size());
    } else {
        nlohmann::json hits_arr = nlohmann::json::array();
        for (const auto& h : sr.hits) {
            nlohmann::json hj;
            hj["path"] = h.path;
            if (h.source == "fuzzy")
                hj["score"] = h.score;
            if (h.line > 0)    hj["line"] = h.line;
            if (h.column > 0)  hj["col"]  = h.column;
            if (!h.snippet.empty()) hj["text"] = h.snippet;
            if (h.metadata.contains("context_before"))
                hj["context_before"] = h.metadata["context_before"];
            if (h.metadata.contains("context_after"))
                hj["context_after"] = h.metadata["context_after"];
            hits_arr.push_back(std::move(hj));
        }
        result["hits"]      = std::move(hits_arr);
        result["num_hits"]  = static_cast<int>(sr.hits.size());
    }

    if (!sr.stats.empty()) result["stats"] = sr.stats;

    PerFileResult agg;
    agg.path   = "<aggregate>";
    agg.ok     = true;
    agg.result = std::move(result);
    std::vector<PerFileResult> out;
    out.push_back(std::move(agg));
    return build_envelope("file_search", std::move(out));
}

ExecuteResult do_file_read_path(const nlohmann::json& args) {
    if (!args.contains("path") || !args["path"].is_string())
        return fatal("file_read: 'path' (string) is required");
    const std::string raw = args["path"].get<std::string>();
    const std::string abs = resolve_tool_path_string(raw);
    pm::log::info_lazy("llm_path_tools", [&raw, &abs]() -> std::string {
        return std::string("file_read path_resolved: ") + raw + " -> " + media::path::path_to_generic_utf8(media::path::path_from_utf8(abs));
    });

    int offset = 1;
    int limit = k_file_read_max_lines;
    const bool windowed = args.contains("offset") || args.contains("limit");
    if (json_get_int(args, "offset", offset) && offset < 1)
        return fatal("file_read: offset must be >= 1");
    if (json_get_int(args, "limit", limit)) {
        if (limit < 1)
            return fatal("file_read: limit must be >= 1");
        if (limit > k_file_read_max_lines)
            limit = k_file_read_max_lines;
    }

    if (const std::string w = llm_path_fs_deny(abs); !w.empty())
        return fatal("file_read: " + w);
    if (const std::string blocked = indexed_read_block(abs); !blocked.empty())
        return fatal(blocked);
    if (const std::string blocked = converted_part_read_block(abs); !blocked.empty())
        return fatal(blocked);

    std::error_code ec;
    const fs::path p = abs;
    if (!fs::exists(p, ec))
        return fatal("file_read: path does not exist");
    if (!fs::is_regular_file(p, ec))
        return fatal("file_read: not a regular file");

    const nlohmann::json& opts = options_or_empty(args);
    bool as_markdown = false;
    if (opts.contains("markdown")) {
        if (!opts["markdown"].is_boolean())
            return fatal("file_read: options.markdown must be a boolean");
        as_markdown = opts["markdown"].get<bool>();
    }
    std::string format_name;
    if (opts.contains("format") && opts["format"].is_string())
        format_name = opts["format"].get<std::string>();

    if (as_markdown) {
        if (!ext_is_markdown_document(p))
            return fatal("file_read: options.markdown applies to pdf, doc, docx, ppt, pptx, "
                         "xls, xlsx, odt, ods, odp, rtf, epub, csv");
    } else if (ext_blocked_text_tool(p)) {
        return fatal("file_read: refusing binary or image extension (use image_* for images)");
    }

    media::cli::yield_to_interrupt();
    if (media::cli::cancel_requested())
        return fatal("file_read: interrupted (Ctrl+C)");

    std::string buf;
    std::string err;
    if (as_markdown) {
        const auto sz = fs::file_size(p, ec);
        if (ec)
            return fatal("file_read: could not read file size: " + ec.message());
        if (sz > k_file_read_max_file_bytes)
            return fatal("file_read: file exceeds 16 MiB limit");
#if defined(FEATURE_COMMAND_MARKDOWN) && FEATURE_COMMAND_MARKDOWN
        const auto format = media::markdown::format_from_name(format_name);
        if (!format)
            return fatal("file_read: unknown format: " + format_name);
        const media::markdown::Result converted = media::markdown::to_markdown(p, *format);
        if (!converted.ok()) {
            const std::string why = converted.error.empty() ? "conversion failed" : converted.error;
            return fatal("file_read: " + why);
        }
        buf = converted.text;
#else
        (void)format_name;
        return fatal("file_read: markdown conversion is not available in this build");
#endif
    } else {
        if (!read_file_bytes(p, buf, err))
            return fatal("file_read: " + err);
        if (buffer_looks_binary_text(buf))
            return fatal("file_read: refusing binary content");
    }

    std::vector<LineSpan> spans;
    collect_line_spans(buf, spans);
    const int total_lines = static_cast<int>(spans.size());
    if (!windowed
        && (total_lines > k_file_read_max_lines
            || buf.size() > pm::llm::k_file_read_large_bytes)) {
        if (as_markdown)
            return fatal("file_read: converted markdown is large (" + std::to_string(total_lines)
                         + " lines); call file_read again with offset and limit");
        return fatal("file_read: large file (" + std::to_string(total_lines)
                     + " lines); use file_search to locate the target, then call "
                       "file_read with offset/limit");
    }
    if (total_lines == 0) {
        PerFileResult r;
        r.path = abs;
        r.ok = true;
        r.result = nlohmann::json{
            {"path", media::path::path_to_generic_utf8(media::path::path_from_utf8(abs))},
            {"text", ""},
            {"line_start", 1},
            {"line_end", 0},
            {"total_lines", 0},
            {"bytes", 0},
            {"truncated", false},
            {"version", media::llm::change::version_token(
                media::llm::change::content_sha256_hex(buf))},
        };
        if (as_markdown)
            r.result["markdown"] = true;
        std::vector<PerFileResult> one;
        one.push_back(std::move(r));
        return build_envelope("file_read", std::move(one));
    }
    if (offset > total_lines)
        return fatal("file_read: offset " + std::to_string(offset)
                     + " past end (" + std::to_string(total_lines) + " lines)");

    const int line_start = offset;
    int line_end = std::min(total_lines, offset + limit - 1);
    const std::size_t from = spans[static_cast<std::size_t>(line_start - 1)].begin;
    std::size_t to = spans[static_cast<std::size_t>(line_end - 1)].end;
    if (to < buf.size()) {
        if (buf[to] == '\r' && to + 1 < buf.size() && buf[to + 1] == '\n')
            to += 2;
        else if (buf[to] == '\n' || buf[to] == '\r')
            ++to;
    }
    while (line_end > line_start
           && (to - from) > k_file_read_window_bytes) {
        --line_end;
        to = spans[static_cast<std::size_t>(line_end - 1)].end;
        if (to < buf.size()) {
            if (buf[to] == '\r' && to + 1 < buf.size() && buf[to + 1] == '\n')
                to += 2;
            else if (buf[to] == '\n' || buf[to] == '\r')
                ++to;
        }
    }
    std::string text = to_lf(buf.substr(from, to - from));
    if (text.size() > k_file_read_window_bytes)
        text.resize(k_file_read_window_bytes);
    const bool truncated = line_end < total_lines
        || (to - from) > k_file_read_window_bytes
        || text.size() == k_file_read_window_bytes;

    PerFileResult r;
    r.path = abs;
    r.ok = true;
    r.result = nlohmann::json{
        {"path", media::path::path_to_generic_utf8(media::path::path_from_utf8(abs))},
        {"text", text},
        {"line_start", line_start},
        {"line_end", line_end},
        {"total_lines", total_lines},
        {"bytes", static_cast<std::uint64_t>(text.size())},
        {"truncated", truncated},
        {"version", media::llm::change::version_token(
            media::llm::change::content_sha256_hex(buf))},
    };
    if (as_markdown)
        r.result["markdown"] = true;
    std::vector<PerFileResult> one;
    one.push_back(std::move(r));
    return build_envelope("file_read", std::move(one));
}

ExecuteResult do_write_file(const nlohmann::json& args) {
    if (!args.contains("path") || !args["path"].is_string())
        return fatal("write_file: 'path' (string) is required");
    if (!args.contains("content") || !args["content"].is_string())
        return fatal("write_file: 'content' (string) is required");
    const std::string   path    = args["path"].get<std::string>();
    const std::string&  content = args["content"].get_ref<const std::string&>();
    if (content.size() > k_write_file_max_bytes)
        return fatal("write_file: content exceeds 16 MiB limit");

    const nlohmann::json& o     = options_or_empty(args);
    const bool            append = o.value("append", false);
    const std::string     out_abs = resolve_tool_path_string(path);
    pm::log::info_lazy("llm_path_tools", [&path, &out_abs]() -> std::string {
        return std::string("write_file path_resolved: ") + path + " -> " + media::path::path_to_generic_utf8(media::path::path_from_utf8(out_abs));
    });
    if (const std::string w = llm_path_fs_deny(out_abs); !w.empty())
        return fatal(std::string("write_file: ") + w);
    if (const std::string wW = llm_path_write_deny(out_abs); !wW.empty())
        return fatal(std::string("write_file: ") + wW);

    media::cli::yield_to_interrupt();
    if (media::cli::cancel_requested()) {
        PerFileResult r;
        r.path  = out_abs;
        r.ok    = false;
        r.error = "interrupted (Ctrl+C)";
        std::vector<PerFileResult> one;
        one.push_back(std::move(r));
        return build_envelope("write_file", std::move(one));
    }

    std::error_code ec;
    const fs::path  p      = out_abs;
    const fs::path  parent = p.parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);

    std::string before;
    if (fs::is_regular_file(p, ec)) {
        std::string read_err;
        (void)read_file_bytes(p, before, read_err);
    }
    ExecuteResult stale;
    if (stale_version("write_file", before, args, stale))
        return stale;

    const std::ios::openmode mode =
        (append ? (std::ios::app) : (std::ios::trunc)) | std::ios::binary;
    std::ofstream ofs(p, std::ios::out | mode);
    if (!ofs) return fatal("write_file: cannot open for writing: " + out_abs);
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!ofs.good()) return fatal("write_file: write failed: " + out_abs);

    const std::string after = append ? (before + content) : content;

    PerFileResult r;
    r.path         = out_abs;
    r.ok           = true;
    r.output_path  = out_abs;
    r.result       = nlohmann::json{
        {"bytes_written", static_cast<std::uint64_t>(content.size())},
        {"append",        append},
    };
    attach_history(r.result, "write_file", p, before, after);
    std::vector<PerFileResult> out;
    out.push_back(std::move(r));
    return build_envelope("write_file", std::move(out));
}

ExecuteResult do_file_str_replace(const nlohmann::json& args) {
    if (!args.contains("path") || !args["path"].is_string())
        return fatal("file_str_replace: 'path' (string) is required");
    if (!args.contains("old_string") || !args["old_string"].is_string())
        return fatal("file_str_replace: 'old_string' (string) is required");
    if (!args.contains("new_string") || !args["new_string"].is_string())
        return fatal("file_str_replace: 'new_string' (string) is required");

    const std::string raw_path = args["path"].get<std::string>();
    std::string old_string = args["old_string"].get<std::string>();
    std::string new_string = args["new_string"].get<std::string>();
    if (old_string.empty())
        return fatal("file_str_replace: old_string is empty");
    if (old_string == new_string)
        return fatal("file_str_replace: old_string and new_string are identical");

    const bool replace_all = args.contains("replace_all") && args["replace_all"].is_boolean()
        && args["replace_all"].get<bool>();
    int start_line = 0;
    int end_line = 0;
    const bool has_start = json_get_int(args, "start_line", start_line);
    const bool has_end = json_get_int(args, "end_line", end_line);
    if (has_start != has_end)
        return fatal("file_str_replace: start_line and end_line must be passed together");
    if (has_start && (start_line < 1 || end_line < start_line))
        return fatal("file_str_replace: invalid start_line/end_line window");

    const std::string abs = resolve_tool_path_string(raw_path);
    pm::log::info_lazy("llm_path_tools", [&raw_path, &abs]() -> std::string {
        return std::string("file_str_replace path_resolved: ") + raw_path + " -> "
            + media::path::path_to_generic_utf8(media::path::path_from_utf8(abs));
    });
    if (const std::string w = llm_path_fs_deny(abs); !w.empty())
        return fatal(std::string("file_str_replace: ") + w);
    if (const std::string wW = llm_path_write_deny(abs); !wW.empty())
        return fatal(std::string("file_str_replace: ") + wW);

    std::error_code ec;
    const fs::path p = abs;
    if (!fs::exists(p, ec))
        return fatal("file_str_replace: path does not exist");
    if (!fs::is_regular_file(p, ec))
        return fatal("file_str_replace: not a regular file");
    if (ext_blocked_text_tool(p))
        return fatal("file_str_replace: refusing binary or image extension");

    media::cli::yield_to_interrupt();
    if (media::cli::cancel_requested())
        return fatal("file_str_replace: interrupted (Ctrl+C)");

    std::string buf;
    std::string err;
    if (!read_file_bytes(p, buf, err))
        return fatal("file_str_replace: " + err);
    if (buffer_looks_binary_text(buf))
        return fatal("file_str_replace: refusing binary content");

    ExecuteResult stale;
    if (stale_version("file_str_replace", buf, args, stale))
        return stale;

    const bool crlf = buf.find("\r\n") != std::string::npos;
    old_string = apply_file_eol(old_string, crlf);
    new_string = apply_file_eol(new_string, crlf);

    std::size_t search_from = 0;
    std::size_t search_to = buf.size();
    if (has_start) {
        std::vector<LineSpan> spans;
        collect_line_spans(buf, spans);
        const int total_lines = static_cast<int>(spans.size());
        if (start_line > total_lines || end_line > total_lines)
            return fatal("file_str_replace: start_line/end_line past end ("
                         + std::to_string(total_lines) + " lines)");
        search_from = spans[static_cast<std::size_t>(start_line - 1)].begin;
        search_to = spans[static_cast<std::size_t>(end_line - 1)].end;
        if (search_to < buf.size()) {
            if (buf[search_to] == '\r' && search_to + 1 < buf.size() && buf[search_to + 1] == '\n')
                search_to += 2;
            else if (buf[search_to] == '\n' || buf[search_to] == '\r')
                ++search_to;
        }
    }

    const std::string hay = buf.substr(search_from, search_to - search_from);
    const auto rel_hits = find_all_nonoverlap(hay, old_string);
    if (rel_hits.empty())
        return fatal("file_str_replace: old_string not found");
    if (!replace_all && rel_hits.size() > 1) {
        std::string lines;
        const std::size_t show = std::min(rel_hits.size(), static_cast<std::size_t>(8));
        for (std::size_t i = 0; i < show; ++i) {
            if (i) lines += ", ";
            lines += std::to_string(line_number_at(buf, search_from + rel_hits[i]));
        }
        return fatal("file_str_replace: old_string matched "
                     + std::to_string(rel_hits.size()) + " times (lines " + lines
                     + "); widen old_string or set replace_all");
    }

    nlohmann::json line_hits = nlohmann::json::array();
    const std::size_t line_cap = std::min(rel_hits.size(), static_cast<std::size_t>(20));
    for (std::size_t i = 0; i < line_cap; ++i)
        line_hits.push_back(line_number_at(buf, search_from + rel_hits[i]));

    std::string replaced;
    replaced.reserve(buf.size() + new_string.size() * rel_hits.size());
    std::size_t cursor = 0;
    for (std::size_t rel : rel_hits) {
        const std::size_t abs_pos = search_from + rel;
        replaced.append(buf, cursor, abs_pos - cursor);
        replaced += new_string;
        cursor = abs_pos + old_string.size();
    }
    replaced.append(buf, cursor, buf.size() - cursor);

    if (replaced.size() > k_write_file_max_bytes)
        return fatal("file_str_replace: result exceeds 16 MiB limit");
    if (!atomic_write_utf8(p, replaced, err))
        return fatal("file_str_replace: " + err);

    PerFileResult r;
    r.path = abs;
    r.ok = true;
    r.output_path = abs;
    r.result = nlohmann::json{
        {"path", media::path::path_to_generic_utf8(media::path::path_from_utf8(abs))},
        {"replacements", static_cast<int>(rel_hits.size())},
        {"lines", std::move(line_hits)},
        {"bytes_written", static_cast<std::uint64_t>(replaced.size())},
    };
    attach_history(r.result, "file_str_replace", p, buf, replaced);
    std::vector<PerFileResult> out;
    out.push_back(std::move(r));
    return build_envelope("file_str_replace", std::move(out));
}

ExecuteResult do_file_delete(const nlohmann::json& args) {
    if (!args.contains("path") || !args["path"].is_string())
        return fatal("file_delete: 'path' (string) is required");
    const std::string raw = args["path"].get<std::string>();
    const std::string abs = resolve_tool_path_string(raw);
    pm::log::info_lazy("llm_path_tools", [&raw, &abs]() -> std::string {
        return std::string("file_delete path_resolved: ") + raw + " -> "
            + media::path::path_to_generic_utf8(media::path::path_from_utf8(abs));
    });
    if (const std::string w = llm_path_fs_deny(abs); !w.empty())
        return fatal(std::string("file_delete: ") + w);
    if (const std::string wW = llm_path_write_deny(abs); !wW.empty())
        return fatal(std::string("file_delete: ") + wW);

    std::error_code ec;
    const fs::path p = abs;
    if (!fs::exists(p, ec))
        return fatal("file_delete: path does not exist");
    if (fs::is_directory(p, ec))
        return fatal("file_delete: refusing directory; use run only if the user asked to remove a folder");
    if (!fs::is_regular_file(p, ec))
        return fatal("file_delete: not a regular file");

    media::cli::yield_to_interrupt();
    if (media::cli::cancel_requested())
        return fatal("file_delete: interrupted (Ctrl+C)");

    std::string before;
    std::string read_err;
    (void)read_file_bytes(p, before, read_err);
    ExecuteResult stale;
    if (stale_version("file_delete", before, args, stale))
        return stale;

    fs::remove(p, ec);
    if (ec)
        return fatal("file_delete: " + ec.message());

    PerFileResult r;
    r.path = abs;
    r.ok = true;
    r.result = nlohmann::json{
        {"path", media::path::path_to_generic_utf8(media::path::path_from_utf8(abs))},
        {"deleted", true},
    };
    attach_history(r.result, "file_delete", p, before, std::string{});
    std::vector<PerFileResult> out;
    out.push_back(std::move(r));
    return build_envelope("file_delete", std::move(out));
}

// ── Catalog schemas ───────────────────────────────────────────────────────

namespace {

nlohmann::json paths_array_schema() {
    return nlohmann::json{
        {"type", "array"},
        {"description", "Host file paths (absolute, or relative to the current Explorer folder "
                         "in-app; else process cwd). When the user says 'these files' without "
                         "naming any, the agent passes the current Explorer selection (or folder)."},
        {"items", {{"type", "string"}}},
        {"minItems", 1},
    };
}

} // namespace

ToolDef make_list_images() {
    return ToolDef{
        "list_images",
        "Enumerate image files inside one or more folders / globs. Use this "
        "BEFORE any other tool when the user asks 'what's in this folder' / "
        "'show me the photos here' / 'how many images do I have'. Filters by "
        "standard image extensions by default (jpg, jpeg, png, webp, tif, "
        "tiff, bmp, gif, avif, heic; plus arw, cr2, cr3, nef, dng, orf, rw2, "
        "raf, pef, nrw, srw, x3f, 3fr, mef, mrw when options.include_raw is "
        "true). Read-only — never writes to disk.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"inputs", {
                    {"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"description", "Folder paths or globs (e.g. 'C:\\\\photos\\\\trip', 'C:\\\\photos\\\\**\\\\*.jpg'). When the user is browsing a folder, pass that folder."},
                }},
                {"options", {
                    {"type", "object"},
                    {"properties", {
                        {"recursive",    {{"type", "boolean"}, {"description", "Recurse into subfolders (default: true)"}}},
                        {"include_raw",  {{"type", "boolean"}, {"description", "Also include camera RAW formats (default: true)"}}},
                        {"max_results",  {{"type", "integer"}, {"minimum", 0},  {"description", "Cap; 0 = unlimited (default: 0)"}}},
                    }},
                }},
            }},
            {"required", nlohmann::json::array({"inputs"})},
        }};
}

ToolDef make_file_glob() {
    return ToolDef{
        "file_glob",
        "List files matching a glob or directory (e.g. *.md, *.dxf, *.svg, *.csv, *.jpg). "
        "Returns path/size/ext metadata; can list binary and media files unlike file_read. "
        "Skips dev/build/cache folders by default (node_modules, .git, CMakeFiles, …). "
        "For fast name search across a large tree use file_search with mode=name and indexer=rg instead. "
        "Read-only; caps at options.max_results (default 500, max 5000).",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"pattern", {{"type", "string"},
                             {"description", "Glob or path: e.g. '*.md', 'src/**/*.txt', or a folder path "
                                             "without wildcards (see options.recursive)."}}},
                {"options", {
                    {"type", "object"},
                    {"properties", {
                        {"max_results", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000},
                            {"description", "Maximum files returned (default: 500)."}}},
                        {"recursive", {{"type", "boolean"},
                            {"description", "When `pattern` resolves to a directory and has no * ? **, "
                                            "recurse into subfolders (default: true)."}}},
                        {"skip_dev_folders", {{"type", "boolean"},
                            {"description", "Skip node_modules, .git, build outputs, IDE caches, … (default: true)."}}},
                    }},
                }},
            }},
            {"required", nlohmann::json::array({"pattern"})},
        }};
}

ToolDef make_file_read() {
    return ToolDef{
        "file_read",
        "Read a UTF-8 text file. For large files you MUST pass offset (1-based line) and "
        "limit (max 400 lines) — an unwindowed read of a large file fails. Locate the "
        "window first with file_search (indexer=rg, regex=true for headings like ^### grant). "
        "For a downloaded or converted document, or a folder of markdown parts, do not "
        "read each part: call search_index on that folder, then search_query, then "
        "search_detail for one passage. "
        "Returns line_start / line_end / total_lines / truncated / version (sha256 of the "
        "whole file, or of the converted markdown) so you can page. "
        "For PDF, Word, PowerPoint, Excel, OpenDocument, RTF, EPUB, or CSV, set "
        "options.markdown true. The source file must be at most 16 MiB. The returned "
        "window is still at most 400 lines and 96 KiB; a longer conversion needs offset "
        "and limit. options.format names the type when the bytes have no signature (csv). "
        "Binary and image extensions are refused (use image_*). "
        "Relative path resolves to the Explorer folder.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"},
                          {"description", "File path: absolute, or relative to the Explorer folder / "
                                          "selection base in-app."}}},
                {"offset", {
                    {"type", "integer"},
                    {"minimum", 1},
                    {"description",
                     "First line to return (1-based). Default 1. "
                     "Not the same as file_search.offset (hit pagination)."},
                }},
                {"limit", {
                    {"type", "integer"},
                    {"minimum", 1},
                    {"maximum", 400},
                    {"description",
                     "Max lines to return. Required together with offset on files over 400 lines "
                     "or 48 KiB. Default: remaining lines, capped at 400."},
                }},
                {"options", {
                    {"type", "object"},
                    {"properties", {
                        {"markdown", {
                            {"type", "boolean"},
                            {"description",
                             "Convert a PDF, Word, PowerPoint, Excel, OpenDocument, RTF, EPUB, or "
                             "CSV file to Markdown, then return that text. Default false. "
                             "Source file cap is 16 MiB. The line window still applies."},
                        }},
                        {"format", {
                            {"type", "string"},
                            {"description",
                             "Document type when options.markdown is true and the bytes have no "
                             "signature. Use csv. Omit to detect from the file."},
                        }},
                    }},
                }},
            }},
            {"required", nlohmann::json::array({"path"})},
        }};
}

nlohmann::json expected_version_schema() {
    return nlohmann::json{
        {"type", "string"},
        {"description",
         "Optional. Whole-file version from file_read (sha256:...). "
         "If the file changed since that read, the mutation fails — re-read the window."},
    };
}

ToolDef make_file_search() {
    return ToolDef{
        "file_search",
        "Search file contents (grep) or file names. "
        "Tip: pass indexer=rg to route through bundled ripgrep — use it whenever you would "
        "reach for `rg` on the command line; all the usual rg semantics apply "
        "(regex, whole_word=-w, type=-t, glob=-g, context=-B/-A/-C, mode=name maps to `rg --files -g`). "
        "mode=grep (default): full-text search returning structured hits; "
        "mode=name: filename search (rg --files). "
        "output_mode: content (hits with path/line/text) | files_with_matches (paths only). "
        "Defaults: recursive, case-insensitive, head_limit=250. "
        "Grep skips a file larger than 8MB, keeps at most 40 hits in one file, and at most 8 context lines. "
        "head_limit 0 removes the hit cap. Name search is not byte-capped. Read-only. "
        "When inputs and path are omitted, searches the current directory first "
        "(if any), then the user's searchable folder bookmarks, in that order. "
        "Markdown/code headings: indexer=rg, regex=true, query like \"^### grant\" or \"^## Commands\".",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"query", {
                    {"type", "string"},
                    {"description", "Pattern to search for. Literal by default (rg -F); set regex=true for regex (rg default). "
                                    "Alias: 'pattern'."},
                }},
                {"pattern", {
                    {"type", "string"},
                    {"description", "Alias for 'query' (backward compat)."},
                }},
                {"inputs", {
                    {"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"description",
                     "Files or directories to search (rg PATH …). "
                     "When omitted (and path is omitted): current directory first if one is set, "
                     "then searchable folder bookmarks, in that order."},
                }},
                {"path", {
                    {"type", "string"},
                    {"description", "Single file or directory (alias for inputs[0])."},
                }},
                {"mode", {
                    {"type", "string"},
                    {"enum", {"grep", "name"}},
                    {"description", "grep = full-text search (default, rg PATTERN); name = filename match. "
                                    "A literal name miss is scored by edit distance (casandra registoirs can hit cassandra-registers). "
                                    "A query that already contains * or ? stays a glob and does not fall through."},
                }},
                {"fuzzy", {
                    {"type", "boolean"},
                    {"description", "Name mode only. When true (default for mode=name), a literal filename miss is ranked by edit distance. "
                                    "Ignored for grep. Set false to keep the literal/glob pass only."},
                }},
                {"indexer", {
                    {"type", "string"},
                    {"enum", {"own", "rg"}},
                    {"description", "rg = bundled ripgrep (recommended for large trees, code search, and any search you'd normally run with rg). "
                                    "own = built-in walker (default)."},
                }},
                {"glob", {
                    {"type", "string"},
                    {"description", "Filename glob filter (rg -g). e.g. \"*.cpp\" or \"*.{ts,tsx}\". Space- or comma-separated."},
                }},
                {"type", {
                    {"type", "string"},
                    {"description", "File type shorthand (rg -t): cpp, c, cs, go, html, java, js, json, md, py, rs, sh, swift, ts, toml, xml, yaml, …"},
                }},
                {"output_mode", {
                    {"type", "string"},
                    {"enum", {"content", "files_with_matches"}},
                    {"description", "content = hits array with path/line/text (default); files_with_matches = deduplicated file paths (rg -l)."},
                }},
                {"regex", {{"type", "boolean"},
                           {"description", "Treat query as a regex (default: false = literal, rg -F). Set true for rg-style regex."}}},
                {"whole_word", {{"type", "boolean"},
                                {"description", "Match whole words only (rg -w). Default: false."}}},
                {"-i", {{"type", "boolean"},
                        {"description", "Case-insensitive (rg -i, default: true)."}}},
                {"multiline", {{"type", "boolean"},
                               {"description", "Multiline mode: . matches newlines (rg -U)."}}},
                {"-B", {{"type", "integer"}, {"minimum", 0},
                        {"description", "Context lines before each match (rg -B N)."}}},
                {"-A", {{"type", "integer"}, {"minimum", 0},
                        {"description", "Context lines after each match (rg -A N)."}}},
                {"-C", {{"type", "integer"}, {"minimum", 0},
                        {"description", "Symmetric context lines (rg -C N, overrides -B / -A)."}}},
                {"head_limit", {
                    {"type", "integer"}, {"minimum", 0},
                    {"description", "Cap on returned hits. Default 250; 0 = unlimited."},
                }},
                {"offset", {
                    {"type", "integer"}, {"minimum", 0},
                    {"description", "Skip first N hits (pagination). Default 0."},
                }},
            }},
            {"required", nlohmann::json::array({"query"})},
        }};
}

ToolDef make_write_file() {
    return ToolDef{
        "write_file",
        "Create, overwrite, or append a UTF-8 text file on disk (reports, notes, .md, "
        "CSV snippets). Use whenever the user asks to save, write, or export text to a "
        "file. Parent directories are created as needed. Relative paths resolve to the current "
        "Explorer folder in-app (same as other path tools; if no folder, the first selected file's "
        "directory; only then the process cwd). Do not claim a file was written without "
        "calling this tool first. For content-preserving rename / duplication of an existing "
        "file, use the `run` shell tool with the host's copy command instead of "
        "file_read + write_file (saves a content round-trip). "
        "To assemble a large file from existing files or line ranges, use file_compose. "
        "For an in-place edit of an existing file use file_str_replace, not a full rewrite.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {
                    {"type", "string"},
                    {"description", "Output path: absolute, or relative to the Explorer folder / selection base in-app (e.g. santa.md, sub/report.txt)."},
                }},
                {"content", {
                    {"type", "string"},
                    {"description", "Full file body in UTF-8. Use \"\\n\" for newlines in JSON string values."},
                }},
                {"options", {
                    {"type", "object"},
                    {"properties", {
                        {"append", {{"type", "boolean"},
                                    {"description", "If true, append to the file (creates the file if missing). Default false (truncate / create)."}}},
                    }},
                }},
                {"expected_version", expected_version_schema()},
            }},
            {"required", nlohmann::json::array({"path", "content"})},
        }};
}

ToolDef make_file_str_replace() {
    return ToolDef{
        "file_str_replace",
        "Surgical edit of an existing UTF-8 file. old_string must match once unless "
        "replace_all is true; on ambiguity the tool fails with line numbers — widen "
        "the snippet or pass start_line/end_line. Do not use this to create files or "
        "rewrite a whole large file (write_file). Locate the section with file_search "
        "(rg + regex for headings) then file_read(offset, limit).",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {
                    {"type", "string"},
                    {"description",
                     "Existing UTF-8 file to edit. Will not create a missing file (use write_file)."},
                }},
                {"old_string", {
                    {"type", "string"},
                    {"description",
                     "Exact text to find. Must be unique unless replace_all is true. "
                     "Include neighboring lines when the snippet repeats (e.g. **Example**)."},
                }},
                {"new_string", {
                    {"type", "string"},
                    {"description", "Replacement text. Empty string deletes the snippet."},
                }},
                {"replace_all", {
                    {"type", "boolean"},
                    {"default", false},
                    {"description", "Replace every match. Default false (require exactly one match)."},
                }},
                {"start_line", {
                    {"type", "integer"},
                    {"minimum", 1},
                    {"description", "Optional 1-based window start; must pair with end_line."},
                }},
                {"end_line", {
                    {"type", "integer"},
                    {"minimum", 1},
                    {"description", "Optional 1-based window end; must pair with start_line."},
                }},
                {"expected_version", expected_version_schema()},
            }},
            {"required", nlohmann::json::array({"path", "old_string", "new_string"})},
        }};
}

ToolDef make_file_delete() {
    return ToolDef{
        "file_delete",
        "Delete one regular file. Refuses directories, missing paths, and "
        "policy-denied locations. Not a recursive rm.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {
                    {"type", "string"},
                    {"description",
                     "Regular file to delete. Directories are refused; use run only if the user asked to remove a folder."},
                }},
                {"expected_version", expected_version_schema()},
            }},
            {"required", nlohmann::json::array({"path"})},
        }};
}

struct DownloadDest {
    fs::path    dir;
    std::string explicit_name;
};

DownloadDest resolve_download_dest(const nlohmann::json& args, std::string& error) {
    DownloadDest out;
    const std::string raw = (args.contains("path") && args["path"].is_string())
        ? args["path"].get<std::string>() : std::string{};
    if (!raw.empty()) {
        const std::string abs = resolve_tool_path_string(raw);
        if (abs.empty()) {
            error = "invalid path";
            return out;
        }
        const fs::path p(abs);
        std::error_code ec;
        const bool as_dir = raw.back() == '/' || raw.back() == '\\' || fs::is_directory(p, ec);
        if (as_dir) {
            out.dir = p;
        } else {
            out.dir = p.parent_path();
            out.explicit_name = media::path::path_to_utf8(p.filename());
            if (out.dir.empty() || out.explicit_name.empty()) {
                error = "path has no directory";
                out = {};
                return out;
            }
        }
#if defined(__ANDROID__)
    } else {
        // Phone: the agent folder is the app profile; keep downloads out of its root.
        out.dir = media::settings::get_config_dir() / "downloads";
#else
    } else if (!agent_path_base_empty()) {
        out.dir = media::path::path_from_utf8(agent_path_base());
    } else {
        std::error_code ec;
        out.dir = fs::current_path(ec);
        if (ec || out.dir.empty())
            out.dir = media::settings::get_config_dir() / "chrome-downloads";
#endif
    }

    if (const std::string denied = llm_path_fs_deny(media::path::path_to_utf8(out.dir)); !denied.empty()) {
        error = denied;
        out = {};
        return out;
    }
    if (!out.explicit_name.empty()) {
        const fs::path file = out.dir / media::path::path_from_utf8(out.explicit_name);
        if (const std::string denied = llm_path_write_deny(media::path::path_to_utf8(file)); !denied.empty()) {
            error = denied;
            out = {};
        }
    }
    return out;
}

ExecuteResult do_download_url(const nlohmann::json& args) {
    if (!args.contains("url") || !args["url"].is_string())
        return fatal("download_url: 'url' (http or https) is required");

    media::net::UrlDownloadRequest req;
    req.url = args["url"].get<std::string>();
    if (args.contains("filename") && args["filename"].is_string())
        req.filename = args["filename"].get<std::string>();

    std::string dest_error;
    const DownloadDest dest = resolve_download_dest(args, dest_error);
    if (!dest_error.empty())
        return fatal(std::string("download_url: ") + dest_error);
    const fs::path dir = dest.dir;
    req.directory = media::path::path_to_utf8(dir);
    if (!dest.explicit_name.empty()) {
        req.filename = dest.explicit_name;
        req.explicit_filename = true;
    }

    std::string guess = dest.explicit_name;
    if (guess.empty()) {
        const auto query = req.url.find('?');
        std::string path = query == std::string::npos ? req.url : req.url.substr(0, query);
        const auto slash = path.find_last_of('/');
        if (slash != std::string::npos)
            guess = path.substr(slash + 1);
    }
    if (guess.empty())
        guess = req.filename;
    guess = media::path::sanitize_filename(guess, "_");
    if (!guess.empty()) {
        const std::string denied = llm_path_write_deny(media::path::path_to_utf8(dir / media::path::path_from_utf8(guess)));
        if (!denied.empty())
            return fatal(std::string("download_url: ") + denied);
    }

    req.cancel_check = [] { return media::cli::cancel_requested(); };
    req.on_progress = [](std::uint64_t received, std::uint64_t total) {
        if (tls_download_progress)
            tls_download_progress(received, total);
    };

    const media::net::UrlDownloadResult saved = media::net::download_url_file(req);
    if (saved.cancelled) {
        ExecuteResult r;
        r.ok = false;
        r.error = "download_url: cancelled";
        r.envelope = nlohmann::json{
            {"ok", false},
            {"error", r.error},
            {"cancelled", true},
            {"bytes", saved.bytes},
        };
        return r;
    }
    if (!saved.ok) {
        const std::string why = saved.error.empty() ? "download failed" : saved.error;
        return fatal(std::string("download_url: ") + why);
    }
    if (const std::string denied = llm_path_write_deny(saved.path); !denied.empty()) {
        std::error_code rm;
        fs::remove(media::path::path_from_utf8(saved.path), rm);
        return fatal(std::string("download_url: ") + denied);
    }
    const media::probe::ProbeResult sniffed = media::probe::probe_file(media::path::path_from_utf8(saved.path));
    if (sniffed.ok && !sniffed.preferred_extension.empty()) {
        const fs::path sniffed_path = dir / media::path::path_from_utf8(std::string("download") + sniffed.preferred_extension);
        if (const std::string denied = llm_path_write_deny(media::path::path_to_utf8(sniffed_path));
            !denied.empty()) {
            std::error_code rm;
            fs::remove(media::path::path_from_utf8(saved.path), rm);
            return fatal(std::string("download_url: ") + denied);
        }
    }

    PerFileResult file;
    file.path = saved.path;
    file.ok = true;
    file.output_path = saved.path;
    file.result = nlohmann::json{
        {"bytes", saved.bytes},
        {"status", saved.status_code},
        {"content_type", saved.content_type},
        {"url", saved.effective_url.empty() ? req.url : saved.effective_url},
    };
    std::vector<PerFileResult> one;
    one.push_back(std::move(file));
    return build_envelope("download_url", std::move(one));
}

ToolDef make_download_url() {
    return ToolDef{
        "download_url",
        "Download one http(s) URL into the current folder, or into path when set. "
        "The current folder is the open Files folder, or the folder --src opened. "
        "path may be a directory, or a file path that sets the saved name. "
        "Relative paths resolve like write_file and are checked by the same write guard. "
        "Use a url from browser_read files[] when the user wants that image or file on disk. "
        "Does not use the browser's cookies. Refuses loopback, private, and link-local hosts, "
        "including after a redirect. One attempt, no retries, stops on the Stop button. "
        "Body cap is 2 GB. "
        "Without a file path, the saved name is the server filename, else the URL's last segment. "
        "A name with no extension is sniffed from the bytes. "
        "Optional filename is used only when those are empty. "
        "Names are sanitized; an existing file is saved as name-2, name-3, …",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"url", {
                    {"type", "string"},
                    {"description", "http or https URL. Prefer files[].url from browser_read."},
                }},
                {"path", {
                    {"type", "string"},
                    {"description",
                     "Optional destination. A directory saves into that folder. "
                     "A file path sets the folder and the saved name. "
                     "Relative to the current folder. Omit to save in the current folder."},
                }},
                {"filename", {
                    {"type", "string"},
                    {"description", "Optional fallback name when the server and the URL path have none."},
                }},
            }},
            {"required", nlohmann::json::array({"url"})},
        }};
}

} // namespace media::llm::path::detail

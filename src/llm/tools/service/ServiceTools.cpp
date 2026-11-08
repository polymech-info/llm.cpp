// Copyright PolyMech / pm-image. Internal use only.
#include "llm/tools/service/ServiceTools.hpp"

#if defined(FEATURE_INSTALLER_VFS) && FEATURE_INSTALLER_VFS

#include "llm/llm_constants.hpp"

#include "cms/pm_cms_download.hpp"
#include "cms/pm_cms_detail.hpp"
#include "cms/pm_cms_list.hpp"
#include "cms/pm_cms_search.hpp"
#include "cms/pm_cms_pages.hpp"
#include "cms/pm_cms_session.hpp"
#include "cms/pm_cms_upload.hpp"
#include "cms/pm_vfs_listing.hpp"
#include "core/settings_runtime.hpp"
#include "service_defaults.hpp"
#include "media/probe.hpp"
#include "logging/pm_log.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>
#include "fs/path_sanitizer.hpp"

namespace media::llm::path::detail {

namespace fs = std::filesystem;

namespace {

constexpr const char k_log_ch[] = "llm_service_tools";

struct ServiceCtx {
    std::string base;
    std::string bearer;
};

std::optional<std::string> arg_string(const nlohmann::json& args, const char* key)
{
    if (!args.contains(key) || !args[key].is_string())
        return std::nullopt;
    const std::string s = args[key].get<std::string>();
    if (s.empty())
        return std::nullopt;
    return s;
}

std::string arg_string_or(const nlohmann::json& args, const char* key, std::string fallback)
{
    if (auto s = arg_string(args, key))
        return *s;
    return fallback;
}

bool arg_bool_or(const nlohmann::json& args, const char* key, bool fallback)
{
    if (!args.contains(key))
        return fallback;
    const auto& v = args[key];
    if (v.is_boolean())
        return v.get<bool>();
    return fallback;
}

bool resolve_service_ctx(const nlohmann::json& args, ServiceCtx& out, std::string& err)
{
    out = {};
    err.clear();
    std::string override_url;
    if (args.contains("server_url") && args["server_url"].is_string())
        override_url = args["server_url"].get<std::string>();

    const bool public_only = arg_bool_or(args, "public_only", false);
    if (public_only) {
        out.base = pm::services::resolve_cms_server_base(override_url);
        if (out.base.empty()) {
            err = "Set SERVER_URL or pass server_url.";
            return false;
        }
        return true;
    }

    pm::cms::CmsSession session;
    if (!pm::cms::resolve_cms_session(override_url, session, err))
        return false;
    out.base   = std::move(session.base);
    out.bearer = std::move(session.bearer);
    return true;
}

pm::cms::LogLine cms_log()
{
    return [](std::string_view line) {
        pm::log::debug(k_log_ch, std::string(line));
    };
}

void append_cms_history_best_effort(const nlohmann::json& row)
{
    std::string err;
    if (!media::runtime_settings::append_cms_history_entry(row, err) && !err.empty())
        pm::log::debug(k_log_ch, std::string("cms history append failed: ") + err);
}

std::vector<std::string> arg_string_array(const nlohmann::json& args, const char* key)
{
    std::vector<std::string> out;
    if (!args.contains(key) || !args[key].is_array())
        return out;
    for (const auto& el : args[key]) {
        if (!el.is_string())
            continue;
        const std::string s = el.get<std::string>();
        if (!s.empty())
            out.push_back(s);
    }
    return out;
}

std::vector<std::string> split_csv_tags(const std::string& csv)
{
    std::vector<std::string> out;
    std::string              cur;
    auto flush = [&]() {
        trim_in_place(cur);
        if (!cur.empty())
            out.push_back(cur);
        cur.clear();
    };
    for (char c : csv) {
        if (c == ',')
            flush();
        else
            cur.push_back(c);
    }
    flush();
    return out;
}

std::string join_vfs_remote(const std::string& remote_dir, const std::string& rel)
{
    std::string dir = remote_dir;
    while (!dir.empty() && (dir.front() == '/' || dir.front() == '\\'))
        dir.erase(dir.begin());
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\'))
        dir.pop_back();
    std::string leaf = rel;
    while (!leaf.empty() && (leaf.front() == '/' || leaf.front() == '\\'))
        leaf.erase(leaf.begin());
    if (dir.empty())
        return leaf;
    if (leaf.empty())
        return dir;
    return dir + "/" + leaf;
}

bool write_temp_markdown(const std::string& markdown, fs::path& out_path, std::string& err)
{
    err.clear();
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path(ec);
    if (ec) {
        err = "temp directory unavailable";
        return false;
    }
    out_path = tmp / ("pm-service-page-" + std::to_string(
                                      std::chrono::steady_clock::now().time_since_epoch().count())
                      + ".md");
    std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
    if (!ofs)
        return false;
    ofs.write(markdown.data(), static_cast<std::streamsize>(markdown.size()));
    if (!ofs.good()) {
        err = "failed writing temp markdown";
        return false;
    }
    return true;
}

bool resolve_markdown_source(const nlohmann::json& args, fs::path& md_path, bool& temp_file,
                             std::string& err)
{
    temp_file = false;
    err.clear();
    const auto markdown = arg_string(args, "markdown");
    const auto path     = arg_string(args, "path");

    if (markdown && path) {
        err = "provide only one of 'markdown' or 'path'";
        return false;
    }
    if (markdown) {
        if (markdown->size() > pm::llm::k_service_page_markdown_max_bytes) {
            err = "markdown exceeds size limit";
            return false;
        }
        const bool has_slug  = arg_string(args, "slug").has_value();
        const bool has_title = arg_string(args, "title").has_value();
        if (!has_slug && !has_title) {
            err = "inline markdown requires title or slug";
            return false;
        }
        if (!write_temp_markdown(*markdown, md_path, err))
            return false;
        temp_file = true;
        return true;
    }
    if (path) {
        const std::string abs = resolve_tool_path_string(*path);
        const std::string deny = llm_path_fs_deny(abs);
        if (!deny.empty()) {
            err = deny;
            return false;
        }
        std::error_code ec;
        if (!fs::is_regular_file(abs, ec)) {
            err = "not a file: " + abs;
            return false;
        }
        md_path = media::path::path_from_utf8(abs);
        return true;
    }
    err = "one of 'markdown' or 'path' is required";
    return false;
}

pm::cms::MarkdownPageOptions build_create_options(const nlohmann::json& args)
{
    pm::cms::MarkdownPageOptions opt;
    if (auto t = arg_string(args, "title"))
        opt.title = *t;
    if (auto s = arg_string(args, "slug"))
        opt.slug = *s;
    if (auto d = arg_string(args, "description"))
        opt.description = *d;
    if (auto o = arg_string(args, "owner"))
        opt.owner_id = *o;
    if (auto p = arg_string(args, "parent"))
        opt.parent_id = *p;
    opt.category_ids = arg_string_array(args, "category_ids");
    if (args.contains("tags")) {
        if (args["tags"].is_array())
            opt.tags = arg_string_array(args, "tags");
        else if (args["tags"].is_string())
            opt.tags = split_csv_tags(args["tags"].get<std::string>());
    }
    if (args.contains("is_public") && args["is_public"].is_boolean()) {
        opt.is_public         = args["is_public"].get<bool>();
        opt.is_public_cli_set = true;
    }
    if (args.contains("visible") && args["visible"].is_boolean()) {
        opt.visible         = args["visible"].get<bool>();
        opt.visible_cli_set = true;
    }
    return opt;
}

pm::cms::UpdateMarkdownPageOptions build_update_options(const nlohmann::json& args)
{
    pm::cms::UpdateMarkdownPageOptions opt;
    if (auto id = arg_string(args, "page_id"))
        opt.page_id = *id;
    if (auto slug = arg_string(args, "slug"))
        opt.target_slug = *slug;
    if (auto owner = arg_string(args, "owner"))
        opt.owner_or_identifier = *owner;
    opt.id_cli_set   = !opt.page_id.empty();
    opt.slug_cli_set = !opt.target_slug.empty();
    if (auto t = arg_string(args, "title"))
        opt.title = *t;
    if (auto ns = arg_string(args, "new_slug"))
        opt.new_slug = *ns;
    if (auto d = arg_string(args, "description"))
        opt.description = *d;
    if (auto p = arg_string(args, "parent")) {
        opt.parent_id  = *p;
        opt.parent_set = true;
    }
    if (args.contains("tags")) {
        opt.tags_set = true;
        if (args["tags"].is_array())
            opt.tags = arg_string_array(args, "tags");
        else if (args["tags"].is_string())
            opt.tags = split_csv_tags(args["tags"].get<std::string>());
    }
    opt.category_ids     = arg_string_array(args, "category_ids");
    opt.category_ids_set = !opt.category_ids.empty();
    if (args.contains("is_public") && args["is_public"].is_boolean()) {
        opt.is_public     = args["is_public"].get<bool>();
        opt.is_public_set = true;
    }
    if (args.contains("visible") && args["visible"].is_boolean()) {
        opt.visible     = args["visible"].get<bool>();
        opt.visible_set = true;
    }
    return opt;
}

nlohmann::json page_result_json(const pm::cms::CreateMarkdownPageResult& r, const fs::path& source)
{
    return nlohmann::json{{"page_id", r.page_id},
                          {"slug", r.slug},
                          {"owner", r.owner_id},
                          {"title", r.title},
                          {"url", r.url},
                          {"source", media::path::path_to_generic_utf8(source)}};
}

// ── Implemented handlers ─────────────────────────────────────────────────────

ExecuteResult do_service_page_create(const nlohmann::json& args)
{
    ServiceCtx ctx;
    std::string err;
    if (!resolve_service_ctx(args, ctx, err))
        return fatal("service_page_create: " + err);

    fs::path    md_path;
    bool        temp = false;
    if (!resolve_markdown_source(args, md_path, temp, err))
        return fatal("service_page_create: " + err);

    pm::cms::MarkdownPageOptions opt = build_create_options(args);
    const pm::cms::CreateMarkdownPageResult r
        = pm::cms::create_markdown_page_from_file(ctx.base, ctx.bearer, md_path, std::move(opt), cms_log());

    if (temp) {
        std::error_code ec;
        fs::remove(md_path, ec);
    }

    if (!r.ok)
        return fatal("service_page_create: " + r.err);

    nlohmann::json payload = page_result_json(r, md_path);
    append_cms_history_best_effort({{"kind", "page"},
                                    {"page_id", r.page_id},
                                    {"slug", r.slug},
                                    {"title", r.title},
                                    {"url", r.url},
                                    {"service", "tanit"}});
    return simple_ok(std::move(payload));
}

ExecuteResult do_service_page_update(const nlohmann::json& args)
{
    ServiceCtx ctx;
    std::string err;
    if (!resolve_service_ctx(args, ctx, err))
        return fatal("service_page_update: " + err);

    fs::path md_path;
    bool     temp = false;
    if (!resolve_markdown_source(args, md_path, temp, err))
        return fatal("service_page_update: " + err);

    pm::cms::UpdateMarkdownPageOptions opt = build_update_options(args);
    if (!opt.id_cli_set && !opt.slug_cli_set) {
        if (temp)
            fs::remove(md_path);
        return fatal("service_page_update: page_id or slug is required");
    }

    const pm::cms::CreateMarkdownPageResult r
        = pm::cms::update_markdown_page_from_file(ctx.base, ctx.bearer, md_path, std::move(opt), cms_log());

    if (temp) {
        std::error_code ec;
        fs::remove(md_path, ec);
    }

    if (!r.ok)
        return fatal("service_page_update: " + r.err);

    nlohmann::json payload = page_result_json(r, md_path);
    payload["updated"]     = true;
    append_cms_history_best_effort({{"kind", "page"},
                                    {"action", "update"},
                                    {"page_id", r.page_id},
                                    {"slug", r.slug},
                                    {"title", r.title},
                                    {"url", r.url},
                                    {"service", "tanit"}});
    return simple_ok(std::move(payload));
}

ExecuteResult do_service_page_list(const nlohmann::json& args)
{
    ServiceCtx ctx;
    std::string err;
    if (!resolve_service_ctx(args, ctx, err))
        return fatal("service_page_list: " + err);

    const std::string user_id = arg_string_or(args, "user_id", "");
    const auto        r       = pm::cms::list_user_pages(ctx.base, ctx.bearer, user_id, cms_log());
    if (!r.ok)
        return fatal("service_page_list: " + r.err);
    return simple_ok(r.json);
}

const nlohmann::json& page_payload_object(const nlohmann::json& j)
{
    if (j.contains("page") && j["page"].is_object())
        return j["page"];
    return j;
}

bool find_markdown_text_content(const nlohmann::json& j, std::string& markdown)
{
    if (j.is_object()) {
        const bool is_markdown_widget = j.value("widgetId", std::string()) == "markdown-text";
        if (is_markdown_widget && j.contains("props") && j["props"].is_object()
            && j["props"].contains("content") && j["props"]["content"].is_string()) {
            markdown = j["props"]["content"].get<std::string>();
            return true;
        }
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (find_markdown_text_content(it.value(), markdown))
                return true;
        }
    } else if (j.is_array()) {
        for (const auto& item : j) {
            if (find_markdown_text_content(item, markdown))
                return true;
        }
    }
    return false;
}

std::string json_scalar_string(const nlohmann::json& j)
{
    if (j.is_string())
        return j.get<std::string>();
    if (j.is_boolean())
        return j.get<bool>() ? "true" : "false";
    if (j.is_number_integer())
        return std::to_string(j.get<long long>());
    if (j.is_number_unsigned())
        return std::to_string(j.get<unsigned long long>());
    if (j.is_number_float())
        return std::to_string(j.get<double>());
    return j.dump();
}

std::string page_json_string(const nlohmann::json& page, const char* key)
{
    if (!page.contains(key) || page[key].is_null())
        return {};
    return json_scalar_string(page[key]);
}

bool write_text_file(const fs::path& path, const std::string& body, std::string& err)
{
    err.clear();
    std::error_code ec;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), ec);
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        err = "cannot open for writing: " + media::path::path_to_generic_utf8(path);
        return false;
    }
    ofs.write(body.data(), static_cast<std::streamsize>(body.size()));
    if (!ofs.good()) {
        err = "write failed: " + media::path::path_to_generic_utf8(path);
        return false;
    }
    return true;
}

ExecuteResult do_service_page_get(const nlohmann::json& args)
{
    const auto page_id = arg_string(args, "page_id");
    const auto slug    = arg_string(args, "slug");
    if (!page_id && !slug)
        return fatal("service_page_get: page_id or slug is required");

    ServiceCtx ctx;
    std::string err;
    if (!resolve_service_ctx(args, ctx, err))
        return fatal("service_page_get: " + err);

    const std::string lang = arg_string_or(args, "lang", "");
    pm::cms::CmsJsonDetailResult r;
    if (slug) {
        std::string owner = arg_string_or(args, "owner", "");
        if (owner.empty() && page_id)
            owner = *page_id;
        if (owner.empty())
            owner = pm::cms::resolve_current_app_user_id_or_default();
        r = pm::cms::get_user_page_detail(ctx.base, ctx.bearer, owner, *slug, lang, cms_log());
    } else {
        r = pm::cms::get_page_detail_by_id(ctx.base, ctx.bearer, *page_id, lang, cms_log());
    }
    if (!r.ok)
        return fatal("service_page_get: " + r.err);

    const nlohmann::json& page = page_payload_object(r.json);
    const auto            local_path_opt = arg_string(args, "local_path");

    std::string markdown;
    const bool  has_markdown = find_markdown_text_content(page, markdown);
    std::string file_body;
    std::string file_kind;
    if (has_markdown) {
        file_body = markdown;
        file_kind = "markdown";
    } else {
        file_body = r.body_raw;
        file_kind = "json";
    }

    if (local_path_opt) {
        const std::string out_abs = resolve_tool_path_string(*local_path_opt);
        const std::string deny    = llm_path_write_deny(out_abs);
        if (!deny.empty())
            return fatal("service_page_get: " + deny);
        if (!write_text_file(media::path::path_from_utf8(out_abs), file_body, err))
            return fatal("service_page_get: " + err);

        return simple_ok(nlohmann::json{{"page_id", page_json_string(page, "id")},
                                        {"slug", page_json_string(page, "slug")},
                                        {"title", page_json_string(page, "title")},
                                        {"url", page_json_string(page, "url")},
                                        {"content_kind", file_kind},
                                        {"local_path", out_abs},
                                        {"size", static_cast<long long>(file_body.size())}});
    }

    nlohmann::json payload{{"page_id", page_json_string(page, "id")},
                           {"slug", page_json_string(page, "slug")},
                           {"title", page_json_string(page, "title")},
                           {"description", page_json_string(page, "description")},
                           {"url", page_json_string(page, "url")},
                           {"owner", page_json_string(page, "owner")}};

    if (has_markdown) {
        if (markdown.size() > pm::llm::k_service_page_markdown_max_bytes) {
            return fatal("service_page_get: markdown is "
                         + std::to_string(markdown.size())
                         + " bytes; set local_path to save to disk");
        }
        payload["markdown"] = std::move(markdown);
        payload["content_kind"] = "markdown";
    } else {
        if (r.body_raw.size() > pm::llm::k_service_files_get_max_inline_bytes) {
            return fatal("service_page_get: page JSON is "
                         + std::to_string(r.body_raw.size())
                         + " bytes; set local_path to save to disk");
        }
        payload["page"]         = page;
        payload["content_kind"] = "json";
    }

    return simple_ok(std::move(payload));
}

ExecuteResult do_service_files_list(const nlohmann::json& args)
{
    ServiceCtx ctx;
    std::string err;
    if (!resolve_service_ctx(args, ctx, err))
        return fatal("service_files_list: " + err);

    const std::string mount = arg_string_or(args, "mount", "home");
    const std::string path  = arg_string_or(args, "path", "");
    const auto        r     = pm::cms::list_vfs_files(ctx.base, ctx.bearer, mount, path, true, cms_log());
    if (!r.ok)
        return fatal("service_files_list: " + r.err);
    return simple_ok(r.json);
}

constexpr int k_files_batch_max = 200;
constexpr int k_files_rows_shown = 50;

std::string clean_vfs_path(std::string p)
{
    std::replace(p.begin(), p.end(), '\\', '/');
    while (!p.empty() && p.front() == '/')
        p.erase(p.begin());
    while (!p.empty() && p.back() == '/')
        p.pop_back();
    if (p == ".")
        p.clear();
    return p;
}

ExecuteResult do_service_files_pull(const ServiceCtx& ctx, const nlohmann::json& args,
                                    const std::string& mount, const std::string& remote)
{
    const std::string out_abs = resolve_tool_path_string(*arg_string(args, "local_dir"));
    const std::string deny    = llm_path_write_deny(out_abs);
    if (!deny.empty())
        return fatal("service_files_get: " + deny);

    pm::cms::VfsPullOptions popts;
    popts.remote_path = clean_vfs_path(remote);
    popts.local_root  = media::path::path_from_utf8(out_abs);
    popts.mount       = mount;
    popts.pattern     = arg_string_or(args, "pattern", "");
    popts.file_options.bearer_token  = ctx.bearer;
    popts.file_options.endpoint      = pm::cms::VfsDownloadEndpoint::Read;
    popts.file_options.log           = cms_log();
    popts.file_options.overwrite     = arg_bool_or(args, "overwrite", false);
    popts.file_options.skip_existing = !popts.file_options.overwrite;

    const pm::cms::VfsPullResult r = pm::cms::pull_vfs_to_local(ctx.base, std::move(popts));
    if (!r.ok && r.files_ok == 0 && r.files_skipped == 0)
        return fatal("service_files_get: " + (r.err.empty() ? std::string("download failed") : r.err));

    nlohmann::json files = nlohmann::json::array();
    for (const auto& f : r.files) {
        if (static_cast<int>(files.size()) >= k_files_rows_shown)
            break;
        nlohmann::json row{{"path", f.remote_path},
                           {"local_path", media::path::path_to_utf8(f.local_path)}};
        if (!f.ok)
            row["error"] = f.err;
        else if (f.skipped)
            row["skipped"] = true;
        files.push_back(std::move(row));
    }
    return simple_ok(nlohmann::json{{"mount", mount},
                                    {"path", remote},
                                    {"local_dir", out_abs},
                                    {"files_ok", r.files_ok},
                                    {"files_skipped", r.files_skipped},
                                    {"files_failed", r.files_failed},
                                    {"bytes", static_cast<long long>(r.bytes_written)},
                                    {"files", std::move(files)}});
}

ExecuteResult do_service_files_get(const nlohmann::json& args)
{
    const auto remote_path = arg_string(args, "path");
    if (!remote_path)
        return fatal("service_files_get: 'path' (remote VFS path) is required");

    ServiceCtx ctx;
    std::string err;
    if (!resolve_service_ctx(args, ctx, err))
        return fatal("service_files_get: " + err);

    const std::string mount = arg_string_or(args, "mount", "home");
    if (arg_string(args, "local_dir"))
        return do_service_files_pull(ctx, args, mount, *remote_path);
    const auto          local_path_opt = arg_string(args, "local_path");

    if (local_path_opt) {
        const std::string out_abs = resolve_tool_path_string(*local_path_opt);
        const std::string deny    = llm_path_write_deny(out_abs);
        if (!deny.empty())
            return fatal("service_files_get: " + deny);

        pm::cms::VfsDownloadOptions dlopts;
        dlopts.bearer_token = ctx.bearer;
        dlopts.endpoint     = pm::cms::VfsDownloadEndpoint::Read;
        dlopts.log          = cms_log();
        dlopts.overwrite    = arg_bool_or(args, "overwrite", false);
        dlopts.skip_existing = !dlopts.overwrite;

        const pm::cms::VfsDownloadResult dr = pm::cms::download_vfs_file_to_path(
            ctx.base, mount, *remote_path, media::path::path_from_utf8(out_abs), std::move(dlopts));
        if (!dr.ok)
            return fatal("service_files_get: " + dr.err);

        return simple_ok(nlohmann::json{{"mount", mount},
                                        {"path", *remote_path},
                                        {"local_path", out_abs},
                                        {"size", static_cast<long long>(dr.bytes_written)},
                                        {"skipped", dr.skipped},
                                        {"resumed_from", dr.resumed_from}});
    }

    const pm::cms::CmsRawContentResult raw
        = pm::cms::read_vfs_file_raw(ctx.base, ctx.bearer, mount, *remote_path, cms_log());
    if (!raw.ok)
        return fatal("service_files_get: " + raw.err);

    if (raw.bytes.size() > pm::llm::k_service_files_get_max_inline_bytes) {
        return fatal("service_files_get: file is "
                     + std::to_string(raw.bytes.size())
                     + " bytes; set local_path to download to disk");
    }

    const size_t probe_len = (std::min)(raw.bytes.size(), std::size_t{65536});
    if (probe_len > 0) {
        const std::string_view probe_bytes(reinterpret_cast<const char*>(raw.bytes.data()), probe_len);
        const auto             probe       = media::probe::probe_buffer(probe_bytes, *remote_path);
        const bool             is_text_like = !probe.ok || probe.kind == media::MediaKind::Text
                                   || probe.kind == media::MediaKind::Code
                                   || probe.kind == media::MediaKind::Script;
        if (!is_text_like) {
            return fatal("service_files_get: binary file; set local_path to download to disk");
        }
    }

    std::string content(reinterpret_cast<const char*>(raw.bytes.data()), raw.bytes.size());
    return simple_ok(nlohmann::json{{"mount", mount},
                                    {"path", *remote_path},
                                    {"size", static_cast<long long>(raw.bytes.size())},
                                    {"content", std::move(content)}});
}

std::int64_t local_mtime_ms(const std::string& abs)
{
    std::error_code ec;
    const auto ft = fs::last_write_time(media::path::path_from_utf8(abs), ec);
    if (ec)
        return 0;
    const auto sys = std::chrono::system_clock::now() + (ft - fs::file_time_type::clock::now());
    return std::chrono::duration_cast<std::chrono::milliseconds>(sys.time_since_epoch()).count();
}

/// One upload, conflict policy applied. Returns a result row; `ok` false on failure.
nlohmann::json upload_one(const ServiceCtx& ctx, const std::string& abs, const std::string& mount,
                          const std::string& remote, pm::cms::VfsUploadConflict policy)
{
    if (policy != pm::cms::VfsUploadConflict::Overwrite) {
        const size_t slash = remote.find_last_of('/');
        const std::string parent = slash == std::string::npos ? std::string() : remote.substr(0, slash);
        const auto listing = pm::cms::list_vfs_files(ctx.base, ctx.bearer, mount, parent, false, cms_log());
        bool exists = false;
        std::int64_t remote_ms = 0;
        if (listing.ok) {
            const std::vector<pm::cms::VfsListEntry> entries = pm::cms::normalize_vfs_listing(listing.json);
            const pm::cms::VfsListEntry* found = pm::cms::find_vfs_list_entry(entries, remote);
            if (found) {
                exists    = true;
                remote_ms = found->mtime_ms;
            }
        } else if (listing.http_status != 404) {
            pm::log::info(k_log_ch, "service_files_upload listing failed; uploading without skip: " + listing.err);
        }
        const auto skip = pm::cms::decide_vfs_upload_skip(policy, exists, local_mtime_ms(abs), remote_ms);
        if (skip.skip) {
            return nlohmann::json{{"ok", true},
                                  {"local_path", abs},
                                  {"mount", mount},
                                  {"vfs_path", remote},
                                  {"skipped", true},
                                  {"skip_reason", skip.reason}};
        }
    }

    const pm::cms::VfsUploadResult r
        = pm::cms::upload_vfs_file(ctx.base, ctx.bearer, media::path::path_from_utf8(abs), mount, remote, cms_log());
    if (!r.ok)
        return nlohmann::json{{"ok", false}, {"local_path", abs}, {"vfs_path", remote}, {"error", r.err}};

    append_cms_history_best_effort({{"kind", "file-upload"},
                                    {"path", abs},
                                    {"mount", r.mount},
                                    {"vfs_path", r.path},
                                    {"url", r.browser_url},
                                    {"service", "tanit"}});

    return nlohmann::json{{"ok", true},
                          {"local_path", abs},
                          {"mount", r.mount},
                          {"vfs_path", r.path},
                          {"url", r.browser_url},
                          {"api_get_url", r.api_get_url},
                          {"skipped", false}};
}

ExecuteResult do_service_files_upload(const nlohmann::json& args)
{
    const auto local_raw = arg_string(args, "path");
    const auto content   = arg_string(args, "content");
    if (local_raw && content)
        return fatal("service_files_upload: provide only one of 'path' or 'content'");
    if (!local_raw && !content)
        return fatal("service_files_upload: 'path' (local file or folder) or 'content' is required");

    ServiceCtx ctx;
    std::string err;
    if (!resolve_service_ctx(args, ctx, err))
        return fatal("service_files_upload: " + err);

    const std::string mount = arg_string_or(args, "mount", "home");
    std::string remote_dir  = arg_string_or(args, "remote_dir", "");
    if (arg_bool_or(args, "public", false)) {
        if (!remote_dir.empty() && clean_vfs_path(remote_dir) != "public"
            && clean_vfs_path(remote_dir).rfind("public/", 0) != 0)
            return fatal("service_files_upload: 'public' puts files under public/; drop remote_dir or start it with public/");
        if (clean_vfs_path(remote_dir).rfind("public", 0) != 0)
            remote_dir = "public";
    }

    if (content) {
        auto rp = arg_string(args, "remote_path");
        if (!rp)
            return fatal("service_files_upload: 'content' needs remote_path (file name in the VFS)");
        if (content->size() > pm::llm::k_service_files_get_max_inline_bytes)
            return fatal("service_files_upload: content exceeds size limit; write a local file and pass 'path'");
        std::string remote = clean_vfs_path(*rp);
        if (!remote_dir.empty() && remote.find('/') == std::string::npos)
            remote = join_vfs_remote(remote_dir, remote);
        const pm::cms::VfsUploadResult r = pm::cms::upload_vfs_bytes(
            ctx.base, ctx.bearer, content->data(), content->size(), mount, remote, cms_log());
        if (!r.ok)
            return fatal("service_files_upload: " + r.err);
        append_cms_history_best_effort({{"kind", "file-upload"},
                                        {"mount", r.mount},
                                        {"vfs_path", r.path},
                                        {"url", r.browser_url},
                                        {"service", "tanit"}});
        return simple_ok(nlohmann::json{{"mount", r.mount},
                                        {"vfs_path", r.path},
                                        {"size", static_cast<long long>(content->size())},
                                        {"url", r.browser_url},
                                        {"api_get_url", r.api_get_url},
                                        {"skipped", false}});
    }

    const std::string abs = resolve_tool_path_string(*local_raw);
    const std::string deny = llm_path_fs_deny(abs);
    if (!deny.empty())
        return fatal("service_files_upload: " + deny);

    pm::cms::VfsUploadConflict policy = pm::cms::VfsUploadConflict::IfNewer;
    if (auto conflict = arg_string(args, "conflict")) {
        if (!pm::cms::parse_vfs_upload_conflict(*conflict, policy, err))
            return fatal("service_files_upload: " + err);
    }
    if (arg_bool_or(args, "overwrite", false))
        policy = pm::cms::VfsUploadConflict::Overwrite;
    if (arg_bool_or(args, "skip", false))
        policy = pm::cms::VfsUploadConflict::Skip;
    if (arg_bool_or(args, "if_newer", false))
        policy = pm::cms::VfsUploadConflict::IfNewer;

    std::error_code ec;
    const fs::path local = media::path::path_from_utf8(abs);
    if (fs::is_directory(local, ec)) {
        // Folder: files keep their paths relative to the folder, under remote_dir/<folder name>.
        std::string base_remote = arg_string(args, "remote_path")
            ? clean_vfs_path(*arg_string(args, "remote_path"))
            : join_vfs_remote(remote_dir, media::path::path_to_utf8(local.filename()));
        std::vector<fs::path> files;
        for (fs::recursive_directory_iterator it(local, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_symlink(ec) || !it->is_regular_file(ec))
                continue;
            if (!llm_path_fs_deny(media::path::path_to_utf8(it->path())).empty())
                continue;
            files.push_back(it->path());
            if (static_cast<int>(files.size()) > k_files_batch_max)
                return fatal("service_files_upload: folder has more than "
                             + std::to_string(k_files_batch_max) + " files; upload a smaller folder");
        }
        if (files.empty())
            return fatal("service_files_upload: folder is empty: " + abs);
        nlohmann::json rows = nlohmann::json::array();
        int uploaded = 0, skipped = 0, failed = 0;
        for (const auto& f : files) {
            const std::string rel = media::path::path_to_generic_utf8(fs::relative(f, local, ec));
            nlohmann::json row = upload_one(ctx, media::path::path_to_utf8(f), mount,
                                            join_vfs_remote(base_remote, rel), policy);
            if (!row.value("ok", false))
                ++failed;
            else if (row.value("skipped", false))
                ++skipped;
            else
                ++uploaded;
            if (static_cast<int>(rows.size()) < k_files_rows_shown)
                rows.push_back(std::move(row));
        }
        if (uploaded == 0 && skipped == 0)
            return fatal("service_files_upload: every file failed; first error: "
                         + rows.front().value("error", std::string("unknown")));
        return simple_ok(nlohmann::json{{"local_path", abs},
                                        {"mount", mount},
                                        {"vfs_path", base_remote},
                                        {"url", pm::cms::build_filebrowser_url(ctx.base, mount, base_remote)},
                                        {"uploaded", uploaded},
                                        {"skipped", skipped},
                                        {"failed", failed},
                                        {"files", std::move(rows)}});
    }
    if (!fs::is_regular_file(local, ec))
        return fatal("service_files_upload: not a file or folder: " + abs);

    std::string remote;
    if (auto rp = arg_string(args, "remote_path"))
        remote = clean_vfs_path(*rp);
    else
        remote = join_vfs_remote(remote_dir, media::path::path_to_utf8(local.filename()));

    nlohmann::json row = upload_one(ctx, abs, mount, remote, policy);
    if (!row.value("ok", false))
        return fatal("service_files_upload: " + row.value("error", std::string("upload failed")));
    row.erase("ok");
    return simple_ok(std::move(row));
}

ExecuteResult do_service_files_delete(const nlohmann::json& args)
{
    std::vector<std::string> paths = arg_string_array(args, "paths");
    if (auto p = arg_string(args, "path"))
        paths.insert(paths.begin(), *p);
    if (paths.empty())
        return fatal("service_files_delete: 'path' or 'paths' (remote VFS paths) is required");
    if (static_cast<int>(paths.size()) > k_files_rows_shown)
        return fatal("service_files_delete: at most " + std::to_string(k_files_rows_shown) + " paths per call");
    for (auto& p : paths) {
        p = clean_vfs_path(p);
        if (p.empty())
            return fatal("service_files_delete: refusing to delete the mount root");
    }

    ServiceCtx ctx;
    std::string err;
    if (!resolve_service_ctx(args, ctx, err))
        return fatal("service_files_delete: " + err);

    const std::string mount = arg_string_or(args, "mount", "home");
    nlohmann::json rows = nlohmann::json::array();
    int deleted = 0;
    for (const auto& p : paths) {
        const pm::cms::CmsDeleteResult r = pm::cms::delete_vfs_path(ctx.base, ctx.bearer, mount, p, cms_log());
        nlohmann::json row{{"path", p}, {"deleted", r.ok}};
        if (r.ok)
            ++deleted;
        else
            row["error"] = r.err.empty() ? ("HTTP " + std::to_string(r.http_status)) : r.err;
        rows.push_back(std::move(row));
    }
    if (deleted == 0)
        return fatal("service_files_delete: " + rows.front().value("error", std::string("delete failed")));
    return simple_ok(nlohmann::json{{"mount", mount}, {"deleted", deleted}, {"results", std::move(rows)}});
}

ExecuteResult do_service_files_link(const nlohmann::json& args)
{
    const auto raw = arg_string(args, "path");
    if (!raw)
        return fatal("service_files_link: 'path' (remote VFS path) is required");
    const std::string path = clean_vfs_path(*raw);
    if (path.empty())
        return fatal("service_files_link: 'path' must name a file or folder");

    ServiceCtx ctx;
    std::string err;
    if (!resolve_service_ctx(args, ctx, err))
        return fatal("service_files_link: " + err);

    const std::string mount = arg_string_or(args, "mount", "home");
    nlohmann::json out{{"mount", mount},
                       {"path", path},
                       {"url", pm::cms::build_filebrowser_url(ctx.base, mount, path)}};

    const size_t slash = path.find_last_of('/');
    const std::string parent = slash == std::string::npos ? std::string() : path.substr(0, slash);
    const auto listing = pm::cms::list_vfs_files(ctx.base, ctx.bearer, mount, parent, false, cms_log());
    const pm::cms::VfsListEntry* found = nullptr;
    std::vector<pm::cms::VfsListEntry> entries;
    if (listing.ok) {
        entries = pm::cms::normalize_vfs_listing(listing.json);
        found   = pm::cms::find_vfs_list_entry(entries, path);
    }
    if (listing.ok && !found)
        return fatal("service_files_link: not found: " + mount + "/" + path);
    if (found && found->is_directory) {
        out["is_directory"] = true;
        return simple_ok(std::move(out));
    }
    const std::string signed_url = pm::cms::sign_vfs_get_url(
        ctx.base, ctx.bearer, mount, path, found ? found->mtime_ms : 0,
        arg_bool_or(args, "download", false), cms_log());
    if (!signed_url.empty()) {
        out["direct_url"]         = signed_url;
        out["direct_url_expires"] = "15 minutes";
    }
    return simple_ok(std::move(out));
}

ExecuteResult do_service_search(const nlohmann::json& args)
{
    const auto query = arg_string(args, "q");
    if (!query)
        return fatal("service_search: 'q' is required");

    const bool public_only = arg_bool_or(args, "public_only", false);
    const auto   vis       = arg_string(args, "visibility_filter");
    if (public_only && vis) {
        return fatal("service_search: visibility_filter requires authenticated search; omit public_only");
    }

    ServiceCtx ctx;
    std::string err;
    if (!resolve_service_ctx(args, ctx, err))
        return fatal("service_search: " + err);

    pm::cms::CmsSearchOptions opts;
    opts.q = *query;
    if (args.contains("limit") && args["limit"].is_number_integer())
        opts.limit = args["limit"].get<int>();
    else if (args.contains("limit") && args["limit"].is_number_unsigned())
        opts.limit = static_cast<int>(args["limit"].get<unsigned>());
    opts.type              = arg_string_or(args, "type", "all");
    opts.sizes             = arg_string_or(args, "sizes", "");
    opts.formats           = arg_string_or(args, "formats", "");
    opts.visibility_filter = vis.value_or("");

    const auto r = pm::cms::search_content(ctx.base, ctx.bearer, std::move(opts), cms_log());
    if (!r.ok)
        return fatal("service_search: " + r.err);
    return simple_ok(r.json);
}

} // namespace

bool is_service_tool(std::string_view name) noexcept
{
    return name == "service_page_create" || name == "service_page_update" || name == "service_page_list"
        || name == "service_page_get"
        || name == "service_files_list" || name == "service_files_get" || name == "service_files_upload"
        || name == "service_files_delete" || name == "service_files_link"
        || name == "service_search";
}

ExecuteResult do_service_tool(const std::string& name, const nlohmann::json& args)
{
    if (name == "service_page_create")  return do_service_page_create(args);
    if (name == "service_page_update")  return do_service_page_update(args);
    if (name == "service_page_list")    return do_service_page_list(args);
    if (name == "service_page_get")     return do_service_page_get(args);
    if (name == "service_files_list")   return do_service_files_list(args);
    if (name == "service_files_get")    return do_service_files_get(args);
    if (name == "service_files_upload") return do_service_files_upload(args);
    if (name == "service_files_delete") return do_service_files_delete(args);
    if (name == "service_files_link")   return do_service_files_link(args);
    if (name == "service_search")       return do_service_search(args);
    return fatal("unknown service tool: " + name);
}

std::vector<ToolDef> service_tool_catalog()
{
    nlohmann::json server_url_prop = {
        {"type", "string"},
        {"description", "Optional Tanit CMS base URL override. Default: SERVER_URL / compiled default."},
    };
    nlohmann::json mount_prop = {
        {"type", "string"},
        {"description", "VFS mount name. Default: home."},
        {"default", "home"},
    };

    return {
        ToolDef{
            "service_page_create",
            "Create a Tanit CMS page from markdown. Auth: zitadel-oauth.json (run tanit-cli login). "
            "Provide inline markdown or a local .md path.",
            nlohmann::json{
                {"type", "object"},
                {"properties", {
                    {"markdown", {{"type", "string"}, {"description", "Inline markdown body."}}},
                    {"path", {{"type", "string"}, {"description", "Local .md file (supports front matter)."}}},
                    {"slug", {{"type", "string"}, {"description", "URL slug (required for inline markdown unless set in front matter)."}}},
                    {"title", {{"type", "string"}, {"description", "Page title."}}},
                    {"description", {{"type", "string"}, {"description", "Meta description."}}},
                    {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                    {"owner", {{"type", "string"}}},
                    {"parent", {{"type", "string"}}},
                    {"category_ids", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                    {"is_public", {{"type", "boolean"}}},
                    {"visible", {{"type", "boolean"}}},
                    {"server_url", server_url_prop},
                }},
            }},
        ToolDef{
            "service_page_update",
            "Update an existing Tanit page from markdown. Identify by page_id or slug.",
            nlohmann::json{
                {"type", "object"},
                {"properties", {
                    {"page_id", {{"type", "string"}, {"description", "Page UUID."}}},
                    {"slug", {{"type", "string"}, {"description", "Existing page slug when page_id omitted."}}},
                    {"markdown", {{"type", "string"}}},
                    {"path", {{"type", "string"}}},
                    {"title", {{"type", "string"}}},
                    {"new_slug", {{"type", "string"}}},
                    {"description", {{"type", "string"}}},
                    {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                    {"owner", {{"type", "string"}}},
                    {"parent", {{"type", "string"}}},
                    {"category_ids", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                    {"is_public", {{"type", "boolean"}}},
                    {"visible", {{"type", "boolean"}}},
                    {"server_url", server_url_prop},
                }},
            }},
        ToolDef{
            "service_page_list",
            "List Tanit CMS pages for the logged-in user. Use to find page_id and slug before service_page_update.",
            nlohmann::json{
                {"type", "object"},
                {"properties", {
                    {"user_id", {
                        {"type", "string"},
                        {"description",
                         "Owner user UUID. Default: app_user_id from zitadel-oauth.json (logged-in user)."},
                    }},
                    {"server_url", server_url_prop},
                }},
            }},
        ToolDef{
            "service_page_get",
            "Fetch a Tanit CMS page by page_id or owner/slug. Returns markdown body inline when present; "
            "set local_path for large pages or to save to disk.",
            nlohmann::json{
                {"type", "object"},
                {"properties", {
                    {"page_id", {{"type", "string"}, {"description", "Page UUID. Fetch by id when slug omitted."}}},
                    {"slug", {{"type", "string"}, {"description", "Page slug (requires owner unless page_id is owner)."}}},
                    {"owner", {{"type", "string"}, {"description", "Owner UUID/username for slug lookup."}}},
                    {"lang", {{"type", "string"}, {"description", "Optional page language query parameter."}}},
                    {"local_path", {{"type", "string"}, {"description", "Save markdown or full page JSON to this local path."}}},
                    {"server_url", server_url_prop},
                }},
            }},
        ToolDef{
            "service_files_list",
            "List files in the Tanit VFS (remote cloud storage). Not local disk — use file_glob for local files.",
            nlohmann::json{
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}, {"description", "Directory inside mount. Empty = root."}}},
                    {"mount", mount_prop},
                    {"server_url", server_url_prop},
                }},
            }},
        ToolDef{
            "service_files_get",
            "Read a file from Tanit VFS. Returns inline text for small text files; set local_path for binary/large files. "
            "Set local_dir to download a whole folder (or one file) into a local directory.",
            nlohmann::json{
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}, {"description", "Remote VFS file or folder path (required)."}}},
                    {"mount", mount_prop},
                    {"local_path", {{"type", "string"}, {"description", "Download one file to this local path instead of inline content."}}},
                    {"local_dir", {{"type", "string"}, {"description", "Download the file or folder tree into this local directory (keeps subfolders)."}}},
                    {"pattern", {{"type", "string"}, {"description", "With local_dir: glob filter, e.g. **/*.jpg."}}},
                    {"overwrite", {{"type", "boolean"}, {"description", "Re-download when the local file exists."}}},
                    {"server_url", server_url_prop},
                }},
                {"required", nlohmann::json::array({"path"})},
            }},
        ToolDef{
            "service_files_upload",
            "Upload to Tanit VFS: a local file, a local folder (up to 200 files, subfolders kept), or inline text "
            "(content + remote_path). Default: overwrite remote when local is newer; skip when remote is newer or the same.",
            nlohmann::json{
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}, {"description", "Local file or folder path."}}},
                    {"content", {{"type", "string"}, {"description", "Inline text to store instead of a local file. Needs remote_path."}}},
                    {"mount", mount_prop},
                    {"remote_path", {{"type", "string"}, {"description", "Exact remote path; default: filename (or folder name) under remote_dir."}}},
                    {"remote_dir", {{"type", "string"}, {"description", "Remote directory when remote_path omitted."}}},
                    {"public", {{"type", "boolean"}, {"description", "Put it under the shared public/ folder (anyone with the link can view)."}}},
                    {"conflict", {
                        {"type", "string"},
                        {"enum", nlohmann::json::array({"if-newer", "skip", "overwrite"})},
                        {"description", "Conflict policy. Default if-newer (overwrite older remotes)."},
                    }},
                    {"skip", {{"type", "boolean"}, {"description", "Skip when the remote file already exists."}}},
                    {"overwrite", {{"type", "boolean"}, {"description", "Always replace the remote file."}}},
                    {"if_newer", {{"type", "boolean"}, {"description", "Overwrite remote only when local is newer (default)."}}},
                    {"server_url", server_url_prop},
                }},
            }},
        ToolDef{
            "service_files_delete",
            "Delete files or folders from Tanit VFS (remote cloud storage). Folders are removed with their contents. "
            "Not local disk — use file_delete for local files. Only delete what the user asked for.",
            nlohmann::json{
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}, {"description", "Remote VFS file or folder path."}}},
                    {"paths", {{"type", "array"}, {"items", {{"type", "string"}}},
                               {"description", "Several remote paths (max 50)."}}},
                    {"mount", mount_prop},
                    {"server_url", server_url_prop},
                }},
            }},
        ToolDef{
            "service_files_link",
            "Get a share link for a Tanit VFS file or folder: the Tanit file browser URL, plus a direct "
            "download URL for a file that works without sign-in for 15 minutes.",
            nlohmann::json{
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}, {"description", "Remote VFS file or folder path (required)."}}},
                    {"mount", mount_prop},
                    {"download", {{"type", "boolean"}, {"description", "Direct URL downloads instead of opening inline."}}},
                    {"server_url", server_url_prop},
                }},
                {"required", nlohmann::json::array({"path"})},
            }},
        ToolDef{
            "service_search",
            "Full-text search Tanit CMS (pages, posts, pictures, VFS files, places). "
            "Default: logged-in user search (private + public). Set public_only for global/public-only search.",
            nlohmann::json{
                {"type", "object"},
                {"properties", {
                    {"q", {{"type", "string"}, {"description", "Search query (required)."}}},
                    {"type", {
                        {"type", "string"},
                        {"enum", nlohmann::json::array({"all", "pages", "posts", "pictures", "files", "places"})},
                        {"default", "all"},
                    }},
                    {"limit", {{"type", "integer"}, {"description", "Max results (default 20, server cap 50)."}}},
                    {"sizes", {{"type", "string"}, {"description", "Responsive image widths, e.g. 320,640."}}},
                    {"formats", {{"type", "string"}, {"description", "Responsive image formats, e.g. avif,webp."}}},
                    {"visibility_filter", {
                        {"type", "string"},
                        {"enum", nlohmann::json::array({"invisible", "private"})},
                        {"description", "Authenticated-only filter for invisible or private content."},
                    }},
                    {"public_only", {
                        {"type", "boolean"},
                        {"description", "Anonymous/global search without bearer (public content only)."},
                    }},
                    {"server_url", server_url_prop},
                }},
                {"required", nlohmann::json::array({"q"})},
            }},
    };
}

} // namespace media::llm::path::detail

#endif // FEATURE_INSTALLER_VFS

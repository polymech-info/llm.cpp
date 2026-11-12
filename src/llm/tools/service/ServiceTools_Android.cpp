/**
 * Android session + VFS picker bridge for the shared ServiceTools.cpp catalog.
 * The phone stores the Zitadel bundle through the Keystore-backed oauth store,
 * so the desktop plaintext oauth readers are provided here.
 */
#include "pm_android_service.hpp"
#include "fs/path_sanitizer.hpp"
#include "pm/vfs/store.hpp"

#include "cms/cms_host.hpp"
#include "cms/pm_cms_ai_gateway.hpp"
#include "cms/pm_cms_download.hpp"
#include "cms/pm_cms_list.hpp"
#include "cms/pm_cms_pages.hpp"
#include "cms/pm_cms_posts.hpp"
#include "cms/pm_cms_session.hpp"
#include "cms/pm_cms_upload.hpp"
#include "service/pm_service_spending.hpp"
#include "cms/pm_vfs_listing.hpp"
#include "core/settings_portable.hpp"
#include "core/settings_runtime.hpp"
#include "lib/pm_zitadel_oauth.hpp"
#include "pm_android_oauth_store.hpp"
#include "service_defaults.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string trim_slash(std::string s)
{
  while (!s.empty() && (s.back() == '/' || s.back() == '\\'))
    s.pop_back();
  return s;
}

bool load_bundle(nlohmann::json& bundle, std::string& err)
{
  const std::string profile =
      media::path::path_to_utf8(media::portable_settings::settings_pixelwiz_profile_dir());
  return pm::android::oauth_load(profile, bundle, err);
}

std::string safe_leaf(const std::string& path)
{
  const auto slash = path.find_last_of("/\\");
  std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
  if (leaf.empty() || leaf == "." || leaf == "..")
    leaf = "vfs-file";
  for (char& c : leaf) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 32 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
      c = '_';
  }
  return leaf;
}

} // namespace

bool pm_zitadel_oauth_read_access_token(std::string& token_out, std::string& err)
{
  const std::string profile =
      media::path::path_to_utf8(media::portable_settings::settings_pixelwiz_profile_dir());
  return pm::android::oauth_api_bearer(profile, token_out, err);
}

bool pm_zitadel_oauth_read_app_user_id(std::string& uuid_out, std::string& err)
{
  nlohmann::json bundle;
  if (!load_bundle(bundle, err))
    return false;
  uuid_out = bundle.value("app_user_id", std::string{});
  if (uuid_out.empty()) {
    err = "signed-in account has no app_user_id";
    return false;
  }
  return true;
}

bool pm_zitadel_oauth_read_app_username(std::string& username_out, std::string& err)
{
  nlohmann::json bundle;
  if (!load_bundle(bundle, err))
    return false;
  username_out = bundle.value("app_username", std::string{});
  if (username_out.empty())
    username_out = bundle.value("app_display_name", std::string{});
  if (username_out.empty()) {
    err = "signed-in account has no username";
    return false;
  }
  return true;
}

bool pm_zitadel_oauth_read_cms_server_base(std::string& base_out, std::string& err)
{
  nlohmann::json bundle;
  if (!load_bundle(bundle, err))
    return false;
  base_out = trim_slash(bundle.value("cms_server_base", std::string{}));
  if (base_out.empty()) {
    err = "login did not pin a CMS server";
    return false;
  }
  return true;
}

namespace pm::services {

std::string resolve_cms_server_base(const std::string& cli_override)
{
  if (!cli_override.empty())
    return trim_slash(cli_override);
  std::string pinned;
  std::string err;
  if (pm_zitadel_oauth_read_cms_server_base(pinned, err) && !pinned.empty())
    return pinned;
  return trim_slash(pm::k_tanit_service_server_base_default_u8);
}

} // namespace pm::services

namespace media::runtime_settings {

bool append_cms_history_entry(const nlohmann::json&, std::string& err, size_t)
{
  err.clear();
  return true;
}

} // namespace media::runtime_settings

namespace pm::android {
namespace {

bool open_session(const std::string& profile_dir, pm::cms::CmsSession& session, std::string& err)
{
  pm::cms::install_host_hooks();
  media::portable_settings::set_profile_dir_override(profile_dir);
  if (pm::cms::resolve_cms_session("", session, err))
    return true;
  if (err.find("SERVER_URL") == std::string::npos)
    return false;
  err.clear();
  return pm::cms::resolve_cms_session(pm::k_tanit_service_server_base_default_u8, session, err);
}

bool remote_target(const std::string& mount,
                   const std::string& path,
                   bool allow_empty,
                   std::string& use_mount,
                   std::string& clean,
                   std::string& err)
{
  use_mount = mount.empty() ? "home" : mount;
  if (!pm::vfs::mount_name_ok(use_mount)) {
    err = "unknown mount";
    return false;
  }
  pm::vfs::Error verr;
  clean = pm::vfs::sanitize_subpath(path, verr);
  if (verr || (!allow_empty && clean.empty())) {
    err = "path escapes the shared folder";
    return false;
  }
  return true;
}

std::string cache_scope(const std::string& profile_dir, const std::string& server)
{
  nlohmann::json bundle;
  std::string load_err;
  std::string account;
  if (pm::android::oauth_load(profile_dir, bundle, load_err) &&
      bundle.contains("app_user_id") && bundle["app_user_id"].is_string())
    account = bundle["app_user_id"].get<std::string>();
  return server + "\n" + account;
}

fs::path cached_file(const std::string& profile_dir,
                     const pm::cms::CmsSession& session,
                     const fs::path& root,
                     const std::string& mount,
                     const std::string& path,
                     std::string& err)
{
  pm::vfs::Error verr;
  const std::string name = pm::vfs::object_filename(
      cache_scope(profile_dir, session.base), mount, path);
  const fs::path dest = pm::vfs::confine_local(root, name, verr);
  if (verr || dest.empty()) {
    err = "cache path is invalid";
    return {};
  }
  return dest;
}

std::shared_ptr<std::mutex> mutex_for(const fs::path& dest)
{
  static std::mutex gate;
  static std::unordered_map<std::string, std::shared_ptr<std::mutex>> locks;
  std::lock_guard<std::mutex> hold(gate);
  auto& found = locks[media::path::path_to_generic_utf8(dest)];
  if (!found)
    found = std::make_shared<std::mutex>();
  return found;
}

struct CachedFileLock {
  std::shared_ptr<std::mutex> slot;
  std::unique_lock<std::mutex> hold;
  explicit CachedFileLock(const fs::path& path)
      : slot(mutex_for(path)), hold(*slot) {}
};

bool pull_into(const pm::cms::CmsSession& session,
               const std::string& mount,
               const std::string& path,
               const fs::path& dest,
               std::int64_t expected_size,
               const std::function<bool()>& cancelled,
               std::uint64_t& bytes,
               std::string& err)
{
  const CachedFileLock busy(dest);
  std::error_code ec;
  fs::create_directories(dest.parent_path(), ec);
  if (ec) {
    err = "could not create the cache directory";
    return false;
  }
  const fs::path incoming = pm::vfs::make_cache_temp(dest.parent_path());
  pm::cms::VfsDownloadOptions options;
  options.bearer_token = session.bearer;
  options.overwrite = true;
  options.skip_existing = false;
  options.keep_partial = false;
  options.retries = 0;
  options.timeout_ms = 120'000;
  options.connect_timeout_ms = 5'000;
  options.expected_size = expected_size;
  options.cancel_check = cancelled;
  const auto downloaded = pm::cms::download_vfs_file_to_path(
      session.base, mount, path, incoming, std::move(options));
  if (!downloaded.ok) {
    err = downloaded.err.empty() ? "Tanit VFS download failed" : downloaded.err;
    pm::vfs::discard_incoming(incoming);
    return false;
  }
  if (expected_size >= 0 &&
      downloaded.bytes_written != static_cast<std::uint64_t>(expected_size)) {
    pm::vfs::discard_incoming(incoming);
    err = "incomplete transfer";
    return false;
  }
  pm::vfs::Error verr;
  if (!pm::vfs::publish_local(incoming, dest, verr)) {
    pm::vfs::discard_incoming(incoming);
    err = verr.message.empty() ? "could not publish the downloaded file" : verr.message;
    return false;
  }
  bytes = downloaded.bytes_written;
  return true;
}

} // namespace

nlohmann::json service_vfs_mounts(const std::string& profile_dir, std::string& err)
{
  pm::cms::CmsSession session;
  if (!open_session(profile_dir, session, err))
    return {};
  const auto listed = pm::cms::list_vfs_mounts(session.base, session.bearer);
  if (!listed.ok) {
    err = listed.err.empty() ? "Tanit VFS mounts failed" : listed.err;
    return {};
  }
  if (!listed.json.is_array()) {
    err = "Tanit VFS mounts were not a list";
    return {};
  }
  std::vector<std::string> names;
  for (const auto& row : listed.json) {
    if (!row.is_object())
      continue;
    const std::string name = row.value("name", "");
    if (!pm::vfs::mount_name_ok(name))
      continue;
    if (std::find(names.begin(), names.end(), name) != names.end())
      continue;
    names.push_back(name);
  }
  std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
    auto lower = [](std::string text) {
      for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return text;
    };
    return lower(a) < lower(b);
  });
  const auto home = std::find(names.begin(), names.end(), "home");
  if (home != names.end() && home != names.begin())
    std::rotate(names.begin(), home, home + 1);
  nlohmann::json mounts = nlohmann::json::array();
  for (const auto& name : names)
    mounts.push_back({{"name", name}});
  return {{"server", session.base}, {"mounts", std::move(mounts)}};
}

nlohmann::json service_vfs_list(const std::string& profile_dir,
                                const std::string& mount,
                                const std::string& path,
                                std::string& err)
{
  std::string use_mount;
  std::string clean;
  if (!remote_target(mount, path, true, use_mount, clean, err))
    return {};
  pm::cms::CmsSession session;
  if (!open_session(profile_dir, session, err))
    return {};
  const auto listed = pm::cms::list_vfs_files(
      session.base, session.bearer, use_mount, clean, true);
  if (!listed.ok) {
    err = listed.err.empty() ? "Tanit VFS list failed" : listed.err;
    return {};
  }
  nlohmann::json entries = nlohmann::json::array();
  for (const auto& item : pm::cms::normalize_vfs_listing(listed.json)) {
    entries.push_back({
        {"name", item.name},
        {"path", item.path},
        {"mime", item.mime},
        {"size", item.size},
        {"mtime", item.mtime_ms},
        {"is_directory", item.is_directory},
    });
  }
  return {
      {"mount", use_mount},
      {"path", clean},
      {"server", session.base},
      {"entries", std::move(entries)},
  };
}

nlohmann::json service_vfs_stage(const std::string& profile_dir,
                                 const std::string& mount,
                                 const std::string& path,
                                 std::string& err)
{
  std::string use_mount;
  std::string clean;
  if (!remote_target(mount, path, false, use_mount, clean, err)) {
    if (path.empty())
      err = "Choose a file inside Tanit VFS";
    return {};
  }
  pm::cms::CmsSession session;
  if (!open_session(profile_dir, session, err))
    return {};
  const fs::path root = pm::vfs::workspace_root(media::path::path_from_utf8(profile_dir));
  std::error_code ec;
  fs::create_directories(root, ec);
  if (ec) {
    err = "could not create the VFS workspace";
    return {};
  }
  const fs::path dest = cached_file(profile_dir, session, root, use_mount, clean, err);
  if (dest.empty())
    return {};
  std::uint64_t bytes = 0;
  if (!pull_into(session, use_mount, clean, dest, -1, {}, bytes, err))
    return {};
  return {
      {"mount", use_mount},
      {"path", clean},
      {"local_path", media::path::path_to_utf8(dest)},
      {"label", safe_leaf(clean)},
  };
}

nlohmann::json service_vfs_cache_name(const std::string& profile_dir,
                                      const std::string& mount,
                                      const std::string& path,
                                      std::string& err)
{
  std::string use_mount;
  std::string clean;
  if (!remote_target(mount, path, false, use_mount, clean, err))
    return {};
  pm::cms::CmsSession session;
  if (!open_session(profile_dir, session, err))
    return {};
  const std::string name = pm::vfs::object_filename(
      cache_scope(profile_dir, session.base), use_mount, clean);
  if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
    err = "cache path is invalid";
    return {};
  }
  return {{"name", name}, {"mount", use_mount}, {"path", clean}};
}

nlohmann::json service_vfs_fetch(const std::string& profile_dir,
                                 const std::string& mount,
                                 const std::string& path,
                                 std::int64_t expected_size,
                                 const std::function<bool()>& cancelled,
                                 std::string& err)
{
  std::string use_mount;
  std::string clean;
  if (!remote_target(mount, path, false, use_mount, clean, err))
    return {};
  const fs::path root = pm::vfs::cache_root(media::path::path_from_utf8(profile_dir));
  std::error_code ec;
  fs::create_directories(root, ec);
  if (ec) {
    err = "cache path is invalid";
    return {};
  }
  pm::cms::CmsSession session;
  if (!open_session(profile_dir, session, err))
    return {};
  const fs::path dest = cached_file(profile_dir, session, root, use_mount, clean, err);
  if (dest.empty())
    return {};
  std::uint64_t bytes = 0;
  if (!pull_into(session, use_mount, clean, dest, expected_size, cancelled, bytes, err))
    return {};
  return {
      {"mount", use_mount},
      {"path", clean},
      {"local_path", media::path::path_to_utf8(dest)},
      {"bytes", bytes},
  };
}

nlohmann::json service_vfs_upload(const std::string& profile_dir,
                                  const std::string& mount,
                                  const std::string& path,
                                  const std::string& cache_key,
                                  std::string& err)
{
  std::string use_mount;
  std::string clean;
  if (!remote_target(mount, path, false, use_mount, clean, err))
    return {};
  const fs::path root = pm::vfs::cache_root(media::path::path_from_utf8(profile_dir));
  pm::vfs::Error verr;
  const fs::path local = pm::vfs::confine_local(root, cache_key, verr);
  if (verr || local.empty()) {
    err = "local file is invalid";
    return {};
  }
  std::error_code ec;
  if (!fs::is_regular_file(local, ec) || ec) {
    err = "local file is invalid";
    return {};
  }
  (void)fs::file_size(local, ec);
  if (ec) {
    err = "local file is invalid";
    return {};
  }
  pm::cms::CmsSession session;
  if (!open_session(profile_dir, session, err))
    return {};
  const auto uploaded = pm::cms::upload_vfs_file(
      session.base, session.bearer, local, use_mount, clean);
  if (!uploaded.ok) {
    err = uploaded.err.empty() ? "Tanit VFS upload failed" : uploaded.err;
    return {};
  }
  return {{"mount", uploaded.mount}, {"path", uploaded.path}};
}

nlohmann::json service_vfs_mkdir(const std::string& profile_dir,
                                 const std::string& mount,
                                 const std::string& path,
                                 std::string& err)
{
  std::string use_mount;
  std::string clean;
  if (!remote_target(mount, path, false, use_mount, clean, err))
    return {};
  pm::cms::CmsSession session;
  if (!open_session(profile_dir, session, err))
    return {};
  const std::string base = pm::cms::trim_trailing_slash(session.base);
  const std::string rel = pm::cms::encode_vfs_relative_path(clean);
  const std::string mounted =
      "/api/vfs/mkdir/" + pm::cms::percent_encode_path_segment(use_mount) + "/" + rel;
  long http = 0;
  std::string body;
  std::string mk_err;
  if (!pm::cms::http_post_json(base, mounted, session.bearer, "{}", http, body, mk_err)) {
    err = mk_err.empty() ? "Tanit VFS mkdir failed" : mk_err;
    return {};
  }
  if (http == 404 && use_mount == "home") {
    long home_http = 0;
    std::string home_body;
    std::string home_err;
    if (pm::cms::http_post_json(base, "/api/vfs/mkdir/" + rel, session.bearer, "{}", home_http, home_body, home_err) &&
        home_http >= 200 && home_http < 300) {
      return {{"mount", use_mount}, {"path", clean}};
    }
  }
  if (http < 200 || http >= 300) {
    err = body.empty() ? "Tanit VFS mkdir failed" : body.substr(0, 240);
    return {};
  }
  return {{"mount", use_mount}, {"path", clean}};
}

nlohmann::json service_vfs_delete(const std::string& profile_dir,
                                  const std::string& mount,
                                  const std::string& path,
                                  std::string& err)
{
  std::string use_mount;
  std::string clean;
  if (!remote_target(mount, path, false, use_mount, clean, err))
    return {};
  pm::cms::CmsSession session;
  if (!open_session(profile_dir, session, err))
    return {};
  const auto deleted = pm::cms::delete_vfs_path(session.base, session.bearer, use_mount, clean);
  const bool gone = deleted.http_status == 404 ||
                    (deleted.http_status >= 200 && deleted.http_status < 300);
  if (!deleted.ok || !gone) {
    err = deleted.err.empty() ? "Tanit VFS delete failed" : deleted.err;
    return {};
  }
  return {{"mount", use_mount}, {"path", clean}};
}

nlohmann::json service_vfs_move(const std::string& profile_dir,
                                const std::string& mount,
                                const std::string& path,
                                const std::string& to_path,
                                std::string& err)
{
  std::string use_mount;
  std::string clean;
  if (!remote_target(mount, path, false, use_mount, clean, err))
    return {};
  std::string dest_mount;
  std::string dest_clean;
  if (!remote_target(use_mount, to_path, false, dest_mount, dest_clean, err))
    return {};
  pm::cms::CmsSession session;
  if (!open_session(profile_dir, session, err))
    return {};
  const nlohmann::json body = {
      {"operation", "move"},
      {"conflictStrategy", "error"},
      {"excludeDefault", false},
      {"source", {{"mount", use_mount}, {"path", clean}}},
      {"destination", {{"mount", use_mount}, {"path", dest_clean}}},
  };
  const std::string base = pm::cms::trim_trailing_slash(session.base);
  long http = 0;
  std::string response;
  std::string move_err;
  if (!pm::cms::http_post_json(base, "/api/vfs/copy", session.bearer, body.dump(), http, response, move_err)) {
    err = move_err.empty() ? "Tanit VFS move failed" : move_err;
    return {};
  }
  if (http == 409) {
    err = "already exists";
    return {};
  }
  if (http < 200 || http >= 300) {
    err = response.empty() ? "Tanit VFS move failed" : response.substr(0, 240);
    return {};
  }
  return {{"mount", use_mount}, {"path", dest_clean}};
}

namespace {

std::string json_string(const nlohmann::json& row, const char* key, const char* alt = nullptr)
{
  auto pick = [&](const char* name) -> std::string {
    if (!name || !row.contains(name) || row[name].is_null())
      return {};
    if (row[name].is_string())
      return row[name].get<std::string>();
    if (row[name].is_number())
      return row[name].dump();
    return {};
  };
  std::string value = pick(key);
  if (value.empty() && alt)
    value = pick(alt);
  return value;
}

double json_number(const nlohmann::json& row, const char* key, const char* alt = nullptr)
{
  auto pick = [&](const char* name) -> double {
    if (!name || !row.contains(name) || row[name].is_null())
      return 0.0;
    if (row[name].is_number())
      return row[name].get<double>();
    if (row[name].is_string()) {
      try {
        return std::stod(row[name].get<std::string>());
      } catch (...) {
      }
    }
    return 0.0;
  };
  if (row.contains(key) && !row[key].is_null())
    return pick(key);
  return pick(alt);
}

nlohmann::json slim_spend_row(const nlohmann::json& row)
{
  return {
      {"startTime", json_string(row, "startTime", "start_time")},
      {"model", json_string(row, "model")},
      {"spend", json_number(row, "spend")},
      {"totalTokens", static_cast<int>(json_number(row, "totalTokens", "total_tokens"))},
  };
}

nlohmann::json rows_from_llm(const std::vector<pm::service::LlmSpendLogEntry>& entries)
{
  nlohmann::json out = nlohmann::json::array();
  for (const auto& entry : entries) {
    out.push_back({
        {"startTime", entry.start_time},
        {"model", entry.model},
        {"spend", entry.spend},
        {"totalTokens", entry.total_tokens},
    });
  }
  return out;
}

} // namespace

nlohmann::json service_spend(const std::string& profile_dir, std::string& err)
{
  pm::cms::CmsSession session;
  if (!open_session(profile_dir, session, err))
    return {};

  const auto spend = pm::cms::get_ai_gateway_me_spend(session.base, session.bearer);
  if (!spend.ok) {
    err = spend.err.empty() ? "Couldn't load Tanit spending" : spend.err;
    return {};
  }

  pm::cms::AiGatewaySpendLogsQuery query;
  query.days_back = 7;
  query.page = 1;
  query.page_size = 8;
  const auto logs = pm::cms::get_ai_gateway_me_spend_logs(session.base, session.bearer, query);
  nlohmann::json entries = nlohmann::json::array();
  std::string source = "gateway";
  if (logs.ok && logs.json.contains("entries") && logs.json["entries"].is_array() &&
      !logs.json["entries"].empty()) {
    for (const auto& row : logs.json["entries"]) {
      if (row.is_object())
        entries.push_back(slim_spend_row(row));
    }
  } else {
    pm::service::LlmSpendLogsQuery llm_query;
    llm_query.days_back = 7;
    llm_query.page = 1;
    llm_query.page_size = 8;
    pm::service::LlmSpendLogsPage page;
    std::string llm_err;
    if (pm::service::get_llm_spend_logs_page(session.bearer, "", llm_query, page, llm_err)) {
      source = "logs";
      entries = rows_from_llm(page.entries);
    }
  }

  nlohmann::json out = {
      {"spend", json_number(spend.json, "spend")},
      {"currency", json_string(spend.json, "currency").empty() ? "usd" : json_string(spend.json, "currency")},
      {"budgetDuration", json_string(spend.json, "budgetDuration", "budget_duration")},
      {"source", source},
      {"entries", std::move(entries)},
  };
  if (spend.json.contains("maxBudget") && spend.json["maxBudget"].is_number())
    out["maxBudget"] = spend.json["maxBudget"].get<double>();
  else if (spend.json.contains("max_budget") && spend.json["max_budget"].is_number())
    out["maxBudget"] = spend.json["max_budget"].get<double>();
  return out;
}

nlohmann::json service_post_create(const std::string& profile_dir,
                                   const std::string& image_path,
                                   const std::string& title,
                                   const std::string& description,
                                   std::string& err)
{
  pm::cms::CmsSession session;
  if (!open_session(profile_dir, session, err))
    return {};
  const fs::path file(image_path);
  if (!fs::is_regular_file(file)) {
    err = "image file not found";
    return {};
  }
  const auto created = pm::cms::create_post_with_pictures(
      session.base, session.bearer, {file}, title, description, "public");
  if (!created.ok) {
    err = created.err.empty() ? "Couldn't publish post" : created.err;
    return {};
  }
  const std::string url =
      pm::cms::build_post_compact_view_url(session.base, created.post_id, created.picture_ids);
  const std::string picture = created.picture_ids.empty() ? std::string{} : created.picture_ids.front();
  return {
      {"post_id", created.post_id},
      {"picture_id", picture},
      {"url", url},
  };
}

nlohmann::json service_files_upload(const std::string& profile_dir,
                                    const std::string& local_path,
                                    std::string& err)
{
  pm::cms::CmsSession session;
  if (!open_session(profile_dir, session, err))
    return {};
  const fs::path file = media::path::path_from_utf8(local_path);
  if (!fs::is_regular_file(file)) {
    err = "file not found";
    return {};
  }
  std::string leaf = safe_leaf(local_path);
  for (char& c : leaf) {
    if (c == ' ')
      c = '_';
  }
  const std::string remote = std::string("public/") + leaf;
  const pm::cms::VfsUploadResult uploaded = pm::cms::upload_vfs_file(
      session.base, session.bearer, file, "home", remote);
  if (!uploaded.ok) {
    err = uploaded.err.empty() ? "Couldn't publish file" : uploaded.err;
    return {};
  }
  return {
      {"mount", uploaded.mount},
      {"vfs_path", uploaded.path.empty() ? remote : uploaded.path},
      {"url", uploaded.browser_url},
  };
}

nlohmann::json service_page_create(const std::string& profile_dir,
                                   const std::string& markdown,
                                   const std::string& title,
                                   std::string& err)
{
  if (markdown.empty()) {
    err = "Nothing to publish";
    return {};
  }
  pm::cms::CmsSession session;
  if (!open_session(profile_dir, session, err))
    return {};
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  pm::cms::MarkdownPageOptions opt;
  opt.title = title.empty() ? "Tanit" : title;
  opt.slug = "tanit-" + std::to_string(ms);
  opt.use_frontmatter = false;
  opt.include_images = false;
  opt.is_public = true;
  opt.is_public_cli_set = true;
  opt.visible = true;
  opt.visible_cli_set = true;
  const auto created = pm::cms::create_markdown_page(session.base, session.bearer, markdown, std::move(opt));
  if (!created.ok) {
    err = created.err.empty() ? "Couldn't publish page" : created.err;
    return {};
  }
  return {
      {"page_id", created.page_id},
      {"slug", created.slug},
      {"url", created.url},
  };
}

} // namespace pm::android

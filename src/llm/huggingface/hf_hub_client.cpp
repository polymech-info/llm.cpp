#include "llm/huggingface/hf_hub_client.hpp"

#include "cms/pm_cms_http.hpp"
#include "core/command_variables.hpp"
#include "core/settings_runtime.hpp"
#include "string/string_utils.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include "fs/path_sanitizer.hpp"

namespace fs = std::filesystem;

namespace pm::hf {
namespace {

std::once_flag g_curl_once;

void ensure_curl_global_init()
{
    std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t curl_write_string(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string encode_repo_id(const std::string& repo_id)
{
    std::string out;
    out.reserve(repo_id.size());
    for (unsigned char c : repo_id) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
            out.push_back(static_cast<char>(c));
        else {
            static constexpr char k_hex[] = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(k_hex[(c >> 4) & 0x0F]);
            out.push_back(k_hex[c & 0x0F]);
        }
    }
    return out;
}

std::uint64_t json_uint64(const nlohmann::json& j)
{
    if (j.is_number_unsigned())
        return j.get<std::uint64_t>();
    if (j.is_number_integer() && j.get<long long>() >= 0)
        return static_cast<std::uint64_t>(j.get<long long>());
    return 0;
}

RepoFileInfo sibling_to_file(const nlohmann::json& item)
{
    RepoFileInfo f;
    if (item.contains("rfilename") && item["rfilename"].is_string())
        f.path = f.name = item["rfilename"].get<std::string>();
    else if (item.contains("path") && item["path"].is_string())
        f.path = f.name = item["path"].get<std::string>();
    if (item.contains("size"))
        f.size_bytes = json_uint64(item["size"]);
    if (item.contains("lfs") && item["lfs"].is_object()) {
        f.lfs = true;
        if (const auto lfs_sz = json_uint64(item["lfs"]["size"]); lfs_sz > 0)
            f.size_bytes = lfs_sz;
    }
    return f;
}

void walk_tree_items(const nlohmann::json& node, std::vector<RepoFileInfo>& out)
{
    if (!node.is_array())
        return;
    for (const auto& item : node) {
        if (!item.is_object())
            continue;
        const std::string type = item.value("type", std::string{});
        if (type == "directory" || type == "dir")
            continue;
        const std::string path = item.value("path", item.value("rfilename", std::string{}));
        if (path.empty())
            continue;
        RepoFileInfo f;
        f.path = f.name = path;
        if (item.contains("size"))
            f.size_bytes = json_uint64(item["size"]);
        if (item.contains("lfs") && item["lfs"].is_object()) {
            f.lfs = true;
            if (const auto lfs_sz = json_uint64(item["lfs"]["size"]); lfs_sz > 0)
                f.size_bytes = lfs_sz;
        }
        if (item.contains("xetHash") && item["xetHash"].is_string())
            f.xet_hash = item["xetHash"].get<std::string>();
        out.push_back(std::move(f));
    }
}

void merge_tree_metadata(std::vector<RepoFileInfo>& files, const nlohmann::json& tree)
{
    std::vector<RepoFileInfo> tree_files;
    walk_tree_items(tree, tree_files);
    for (auto& f : files) {
        const std::string leaf = media::path::path_to_utf8(media::path::path_from_utf8(f.path).filename());
        for (const auto& t : tree_files) {
            if (t.path == f.path || media::path::path_to_utf8(media::path::path_from_utf8(t.path).filename()) == leaf) {
                if (f.size_bytes == 0 && t.size_bytes > 0)
                    f.size_bytes = t.size_bytes;
                if (t.lfs)
                    f.lfs = true;
                if (!t.xet_hash.empty())
                    f.xet_hash = t.xet_hash;
                break;
            }
        }
    }
}

bool files_need_tree_enrichment(const std::vector<RepoFileInfo>& files)
{
    for (const auto& f : files) {
        if (f.size_bytes == 0 || f.xet_hash.empty())
            return true;
    }
    return false;
}

HubRequestResult http_get_url(const std::string& url, const std::string& token, const char* method = "GET",
                              const std::string& range_header = {}, bool parse_json = true)
{
    HubRequestResult out;
    ensure_curl_global_init();

    CURL* curl = curl_easy_init();
    if (!curl) {
        out.err = "curl_easy_init failed";
        return out;
    }

    struct curl_slist* hdr = nullptr;
    if (!token.empty())
        hdr = curl_slist_append(hdr, ("Authorization: Bearer " + token).c_str());
    if (!range_header.empty())
        hdr = curl_slist_append(hdr, ("Range: " + range_header).c_str());

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (method && method[0] != '\0' && std::string(method) != "GET")
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    if (hdr)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, parse_json ? 60L : 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    const CURLcode cc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.http_status);
    if (hdr)
        curl_slist_free_all(hdr);
    curl_easy_cleanup(curl);

    out.body = std::move(body);
    if (cc != CURLE_OK) {
        out.err = curl_easy_strerror(cc);
        return out;
    }
    if (out.http_status < 200 || out.http_status >= 300) {
        out.err = "HTTP " + std::to_string(out.http_status);
        return out;
    }
    if (!parse_json)
        out.ok = true;
    else if (!out.body.empty()) {
        try {
            out.json = nlohmann::json::parse(out.body);
            out.ok   = true;
        } catch (const std::exception& e) {
            out.err = std::string("invalid JSON: ") + e.what();
            return out;
        }
    } else {
        out.ok = true;
    }
    return out;
}

} // namespace

std::string resolve_hf_token(const std::string& override_token)
{
    if (!override_token.empty())
        return override_token;
    if (const auto user = media::commands::user_variable_value("HF_TOKEN")) {
        const std::string trimmed = media::strutil::trim(*user);
        if (!trimmed.empty())
            return trimmed;
    }
    std::string provider_key;
    std::string ignored_url;
    media::runtime_settings::merge_provider_credentials("huggingface", false, provider_key, ignored_url);
    if (!provider_key.empty())
        return provider_key;
    return pm::cms::getenv_trimmed("HF_TOKEN");
}

std::string hf_api_base()
{
    return "https://huggingface.co";
}

std::string build_resolve_url(const std::string& repo_id, const std::string& filename)
{
    return hf_api_base() + "/" + encode_repo_id(repo_id) + "/resolve/main/" + filename;
}

HubRequestResult hub_get_json(const std::string& path, const std::string& token)
{
    const std::string url = path.rfind("http", 0) == 0 ? path : (hf_api_base() + path);
    return http_get_url(url, token);
}

HubRequestResult fetch_model_info(const std::string& repo_id, const std::string& token)
{
    return hub_get_json("/api/models/" + encode_repo_id(repo_id), token);
}

HubRequestResult fetch_model_readme(const std::string& repo_id, const std::string& token)
{
    const std::string tok = token.empty() ? resolve_hf_token() : token;
    HubRequestResult last;
    for (const char* rev : {"main", "master"}) {
        const std::string url =
            hf_api_base() + "/" + encode_repo_id(repo_id) + "/raw/" + rev + "/README.md";
        last = http_get_url(url, tok, "GET", {}, false);
        if (last.ok && !last.body.empty())
            return last;
    }
    return last;
}

// Same expand set Unsloth's Model Hub listing uses so search rows carry tags,
// dates, library, and param totals without a second model_info round-trip.
const char* hub_list_expand()
{
    return "&cardData=true&expand=safetensors&expand=tags&expand=library_name"
           "&expand=createdAt&expand=downloadsAllTime&expand=gguf";
}

HubRequestResult fetch_org_models(const std::string& org, const std::string& token, int limit)
{
    const int capped = std::clamp(limit, 1, 500);
    return hub_get_json("/api/models?author=" + encode_repo_id(org) + "&limit=" + std::to_string(capped)
                            + hub_list_expand(),
                        token);
}

HubRequestResult fetch_search_models(const std::string& query, const std::string& token, int limit,
                                     bool gguf_only)
{
    const int capped = std::clamp(limit, 1, 100);
    std::string path = "/api/models?search=" + encode_repo_id(query)
        + "&sort=downloads&direction=-1&limit=" + std::to_string(capped)
        + hub_list_expand();
    if (gguf_only)
        path += "&filter=gguf";
    return hub_get_json(path, token);
}

HubRequestResult fetch_popular_gguf_models(int limit, const std::string& token)
{
    const int capped = std::clamp(limit, 1, 100);
    return hub_get_json(
        "/api/models?filter=gguf&pipeline_tag=text-generation&sort=downloads&direction=-1&limit="
            + std::to_string(capped) + hub_list_expand(),
        token);
}

std::vector<RepoFileInfo> list_repo_files(const std::string& repo_id, const std::string& token,
                                          std::string* err_out)
{
    if (err_out)
        err_out->clear();

    const HubRequestResult info = fetch_model_info(repo_id, token);
    std::vector<RepoFileInfo> files;
    if (info.ok && info.json.contains("siblings") && info.json["siblings"].is_array()) {
        for (const auto& sib : info.json["siblings"]) {
            if (!sib.is_object())
                continue;
            auto f = sibling_to_file(sib);
            if (!f.path.empty())
                files.push_back(std::move(f));
        }
    }

    if (files_need_tree_enrichment(files)) {
        const HubRequestResult tree =
            hub_get_json("/api/models/" + encode_repo_id(repo_id) + "/tree/main?recursive=1", token);
        if (tree.ok)
            merge_tree_metadata(files, tree.json);
    }

    if (!files.empty())
        return files;

    const HubRequestResult tree =
        hub_get_json("/api/models/" + encode_repo_id(repo_id) + "/tree/main?recursive=1", token);
    if (!tree.ok) {
        if (err_out) {
            if (!info.ok && !info.err.empty())
                *err_out = info.err;
            else
                *err_out = tree.err.empty() ? "failed to list repo files" : tree.err;
        }
        return {};
    }
    walk_tree_items(tree.json, files);
    if (files.empty() && err_out)
        *err_out = "repo has no listed files";
    return files;
}

HubRequestResult probe_remote_file(const std::string& url, const std::string& token)
{
    return http_get_url(url, token, "HEAD");
}

HubRequestResult hub_http_get_bytes(const std::string& url, const std::string& bearer_token,
                                    const std::string& range_header)
{
    return http_get_url(url, bearer_token, "GET", range_header, /*parse_json=*/false);
}

std::optional<XetReadToken> fetch_xet_read_token(const std::string& repo_id, const std::string& revision,
                                                 const std::string& hf_token)
{
    if (repo_id.empty() || revision.empty())
        return std::nullopt;
    const HubRequestResult res = hub_get_json(
        "/api/models/" + encode_repo_id(repo_id) + "/xet-read-token/" + encode_repo_id(revision), hf_token);
    if (!res.ok || !res.json.is_object())
        return std::nullopt;
    XetReadToken out;
    out.cas_url       = res.json.value("casUrl", std::string{});
    out.access_token  = res.json.value("accessToken", std::string{});
    out.exp_unix      = res.json.value("exp", static_cast<std::int64_t>(0));
    if (out.cas_url.empty() || out.access_token.empty())
        return std::nullopt;
    return out;
}

std::string fetch_model_revision(const std::string& repo_id, const std::string& hf_token)
{
    const HubRequestResult info = fetch_model_info(repo_id, hf_token);
    if (!info.ok || !info.json.is_object())
        return {};
    return info.json.value("sha", std::string{});
}

} // namespace pm::hf

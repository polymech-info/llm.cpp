#include "llm/huggingface/hf_download.hpp"

#include "llm/huggingface/hf_catalog.hpp"
#include "llm/huggingface/hf_gguf.hpp"
#include "llm/huggingface/hf_hub_client.hpp"
#include "llm/huggingface/hf_manifest.hpp"
#include "llm/huggingface/hf_meta.hpp"
#include "llm/huggingface/hf_mmproj.hpp"
#include "llm/huggingface/hf_ref.hpp"
#include "llm/huggingface/hf_sidecar.hpp"
#include "llm/huggingface/hf_stream_download.hpp"
#include "llm/huggingface/hf_xet_download.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include "fs/path_sanitizer.hpp"

namespace fs = std::filesystem;

namespace pm::hf {
namespace {

std::string trim_ascii_copy(std::string s)
{
    auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](char c) {
        return !is_ws(static_cast<unsigned char>(c));
    }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [&](char c) {
        return !is_ws(static_cast<unsigned char>(c));
    }).base(), s.end());
    return s;
}

std::string card_description(const nlohmann::json& item)
{
    if (item.contains("cardData") && item["cardData"].is_object()) {
        const auto desc = item["cardData"].value("description", std::string{});
        if (!desc.empty())
            return desc;
    }
    return item.value("description", std::string{});
}

std::string strip_readme_frontmatter(std::string body)
{
    if (body.size() >= 3
        && static_cast<unsigned char>(body[0]) == 0xEF
        && static_cast<unsigned char>(body[1]) == 0xBB
        && static_cast<unsigned char>(body[2]) == 0xBF)
        body.erase(0, 3);
    if (body.rfind("---", 0) != 0)
        return body;
    const auto end = body.find("\n---", 3);
    if (end == std::string::npos)
        return body;
    const auto rest = body.find('\n', end + 4);
    if (rest == std::string::npos)
        return {};
    return body.substr(rest + 1);
}

std::string clip_readme(std::string body, std::size_t max = 8000)
{
    body = strip_readme_frontmatter(std::move(body));
    auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    body.erase(body.begin(), std::find_if(body.begin(), body.end(), [&](char c) {
        return !is_ws(static_cast<unsigned char>(c));
    }));
    if (body.size() <= max)
        return body;
    return body.substr(0, max) + "\n…";
}

nlohmann::json hub_list_row(const nlohmann::json& item, const std::string& fallback_author)
{
    nlohmann::json row = {
        {"id", item.value("id", item.value("modelId", std::string{}))},
        {"repoId", item.value("id", item.value("modelId", std::string{}))},
        {"author", item.value("author", fallback_author)},
        {"pipelineTag", item.value("pipeline_tag", std::string{})},
        {"downloads", item.value("downloads", 0)},
        {"likes", item.value("likes", 0)},
        {"tags", item.value("tags", nlohmann::json::array())},
        {"library_name", item.value("library_name", std::string{})},
        {"lastModified", item.value("lastModified", std::string{})},
        {"createdAt", item.value("createdAt", std::string{})},
        {"private", item.value("private", false)},
        {"gated", item.value("gated", false)},
        {"description", card_description(item)},
    };
    if (item.contains("safetensors"))
        row["safetensors"] = item["safetensors"];
    if (item.contains("gguf"))
        row["gguf"] = item["gguf"];
    if (item.contains("downloadsAllTime"))
        row["downloadsAllTime"] = item["downloadsAllTime"];
    if (item.contains("cardData") && item["cardData"].is_object())
        row["cardData"] = item["cardData"];
    if (row.value("description", std::string{}).empty()) {
        const std::string repo_id = row.value("repoId", std::string{});
        if (const auto cat = resolve_model_ref(repo_id)) {
            if (!cat->description.empty())
                row["description"] = cat->description;
        }
    }
    if (item.contains("siblings") && item["siblings"].is_array()) {
        nlohmann::json files = nlohmann::json::array();
        for (const auto& sib : item["siblings"]) {
            if (!sib.is_object())
                continue;
            files.push_back({
                {"name", sib.value("rfilename", std::string{})},
                {"bytes", sib.value("size", 0)},
            });
        }
        row["files"] = std::move(files);
    }
    return row;
}

nlohmann::json parse_hub_model_list(const nlohmann::json& listing, const std::string& fallback_author)
{
    nlohmann::json models = nlohmann::json::array();
    if (!listing.is_array())
        return models;
    for (const auto& item : listing) {
        if (!item.is_object())
            continue;
        nlohmann::json row = hub_list_row(item, fallback_author);
        if (row.value("repoId", std::string{}).empty())
            continue;
        models.push_back(std::move(row));
    }
    return models;
}

} // namespace

ModelCardResult fetch_model_card(const std::string& alias_or_repo, const std::string& token)
{
    ModelCardResult out;
    const auto entry = resolve_model_ref(alias_or_repo);
    if (!entry) {
        out.err = "unknown model alias or repo: " + alias_or_repo;
        return out;
    }
    out.entry = *entry;

    const std::string tok = token.empty() ? resolve_hf_token() : token;
    const HubRequestResult info = fetch_model_info(out.entry.repo_id, tok);
    if (info.ok)
        out.model_info = info.json;
    else if (info.http_status != 404)
        out.err = info.err.empty() ? "model info failed" : info.err;

    std::string desc = card_description(out.model_info);
    if (desc.empty())
        desc = out.entry.description;

    const HubRequestResult readme = fetch_model_readme(out.entry.repo_id, tok);
    if (readme.ok && !readme.body.empty()) {
        out.readme = clip_readme(readme.body);
        if (desc.empty())
            desc = out.readme;
    }

    out.description = std::move(desc);
    if (out.description.empty() && !out.err.empty())
        return out;
    if (out.description.empty() && !info.ok && !readme.ok) {
        out.err = info.err.empty() ? (readme.err.empty() ? "model card failed" : readme.err) : info.err;
        return out;
    }
    out.ok = true;
    out.err.clear();
    return out;
}

ProbeResult probe_model(const std::string& alias_or_repo, const std::string& token,
                        const std::string& variant)
{
    ProbeResult out;
    const auto entry = resolve_model_ref(alias_or_repo);
    if (!entry) {
        const ParsedRef parsed = parse_hf_ref(alias_or_repo);
        if (parsed.kind == RefKind::Org)
            out.err = "organization ref requires `hg org` (e.g. hg org " + parsed.org + ")";
        else
            out.err = "unknown model alias or repo: " + alias_or_repo;
        return out;
    }
    out.entry = *entry;

    const std::string tok = token.empty() ? resolve_hf_token() : token;
    std::string list_err;
    const auto repo_files = list_repo_files(out.entry.repo_id, tok, &list_err);
    if (repo_files.empty()) {
        out.err = list_err.empty() ? "failed to list repo files" : list_err;
        return out;
    }

    out.variants      = list_gguf_variants(repo_files);
    out.variant_infos = all_variant_infos(repo_files);
    if (!variant.empty()) {
        std::string pick_err;
        if (auto info = build_variant_info(repo_files, variant, &pick_err))
            out.variant_infos = {*info};
        else if (!pick_err.empty())
            out.err = pick_err;
    }

    const HubRequestResult info = fetch_model_info(out.entry.repo_id, tok);
    if (info.ok)
        out.model_info = info.json;

    enrich_probe_result(out, repo_files);
    out.ok = out.err.empty();
    return out;
}

OrgProbeResult probe_org(const std::string& org_or_url, const std::string& token, int limit)
{
    OrgProbeResult out;
    const ParsedRef parsed = parse_hf_ref(org_or_url);
    if (parsed.kind != RefKind::Org || parsed.org.empty()) {
        out.err = "expected organization slug or URL (e.g. sentence-transformers or https://huggingface.co/sentence-transformers)";
        return out;
    }
    out.org = parsed.org;

    const std::string tok = token.empty() ? resolve_hf_token() : token;
    const HubRequestResult listing = fetch_org_models(out.org, tok, limit);
    if (!listing.ok) {
        out.err = listing.err.empty() ? "failed to list organization models" : listing.err;
        return out;
    }

    out.models = parse_hub_model_list(listing.json, out.org);
    out.ok = true;
    return out;
}

SearchProbeResult search_models(const std::string& query, const std::string& token, int limit,
                                bool gguf_only)
{
    SearchProbeResult out;
    out.query = trim_ascii_copy(query);
    out.gguf_only = gguf_only;
    if (out.query.empty()) {
        out.err = "search query is required";
        return out;
    }

    const std::string tok = token.empty() ? resolve_hf_token() : token;
    const HubRequestResult listing = fetch_search_models(out.query, tok, limit, gguf_only);
    if (!listing.ok) {
        out.err = listing.err.empty() ? "failed to search Hugging Face models" : listing.err;
        return out;
    }

    out.models = parse_hub_model_list(listing.json, {});
    out.ok = true;
    return out;
}

DownloadResult download_gguf_variant(DownloadRequest request)
{
    DownloadResult out;
    const std::string variant = request.variant.empty() ? request.entry.default_variant : request.variant;
    if (variant.empty()) {
        out.err = "variant is required (use --variant or catalog default_variant)";
        return out;
    }

    out.output_dir = resolve_output_dir(request.output_root, request.entry, variant);
    const std::string token = request.token.empty() ? resolve_hf_token() : request.token;

    if (request.remanifest_only) {
        std::error_code ec;
        if (!fs::is_directory(out.output_dir, ec)) {
            out.err = "output directory does not exist: " + media::path::path_to_utf8(out.output_dir);
            return out;
        }
        if (request.write_manifest) {
            const fs::path manifest_path = write_manifest(request.entry, out.output_dir, variant);
            out.manifest                 = build_manifest(request.entry, out.output_dir, variant);
            out.files.push_back(manifest_path);
        }
        if (request.write_sidecar) {
            const fs::path sidecar_path = rebuild_sidecar_from_dir(out.output_dir, request.entry, variant);
            if (!sidecar_path.empty()) {
                std::ifstream in(sidecar_path, std::ios::binary);
                if (in)
                    out.sidecar = nlohmann::json::parse(in);
                out.files.push_back(sidecar_path);
            }
        }
        out.ok = true;
        return out;
    }

    std::string list_err;
    const auto repo_files = list_repo_files(request.entry.repo_id, token, &list_err);
    if (repo_files.empty()) {
        out.err = list_err.empty() ? "failed to list repo files" : list_err;
        return out;
    }

    std::string pick_err;
    auto filenames = pick_gguf_filenames(repo_files, variant, &pick_err);
    if (filenames.empty()) {
        out.err = pick_err.empty() ? "no files for variant" : pick_err;
        return out;
    }

    const RepoCapabilities download_caps = detect_repo_capabilities(repo_files);
    if (download_caps.recommended_mmproj) {
        const RepoFileInfo& proj = *download_caps.recommended_mmproj;
        const std::string proj_name = proj.name.empty() ? proj.path : proj.name;
        if (!proj_name.empty()) {
            const std::string proj_leaf = media::path::path_to_utf8(media::path::path_from_utf8(proj_name).filename());
            const bool already = std::any_of(
                filenames.begin(), filenames.end(), [&](const std::string& name) {
                    return name == proj_name || media::path::path_to_utf8(media::path::path_from_utf8(name).filename()) == proj_leaf;
                });
            if (!already)
                filenames.push_back(proj_name);
        }
    }

    std::error_code ec;
    fs::create_directories(out.output_dir, ec);

    const int file_count = static_cast<int>(filenames.size());
    std::uint64_t aggregate_done = 0;
    std::uint64_t aggregate_total = 0;
    for (const auto& name : filenames) {
        for (const auto& rf : repo_files) {
            const std::string rf_name = rf.name.empty() ? rf.path : rf.name;
            if (rf_name == name || media::path::path_to_utf8(media::path::path_from_utf8(rf_name).filename()) == media::path::path_to_utf8(media::path::path_from_utf8(name).filename())) {
                aggregate_total += rf.size_bytes;
                break;
            }
        }
    }

    int file_index = 0;
    bool any_skipped = false;
    for (const auto& remote_name : filenames) {
        ++file_index;
        if (request.cancel_check && request.cancel_check()) {
            out.cancelled = true;
            out.err       = "download cancelled";
            return out;
        }

        const std::string leaf = media::path::path_to_utf8(media::path::path_from_utf8(remote_name).filename());
        const fs::path local_path = out.output_dir / leaf;
        const std::string url     = build_resolve_url(request.entry.repo_id, remote_name);

        std::int64_t expected = -1;
        std::string xet_hash;
        for (const auto& rf : repo_files) {
            const std::string rf_name = rf.name.empty() ? rf.path : rf.name;
            if (rf_name == remote_name || media::path::path_to_utf8(media::path::path_from_utf8(rf_name).filename()) == leaf) {
                if (rf.size_bytes > 0)
                    expected = static_cast<std::int64_t>(rf.size_bytes);
                xet_hash = rf.xet_hash;
                break;
            }
        }

        StreamDownloadOptions fopts;
        fopts.bearer_token  = token;
        fopts.resume        = true;
        fopts.skip_existing = request.skip_existing;
        fopts.overwrite     = request.overwrite;
        fopts.expected_size = expected;
        fopts.cancel_check  = request.cancel_check;
        fopts.pause_check   = request.pause_check;
        fopts.on_progress   = [&](const DownloadProgress& p) {
            if (!request.on_progress)
                return;
            DownloadProgress agg = p;
            agg.current_file = leaf;
            agg.file_index   = file_index;
            agg.file_count   = file_count;
            if (p.bytes_total > 0)
                agg.bytes_total = aggregate_total;
            else if (aggregate_total > 0)
                agg.bytes_total = aggregate_total;
            agg.bytes_done = aggregate_done + p.bytes_done;
            request.on_progress(agg);
        };

        StreamDownloadResult file_result;
        if (!xet_hash.empty()) {
            XetFileDownloadRequest xreq;
            xreq.repo_id        = request.entry.repo_id;
            xreq.revision       = fetch_model_revision(request.entry.repo_id, token);
            xreq.xet_hash       = xet_hash;
            xreq.hf_token       = token;
            xreq.local_path     = local_path;
            xreq.expected_size  = expected;
            xreq.options        = fopts;
            file_result         = xet_download_file(std::move(xreq));
        } else {
            file_result = stream_download_url(url, local_path, std::move(fopts));
        }
        if (file_result.cancelled) {
            out.cancelled = true;
            out.err       = file_result.err.empty() ? "download cancelled" : file_result.err;
            return out;
        }
        if (!file_result.ok) {
            out.err = file_result.err.empty() ? "download failed" : file_result.err;
            return out;
        }
        if (file_result.skipped)
            any_skipped = true;
        aggregate_done += file_result.bytes_written;
        out.files.push_back(local_path);
    }

    if (request.write_manifest) {
        const fs::path manifest_path = write_manifest(request.entry, out.output_dir, variant);
        out.manifest                 = build_manifest(request.entry, out.output_dir, variant);
        out.files.push_back(manifest_path);
    }

    if (request.write_sidecar) {
        VariantInfo weights;
        std::string pick_err;
        if (auto info = build_variant_info(repo_files, variant, &pick_err)) {
            enrich_variant_info(*info);
            weights = *info;
        }
        const GgufHeaderMeta header = read_header_for_variant_dir(out.output_dir);
        if (header.valid)
            enrich_variant_info(weights, &header);
        const RepoCapabilities caps = detect_repo_capabilities(repo_files);
        HubRequestResult model_info = fetch_model_info(request.entry.repo_id, token);
        const fs::path sidecar_path =
            write_sidecar(request.entry, out.output_dir, variant, weights, caps, header,
                          model_info.ok ? model_info.json : nlohmann::json::object());
        out.sidecar = build_sidecar(request.entry, variant, weights, caps, header,
                                    model_info.ok ? model_info.json : nlohmann::json::object());
        out.files.push_back(sidecar_path);
    }

    out.skipped = any_skipped && aggregate_done == 0;
    out.ok      = true;
    return out;
}

DownloadResult download_named_file(DownloadRequest request)
{
    DownloadResult out;
    const std::string filename = request.variant.empty()
        ? request.entry.default_variant
        : request.variant;
    if (filename.empty()) {
        out.err = "filename is required (catalog default_variant or --variant)";
        return out;
    }

    const fs::path root = request.output_root.empty()
        ? default_models_root()
        : fs::absolute(request.output_root);
    out.output_dir = (root / request.entry.store_name).lexically_normal();

    std::error_code ec;
    fs::create_directories(out.output_dir, ec);
    const fs::path local_path = out.output_dir /
        (request.entry.local_filename.empty()
             ? media::path::path_from_utf8(filename).filename()
             : media::path::path_from_utf8(request.entry.local_filename).filename());

    const std::string token = request.token.empty() ? resolve_hf_token() : request.token;
    const std::string url = !request.entry.source_url.empty()
        ? request.entry.source_url
        : build_resolve_url(request.entry.repo_id, filename);

    StreamDownloadOptions fopts;
    fopts.bearer_token  = token;
    fopts.resume        = true;
    fopts.skip_existing = request.skip_existing;
    fopts.overwrite     = request.overwrite;
    fopts.cancel_check  = request.cancel_check;
    fopts.pause_check   = request.pause_check;
    fopts.on_progress   = request.on_progress;

    const StreamDownloadResult file_result = stream_download_url(url, local_path, std::move(fopts));
    if (file_result.cancelled) {
        out.cancelled = true;
        out.err = file_result.err.empty() ? "download cancelled" : file_result.err;
        return out;
    }
    if (!file_result.ok) {
        out.err = file_result.err.empty() ? "download failed" : file_result.err;
        return out;
    }
    out.files.push_back(local_path);
    out.skipped = file_result.skipped;
    out.ok = true;
    return out;
}

} // namespace pm::hf

#include "llm/huggingface/hf_xet_download.hpp"

#include "llm/huggingface/hf_hub_client.hpp"
#include "llm/huggingface/hf_xet_xorb.hpp"

#include <chrono>
#include <fstream>
#include <thread>
#include "fs/path_sanitizer.hpp"

namespace fs = std::filesystem;

namespace pm::hf {
namespace {

struct ChunkRange {
    std::size_t start = 0;
    std::size_t end   = 0; ///< end-exclusive
};

struct ReconTerm {
    std::string xorb_hash;
    std::uint64_t unpacked_length = 0;
    ChunkRange    range;
};

struct FetchSlice {
    ChunkRange    chunk_range;
    std::string   url;
    ChunkRange    url_range; ///< byte range in xorb blob, end-inclusive for HTTP Range header
};

bool parse_chunk_range(const nlohmann::json& j, ChunkRange& out)
{
    if (!j.is_object())
        return false;
    out.start = j.value("start", static_cast<std::size_t>(0));
    out.end   = j.value("end", static_cast<std::size_t>(0));
    return out.end >= out.start;
}

bool range_contains(const ChunkRange& outer, const ChunkRange& inner)
{
    return outer.start <= inner.start && outer.end >= inner.end;
}

std::string make_range_header(const ChunkRange& r)
{
    return "bytes=" + std::to_string(r.start) + "-" + std::to_string(r.end);
}

bool token_expired_soon(const XetReadToken& tok, std::int64_t skew_sec = 30)
{
    if (tok.exp_unix <= 0)
        return false;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return now + skew_sec >= tok.exp_unix;
}

std::optional<FetchSlice> pick_fetch_slice(const nlohmann::json& fetch_arr, const ChunkRange& term_range)
{
    if (!fetch_arr.is_array())
        return std::nullopt;
    for (const auto& item : fetch_arr) {
        if (!item.is_object())
            continue;
        FetchSlice slice;
        if (!parse_chunk_range(item.value("range", nlohmann::json::object()), slice.chunk_range))
            continue;
        if (!range_contains(slice.chunk_range, term_range))
            continue;
        slice.url = item.value("url", std::string{});
        if (!parse_chunk_range(item.value("url_range", nlohmann::json::object()), slice.url_range))
            continue;
        if (slice.url.empty())
            continue;
        return slice;
    }
    return std::nullopt;
}

bool append_term_bytes(const ReconTerm& term, const FetchSlice& fetch, const XetReadToken& /*tok*/,
                       std::vector<std::uint8_t>& out, std::string* err_out)
{
    const HubRequestResult blob =
        hub_http_get_bytes(fetch.url, /*bearer=*/{}, make_range_header(fetch.url_range));
    if (!blob.ok) {
        if (err_out)
            *err_out = blob.err.empty() ? ("HTTP " + std::to_string(blob.http_status)) : blob.err;
        return false;
    }
    if (blob.body.empty()) {
        if (err_out)
            *err_out = "xet: empty xorb download";
        return false;
    }

    std::vector<std::vector<std::uint8_t>> chunks;
    std::string parse_err;
    if (!xet_parse_xorb_chunks(reinterpret_cast<const std::uint8_t*>(blob.body.data()), blob.body.size(),
                               chunks, &parse_err)) {
        if (err_out)
            *err_out = parse_err;
        return false;
    }

    const std::size_t expected_chunks = fetch.chunk_range.end - fetch.chunk_range.start;
    if (chunks.size() != expected_chunks) {
        if (err_out)
            *err_out = "xet: chunk count mismatch (got " + std::to_string(chunks.size()) + ", expected "
                       + std::to_string(expected_chunks) + ")";
        return false;
    }

    std::size_t written = 0;
    for (std::size_t idx = term.range.start; idx < term.range.end; ++idx) {
        const std::size_t local = idx - fetch.chunk_range.start;
        if (local >= chunks.size()) {
            if (err_out)
                *err_out = "xet: term chunk index out of range";
            return false;
        }
        out.insert(out.end(), chunks[local].begin(), chunks[local].end());
        written += chunks[local].size();
    }

    if (term.unpacked_length > 0 && written > term.unpacked_length)
        out.resize(out.size() - (written - term.unpacked_length));
    return true;
}

void emit_progress(const StreamDownloadOptions& opts, std::uint64_t done, std::uint64_t total,
                   const std::string& current_file)
{
    if (!opts.on_progress)
        return;
    DownloadProgress p;
    p.bytes_done    = done;
    p.bytes_total   = total;
    p.current_file  = current_file;
    p.file_index    = 1;
    p.file_count    = 1;
    p.paused        = opts.pause_check && opts.pause_check();
    opts.on_progress(p);
}

} // namespace

StreamDownloadResult xet_download_file(XetFileDownloadRequest request)
{
    StreamDownloadResult out;
    out.local_path = request.local_path;

    if (request.repo_id.empty() || request.xet_hash.empty()) {
        out.err = "xet: repo_id and xet_hash are required";
        return out;
    }

    const std::string hf_token =
        request.hf_token.empty() ? resolve_hf_token() : request.hf_token;
    std::string revision = request.revision;
    if (revision.empty())
        revision = fetch_model_revision(request.repo_id, hf_token);
    if (revision.empty()) {
        out.err = "xet: failed to resolve model revision";
        return out;
    }

    std::optional<XetReadToken> xet_tok = fetch_xet_read_token(request.repo_id, revision, hf_token);
    if (!xet_tok) {
        out.err = "xet: failed to fetch xet-read-token";
        return out;
    }

    std::string cas_base = xet_tok->cas_url;
    while (!cas_base.empty() && cas_base.back() == '/')
        cas_base.pop_back();
    const std::string recon_url = cas_base + "/v1/reconstructions/" + request.xet_hash;

    HubRequestResult recon = hub_http_get_bytes(recon_url, xet_tok->access_token);
    if (!recon.ok && recon.http_status == 401) {
        xet_tok = fetch_xet_read_token(request.repo_id, revision, hf_token);
        if (xet_tok)
            recon = hub_http_get_bytes(recon_url, xet_tok->access_token);
    }
    if (!recon.ok) {
        out.http_status = recon.http_status;
        out.err         = recon.err.empty() ? ("xet reconstruction HTTP " + std::to_string(recon.http_status))
                                            : recon.err;
        return out;
    }

    nlohmann::json recon_json;
    try {
        recon_json = nlohmann::json::parse(recon.body);
    } catch (const std::exception& e) {
        out.err = std::string("xet: invalid reconstruction JSON: ") + e.what();
        return out;
    }

    if (!recon_json.contains("terms") || !recon_json["terms"].is_array()) {
        out.err = "xet: reconstruction missing terms";
        return out;
    }
    const nlohmann::json& fetch_info = recon_json.value("fetch_info", nlohmann::json::object());
    const std::uint64_t offset_skip  = recon_json.value("offset_into_first_range", static_cast<std::uint64_t>(0));

    std::vector<ReconTerm> terms;
    std::uint64_t total_unpacked = 0;
    for (const auto& t : recon_json["terms"]) {
        if (!t.is_object())
            continue;
        ReconTerm term;
        term.xorb_hash       = t.value("hash", std::string{});
        term.unpacked_length = t.value("unpacked_length", static_cast<std::uint64_t>(0));
        if (!parse_chunk_range(t.value("range", nlohmann::json::object()), term.range))
            continue;
        if (term.xorb_hash.empty())
            continue;
        total_unpacked += term.unpacked_length;
        terms.push_back(std::move(term));
    }
    if (terms.empty()) {
        out.err = "xet: no reconstruction terms";
        return out;
    }

    const std::uint64_t expected_total = request.expected_size >= 0
        ? static_cast<std::uint64_t>(request.expected_size)
        : total_unpacked;

    std::error_code ec;
    if (request.local_path.has_parent_path())
        fs::create_directories(request.local_path.parent_path(), ec);

    fs::path part_path = request.local_path;
    part_path += media::path::path_from_utf8(".part");
    if (request.options.overwrite || !request.options.skip_existing) {
        fs::remove(part_path, ec);
        ec.clear();
    } else if (fs::is_regular_file(request.local_path, ec)) {
        const auto sz = fs::file_size(request.local_path, ec);
        if (!ec && request.expected_size >= 0
            && sz == static_cast<std::uint64_t>(request.expected_size)) {
            out.ok            = true;
            out.skipped       = true;
            out.bytes_written = sz;
            out.local_path    = request.local_path;
            return out;
        }
    }

    std::ofstream ofs(part_path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        out.err = "xet: cannot open output file: " + media::path::path_to_utf8(part_path);
        return out;
    }

    std::uint64_t aggregate_done = 0;
    bool first_term              = true;
    for (const auto& term : terms) {
        if (request.options.cancel_check && request.options.cancel_check()) {
            out.cancelled = true;
            out.err       = "download cancelled";
            ofs.close();
            fs::remove(part_path, ec);
            return out;
        }
        while (request.options.pause_check && request.options.pause_check()) {
            emit_progress(request.options, aggregate_done, expected_total, media::path::path_to_utf8(request.local_path.filename()));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (request.options.cancel_check && request.options.cancel_check()) {
                out.cancelled = true;
                out.err       = "download cancelled";
                ofs.close();
                fs::remove(part_path, ec);
                return out;
            }
        }

        if (token_expired_soon(*xet_tok)) {
            if (auto refreshed = fetch_xet_read_token(request.repo_id, revision, hf_token))
                xet_tok = refreshed;
        }

        const nlohmann::json fetch_arr =
            fetch_info.contains(term.xorb_hash) ? fetch_info[term.xorb_hash] : nlohmann::json::array();
        const std::optional<FetchSlice> fetch = pick_fetch_slice(fetch_arr, term.range);
        if (!fetch) {
            out.err = "xet: no fetch_info slice for term " + term.xorb_hash;
            ofs.close();
            fs::remove(part_path, ec);
            return out;
        }

        std::vector<std::uint8_t> term_bytes;
        std::string term_err;
        if (!append_term_bytes(term, *fetch, *xet_tok, term_bytes, &term_err)) {
            out.err = term_err;
            ofs.close();
            fs::remove(part_path, ec);
            return out;
        }

        if (first_term && offset_skip > 0) {
            if (offset_skip >= term_bytes.size()) {
                out.err = "xet: offset_into_first_range exceeds first term";
                ofs.close();
                fs::remove(part_path, ec);
                return out;
            }
            term_bytes.erase(term_bytes.begin(),
                             term_bytes.begin() + static_cast<std::ptrdiff_t>(offset_skip));
            first_term = false;
        } else {
            first_term = false;
        }

        ofs.write(reinterpret_cast<const char*>(term_bytes.data()),
                  static_cast<std::streamsize>(term_bytes.size()));
        if (!ofs.good()) {
            out.err = "xet: write failed";
            ofs.close();
            fs::remove(part_path, ec);
            return out;
        }

        aggregate_done += term_bytes.size();
        emit_progress(request.options, aggregate_done, expected_total, media::path::path_to_utf8(request.local_path.filename()));
    }

    ofs.close();
    std::error_code mv_ec;
    fs::rename(part_path, request.local_path, mv_ec);
    if (mv_ec) {
        fs::copy_file(part_path, request.local_path, fs::copy_options::overwrite_existing, mv_ec);
        if (mv_ec) {
            out.err = "xet: rename failed: " + mv_ec.message();
            return out;
        }
        fs::remove(part_path, mv_ec);
    }

    out.ok            = true;
    out.bytes_written = aggregate_done;
    out.effective_url = recon_url;
    return out;
}

} // namespace pm::hf

#include "llm/huggingface/hf_stream_download.hpp"

#include <curl/curl.h>

#include <chrono>
#include <fstream>
#include <random>
#include <thread>
#include "fs/path_sanitizer.hpp"

namespace fs = std::filesystem;

namespace pm::hf {
namespace {

bool should_retry_http(long status)
{
    return status == 408 || status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

bool should_retry_curl(CURLcode code)
{
    return code == CURLE_OPERATION_TIMEDOUT || code == CURLE_COULDNT_CONNECT || code == CURLE_COULDNT_RESOLVE_HOST
           || code == CURLE_RECV_ERROR || code == CURLE_SEND_ERROR || code == CURLE_GOT_NOTHING
           || code == CURLE_SSL_CONNECT_ERROR;
}

int backoff_ms(int base_ms, int attempt)
{
    const int scaled = base_ms * (1 << std::min(attempt, 6));
    std::mt19937 rng(static_cast<unsigned>(
        std::chrono::steady_clock::now().time_since_epoch().count() ^ static_cast<unsigned>(attempt)));
    std::uniform_int_distribution<int> jitter(0, base_ms);
    return std::min(scaled + jitter(rng), 30'000);
}

std::uint64_t file_size_bytes(const fs::path& p)
{
    std::error_code ec;
    const auto sz = fs::file_size(p, ec);
    return ec ? 0u : static_cast<std::uint64_t>(sz);
}

struct FileSink {
    std::ofstream*                        out = nullptr;
    std::uint64_t                         bytes_written = 0;
    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::time_point last_progress_emit;
    StreamDownloadOptions                 opts;
    std::uint64_t                         resume_base = 0;
    std::uint64_t                         expected_total = 0;
};

void emit_progress(FileSink& sink, std::uint64_t done, std::uint64_t total = 0)
{
    if (!sink.opts.on_progress)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (sink.last_progress_emit.time_since_epoch().count() != 0
        && now - sink.last_progress_emit < std::chrono::milliseconds(250))
        return;
    sink.last_progress_emit = now;
    const double elapsed = std::max(0.001, std::chrono::duration<double>(now - sink.started).count());
    DownloadProgress p;
    p.bytes_done    = sink.resume_base + done;
    p.bytes_per_sec = static_cast<double>(done) / elapsed;
    if (total > 0)
        p.bytes_total = total + sink.resume_base;
    else if (sink.expected_total > 0)
        p.bytes_total = sink.expected_total;
    p.paused = sink.opts.pause_check && sink.opts.pause_check();
    sink.opts.on_progress(p);
}

size_t curl_write_file(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* sink = static_cast<FileSink*>(userdata);
    const size_t n = size * nmemb;
    if (!sink || !sink->out || !sink->out->is_open())
        return 0;
    sink->out->write(ptr, static_cast<std::streamsize>(n));
    if (!sink->out->good())
        return 0;
    sink->bytes_written += n;
    emit_progress(*sink, sink->bytes_written);
    return n;
}

int curl_xferinfo(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t)
{
    auto* sink = static_cast<FileSink*>(userdata);
    if (!sink)
        return 0;
    if (sink->opts.cancel_check && sink->opts.cancel_check())
        return 1;
    while (sink->opts.pause_check && sink->opts.pause_check()) {
        emit_progress(*sink, static_cast<std::uint64_t>(dlnow > 0 ? dlnow : sink->bytes_written),
                      static_cast<std::uint64_t>(dltotal));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (sink->opts.cancel_check && sink->opts.cancel_check())
            return 1;
    }
    if (dltotal > 0 || dlnow > 0) {
        emit_progress(*sink, static_cast<std::uint64_t>(dlnow > 0 ? dlnow : sink->bytes_written),
                      static_cast<std::uint64_t>(dltotal));
    }
    return 0;
}

struct AttemptOutcome {
    CURLcode      curl_code = CURLE_OK;
    long          http_status = 0;
    std::string   error;
    std::string   effective_url;
    std::uint64_t bytes_this_attempt = 0;
};

AttemptOutcome run_download_attempt(const std::string& url, std::uint64_t resume_from, FileSink& sink)
{
    AttemptOutcome out;
    CURL* curl = curl_easy_init();
    if (!curl) {
        out.error     = "curl_easy_init failed";
        out.curl_code = CURLE_FAILED_INIT;
        return out;
    }

    struct curl_slist* hdr = nullptr;
    if (!sink.opts.bearer_token.empty())
        hdr = curl_slist_append(hdr, ("Authorization: Bearer " + sink.opts.bearer_token).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (hdr)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_xferinfo);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &sink);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    if (sink.opts.timeout_ms > 0)
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, sink.opts.timeout_ms);
    if (sink.opts.connect_timeout_ms > 0)
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, sink.opts.connect_timeout_ms);
    if (resume_from > 0)
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(resume_from));

    out.curl_code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.http_status);
    char* eff = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
    if (eff)
        out.effective_url = eff;
    out.bytes_this_attempt = sink.bytes_written;

    if (out.curl_code != CURLE_OK) {
        out.error = curl_easy_strerror(out.curl_code);
        if (out.curl_code == CURLE_ABORTED_BY_CALLBACK)
            out.error = "download cancelled";
    }

    if (hdr)
        curl_slist_free_all(hdr);
    curl_easy_cleanup(curl);
    return out;
}

} // namespace

StreamDownloadResult stream_download_url(const std::string& url, const fs::path& local_path,
                                         StreamDownloadOptions options)
{
    StreamDownloadResult out;
    out.local_path = local_path;
    if (url.empty()) {
        out.err = "missing download URL";
        return out;
    }

    std::error_code ec;
    if (local_path.has_parent_path())
        fs::create_directories(local_path.parent_path(), ec);

    fs::path part_path = local_path;
    part_path += media::path::path_from_utf8(".part");
    std::uint64_t  resume_from = 0;

    if (options.skip_existing && !options.overwrite) {
        if (fs::is_regular_file(local_path, ec)) {
            const std::uint64_t local_sz = file_size_bytes(local_path);
            const bool size_ok           = options.expected_size < 0
                || local_sz == static_cast<std::uint64_t>(options.expected_size);
            if (size_ok) {
                out.ok            = true;
                out.skipped       = true;
                out.bytes_written = local_sz;
                out.resumed_from  = local_sz;
                out.effective_url = url;
                return out;
            }
        }
        ec.clear();
    }

    if (options.resume) {
        if (fs::exists(part_path, ec))
            resume_from = file_size_bytes(part_path);
        else if (fs::exists(local_path, ec))
            resume_from = file_size_bytes(local_path);
    }

    if (options.expected_size >= 0 && static_cast<std::uint64_t>(options.expected_size) == resume_from
        && fs::exists(local_path, ec) && !options.overwrite) {
        out.ok            = true;
        out.skipped       = true;
        out.bytes_written = resume_from;
        out.resumed_from  = resume_from;
        out.effective_url = url;
        return out;
    }

    const bool append_part = resume_from > 0 && fs::exists(part_path, ec);
    std::ofstream ofs;
    if (append_part)
        ofs.open(part_path, std::ios::binary | std::ios::app);
    else
        ofs.open(part_path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        out.err = "cannot open output file: " + media::path::path_to_utf8(part_path);
        return out;
    }

    FileSink sink;
    sink.out             = &ofs;
    sink.started         = std::chrono::steady_clock::now();
    sink.opts            = std::move(options);
    sink.resume_base     = resume_from;
    sink.expected_total  = sink.opts.expected_size >= 0
        ? static_cast<std::uint64_t>(sink.opts.expected_size) : 0;

    const int max_attempts = std::max(1, sink.opts.retries + 1);
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        if (sink.opts.cancel_check && sink.opts.cancel_check()) {
            out.cancelled = true;
            out.err       = "download cancelled";
            break;
        }

        sink.bytes_written = 0;
        const AttemptOutcome att = run_download_attempt(url, resume_from, sink);
        out.http_status          = att.http_status;
        out.effective_url        = att.effective_url.empty() ? url : att.effective_url;

        if (att.curl_code == CURLE_ABORTED_BY_CALLBACK) {
            out.cancelled = true;
            out.err       = att.error;
            break;
        }
        if (att.curl_code == CURLE_OK && att.http_status >= 200 && att.http_status < 300) {
            ofs.close();
            std::error_code mv_ec;
            fs::rename(part_path, local_path, mv_ec);
            if (mv_ec) {
                fs::copy_file(part_path, local_path, fs::copy_options::overwrite_existing, mv_ec);
                if (mv_ec) {
                    out.err = "rename failed: " + mv_ec.message();
                    return out;
                }
                fs::remove(part_path, mv_ec);
            }
            out.ok            = true;
            out.bytes_written = file_size_bytes(local_path);
            out.resumed_from  = resume_from;
            return out;
        }

        const bool retryable = should_retry_curl(att.curl_code)
            || (att.curl_code == CURLE_OK && should_retry_http(att.http_status));
        out.err = att.error.empty()
            ? ("HTTP " + std::to_string(att.http_status))
            : att.error;

        if (!retryable || attempt + 1 >= max_attempts)
            break;

        if (!sink.opts.keep_partial) {
            ofs.close();
            fs::remove(part_path, ec);
            resume_from = 0;
            ofs.open(part_path, std::ios::binary | std::ios::trunc);
            sink.out = &ofs;
        }

        const int delay = backoff_ms(sink.opts.retry_delay_ms, attempt);
        for (int slept = 0; slept < delay; slept += 50) {
            if (sink.opts.cancel_check && sink.opts.cancel_check()) {
                out.cancelled = true;
                out.err       = "download cancelled";
                return out;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    ofs.close();
    if (!sink.opts.keep_partial && fs::exists(part_path, ec))
        fs::remove(part_path, ec);
    return out;
}

} // namespace pm::hf

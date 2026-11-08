#include "run_capture.hpp"

#include "constants.hpp"
#include "core/settings_store.hpp"
#include "fs/path_sanitizer.hpp"
#include "llm/llm_constants.hpp"
#include "logging/pm_log.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace media::llm::run {

namespace {

constexpr const char k_ch[] = "run_tool";

thread_local std::string tl_capture_session_id;

std::string sanitize_capture_session_id(const std::string& id) {
    std::string out;
    out.reserve(id.size());
    for (unsigned char c : id) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.')
            out.push_back(static_cast<char>(c));
        else
            out.push_back('_');
    }
    while (!out.empty() && out.front() == '.')
        out.erase(out.begin());
    for (std::size_t i = 0; i + 1 < out.size(); ++i) {
        if (out[i] == '.' && out[i + 1] == '.') {
            out[i] = '_';
            out[i + 1] = '_';
        }
    }
    if (out.empty())
        out = "session";
    if (out.size() > 80)
        out.resize(80);
    return out;
}

unsigned long capture_pid() {
#if defined(_WIN32)
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

bool stream_name_ok(std::string_view stream_name) {
    return stream_name == "stdout" || stream_name == "stderr";
}

std::size_t utf8_floor(std::string_view s, std::size_t n) {
    if (n >= s.size())
        return s.size();
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0u) == 0x80u)
        --n;
    return n;
}

std::size_t utf8_ceil(std::string_view s, std::size_t n) {
    if (n >= s.size())
        return s.size();
    while (n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0u) == 0x80u)
        ++n;
    return n;
}

/// Same line breaks as file_read (LF, CR, CRLF), without allocating a span per line.
std::size_t count_text_lines(std::string_view buf) {
    const std::size_t n = buf.size();
    if (n == 0)
        return 0;
    std::size_t lines = 0;
    std::size_t i = 0;
    while (i < n) {
        while (i < n && buf[i] != '\n' && buf[i] != '\r')
            ++i;
        ++lines;
        if (i >= n)
            break;
        if (buf[i] == '\r' && i + 1 < n && buf[i + 1] == '\n')
            i += 2;
        else
            ++i;
    }
    return lines;
}

bool path_is_inside(const fs::path& root, const fs::path& candidate) {
    std::error_code ec;
    const fs::path base = fs::weakly_canonical(root, ec);
    if (ec || base.empty())
        return false;
    ec.clear();
    const fs::path abs = fs::weakly_canonical(candidate, ec);
    if (ec || abs.empty())
        return false;
    const fs::path rel = abs.lexically_relative(base);
    const std::string s = media::path::path_to_generic_utf8(rel);
    if (s.empty() || s == "." || s == "..")
        return false;
    if (s.rfind("../", 0) == 0)
        return false;
    return s.find("/../") == std::string::npos;
}

bool is_capture_filename(const fs::path& p) {
    const std::string name = media::path::path_to_utf8(p.filename());
    if (name.rfind("run-", 0) != 0)
        return false;
    return name.size() >= 11
        && (name.ends_with(".stdout.txt") || name.ends_with(".stderr.txt")
            || name.ends_with(".stdout.txt.tmp") || name.ends_with(".stderr.txt.tmp"));
}

bool older_than_max_age(const fs::path& p) {
    std::error_code ec;
    const auto mtime = fs::last_write_time(p, ec);
    if (ec)
        return false;
    const auto age = fs::file_time_type::clock::now() - mtime;
    return age > std::chrono::hours(pm::llm::k_run_output_capture_max_age_hours);
}

void sweep_stale_captures(const fs::path& root) {
    std::error_code ec;
    if (!fs::exists(root, ec) || ec)
        return;
    std::vector<fs::path> files;
    std::vector<fs::path> dirs;
    for (fs::recursive_directory_iterator it(
             root, fs::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
        const fs::path cur = it->path();
        if (!path_is_inside(root, cur))
            continue;
        std::error_code st;
        if (it->is_regular_file(st) && is_capture_filename(cur) && older_than_max_age(cur))
            files.push_back(cur);
        else if (it->is_directory(st))
            dirs.push_back(cur);
    }
    for (const fs::path& f : files)
        fs::remove(f, ec);
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
        return a.native().size() > b.native().size();
    });
    for (const fs::path& d : dirs) {
        if (!path_is_inside(root, d))
            continue;
        fs::remove(d, ec); // fails when the directory is not empty
    }
}

void maybe_sweep_stale_captures() {
    static std::mutex sweep_mu;
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(sweep_mu);
        if (last != std::chrono::steady_clock::time_point{}
            && now - last < std::chrono::hours(1))
            return;
        last = now;
    }
    try {
        sweep_stale_captures(agent_run_temp_directory());
    } catch (const std::exception& e) {
        pm::log::warn_lazy(k_ch, [&] {
            return std::string("run capture cleanup failed: ") + e.what();
        });
    } catch (...) {
        pm::log::warn_lazy(k_ch, [] { return std::string("run capture cleanup failed"); });
    }
}

std::string session_folder_name(std::string_view session_id) {
    std::string raw(session_id);
    if (raw.empty())
        raw = tl_capture_session_id;
    if (raw.empty())
        return "unscoped";
    return sanitize_capture_session_id(raw);
}

bool write_capture_file(const fs::path& dest, std::string_view bytes, std::string& err) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    if (ec) {
        err = "failed to create run capture file";
        return false;
    }
    fs::path tmp = dest;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "failed to create run capture file";
            return false;
        }
        std::size_t off = 0;
        while (off < bytes.size()) {
            const std::size_t room = bytes.size() - off;
            const std::size_t chunk = std::min(
                room, static_cast<std::size_t>(64u * 1024u * 1024u));
            out.write(bytes.data() + off, static_cast<std::streamsize>(chunk));
            if (!out)
                break;
            off += chunk;
        }
        out.flush();
        if (!out) {
            err = "failed to create run capture file";
            std::error_code rm;
            fs::remove(tmp, rm);
            return false;
        }
    }
    fs::rename(tmp, dest, ec);
    if (ec) {
        std::error_code rm;
        fs::remove(dest, rm);
        ec.clear();
        fs::rename(tmp, dest, ec);
        if (ec) {
            err = "failed to create run capture file";
            fs::remove(tmp, rm);
            return false;
        }
    }
    return true;
}

std::string clipping_marker(std::string_view stream_name,
                            std::size_t omitted_bytes,
                            std::size_t line_count,
                            const fs::path& path,
                            const std::string& file_error) {
    std::string m;
    m.reserve(256 + (path.empty() ? file_error.size() : 0));
    m += "\n\n[... run ";
    m += stream_name;
    m += " clipped: ";
    m += std::to_string(omitted_bytes);
    m += " bytes omitted, ";
    m += std::to_string(line_count);
    m += " lines total.\n";
    if (!path.empty()) {
        m += "Full captured output: ";
        m += media::path::path_to_utf8(path);
        m += "\nUse file_read with offset (1-based line) and limit. ";
        if (line_count > 0) {
            const std::size_t window = static_cast<std::size_t>(pm::llm::k_file_read_window_lines);
            const std::size_t tail_line =
                line_count > window ? line_count - window + 1 : 1;
            m += "The tail starts near offset ";
            m += std::to_string(tail_line);
            m += ". ";
        }
        m += "...]\n\n";
    } else {
        m += "Full output was not saved: ";
        m += file_error.empty() ? "failed to create run capture file" : file_error;
        m += " ...]\n\n";
    }
    return m;
}

void attach_one(nlohmann::json& envelope, const char* name, const RunCapturedOutput& out) {
    const std::string n(name);
    envelope[n] = out.inline_text;
    envelope[n + "_clipped"] = out.clipped;
    envelope[n + "_captured_bytes"] = static_cast<std::uint64_t>(out.captured_bytes);
    envelope[n + "_inline_bytes"] = static_cast<std::uint64_t>(out.inline_bytes);
    envelope[n + "_lines"] = static_cast<std::uint64_t>(out.line_count);
    envelope[n + "_capture_complete"] = out.capture_complete;
    if (!out.full_output_path.empty())
        envelope[n + "_file"] = media::path::path_to_utf8(out.full_output_path);
    else if (out.clipped)
        envelope[n + "_file"] = nullptr;
    if (!out.file_error.empty())
        envelope[n + "_file_error"] = out.file_error;
}

} // namespace

void set_capture_session_id(const std::string& id) {
    tl_capture_session_id = id;
}

void clear_capture_session_id() {
    tl_capture_session_id.clear();
}

fs::path agent_run_temp_directory() {
    return media::settings::get_config_dir() / "agent-temp" / "run";
}

fs::path create_run_capture_path(std::string_view stream_name,
                                 std::string_view session_id) {
    if (!stream_name_ok(stream_name))
        return {};
    static std::atomic<std::uint64_t> seq{0};
    const std::uint64_t n = seq.fetch_add(1, std::memory_order_relaxed) + 1;
    char seq_buf[32];
    std::snprintf(seq_buf, sizeof(seq_buf), "%06llu",
                  static_cast<unsigned long long>(n));
    const std::string filename = std::string("run-") + std::to_string(capture_pid())
        + "-" + seq_buf + "." + std::string(stream_name) + ".txt";
    return agent_run_temp_directory() / session_folder_name(session_id) / filename;
}

RunCapturedOutput prepare_run_output(std::string_view stream_name,
                                     std::string_view captured) {
    RunCapturedOutput out;
    out.captured_bytes = captured.size();
    out.line_count = count_text_lines(captured);
    out.capture_complete = true;
    maybe_sweep_stale_captures();
    if (!stream_name_ok(stream_name)) {
        out.inline_text.assign(captured.data(), captured.size());
        out.inline_bytes = out.inline_text.size();
        out.file_error = "failed to create run capture file";
        return out;
    }
    if (captured.size() <= pm::llm::k_run_output_inline_max_bytes) {
        out.inline_text.assign(captured.data(), captured.size());
        out.inline_bytes = out.inline_text.size();
        return out;
    }

    const std::size_t head_end = utf8_floor(captured, pm::llm::k_run_output_head_bytes);
    std::size_t tail_begin = 0;
    if (captured.size() > pm::llm::k_run_output_tail_bytes)
        tail_begin = utf8_ceil(captured, captured.size() - pm::llm::k_run_output_tail_bytes);
    if (tail_begin < head_end)
        tail_begin = head_end;
    const std::size_t omitted = tail_begin - head_end;
    if (omitted == 0) {
        out.inline_text.assign(captured.data(), captured.size());
        out.inline_bytes = out.inline_text.size();
        return out;
    }

    out.clipped = true;
    const fs::path path = create_run_capture_path(stream_name);
    std::string detail;
    if (path.empty()) {
        out.file_error = "failed to create run capture file";
        detail = out.file_error;
    } else {
        try {
            if (write_capture_file(path, captured, out.file_error))
                out.full_output_path = path;
            else
                detail = out.file_error;
        } catch (const std::exception& e) {
            out.file_error = "failed to create run capture file";
            out.full_output_path.clear();
            detail = e.what();
        } catch (...) {
            out.file_error = "failed to create run capture file";
            out.full_output_path.clear();
            detail = "unknown error";
        }
    }
    if (!out.file_error.empty()) {
        pm::log::warn_lazy(k_ch, [&] {
            return std::string("run capture ") + std::string(stream_name)
                + " not saved (" + std::to_string(captured.size()) + " bytes): "
                + (detail.empty() ? out.file_error : detail);
        });
    }

    const std::string marker = clipping_marker(
        stream_name, omitted, out.line_count, out.full_output_path, out.file_error);
    out.inline_text.reserve(head_end + marker.size() + (captured.size() - tail_begin));
    out.inline_text.append(captured.data(), head_end);
    out.inline_text += marker;
    out.inline_text.append(captured.data() + tail_begin, captured.size() - tail_begin);
    out.inline_bytes = out.inline_text.size();
    return out;
}

void attach_run_captured_output(nlohmann::json& envelope,
                                std::string_view stdout_text,
                                std::string_view stderr_text) {
    attach_one(envelope, "stdout", prepare_run_output("stdout", stdout_text));
    attach_one(envelope, "stderr", prepare_run_output("stderr", stderr_text));
}

} // namespace media::llm::run

#include "agent_store_io.hpp"
#include "agent_internal.hpp"

#include "core/settings_store.hpp"
#include "fs/path_sanitizer.hpp"
#include "logging/logger.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif
#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace media::llm::agent::detail {

bool replace_file_preserving_destination(const fs::path& from, const fs::path& to)
{
#if defined(_WIN32)
    if (MoveFileExW(from.c_str(), to.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    return false;
#else
    std::error_code ec;
    fs::rename(from, to, ec);
    return !ec;
#endif
}

fs::path unique_sibling_temp(const fs::path& dest)
{
    static std::atomic<unsigned> seq{0};
#if defined(_WIN32)
    const unsigned pid = static_cast<unsigned>(GetCurrentProcessId());
#else
    const unsigned pid = static_cast<unsigned>(::getpid());
#endif
    fs::path tmp = dest;
    tmp += "." + std::to_string(pid) + "." + std::to_string(seq.fetch_add(1)) + ".tmp";
    return tmp;
}

} // namespace media::llm::agent::detail

namespace media::llm::agent {

void schedule_store_invalidate();
void session_store_invalidate();

namespace {

std::mutex g_session_dir_mutex;
std::optional<fs::path> g_session_dir_override;
std::atomic<int> g_schedule_generation{0};
void (*g_session_dir_hook)() = nullptr;

fs::path resolve_session_store_dir_unlocked() {
    if (g_session_dir_override)
        return *g_session_dir_override;
    if (const char* env = std::getenv("PM_IMAGE_SESSION_DIR"); env && *env)
        return media::path::path_from_utf8(env);
    try {
        return media::settings::get_config_dir() / "sessions";
    } catch (const std::exception& ex) {
        logger::error(std::string("session store dir unavailable: ") + ex.what());
        return {};
    } catch (...) {
        logger::error("session store dir unavailable");
        return {};
    }
}

} // namespace

void set_session_dir_hook(void (*hook)()) {
    g_session_dir_hook = hook;
}

void schedule_generation_bump() {
    g_schedule_generation.fetch_add(1, std::memory_order_relaxed);
}

int schedule_generation() {
    return g_schedule_generation.load(std::memory_order_relaxed);
}

bool debug_store_dumps_enabled() {
    if (const char* env = std::getenv("PM_IMAGE_DEBUG_DUMPS"); env && *env)
        return std::string(env) != "0";
    return false;
}

void set_session_store_dir(const fs::path& dir) {
    std::optional<fs::path> next;
    if (!dir.empty())
        next = dir;
    {
        std::lock_guard<std::mutex> lk(g_session_dir_mutex);
        if (next == g_session_dir_override)
            return;
    }
    if (g_session_dir_hook)
        g_session_dir_hook();
    {
        std::lock_guard<std::mutex> lk(g_session_dir_mutex);
        g_session_dir_override = std::move(next);
    }
    schedule_generation_bump();
    schedule_store_invalidate();
    session_store_invalidate();
}

fs::path session_file_path_for(const std::string& sanitized_id) {
    std::lock_guard<std::mutex> lk(g_session_dir_mutex);
    return resolve_session_store_dir_unlocked() / (sanitized_id + ".json");
}

bool atomic_write_json(const fs::path& path, const nlohmann::json& j) {
    if (path.empty() || !path.has_parent_path())
        return false;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // Some libcs set ec when the directory already exists. That must not skip the write.
    if (!fs::is_directory(path.parent_path())) {
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_ERROR, "pm-chat",
                            "session dir failed %s path=%s",
                            ec.message().c_str(), media::path::path_to_utf8(path).c_str());
#endif
        return false;
    }

    // Open by byte string. ofstream(fs::path) was not creating session files
    // under the app files dir on Android, so the picker reloaded an empty stub.
    const fs::path tmp = detail::unique_sibling_temp(path);
    const std::string tmp_s = media::path::path_to_utf8(tmp);
    const std::string dest = media::path::path_to_utf8(path);
    try {
        std::ofstream o(tmp_s.c_str(), std::ios::binary | std::ios::trunc);
        if (!o.is_open()) {
#if defined(__ANDROID__)
            __android_log_print(ANDROID_LOG_ERROR, "pm-chat",
                                "session write open failed errno=%d path=%s", errno, tmp_s.c_str());
#endif
            return false;
        }
        o << detail::safe_json_dump(j, 2);
        o.flush();
        if (!o) {
#if defined(__ANDROID__)
            __android_log_print(ANDROID_LOG_ERROR, "pm-chat",
                                "session write flush failed errno=%d path=%s", errno, dest.c_str());
#endif
            std::error_code rm;
            fs::remove(tmp, rm);
            return false;
        }
    } catch (...) {
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_ERROR, "pm-chat", "session write threw path=%s", dest.c_str());
#endif
        std::error_code rm;
        fs::remove(tmp, rm);
        return false;
    }

    if (!detail::replace_file_preserving_destination(tmp, path)) {
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_ERROR, "pm-chat",
                            "session rename failed path=%s", dest.c_str());
#endif
        std::error_code rm;
        fs::remove(tmp, rm);
        return false;
    }
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "pm-chat", "session wrote %s", dest.c_str());
#endif
    return true;
}

bool read_json_file(const fs::path& path, nlohmann::json& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    try { in >> out; } catch (...) { return false; }
    return true;
}

void merge_memory_state(nlohmann::json& target, const nlohmann::json& patch) {
    if (!target.is_object())
        target = nlohmann::json::object();
    if (!patch.is_object())
        return;

    for (auto it = patch.begin(); it != patch.end(); ++it) {
        if (it.value().is_null()) {
            target.erase(it.key());
            continue;
        }
        if (target.contains(it.key()) && target[it.key()].is_object() && it.value().is_object()) {
            merge_memory_state(target[it.key()], it.value());
            continue;
        }
        target[it.key()] = it.value();
    }
}

std::string now_iso8601() {
    auto      now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm   tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::string format_iso8601(const std::chrono::system_clock::time_point& tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm   tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::optional<std::chrono::system_clock::time_point> parse_iso8601(const std::string& s) {
    if (s.size() < 19) return std::nullopt;
    std::tm tm{};
    std::istringstream ss(s.substr(0, 19));
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) return std::nullopt;
    tm.tm_isdst = 0;
#if defined(_WIN32)
    std::time_t t = _mkgmtime(&tm);
#else
    std::time_t t = timegm(&tm);
#endif
    if (t == static_cast<std::time_t>(-1)) return std::nullopt;

    std::size_t i = 19;
    if (i < s.size() && s[i] == '.') {
        ++i;
        if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i])))
            return std::nullopt;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
            ++i;
    }
    int offset_min = 0;
    if (i < s.size()) {
        if (s[i] == 'Z' || s[i] == 'z') {
            if (i + 1 != s.size()) return std::nullopt;
        } else if (s[i] == '+' || s[i] == '-') {
            const int sign = s[i] == '-' ? -1 : 1;
            ++i;
            if (i + 2 > s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))
                || !std::isdigit(static_cast<unsigned char>(s[i + 1])))
                return std::nullopt;
            const int hh = (s[i] - '0') * 10 + (s[i + 1] - '0');
            i += 2;
            int mm = 0;
            if (i < s.size() && s[i] == ':') ++i;
            if (i < s.size()) {
                if (i + 2 > s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))
                    || !std::isdigit(static_cast<unsigned char>(s[i + 1])))
                    return std::nullopt;
                mm = (s[i] - '0') * 10 + (s[i + 1] - '0');
                i += 2;
            }
            if (i != s.size() || hh > 23 || mm > 59) return std::nullopt;
            offset_min = sign * (hh * 60 + mm);
        } else {
            return std::nullopt;
        }
    }
    auto tp = std::chrono::system_clock::from_time_t(t);
    tp -= std::chrono::minutes(offset_min);
    return tp;
}

std::filesystem::path session_store_dir() {
    std::lock_guard<std::mutex> lk(g_session_dir_mutex);
    const fs::path dir = resolve_session_store_dir_unlocked();
    if (dir.empty())
        return {};
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::filesystem::path schedule_store_dir() {
    const fs::path sessions = session_store_dir();
    if (sessions.empty())
        return {};
    fs::path parent = sessions.parent_path();
    if (parent.empty())
        return {};
    return parent / "schedules";
}

std::string sanitize_session_id(const std::string& id) {
    if (id.empty())
        return {};
    bool rewritten = false;
    std::string out;
    out.reserve(id.size());
    for (unsigned char c : id) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
            rewritten = true;
        }
    }
    while (!out.empty() && out.front() == '.') {
        out.erase(out.begin());
        rewritten = true;
    }
    for (size_t i = 0; i + 1 < out.size(); ++i) {
        if (out[i] == '.' && out[i + 1] == '.') {
            out[i]     = '_';
            out[i + 1] = '_';
            rewritten = true;
        }
    }
    if (out.empty()) {
        out = "id";
        rewritten = true;
    }
    if (!rewritten && out.size() <= 80)
        return out;
    std::uint32_t hash = 2166136261u;
    for (unsigned char c : id) {
        hash ^= c;
        hash *= 16777619u;
    }
    std::ostringstream suffix;
    suffix << '_' << std::hex << std::setw(8) << std::setfill('0') << hash;
    const std::string tail = suffix.str();
    if (out.size() + tail.size() > 80)
        out.resize(80 - tail.size());
    return out + tail;
}

} // namespace media::llm::agent

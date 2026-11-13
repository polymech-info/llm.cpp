#include "agent_memory.hpp"
#include "agent_memory_artifacts.hpp"
#include "agent_internal.hpp"

#include "constants.hpp"
#include "llm_constants.hpp"
#include "core/settings_store.hpp"
#include "llm/chat_session_store.hpp"
#include "logging/logger.h"
#if !defined(PM_LEAN_SECURITY) && !defined(PM_ANDROID_CHAT)
#include "llm/tools/run/run_capture.hpp"
#endif
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <ctime>
#if defined(__ANDROID__)
#include <android/log.h>
#endif
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include "fs/path_sanitizer.hpp"

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

// ── Internal store ───────────────────────────────────────────────────────────

namespace {

constexpr size_t k_max_events = 50;

struct TaskStore {
    std::mutex                             mutex;
    std::unordered_map<std::string, ScheduledTask> tasks;    // scheduler tasks, mirrored under schedules/
    std::unordered_map<std::string, ScheduledTask> sessions; // chat-session memory (persisted to disk)
    int                                    next_seq = 1;
    bool                                   tasks_loaded = false;
    int                                    loaded_generation = -1;
};

std::atomic<int> g_schedule_generation{0};

TaskStore& store() {
    static TaskStore s;
    return s;
}

thread_local std::string tl_current_task_id;

std::string gen_id(const std::string& title) {
    auto& s = store();   // lock held by caller
    std::string safe;
    for (unsigned char c : title) {
        if (std::isalnum(c))
            safe += static_cast<char>(std::tolower(c));
        else if (!safe.empty() && safe.back() != '-')
            safe += '-';
        if (safe.size() >= 20) break;
    }
    while (!safe.empty() && safe.back() == '-') safe.pop_back();
    if (safe.empty()) safe = "task";
    return safe + "-" + std::to_string(s.next_seq++);
}

// ── Session-disk plumbing ───────────────────────────────────────────────────

std::mutex                  g_session_dir_mutex;
std::optional<fs::path>     g_session_dir_override;

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

bool ensure_session_dir(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    return !ec;
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

nlohmann::json serialize_session_task(const ScheduledTask& t) {
    nlohmann::json j;
    j["id"]           = t.id;
    j["title"]        = t.title;
    j["created_at"]   = t.created_at;
    j["updated_at"]   = t.updated_at;
    j["memory_state"] = t.memory_state;
    j["events"]       = t.events;
    return j;
}

void deserialize_session_task(const nlohmann::json& j, ScheduledTask& t) {
    if (j.contains("id") && j["id"].is_string())             t.id = j["id"].get<std::string>();
    if (j.contains("title") && j["title"].is_string())       t.title = j["title"].get<std::string>();
    if (j.contains("created_at") && j["created_at"].is_string()) t.created_at = j["created_at"].get<std::string>();
    if (j.contains("updated_at") && j["updated_at"].is_string()) t.updated_at = j["updated_at"].get<std::string>();
    if (j.contains("memory_state") && j["memory_state"].is_object())
        t.memory_state = j["memory_state"];
    if (j.contains("events") && j["events"].is_array())
        t.events = j["events"].get<std::vector<nlohmann::json>>();
}

/// Persist a session entry. Called while the TaskStore mutex is held.
/// False means the in-memory record was not committed.
bool persist_session_locked(const ScheduledTask& t) {
    if (t.id.empty()) return false;
    const std::string fid = sanitize_session_id(t.id);
    if (fid.empty()) return false;
    if (media::llm::chat_sessions::using_secure_storage()) {
        nlohmann::json existing;
        bool found = false;
        std::string err;
        // A bad signature or truncated document must stay on disk. Replacing it
        // with an empty session would look like a valid load on the next turn.
        if (!media::llm::chat_sessions::load_session_memory_document(fid, existing, found, err)) {
            logger::error("session persist refused id=" + t.id + " " + err);
            return false;
        }
        if (!media::llm::chat_sessions::save_session_memory_document(fid, serialize_session_task(t), err)) {
            logger::error("session persist failed id=" + t.id + " " + err);
            return false;
        }
        return true;
    }
    const fs::path path = session_file_path_for(fid);
    if (!atomic_write_json(path, serialize_session_task(t))) {
        logger::error("session persist failed id=" + t.id
                      + " path=" + media::path::path_to_utf8(path));
        return false;
    }
    return true;
}

/// Try to materialize a session from disk into the in-memory cache.
/// Returns the inserted task pointer (or existing one if already cached).
ScheduledTask* try_load_session_into_cache_locked(TaskStore& s, const std::string& id) {
    auto it = s.sessions.find(id);
    if (it != s.sessions.end()) return &it->second;

    const std::string fid = sanitize_session_id(id);
    if (fid.empty()) return nullptr;

    if (media::llm::chat_sessions::using_secure_storage()) {
        nlohmann::json j;
        bool found = false;
        std::string err;
        if (!media::llm::chat_sessions::load_session_memory_document(fid, j, found, err)) {
            logger::error("session load failed id=" + id + " " + err);
            return nullptr;
        }
        if (!found)
            return nullptr;

        ScheduledTask t;
        t.id = id;
        t.title = "chat-session";
        deserialize_session_task(j, t);
        // Don't trust the document's id field over the caller's id.
        t.id = id;

        auto [ins, _ok] = s.sessions.emplace(id, std::move(t));
        return &ins->second;
    }

    const fs::path path = session_file_path_for(fid);
    std::error_code ec;
    if (!fs::exists(path, ec)) return nullptr;

    nlohmann::json j;
    if (!read_json_file(path, j)) return nullptr;

    ScheduledTask t;
    t.id    = id;
    t.title = "chat-session";
    deserialize_session_task(j, t);
    // Don't trust the file's id field over the caller's id.
    t.id = id;

    auto [ins, _ok] = s.sessions.emplace(id, std::move(t));
    return &ins->second;
}

bool debug_dumps_enabled() {
    if (const char* env = std::getenv("PM_IMAGE_DEBUG_DUMPS"); env && *env) {
        return std::string(env) != "0";
    }
    return false;
}

} // namespace

// ── Time helpers ─────────────────────────────────────────────────────────────

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

static std::string tp_to_iso8601(const std::chrono::system_clock::time_point& tp) {
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

// ── Serialization ────────────────────────────────────────────────────────────

nlohmann::json task_to_json(const ScheduledTask& t) {
    nlohmann::json j;
    j["id"]                   = t.id;
    j["title"]                = t.title;
    j["prompt"]               = t.prompt;
    j["enabled"]              = t.enabled;
    j["created_at"]           = t.created_at;
    j["updated_at"]           = t.updated_at;
    j["run_count"]            = t.run_count;
    j["consecutive_failures"] = t.consecutive_failures;
    if (!t.last_error.empty())   j["last_error"]   = t.last_error;
    if (!t.folder_hint.empty())  j["folder_hint"]  = t.folder_hint;
    j["memory_state"]         = t.memory_state;

    nlohmann::json sched;
    switch (t.schedule.kind) {
        case ScheduleKind::At:    sched["kind"] = "at";    break;
        case ScheduleKind::In:    sched["kind"] = "in";    break;
        case ScheduleKind::Every: sched["kind"] = "every"; break;
    }
    if (t.schedule.interval) sched["interval_seconds"] = static_cast<int>(t.schedule.interval->count());
    if (t.schedule.delay)    sched["delay_seconds"]    = static_cast<int>(t.schedule.delay->count());
    if (t.schedule.max_runs) sched["max_runs"]         = *t.schedule.max_runs;
    if (t.schedule.run_at)   sched["run_at"]           = tp_to_iso8601(*t.schedule.run_at);
    if (t.schedule.start_at) sched["start_at"]         = tp_to_iso8601(*t.schedule.start_at);
    j["schedule"]             = std::move(sched);

    if (t.next_run_at) j["next_run_at"] = tp_to_iso8601(*t.next_run_at);
    if (t.last_run_at) j["last_run_at"] = tp_to_iso8601(*t.last_run_at);

    if (!t.events.empty()) {
        j["events"] = t.events;
    }
    return j;
}

static int json_int_field(const nlohmann::json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_number()) return 0;
    try { return j[key].get<int>(); }
    catch (...) { return 0; }
}

static bool task_from_json(const nlohmann::json& j, ScheduledTask& t) {
    try {
        if (!j.is_object() || !j.contains("id") || !j["id"].is_string()) return false;
        t = {};
        t.id = j["id"].get<std::string>();
        if (t.id.empty()) return false;
        t.title = j.value("title", std::string{});
        t.prompt = j.value("prompt", std::string{});
        t.enabled = j.value("enabled", true);
        t.created_at = j.value("created_at", std::string{});
        t.updated_at = j.value("updated_at", std::string{});
        t.folder_hint = j.value("folder_hint", std::string{});
        t.run_count = json_int_field(j, "run_count");
        t.consecutive_failures = json_int_field(j, "consecutive_failures");
        t.last_error = j.value("last_error", std::string{});
        if (j.contains("memory_state") && j["memory_state"].is_object())
            t.memory_state = j["memory_state"];
        if (j.contains("events") && j["events"].is_array()) {
            const auto& arr = j["events"];
            const std::size_t n = arr.size();
            const std::size_t start = n > k_max_events ? n - k_max_events : 0;
            t.events.reserve(n - start);
            for (std::size_t i = start; i < n; ++i)
                t.events.push_back(arr[i]);
        }
        if (j.contains("schedule") && j["schedule"].is_object()) {
            const auto& sched = j["schedule"];
            const auto kind = sched.value("kind", std::string{"every"});
            if (kind == "at") t.schedule.kind = ScheduleKind::At;
            else if (kind == "in") t.schedule.kind = ScheduleKind::In;
            else t.schedule.kind = ScheduleKind::Every;
            if (sched.contains("interval_seconds") && sched["interval_seconds"].is_number())
                t.schedule.interval = std::chrono::seconds(json_int_field(sched, "interval_seconds"));
            if (sched.contains("delay_seconds") && sched["delay_seconds"].is_number())
                t.schedule.delay = std::chrono::seconds(json_int_field(sched, "delay_seconds"));
            if (sched.contains("max_runs") && sched["max_runs"].is_number())
                t.schedule.max_runs = json_int_field(sched, "max_runs");
            if (sched.contains("run_at") && sched["run_at"].is_string())
                t.schedule.run_at = parse_iso8601(sched["run_at"].get<std::string>());
            if (sched.contains("start_at") && sched["start_at"].is_string())
                t.schedule.start_at = parse_iso8601(sched["start_at"].get<std::string>());
        }
        if (j.contains("next_run_at") && j["next_run_at"].is_string())
            t.next_run_at = parse_iso8601(j["next_run_at"].get<std::string>());
        if (j.contains("last_run_at") && j["last_run_at"].is_string())
            t.last_run_at = parse_iso8601(j["last_run_at"].get<std::string>());
        return true;
    } catch (...) {
        return false;
    }
}

nlohmann::json task_to_summary_json(const ScheduledTask& t) {
    nlohmann::json j;
    j["id"]      = t.id;
    j["title"]   = t.title;
    j["enabled"] = t.enabled;
    j["run_count"] = t.run_count;
    switch (t.schedule.kind) {
        case ScheduleKind::At:    j["schedule_kind"] = "at";    break;
        case ScheduleKind::In:    j["schedule_kind"] = "in";    break;
        case ScheduleKind::Every: j["schedule_kind"] = "every"; break;
    }
    if (t.schedule.interval) j["interval_seconds"] = static_cast<int>(t.schedule.interval->count());
    if (t.next_run_at)       j["next_run_at"]      = tp_to_iso8601(*t.next_run_at);
    if (t.last_run_at)       j["last_run_at"]      = tp_to_iso8601(*t.last_run_at);
    if (!t.last_error.empty() && t.consecutive_failures > 0) j["last_error"] = t.last_error;
    return j;
}

// ── Optional CWD debug dump ─────────────────────────────────────────────────
// Off by default — historically these files leaked into every CLI invocation's
// working directory. Re-enable via PM_IMAGE_DEBUG_DUMPS=1 for local debugging.
// Session memory now persists via per-session files under session_store_dir().

static void dump_debug_files_locked(const TaskStore& s) {
    if (!debug_dumps_enabled()) return;

    nlohmann::json sched_arr = nlohmann::json::array();
    for (const auto& kv : s.tasks)
        sched_arr.push_back(task_to_json(kv.second));
    try { std::ofstream("scheduler.json") << sched_arr.dump(2); } catch (...) {}

    nlohmann::json mem_arr = nlohmann::json::array();
    for (const auto& kv : s.sessions) {
        const ScheduledTask& t = kv.second;
        nlohmann::json mj;
        mj["id"]           = t.id;
        mj["memory_state"] = t.memory_state;
        mj["events"]       = t.events;
        mem_arr.push_back(std::move(mj));
    }
    try { std::ofstream("memory.json") << mem_arr.dump(2); } catch (...) {}
}

// ── Compute initial next_run_at from spec ────────────────────────────────────

static std::optional<std::chrono::system_clock::time_point>
compute_initial_next_run(const ScheduleSpec& spec) {
    const auto now = std::chrono::system_clock::now();
    switch (spec.kind) {
        case ScheduleKind::At:
            return spec.run_at;
        case ScheduleKind::In:
            if (spec.delay) return now + *spec.delay;
            return std::nullopt;
        case ScheduleKind::Every:
            if (spec.start_at) return *spec.start_at;
            if (spec.interval) return now + *spec.interval;
            return std::nullopt;
    }
    return std::nullopt;
}

// ── Scheduler files ──────────────────────────────────────────────────────────

static fs::path schedule_file_for(const std::string& id) {
    const std::string fid = sanitize_session_id(id);
    if (fid.empty()) return {};
    return schedule_store_dir() / (fid + ".json");
}

static bool persist_task_locked(const ScheduledTask& t) {
    if (t.id.empty()) return false;
    const fs::path path = schedule_file_for(t.id);
    if (path.empty()) return false;
    if (!atomic_write_json(path, task_to_json(t))) {
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_ERROR, "pm-chat",
                            "schedule write failed id=%s", t.id.c_str());
#endif
        return false;
    }
    return true;
}

static void ensure_tasks_loaded_locked(TaskStore& s) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int gen = g_schedule_generation.load(std::memory_order_relaxed);
        if (s.tasks_loaded && s.loaded_generation == gen) return;
        s.tasks.clear();
        std::error_code ec;
        const fs::path dir = schedule_store_dir();
        if (fs::is_directory(dir, ec)) {
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                if (it->path().extension() != ".json") continue;
                nlohmann::json j;
                if (!read_json_file(it->path(), j)) continue;
                ScheduledTask loaded;
                if (!task_from_json(j, loaded) || loaded.id.empty()) continue;
                const std::string id = loaded.id;
                s.tasks.insert_or_assign(id, std::move(loaded));
            }
        }
        if (g_schedule_generation.load(std::memory_order_relaxed) == gen) {
            s.tasks_loaded = true;
            s.loaded_generation = gen;
            return;
        }
        s.tasks_loaded = false;
    }
    s.tasks_loaded = true;
    s.loaded_generation = g_schedule_generation.load(std::memory_order_relaxed);
}

// ── Store API ────────────────────────────────────────────────────────────────

std::string task_store_create(ScheduledTask task) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);

    if (task.id.empty()) {
        do {
            task.id = gen_id(task.title);
        } while (s.tasks.find(task.id) != s.tasks.end());
    }

    const std::string now = now_iso8601();
    task.created_at = now;
    task.updated_at = now;

    if (!task.next_run_at)
        task.next_run_at = compute_initial_next_run(task.schedule);

    std::string id = task.id;
    s.tasks[id] = std::move(task);
    persist_task_locked(s.tasks.at(id));
    dump_debug_files_locked(s);
    return id;
}

std::optional<ScheduledTask> task_store_get(const std::string& id) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);
    // Scheduler tasks first, then in-memory sessions, then disk-backed sessions.
    auto it = s.tasks.find(id);
    if (it != s.tasks.end()) return it->second;
    auto sit = s.sessions.find(id);
    if (sit != s.sessions.end()) return sit->second;
    if (auto* loaded = try_load_session_into_cache_locked(s, id))
        return *loaded;
    return std::nullopt;
}

std::vector<ScheduledTask> task_store_list() {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);
    std::vector<ScheduledTask> out;
    out.reserve(s.tasks.size());
    for (const auto& kv : s.tasks)
        out.push_back(kv.second);
    return out;
}

bool task_store_update(const ScheduledTask& task) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);
    auto it = s.tasks.find(task.id);
    if (it == s.tasks.end()) return false;
    it->second             = task;
    it->second.updated_at  = now_iso8601();
    persist_task_locked(it->second);
    dump_debug_files_locked(s);
    return true;
}

bool task_store_cancel(const std::string& id) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);
    auto it = s.tasks.find(id);
    if (it == s.tasks.end()) return false;
    it->second.enabled    = false;
    it->second.updated_at = now_iso8601();
    persist_task_locked(it->second);
    dump_debug_files_locked(s);
    return true;
}

void task_store_clear_all() {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);
    std::error_code ec;
    const fs::path dir = schedule_store_dir();
    std::vector<fs::path> doomed;
    if (fs::is_directory(dir, ec)) {
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const auto ext = media::path::path_to_utf8(it->path().extension());
            if (ext == ".json" || ext == ".tmp")
                doomed.push_back(it->path());
        }
    }
    for (const auto& path : doomed)
        fs::remove(path, ec);
    s.tasks.clear();
    dump_debug_files_locked(s);
}

void task_store_drop_cache() {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    s.tasks.clear();
    s.tasks_loaded = false;
}

bool task_store_erase(const std::string& id) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);
    const bool had = s.tasks.erase(id) > 0;
    std::error_code ec;
    const fs::path path = schedule_file_for(id);
    if (!path.empty()) {
        fs::remove(path, ec);
        fs::remove(media::path::path_from_utf8(media::path::path_to_utf8(path) + ".tmp"), ec);
    }
    dump_debug_files_locked(s);
    return had;
}

bool task_store_write_memory(const std::string& task_id, const nlohmann::json& state) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);
    auto it = s.tasks.find(task_id);
    if (it != s.tasks.end()) {
        if (state.empty())
            it->second.memory_state = nlohmann::json::object();
        else
            merge_memory_state(it->second.memory_state, state);
        it->second.updated_at   = now_iso8601();
        const bool saved = persist_task_locked(it->second);
        dump_debug_files_locked(s);
        return saved;
    }
    // Session — fall through to disk if not cached.
    ScheduledTask* st = nullptr;
    auto sit = s.sessions.find(task_id);
    if (sit != s.sessions.end()) {
        st = &sit->second;
    } else {
        st = try_load_session_into_cache_locked(s, task_id);
    }
    if (!st) {
        // Consent can land before the chat session file exists. Create the
        // session record so the review and the decision are not dropped.
        if (sanitize_session_id(task_id).empty())
            return false;
        ScheduledTask created;
        created.id = task_id;
        created.title = "chat-session";
        created.created_at = now_iso8601();
        created.updated_at = created.created_at;
        auto [ins, inserted] = s.sessions.emplace(task_id, std::move(created));
        (void)inserted;
        st = &ins->second;
    }
    if (state.empty())
        st->memory_state = nlohmann::json::object();
    else
        merge_memory_state(st->memory_state, state);
    st->updated_at   = now_iso8601();
    const bool saved = persist_session_locked(*st);
    dump_debug_files_locked(s);
    return saved;
}

void task_store_append_event(const std::string& task_id, const nlohmann::json& event) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ensure_tasks_loaded_locked(s);

    auto it = s.tasks.find(task_id);
    if (it != s.tasks.end()) {
        auto& evs = it->second.events;
        evs.push_back(event);
        if (evs.size() > k_max_events)
            evs.erase(evs.begin(), evs.begin() + static_cast<std::ptrdiff_t>(evs.size() - k_max_events));
        it->second.updated_at = now_iso8601();
        persist_task_locked(it->second);
        dump_debug_files_locked(s);
        return;
    }

    ScheduledTask* sess = nullptr;
    auto sit = s.sessions.find(task_id);
    if (sit != s.sessions.end()) {
        sess = &sit->second;
    } else {
        sess = try_load_session_into_cache_locked(s, task_id);
    }
    if (!sess) return;
    auto& evs = sess->events;
    evs.push_back(event);
    if (evs.size() > k_max_events)
        evs.erase(evs.begin(), evs.begin() + static_cast<std::ptrdiff_t>(evs.size() - k_max_events));
    sess->updated_at = now_iso8601();
    persist_session_locked(*sess);
    dump_debug_files_locked(s);
}

// ── Per-turn task context ────────────────────────────────────────────────────

void set_current_task_id(const std::string& id) {
    tl_current_task_id = id;
#if !defined(PM_LEAN_SECURITY)
    media::llm::run::set_capture_session_id(id);
#endif
}
void clear_current_task_id() {
    tl_current_task_id.clear();
#if !defined(PM_LEAN_SECURITY)
    media::llm::run::clear_capture_session_id();
#endif
}
std::string get_current_task_id() { return tl_current_task_id; }

// ── Session memory ────────────────────────────────────────────────────────────
// Sessions live in TaskStore::sessions — completely separate from the scheduler's
// TaskStore::tasks map. They are never iterated by the scheduler and never appear
// in scheduler.json.

// Generated session ids include a millisecond timestamp so that concurrent
// process launches (the chat panel + a CLI invocation, two CLI invocations,
// or the same app started twice quickly) don't collide on the same disk file.
static std::string generate_unique_session_id(int seq) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
#if defined(_WIN32)
    const unsigned pid = static_cast<unsigned>(GetCurrentProcessId());
#else
    const unsigned pid = static_cast<unsigned>(::getpid());
#endif
    std::ostringstream oss;
    oss << "session-" << std::hex << ms << std::dec << '-' << pid << '-' << seq;
    return oss.str();
}

std::string session_create() {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);

    ScheduledTask st;
    st.id         = generate_unique_session_id(s.next_seq++);
    st.title      = "chat-session";
    st.created_at = now_iso8601();
    st.updated_at = st.created_at;

    const std::string id = st.id;
    auto [it, _ok] = s.sessions.emplace(id, std::move(st));
    persist_session_locked(it->second);
    return id;
}

std::string session_get_or_create(const std::string& preferred_id) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);

    if (!preferred_id.empty()) {
        // Already cached in memory?
        auto it = s.sessions.find(preferred_id);
        if (it != s.sessions.end())
            return it->first;
        // Pull from disk into the cache if a file exists.
        if (auto* loaded = try_load_session_into_cache_locked(s, preferred_id))
            return loaded->id;

        // Brand new session — create + persist.
        ScheduledTask st;
        st.id         = preferred_id;
        st.title      = "chat-session";
        st.created_at = now_iso8601();
        st.updated_at = st.created_at;
        auto [ins, _ok] = s.sessions.emplace(preferred_id, std::move(st));
        persist_session_locked(ins->second);
        return preferred_id;
    }

    ScheduledTask st;
    st.id         = generate_unique_session_id(s.next_seq++);
    st.title      = "chat-session";
    st.created_at = now_iso8601();
    st.updated_at = st.created_at;
    const std::string id = st.id;
    auto [ins, _ok] = s.sessions.emplace(id, std::move(st));
    persist_session_locked(ins->second);
    return id;
}

void session_load(const std::string& session_id,
                  nlohmann::json& out_memory_state,
                  nlohmann::json& out_recent_events,
                  size_t max_events)
{
    out_memory_state  = nlohmann::json::object();
    out_recent_events = nlohmann::json::array();

    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);

    auto it = s.sessions.find(session_id);
    ScheduledTask* src = (it != s.sessions.end()) ? &it->second
                                                  : try_load_session_into_cache_locked(s, session_id);
    if (!src) {
        lk.unlock();
        const nlohmann::json recovery = session_recover_from_journal(session_id);
        const int promoted = recovery.is_object() ? recovery.value("recovered", 0) : 0;
        if (promoted <= 0)
            return;
        lk.lock();
        it = s.sessions.find(session_id);
        src = (it != s.sessions.end()) ? &it->second
                                       : try_load_session_into_cache_locked(s, session_id);
        if (!src)
            return;
        out_memory_state = src->memory_state;
        detail::sanitize_json_utf8_strings(out_memory_state);
        const auto& evs = src->events;
        const size_t start = (evs.size() > max_events) ? evs.size() - max_events : 0;
        for (size_t i = start; i < evs.size(); ++i)
            out_recent_events.push_back(evs[i]);
        detail::sanitize_json_utf8_strings(out_recent_events);
        return;
    }

    out_memory_state = src->memory_state;
    detail::sanitize_json_utf8_strings(out_memory_state);

    // Return the last max_events entries (oldest-first so the LLM reads in order).
    const auto& evs = src->events;
    const size_t start = (evs.size() > max_events) ? evs.size() - max_events : 0;
    for (size_t i = start; i < evs.size(); ++i)
        out_recent_events.push_back(evs[i]);
    detail::sanitize_json_utf8_strings(out_recent_events);

    lk.unlock();

    // Promote any successful mid-turn tool work that survived a crash but never
    // reached end-of-turn artifact persistence.
    const nlohmann::json recovery = session_recover_from_journal(session_id);
    const int promoted = recovery.is_object() ? recovery.value("recovered", 0) : 0;
    if (promoted <= 0)
        return;

    lk.lock();
    it = s.sessions.find(session_id);
    src = (it != s.sessions.end()) ? &it->second
                                   : try_load_session_into_cache_locked(s, session_id);
    if (!src)
        return;

    out_memory_state = src->memory_state;
    detail::sanitize_json_utf8_strings(out_memory_state);

    const auto& evs2 = src->events;
    const size_t start2 = (evs2.size() > max_events) ? evs2.size() - max_events : 0;
    out_recent_events = nlohmann::json::array();
    for (size_t i = start2; i < evs2.size(); ++i)
        out_recent_events.push_back(evs2[i]);
    detail::sanitize_json_utf8_strings(out_recent_events);
}

bool session_read_memory_state(const std::string& session_id,
                               nlohmann::json& out_memory_state)
{
    out_memory_state = nlohmann::json::object();
    if (session_id.empty())
        return false;

    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);

    auto it = s.sessions.find(session_id);
    ScheduledTask* src = (it != s.sessions.end()) ? &it->second
                                                  : try_load_session_into_cache_locked(s, session_id);
    if (!src)
        return false;

    out_memory_state = src->memory_state;
    detail::sanitize_json_utf8_strings(out_memory_state);
    return true;
}

void session_append_turn(const std::string& session_id, const nlohmann::json& entry) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);

    auto it = s.sessions.find(session_id);
    ScheduledTask* sess = (it != s.sessions.end()) ? &it->second
                                                   : try_load_session_into_cache_locked(s, session_id);
    if (!sess) return;

    nlohmann::json stamped = entry;
    detail::sanitize_json_utf8_strings(stamped);
    auto& evs = sess->events;
    evs.push_back(std::move(stamped));

    // Evict oldest entries beyond the session ring-buffer limit.
    if (evs.size() > pm::llm::k_session_memory_max_turns)
        evs.erase(evs.begin(),
                  evs.begin() + static_cast<std::ptrdiff_t>(
                      evs.size() - pm::llm::k_session_memory_max_turns));

    sess->updated_at = now_iso8601();
    persist_session_locked(*sess);
    dump_debug_files_locked(s);
}

// ── #9 mid-turn tool journal (append-only durability) ──────────────────────

namespace {

std::mutex& journal_mutex() {
    static std::mutex m;
    return m;
}

fs::path session_journal_path_unlocked(const std::string& session_id) {
    return session_store_dir() / (sanitize_session_id(session_id) + ".journal.jsonl");
}

/// Roll the journal when it grows past the cap. Read every line, keep the
/// newest `keep` entries, rewrite atomically. Best-effort: never throws.
void compact_journal_if_needed_unlocked(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec)
        return;

    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) return;

    std::vector<std::string> lines;
    lines.reserve(pm::llm::k_session_journal_max_entries);
    std::string ln;
    while (std::getline(in, ln)) {
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        if (!ln.empty()) lines.push_back(std::move(ln));
    }
    in.close();

    if (lines.size() <= pm::llm::k_session_journal_max_entries)
        return;

    const std::size_t drop = lines.size() - pm::llm::k_session_journal_max_entries;
    const fs::path tmp_path = detail::unique_sibling_temp(path);
    {
        std::ofstream out(tmp_path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!out) return;
        for (std::size_t i = drop; i < lines.size(); ++i)
            out << lines[i] << "\n";
        out.flush();
        if (!out) {
            std::error_code rm;
            fs::remove(tmp_path, rm);
            return;
        }
    }
    if (!detail::replace_file_preserving_destination(tmp_path, path)) {
        std::error_code rm;
        fs::remove(tmp_path, rm);
    }
}

} // namespace

std::filesystem::path session_journal_path(const std::string& session_id) {
    std::lock_guard<std::mutex> lk(journal_mutex());
    return session_journal_path_unlocked(session_id);
}

void session_append_journal(const std::string& session_id, const nlohmann::json& entry) {
    if (session_id.empty()) return;
    try {
    std::lock_guard<std::mutex> lk(journal_mutex());
    if (media::llm::chat_sessions::using_secure_storage()) {
        nlohmann::json entries = nlohmann::json::array();
        nlohmann::json existing;
        bool found = false;
        std::string err;
        if (media::llm::chat_sessions::load_session_log_document(session_id, "journal", existing, found, err) &&
            found && existing.is_object() && existing.contains("entries") && existing["entries"].is_array()) {
            entries = existing["entries"];
        }

        nlohmann::json stamped = entry.is_object() ? entry : nlohmann::json::object();
        if (!stamped.contains("ts"))
            stamped["ts"] = now_iso8601();
        detail::sanitize_json_utf8_strings(stamped);
        entries.push_back(std::move(stamped));
        while (entries.size() > pm::llm::k_session_journal_max_entries)
            entries.erase(entries.begin());

        nlohmann::json doc{
            {"schema_version", "1"},
            {"kind", "session_journal"},
            {"session_id", session_id},
            {"updated_at", now_iso8601()},
            {"entries", std::move(entries)},
        };
        detail::sanitize_json_utf8_strings(doc);
        err.clear();
        (void)media::llm::chat_sessions::save_session_log_document(session_id, "journal", doc, err);
        return;
    }
    const fs::path path = session_journal_path_unlocked(session_id);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    nlohmann::json stamped = entry.is_object() ? entry : nlohmann::json::object();
    if (!stamped.contains("ts"))
        stamped["ts"] = now_iso8601();
    detail::sanitize_json_utf8_strings(stamped);

    {
        const std::string journal_s = media::path::path_to_utf8(path);
        std::ofstream out(journal_s.c_str(), std::ios::out | std::ios::app | std::ios::binary);
        if (!out) {
#if defined(__ANDROID__)
            __android_log_print(ANDROID_LOG_ERROR, "pm-chat",
                                "session journal open failed errno=%d path=%s", errno, journal_s.c_str());
#endif
            return;
        }
        out << detail::safe_json_dump(stamped) << "\n";
    }
    compact_journal_if_needed_unlocked(path);
    } catch (...) {}
}

nlohmann::json session_journal_read(const std::string& session_id) {
    nlohmann::json out = nlohmann::json::array();
    if (session_id.empty()) return out;
    std::lock_guard<std::mutex> lk(journal_mutex());
    if (media::llm::chat_sessions::using_secure_storage()) {
        nlohmann::json doc;
        bool found = false;
        std::string err;
        if (media::llm::chat_sessions::load_session_log_document(session_id, "journal", doc, found, err) &&
            found && doc.is_object() && doc.contains("entries") && doc["entries"].is_array()) {
            return doc["entries"];
        }
        return out;
    }
    const fs::path path = session_journal_path_unlocked(session_id);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec)
        return out;
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) return out;
    std::string ln;
    while (std::getline(in, ln)) {
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        if (ln.empty()) continue;
        try { out.push_back(nlohmann::json::parse(ln)); }
        catch (...) {} // skip malformed lines (defensive)
    }
    return out;
}


// ── Session-disk public API ─────────────────────────────────────────────────

std::filesystem::path session_store_dir() {
    std::lock_guard<std::mutex> lk(g_session_dir_mutex);
    const fs::path dir = resolve_session_store_dir_unlocked();
    if (dir.empty())
        return {};
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

void set_session_store_dir(const std::filesystem::path& dir) {
    auto& s = store();
    std::lock_guard<std::mutex> store_lk(s.mutex);
    std::lock_guard<std::mutex> dir_lk(g_session_dir_mutex);
    std::optional<fs::path> next;
    if (!dir.empty()) next = dir;
    if (next == g_session_dir_override) return;
    g_session_dir_override = std::move(next);
    g_schedule_generation.fetch_add(1, std::memory_order_relaxed);
    s.tasks.clear();
    s.tasks_loaded = false;
    s.sessions.clear();
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

std::vector<std::string> session_list_persisted() {
    std::vector<std::string> out;
    std::string err;
    if (media::llm::chat_sessions::try_list_secure_memory_ids(out, err)) {
        if (!err.empty())
            logger::error("session list failed: " + err);
        std::sort(out.begin(), out.end());
        return out;
    }
    const fs::path dir = session_store_dir();
    std::error_code ec;
    if (dir.empty() || !fs::exists(dir, ec)) return out;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const fs::path& p = entry.path();
        if (p.extension() == ".json")
            out.push_back(media::path::path_to_utf8(p.stem()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool session_delete(const std::string& session_id) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);

    bool dropped = false;
    auto it = s.sessions.find(session_id);
    if (it != s.sessions.end()) {
        s.sessions.erase(it);
        dropped = true;
    }

    const std::string fid = sanitize_session_id(session_id);
    if (fid.empty())
        return false;
    if (media::llm::chat_sessions::using_secure_storage()) {
        std::string err;
        const bool removed_memory = media::llm::chat_sessions::delete_session_memory_document(fid, err);
        if (!removed_memory) {
            logger::error("session delete memory failed id=" + session_id + " " + err);
            return false;
        }
        err.clear();
        const bool removed_journal = media::llm::chat_sessions::delete_session_log_document(fid, "journal", err);
        if (!removed_journal) {
            logger::error("session delete journal failed id=" + session_id + " " + err);
            return false;
        }
        return true;
    }
    const fs::path path = session_file_path_for(fid);
    const fs::path journal_path = session_journal_path_unlocked(session_id);
    std::error_code ec;
    const bool session_existed = fs::exists(path, ec) && !ec;
    ec.clear();
    if (session_existed && !fs::remove(path, ec)) {
        logger::error("session delete failed id=" + session_id + " " + ec.message());
        return false;
    }
    bool journal_existed = false;
    {
        std::lock_guard<std::mutex> journal_lk(journal_mutex());
        ec.clear();
        journal_existed = fs::exists(journal_path, ec) && !ec;
        ec.clear();
        if (journal_existed && !fs::remove(journal_path, ec)) {
            logger::error("session journal delete failed id=" + session_id + " " + ec.message());
            return false;
        }
    }
    return dropped || session_existed || journal_existed;
}

// ── Named session snapshots (P12) ────────────────────────────────────────────

std::filesystem::path snapshot_store_dir() {
    fs::path dir;
    {
        std::lock_guard<std::mutex> lk(g_session_dir_mutex);
        dir = resolve_session_store_dir_unlocked().parent_path() / "snapshots";
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

bool session_snapshot_save(const std::string& session_id,
                           const std::string& snapshot_name)
{
    if (session_id.empty() || snapshot_name.empty())
        return false;

    nlohmann::json existing;
    if (!session_read_memory_state(session_id, existing))
        return false;

    nlohmann::json memory_state, recent_events;
    session_load(session_id, memory_state, recent_events, 20);

    nlohmann::json snap;
    snap["name"]              = snapshot_name;
    snap["saved_at"]          = now_iso8601();
    snap["source_session_id"] = session_id;
    snap["memory_state"]      = memory_state;
    snap["recent_events"]     = recent_events;

    const std::string safe_name = sanitize_session_id(snapshot_name);
    if (safe_name.empty())
        return false;
    if (media::llm::chat_sessions::using_secure_storage()) {
        std::string err;
        if (!media::llm::chat_sessions::save_session_log_document("snapshots", safe_name, snap, err)) {
            logger::error("snapshot save failed name=" + snapshot_name + " " + err);
            return false;
        }
        return true;
    }
    const fs::path snap_path = snapshot_store_dir() / (safe_name + ".json");
    return atomic_write_json(snap_path, snap);
}

bool session_snapshot_load(const std::string& snapshot_name,
                           nlohmann::json& out_memory_state,
                           nlohmann::json& out_recent_events)
{
    if (snapshot_name.empty())
        return false;

    const std::string safe_name = sanitize_session_id(snapshot_name);
    if (safe_name.empty())
        return false;

    nlohmann::json snap;
    if (media::llm::chat_sessions::using_secure_storage()) {
        bool found = false;
        std::string err;
        if (!media::llm::chat_sessions::load_session_log_document("snapshots", safe_name, snap, found, err)
            || !found || !snap.is_object())
            return false;
    } else {
        const fs::path snap_path = snapshot_store_dir() / (safe_name + ".json");
        std::error_code ec;
        if (!fs::exists(snap_path, ec))
            return false;
        try {
            std::ifstream f(snap_path);
            if (!f.is_open()) return false;
            snap = nlohmann::json::parse(f);
        } catch (...) {
            return false;
        }
    }
    if (!snap.is_object() || !snap.contains("memory_state") || !snap["memory_state"].is_object())
        return false;
    out_memory_state = snap["memory_state"];
    out_recent_events = (snap.contains("recent_events") && snap["recent_events"].is_array())
        ? snap["recent_events"]
        : nlohmann::json::array();
    return true;
}

nlohmann::json scheduler_persistence_selftest() {
    nlohmann::json out;
    std::string id;
    try {
        ScheduledTask task;
        task.title = "persist-probe";
        task.prompt = "say pong later";
        task.schedule.kind = ScheduleKind::In;
        task.schedule.delay = std::chrono::seconds(3600);
        id = task_store_create(std::move(task));
        out["created_id"] = id;
        std::error_code ec;
        const fs::path file = schedule_file_for(id);
        out["file_exists"] = !file.empty() && fs::is_regular_file(file, ec);
        task_store_drop_cache();
        const auto loaded = task_store_get(id);
        const bool reloaded = loaded.has_value()
            && loaded->title == "persist-probe"
            && loaded->prompt == "say pong later"
            && loaded->enabled
            && loaded->schedule.kind == ScheduleKind::In
            && loaded->schedule.delay
            && loaded->schedule.delay->count() == 3600
            && loaded->next_run_at.has_value();
        out["reloaded"] = reloaded;
        const bool cancelled = task_store_cancel(id);
        out["cancelled"] = cancelled;
        task_store_drop_cache();
        const auto after = task_store_get(id);
        const bool disabled = after.has_value() && !after->enabled;
        out["disabled"] = disabled;
        task_store_erase(id);
        task_store_drop_cache();
        const bool erased = !task_store_get(id).has_value() && !fs::is_regular_file(file, ec);
        out["erased"] = erased;
        out["ok"] = out.value("file_exists", false) && reloaded && cancelled && disabled && erased;
        id.clear();
    } catch (const std::exception& ex) {
        if (!id.empty()) task_store_erase(id);
        out["ok"] = false;
        out["error"] = ex.what();
    } catch (...) {
        if (!id.empty()) task_store_erase(id);
        out["ok"] = false;
        out["error"] = "scheduler persistence selftest failed";
    }
    return out;
}

} // namespace media::llm::agent

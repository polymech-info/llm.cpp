#include "agent_memory.hpp"
#include "agent_store_io.hpp"
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
#include <unordered_set>
#include "fs/path_sanitizer.hpp"

namespace fs = std::filesystem;


namespace media::llm::agent {

// ── Internal store ───────────────────────────────────────────────────────────

namespace {

constexpr size_t k_max_events = 50;

struct SessionStore {
    std::mutex mutex;
    std::unordered_map<std::string, ScheduledTask> sessions;
    int next_seq = 1;
};

SessionStore& store() {
    static SessionStore s;
    return s;
}

thread_local std::string tl_current_task_id;
thread_local bool tl_current_is_schedule = false;

void dump_session_debug_locked(const SessionStore& s) {
    if (!debug_store_dumps_enabled()) return;
    nlohmann::json mem_arr = nlohmann::json::array();
    for (const auto& kv : s.sessions) {
        const ScheduledTask& t = kv.second;
        mem_arr.push_back({
            {"id", t.id},
            {"memory_state", t.memory_state},
            {"events", t.events},
        });
    }
    try { std::ofstream("memory.json") << mem_arr.dump(2); } catch (...) {}
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

/// Persist a session entry. Called while the session-store mutex is held.
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
ScheduledTask* try_load_session_into_cache_locked(SessionStore& s, const std::string& id) {
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


} // namespace





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

void set_current_task_is_schedule(bool schedule) { tl_current_is_schedule = schedule; }
bool current_task_is_schedule() { return tl_current_is_schedule; }

void session_store_invalidate() {
    auto& s = store();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.sessions.clear();
}

std::optional<ScheduledTask> session_memory_get(const std::string& id) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    auto it = s.sessions.find(id);
    if (it != s.sessions.end())
        return it->second;
    if (ScheduledTask* loaded = try_load_session_into_cache_locked(s, id))
        return *loaded;
    return std::nullopt;
}

bool session_write_memory(const std::string& session_id, const nlohmann::json& state) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ScheduledTask* st = nullptr;
    auto sit = s.sessions.find(session_id);
    if (sit != s.sessions.end())
        st = &sit->second;
    else
        st = try_load_session_into_cache_locked(s, session_id);
    if (!st) {
        if (sanitize_session_id(session_id).empty())
            return false;
        ScheduledTask created;
        created.id = session_id;
        created.title = "chat-session";
        created.created_at = now_iso8601();
        created.updated_at = created.created_at;
        auto [ins, inserted] = s.sessions.emplace(session_id, std::move(created));
        (void)inserted;
        st = &ins->second;
    }
    ScheduledTask next = *st;
    if (state.empty())
        next.memory_state = nlohmann::json::object();
    else
        merge_memory_state(next.memory_state, state);
    next.updated_at = now_iso8601();
    if (!persist_session_locked(next))
        return false;
    *st = std::move(next);
    dump_session_debug_locked(s);
    return true;
}

void session_append_event(const std::string& session_id, const nlohmann::json& event) {
    auto& s = store();
    std::unique_lock<std::mutex> lk(s.mutex);
    ScheduledTask* sess = nullptr;
    auto sit = s.sessions.find(session_id);
    if (sit != s.sessions.end())
        sess = &sit->second;
    else
        sess = try_load_session_into_cache_locked(s, session_id);
    if (!sess)
        return;
    ScheduledTask next = *sess;
    next.events.push_back(event);
    if (next.events.size() > k_max_events) {
        next.events.erase(next.events.begin(),
                          next.events.begin() + static_cast<std::ptrdiff_t>(
                              next.events.size() - k_max_events));
    }
    next.updated_at = now_iso8601();
    if (!persist_session_locked(next))
        return;
    *sess = std::move(next);
    dump_session_debug_locked(s);
}

// ── Session memory ────────────────────────────────────────────────────────────
// Sessions live in SessionStore — completely separate from the scheduler's
// task map. They are never iterated by the scheduler and never appear
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

    if (current_task_is_schedule() && session_id == get_current_task_id()) {
        const auto task = task_store_get(session_id);
        if (!task)
            return;
        out_memory_state = task->memory_state;
        detail::sanitize_json_utf8_strings(out_memory_state);
        const auto& evs = task->events;
        const size_t start = (evs.size() > max_events) ? evs.size() - max_events : 0;
        for (size_t i = start; i < evs.size(); ++i)
            out_recent_events.push_back(evs[i]);
        detail::sanitize_json_utf8_strings(out_recent_events);
        return;
    }

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

    if (current_task_is_schedule() && session_id == get_current_task_id()) {
        const auto task = task_store_get(session_id);
        if (!task)
            return false;
        out_memory_state = task->memory_state;
        detail::sanitize_json_utf8_strings(out_memory_state);
        return true;
    }

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
    dump_session_debug_locked(s);
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

    auto it = s.sessions.find(session_id);
    if (it != s.sessions.end())
        s.sessions.erase(it);

    const std::string fid = sanitize_session_id(session_id);
    if (fid.empty())
        return false;
    std::string sidecar_err;
    if (!media::llm::chat_sessions::delete_session_sidecars(fid, sidecar_err)) {
        logger::error("session delete documents failed id=" + session_id + " " + sidecar_err);
        return false;
    }
    const fs::path dir = session_store_dir();
    const fs::path path = session_file_path_for(fid);
    const fs::path journal_path = session_journal_path_unlocked(session_id);
    std::error_code ec;
    if (fs::exists(path, ec) && !ec && !fs::remove(path, ec)) {
        logger::error("session delete failed id=" + session_id + " " + ec.message());
        return false;
    }
    {
        std::lock_guard<std::mutex> journal_lk(journal_mutex());
        ec.clear();
        if (fs::exists(journal_path, ec) && !ec && !fs::remove(journal_path, ec)) {
            logger::error("session journal delete failed id=" + session_id + " " + ec.message());
            return false;
        }
    }
    ec.clear();
    if (fs::is_directory(dir, ec)) {
        const std::string prefix = fid + ".";
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec)
                break;
            if (!entry.is_regular_file(ec))
                continue;
            const std::string name = media::path::path_to_utf8(entry.path().filename());
            if (name.rfind(prefix, 0) != 0)
                continue;
            fs::remove(entry.path(), ec);
        }
    }
    return true;
}

// ── Named session snapshots (P12) ────────────────────────────────────────────

std::filesystem::path snapshot_store_dir() {
    const fs::path dir = session_store_dir().parent_path() / "snapshots";
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


} // namespace media::llm::agent

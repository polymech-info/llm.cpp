#include "llm/change_store.hpp"

#include "fs/fs_guard.hpp"
#include "core/settings_store.hpp"
#include "llm/agent_memory.hpp"
#include "storage/secure_storage.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

namespace media::llm::change {
namespace {

namespace fs = std::filesystem;

thread_local std::string tl_tool_call_id;
std::mutex g_mu;
std::optional<fs::path> g_override;

std::string trim_copy(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

std::int64_t unix_ms_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string iso_from_unix_ms(std::int64_t ms) {
    const auto secs = static_cast<std::time_t>(ms / 1000);
    const int frac = static_cast<int>(ms % 1000);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, frac < 0 ? 0 : frac);
    return buf;
}

void resolve_timestamp(const RecordInput& in, std::string& ts, std::int64_t& ts_ms) {
    ts_ms = in.ts_ms > 0 ? in.ts_ms : unix_ms_now();
    ts = !in.ts.empty() ? in.ts : iso_from_unix_ms(ts_ms);
}

std::string sanitize_id(std::string_view raw) {
    std::string o;
    o.reserve(raw.size());
    for (unsigned char c : raw) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.')
            o.push_back(static_cast<char>(c));
        else
            o.push_back('_');
    }
    if (o.empty()) o = "_none";
    if (o.size() > 80) o.resize(80);
    return o;
}

bool write_atomic(const fs::path& dest, const std::string& data, std::string& err) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    const fs::path tmp = dest.native() + media::path::path_from_utf8(".tmp~").native();
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "cannot open temp: " + media::path::path_to_generic_utf8(tmp);
            return false;
        }
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f.good()) {
            err = "temp write failed: " + media::path::path_to_generic_utf8(tmp);
            return false;
        }
    }
    fs::rename(tmp, dest, ec);
    if (ec) {
        fs::remove(dest, ec);
        fs::rename(tmp, dest, ec);
        if (ec) {
            fs::remove(tmp, ec);
            err = "rename failed: " + media::path::path_to_generic_utf8(dest);
            return false;
        }
    }
    return true;
}

nlohmann::json read_json_file(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return nlohmann::json::object();
    std::ifstream f(p, std::ios::binary);
    if (!f) return nlohmann::json::object();
    try {
        nlohmann::json j;
        f >> j;
        return j.is_object() ? j : nlohmann::json::object();
    } catch (...) {
        return nlohmann::json::object();
    }
}

std::vector<std::string> split_lines(std::string_view s) {
    std::vector<std::string> lines;
    std::string cur;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
            lines.push_back(std::move(cur));
            cur.clear();
        } else if (s[i] == '\n') {
            lines.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(s[i]);
        }
    }
    if (!cur.empty() || (!s.empty() && (s.back() == '\n' || s.back() == '\r')))
        lines.push_back(std::move(cur));
    else if (cur.size())
        lines.push_back(std::move(cur));
    return lines;
}

struct DiffStats {
    int plus = 0;
    int minus = 0;
    nlohmann::json hunks = nlohmann::json::array();
    std::string unified;
};

// Coarse but correct: one hunk from first to last differing line, plus 3
// lines of context. Enough for review later; not the agent edit protocol.
DiffStats diff_lines(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    DiffStats st;
    const int na = static_cast<int>(a.size());
    const int nb = static_cast<int>(b.size());
    int i = 0;
    int j = 0;
    while (i < na && j < nb && a[static_cast<std::size_t>(i)] == b[static_cast<std::size_t>(j)]) {
        ++i;
        ++j;
    }
    int i2 = na - 1;
    int j2 = nb - 1;
    while (i2 >= i && j2 >= j
           && a[static_cast<std::size_t>(i2)] == b[static_cast<std::size_t>(j2)]) {
        --i2;
        --j2;
    }
    if (i > i2 && j > j2)
        return st;

    const int ctx = 3;
    const int old_start = std::max(0, i - ctx);
    const int new_start = std::max(0, j - ctx);
    const int old_end = std::min(na, i2 + 1 + ctx);
    const int new_end = std::min(nb, j2 + 1 + ctx);
    const int old_count = old_end - old_start;
    const int new_count = new_end - new_start;

    nlohmann::json lines = nlohmann::json::array();
    std::ostringstream uni;
    uni << "@@ -" << (old_start + 1) << "," << old_count
        << " +" << (new_start + 1) << "," << new_count << " @@\n";

    int ai = old_start;
    int bi = new_start;
    while (ai < i && bi < j) {
        const std::string& ln = a[static_cast<std::size_t>(ai)];
        lines.push_back(" " + ln);
        uni << " " << ln << "\n";
        ++ai;
        ++bi;
    }
    while (ai <= i2 && ai < na) {
        const std::string& ln = a[static_cast<std::size_t>(ai)];
        lines.push_back("-" + ln);
        uni << "-" << ln << "\n";
        ++st.minus;
        ++ai;
    }
    while (bi <= j2 && bi < nb) {
        const std::string& ln = b[static_cast<std::size_t>(bi)];
        lines.push_back("+" + ln);
        uni << "+" << ln << "\n";
        ++st.plus;
        ++bi;
    }
    while (ai < old_end && bi < new_end && ai < na && bi < nb) {
        const std::string& ln = a[static_cast<std::size_t>(ai)];
        lines.push_back(" " + ln);
        uni << " " << ln << "\n";
        ++ai;
        ++bi;
    }

    st.hunks.push_back({
        {"old_start", old_start + 1},
        {"old_count", old_count},
        {"new_start", new_start + 1},
        {"new_count", new_count},
        {"lines", std::move(lines)},
    });
    st.unified = uni.str();
    return st;
}

fs::path object_path(const fs::path& root, const std::string& hex) {
    if (hex.size() < 2) return root / "objects" / "sha256" / hex;
    return root / "objects" / "sha256" / hex.substr(0, 2) / hex;
}

bool put_object(const fs::path& root, const std::string& hex, std::string_view bytes, std::string& err) {
    if (hex.empty()) return true;
    const fs::path dest = object_path(root, hex);
    std::error_code ec;
    if (fs::exists(dest, ec)) return true;
    return write_atomic(dest, std::string(bytes), err);
}

std::string strip_version_prefix(std::string_view v) {
    std::string s = trim_copy(std::string(v));
    if (s.size() > 7) {
        const std::string pre = s.substr(0, 7);
        std::string low = pre;
        low = media::strutil::lower_ascii(std::move(low));
        if (low == "sha256:") s = s.substr(7);
    }
    s = media::strutil::lower_ascii(std::move(s));
    return s;
}

} // namespace

void set_active_tool_call_id(const std::string& id) { tl_tool_call_id = id; }
void clear_active_tool_call_id() { tl_tool_call_id.clear(); }

void set_history_dir_override(const fs::path& dir) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_override = dir;
}

void clear_history_dir_override() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_override.reset();
}

std::filesystem::path default_local_history_dir() {
    return media::settings::get_config_dir() / "history";
}

std::filesystem::path history_root() {
    if (const char* env = std::getenv("PM_CHANGE_HISTORY_DIR"); env && *env)
        return media::path::path_from_utf8(env);
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_override && !g_override->empty())
            return *g_override;
    }
    return default_local_history_dir();
}

std::string content_sha256_hex(std::string_view bytes) {
    std::vector<std::uint8_t> v(bytes.begin(), bytes.end());
    return pm::storage::sha256_hex(v);
}

std::string version_token(std::string_view hex) {
    if (hex.empty()) return {};
    return "sha256:" + std::string(hex);
}

std::string file_uri_from_path(const fs::path& abs) {
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(abs, ec);
    if (ec || canon.empty()) canon = fs::absolute(abs, ec);
    std::string g = media::path::path_to_generic_utf8(canon);
    if (g.size() >= 2 && std::isalpha(static_cast<unsigned char>(g[0])) && g[1] == ':')
        return "file:///" + g;
    if (!g.empty() && g[0] == '/')
        return "file://" + g;
    return "file:///" + g;
}

bool check_expected_version(std::string_view current_bytes,
                            const nlohmann::json& args,
                            std::string& err) {
    if (!args.contains("expected_version") || !args["expected_version"].is_string())
        return true;
    const std::string want = strip_version_prefix(args["expected_version"].get<std::string>());
    if (want.empty()) return true;
    const std::string have = content_sha256_hex(current_bytes);
    if (have == want) return true;
    err = "stale version (expected sha256:" + want + ", current sha256:" + have
        + "); re-read the file";
    return false;
}

RecordResult record_mutation(const RecordInput& in) {
    RecordResult out;
    try {
        const fs::path root = history_root();
        if (root.empty()) {
            out.error = "history root is empty";
            return out;
        }
        const fs::path abs = in.path;
        const std::string uri = file_uri_from_path(abs);
        const std::string before_hex = content_sha256_hex(in.before);
        const std::string after_hex = content_sha256_hex(in.after);
        out.before_version = version_token(before_hex);
        out.after_version = version_token(after_hex);
        out.uri = uri;
        if (before_hex.empty() || after_hex.empty()) {
            out.error = "sha256 failed";
            return out;
        }

        const auto a_lines = split_lines(in.before);
        const auto b_lines = split_lines(in.after);
        const bool too_big = a_lines.size() + b_lines.size() > 8000
            || in.before.size() + in.after.size() > (512u * 1024u);
        DiffStats diff;
        if (!too_big)
            diff = diff_lines(a_lines, b_lines);
        out.plus = diff.plus;
        out.minus = diff.minus;
        out.hunks = diff.hunks;
        resolve_timestamp(in, out.ts, out.ts_ms);
        out.meta = in.meta.is_object() || in.meta.is_null()
            ? (in.meta.is_null() ? nlohmann::json::object() : in.meta)
            : nlohmann::json{{"value", in.meta}};

        std::string session = in.session_id;
        if (session.empty())
            session = media::llm::agent::get_current_task_id();
        out.session_id = session;
        const std::string sess_id = sanitize_id(session.empty() ? "_none" : session);
        const std::string seed = session + "\n" + uri + "\n" + before_hex + "\n" + after_hex + "\n"
            + out.ts + "\n" + in.tool + "\n" + tl_tool_call_id;
        const std::string change_hex = content_sha256_hex(seed);
        const std::string change_id = change_hex.size() >= 16 ? change_hex.substr(0, 16) : change_hex;
        out.change_id = change_id;

        std::string err;
        if (!put_object(root, before_hex, in.before, err)) {
            out.error = err;
            return out;
        }
        if (!put_object(root, after_hex, in.after, err)) {
            out.error = err;
            return out;
        }

        const std::string uri_hex = content_sha256_hex(uri);
        const fs::path file_man = root / "files" / (uri_hex + ".json");
        nlohmann::json fm = read_json_file(file_man);
        fm["schema"] = "pm.change.file.v1";
        fm["uri"] = uri;
        fm["path"] = media::path::path_to_generic_utf8(abs);
        fm["latest_version"] = out.after_version;
        if (!fm.contains("change_ids") || !fm["change_ids"].is_array())
            fm["change_ids"] = nlohmann::json::array();
        fm["change_ids"].push_back(change_id);
        fm["latest_change"] = nlohmann::json{
            {"id", change_id},
            {"session_id", session},
            {"state", "pending"},
            {"tool", in.tool},
            {"plus", out.plus},
            {"minus", out.minus},
        };
        if (!write_atomic(file_man, fm.dump(2), err)) {
            out.error = err;
            return out;
        }

        nlohmann::json rec{
            {"schema", "pm.change.v1"},
            {"id", change_id},
            {"state", "pending"},
            {"ts", out.ts},
            {"ts_ms", out.ts_ms},
            {"tool", in.tool},
            {"session_id", session},
            {"tool_call_id", tl_tool_call_id},
            {"uri", uri},
            {"path", media::path::path_to_generic_utf8(abs)},
            {"before", {{"sha256", before_hex}, {"object", "sha256:" + before_hex}}},
            {"after", {{"sha256", after_hex}, {"object", "sha256:" + after_hex}}},
            {"stats", {{"plus", out.plus}, {"minus", out.minus}}},
            {"hunks", out.hunks},
            {"meta", out.meta},
        };
        if (!diff.unified.empty())
            rec["diff"] = diff.unified;
        if (too_big)
            rec["diff_skipped"] = "too_large";

        const fs::path rec_path = root / "sessions" / sess_id / (change_id + ".json");
        if (!write_atomic(rec_path, rec.dump(2), err)) {
            out.error = err;
            return out;
        }

        const fs::path cs_path = root / "sessions" / sess_id / "changeset.json";
        nlohmann::json cs = read_json_file(cs_path);
        cs["schema"] = "pm.change.session.v1";
        cs["session_id"] = session;
        if (!cs.contains("changes") || !cs["changes"].is_array())
            cs["changes"] = nlohmann::json::array();
        nlohmann::json cs_row{
            {"id", change_id},
            {"path", media::path::path_to_generic_utf8(abs)},
            {"tool", in.tool},
            {"plus", out.plus},
            {"minus", out.minus},
            {"state", "pending"},
            {"ts", out.ts},
            {"ts_ms", out.ts_ms},
        };
        if (!out.meta.empty())
            cs_row["meta"] = out.meta;
        cs["changes"].push_back(std::move(cs_row));
        if (!write_atomic(cs_path, cs.dump(2), err)) {
            out.error = err;
            return out;
        }
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    } catch (...) {
        out.error = "record_mutation failed";
    }
    return out;
}

namespace {

std::string read_object_bytes(const fs::path& root, const std::string& hex, std::string& err) {
    if (hex.empty()) {
        err = "empty object hash";
        return {};
    }
    const fs::path dest = object_path(root, hex);
    std::ifstream f(dest, std::ios::binary);
    if (!f) {
        err = "object missing: " + media::path::path_to_generic_utf8(dest);
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string hex_from_record_side(const nlohmann::json& rec, const char* side) {
    if (!rec.contains(side) || !rec[side].is_object()) return {};
    if (rec[side].contains("sha256") && rec[side]["sha256"].is_string())
        return strip_version_prefix(rec[side]["sha256"].get<std::string>());
    if (rec[side].contains("object") && rec[side]["object"].is_string())
        return strip_version_prefix(rec[side]["object"].get<std::string>());
    return {};
}

nlohmann::json load_record_file(const fs::path& root, const std::string& session_id,
                                const std::string& change_id) {
    if (session_id.empty() || change_id.empty()) return nlohmann::json::object();
    const fs::path p = root / "sessions" / sanitize_id(session_id) / (change_id + ".json");
    return read_json_file(p);
}

nlohmann::json scan_record_by_id(const fs::path& root, const std::string& change_id) {
    if (change_id.empty()) return nlohmann::json::object();
    std::error_code ec;
    const fs::path sessions = root / "sessions";
    if (!fs::exists(sessions, ec)) return nlohmann::json::object();
    for (fs::directory_iterator it(sessions, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        nlohmann::json rec = read_json_file(it->path() / (change_id + ".json"));
        if (rec.contains("id") && rec["id"].is_string())
            return rec;
    }
    return nlohmann::json::object();
}

bool paths_same_uri(const fs::path& a, const std::string& stored) {
    if (stored.empty()) return false;
    const std::string ua = file_uri_from_path(a);
    const std::string ub = file_uri_from_path(media::path::path_from_utf8(stored));
    if (!ua.empty() && ua == ub) return true;
    std::error_code ec;
    fs::path ca = fs::weakly_canonical(a, ec);
    fs::path cb = fs::weakly_canonical(media::path::path_from_utf8(stored), ec);
    return !ca.empty() && ca == cb;
}

nlohmann::json summary_from_record(const nlohmann::json& rec) {
    if (!rec.contains("id")) return nlohmann::json::object();
    nlohmann::json s{
        {"id", rec.value("id", std::string{})},
        {"session_id", rec.value("session_id", std::string{})},
        {"path", rec.value("path", std::string{})},
        {"uri", rec.value("uri", std::string{})},
        {"tool", rec.value("tool", std::string{})},
        {"state", rec.value("state", std::string{})},
        {"ts", rec.value("ts", std::string{})},
        {"ts_ms", rec.value("ts_ms", std::int64_t{0})},
    };
    if (rec.contains("stats") && rec["stats"].is_object()) {
        s["plus"] = rec["stats"].value("plus", 0);
        s["minus"] = rec["stats"].value("minus", 0);
    }
    if (rec.contains("meta") && rec["meta"].is_object())
        s["meta"] = rec["meta"];
    if (rec.contains("hunks") && rec["hunks"].is_array())
        s["hunks"] = rec["hunks"];
    return s;
}

std::vector<nlohmann::json> collect_pending_records(const fs::path& abs) {
    std::vector<nlohmann::json> out;
    const fs::path root = history_root();
    if (root.empty()) return out;
    std::error_code ec;
    const fs::path sessions = root / "sessions";
    if (!fs::exists(sessions, ec)) return out;
    for (fs::directory_iterator it(sessions, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const nlohmann::json cs = read_json_file(it->path() / "changeset.json");
        if (!cs.contains("changes") || !cs["changes"].is_array()) continue;
        for (const auto& row : cs["changes"]) {
            if (!row.is_object() || row.value("state", std::string{}) != "pending") continue;
            if (!paths_same_uri(abs, row.value("path", std::string{}))) continue;
            nlohmann::json rec = load_record_file(root,
                cs.value("session_id", std::string{}),
                row.value("id", std::string{}));
            if (rec.value("state", std::string{}) == "pending")
                out.push_back(std::move(rec));
        }
    }
    std::sort(out.begin(), out.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return a.value("ts_ms", std::int64_t{0}) < b.value("ts_ms", std::int64_t{0});
    });
    return out;
}

nlohmann::json find_pending_record(const fs::path& abs) {
    const auto all = collect_pending_records(abs);
    if (all.empty()) return nlohmann::json::object();
    return all.back();
}

constexpr std::size_t k_review_text_cap = 512u * 1024u;

nlohmann::json find_record_on_path(const fs::path& abs, const std::string& change_id) {
    if (change_id.empty()) return find_pending_record(abs);
    const fs::path root = history_root();
    for (const auto& rec : collect_pending_records(abs)) {
        if (rec.value("id", std::string{}) == change_id)
            return rec;
    }
    nlohmann::json rec = scan_record_by_id(root, change_id);
    if (rec.contains("id") && paths_same_uri(abs, rec.value("path", std::string{})))
        return rec;
    return nlohmann::json::object();
}

PendingChange fill_pending(const nlohmann::json& rec, const std::vector<nlohmann::json>& all) {
    PendingChange out;
    out.pending_count = static_cast<int>(all.size());
    out.changes = nlohmann::json::array();
    for (const auto& item : all)
        out.changes.push_back(summary_from_record(item));
    if (!rec.contains("id"))
        return out;
    out.found = true;
    out.record = summary_from_record(rec);
    const fs::path root = history_root();
    std::string err;
    std::string before = read_object_bytes(root, hex_from_record_side(rec, "before"), err);
    if (!err.empty()) {
        out.error = err;
        return out;
    }
    err.clear();
    std::string after = read_object_bytes(root, hex_from_record_side(rec, "after"), err);
    if (!err.empty()) {
        out.error = err;
        return out;
    }
    if (before.size() + after.size() > k_review_text_cap * 2) {
        out.text_skipped = true;
        return out;
    }
    out.before_text = std::move(before);
    out.after_text = std::move(after);
    return out;
}

bool update_record_state(const fs::path& root, nlohmann::json& rec, const std::string& state,
                         std::string& err) {
    rec["state"] = state;
    const std::string session = rec.value("session_id", std::string{});
    const std::string id = rec.value("id", std::string{});
    if (session.empty() || id.empty()) {
        err = "record missing session_id or id";
        return false;
    }
    const fs::path rec_path = root / "sessions" / sanitize_id(session) / (id + ".json");
    if (!write_atomic(rec_path, rec.dump(2), err))
        return false;

    const fs::path cs_path = root / "sessions" / sanitize_id(session) / "changeset.json";
    nlohmann::json cs = read_json_file(cs_path);
    if (cs.contains("changes") && cs["changes"].is_array()) {
        for (auto& row : cs["changes"]) {
            if (row.is_object() && row.value("id", std::string{}) == id)
                row["state"] = state;
        }
        if (!write_atomic(cs_path, cs.dump(2), err))
            return false;
    }

    const std::string uri = rec.value("uri", std::string{});
    if (!uri.empty()) {
        const fs::path file_man = root / "files" / (content_sha256_hex(uri) + ".json");
        nlohmann::json fm = read_json_file(file_man);
        if (fm.contains("latest_change") && fm["latest_change"].is_object()
            && fm["latest_change"].value("id", std::string{}) == id) {
            fm["latest_change"]["state"] = state;
            if (!write_atomic(file_man, fm.dump(2), err))
                return false;
        }
    }
    return true;
}

} // namespace

nlohmann::json pending_summary_for_path(const fs::path& abs) {
    try {
        const auto all = collect_pending_records(abs);
        nlohmann::json s = all.empty() ? nlohmann::json::object() : summary_from_record(all.back());
        if (!s.empty())
            s["pending_count"] = static_cast<int>(all.size());
        return s;
    } catch (...) {
        return nlohmann::json::object();
    }
}

nlohmann::json list_pending_for_path(const fs::path& abs) {
    nlohmann::json arr = nlohmann::json::array();
    try {
        for (const auto& rec : collect_pending_records(abs))
            arr.push_back(summary_from_record(rec));
    } catch (...) {
    }
    return arr;
}

PendingChange load_pending_for_path(const fs::path& abs) {
    return load_pending_for_path(abs, {});
}

PendingChange load_pending_for_path(const fs::path& abs, const std::string& change_id) {
    PendingChange out;
    try {
        if (history_root().empty()) {
            out.error = "history root is empty";
            return out;
        }
        const auto all = collect_pending_records(abs);
        nlohmann::json rec = change_id.empty()
            ? (all.empty() ? nlohmann::json::object() : all.back())
            : find_record_on_path(abs, change_id);
        return fill_pending(rec, all);
    } catch (const std::exception& e) {
        out.error = e.what();
    } catch (...) {
        out.error = "load_pending_for_path failed";
    }
    return out;
}

namespace {

std::string read_file_bytes(const fs::path& abs, std::string& err) {
    std::ifstream f(abs, std::ios::binary);
    if (!f) {
        err = "cannot read current file";
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int remaining_pending_count(const fs::path& abs) {
    try {
        return static_cast<int>(collect_pending_records(abs).size());
    } catch (...) {
        return 0;
    }
}

void collect_record_object_hexes(const nlohmann::json& rec, std::vector<std::string>& out) {
    const std::string before = hex_from_record_side(rec, "before");
    const std::string after = hex_from_record_side(rec, "after");
    if (!before.empty()) out.push_back(before);
    if (!after.empty()) out.push_back(after);
}

bool record_matches_path(const nlohmann::json& rec, const fs::path& abs) {
    if (paths_same_uri(abs, rec.value("path", std::string{})))
        return true;
    const std::string uri = rec.value("uri", std::string{});
    return !uri.empty() && uri == file_uri_from_path(abs);
}

bool is_change_record_file(const fs::path& p) {
    const std::string name = media::path::path_to_utf8(p.filename());
    if (name == "changeset.json" || name.find(".tmp~") != std::string::npos)
        return false;
    return name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0;
}

bool object_referenced_anywhere(const fs::path& root, const std::string& hex) {
    if (hex.empty()) return false;
    std::error_code ec;
    const fs::path sessions = root / "sessions";
    if (!fs::exists(sessions, ec)) return false;
    if (ec) return true;
    for (fs::directory_iterator it(sessions, ec); it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) return true;
        if (!it->is_directory(ec)) {
            if (ec) return true;
            continue;
        }
        std::error_code rec_ec;
        for (fs::directory_iterator rit(it->path(), rec_ec);
             rit != fs::directory_iterator(); rit.increment(rec_ec)) {
            if (rec_ec) return true;
            if (!rit->is_regular_file(rec_ec) || !is_change_record_file(rit->path())) {
                if (rec_ec) return true;
                continue;
            }
            const nlohmann::json rec = read_json_file(rit->path());
            if (hex_from_record_side(rec, "before") == hex)
                return true;
            if (hex_from_record_side(rec, "after") == hex)
                return true;
        }
        if (rec_ec) return true;
    }
    return ec ? true : false;
}

void try_remove_empty_dir(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    if (fs::is_empty(dir, ec) && !ec)
        fs::remove(dir, ec);
}

void gc_resolved_for_path(const fs::path& abs) {
    try {
        if (abs.empty()) return;
        if (remaining_pending_count(abs) > 0) return;
        const fs::path root = history_root();
        if (root.empty()) return;
        std::error_code ec;
        const fs::path sessions = root / "sessions";
        if (!fs::exists(sessions, ec)) return;

        std::vector<fs::path> sess_dirs;
        for (fs::directory_iterator it(sessions, ec); !ec && it != fs::directory_iterator();
             it.increment(ec)) {
            if (it->is_directory(ec))
                sess_dirs.push_back(it->path());
        }

        struct Victim {
            fs::path rec_path;
            std::string id;
        };
        std::vector<Victim> victims;
        std::set<std::string> removed_ids;
        std::vector<std::string> object_hexes;
        std::string uri;

        struct SessionRewrite {
            fs::path cs_path;
            fs::path sess_dir;
            nlohmann::json cs;
            bool empty = false;
        };
        std::vector<SessionRewrite> rewrites;

        for (const auto& sess_dir : sess_dirs) {
            const fs::path cs_path = sess_dir / "changeset.json";
            nlohmann::json cs = read_json_file(cs_path);
            nlohmann::json kept = nlohmann::json::array();
            bool touched = false;
            if (cs.contains("changes") && cs["changes"].is_array()) {
                for (const auto& row : cs["changes"]) {
                    if (!row.is_object()) continue;
                    const std::string path = row.value("path", std::string{});
                    const std::string id = row.value("id", std::string{});
                    if (!paths_same_uri(abs, path)) {
                        kept.push_back(row);
                        continue;
                    }
                    if (row.value("state", std::string{}) == "pending")
                        return;
                    nlohmann::json rec = load_record_file(root,
                        cs.value("session_id", std::string{}), id);
                    if (rec.value("state", std::string{}) == "pending")
                        return;
                    touched = true;
                    if (uri.empty())
                        uri = rec.value("uri", std::string{});
                    collect_record_object_hexes(rec, object_hexes);
                    if (!id.empty()) {
                        removed_ids.insert(id);
                        victims.push_back({sess_dir / (id + ".json"), id});
                    }
                }
            }
            if (touched) {
                SessionRewrite rw;
                rw.cs_path = cs_path;
                rw.sess_dir = sess_dir;
                cs["changes"] = std::move(kept);
                rw.empty = !cs.contains("changes") || !cs["changes"].is_array()
                    || cs["changes"].empty();
                rw.cs = std::move(cs);
                rewrites.push_back(std::move(rw));
            }
        }

        for (const auto& sess_dir : sess_dirs) {
            std::error_code rec_ec;
            for (fs::directory_iterator rit(sess_dir, rec_ec);
                 !rec_ec && rit != fs::directory_iterator(); rit.increment(rec_ec)) {
                if (!rit->is_regular_file(rec_ec) || !is_change_record_file(rit->path()))
                    continue;
                const nlohmann::json rec = read_json_file(rit->path());
                if (!rec.contains("id") || !record_matches_path(rec, abs))
                    continue;
                const std::string id = rec.value("id", std::string{});
                if (rec.value("state", std::string{}) == "pending")
                    return;
                if (removed_ids.count(id))
                    continue;
                if (uri.empty())
                    uri = rec.value("uri", std::string{});
                collect_record_object_hexes(rec, object_hexes);
                if (!id.empty())
                    removed_ids.insert(id);
                victims.push_back({rit->path(), id});
            }
        }

        if (victims.empty())
            return;

        for (const auto& v : victims)
            fs::remove(v.rec_path, ec);

        for (auto& rw : rewrites) {
            if (rw.empty) {
                fs::remove(rw.cs_path, ec);
                try_remove_empty_dir(rw.sess_dir);
            } else {
                std::string err;
                write_atomic(rw.cs_path, rw.cs.dump(2), err);
            }
        }
        for (const auto& sess_dir : sess_dirs)
            try_remove_empty_dir(sess_dir);

        if (uri.empty())
            uri = file_uri_from_path(abs);
        if (!uri.empty()) {
            const fs::path file_man = root / "files" / (content_sha256_hex(uri) + ".json");
            nlohmann::json fm = read_json_file(file_man);
            if (fm.contains("change_ids") && fm["change_ids"].is_array()) {
                nlohmann::json kept = nlohmann::json::array();
                for (const auto& id : fm["change_ids"]) {
                    if (!id.is_string()) continue;
                    if (removed_ids.count(id.get<std::string>()))
                        continue;
                    kept.push_back(id);
                }
                if (kept.empty()) {
                    fs::remove(file_man, ec);
                    try_remove_empty_dir(file_man.parent_path());
                } else {
                    fm["change_ids"] = std::move(kept);
                    if (fm.contains("latest_change") && fm["latest_change"].is_object()
                        && removed_ids.count(fm["latest_change"].value("id", std::string{}))) {
                        fm.erase("latest_change");
                    }
                    std::string err;
                    write_atomic(file_man, fm.dump(2), err);
                }
            } else if (fs::exists(file_man, ec)) {
                fs::remove(file_man, ec);
                try_remove_empty_dir(file_man.parent_path());
            }
        }

        std::sort(object_hexes.begin(), object_hexes.end());
        object_hexes.erase(std::unique(object_hexes.begin(), object_hexes.end()),
                           object_hexes.end());
        for (const auto& hex : object_hexes) {
            if (object_referenced_anywhere(root, hex))
                continue;
            const fs::path dest = object_path(root, hex);
            fs::remove(dest, ec);
            try_remove_empty_dir(dest.parent_path());
        }
    } catch (...) {
    }
}

} // namespace

ResolveResult resolve_change(const std::string& session_id,
                             const std::string& change_id,
                             ResolveAction action) {
    ResolveResult out;
    try {
        const fs::path root = history_root();
        if (root.empty()) {
            out.error = "history root is empty";
            return out;
        }
        nlohmann::json rec = load_record_file(root, session_id, change_id);
        if (!rec.contains("id"))
            rec = scan_record_by_id(root, change_id);
        if (!rec.contains("id")) {
            out.error = "change not found";
            return out;
        }
        const std::string cur_state = rec.value("state", std::string{});
        if (cur_state != "pending") {
            out.error = "change is not pending (state=" + cur_state + ")";
            return out;
        }
        const std::string path = rec.value("path", std::string{});
        const fs::path abs(path);
        auto all = path.empty() ? std::vector<nlohmann::json>{} : collect_pending_records(abs);
        const std::int64_t rec_ts = rec.value("ts_ms", std::int64_t{0});
        const std::string next = action == ResolveAction::Accept ? "accepted" : "rejected";
        if (action == ResolveAction::Reject) {
            if (path.empty()) {
                out.error = "record has no path";
                return out;
            }
            const std::string deny = media::llm::llm_fs_guard_write_deny_reason(abs);
            if (!deny.empty()) {
                out.error = deny;
                return out;
            }
            std::string err;
            const std::string have = read_file_bytes(abs, err);
            if (!err.empty()) {
                out.error = err;
                return out;
            }
            const nlohmann::json tip = all.empty() ? rec : all.back();
            const std::string tip_hex = hex_from_record_side(tip, "after");
            if (content_sha256_hex(have) != tip_hex) {
                out.error = "file changed since this edit; re-read before rejecting";
                return out;
            }
            const std::string before = read_object_bytes(root, hex_from_record_side(rec, "before"), err);
            if (!err.empty()) {
                out.error = err;
                return out;
            }
            if (!write_atomic(abs, before, err)) {
                out.error = err;
                return out;
            }
            out.applied_text = before;
            for (auto& newer : all) {
                if (newer.value("id", std::string{}) == rec.value("id", std::string{}))
                    continue;
                if (newer.value("ts_ms", std::int64_t{0}) < rec_ts)
                    continue;
                if (!update_record_state(root, newer, "rejected", err)) {
                    out.error = err;
                    return out;
                }
            }
        } else {
            std::string err;
            out.applied_text = read_file_bytes(abs, err);
        }
        std::string err;
        if (!update_record_state(root, rec, next, err)) {
            out.error = err;
            return out;
        }
        out.ok = true;
        out.state = next;
        out.remaining_pending = remaining_pending_count(abs);
        if (out.remaining_pending == 0)
            gc_resolved_for_path(abs);
    } catch (const std::exception& e) {
        out.error = e.what();
    } catch (...) {
        out.error = "resolve_change failed";
    }
    return out;
}

ResolveResult resolve_all_for_path(const fs::path& abs, ResolveAction action) {
    ResolveResult out;
    try {
        const fs::path root = history_root();
        if (root.empty()) {
            out.error = "history root is empty";
            return out;
        }
        auto all = collect_pending_records(abs);
        if (all.empty()) {
            out.ok = true;
            out.state = action == ResolveAction::Accept ? "accepted" : "rejected";
            gc_resolved_for_path(abs);
            return out;
        }
        const std::string next = action == ResolveAction::Accept ? "accepted" : "rejected";
        if (action == ResolveAction::Reject) {
            const std::string deny = media::llm::llm_fs_guard_write_deny_reason(abs);
            if (!deny.empty()) {
                out.error = deny;
                return out;
            }
            std::string err;
            const std::string have = read_file_bytes(abs, err);
            if (!err.empty()) {
                out.error = err;
                return out;
            }
            const std::string tip_hex = hex_from_record_side(all.back(), "after");
            if (content_sha256_hex(have) != tip_hex) {
                out.error = "file changed since these edits; re-read before rejecting";
                return out;
            }
            const std::string before = read_object_bytes(root, hex_from_record_side(all.front(), "before"), err);
            if (!err.empty()) {
                out.error = err;
                return out;
            }
            if (!write_atomic(abs, before, err)) {
                out.error = err;
                return out;
            }
            out.applied_text = before;
        } else {
            std::string err;
            out.applied_text = read_file_bytes(abs, err);
        }
        for (auto& rec : all) {
            std::string err;
            if (!update_record_state(root, rec, next, err)) {
                out.error = err;
                return out;
            }
        }
        out.ok = true;
        out.state = next;
        out.remaining_pending = 0;
        gc_resolved_for_path(abs);
    } catch (const std::exception& e) {
        out.error = e.what();
    } catch (...) {
        out.error = "resolve_all_for_path failed";
    }
    return out;
}

void attach_record_fields(nlohmann::json& result, const RecordResult& rec) {
    if (!rec.after_version.empty())
        result["version"] = rec.after_version;
    if (!rec.before_version.empty())
        result["before_version"] = rec.before_version;
    if (!rec.session_id.empty())
        result["session_id"] = rec.session_id;
    if (!rec.change_id.empty())
        result["change_id"] = rec.change_id;
    if (!rec.ts.empty())
        result["ts"] = rec.ts;
    if (rec.ts_ms > 0)
        result["ts_ms"] = rec.ts_ms;
    if (!rec.meta.empty())
        result["meta"] = rec.meta;
    result["plus"] = rec.plus;
    result["minus"] = rec.minus;
    if (!rec.ok)
        result["history_ok"] = false;
}

} // namespace media::llm::change

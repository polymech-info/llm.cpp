#include "agent_internal.hpp"

#include "constants.hpp"
#include "string/string_sanitizers.hpp"
#include "llm_constants.hpp"
#include "path_tool_catalog.hpp"
#include "agent_memory.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

namespace media::llm::agent::detail {

namespace {

/// Per-item summary char cap at *render* time. The store-side clamp is
/// k_session_item_summary_chars (400) which is fine for persistence but
/// wasteful in the live prompt — the model rarely needs more than the head
/// of a summary to recognise the item. Aggressively trim here.
constexpr std::size_t k_inject_item_summary_chars = 140;

std::string truncate_inline(const std::string& s, std::size_t max_chars)
{
    if (s.size() <= max_chars)
        return s;
    return media::str_san::utf8_safe_ellipsize(s, max_chars);
}

/// #10 staleness probe. Compares the recorded size_bytes / mtime_epoch on
/// a file artifact against the live disk state and returns a short tag
/// (missing/resized/modified) when they disagree, or empty when fresh.
///
/// The renderer surfaces this as a `stale=<reason>` field next to the
/// other locator fields, so the LLM can decide to re-read instead of
/// trusting the cached summary. Items captured before this stamp landed
/// silently skip the check (no size_bytes / mtime_epoch in metadata).
std::string probe_file_artifact_staleness(const nlohmann::json& item)
{
    if (!item.is_object()) return {};
    const std::string kind = json_child_string(item, "kind");
    if (kind != "file") return {};
    const std::string path = json_child_string(json_child_object(item, "locator"), "path");
    if (path.empty()) return {};
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path fsp(path);
    if (!fs::is_regular_file(fsp, ec)) {
        if (ec == std::errc::permission_denied)
            return "permission";
        if (ec == std::errc::no_such_file_or_directory)
            return "missing";
        std::error_code exists_ec;
        if (!fs::exists(fsp, exists_ec) || exists_ec)
            return exists_ec == std::errc::permission_denied ? "permission" : "missing";
        return "unavailable";
    }
    const auto& meta = json_child_object(item, "metadata");
    if (meta.contains("size_bytes") && meta["size_bytes"].is_number()) {
        std::error_code sec;
        const auto sz = fs::file_size(fsp, sec);
        if (!sec && static_cast<long long>(sz) != json_child_int64(meta, "size_bytes"))
            return "resized";
    }
    if (meta.contains("mtime_epoch") && meta["mtime_epoch"].is_number()) {
        std::error_code mec;
        const auto mt = fs::last_write_time(fsp, mec);
        if (!mec) {
            const auto sys_t = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                mt - decltype(mt)::clock::now() + std::chrono::system_clock::now());
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(sys_t.time_since_epoch()).count();
            const long long stamped = json_child_int64(meta, "mtime_epoch");
            if (secs > stamped + 1 || secs < stamped - 1)  // 1-sec slop for FAT
                return "modified";
        }
    }
    return {};
}

void append_one_session_item(std::ostringstream& s, const std::string& id, const nlohmann::json& item)
{
    if (!item.is_object()) return;
    s << "- " << id
      << ": kind=" << json_child_string(item, "kind", "unknown");
    const std::string role = json_child_string(item, "role");
    if (!role.empty() && role != "unknown")
        s << " role=" << role;
    if (item.contains("locator") && item["locator"].is_object()) {
        const auto& loc = item["locator"];
        if (loc.contains("path") && loc["path"].is_string())
            s << " path=" << loc["path"].get<std::string>();
        if (loc.contains("index") && loc["index"].is_string())
            s << " index=" << loc["index"].get<std::string>();
        if (loc.contains("url") && loc["url"].is_string())
            s << " url=" << loc["url"].get<std::string>();
        if (loc.contains("blob_ref") && loc["blob_ref"].is_string())
            s << " blob_ref=" << loc["blob_ref"].get<std::string>();
    } else if (item.contains("path") && item["path"].is_string()) {
        s << " path=" << item["path"].get<std::string>();
    }
    if (item.contains("content_type") && item["content_type"].is_string())
        s << " content_type=" << item["content_type"].get<std::string>();
    if (item.contains("schema_hint") && item["schema_hint"].is_string())
        s << " schema=" << item["schema_hint"].get<std::string>();
    if (item.contains("metadata") && item["metadata"].is_object()) {
        const auto& m = item["metadata"];
        if (m.contains("count"))
            s << " count=" << m["count"].dump();
        if (m.contains("source_pattern") && m["source_pattern"].is_string())
            s << " source_pattern=" << m["source_pattern"].get<std::string>();
        if (json_child_string(item, "kind") == "tool_checkpoint") {
            if (m.contains("exit_code"))
                s << " exit_code=" << m["exit_code"].dump();
            if (m.contains("command") && m["command"].is_string())
                s << " command="
                  << truncate_inline(m["command"].get<std::string>(), 120);
        }
        if (json_child_bool(m, "body_available")) {
            s << " body_available=true";
            if (m.contains("assistant_chars"))
                s << " assistant_chars=" << m["assistant_chars"].dump();
        }
        if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
            bool first = true;
            for (const auto& call : m["tool_calls"]) {
                if (!call.is_object() ||
                    !json_child_bool(call, "payload_available") ||
                    json_child_bool(call, "payload_denied"))
                    continue;
                const std::string call_id = json_child_string(call, "id");
                const std::string name = json_child_string(call, "name");
                if (call_id.empty()) continue;
                s << (first ? " payload_calls=" : ",");
                if (!name.empty()) s << name << ":";
                s << call_id;
                if (call.contains("command") && call["command"].is_string())
                    s << "{command="
                      << truncate_inline(call["command"].get<std::string>(), 120)
                      << "}";
                first = false;
            }
        }
        if (json_child_bool(m, "payload_available") &&
            !json_child_bool(m, "payload_denied") &&
            m.contains("transcript_tool_call_id") &&
            m["transcript_tool_call_id"].is_string()) {
            s << " payload_call_id="
              << m["transcript_tool_call_id"].get<std::string>();
            if (item.contains("origin") && item["origin"].is_object()) {
                const std::string tool_name =
                    json_child_string(item["origin"], "tool_name");
                if (!tool_name.empty()) s << " payload_tool=" << tool_name;
            }
        }
    }
    // #10 live staleness check. Cheap stat per item; nothing is mutated
    // in memory_state — the tag is renderer-only.
    if (const std::string st = probe_file_artifact_staleness(item); !st.empty())
        s << " stale=" << st;
    if (item.contains("summary") && item["summary"].is_string())
        s << " — " << truncate_inline(item["summary"].get<std::string>(), k_inject_item_summary_chars);
    s << "\n";
}

/// Sort {id, value} pairs by recency. Primary key is `last_used_at` (LRU-
/// aware so newly bumped items rise to the top regardless of original
/// creation time); secondary key is `created_at`. Items lacking both sink
/// to the bottom but stay visible.
std::vector<std::pair<std::string, const nlohmann::json*>> sort_items_newest_first(const nlohmann::json& rows)
{
    std::vector<std::pair<std::string, const nlohmann::json*>> out;
    out.reserve(rows.size());
    for (auto it = rows.begin(); it != rows.end(); ++it) {
        if (it.value().is_object()) {
            const std::string kind = json_child_string(it.value(), "kind");
            const std::string role = json_child_string(it.value(), "role");
            // Internal bookkeeping is useful in logs but should not displace
            // file/media locators and turn pairs from the small live prompt.
            if (kind == "self_check" || role == "security_review" || role == "security_decision")
                continue;
        }
        out.emplace_back(it.key(), &it.value());
    }
    auto ts = [](const nlohmann::json& j, const char* key) -> std::string {
        if (!j.is_object()) return {};
        auto it = j.find(key);
        return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string{};
    };
    std::sort(out.begin(), out.end(), [&](const auto& a, const auto& b) {
        const std::string la = ts(*a.second, "last_used_at");
        const std::string lb = ts(*b.second, "last_used_at");
        const std::string ca = ts(*a.second, "created_at");
        const std::string cb = ts(*b.second, "created_at");
        const std::string ka = la.empty() ? ca : la;
        const std::string kb = lb.empty() ? cb : lb;
        if (ka.empty() != kb.empty()) return !ka.empty();
        return ka > kb;
    });
    return out;
}

std::string format_stored_literal(const nlohmann::json& value)
{
    if (value.is_string())
        return media::str_san::utf8_safe_ellipsize(value.get<std::string>(), 160, "\u2026");
    if (!value.is_array() || value.empty())
        return {};
    std::string out;
    for (const auto& entry : value) {
        std::string piece;
        if (entry.is_string())
            piece = entry.get<std::string>();
        else if (entry.is_object()) {
            const std::string name = json_child_string(entry, "name");
            const std::string exe  = json_child_string(entry, "exe");
            if (!name.empty() && !exe.empty())
                piece = name + ":" + exe;
            else
                piece = !name.empty() ? name : exe;
        }
        if (piece.empty())
            continue;
        if (!out.empty())
            out += ", ";
        out += piece;
    }
    return media::str_san::utf8_safe_ellipsize(out, 160, "\u2026");
}

} // namespace

void append_session_items(std::ostringstream& s,
                          const nlohmann::json& memory_state,
                          std::size_t chars_budget,
                          std::size_t max_items)
{
    if (chars_budget == 0) chars_budget = pm::llm::k_session_items_inject_chars;
    if (max_items == 0)    max_items    = pm::llm::k_session_items_inject_max;
    if (!memory_state.is_object())
        return;
    const bool has_items = memory_state.contains("items") && memory_state["items"].is_object() && !memory_state["items"].empty();
    const bool has_artifacts = memory_state.contains("artifacts") && memory_state["artifacts"].is_object() && !memory_state["artifacts"].empty();
    if (!has_items && !has_artifacts)
        return;

    const auto& rows = has_items ? memory_state["items"] : memory_state["artifacts"];
    bool has_retrievable_body = false;
    bool has_retrievable_payload = false;
    for (auto it = rows.begin(); it != rows.end(); ++it) {
        if (it.value().is_object() && it.value().contains("metadata") &&
            it.value()["metadata"].is_object() &&
            json_child_bool(it.value()["metadata"], "body_available")) {
            has_retrievable_body = true;
        }
        if (it.value().is_object() && it.value().contains("metadata") &&
            it.value()["metadata"].is_object()) {
            const auto& metadata = it.value()["metadata"];
            if (json_child_bool(metadata, "payload_available") &&
                !json_child_bool(metadata, "payload_denied"))
                has_retrievable_payload = true;
            const auto calls = json_child_array(metadata, "tool_calls");
            if (!calls.empty()) {
                for (const auto& call : calls) {
                    if (call.is_object() &&
                        json_child_bool(call, "payload_available") &&
                        !json_child_bool(call, "payload_denied")) {
                        has_retrievable_payload = true;
                        break;
                    }
                }
            }
        }
        if (has_retrievable_body && has_retrievable_payload) break;
    }

    std::ostringstream block;
    block << "\n## Session Items\n"
             "Use these durable references by role, origin, locator, and recency before asking the user for previous outputs. Newest first.\n";
    if (has_retrievable_body)
        block << "For body_available=true, recover the exact prior reply with memory_find(itemId=<turn id>, includeBody=true) before writing or exporting it.\n";
    if (has_retrievable_payload)
        block << "For payload_call(s), recover one exact same-session tool result with memory_find(itemId=<item id>, toolCallId=<call id>, includePayload=true). Do not rerun the tool.\n";

    const auto sorted = sort_items_newest_first(rows);
    std::size_t shown = 0;
    bool truncated = false;
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        if (shown >= max_items) { truncated = (i < sorted.size()); break; }
        // Conversation pairs have their own ## Recent turns block.
        if (sorted[i].second->value("role", std::string{}) == "turn_pair")
            continue;
        std::ostringstream line;
        append_one_session_item(line, sorted[i].first, *sorted[i].second);
        const std::string ln = line.str();
        if (block.tellp() > 0 && static_cast<std::size_t>(block.tellp()) + ln.size() > chars_budget) {
            truncated = true;
            continue;
        }
        block << ln;
        ++shown;
    }
    if (shown == 0)
        return;
    if (truncated)
        block << "- ... more session items omitted (budget) ...\n";
    s << block.str();
}

namespace {

/// Newlines become spaces so a prompt or reply occupies at most two lines.
std::string recent_turn_text(std::string text, std::size_t max_chars)
{
    std::string flat;
    flat.reserve(text.size());
    bool pending_space = false;
    for (unsigned char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            pending_space = !flat.empty();
            continue;
        }
        if (pending_space) {
            flat.push_back(' ');
            pending_space = false;
        }
        flat.push_back(static_cast<char>(c));
    }
    return media::str_san::utf8_safe_ellipsize(std::move(flat), max_chars, "\u2026");
}

} // namespace

void append_recent_turns(std::ostringstream& s,
                         const nlohmann::json& memory_state,
                         std::size_t max_turns,
                         bool advertise_body_recall)
{
    if (max_turns == 0) max_turns = pm::llm::k_recent_turns_inject_max;
    if (!memory_state.is_object() || !memory_state.contains("items") ||
        !memory_state["items"].is_object() || max_turns == 0)
        return;

    std::vector<std::pair<std::string, const nlohmann::json*>> pairs;
    for (auto it = memory_state["items"].begin(); it != memory_state["items"].end(); ++it) {
        if (!it.value().is_object())
            continue;
        if (json_child_string(it.value(), "role") != "turn_pair")
            continue;
        pairs.emplace_back(it.key(), &it.value());
    }
    std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) {
        const std::string ca = json_child_string(*a.second, "created_at");
        const std::string cb = json_child_string(*b.second, "created_at");
        if (ca != cb) return ca > cb;
        return a.first > b.first;
    });
    if (pairs.size() > max_turns)
        pairs.resize(max_turns);
    if (pairs.empty())
        return;

    std::vector<std::string> lines;
    lines.reserve(pairs.size());
    std::size_t total_chars = 0;
    for (const auto& row : pairs) {
        const auto& item = *row.second;
        const auto& meta = item.contains("metadata") && item["metadata"].is_object()
            ? item["metadata"] : item;
        std::string user = json_child_string(meta, "user");
        std::string asst = json_child_string(meta, "assistant");
        if (user.empty() && asst.empty() && item.contains("summary") && item["summary"].is_string())
            user = item["summary"].get<std::string>();
        user = recent_turn_text(std::move(user), pm::llm::k_recent_turns_user_chars);
        asst = recent_turn_text(std::move(asst), pm::llm::k_recent_turns_asst_chars);

        const nlohmann::json* calls = nullptr;
        if (meta.contains("tool_calls") && meta["tool_calls"].is_array())
            calls = &meta["tool_calls"];

        std::string line;
        if (!row.first.empty())
            line += "[" + row.first + "] ";
        if (!user.empty())
            line += "User: \u201c" + user + "\u201d\n";
        else if (!row.first.empty())
            line += "\n";
        if (!asst.empty())
            line += "  Assistant: \u201c" + asst + "\u201d\n";
        if (json_child_bool(meta, "body_available"))
            line += "  body_available=true\n";

        std::size_t shown_calls = 0;
        std::size_t hidden_calls = 0;
        if (calls) {
            for (const auto& call : *calls) {
                if (!call.is_object())
                    continue;
                const std::string name = json_child_string(call, "name");
                const std::string id   = json_child_string(call, "id");
                if (name.empty() && id.empty())
                    continue;
                if (shown_calls >= pm::llm::k_recent_turns_hint_calls) {
                    ++hidden_calls;
                    continue;
                }
                ++shown_calls;
                const std::string status = json_child_string(call, "status");
                line += "  " + name;
                if (!status.empty())
                    line += " " + status;
                if (call.contains("payload_available") && call["payload_available"].is_boolean())
                    line += json_child_bool(call, "payload_available")
                        ? " payload_available=true" : " payload_available=false";
                for (const char* key : {"query", "command", "path", "output_files", "input_files"}) {
                    if (!call.contains(key))
                        continue;
                    const std::string literal = format_stored_literal(call[key]);
                    if (literal.empty())
                        continue;
                    line += std::string(" ") + key + "=" + literal;
                    break;
                }
                line += "\n";
                if (json_child_bool(call, "payload_available") &&
                    !json_child_bool(call, "payload_denied") &&
                    !id.empty() && !row.first.empty()) {
                    line += "  memory_find(itemId=" + row.first
                        + ", toolCallId=" + id + ", includePayload=true)\n";
                }
            }
        }
        if (hidden_calls > 0)
            line += "  \u2026 " + std::to_string(hidden_calls) + " more calls\n";
        if (shown_calls == 0) {
            std::string tools;
            const auto& tools_j = meta.contains("tools") ? meta["tools"] : nlohmann::json();
            if (tools_j.is_array()) {
                for (const auto& tn : tools_j) {
                    if (!tn.is_string()) continue;
                    if (!tools.empty()) tools += ", ";
                    tools += tn.get<std::string>();
                }
            }
            if (!tools.empty())
                line += "  tools: " + tools + "\n";
        }
        if (meta.contains("proposed") && meta["proposed"].is_object() && !meta["proposed"].empty()) {
            const auto& proposed = meta["proposed"];
            std::string proposed_line = "  \u2192 proposed:";
            bool any = false;
            for (const char* field : {"intent", "action", "subject"}) {
                const std::string v = json_child_string(proposed, field);
                if (v.empty())
                    continue;
                proposed_line += std::string(" ") + field + "=" + v;
                any = true;
            }
            if (any)
                line += proposed_line + "\n";
        }
        if (line.empty())
            continue;
        constexpr std::size_t k_recent_turn_record_chars = 520;
        if (line.size() > k_recent_turn_record_chars)
            line = media::str_san::utf8_safe_ellipsize(line, k_recent_turn_record_chars, "\u2026\n");
        if (total_chars + line.size() > pm::llm::k_recent_turns_inject_chars)
            break;
        total_chars += line.size();
        lines.push_back(std::move(line));
    }
    if (lines.empty())
        return;

    // Newest-first collection; emit oldest first so the unfinished ask sits
    // next to the live user message.
    std::reverse(lines.begin(), lines.end());

    s << "\n## Recent turns\n"
         "Last conversation turns (oldest first). User and assistant are two lines each. "
         "Tool lines are hints, not results. Continue the latest unfinished ask.";
    if (advertise_body_recall)
        s << " Recover a truncated reply with memory_find(itemId=<turn id>, includeBody=true).";
    s << "\n";
    for (const auto& line : lines)
        s << line;
}

namespace {

void append_relevant_session_rules_impl(std::ostringstream& s,
                                        const nlohmann::json& memory_state,
                                        std::size_t chars_budget)
{
    if (!memory_state.is_object() || chars_budget == 0 ||
        !memory_state.contains("instructions") || !memory_state["instructions"].is_object())
        return;
    const auto& rules = memory_state["instructions"];
    if (rules.empty())
        return;
    std::vector<std::string> hard;
    std::vector<std::string> preferences;
    std::vector<std::string> hints;
    for (auto it = rules.begin(); it != rules.end(); ++it) {
        if (!it.value().is_object()) continue;
        const auto& r = it.value();
        if (r.contains("active") && !json_child_bool(r, "active", true)) continue;
        const std::string text = json_child_string(r, "text");
        if (text.empty()) continue;
        const std::string priority = json_child_string(r, "priority", "hint");
        if (priority == "hard") hard.push_back(text);
        else if (priority == "preference") preferences.push_back(text);
        else hints.push_back(text);
    }
    if (hard.empty() && preferences.empty() && hints.empty())
        return;
    std::ostringstream block;
    block << "\n## Relevant Session Rules\n";
    bool truncated = false;
    auto emit = [&](const char* label, const std::vector<std::string>& rows) {
        if (rows.empty() || truncated) return;
        block << label << ":\n";
        int n = 0;
        for (const auto& row : rows) {
            const std::string ln = std::string{"- "} + row + "\n";
            if (static_cast<std::size_t>(block.tellp()) + ln.size() > chars_budget) {
                truncated = true;
                break;
            }
            block << ln;
            if (++n >= 10) break;
        }
    };
    emit("Hard", hard);
    emit("Preferences", preferences);
    emit("Hints", hints);
    if (truncated)
        block << "- ... more rules omitted (budget) ...\n";
    s << block.str();
}

} // namespace

void append_relevant_session_rules(std::ostringstream& s,
                                   const nlohmann::json& memory_state,
                                   std::size_t chars_budget)
{
    if (chars_budget == 0) chars_budget = pm::llm::k_session_rules_inject_chars;
    append_relevant_session_rules_impl(s, memory_state, chars_budget);
}

void append_pending_recall(std::ostringstream& s, std::string_view block)
{
    if (block.empty()) return;
    s << block;
    if (block.back() != '\n')
        s << '\n';
}

namespace {

bool has_structured_session_memory(const nlohmann::json& memory_state)
{
    return memory_state.is_object()
        && ((memory_state.contains("items") && memory_state["items"].is_object() && !memory_state["items"].empty())
            || (memory_state.contains("artifacts") && memory_state["artifacts"].is_object() && !memory_state["artifacts"].empty())
            || (memory_state.contains("instructions") && memory_state["instructions"].is_object() && !memory_state["instructions"].empty()));
}

} // namespace

// ── Executor hard-rules block ─────────────────────────────────────────────────
//
// Tool *routing* (which tool for which intent) lives in planner-prompt.md —
// that file is the post-ship optimization surface. This function only emits
// executor constraints that the OpenAI tool schemas do not encode well:
// path resolution, settings-only credentials, rate-limit / inspect-first
// gotchas. Family lists, tool encyclopedias, and intent→tool cheat-sheets
// intentionally do not belong here.
//
// tools: the assembled OpenAI tools JSON (planner seed + progressive grants).
void append_agent_tooling_and_context(std::ostringstream& s,
                                      const Turn&         turn,
                                      const nlohmann::json& tools,
                                      bool emit_identity,
                                      bool emit_selection,
                                      bool emit_open_files,
                                      bool emit_folder)
{
    std::unordered_set<std::string> enabled_tools;
    for (const auto& t : tools) {
        if (!t.contains("function") || !t["function"].is_object()) continue;
        std::string nm = t["function"].value("name", std::string{});
        if (nm.empty()) continue;
        nm = media::strutil::lower_ascii(nm);
        enabled_tools.insert(std::move(nm));
    }
    auto has = [&](const char* name) -> bool {
        return enabled_tools.count(name) > 0;
    };

    if (emit_identity)
        s << "You are " << pm::brand::k_app_id_u8 << "'s chat agent.\n\n";

    if (enabled_tools.empty()) {
        s << "No tools are available this session. Answer in text only; do not claim to read, write, inspect, "
             "edit, or enumerate local files/images unless the user provided that content in the prompt.\n\n";
        if (!turn.selection.empty()) {
            s << "The user currently has " << turn.selection.size()
              << " file(s) selected, but tool access is disabled for this turn.\n";
        } else if (!turn.open_files.empty()) {
            s << "The user currently has " << turn.open_files.size()
              << " file(s) open in viewer tabs, but tool access is disabled for this turn.\n";
        } else if (!turn.folder_hint.empty()) {
            s << "The user is browsing folder: " << turn.folder_hint
              << "\nTool access is disabled, so treat this as passive context only.\n";
        } else {
            s << "The user has no active selection and no current folder.\n";
        }
        s << "\n## Guidelines:\n"
             "  - If the task requires local files, images, shell commands, memory, scheduling, or app control, "
             "say that tool access is disabled for this run.\n";
        return;
    }

    // Each block is skipped only when system-prompt.md already substitutes that
    // placeholder. Editor caret has no placeholder, so it stays here.
    if (emit_selection && !turn.selection.empty()) {
        s << "Selected files (" << turn.selection.size() << "):\n";
        for (std::size_t i = 0; i < turn.selection.size(); ++i)
            s << "  [" << i << "] " << format_path_with_stats(turn.selection[i]) << "\n";
        s << "Use these paths in tool calls unless the user names a different path.\n\n";
    }
    if (emit_open_files && !turn.open_files.empty()) {
        s << "Open files (" << turn.open_files.size() << "):\n";
        for (std::size_t i = 0; i < turn.open_files.size(); ++i)
            s << "  [" << i << "] " << format_path_with_stats(turn.open_files[i]) << "\n";
        s << "These are files open in viewer tabs; they may differ from the current file-panel selection.\n\n";
    }
    if (!turn.editor_path.empty() && turn.editor_line > 0) {
        s << "Editor caret: " << turn.editor_path << ':' << turn.editor_line
          << ':' << turn.editor_column;
        const bool ranged =
            turn.editor_sel_start_line > 0
            && (turn.editor_sel_start_line != turn.editor_sel_end_line
                || turn.editor_sel_start_col != turn.editor_sel_end_col);
        if (ranged) {
            s << " sel " << turn.editor_sel_start_line << ':' << turn.editor_sel_start_col
              << '-' << turn.editor_sel_end_line << ':' << turn.editor_sel_end_col;
        }
        s << "\n";
        if (!turn.editor_selected_text.empty()) {
            s << "Inner selection (" << turn.editor_selected_text.size() << " chars): "
              << media::str_san::utf8_safe_ellipsize(turn.editor_selected_text, 400)
              << "\n";
        }
        s << "Prefer this caret / selection as the edit window when the user says "
             "\"here\", \"this\", or does not name a heading.\n\n";
    }
    if (emit_folder && turn.selection.empty() && turn.open_files.empty()) {
        if (!turn.folder_hint.empty()) {
            s << "Working folder: " << turn.folder_hint
              << " (no file selection). Use this exact path in tool calls; invented paths will fail.\n\n";
        } else if (emit_selection && emit_open_files) {
            s << "No active selection, no open viewer files, and no current folder. "
                 "If the user refers to files without naming them, ask which folder or files to work on.\n\n";
        }
    }

    // Hard rules only — gated on the tools actually seeded for this turn.
    s << "## Guidelines:\n"
#if defined(__ANDROID__)
         "  - Relative path strings resolve against the app data folder, "
         "or the parent of the first selected file. "
         "Use the selected files listed above. Invented paths will fail.\n"
#else
         "  - Relative path strings resolve against the current file-panel folder "
         "(last-used explorer, or the parent of the first selected file). Invented paths will fail.\n"
#endif
         "  - Call the tool — do not narrate what you would do. "
         "If a tool reports per-file failures, state them briefly once.\n";

    if (has("file_read"))
        s << "  - For existing large files: locate with **file_search**, then "
             "**file_read(offset, limit)**. An unwindowed file_read on a large file fails — "
             "do not retry without a window. Never whole-file-read then whole-file-write "
             "unless the task is a genuine full rewrite.\n"
             "  - A converted document (pdf-to-md folder of parts) is **search_index** on that "
             "folder, then **search_query**, then **search_detail** for one passage. Do not "
             "file_read each part. If search_index is not in your tool list, call "
             "**request_tools** for search_index, search_query, and search_detail first.\n";
    if (has("search_index"))
        s << "  - After download or pdf-to-md, call **search_index** on the output folder, "
             "then **search_query** once. On the next turn call **write_file** with the summary. "
             "**search_detail** at most once. Do not keep searching, and do not file_read every part.\n"
             "  - A session item with role=search_index is an index from an earlier turn. "
             "Follow up with **search_query** using its index id. Do not re-read the folder.\n";
    if (has("write_file"))
        s << "  - When creating or fully rewriting a text file, call **write_file** with the full content — "
             "do not claim the file exists without calling the tool. "
             "Do not rewrite a large file to change one section.\n";
    if (has("file_compose"))
        s << "  - To assemble a large file from existing files or line ranges, call **file_compose**. "
             "The filesystem copies those bytes. Do not file_read a large source and paste it into "
             "write_file. overwrite defaults to false. Use file_str_replace for a small edit of an "
             "existing file.\n";
    if (has("file_str_replace"))
        s << "  - To edit an existing file: **file_search** (indexer=rg, regex=true for headings "
             "like `^### grant`) → **file_read** with offset/limit → **file_str_replace** with a unique "
             "old_string. Widen the snippet or pass start_line/end_line if the match is ambiguous. "
             "Do not use `run` + sed/python to edit when file_str_replace is available.\n";
    if (has("file_delete"))
        s << "  - To remove a file, call **file_delete**. Directories are refused.\n";

    {
        std::vector<std::string> settings_tools;
        for (const char* t : {"image_create", "create_video", "image_understand"})
            if (has(t)) settings_tools.push_back(t);
        if (!settings_tools.empty()) {
            s << "  - For ";
            for (std::size_t i = 0; i < settings_tools.size(); ++i) {
                if (i) s << (i + 1 == settings_tools.size() ? " and " : ", ");
                s << "**" << settings_tools[i] << "**";
            }
            s << ": provider/model and API keys come only from **app Settings** — do not "
                 "put model/provider/api_key in tool arguments.\n";
        }
    }
}

// ── System prompt builder ─────────────────────────────────────────────────────

static std::string build_system_prompt_impl(const Turn& turn,
                                            const nlohmann::json& tools,
                                            nlohmann::json* prompt_meta = nullptr) {
    // Render system-prompt.md through ${VAR} substitution if the file exists.
    // The file provides the agent identity / persona preamble; the dynamic
    // tooling section that follows always runs but skips its own "You are…"
    // intro so there is no duplication.
    const PromptFileLoad system_prompt_file = load_system_prompt_md();
    const std::string external = render_prompt_file(system_prompt_file.content, turn);

    if (prompt_meta) {
        (*prompt_meta)["system"] = {
            {"source", external.empty() ? "builtin" : "file"},
            {"path", system_prompt_file.path.empty()
                ? nlohmann::json(nullptr)
                : nlohmann::json(media::path::path_to_utf8(system_prompt_file.path))},
        };
    }

    std::ostringstream s;
    bool has_memory_read = false;
    bool has_memory_write = false;
    bool has_memory_find = false;
    if (tools.is_array()) {
        for (const auto& t : tools) {
            if (!t.contains("function") || !t["function"].is_object()) continue;
            std::string nm = json_child_string(t["function"], "name");
            nm = media::strutil::lower_ascii(nm);
            if (nm == "memory_read") has_memory_read = true;
            else if (nm == "memory_write") has_memory_write = true;
            else if (nm == "memory_find") has_memory_find = true;
        }
    }
    const bool has_memory_tools = has_memory_read || has_memory_write;

    if (!external.empty())
        s << external << "\n\n---\n\n";

    append_runtime_environment_context(s);
    const auto prompt_has = [&](const char* token) {
        return system_prompt_file.content.find(token) != std::string::npos;
    };
    append_agent_tooling_and_context(s, turn, tools,
        /*emit_identity=*/external.empty(),
        /*emit_selection=*/!prompt_has("${CURRENT_SELECTION}"),
        /*emit_open_files=*/!prompt_has("${CURRENT_OPEN_FILES}"),
        /*emit_folder=*/!prompt_has("${FOLDER}"));

    // ── P1: planner intent-context block ─────────────────────────────────────
    // Emitted whenever the planner produced at least one intent-context field,
    // including zero-tool turns (fallback=true, reason="planner selected no
    // tools") — these are pure-query turns where intent is still meaningful.
    // Only skipped when fallback means "planner didn't attempt" (error /
    // disabled / realtime), detected by absence of any intent field.
    if (turn.planner_meta.is_object() &&
        (turn.planner_meta.contains("intent") || turn.planner_meta.contains("action") ||
         turn.planner_meta.contains("subject"))) {
        const std::string intent  = turn.planner_meta.value("intent",  std::string{});
        const std::string action  = turn.planner_meta.value("action",  std::string{});
        const std::string subject = turn.planner_meta.value("subject", std::string{});
        const float confidence    = turn.planner_meta.value("confidence", 1.0f);
        if (!intent.empty() || !action.empty() || !subject.empty()) {
            s << "\n## Turn Context\n";
            if (!intent.empty())  s << "- intent: "  << intent  << "\n";
            if (!action.empty())  s << "- action: "  << action  << "\n";
            if (!subject.empty()) s << "- subject: " << subject << "\n";
            if (confidence < 0.5f)
                s << "- confidence: low \xe2\x80\x94 verify intent with the user before acting\n";
            // Planner reason: pass through when it carries actionable guidance
            // (e.g. a resolved exe path, a custom command id, a chosen viewer).
            // Keep it short — truncate at 200 chars to avoid context bloat.
            const std::string reason = turn.planner_meta.value("reason", std::string{});
            if (!reason.empty()) {
                const std::string r = media::str_san::utf8_safe_ellipsize(reason, 200);
                s << "- planner note: " << r << "\n";
            }
            if (turn.planner_meta.value("plan_invalid_after_filter", false)) {
                const auto& rejected = turn.planner_meta.value(
                    "rejected_tools", nlohmann::json::array());
                if (rejected.is_array() && !rejected.empty()) {
                    s << "- rejected tools:";
                    for (const auto& name : rejected) {
                        if (name.is_string())
                            s << " " << name.get<std::string>();
                    }
                    s << "\n";
                }
                const std::string why = turn.planner_meta.value("rejected_reason", std::string{});
                if (!why.empty())
                    s << "- rejected reason: " << why << "\n";
                s << "- The action tool is not available. Do not restate the known subject "
                     "or the literals in Recent turns. If more than one allowed action remains, "
                     "ask which. Do not pick a substitute tool.\n";
            }
            s << "\n";
        }
    }

    // ── P3: mode overlay ──────────────────────────────────────────────────────
    // When the planner (or caller) set a non-Auto TurnMode, load the matching
    // `mode-<name>.md` from the prompts directory and inject it as a behavioural
    // guidance block.  Gracefully a no-op when the file does not exist.
    if (turn.mode != TurnMode::Auto) {
        const PromptFileLoad overlay_file = detail::load_mode_overlay_md(turn.mode);
        const std::string& overlay = overlay_file.content;
        if (prompt_meta && !overlay.empty()) {
            (*prompt_meta)["mode_overlay"] = {
                {"mode", std::string(detail::turn_mode_name(turn.mode))},
                {"source", "file"},
                {"path", media::path::path_to_utf8(overlay_file.path)},
            };
        }
        if (!overlay.empty()) {
            s << "\n## Behavioral Mode: " << detail::turn_mode_name(turn.mode) << "\n";
            s << overlay << "\n";
        }
    }

    if (!turn.task_id.empty()) {
        if (turn.is_scheduled_tick) {
            // ── Scheduled task context ───────────────────────────────────────
            s << "\nScheduled task context:\n"
              << "  task_id: " << turn.task_id << "\n";
            if (turn.memory_state.is_object() && !turn.memory_state.empty())
                s << "Persistent memory state (JSON):\n"
                  << safe_json_dump(turn.memory_state, 2) << "\n";
            if (turn.recent_events.is_array() && !turn.recent_events.empty())
                s << "Recent task events (newest last):\n"
                  << safe_json_dump(turn.recent_events, 2) << "\n";
            s << "\nFor this scheduled tick:\n"
                 "  - Do the smallest useful unit of work.\n"
                 "  - Use tools for actual capture/read/write actions.\n"
                 "  - Do not ask the user to manually trigger the next tick.\n"
                 "  - Do not say you will wait; the scheduler will call you again.\n";
            if (has_memory_write)
                s << "  - Update memory via memory_write after meaningful state changes.\n";
        } else {
            // ── Session memory context (cross-turn chat persistence) ─────────
            using SMF = pm::llm::StructuredMemoryFeature;
            const bool replay_transcript =
                pm::llm::has_feature(turn.structured_memory_features, SMF::ReplayTranscript);
            // Compact last-4 tail when the full transcript dump is off. Replay
            // already carries the same text at larger budgets.
            if (pm::llm::has_feature(turn.structured_memory_features, SMF::InjectItems)
                && !replay_transcript)
                append_recent_turns(s, turn.memory_state, 0, has_memory_find);
            if (pm::llm::has_feature(turn.structured_memory_features, SMF::InjectItems))
                append_session_items(s, turn.memory_state);
            if (pm::llm::has_feature(turn.structured_memory_features, SMF::InjectRules))
                append_relevant_session_rules(s, turn.memory_state);
            if (pm::llm::has_feature(turn.structured_memory_features, SMF::InjectPendingRecall)
                && !turn.pending_recall_inject.empty())
                append_pending_recall(s, turn.pending_recall_inject);
            if (replay_transcript && turn.recent_events.is_array() && !turn.recent_events.empty()) {
                std::vector<std::string> kept;
                std::size_t total_chars = 0;
                bool omitted = false;
                for (auto it = turn.recent_events.rbegin(); it != turn.recent_events.rend(); ++it) {
                    const auto& ev = *it;
                    if (!ev.is_object()) continue;
                    const std::string ts   = json_child_string(ev, "ts");
                    std::string       user = json_child_string(ev, "user");
                    std::string       asst = json_child_string(ev, "assistant");

                    std::string tool_line;
                    if (ev.contains("tools") && ev["tools"].is_array()) {
                        for (const auto& tn : ev["tools"]) {
                            if (!tn.is_string()) continue;
                            if (!tool_line.empty()) tool_line += ", ";
                            tool_line += tn.get<std::string>();
                        }
                    }
                    user = media::str_san::utf8_safe_ellipsize(
                        user, pm::llm::k_session_memory_user_chars, "\u2026");
                    asst = media::str_san::utf8_safe_ellipsize(
                        asst, pm::llm::k_session_memory_asst_chars, "\u2026");

                    std::string line;
                    if (!ts.empty()) line += "[" + ts + "] ";
                    line += "User: \u201c" + user + "\u201d";
                    if (!tool_line.empty())
                        line += "\n  \u2192 tools: " + tool_line;
                    if (ev.contains("spoken") && ev["spoken"].is_array()) {
                        for (const auto& sp : ev["spoken"])
                            if (sp.is_string())
                                line += "\n  \u2192 spoke: \u201c" + sp.get<std::string>() + "\u201d";
                    }
                    if (ev.contains("media_outputs") && ev["media_outputs"].is_array()) {
                        std::string mline;
                        for (const auto& mp : ev["media_outputs"]) {
                            if (!mp.is_string()) continue;
                            const std::string& p = mp.get_ref<const std::string&>();
                            const auto slash = p.find_last_of("/\\");
                            mline += (mline.empty() ? "" : ", ")
                                   + (slash != std::string::npos ? p.substr(slash + 1) : p);
                        }
                        if (!mline.empty()) line += "\n  \u2192 media: " + mline;
                    }
                    if (!asst.empty())
                        line += "\n  \u2192 Assistant: \u201c" + asst + "\u201d";
                    line += "\n";

                    if (total_chars + line.size() > pm::llm::k_session_memory_inject_chars) {
                        omitted = true;
                        if (kept.empty())
                            kept.push_back(media::str_san::utf8_safe_ellipsize(
                                line, pm::llm::k_session_memory_inject_chars, "\u2026\n"));
                        break;
                    }
                    total_chars += line.size();
                    kept.push_back(std::move(line));
                }
                std::reverse(kept.begin(), kept.end());
                s << "\n## Conversation History\n"
                     "Past turns in this session (oldest first):\n";
                if (omitted)
                    s << "  [\u2026 earlier turns omitted to fit context \u2026]\n";
                for (const auto& line : kept)
                    s << line;
            }
            if (turn.memory_state.is_object() && !turn.memory_state.empty()) {
                if (!has_structured_session_memory(turn.memory_state)) {
                    s << "\n## Stored Memory\n"
                         "Facts you have explicitly saved with memory_write:\n"
                      << safe_json_dump(turn.memory_state, 2) << "\n";
                } else {
                    nlohmann::json extras = nlohmann::json::object();
                    for (auto it = turn.memory_state.begin(); it != turn.memory_state.end(); ++it) {
                        if (it.key() == "items" || it.key() == "artifacts"
                            || it.key() == "instructions" || it.key() == "journal_recovery")
                            continue;
                        extras[it.key()] = it.value();
                    }
                    if (!extras.empty()) {
                        s << "\n## Stored Memory\n"
                             "Explicit facts saved beside structured session items:\n"
                          << media::str_san::utf8_safe_ellipsize(safe_json_dump(extras, 2), 800, "\u2026")
                          << "\n";
                    }
                }
            }
            if (has_memory_tools) {
                s << "\n";
                if (has_memory_write)
                    s << "Use `memory_write` to persist facts across turns (user name, preferences, "
                         "secrets, notes). ";
                if (has_memory_read)
                    s << "Use `memory_read` to retrieve them. ";
                s << "Memory persists for the lifetime of this chat session.\n";
            }
        }
    }

    if (!turn.system_extra.empty())
        s << "\n" << turn.system_extra << "\n";

    const PromptFileLoad prefs = load_user_preferences_md();
    if (!prefs.content.empty()) {
        if (prompt_meta) {
            (*prompt_meta)["user_preferences"] = {
                {"source", "file"},
                {"path", prefs.path.empty()
                    ? nlohmann::json(nullptr)
                    : nlohmann::json(media::path::path_to_utf8(prefs.path))},
            };
        }
        s << "\n## User preferences\n"
          << render_prompt_file(prefs.content, turn) << "\n";
    }

    return s.str();
}

} // namespace media::llm::agent::detail

// ── Public entry point ────────────────────────────────────────────────────────

namespace media::llm::agent {

// Primary overload — caller supplies the assembled tools JSON (path + MCP).
std::string build_system_prompt(const Turn& turn, const nlohmann::json& tools) {
    return detail::build_system_prompt_impl(turn, tools);
}

std::string build_system_prompt(const Turn& turn,
                                const nlohmann::json& tools,
                                nlohmann::json* prompt_meta) {
    return detail::build_system_prompt_impl(turn, tools, prompt_meta);
}

// Backward-compat overload (CLI --dry-run, tests): rebuilds tools from the
// path catalog using the Turn's disabled_path_tools filter.
std::string build_system_prompt(const Turn& turn) {
    nlohmann::json tools = turn.disabled_path_tools.empty()
        ? media::llm::path::tool_catalog_openai()
        : media::llm::path::tool_catalog_openai_excluding(turn.disabled_path_tools);
    return detail::build_system_prompt_impl(turn, tools);
}

} // namespace media::llm::agent

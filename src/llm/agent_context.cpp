#include "agent_internal.hpp"

#include "constants.hpp"
#include "string/string_sanitizers.hpp"
#include "llm_constants.hpp"
#include "path_tool_catalog.hpp"
#include "agent_memory.hpp"

#include "chat_session_store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
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

/// Clip to `max_bytes`, then flatten controls and newlines. One stored
/// field stays on the line it was written to, and a multi-megabyte value
/// is cut before it is copied into the prompt.
std::string clip_field(std::string_view raw, std::size_t max_bytes)
{
    if (max_bytes == 0 || raw.empty())
        return {};
    std::string s = raw.size() <= max_bytes
        ? std::string(raw)
        : media::str_san::utf8_safe_ellipsize(raw, max_bytes, "\u2026");
    std::string flat;
    flat.reserve(s.size());
    bool pending_space = false;
    for (unsigned char c : s) {
        if (c <= 0x20) {
            pending_space = !flat.empty();
            continue;
        }
        if (pending_space) {
            flat.push_back(' ');
            pending_space = false;
        }
        flat.push_back(static_cast<char>(c));
    }
    if (!flat.empty() && flat.back() == ' ')
        flat.pop_back();
    return flat;
}

std::string_view json_sv(const nlohmann::json& o, const char* key)
{
    if (!o.is_object()) return {};
    const auto it = o.find(key);
    if (it == o.end() || !it->is_string()) return {};
    return it->get_ref<const std::string&>();
}

float json_number_or(const nlohmann::json& o, const char* key, float def)
{
    if (!o.is_object()) return def;
    const auto it = o.find(key);
    if (it == o.end() || !it->is_number()) return def;
    if (it->is_number_float()) return static_cast<float>(it->get<double>());
    if (it->is_number_unsigned()) return static_cast<float>(it->get<std::uint64_t>());
    return static_cast<float>(it->get<std::int64_t>());
}

/// Bounded copy of known-shape JSON for a prompt. Depth applies to arrays
/// and objects. Object keys are kept whole or the entry is skipped: a
/// clipped key can collide with another key and drop a value. The result
/// is still valid JSON. `budget` counts string payload bytes, not the
/// serialized form — `prompt_json` drops whole entries until `dump()` fits.
nlohmann::json clip_json_for_prompt(const nlohmann::json& j, int depth, std::size_t& budget)
{
    if (budget < 8)
        return "\u2026";
    if (j.is_string()) {
        const auto& src = j.get_ref<const std::string&>();
        std::string s = src.size() <= 160
            ? src
            : media::str_san::utf8_safe_ellipsize(src, 160, "\u2026");
        if (s.size() > budget)
            s = media::str_san::utf8_safe_ellipsize(s, budget, "\u2026");
        budget -= std::min(budget, s.size());
        return s;
    }
    if (j.is_number() || j.is_boolean() || j.is_null()) {
        if (budget > 0) --budget;
        return j;
    }
    if (depth >= 4)
        return "\u2026";
    if (j.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        std::size_t n = 0;
        for (const auto& el : j) {
            if (n >= 8 || budget < 16)
                break;
            out.push_back(clip_json_for_prompt(el, depth + 1, budget));
            ++n;
        }
        return out;
    }
    if (j.is_object()) {
        nlohmann::json out = nlohmann::json::object();
        std::size_t n = 0;
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (n >= 24 || budget < 16)
                break;
            if (it.key().size() > 128)
                continue;
            out[it.key()] = clip_json_for_prompt(it.value(), depth + 1, budget);
            ++n;
        }
        return out;
    }
    return "\u2026";
}

/// Serialize `j` without cutting the dump mid-token. If the clipped tree
/// still exceeds `max_bytes`, drop whole array elements or object keys and
/// serialize again. Never returns a truncated JSON document.
std::string prompt_json(const nlohmann::json& j, std::size_t max_bytes)
{
    if (max_bytes < 2 || j.is_null())
        return {};
    std::size_t budget = max_bytes;
    nlohmann::json clipped = clip_json_for_prompt(j, 0, budget);
    auto fits = [&](const nlohmann::json& node) -> std::string {
        const std::string dumped = safe_json_dump(node, 2);
        return dumped.size() <= max_bytes ? dumped : std::string{};
    };
    if (std::string dumped = fits(clipped); !dumped.empty())
        return dumped;
    if (clipped.is_array()) {
        while (!clipped.empty()) {
            clipped.erase(clipped.size() - 1);
            if (std::string dumped = fits(clipped); !dumped.empty())
                return dumped;
        }
        return max_bytes >= 2 ? "[]" : std::string{};
    }
    if (clipped.is_object()) {
        while (!clipped.empty()) {
            clipped.erase(std::prev(clipped.end()).key());
            if (std::string dumped = fits(clipped); !dumped.empty())
                return dumped;
        }
        return max_bytes >= 2 ? "{}" : std::string{};
    }
    return {};
}

/// A path, URL, or id is emitted whole or not at all. Truncating one
/// produces a locator the model will try to open.
std::string locator_field(std::string_view raw, std::size_t max_bytes)
{
    if (raw.empty() || raw.size() > max_bytes)
        return {};
    for (unsigned char c : raw) {
        if (c < 0x20)
            return {};
    }
    return std::string(raw);
}

/// Writes text that fits a byte budget. A failed append leaves the writer
/// unchanged so the caller can drop a whole record or stop.
class PromptWriter {
public:
    explicit PromptWriter(std::size_t limit) : limit_(limit) {}

    [[nodiscard]] bool append(std::string_view text)
    {
        if (text.size() > remaining())
            return false;
        data_.append(text);
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return data_.size() >= limit_ ? 0 : limit_ - data_.size();
    }

    [[nodiscard]] bool empty() const noexcept { return data_.empty(); }

    std::string finish() && { return std::move(data_); }

private:
    std::string data_;
    std::size_t limit_ = 0;
};

std::size_t stream_size(std::ostringstream& block)
{
    const auto pos = block.tellp();
    return pos > 0 ? static_cast<std::size_t>(pos) : 0;
}

void append_one_session_item(std::ostringstream& s, const std::string& id, const nlohmann::json& item)
{
    if (!item.is_object()) return;
    constexpr std::size_t k_id_chars = 80;
    std::string kind = clip_field(json_sv(item, "kind"), 32);
    if (kind.empty()) kind = "unknown";
    const std::string shown_id = locator_field(id, k_id_chars);
    s << "- " << (shown_id.empty() ? std::string("item") : shown_id)
      << ": kind=" << kind;
    const std::string role = clip_field(json_sv(item, "role"), 32);
    if (!role.empty() && role != "unknown")
        s << " role=" << role;
    const bool has_loc = item.contains("locator") && item["locator"].is_object() && !item["locator"].empty();
    const nlohmann::json& path_src = has_loc ? item["locator"] : item;
    const std::string path = locator_field(json_sv(path_src, "path"), 4096);
    if (!path.empty())
        s << " path=" << path;
    if (has_loc) {
        const auto& loc = item["locator"];
        const std::string index = locator_field(json_sv(loc, "index"), 80);
        const std::string url = locator_field(json_sv(loc, "url"), 4096);
        const std::string blob = locator_field(json_sv(loc, "blob_ref"), 80);
        if (!index.empty()) s << " index=" << index;
        if (!url.empty()) s << " url=" << url;
        if (!blob.empty()) s << " blob_ref=" << blob;
    }
    const std::string content_type = clip_field(json_sv(item, "content_type"), 64);
    const std::string schema = clip_field(json_sv(item, "schema_hint"), 64);
    if (!content_type.empty()) s << " content_type=" << content_type;
    if (!schema.empty()) s << " schema=" << schema;
    if (item.contains("metadata") && item["metadata"].is_object()) {
        const auto& m = item["metadata"];
        if (m.contains("count") && m["count"].is_number())
            s << " count=" << json_child_int64(m, "count");
        const std::string pattern = clip_field(json_sv(m, "source_pattern"), 120);
        if (!pattern.empty())
            s << " source_pattern=" << pattern;
        if (kind == "tool_checkpoint") {
            if (m.contains("exit_code") && m["exit_code"].is_number())
                s << " exit_code=" << json_child_int64(m, "exit_code");
            const std::string command = clip_field(json_sv(m, "command"), 120);
            if (!command.empty())
                s << " command=" << command;
        }
        if (json_child_bool(m, "body_available")) {
            s << " body_available=true";
            if (m.contains("assistant_chars") && m["assistant_chars"].is_number())
                s << " assistant_chars=" << json_child_int64(m, "assistant_chars");
        }
        if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
            bool first = true;
            std::size_t shown_calls = 0;
            for (const auto& call : m["tool_calls"]) {
                if (!call.is_object() ||
                    !json_child_bool(call, "payload_available") ||
                    json_child_bool(call, "payload_denied"))
                    continue;
                const std::string call_id = locator_field(json_sv(call, "id"), k_id_chars);
                const std::string name = clip_field(json_sv(call, "name"), 40);
                if (call_id.empty()) continue;
                if (shown_calls >= 4) {
                    s << (first ? " payload_calls=\u2026" : ",\u2026");
                    break;
                }
                s << (first ? " payload_calls=" : ",");
                if (!name.empty()) s << name << ":";
                s << call_id;
                const std::string command = clip_field(json_sv(call, "command"), 120);
                if (!command.empty())
                    s << "{command=" << command << "}";
                first = false;
                ++shown_calls;
            }
        }
        if (json_child_bool(m, "payload_available") &&
            !json_child_bool(m, "payload_denied")) {
            const std::string call_id = locator_field(json_sv(m, "transcript_tool_call_id"), k_id_chars);
            if (!call_id.empty()) {
                s << " payload_call_id=" << call_id;
                const std::string tool_name = item.contains("origin") && item["origin"].is_object()
                    ? clip_field(json_sv(item["origin"], "tool_name"), 40)
                    : std::string{};
                if (!tool_name.empty()) s << " payload_tool=" << tool_name;
            }
        }
    }
    if (item.contains("summary") && item["summary"].is_string())
        s << " — " << clip_field(item["summary"].get_ref<const std::string&>(), k_inject_item_summary_chars);
    s << "\n";
}

/// One row with its sort fields already pulled out of JSON. The pointed-to
/// item must stay alive for the sort. Recency is last_used_at, else
/// created_at. Equal keys then compare created_at descending, then id
/// ascending.
struct ItemCandidate {
    std::string id;
    std::string_view created_at;
    std::string_view last_used_at;
    const nlohmann::json* item = nullptr;

    std::tuple<std::string_view, std::string_view, std::string_view>
    sort_key() const noexcept
    {
        return {last_used_at.empty() ? created_at : last_used_at, created_at, id};
    }
};

void sort_item_refs(std::vector<std::pair<std::string, const nlohmann::json*>>& out)
{
    std::vector<ItemCandidate> candidates;
    candidates.reserve(out.size());
    for (const auto& row : out) {
        candidates.push_back(ItemCandidate{
            .id = row.first,
            .created_at = json_sv(*row.second, "created_at"),
            .last_used_at = json_sv(*row.second, "last_used_at"),
            .item = row.second,
        });
    }
    std::sort(candidates.begin(), candidates.end(), [](const ItemCandidate& a, const ItemCandidate& b) {
        const auto ka = a.sort_key();
        const auto kb = b.sort_key();
        if (std::get<0>(ka).empty() != std::get<0>(kb).empty())
            return !std::get<0>(ka).empty();
        if (std::get<0>(ka) != std::get<0>(kb))
            return std::get<0>(ka) > std::get<0>(kb);
        if (std::get<1>(ka) != std::get<1>(kb))
            return std::get<1>(ka) > std::get<1>(kb);
        return std::get<2>(ka) < std::get<2>(kb);
    });
    out.clear();
    for (const auto& c : candidates)
        out.emplace_back(std::string(c.id), c.item);
}

std::string format_stored_literal(const nlohmann::json& value)
{
    if (value.is_string())
        return clip_field(value.get_ref<const std::string&>(), 160);
    if (!value.is_array() || value.empty())
        return {};
    std::string out;
    std::size_t n = 0;
    for (const auto& entry : value) {
        if (n >= 4) {
            out += ", \u2026";
            break;
        }
        std::string piece;
        if (entry.is_string())
            piece = clip_field(entry.get_ref<const std::string&>(), 80);
        else if (entry.is_object()) {
            const std::string name = clip_field(json_sv(entry, "name"), 80);
            const std::string exe  = clip_field(json_sv(entry, "exe"), 80);
            if (!name.empty() && !exe.empty())
                piece = name + ":" + exe;
            else
                piece = !name.empty() ? name : exe;
            piece = clip_field(piece, 80);
        }
        if (piece.empty())
            continue;
        if (!out.empty())
            out += ", ";
        out += piece;
        ++n;
    }
    return media::str_san::utf8_safe_ellipsize(out, 160, "\u2026");
}

} // namespace

void append_session_items(std::ostringstream& s,
                          const nlohmann::json& memory_state,
                          std::size_t chars_budget,
                          std::size_t max_items,
                          bool advertise_recall)
{
    if (chars_budget == 0) chars_budget = pm::llm::k_session_items_inject_chars;
    if (max_items == 0)    max_items    = pm::llm::k_session_items_inject_max;
    if (!memory_state.is_object())
        return;

    // `items` is canonical. Legacy `artifacts` fill ids that items do not
    // already carry, so one new item cannot hide the old collection.
    std::vector<std::pair<std::string, const nlohmann::json*>> rows;
    std::unordered_set<std::string> seen;
    auto take = [&](const char* key) {
        if (!memory_state.contains(key) || !memory_state[key].is_object())
            return;
        for (auto it = memory_state[key].begin(); it != memory_state[key].end(); ++it) {
            if (!seen.insert(it.key()).second)
                continue;
            if (it.value().is_object()) {
                const std::string kind = json_child_string(it.value(), "kind");
                const std::string role = json_child_string(it.value(), "role");
                if (kind == "self_check" || role == "security_review" || role == "security_decision")
                    continue;
            }
            rows.emplace_back(it.key(), &it.value());
        }
    };
    take("items");
    take("artifacts");
    if (rows.empty())
        return;

    bool has_retrievable_body = false;
    bool has_retrievable_payload = false;
    for (const auto& row : rows) {
        if (!row.second->is_object() || !row.second->contains("metadata") ||
            !(*row.second)["metadata"].is_object())
            continue;
        const auto& metadata = (*row.second)["metadata"];
        if (json_child_bool(metadata, "body_available"))
            has_retrievable_body = true;
        if (json_child_bool(metadata, "payload_available") &&
            !json_child_bool(metadata, "payload_denied"))
            has_retrievable_payload = true;
        if (!has_retrievable_payload && metadata.contains("tool_calls") &&
            metadata["tool_calls"].is_array()) {
            for (const auto& call : metadata["tool_calls"]) {
                if (call.is_object() &&
                    json_child_bool(call, "payload_available") &&
                    !json_child_bool(call, "payload_denied")) {
                    has_retrievable_payload = true;
                    break;
                }
            }
        }
        if (has_retrievable_body && has_retrievable_payload)
            break;
    }

    constexpr std::string_view k_omit = "- ... more session items omitted (budget) ...\n";
    PromptWriter block{chars_budget};
    if (!block.append(
            "\n## Session Items\n"
            "Use these durable references by role, origin, locator, and recency before asking the user for previous outputs. Newest first. "
            "Image items may be passed to create_video as start_frame / last_frame (newest matching still first).\n"))
        return;
    if (advertise_recall && has_retrievable_body)
        (void)block.append("For body_available=true, recover the exact prior reply with memory_find(itemId=<turn id>, includeBody=true) before writing or exporting it.\n");
    if (advertise_recall && has_retrievable_payload)
        (void)block.append("For payload_call(s), recover one exact same-session tool result with memory_find(itemId=<item id>, toolCallId=<call id>, includePayload=true). Do not rerun the tool.\n");
    if (block.remaining() < k_omit.size())
        return;

    sort_item_refs(rows);
    std::size_t shown = 0;
    bool truncated = false;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (shown >= max_items) { truncated = (i < rows.size()); break; }
        if (json_child_string(*rows[i].second, "role") == "turn_pair")
            continue;
        std::ostringstream line;
        append_one_session_item(line, rows[i].first, *rows[i].second);
        const std::string ln = line.str();
        if (ln.empty())
            continue;
        if (ln.size() + k_omit.size() > block.remaining()) {
            truncated = true;
            continue;
        }
        if (!block.append(ln)) {
            truncated = true;
            continue;
        }
        ++shown;
    }
    if (shown == 0)
        return;
    if (truncated)
        (void)block.append(k_omit);
    s << std::move(block).finish();
}

namespace {

/// Newlines become spaces so a prompt or reply occupies at most two lines.
/// Only the head of `text` is walked, so a multi-megabyte turn does not
/// get copied into the prompt builder.
std::string recent_turn_text(std::string_view text, std::size_t max_chars)
{
    if (max_chars == 0 || text.empty())
        return {};
    const std::string bounded = text.size() <= max_chars * 4
        ? std::string(text)
        : media::str_san::utf8_safe_ellipsize(text, max_chars * 4, "");
    text = bounded;
    std::string flat;
    flat.reserve(std::min(text.size(), max_chars + 8));
    bool pending_space = false;
    for (unsigned char c : text) {
        if (flat.size() >= max_chars + 8)
            break;
        if (c <= 0x20) {
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
                         RecentTurnView view)
{
    const std::size_t max_turns = view.max_turns == 0
        ? pm::llm::k_recent_turns_inject_max : view.max_turns;
    const bool advertise_body_recall = view.advertise_body_recall;
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
        auto field = [](const nlohmann::json& o, const char* key) -> std::string_view {
            if (!o.is_object()) return {};
            const auto it = o.find(key);
            if (it == o.end() || !it->is_string()) return {};
            return it->get_ref<const std::string&>();
        };
        std::string_view user_sv = field(meta, "user");
        std::string_view asst_sv = field(meta, "assistant");
        if (user_sv.empty() && asst_sv.empty())
            user_sv = field(item, "summary");
        const std::string user = view.include_quotes
            ? recent_turn_text(user_sv, pm::llm::k_recent_turns_user_chars) : std::string{};
        const std::string asst = view.include_quotes
            ? recent_turn_text(asst_sv, pm::llm::k_recent_turns_asst_chars) : std::string{};

        const nlohmann::json* calls = nullptr;
        if (meta.contains("tool_calls") && meta["tool_calls"].is_array())
            calls = &meta["tool_calls"];

        const std::string turn_id = locator_field(row.first, 80);
        const char* user_pre = "User: \u201c";
        const char* user_post = "\u201d\n";
        const char* asst_pre = "  Assistant: \u201c";
        const char* asst_post = "\u201d\n";
        std::string user_line = user.empty() ? std::string{} : std::string(user_pre) + user + user_post;
        std::string asst_line = asst.empty() ? std::string{} : std::string(asst_pre) + asst + asst_post;

        // Hints are droppable. Recall lines are kept whole or omitted.
        // A single ellipsize of the record used to cut the recall id off the end.
        std::string extras;
        std::string recalls;
        auto append_recall = [&](std::string_view call_id) {
            if (!advertise_body_recall || turn_id.empty() || call_id.empty())
                return;
            const std::string call = locator_field(call_id, 80);
            if (call.empty())
                return;
            const std::string recall = "  memory_find(itemId=" + turn_id
                + ", toolCallId=" + call
                + ", includePayload=true)\n";
            if (recalls.size() + recall.size() > 240)
                return;
            recalls += recall;
        };

        std::size_t shown_calls = 0;
        std::size_t hidden_calls = 0;
        if (calls) {
            for (const auto& call : *calls) {
                if (!call.is_object())
                    continue;
                const std::string name = clip_field(field(call, "name"), 40);
                const std::string_view id = field(call, "id");
                if (name.empty() && id.empty())
                    continue;
                if (shown_calls >= pm::llm::k_recent_turns_hint_calls) {
                    ++hidden_calls;
                    if (json_child_bool(call, "payload_available") &&
                        !json_child_bool(call, "payload_denied"))
                        append_recall(id);
                    continue;
                }
                ++shown_calls;
                std::string hint = "  " + name;
                const std::string status = clip_field(field(call, "status"), 24);
                if (!status.empty())
                    hint += " " + status;
                if (call.contains("payload_available") && call["payload_available"].is_boolean())
                    hint += json_child_bool(call, "payload_available")
                        ? " payload_available=true" : " payload_available=false";
                for (const char* key : {"query", "command", "path", "output_files", "input_files"}) {
                    if (!call.contains(key))
                        continue;
                    const std::string literal = format_stored_literal(call[key]);
                    if (literal.empty())
                        continue;
                    hint += std::string(" ") + key + "=" + literal;
                    break;
                }
                hint += "\n";
                if (hint.size() > 180)
                    hint = media::str_san::utf8_safe_ellipsize(hint, 180, "\u2026\n");
                extras += hint;
                if (json_child_bool(call, "payload_available") &&
                    !json_child_bool(call, "payload_denied"))
                    append_recall(id);
            }
        }
        if (hidden_calls > 0)
            extras += "  \u2026 " + std::to_string(hidden_calls) + " more calls\n";
        if (shown_calls == 0 && meta.contains("tools") && meta["tools"].is_array()) {
            std::string tools;
            for (const auto& tn : meta["tools"]) {
                if (!tn.is_string()) continue;
                const std::string name = clip_field(tn.get_ref<const std::string&>(), 40);
                if (name.empty()) continue;
                if (!tools.empty()) tools += ", ";
                if (tools.size() >= 120) {
                    tools += "\u2026";
                    break;
                }
                tools += name;
            }
            if (!tools.empty())
                extras += "  tools: " + tools + "\n";
        }
        if (meta.contains("proposed") && meta["proposed"].is_object() && !meta["proposed"].empty()) {
            const auto& proposed = meta["proposed"];
            std::string proposed_line = "  \u2192 proposed:";
            bool any = false;
            for (const char* key : {"intent", "action", "subject"}) {
                const std::string v = clip_field(field(proposed, key), 60);
                if (v.empty())
                    continue;
                proposed_line += std::string(" ") + key + "=" + v;
                any = true;
            }
            if (any)
                extras += proposed_line + "\n";
        }

        constexpr std::size_t k_recent_turn_record_chars = 520;
        std::string prefix = turn_id.empty() ? std::string{} : "[" + turn_id + "] ";
        if (user_line.empty() && !prefix.empty())
            prefix.push_back('\n');

        auto shrink_wrapped = [](std::string& line, const std::string& body,
                                 const char* pre, const char* post, std::size_t allow) {
            if (line.size() <= allow)
                return;
            const std::size_t chrome =
                std::char_traits<char>::length(pre) + std::char_traits<char>::length(post);
            if (allow <= chrome) {
                line.clear();
                return;
            }
            line = std::string(pre)
                + media::str_san::utf8_safe_ellipsize(body, allow - chrome, "\u2026")
                + post;
        };
        auto trim_lines = [](std::string& block, std::size_t max_bytes) {
            while (block.size() > max_bytes) {
                if (block.size() < 2) {
                    block.clear();
                    return;
                }
                const auto cut = block.rfind('\n', block.size() - 2);
                if (cut == std::string::npos) {
                    block.clear();
                    return;
                }
                block.resize(cut + 1);
            }
        };

        if (prefix.size() >= k_recent_turn_record_chars)
            continue;
        if (prefix.size() + recalls.size() > k_recent_turn_record_chars)
            trim_lines(recalls, k_recent_turn_record_chars - prefix.size());
        const std::size_t room = k_recent_turn_record_chars - prefix.size() - recalls.size();
        const std::size_t user_allow = std::min(user_line.size(), room - room / 2);
        shrink_wrapped(user_line, user, user_pre, user_post, user_allow);
        const std::size_t asst_allow = room > user_line.size() ? room - user_line.size() : 0;
        shrink_wrapped(asst_line, asst, asst_pre, asst_post, asst_allow);
        const std::size_t used =
            prefix.size() + user_line.size() + asst_line.size() + recalls.size();
        if (used < k_recent_turn_record_chars)
            trim_lines(extras, k_recent_turn_record_chars - used);
        else
            extras.clear();

        std::string line = prefix + user_line + asst_line + extras + recalls;
        if (line.empty())
            continue;
        if (total_chars + line.size() > pm::llm::k_recent_turns_inject_chars)
            break;
        total_chars += line.size();
        lines.push_back(std::move(line));
    }
    if (lines.empty())
        return;

    // Newest-first collection; emit oldest first so the latest turn sits
    // next to the live user message.
    std::reverse(lines.begin(), lines.end());

    s << "\n## Recent turns\n"
         "Last conversation turns (oldest first). User and assistant are two lines each. "
         "Tool lines are hints, not results. These turns are context for the current request; "
         "continue an earlier action only when this request refers to it.";
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
    bool overflow = false;
    for (auto it = rules.begin(); it != rules.end(); ++it) {
        if (!it.value().is_object()) continue;
        const auto& r = it.value();
        if (it.key() == "rule_terse_followup_acts_on_latest") continue;
        if (r.contains("active") && !json_child_bool(r, "active", true)) continue;
        const auto text_it = r.find("text");
        if (text_it == r.end() || !text_it->is_string()) continue;
        const std::string text = clip_field(text_it->get_ref<const std::string&>(), 400);
        if (text.empty()) continue;
        const std::string priority = json_child_string(r, "priority", "hint");
        auto push = [](std::vector<std::string>& bucket, std::string row) {
            if (bucket.size() >= pm::llm::k_session_rules_max)
                return false;
            bucket.push_back(std::move(row));
            return true;
        };
        const bool stored =
            priority == "hard" ? push(hard, text) :
            priority == "preference" ? push(preferences, text) :
            push(hints, text);
        if (!stored)
            overflow = true;
    }
    if (hard.empty() && preferences.empty() && hints.empty())
        return;
    std::ostringstream block;
    block << "\n## Relevant Session Rules\n";
    constexpr std::string_view k_omit = "- ... more rules omitted (budget) ...\n";
    if (stream_size(block) + k_omit.size() > chars_budget)
        return;
    bool truncated = false;
    auto emit = [&](const char* label, const std::vector<std::string>& rows) {
        if (rows.empty() || truncated) return;
        const std::string heading = std::string(label) + ":\n";
        if (stream_size(block) + heading.size() + k_omit.size() > chars_budget) {
            truncated = true;
            return;
        }
        block << heading;
        for (const auto& row : rows) {
            const std::string ln = std::string{"- "} + row + "\n";
            if (stream_size(block) + ln.size() + k_omit.size() > chars_budget) {
                truncated = true;
                break;
            }
            block << ln;
        }
    };
    emit("Hard", hard);
    emit("Preferences", preferences);
    emit("Hints", hints);
    if ((truncated || overflow) && stream_size(block) + k_omit.size() <= chars_budget)
        block << k_omit;
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
    const std::string clipped = block.size() <= pm::llm::k_recall_inject_chars
        ? std::string(block)
        : media::str_san::utf8_safe_ellipsize(block, pm::llm::k_recall_inject_chars, "\u2026\n");
    if (clipped.empty()) return;
    s << clipped;
    if (clipped.back() != '\n')
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
struct ToolCapabilities {
    std::unordered_set<std::string> enabled;
    bool memory_read = false;
    bool memory_write = false;
    bool memory_find = false;

    bool contains(std::string_view name) const
    {
        return enabled.contains(std::string(name));
    }

    static ToolCapabilities from_json(const nlohmann::json& tools)
    {
        ToolCapabilities caps;
        if (!tools.is_array())
            return caps;
        for (const auto& t : tools) {
            if (!t.contains("function") || !t["function"].is_object())
                continue;
            std::string nm = json_child_string(t["function"], "name");
            if (nm.empty())
                continue;
            nm = media::strutil::lower_ascii(std::move(nm));
            if (nm == "memory_read") caps.memory_read = true;
            else if (nm == "memory_write") caps.memory_write = true;
            else if (nm == "memory_find") caps.memory_find = true;
            caps.enabled.insert(std::move(nm));
        }
        return caps;
    }
};

struct ToolRule {
    std::string_view tool;
    std::string_view text;
};

constexpr std::array<ToolRule, 4> k_tool_rules{{
    {"write_file",
     "  - When creating or fully rewriting a text file, call **write_file** with the full content — "
     "do not claim the file exists without calling the tool. "
     "Do not rewrite a large file to change one section.\n"},
    {"file_compose",
     "  - To assemble a large file from existing files or line ranges, call **file_compose**. "
     "The filesystem copies those bytes. Do not file_read a large source and paste it into "
     "write_file when **file_compose** is available. overwrite defaults to false.\n"},
    {"file_str_replace",
     "  - To edit an existing file: **file_search** (when it is available) then **file_read** "
     "with offset/limit, then **file_str_replace** with a unique old_string. "
     "Do not use `run` to edit when file_str_replace is available.\n"},
    {"file_delete",
     "  - To remove a file, call **file_delete**. Directories are refused.\n"},
}};

void append_agent_tooling_and_context(std::ostringstream& s,
                                      const Turn&         turn,
                                      const nlohmann::json& tools,
                                      ContextSections sections)
{
    const ToolCapabilities caps = ToolCapabilities::from_json(tools);
    auto has = [&](std::string_view name) -> bool {
        return caps.contains(name);
    };

    if (sections.identity)
        s << "You are " << pm::brand::k_app_id_u8 << "'s chat agent.\n\n";

    if (caps.enabled.empty()) {
        s << "No tools are available this session. Answer in text only; do not claim to read, write, inspect, "
             "edit, or enumerate local files/images unless the user provided that content in the prompt.\n\n";
        if (sections.selection && !turn.selection.empty()) {
            s << "The user currently has " << turn.selection.size()
              << " file(s) selected, but tool access is disabled for this turn.\n";
        } else if (sections.open_files && !turn.open_files.empty()) {
            s << "The user currently has " << turn.open_files.size()
              << " file(s) open in viewer tabs, but tool access is disabled for this turn.\n";
        } else if (sections.folder && !turn.folder_hint.empty()) {
            s << "The user is browsing folder: "
              << clip_field(turn.folder_hint, pm::llm::k_prompt_field_chars)
              << "\nTool access is disabled, so treat this as passive context only.\n";
        } else {
            s << "The user has no active selection and no current folder.\n";
        }
        s << "\n## Guidelines:\n"
             "  - If the task requires local files, images, shell commands, memory, scheduling, or app control, "
             "say that tool access is disabled for this run.\n";
        return;
    }

    auto append_paths = [&](const char* title, const std::vector<std::string>& paths, const char* trailer) {
        s << title << " (" << paths.size() << "):\n";
        const std::size_t n = std::min(paths.size(), pm::llm::k_prompt_path_list_max);
        for (std::size_t i = 0; i < n; ++i) {
            const std::string shown = locator_field(paths[i], 4096);
            if (shown.empty())
                continue;
            s << "  [" << i << "] " << shown << "\n";
        }
        if (paths.size() > n)
            s << "  ... " << (paths.size() - n) << " more paths omitted\n";
        s << trailer;
    };
    // Each block is skipped only when system-prompt.md already substitutes that
    // placeholder. Editor caret has no placeholder, so it stays here.
    if (sections.selection && !turn.selection.empty())
        append_paths("Selected files", turn.selection,
            "Use these paths in tool calls unless the user names a different path.\n\n");
    if (sections.open_files && !turn.open_files.empty())
        append_paths("Open files", turn.open_files,
            "These are files open in viewer tabs; they may differ from the current file-panel selection.\n\n");
    if (sections.editor && !turn.editor_path.empty() && turn.editor_line > 0) {
        const std::string editor_path = locator_field(turn.editor_path, 4096);
        if (editor_path.empty()) {
            /* A clipped path is not a path. Skip the caret block. */
        } else {
        s << "Editor caret: " << editor_path
          << ':' << turn.editor_line
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
    }
    if (sections.folder && turn.selection.empty() && turn.open_files.empty()) {
        const std::string folder = locator_field(turn.folder_hint, 4096);
        if (!folder.empty()) {
            s << "Working folder: "
              << folder
              << " (no file selection). Use this exact path in tool calls; invented paths will fail.\n\n";
        } else if (sections.selection && sections.open_files && turn.folder_hint.empty()) {
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

    if (has("file_read")) {
        s << "  - For existing large files: **file_read(offset, limit)**. An unwindowed file_read on a large file fails — "
             "do not retry without a window. Never whole-file-read then whole-file-write "
             "unless the task is a genuine full rewrite.\n";
        if (has("file_search"))
            s << "  - Locate a passage with **file_search** before reading a large file.\n";
        if (has("search_index") && has("search_query"))
            s << "  - A converted document is **search_index** on that folder, then **search_query**"
              << (has("search_detail") ? ", then **search_detail** for one passage" : "")
              << ". Do not file_read each part.\n";
        else if (has("request_tools"))
            s << "  - If a converted document needs search_index, call **request_tools** before searching.\n";
    }
    if (has("search_index") && has("search_query")) {
        s << "  - After download or pdf-to-md, call **search_index** on the output folder, "
             "then **search_query** once.";
        if (has("search_detail"))
            s << " **search_detail** at most once.";
        s << " Do not keep searching.\n"
             "  - A session item with role=search_index is an index from an earlier turn. "
             "Follow up with **search_query** using its index id.\n";
    }
    for (const auto& rule : k_tool_rules) {
        if (has(rule.tool))
            s << rule.text;
    }

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
    using ACF = pm::llm::AgentContextFeature;
    const auto context_on = [&](ACF feature) {
        return pm::llm::has_feature(turn.context_features, feature);
    };
    const PromptFileLoad system_prompt_file = context_on(ACF::System)
        ? load_system_prompt_md()
        : PromptFileLoad{};
    const std::string external_raw = context_on(ACF::System)
        ? render_prompt_file(system_prompt_file.content, turn)
        : std::string{};
    const std::string external = external_raw.size() <= pm::llm::k_system_prompt_file_chars
        ? external_raw
        : media::str_san::utf8_safe_ellipsize(external_raw, pm::llm::k_system_prompt_file_chars, "\n\u2026\n");

    if (prompt_meta) {
        const bool from_file = context_on(ACF::System) && !external.empty();
        (*prompt_meta)["system"] = {
            {"source", from_file ? "file" : "builtin"},
            {"path", from_file && !system_prompt_file.path.empty()
                ? nlohmann::json(media::path::path_to_utf8(system_prompt_file.path))
                : nlohmann::json(nullptr)},
        };
    }

    std::ostringstream s;
    const ToolCapabilities caps = ToolCapabilities::from_json(tools);
    const bool has_memory_read = caps.memory_read;
    const bool has_memory_write = caps.memory_write;
    const bool has_memory_find = caps.memory_find;
    const bool has_memory_tools = has_memory_read || has_memory_write;

    if (!external.empty())
        s << external << "\n\n---\n\n";

    if (context_on(ACF::Runtime))
        append_runtime_environment_context(s);
    const auto prompt_has = [&](const char* token) {
        return context_on(ACF::System)
            && system_prompt_file.content.find(token) != std::string::npos;
    };
    if (context_on(ACF::Location))
        s << user_location_prompt_suffix(turn, prompt_has("${USER_LOCATION}"));
    append_agent_tooling_and_context(s, turn, tools, ContextSections{
        .identity = external.empty(),
        .selection = context_on(ACF::Selection) && !prompt_has("${CURRENT_SELECTION}"),
        .open_files = context_on(ACF::OpenFiles) && !prompt_has("${CURRENT_OPEN_FILES}"),
        .folder = context_on(ACF::Folder) && !prompt_has("${FOLDER}"),
        .editor = context_on(ACF::Editor),
    });

    // ── P1: planner intent-context block ─────────────────────────────────────
    // Emitted whenever the planner produced at least one intent-context field,
    // including zero-tool turns (fallback=true, reason="planner selected no
    // tools") — these are pure-query turns where intent is still meaningful.
    // Only skipped when fallback means "planner didn't attempt" (error /
    // disabled / realtime), detected by absence of any intent field.
    if (context_on(ACF::Planner) && turn.planner_meta.is_object() &&
        (turn.planner_meta.contains("intent") || turn.planner_meta.contains("action") ||
         turn.planner_meta.contains("subject"))) {
        const std::string intent  = clip_field(json_sv(turn.planner_meta, "intent"), 200);
        const std::string action  = clip_field(json_sv(turn.planner_meta, "action"), 200);
        const std::string subject = clip_field(json_sv(turn.planner_meta, "subject"), 200);
        const float confidence    = json_number_or(turn.planner_meta, "confidence", 1.0f);
        if (!intent.empty() || !action.empty() || !subject.empty()) {
            s << "\n## Turn Context\n";
            if (!intent.empty())  s << "- intent: "  << intent  << "\n";
            if (!action.empty())  s << "- action: "  << action  << "\n";
            if (!subject.empty()) s << "- subject: " << subject << "\n";
            if (confidence < 0.5f)
                s << "- confidence: low \xe2\x80\x94 verify intent with the user before acting\n";
            // Planner reason: pass through when it carries actionable guidance
            // (e.g. a resolved exe path, a custom command id, a chosen viewer).
            const std::string reason = clip_field(json_sv(turn.planner_meta, "reason"), 200);
            if (!reason.empty())
                s << "- planner note: " << reason << "\n";
            if (json_child_bool(turn.planner_meta, "plan_invalid_after_filter")) {
                if (turn.planner_meta.contains("rejected_tools") &&
                    turn.planner_meta["rejected_tools"].is_array() &&
                    !turn.planner_meta["rejected_tools"].empty()) {
                    s << "- rejected tools:";
                    std::size_t n = 0;
                    for (const auto& name : turn.planner_meta["rejected_tools"]) {
                        if (!name.is_string())
                            continue;
                        if (n >= 12) {
                            s << " \u2026";
                            break;
                        }
                        const std::string shown = clip_field(name.get_ref<const std::string&>(), 40);
                        if (shown.empty())
                            continue;
                        s << " " << shown;
                        ++n;
                    }
                    s << "\n";
                }
                const std::string why = clip_field(json_sv(turn.planner_meta, "rejected_reason"), 200);
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
    if (context_on(ACF::Mode) && turn.mode != TurnMode::Auto) {
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
            if (overlay.size() <= pm::llm::k_prompt_overlay_chars)
                s << overlay << "\n";
            else
                s << media::str_san::utf8_safe_ellipsize(overlay, pm::llm::k_prompt_overlay_chars, "\n\u2026\n") << "\n";
        }
    }

    if (!turn.task_id.empty()) {
        if (turn.is_scheduled_tick) {
            // ── Scheduled task context ───────────────────────────────────────
            s << "\nScheduled task context:\n"
              << "  task_id: " << clip_field(turn.task_id, 80) << "\n";
            if (turn.memory_state.is_object() && !turn.memory_state.empty())
                s << "Persistent memory state (JSON):\n"
                  << prompt_json(turn.memory_state, pm::llm::k_prompt_json_inject_chars) << "\n";
            if (turn.recent_events.is_array() && !turn.recent_events.empty()) {
                const std::size_t n = turn.recent_events.size();
                const std::size_t keep = std::min(n, pm::llm::k_scheduled_events_inject_max);
                nlohmann::json tail = nlohmann::json::array();
                for (std::size_t i = n - keep; i < n; ++i)
                    tail.push_back(turn.recent_events[i]);
                s << "Recent task events (newest last):\n"
                  << prompt_json(tail, pm::llm::k_prompt_json_inject_chars) << "\n";
            }
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
            const bool native_history =
                pm::llm::has_feature(turn.structured_memory_features, SMF::InjectRecentMessages);
            // Quotes move into the message list when native history is on.
            // Tool hints stay here so a follow-up can still see the call id.
            if (pm::llm::has_feature(turn.structured_memory_features, SMF::InjectItems)
                && !replay_transcript)
                append_recent_turns(s, turn.memory_state, RecentTurnView{
                    .advertise_body_recall = has_memory_find,
                    .include_quotes = !native_history,
                });
            if (pm::llm::has_feature(turn.structured_memory_features, SMF::InjectItems))
                append_session_items(s, turn.memory_state, 0, 0, has_memory_find);
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
                    auto ev_sv = [](const nlohmann::json& o, const char* key) -> std::string_view {
                        if (!o.is_object()) return {};
                        const auto it = o.find(key);
                        if (it == o.end() || !it->is_string()) return {};
                        return it->get_ref<const std::string&>();
                    };
                    const std::string ts = clip_field(ev_sv(ev, "ts"), 40);
                    const std::string_view user_sv = ev_sv(ev, "user");
                    const std::string_view asst_sv = ev_sv(ev, "assistant");
                    const std::string user = user_sv.size() <= pm::llm::k_session_memory_user_chars
                        ? std::string(user_sv)
                        : media::str_san::utf8_safe_ellipsize(user_sv, pm::llm::k_session_memory_user_chars, "\u2026");
                    const std::string asst = asst_sv.size() <= pm::llm::k_session_memory_asst_chars
                        ? std::string(asst_sv)
                        : media::str_san::utf8_safe_ellipsize(asst_sv, pm::llm::k_session_memory_asst_chars, "\u2026");

                    std::string tool_line;
                    if (ev.contains("tools") && ev["tools"].is_array()) {
                        for (const auto& tn : ev["tools"]) {
                            if (!tn.is_string()) continue;
                            const std::string name = clip_field(tn.get_ref<const std::string&>(), 40);
                            if (name.empty()) continue;
                            if (!tool_line.empty()) tool_line += ", ";
                            if (tool_line.size() >= 160) {
                                tool_line += "\u2026";
                                break;
                            }
                            tool_line += name;
                        }
                    }

                    std::string line;
                    if (!ts.empty()) line += "[" + ts + "] ";
                    line += "User: \u201c" + user + "\u201d";
                    if (!tool_line.empty())
                        line += "\n  \u2192 tools: " + tool_line;
                    if (ev.contains("spoken") && ev["spoken"].is_array()) {
                        std::size_t spoken_n = 0;
                        for (const auto& sp : ev["spoken"]) {
                            if (!sp.is_string()) continue;
                            if (++spoken_n > 4) {
                                line += "\n  \u2192 spoke: \u2026";
                                break;
                            }
                            line += "\n  \u2192 spoke: \u201c"
                                + clip_field(sp.get_ref<const std::string&>(), 160)
                                + "\u201d";
                        }
                    }
                    if (ev.contains("media_outputs") && ev["media_outputs"].is_array()) {
                        std::string mline;
                        std::size_t media_n = 0;
                        for (const auto& mp : ev["media_outputs"]) {
                            if (!mp.is_string()) continue;
                            if (++media_n > 8) {
                                mline += ", \u2026";
                                break;
                            }
                            const std::string& p = mp.get_ref<const std::string&>();
                            const auto slash = p.find_last_of("/\\");
                            const std::string_view base = slash != std::string::npos
                                ? std::string_view(p).substr(slash + 1)
                                : std::string_view(p);
                            const std::string name = clip_field(base, 80);
                            if (name.empty()) continue;
                            mline += (mline.empty() ? "" : ", ") + name;
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
            if (pm::llm::has_feature(turn.structured_memory_features, SMF::InjectStoredFacts)
                && turn.memory_state.is_object() && !turn.memory_state.empty()) {
                if (!has_structured_session_memory(turn.memory_state)) {
                    s << "\n## Stored Memory\n"
                         "Facts you have explicitly saved with memory_write:\n"
                      << prompt_json(turn.memory_state, pm::llm::k_prompt_json_inject_chars) << "\n";
                } else {
                    nlohmann::json extras = nlohmann::json::object();
                    std::size_t nkeys = 0;
                    for (auto it = turn.memory_state.begin(); it != turn.memory_state.end(); ++it) {
                        if (it.key() == "items" || it.key() == "artifacts"
                            || it.key() == "instructions" || it.key() == "journal_recovery")
                            continue;
                        if (it.key().size() > 128)
                            continue;
                        if (nkeys >= 24)
                            break;
                        extras[it.key()] = it.value();
                        ++nkeys;
                    }
                    const std::string dumped = prompt_json(extras, 800);
                    if (!dumped.empty() && dumped != "{}") {
                        s << "\n## Stored Memory\n"
                             "Explicit facts saved beside structured session items:\n"
                          << dumped << "\n";
                    }
                }
            }
            if (has_memory_tools) {
                s << "\n";
                if (has_memory_write)
                    s << "Use `memory_write` to persist relevant, non-sensitive facts and preferences "
                         "for this session (name, format, notes). Never store credentials, API keys, "
                         "passwords, tokens, or private keys. ";
                if (has_memory_read)
                    s << "Use `memory_read` to retrieve them. ";
                s << "Memory persists for the lifetime of this chat session.\n";
            }
        }
    }

    if (context_on(ACF::Extra) && !turn.system_extra.empty()) {
        const std::string_view extra = turn.system_extra;
        s << "\n";
        if (extra.size() <= pm::llm::k_system_extra_inject_chars)
            s << extra;
        else
            s << media::str_san::utf8_safe_ellipsize(extra, pm::llm::k_system_extra_inject_chars, "\n\u2026\n");
        s << "\n";
    }

    if (context_on(ACF::Preferences)) {
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
            const std::string rendered = render_prompt_file(prefs.content, turn);
            s << "\n## User preferences\n";
            if (rendered.size() <= pm::llm::k_prompt_overlay_chars)
                s << rendered << "\n";
            else
                s << media::str_san::utf8_safe_ellipsize(rendered, pm::llm::k_prompt_overlay_chars, "\n\u2026\n") << "\n";
        }
    }

    std::string out = s.str();
    if (out.size() > pm::llm::k_system_prompt_max_chars) {
        std::size_t cut = out.rfind('\n', pm::llm::k_system_prompt_max_chars);
        if (cut == std::string::npos || cut < pm::llm::k_system_prompt_max_chars / 2)
            cut = pm::llm::k_system_prompt_max_chars;
        out.resize(cut);
        out += "\n[remaining context omitted]\n";
    }
    return out;
}

std::vector<DialogueMessage> recent_dialogue_messages(
    const nlohmann::json& memory_state,
    std::string_view current_user)
{
    std::vector<DialogueMessage> out;
    if (!memory_state.is_object() || !memory_state.contains("items")
        || !memory_state["items"].is_object())
        return out;

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
    if (pairs.size() > pm::llm::k_recent_messages_max_pairs)
        pairs.resize(pm::llm::k_recent_messages_max_pairs);
    if (pairs.empty())
        return out;

    std::size_t budget = pm::llm::k_recent_messages_chars;
    std::vector<DialogueMessage> newest_first;
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const auto& item = *pairs[i].second;
        const auto& meta = item.contains("metadata") && item["metadata"].is_object()
            ? item["metadata"] : item;
        const std::string user = json_child_string(meta, "user");
        if (i == 0 && !current_user.empty() && user == current_user)
            continue;
        std::string assistant = json_child_string(meta, "assistant");
        const bool newest = newest_first.empty();
        if (newest && json_child_bool(meta, "body_available")
            && meta.contains("source_log") && meta["source_log"].is_object()) {
            const auto& log = meta["source_log"];
            media::llm::chat_sessions::AgentFinalTextSlice slice;
            bool found = false;
            std::string err;
            const std::size_t want = std::min(budget, pm::llm::k_recent_messages_prev_reply_chars);
            if (media::llm::chat_sessions::load_agent_final_text(
                    json_child_string(log, "session_id"),
                    json_child_string(log, "log_id"),
                    json_child_string(log, "path"),
                    0, want, slice, found, err) && found && !slice.text.empty()) {
                assistant = std::move(slice.text);
                if (slice.truncated) {
                    const std::string id = locator_field(pairs[i].first, 80);
                    if (!id.empty())
                        assistant += "\n(truncated; memory_find itemId=" + id + " includeBody=true)";
                }
            }
        }
        if (user.empty() && assistant.empty())
            continue;
        auto take = [&](std::string role, std::string text) {
            if (text.empty() || budget < 16)
                return;
            if (text.size() + 1 > budget)
                text = media::str_san::utf8_safe_ellipsize(text, budget, "\u2026");
            if (text.empty())
                return;
            budget -= text.size();
            newest_first.push_back(DialogueMessage{std::move(role), std::move(text)});
        };
        if (!assistant.empty())
            take("assistant", std::move(assistant));
        if (!user.empty())
            take("user", user);
        if (budget < 16)
            break;
    }
    out.assign(newest_first.rbegin(), newest_first.rend());
    return out;
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

namespace {

std::mutex g_user_location_mu;
std::string g_user_location;

} // namespace

void set_cached_user_location(std::string line)
{
    line = normalize_user_location(std::move(line));
    std::lock_guard<std::mutex> lock(g_user_location_mu);
    g_user_location = std::move(line);
}

std::string cached_user_location()
{
    std::lock_guard<std::mutex> lock(g_user_location_mu);
    return g_user_location;
}

} // namespace media::llm::agent

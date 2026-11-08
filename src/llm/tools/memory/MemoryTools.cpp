// Copyright PolyMech / pm-image. Internal use only.
#include "llm/tools/memory/MemoryTools.hpp"
#include "json/json_utils.hpp"

#include "llm/agent_memory.hpp"
#include "llm/agent_memory_recall.hpp"
#include "llm/chat_session_store.hpp"
#include "model/resource_types.hpp"
#include "pm_branding_config.hpp"

#if defined(FEATURE_SECURE_STORAGE) && FEATURE_SECURE_STORAGE
#include "storage/secure_storage.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <vector>
#include "string/string_utils.hpp"

namespace media::llm::path::detail {

namespace {

// ── Global long-term memory helpers ───────────────────────────────────────
//
// For real scheduled tasks (exist in s.tasks) we go through task_store_*
// so in-memory ephemeral state is preserved as before.
//
// For regular chat turns (including single-turn CLI calls) we persist one
// global long-term memory document:
//   default_memory.doc
// under the sessions storage root (secure URI when secure backend is active).
//
// Document format:
//   { "memory_state": { ... }, "updated_at": "..." }

constexpr const char* k_default_memory_doc_name = "default_memory.doc";

std::string default_memory_uri()
{
    return std::string("pm://session-memory/") + k_default_memory_doc_name;
}

std::vector<std::string> normalize_tags(const nlohmann::json& tags_arg)
{
    std::vector<std::string> tags;
    if (!tags_arg.is_array()) return tags;
    for (const auto& t : tags_arg) {
        if (!t.is_string()) continue;
        const std::string s = t.get<std::string>();
        if (s.empty()) continue;
        if (std::find(tags.begin(), tags.end(), s) == tags.end())
            tags.push_back(s);
    }
    return tags;
}

bool secure_plaintext_allowed(media::llm::chat_sessions::ProtectionMode protection)
{
    using ProtectionMode = media::llm::chat_sessions::ProtectionMode;
    return protection == ProtectionMode::Plain || protection == ProtectionMode::Signed;
}

bool load_default_memory_document(nlohmann::json& out_document, bool& found, std::string& err)
{
    err.clear();
    found = false;
    out_document = nullptr;

    auto options = media::llm::chat_sessions::active_options();
    if (options.protection == media::llm::chat_sessions::ProtectionMode::Plain ||
        options.protection == media::llm::chat_sessions::ProtectionMode::Encrypted) {
        options.protection = media::llm::chat_sessions::ProtectionMode::Signed;
    }
    const auto root = options.root_override.empty() ? media::llm::agent::session_store_dir()
                                                     : options.root_override;

#if defined(FEATURE_SECURE_STORAGE) && FEATURE_SECURE_STORAGE
    if (options.backend == media::llm::chat_sessions::StoreBackend::SecureStorage) {
        pm::storage::DocumentLoadPolicy policy;
        policy.allowed_trust = {pm::model::TrustLevel::UserLocal};
        policy.require_encryption =
            options.protection == media::llm::chat_sessions::ProtectionMode::Encrypted ||
            options.protection == media::llm::chat_sessions::ProtectionMode::EncryptedWithUserPresence;
        policy.require_signature = options.protection != media::llm::chat_sessions::ProtectionMode::Plain;
        policy.allow_vendor_seed_plaintext = secure_plaintext_allowed(options.protection);

        const auto paths = pm::storage::resolve_storage_paths(root);
        const auto loaded = pm::storage::load_document(paths, default_memory_uri(), policy);
        if (!loaded.ok) {
            if (loaded.error.rfind("document not found:", 0) == 0)
                return true;
            err = loaded.error;
            return false;
        }
        found = true;
        try {
            out_document = media::json::try_parse(std::string_view(
                reinterpret_cast<const char*>(loaded.document.data.data()),
                loaded.document.data.size()));
        } catch (...) {
            out_document = nullptr;
        }
        if (!out_document.is_object()) {
            err = "invalid default memory json";
            out_document = nullptr;
            return false;
        }
        return true;
    }
#endif

    const auto path = root / k_default_memory_doc_name;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return true;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "open default memory failed";
        return false;
    }
    out_document = media::json::try_parse(f);
    if (!out_document.is_object()) {
        err = "invalid default memory json";
        out_document = nullptr;
        return false;
    }
    found = true;
    return true;
}

bool save_default_memory_document(const nlohmann::json& document, std::string& err)
{
    err.clear();
    auto options = media::llm::chat_sessions::active_options();
    if (options.protection == media::llm::chat_sessions::ProtectionMode::Plain ||
        options.protection == media::llm::chat_sessions::ProtectionMode::Encrypted) {
        options.protection = media::llm::chat_sessions::ProtectionMode::Signed;
    }
    const auto root = options.root_override.empty() ? media::llm::agent::session_store_dir()
                                                     : options.root_override;

#if defined(FEATURE_SECURE_STORAGE) && FEATURE_SECURE_STORAGE
    if (options.backend == media::llm::chat_sessions::StoreBackend::SecureStorage) {
        const std::string text = document.dump(2);
        pm::storage::PutDocumentRequest req;
        req.uri = default_memory_uri();
        req.type = pm::model::ResourceType::SessionMemory;
        req.trust = pm::model::TrustLevel::UserLocal;
        req.version = "1";
        req.provider = pm::brand::k_app_id_u8;
        req.encrypt = !secure_plaintext_allowed(options.protection);
        req.allow_plaintext_seed = secure_plaintext_allowed(options.protection);
        req.data.assign(text.begin(), text.end());

        const auto paths = pm::storage::resolve_storage_paths(root);
        const auto saved = pm::storage::put_document(paths, req);
        if (!saved.ok) {
            err = saved.error;
            return false;
        }
        if (options.protection != media::llm::chat_sessions::ProtectionMode::Plain) {
            const auto signed_doc = pm::storage::sign_document(paths, req.uri);
            if (!signed_doc.ok) {
                err = signed_doc.error;
                return false;
            }
        }
        return true;
    }
#endif

    const auto path = root / k_default_memory_doc_name;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        err = ec.message();
        return false;
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        err = "open default memory failed";
        return false;
    }
    f << document.dump(2);
    if (!f.good()) {
        err = "write default memory failed";
        return false;
    }
    return true;
}

/// Load the long-term memory_state JSON from default_memory.doc.
/// Returns empty object when no document exists yet.
nlohmann::json default_memory_doc_read(std::string& err)
{
    nlohmann::json doc;
    bool found = false;
    if (!load_default_memory_document(doc, found, err))
        return nlohmann::json::object();
    if (!found || !doc.is_object())
        return nlohmann::json::object();
    return doc.value("memory_state", nlohmann::json::object());
}

nlohmann::json default_memory_doc_read_items(std::string& err)
{
    nlohmann::json doc;
    bool found = false;
    if (!load_default_memory_document(doc, found, err))
        return nlohmann::json::object();
    if (!found || !doc.is_object())
        return nlohmann::json::object();
    return doc.value("memory_items", nlohmann::json::object());
}

/// Merge `patch` into the global default memory document.
/// An empty patch object clears all memory.
bool default_memory_doc_write(const nlohmann::json& patch,
                              const std::vector<std::string>& tags,
                              std::string& err)
{
    nlohmann::json doc;
    bool found = false;
    if (!load_default_memory_document(doc, found, err))
        return false;

    nlohmann::json memory_state = nlohmann::json::object();
    nlohmann::json memory_items = nlohmann::json::object();
    if (found && doc.is_object())
        memory_state = doc.value("memory_state", nlohmann::json::object());
    if (found && doc.is_object())
        memory_items = doc.value("memory_items", nlohmann::json::object());

    const std::string ts = media::llm::agent::now_iso8601();

    if (patch.empty() || (patch.is_object() && patch.empty())) {
        memory_state = nlohmann::json::object();
        memory_items = nlohmann::json::object();
    } else {
        // Merge: null values delete keys; other values overwrite.
        for (auto it = patch.begin(); it != patch.end(); ++it) {
            if (it.value().is_null()) {
                memory_state.erase(it.key());
                memory_items.erase(it.key());
            } else {
                memory_state[it.key()] = it.value();
                nlohmann::json item = nlohmann::json::object();
                if (memory_items.contains(it.key()) && memory_items[it.key()].is_object())
                    item = memory_items[it.key()];
                item["value"] = it.value();
                item["updated_at"] = ts;
                if (!tags.empty()) {
                    item["tags"] = nlohmann::json::array();
                    for (const auto& t : tags) item["tags"].push_back(t);
                } else if (!item.contains("tags")) {
                    item["tags"] = nlohmann::json::array();
                }
                memory_items[it.key()] = std::move(item);
            }
        }
    }

    nlohmann::json new_doc{
        {"memory_state", memory_state},
        {"memory_items", memory_items},
        {"updated_at",   media::llm::agent::now_iso8601()},
    };
    return save_default_memory_document(new_doc, err);
}

} // namespace

// ── Tool implementations ──────────────────────────────────────────────────

ExecuteResult do_memory_read(const nlohmann::json& args) {
    std::string id = args.value("task_id", std::string{});

    // Explicit task_id → read scheduler/session task-store entry.
    if (!id.empty()) {
        const auto task = media::llm::agent::task_store_get(id);
        if (!task)
            return simple_error("memory_read: task not found: " + id);
        const bool empty = task->memory_state.empty() ||
                           (task->memory_state.is_object() && task->memory_state.empty());
        ExecuteResult r;
        r.ok = true;
        r.envelope = nlohmann::json{
            {"ok",      true},
            {"state",   task->memory_state},
            {"source",  "task"},
            {"message", empty ? "No facts stored yet. Use memory_write to save some."
                              : "Memory loaded successfully."},
        };
        return r;
    }

    // Default path → load global long-term memory document.
    std::string err;
    const nlohmann::json memory_state = default_memory_doc_read(err);
    if (!err.empty())
        return simple_error("memory_read: " + err);
    const nlohmann::json memory_items = default_memory_doc_read_items(err);
    if (!err.empty())
        return simple_error("memory_read: " + err);

    const bool empty = memory_state.empty() ||
                       (memory_state.is_object() && memory_state.empty());
    ExecuteResult r;
    r.ok = true;
    r.envelope = nlohmann::json{
        {"ok",      true},
        {"state",   memory_state},
        {"items",   memory_items},
        {"source",  "default_memory"},
        {"message", empty ? "No facts stored yet. Use memory_write to save some."
                          : "Memory loaded successfully."},
    };
    return r;
}

ExecuteResult do_memory_write(const nlohmann::json& args) {
    if (!args.contains("state") || !args["state"].is_object())
        return simple_error("memory_write: 'state' (object) is required");

    const std::string id = args.value("task_id", std::string{});

    // Explicit task_id → write into scheduler/session task-store entry.
    if (!id.empty()) {
        if (!media::llm::agent::task_store_write_memory(id, args["state"]))
            return simple_error("memory_write: task context not found: " + id);
        ExecuteResult r;
        r.ok = true;
        r.envelope = nlohmann::json{
            {"ok",      true},
            {"saved",   args["state"]},
            {"source",  "task"},
            {"message", "Memory saved. It will be injected into your system prompt on the next turn."},
        };
        return r;
    }

    const std::vector<std::string> tags =
        args.contains("tags") ? normalize_tags(args["tags"]) : std::vector<std::string>{};

    // Default path → global long-term memory document.
    std::string err;
    if (!default_memory_doc_write(args["state"], tags, err))
        return simple_error("memory_write: " + (err.empty() ? "secure storage unavailable" : err));

    ExecuteResult r;
    r.ok = true;
    r.envelope = nlohmann::json{
        {"ok",      true},
        {"saved",   args["state"]},
        {"tags",    tags},
        {"updated_at", media::llm::agent::now_iso8601()},
        {"source",  "default_memory"},
        {"message", "Memory saved. It will be injected into your system prompt on the next turn."},
    };
    return r;
}

ExecuteResult do_memory_append_event(const nlohmann::json& args) {
    if (!args.contains("event") || !args["event"].is_object())
        return simple_error("memory_append_event: 'event' (object) is required");

    const std::string id = media::llm::agent::get_current_task_id();
    if (id.empty())
        return simple_error("memory_append_event: no active session or scheduled task context");

    nlohmann::json ev = args["event"];
    if (!ev.contains("time")) ev["time"] = media::llm::agent::now_iso8601();
    media::llm::agent::task_store_append_event(id, ev);
    return simple_ok(nlohmann::json{{"task_id", id}});
}

bool tool_payload_retrieval_denied(std::string name) {
    name = media::strutil::lower_ascii(std::move(name));
    static constexpr std::string_view denied[] = {
        "credential", "consent", "security_review", "security_decision",
        "oauth", "api_key", "private_key", "secret_store", "auth_token",
    };
    for (const auto needle : denied) {
        if (name.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

ExecuteResult do_memory_find(const nlohmann::json& args) {
    const std::string query =
        args.value("query", std::string{});
    const std::string item_id =
        args.value("itemId", std::string{});
    const std::string requested_session_id =
        args.value("sessionId", std::string{});
    const bool include_body =
        args.value("includeBody", false);
    const bool include_payload =
        args.value("includePayload", false);
    const std::string tool_call_id =
        args.value("toolCallId", std::string{});
    const std::size_t body_offset =
        args.contains("offset") && args["offset"].is_number_unsigned()
            ? args["offset"].get<std::size_t>()
            : (args.contains("offset") && args["offset"].is_number_integer()
                   ? static_cast<std::size_t>(std::max<long long>(0, args["offset"].get<long long>()))
                   : 0);
    const std::size_t max_body_bytes =
        args.contains("maxBytes") && args["maxBytes"].is_number_integer()
            ? static_cast<std::size_t>(std::clamp<long long>(
                  args["maxBytes"].get<long long>(), 1, 256 * 1024))
            : 64 * 1024;
    const bool search_past_chats =
        args.value("searchPastChats", false);
    const bool include_artifacts =
        args.value("includeArtifacts", false);
    const bool include_security =
        args.value("includeSecurity", false);
    const int max_results =
        args.contains("maxResults") && args["maxResults"].is_number_integer()
            ? std::clamp(args["maxResults"].get<int>(), 1, 200) : 30;

    nlohmann::json results = nlohmann::json::array();
    std::string err;

    // Exact turn materialization is deliberately separate from fuzzy search:
    // itemId identifies a persisted, internally-created turn_pair and its
    // source_log locator identifies the canonical agent-run document.
    if (!item_id.empty()) {
        const nlohmann::json store = media::llm::chat_sessions::query_store(err);
        if (!err.empty())
            return simple_error("memory_find: " + err);

        const std::string current_session_id = media::llm::agent::get_current_task_id();
        nlohmann::json exact;
        if (store.contains("memoryItems") && store["memoryItems"].is_array()) {
            for (const auto& row : store["memoryItems"]) {
                if (!row.is_object() || row.value("id", std::string{}) != item_id)
                    continue;
                const std::string row_session_id = row.value("sessionId", std::string{});
                if (!requested_session_id.empty() && row_session_id != requested_session_id)
                    continue;
                if (exact.is_null() ||
                    (!current_session_id.empty() && row_session_id == current_session_id))
                    exact = row;
                if (!current_session_id.empty() && row_session_id == current_session_id)
                    break;
            }
        }

        ExecuteResult r;
        r.ok = true;
        if (exact.is_null()) {
            r.envelope = {
                {"ok", true},
                {"count", 0},
                {"items", nlohmann::json::array()},
                {"message", "Exact memory item not found."},
            };
            return r;
        }

        r.envelope = {
            {"ok", true},
            {"count", 1},
            {"items", nlohmann::json::array({exact})},
            {"message", "Exact memory item found."},
        };
        if (include_payload) {
            r.envelope["payload_found"] = false;
            const nlohmann::json metadata =
                exact.value("metadata", nlohmann::json::object());
            const bool is_turn_pair =
                exact.value("role", std::string{}) == "turn_pair";
            const bool is_tool_observation =
                exact.value("kind", std::string{}) == "tool_observation" &&
                metadata.value("payload_available", false);
            const bool is_tool_checkpoint =
                exact.value("kind", std::string{}) == "tool_checkpoint" &&
                metadata.value("payload_available", false);
            std::string resolved_call_id = tool_call_id;
            if (is_tool_observation || is_tool_checkpoint)
                resolved_call_id =
                    metadata.value("transcript_tool_call_id", std::string{});
            if (resolved_call_id.empty()) {
                r.envelope["message"] =
                    "toolCallId is required when includePayload=true.";
                return r;
            }
            const std::string owner_session_id =
                exact.value("sessionId", std::string{});
            if (current_session_id.empty() ||
                owner_session_id != current_session_id) {
                r.envelope["message"] =
                    "Exact tool payload retrieval is limited to the current session.";
                return r;
            }
            const nlohmann::json source_log =
                metadata.value("source_log", nlohmann::json::object());
            if ((!is_turn_pair && !is_tool_observation && !is_tool_checkpoint) ||
                !source_log.is_object()) {
                r.envelope["message"] =
                    "This item has no canonical tool-result locator.";
                return r;
            }

            media::llm::chat_sessions::AgentToolResultSlice slice;
            bool payload_found = false;
            if (!media::llm::chat_sessions::load_agent_tool_result(
                    source_log.value("session_id", std::string{}),
                    source_log.value("log_id", std::string{}),
                    source_log.value("path", std::string{}),
                    resolved_call_id, body_offset, max_body_bytes,
                    slice, payload_found, err)) {
                return simple_error("memory_find: " + err);
            }
            if (payload_found && tool_payload_retrieval_denied(slice.tool_name)) {
                r.envelope["payload_denied"] = true;
                r.envelope["message"] =
                    "Retrieval of this sensitive tool payload is denied.";
                return r;
            }
            r.envelope["payload_found"] = payload_found;
            if (!payload_found) {
                r.envelope["message"] =
                    "The canonical tool result is missing or the call ID was not found.";
                return r;
            }
            r.envelope["tool_name"] = slice.tool_name;
            r.envelope["tool_call_id"] = resolved_call_id;
            r.envelope["payload_text"] = std::move(slice.text);
            r.envelope["total_bytes"] = slice.total_bytes;
            r.envelope["offset"] = slice.offset;
            r.envelope["next_offset"] = slice.offset +
                r.envelope["payload_text"].get_ref<const std::string&>().size();
            r.envelope["truncated"] = slice.truncated;
            r.envelope["message"] = slice.truncated
                ? "Tool payload materialized; request next_offset for remaining bytes."
                : "Tool payload materialized.";
        }
        if (!include_body)
            return r;

        const nlohmann::json metadata =
            exact.value("metadata", nlohmann::json::object());
        const nlohmann::json source_log =
            metadata.value("source_log", nlohmann::json::object());
        if (exact.value("role", std::string{}) != "turn_pair" ||
            !metadata.value("body_available", false) || !source_log.is_object()) {
            r.envelope["body_found"] = false;
            r.envelope["message"] = "This item has no retrievable assistant body.";
            return r;
        }

        media::llm::chat_sessions::AgentFinalTextSlice slice;
        bool body_found = false;
        if (!media::llm::chat_sessions::load_agent_final_text(
                source_log.value("session_id", std::string{}),
                source_log.value("log_id", std::string{}),
                source_log.value("path", std::string{}),
                body_offset, max_body_bytes, slice, body_found, err)) {
            return simple_error("memory_find: " + err);
        }
        r.envelope["body_found"] = body_found;
        if (!body_found) {
            r.envelope["message"] = "The canonical agent log is missing or was deleted.";
            return r;
        }
        r.envelope["final_text"] = std::move(slice.text);
        r.envelope["total_bytes"] = slice.total_bytes;
        r.envelope["offset"] = slice.offset;
        r.envelope["next_offset"] = slice.offset +
            r.envelope["final_text"].get_ref<const std::string&>().size();
        r.envelope["truncated"] = slice.truncated;
        r.envelope["message"] = slice.truncated
            ? "Assistant body materialized; request next_offset for the remaining bytes."
            : "Assistant body materialized.";
        return r;
    }

    // Query mode shares the same BM25 + optional dense + RRF ranker used by
    // automatic recall. Empty-query listing and explicit artifact/security
    // inventory retain the legacy enumerator behavior below.
    if (!query.empty() && !include_artifacts && !include_security) {
        const std::string current_session_id =
            media::llm::agent::get_current_task_id();
        nlohmann::json memory_state = nlohmann::json::object();
        if (!current_session_id.empty())
            (void)media::llm::agent::session_read_memory_state(
                current_session_id, memory_state);

        auto candidates = media::llm::agent::memory_recall_collect_candidates(
            current_session_id, memory_state, search_past_chats);
        candidates.erase(
            std::remove_if(candidates.begin(), candidates.end(),
                [&](const media::llm::agent::MemoryRecallCandidate& candidate) {
                    if (!requested_session_id.empty() &&
                        candidate.session_id != requested_session_id)
                        return true;
                    // Path artifacts are opt-in in memory_find.
                    return candidate.item.contains("locator") &&
                           candidate.item["locator"].is_object() &&
                           candidate.item["locator"].contains("path");
                }),
            candidates.end());

        auto settings = media::llm::agent::MemoryRecallSettings::resolve();
        settings.top_k = static_cast<std::size_t>(max_results);
        media::llm::agent::MemoryRecallTelemetry telemetry;
        const auto hits = media::llm::agent::memory_recall_rank(
            query, candidates, settings,
            pm::llm::k_default_structured_memory_features,
            {}, &telemetry);
        for (const auto& hit : hits) {
            nlohmann::json row = hit.item;
            if (!row.is_object()) continue;
            row["sessionId"] = hit.session_id;
            if (!row.contains("id")) row["id"] = hit.item_id;
            results.push_back(std::move(row));
        }

        ExecuteResult r;
        r.ok = true;
        r.envelope = nlohmann::json{
            {"ok", true},
            {"count", results.size()},
            {"items", std::move(results)},
            {"ranking", "bm25_dense_rrf"},
            {"ranking_telemetry", telemetry.to_json()},
            {"message", hits.empty() ? "No matching memory items found."
                                     : std::to_string(hits.size()) + " ranked item(s) found."},
        };
        if (include_body && !hits.empty()) {
            const auto& top = hits.front().item;
            const nlohmann::json metadata =
                top.value("metadata", nlohmann::json::object());
            const nlohmann::json source_log =
                metadata.value("source_log", nlohmann::json::object());
            if (top.value("role", std::string{}) != "turn_pair" ||
                !metadata.value("body_available", false) || !source_log.is_object()) {
                r.envelope["body_found"] = false;
                r.envelope["message"] =
                    "Ranked items found, but the top item has no retrievable assistant body.";
                return r;
            }

            media::llm::chat_sessions::AgentFinalTextSlice slice;
            bool body_found = false;
            if (!media::llm::chat_sessions::load_agent_final_text(
                    source_log.value("session_id", std::string{}),
                    source_log.value("log_id", std::string{}),
                    source_log.value("path", std::string{}),
                    body_offset, max_body_bytes, slice, body_found, err)) {
                return simple_error("memory_find: " + err);
            }
            r.envelope["body_found"] = body_found;
            if (!body_found) {
                r.envelope["message"] =
                    "The top-ranked item's canonical agent log is missing or was deleted.";
                return r;
            }
            r.envelope["materialized_item_id"] = hits.front().item_id;
            r.envelope["materialized_session_id"] = hits.front().session_id;
            r.envelope["final_text"] = std::move(slice.text);
            r.envelope["total_bytes"] = slice.total_bytes;
            r.envelope["offset"] = slice.offset;
            r.envelope["next_offset"] = slice.offset +
                r.envelope["final_text"].get_ref<const std::string&>().size();
            r.envelope["truncated"] = slice.truncated;
            r.envelope["message"] = slice.truncated
                ? "Top-ranked assistant body materialized; request next_offset for remaining bytes."
                : "Top-ranked assistant body materialized.";
        }
        if (include_payload && !hits.empty()) {
            r.envelope["payload_found"] = false;
            if (current_session_id.empty() ||
                hits.front().session_id != current_session_id) {
                r.envelope["message"] =
                    "Exact tool payload retrieval is limited to the current session.";
                return r;
            }
            const auto& top = hits.front().item;
            const nlohmann::json metadata =
                top.value("metadata", nlohmann::json::object());
            const bool is_turn_pair =
                top.value("role", std::string{}) == "turn_pair";
            const bool is_tool_observation =
                top.value("kind", std::string{}) == "tool_observation" &&
                metadata.value("payload_available", false);
            const bool is_tool_checkpoint =
                top.value("kind", std::string{}) == "tool_checkpoint" &&
                metadata.value("payload_available", false);
            std::string resolved_call_id = tool_call_id;
            if (is_tool_observation || is_tool_checkpoint)
                resolved_call_id =
                    metadata.value("transcript_tool_call_id", std::string{});
            if (resolved_call_id.empty()) {
                r.envelope["message"] =
                    "toolCallId is required when includePayload=true.";
                return r;
            }
            const nlohmann::json source_log =
                metadata.value("source_log", nlohmann::json::object());
            if ((!is_turn_pair && !is_tool_observation && !is_tool_checkpoint) ||
                !source_log.is_object()) {
                r.envelope["message"] =
                    "The top-ranked item has no canonical tool-result locator.";
                return r;
            }
            media::llm::chat_sessions::AgentToolResultSlice slice;
            bool payload_found = false;
            if (!media::llm::chat_sessions::load_agent_tool_result(
                    source_log.value("session_id", std::string{}),
                    source_log.value("log_id", std::string{}),
                    source_log.value("path", std::string{}),
                    resolved_call_id, body_offset, max_body_bytes,
                    slice, payload_found, err)) {
                return simple_error("memory_find: " + err);
            }
            if (payload_found && tool_payload_retrieval_denied(slice.tool_name)) {
                r.envelope["payload_denied"] = true;
                r.envelope["message"] =
                    "Retrieval of this sensitive tool payload is denied.";
                return r;
            }
            r.envelope["payload_found"] = payload_found;
            if (!payload_found) {
                r.envelope["message"] =
                    "The canonical tool result is missing or the call ID was not found.";
                return r;
            }
            r.envelope["materialized_item_id"] = hits.front().item_id;
            r.envelope["materialized_session_id"] = hits.front().session_id;
            r.envelope["tool_name"] = slice.tool_name;
            r.envelope["tool_call_id"] = resolved_call_id;
            r.envelope["payload_text"] = std::move(slice.text);
            r.envelope["total_bytes"] = slice.total_bytes;
            r.envelope["offset"] = slice.offset;
            r.envelope["next_offset"] = slice.offset +
                r.envelope["payload_text"].get_ref<const std::string&>().size();
            r.envelope["truncated"] = slice.truncated;
            r.envelope["message"] = slice.truncated
                ? "Top-ranked tool payload materialized; request next_offset for remaining bytes."
                : "Top-ranked tool payload materialized.";
        }
        return r;
    }

    // Always search default long-term memory first.
    {
        const nlohmann::json state = default_memory_doc_read(err);
        if (!err.empty())
            return simple_error("memory_find: " + err);
        const nlohmann::json items_meta = default_memory_doc_read_items(err);
        if (!err.empty())
            return simple_error("memory_find: " + err);

        if (items_meta.is_object() && !items_meta.empty()) {
            for (auto it = items_meta.begin(); it != items_meta.end(); ++it) {
                if (!it.value().is_object()) continue;
                nlohmann::json row{
                    {"kind", "default_memory_item"},
                    {"sessionId", "default_memory.doc"},
                    {"key", it.key()},
                    {"value", it.value().value("value", nlohmann::json{})},
                    {"tags", it.value().value("tags", nlohmann::json::array())},
                    {"updated_at", it.value().value("updated_at", std::string{})},
                };
                const bool matches = [&]() {
                    if (query.empty()) return true;
                    std::string hay = row.dump();
                    std::string needle = query;
                    hay = media::strutil::lower_ascii(std::move(hay));
                    needle = media::strutil::lower_ascii(std::move(needle));
                    return hay.find(needle) != std::string::npos;
                }();
                if (matches) results.push_back(std::move(row));
            }
        } else if (state.is_object()) {
            const bool matches = [&]() {
                if (query.empty()) return !state.empty();
                std::string hay = state.dump();
                std::string needle = query;
                hay = media::strutil::lower_ascii(std::move(hay));
                needle = media::strutil::lower_ascii(std::move(needle));
                return hay.find(needle) != std::string::npos;
            }();
            if (matches) {
                results.push_back(nlohmann::json{
                    {"kind", "default_memory"},
                    {"sessionId", "default_memory.doc"},
                    {"state", state},
                });
            }
        }
    }

    if (search_past_chats) {
        const nlohmann::json store = media::llm::chat_sessions::query_store(err);
        if (!err.empty())
            return simple_error("memory_find: " + err);

        const auto matches_query = [&](const nlohmann::json& row) {
            if (query.empty()) return true;
            std::string hay = row.dump();
            std::string needle = query;
            hay = media::strutil::lower_ascii(std::move(hay));
            needle = media::strutil::lower_ascii(std::move(needle));
            return hay.find(needle) != std::string::npos;
        };

        std::vector<std::string> excluded_ids;
        if (!include_artifacts && store.contains("artifacts") && store["artifacts"].is_array()) {
            for (const auto& row : store["artifacts"]) {
                if (row.is_object() && row.contains("id") && row["id"].is_string())
                    excluded_ids.push_back(row["id"].get<std::string>());
            }
        }
        if (!include_security && store.contains("securityItems") && store["securityItems"].is_array()) {
            for (const auto& row : store["securityItems"]) {
                if (row.is_object() && row.contains("id") && row["id"].is_string())
                    excluded_ids.push_back(row["id"].get<std::string>());
            }
        }

        if (store.contains("memoryItems") && store["memoryItems"].is_array()) {
            for (const auto& row : store["memoryItems"]) {
                if (!row.is_object()) continue;
                if (row.contains("id") && row["id"].is_string()) {
                    const auto idv = row["id"].get<std::string>();
                    if (std::find(excluded_ids.begin(), excluded_ids.end(), idv) != excluded_ids.end())
                        continue;
                }
                if (matches_query(row))
                    results.push_back(row);
            }
        }
    }

    if (max_results > 0 && static_cast<int>(results.size()) > max_results) {
        nlohmann::json capped = nlohmann::json::array();
        for (int i = 0; i < max_results; ++i)
            capped.push_back(results[static_cast<std::size_t>(i)]);
        results = std::move(capped);
    }

    ExecuteResult r;
    r.ok = true;
    r.envelope = nlohmann::json{
        {"ok",      true},
        {"count",   results.size()},
        {"items",   results},
        {"message", results.empty() ? "No matching memory items found."
                                    : std::to_string(results.size()) + " item(s) found."},
    };
    return r;
}

// ── Catalog schemas ───────────────────────────────────────────────────────

ToolDef make_memory_read() {
    return ToolDef{
        "memory_read",
        "Read persistent long-term memory and per-item metadata.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"task_id", {{"type", "string"}, {"description", "Optional: read a scheduled task's memory instead."}}},
            }},
        }};
}

ToolDef make_memory_write() {
    return ToolDef{
        "memory_write",
        "Merge a JSON object into long-term memory. Use to store facts, preferences, or context "
        "across turns. null values delete keys; {} clears all memory. Keep values small.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"state", {{"type", "object"}, {"description", "Partial object to merge. null deletes a key; {} clears all."}}},
                {"tags", {{"type", "array"},
                          {"items", {{"type", "string"}}},
                          {"description", "Optional tags applied to every key in this write."}}},
            }},
            {"required", nlohmann::json::array({"state"})},
        }};
}

ToolDef make_memory_append_event() {
    return ToolDef{
        "memory_append_event",
        "Append a structured event to the session event log.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"event", {{"type", "object"}, {"description", "JSON object. Include at least a 'type' string."}}},
            }},
            {"required", nlohmann::json::array({"event"})},
        }};
}

ToolDef make_memory_find() {
    return ToolDef{
        "memory_find",
        "Rank session memory facts or items with BM25 plus available dense recall. "
        "Set searchPastChats to also search persisted past sessions. To recover an "
        "unabridged prior assistant reply, pass its exact turn itemId with includeBody=true. "
        "To recover one same-session canonical tool result, pass includePayload=true with "
        "a tool-checkpoint itemId, or provide its toolCallId.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"query", {
                    {"type", "string"},
                    {"description", "Natural-language ranked query. Empty = list items without ranking."},
                }},
                {"itemId", {
                    {"type", "string"},
                    {"description", "Exact session-item ID (for example turn_ab12). Bypasses fuzzy search."},
                }},
                {"sessionId", {
                    {"type", "string"},
                    {"description", "Optional exact owning session ID used to disambiguate itemId."},
                }},
                {"includeBody", {
                    {"type", "boolean"},
                    {"description", "Materialize canonical assistant final_text for an exact itemId, or for the top-ranked query result."},
                    {"default", false},
                }},
                {"includePayload", {
                    {"type", "boolean"},
                    {"description", "Materialize one canonical same-session tool_result envelope. A tool-checkpoint itemId supplies its stored call ID; other items require toolCallId."},
                    {"default", false},
                }},
                {"toolCallId", {
                    {"type", "string"},
                    {"description", "Exact transcript tool-call ID exposed in turn metadata."},
                }},
                {"maxBytes", {
                    {"type", "integer"},
                    {"minimum", 1},
                    {"maximum", 262144},
                    {"description", "Maximum UTF-8 bytes of assistant body or tool payload to return. Default 65536."},
                }},
                {"offset", {
                    {"type", "integer"},
                    {"minimum", 0},
                    {"description", "UTF-8 byte offset for the body or payload slice. Use next_offset to continue."},
                }},
                {"searchPastChats", {
                    {"type", "boolean"},
                    {"description", "Also search all persisted past sessions."},
                    {"default", false},
                }},
                {"includeArtifacts", {
                    {"type", "boolean"},
                    {"description", "Include artifact items. Default false."},
                    {"default", false},
                }},
                {"includeSecurity", {
                    {"type", "boolean"},
                    {"description", "Include security-review items. Default false."},
                    {"default", false},
                }},
                {"maxResults", {
                    {"type", "integer"},
                    {"minimum", 1},
                    {"maximum", 200},
                    {"description", "Max items to return. Default 30."},
                }},
            }},
        }};
}

bool load_global_memory_document(nlohmann::json& out_document)
{
    bool found = false;
    std::string err;
    if (!load_default_memory_document(out_document, found, err))
        return false;
    if (!found || !out_document.is_object())
        out_document = nlohmann::json::object();
    return true;
}

} // namespace media::llm::path::detail

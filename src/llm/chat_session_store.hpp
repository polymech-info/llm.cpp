#pragma once

#include "security/security_defaults.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace media::llm::chat_sessions {

enum class StoreBackend {
    LegacyFiles,
    SecureStorage,
};

enum class ProtectionMode {
    Plain,
    Signed,
    Encrypted,
    EncryptedWithUserPresence,
};

struct DeleteResult {
    bool removed = false;
    bool agent_session_removed = false;
    int agent_logs_removed = 0;
    std::string agent_task_id;
};

struct StoreOptions {
    StoreBackend backend = StoreBackend::SecureStorage;
    ProtectionMode protection = ProtectionMode::Signed;
    std::filesystem::path root_override;
    int max_sessions = pm::security::k_session_storage_max_sessions_default;
};

struct PruneOptions {
    int keep_latest = 0;
};

struct PruneResult {
    int sessions_removed = 0;
    int documents_removed = 0;
};

StoreOptions active_options();
bool using_secure_storage();
std::filesystem::path store_dir();
std::filesystem::path session_path(const std::string& id);

nlohmann::json meta_row(const nlohmann::json& session);
std::vector<nlohmann::json> list_meta(std::string& err);
bool load(const std::string& id, nlohmann::json& out_session, std::string& err);
bool save(const nlohmann::json& session, std::string& err);
bool merge_host_fields(const std::string& id, const nlohmann::json& patch, std::string& err);
std::string lookup_agent_task_id(const std::string& chat_session_id);
bool store_agent_task_id(const std::string& chat_session_id, const std::string& agent_task_id, std::string& err);
bool store_agent_log_path(const std::string& chat_session_id, const std::string& agent_log_path, std::string& err);
DeleteResult remove(const std::string& id, std::string& err);

bool load_session_memory_document(const std::string& session_id,
                                  nlohmann::json& out_document,
                                  bool& found,
                                  std::string& err);
bool save_session_memory_document(const std::string& session_id,
                                  const nlohmann::json& document,
                                  std::string& err);
bool delete_session_memory_document(const std::string& session_id, std::string& err);
bool load_session_log_document(const std::string& session_id,
                               const std::string& log_id,
                               nlohmann::json& out_document,
                               bool& found,
                               std::string& err);
bool save_session_log_document(const std::string& session_id,
                               const std::string& log_id,
                               const nlohmann::json& document,
                               std::string& err);
bool delete_session_log_document(const std::string& session_id,
                                 const std::string& log_id,
                                 std::string& err);

struct AgentFinalTextSlice {
    std::string text;
    std::size_t total_bytes = 0;
    std::size_t offset = 0;
    bool truncated = false;
};

struct AgentToolResultSlice {
    std::string tool_name;
    std::string text;
    std::size_t total_bytes = 0;
    std::size_t offset = 0;
    bool truncated = false;
};

/// Resolve a trusted turn-pair log locator through secure session storage,
/// falling back to its runner-written JSON path, then return a bounded slice
/// of result.final_text. Missing/deleted logs report found=false without error.
bool load_agent_final_text(const std::string& session_id,
                           const std::string& log_id,
                           const std::string& fallback_path,
                           std::size_t offset,
                           std::size_t max_bytes,
                           AgentFinalTextSlice& out,
                           bool& found,
                           std::string& err);

/// Resolve one canonical tool_result event by transcript tool-call id and
/// return a UTF-8-safe bounded slice of its serialized envelope.
bool load_agent_tool_result(const std::string& session_id,
                            const std::string& log_id,
                            const std::string& fallback_path,
                            const std::string& tool_call_id,
                            std::size_t offset,
                            std::size_t max_bytes,
                            AgentToolResultSlice& out,
                            bool& found,
                            std::string& err);

/// Seed a visible chat transcript from a headless/assistant realtime agent
/// session, and copy the backend agent memory JSON into the default session
/// store for continuity after a UI handoff.
bool seed_from_agent_session(const std::string& session_id,
                             const std::string& source_session_dir,
                             std::string& err);

/// Low-level enumeration surface for future sidecars/embedding harvesters.
/// Returns session metadata plus first-class memory/artifact/security sidecar
/// entries without requiring callers to know which storage backend is active.
nlohmann::json query_store(std::string& err);
PruneResult prune(const PruneOptions& options, std::string& err);

/// Collect filesystem artifact paths from recent chat sessions (file/image entries
/// and resultPaths), matching the viewer home-widget past-sessions behavior.
std::vector<std::string> recent_session_artifact_paths(int max_sessions,
                                                       int max_paths,
                                                       std::string& err);

} // namespace media::llm::chat_sessions

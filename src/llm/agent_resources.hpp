#pragma once

#include "agent_skills.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace media::llm::resources {

enum class ResourceKind { Skill, Text };

struct RetrievalResource {
    std::string uri;
    std::string qualified_name;
    ResourceKind kind = ResourceKind::Text;
    std::string scope;
    std::string title;
    std::string description;
    std::string search_text;
    std::filesystem::path body_path;
    std::filesystem::path base_path;
    std::string vfs_mount;
    std::string vfs_path;
    std::string vfs_server_url;
    bool vfs_public_only = false;
    std::string media_type = "text/plain";
    std::string content_hash;
    std::string trust;
    std::vector<std::string> declared_tools;
    std::vector<std::string> missing_requirements;
    bool available = true;
    bool disabled = false;
    bool pinned = false;
    bool always = false;
};

struct ResourceSnapshot {
    std::vector<RetrievalResource> entries;
    skills::SkillSnapshot skills;
};

std::string canonical_uri(ResourceKind kind, std::string_view scope, std::string_view id);
bool is_canonical_resource_uri(std::string_view uri, ResourceKind kind);

ResourceSnapshot discover(
    const skills::SkillPolicy& policy,
    const std::string& cwd_hint,
    const std::string& embed_model_hint = {});

/// Upsert skill/resource rows into the local vector store. Not on the first-token
/// path — `discover()` lists only. Call from info_lookup / ranking when needed.
void index_resources(
    const std::vector<RetrievalResource>& entries,
    const std::string& embed_model_hint = {});

std::vector<std::size_t> rank(
    std::string_view query,
    const std::vector<RetrievalResource>& resources,
    std::size_t top_k,
    const std::string& embed_model_hint = {});

std::optional<std::size_t> resolve(
    std::string_view uri_or_name,
    const std::vector<RetrievalResource>& resources);

nlohmann::json metadata_json(const RetrievalResource& resource);

/// Materialize one trusted/available resource. Relative links stay on demand.
nlohmann::json materialize(
    const RetrievalResource& resource,
    std::size_t byte_limit,
    int traversal_depth,
    const std::string& session_id);

} // namespace media::llm::resources

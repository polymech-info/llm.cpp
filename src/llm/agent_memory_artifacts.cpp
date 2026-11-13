#include "agent_memory_artifacts.hpp"
#include "json/json_utils.hpp"

#include "agent_memory.hpp"
#include "agent_internal.hpp"
#include "string/string_sanitizers.hpp"
#include "fs/file_extensions.hpp"
#include "llm_constants.hpp"
#include "logging/logger.h"
#include "media/probe.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

namespace fs = std::filesystem;

namespace media::llm::agent {
namespace {

std::string json_str(const nlohmann::json& j, const char* key)
{
    if (!j.is_object() || !j.contains(key) || !j[key].is_string())
        return {};
    return j[key].get<std::string>();
}

enum class ArtifactKind {
    File,
    WebPage,
    Url,
    JsonData,
    Table,
    Image,
    Video,
    Audio,
    Text,
    ToolObservation,
    ExternalHandle,
    Directory,
    Conversation,
    Unknown
};

enum class ArtifactSource {
    WrittenByTool,
    ReturnedByTool,
    Discovered,
    UserSelected,
    UserMentioned
};

enum class ArtifactRole {
    DirectoryListing,
    SearchResults,
    Report,
    GeneratedImage,    // produced by image_create / image_transform (raster)
    ResizedImage,      // produced by image_resize (raster)
    RasterImage,       // raster bitmap (jpeg/png/webp/...), origin agnostic
    VectorImage,       // SVG and similar text-based vector formats
    CadDrawing,        // .dxf / .dwg / .ai
    Model3d,           // .scad / .stl / .obj / .glb / .gltf / .ply
    SourceCode,        // .cpp / .py / .js / .ts / .rs / .go / ...
    GeneratedVideo,    // produced by create_video
    Screenshot,        // produced by app_screenshot / image_from_camera
    JsonData,
    MarkdownDocument,
    TextDocument,
    Unknown
};

std::string to_string(ArtifactKind kind)
{
    switch (kind) {
    case ArtifactKind::File:            return "file";
    case ArtifactKind::WebPage:         return "web_page";
    case ArtifactKind::Url:             return "url";
    case ArtifactKind::JsonData:        return "json_data";
    case ArtifactKind::Table:           return "table";
    case ArtifactKind::Image:           return "image";
    case ArtifactKind::Video:           return "video";
    case ArtifactKind::Audio:           return "audio";
    case ArtifactKind::Text:            return "text";
    case ArtifactKind::ToolObservation: return "tool_observation";
    case ArtifactKind::ExternalHandle:  return "external_handle";
    case ArtifactKind::Directory:       return "directory";
    case ArtifactKind::Conversation:    return "conversation";
    case ArtifactKind::Unknown:         return "unknown";
    }
    return "unknown";
}

std::string to_string(ArtifactSource source)
{
    switch (source) {
    case ArtifactSource::WrittenByTool:  return "written_by_tool";
    case ArtifactSource::ReturnedByTool: return "returned_by_tool";
    case ArtifactSource::Discovered:     return "discovered";
    case ArtifactSource::UserSelected:   return "user_selected";
    case ArtifactSource::UserMentioned:  return "user_mentioned";
    }
    return "unknown";
}

std::string to_string(ArtifactRole role)
{
    switch (role) {
    case ArtifactRole::DirectoryListing: return "directory_listing";
    case ArtifactRole::SearchResults:    return "search_results";
    case ArtifactRole::Report:           return "report";
    case ArtifactRole::GeneratedImage:   return "generated_image";
    case ArtifactRole::ResizedImage:     return "resized_image";
    case ArtifactRole::RasterImage:      return "raster_image";
    case ArtifactRole::VectorImage:      return "vector_image";
    case ArtifactRole::CadDrawing:       return "cad_drawing";
    case ArtifactRole::Model3d:          return "model_3d";
    case ArtifactRole::SourceCode:       return "source_code";
    case ArtifactRole::GeneratedVideo:   return "generated_video";
    case ArtifactRole::Screenshot:       return "screenshot";
    case ArtifactRole::JsonData:         return "json_data";
    case ArtifactRole::MarkdownDocument: return "markdown_document";
    case ArtifactRole::TextDocument:     return "text_document";
    case ArtifactRole::Unknown:          return "unknown";
    }
    return "unknown";
}

using media::strutil::lower_ascii;

std::string lowercase_extension(std::string_view path)
{
    std::string ext = media::path::path_to_utf8(media::path::path_from_utf8(std::string(path)).extension());
    ext = media::strutil::lower_ascii(std::move(ext));
    return ext;
}

std::string_view trim_left_view(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    return s;
}

bool starts_with_sv(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool contains_sv(std::string_view s, std::string_view needle)
{
    return s.find(needle) != std::string_view::npos;
}

bool parse_mcp_tool_name(std::string_view name, std::string& server, std::string& tool)
{
    if (!starts_with_sv(name, "mcp_"))
        return false;
    std::string_view rest = name.substr(4);
    const std::size_t sep = rest.find("__");
    if (sep == std::string_view::npos || sep == 0 || sep + 2 >= rest.size())
        return false;
    server = std::string(rest.substr(0, sep));
    tool = std::string(rest.substr(sep + 2));
    return true;
}

bool tool_payload_retrieval_allowed(std::string_view name)
{
    const std::string lower = lower_ascii(std::string(name));
    static constexpr std::string_view denied[] = {
        "credential", "consent", "security_review", "security_decision",
        "oauth", "api_key", "private_key", "secret_store", "auth_token",
    };
    for (const auto needle : denied) {
        if (contains_sv(lower, needle))
            return false;
    }
    return true;
}

std::string args_hash(const nlohmann::json& args)
{
    return std::to_string(std::hash<std::string>{}(args.dump()));
}

ArtifactKind kind_from_content_type(std::string_view content_type)
{
    if (content_type == "application/json") return ArtifactKind::JsonData;
    if (content_type == "text/markdown") return ArtifactKind::Text;
    if (starts_with_sv(content_type, "image/")) return ArtifactKind::Image;
    if (starts_with_sv(content_type, "video/")) return ArtifactKind::Video;
    if (starts_with_sv(content_type, "audio/")) return ArtifactKind::Audio;
    if (starts_with_sv(content_type, "text/")) return ArtifactKind::Text;
    return ArtifactKind::Unknown;
}

ArtifactKind kind_from_extension(std::string_view path)
{
    static const std::unordered_map<std::string_view, ArtifactKind> by_ext = {
        {".json", ArtifactKind::JsonData},
        {".md", ArtifactKind::Text},
        {".markdown", ArtifactKind::Text},
        {".txt", ArtifactKind::Text},
        {".csv", ArtifactKind::Text},
        {".log", ArtifactKind::Text},
        {".jpg", ArtifactKind::Image},
        {".jpeg", ArtifactKind::Image},
        {".png", ArtifactKind::Image},
        {".webp", ArtifactKind::Image},
        {".gif", ArtifactKind::Image},
        {".bmp", ArtifactKind::Image},
        {".tif", ArtifactKind::Image},
        {".tiff", ArtifactKind::Image},
        {".avif", ArtifactKind::Image},
        {".heic", ArtifactKind::Image},
        {".mp4", ArtifactKind::Video},
        {".mov", ArtifactKind::Video},
        {".webm", ArtifactKind::Video},
        {".avi", ArtifactKind::Video},
        {".mkv", ArtifactKind::Video},
        {".mp3", ArtifactKind::Audio},
        {".wav", ArtifactKind::Audio},
        {".ogg", ArtifactKind::Audio},
        {".flac", ArtifactKind::Audio},
    };
    const std::string ext = lowercase_extension(path);
    auto it = by_ext.find(std::string_view(ext));
    return it != by_ext.end() ? it->second : ArtifactKind::Unknown;
}

std::string infer_content_type(std::string_view path, std::optional<std::string_view> content_sample)
{
    if (content_sample) {
        const std::string_view s = trim_left_view(*content_sample);
        if (starts_with_sv(s, "{") || starts_with_sv(s, "["))
            return "application/json";
        if (starts_with_sv(s, "# ") || contains_sv(s, "\n# "))
            return "text/markdown";
        const ::media::probe::ProbeResult probed = ::media::probe::probe_buffer(*content_sample, path);
        if (probed.ok && !probed.content_type.empty())
            return lower_ascii(probed.content_type);
    }
    const std::string ext = lowercase_extension(path);
    if (ext == ".json") return "application/json";
    if (ext == ".md" || ext == ".markdown") return "text/markdown";
    if (ext == ".txt" || ext == ".csv" || ext == ".log") return "text/plain";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".dxf") return "application/dxf";
    if (ext == ".scad") return "text/x-scad";
    if (ext == ".stl") return "model/stl";
    if (ext == ".obj") return "model/obj";
    if (ext == ".glb" || ext == ".gltf") return "model/gltf-binary";
    if (ext == ".ply") return "model/ply";
    // Final fallback: probe the file on disk by content. Useful for binary
    // outputs (image_create, screenshot, video) where we have no content
    // sample at hand. probe_file is a no-op when the path doesn't exist.
    if (!path.empty()) {
        std::error_code ec;
        const fs::path fsp(std::string{path});
        if (fs::is_regular_file(fsp, ec) && !ec) {
            const ::media::probe::ProbeResult probed = ::media::probe::probe_file(fsp);
            if (probed.ok && !probed.content_type.empty())
                return lower_ascii(probed.content_type);
        }
    }
    return {};
}

ArtifactKind infer_artifact_kind(std::string_view path,
                                 std::optional<std::string_view> mime,
                                 std::string_view producing_tool,
                                 std::optional<std::string_view> content_sample)
{
    if (producing_tool == "write_file" || producing_tool == "file_str_replace")
        return ArtifactKind::File;
    if (mime) {
        ArtifactKind from_mime = kind_from_content_type(*mime);
        if (from_mime != ArtifactKind::Unknown)
            return from_mime;
    }
    if (content_sample) {
        const std::string_view s = trim_left_view(*content_sample);
        if (starts_with_sv(s, "{") || starts_with_sv(s, "["))
            return ArtifactKind::JsonData;
        if (starts_with_sv(s, "# ") || contains_sv(s, "\n# "))
            return ArtifactKind::Text;
    }
    return kind_from_extension(path);
}

bool json_has_keys(const nlohmann::json& doc, std::initializer_list<std::string_view> keys)
{
    if (!doc.is_object())
        return false;
    for (std::string_view key : keys) {
        if (!doc.contains(std::string(key)))
            return false;
    }
    return true;
}

ArtifactRole infer_artifact_role(ArtifactKind kind,
                                 std::string_view content_type,
                                 const nlohmann::json* parsed_content,
                                 std::string_view path,
                                 std::string_view producing_tool)
{
    if (parsed_content && kind == ArtifactKind::File &&
        json_has_keys(*parsed_content, {"cwd", "pattern", "count", "files"}))
        return ArtifactRole::DirectoryListing;
    if (content_type == "application/json")
        return ArtifactRole::JsonData;
    if (content_type == "text/markdown")
        return ArtifactRole::MarkdownDocument;
    if (content_type == "text/plain")
        return ArtifactRole::TextDocument;

    // Producing-tool first: these always imply a specific role regardless of
    // bytes on disk.
    if (producing_tool == "image_create" || producing_tool == "image_transform")
        return ArtifactRole::GeneratedImage;
    if (producing_tool == "image_resize" || producing_tool == "image_crop")
        return ArtifactRole::ResizedImage;
    if (producing_tool == "create_video")
        return ArtifactRole::GeneratedVideo;
    if (producing_tool == "app_screenshot" || producing_tool == "image_from_camera")
        return ArtifactRole::Screenshot;

    // Content-type driven (probe / mime).
    if (content_type == "image/svg+xml")
        return ArtifactRole::VectorImage;
    if (starts_with_sv(content_type, "image/"))
        return ArtifactRole::RasterImage;
    if (starts_with_sv(content_type, "video/"))
        return ArtifactRole::GeneratedVideo;
    if (content_type == "application/dxf" || content_type == "image/vnd.dxf")
        return ArtifactRole::CadDrawing;
    if (starts_with_sv(content_type, "model/"))
        return ArtifactRole::Model3d;

    // Extension fallback for text formats libmagic returns as text/plain
    // (CAD ASCII, OpenSCAD, source code).
    const std::string ext = lowercase_extension(path);
    for (const char* e : pmui::k_cad_drawing_exts)
        if (ext == e) return ArtifactRole::CadDrawing;
    if (ext == ".svg")
        return ArtifactRole::VectorImage;
    for (const char* e : pmui::k_model_3d_exts)
        if (ext == e) return ArtifactRole::Model3d;
    for (const char* e : pmui::k_source_code_exts)
        if (ext == e) return ArtifactRole::SourceCode;

    if (kind == ArtifactKind::Image)
        return ArtifactRole::RasterImage;
    if (kind == ArtifactKind::Video)
        return ArtifactRole::GeneratedVideo;
    if (kind == ArtifactKind::Text)
        return ArtifactRole::TextDocument;
    return ArtifactRole::Unknown;
}

std::string sanitize_artifact_id(std::string s)
{
    std::string out;
    out.reserve(s.size());
    bool last_us = false;
    for (unsigned char c : s) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
            last_us = false;
        } else if (!last_us && !out.empty()) {
            out.push_back('_');
            last_us = true;
        }
    }
    while (!out.empty() && out.back() == '_')
        out.pop_back();
    if (out.empty()) out = "artifact";
    if (out.size() > 64) out.resize(64);
    return out;
}

std::string infer_artifact_id(const std::string& path, ArtifactRole role, ArtifactKind kind)
{
    const fs::path p(path);
    std::string id = sanitize_artifact_id(media::path::path_to_utf8(p.stem()));

    // Extension is the strongest disambiguator: two files with the same stem
    // but different formats (modern_table.scad vs modern_table.dxf) must yield
    // distinct ids. We always append the sanitized extension when present.
    const std::string ext = lowercase_extension(path);
    if (ext.size() > 1) {
        const std::string ext_suffix = "_" + sanitize_artifact_id(ext.substr(1));
        if (id.size() < ext_suffix.size() || id.substr(id.size() - ext_suffix.size()) != ext_suffix)
            id += ext_suffix;
    } else if (role != ArtifactRole::Unknown) {
        // Extensionless paths still get a role-based suffix so that distinct
        // roles (json_data, markdown_document, ...) don't collide.
        const std::string label = to_string(role);
        if (!label.empty() && label != "unknown" && label != "file") {
            const std::string suffix = "_" + sanitize_artifact_id(label);
            if (id.size() < suffix.size() || id.substr(id.size() - suffix.size()) != suffix)
                id += suffix;
        }
    }
    (void)kind;
    return id;
}

/// Clamp a free-form summary to at most `max_chars` characters. We trim mid-
/// word with a trailing ellipsis-marker; nothing here is user-facing prose.
std::string clamp_summary(std::string text, std::size_t max_chars)
{
    if (max_chars == 0 || text.size() <= max_chars)
        return text;
    return media::str_san::utf8_safe_ellipsize(text, max_chars);
}

/// Recency timestamp for ordering: prefer `last_used_at`, fall back to
/// `created_at`. Missing / malformed → empty. Used both for newest-first
/// rendering and for LRU eviction.
std::string item_recency_at(const nlohmann::json& item)
{
    if (!item.is_object()) return {};
    if (auto it = item.find("last_used_at"); it != item.end() && it->is_string())
        return it->get<std::string>();
    if (auto it = item.find("created_at"); it != item.end() && it->is_string())
        return it->get<std::string>();
    return {};
}

std::string summarize_written_content(const std::string& content,
                                      std::string_view content_type,
                                      ArtifactRole role,
                                      const std::string& path,
                                      nlohmann::json& metadata)
{
    metadata = nlohmann::json::object();
    metadata["content_chars"] = static_cast<int>(content.size());
    if (content_type == "application/json") {
        try {
            nlohmann::json doc = nlohmann::json::parse(content);
            if (doc.is_object()) {
                nlohmann::json keys = nlohmann::json::array();
                for (auto it = doc.begin(); it != doc.end() && keys.size() < 12; ++it)
                    keys.push_back(it.key());
                metadata["json_keys"] = keys;
                if (doc.contains("count") && doc["count"].is_number_integer())
                    metadata["count"] = doc["count"];
                if (doc.contains("pattern") && doc["pattern"].is_string())
                    metadata["source_pattern"] = doc["pattern"];
                std::string filename = media::path::path_to_utf8(media::path::path_from_utf8(path).filename());
                filename = media::strutil::lower_ascii(std::move(filename));
                if (filename == "package.json") {
                    metadata["manifest_type"] = doc.value("type", std::string{});
                    if (doc.contains("bin"))
                        metadata["manifest_bin"] = doc["bin"];
                    if (doc.contains("scripts") && doc["scripts"].is_object()) {
                        nlohmann::json scripts = nlohmann::json::object();
                        for (const char* key : {"build", "test", "start"}) {
                            if (doc["scripts"].contains(key) && doc["scripts"][key].is_string())
                                scripts[key] = doc["scripts"][key];
                        }
                        if (!scripts.empty())
                            metadata["manifest_scripts"] = std::move(scripts);
                    }
                    nlohmann::json dependencies = nlohmann::json::array();
                    if (doc.contains("dependencies") && doc["dependencies"].is_object()) {
                        for (auto it = doc["dependencies"].begin();
                             it != doc["dependencies"].end() && dependencies.size() < 12; ++it)
                            dependencies.push_back(it.key());
                    }
                    if (!dependencies.empty())
                        metadata["manifest_dependencies"] = dependencies;
                    std::string summary = "Node package manifest";
                    if (!metadata.value("manifest_type", std::string{}).empty())
                        summary += " type=" + metadata["manifest_type"].get<std::string>();
                    if (metadata.contains("manifest_scripts"))
                        summary += " scripts=" + metadata["manifest_scripts"].dump();
                    if (!dependencies.empty())
                        summary += " dependencies=" + dependencies.dump();
                    return summary;
                }
                if (role == ArtifactRole::DirectoryListing)
                    return "JSON directory listing with " + std::to_string(doc.value("count", 0)) + " item(s)";
                return "JSON object with keys: " + keys.dump();
            }
            if (doc.is_array()) {
                metadata["count"] = static_cast<int>(doc.size());
                return "JSON array with " + std::to_string(doc.size()) + " item(s)";
            }
        } catch (...) {
            return "JSON text";
        }
    }
    if (content_type == "text/markdown")
        return "Markdown document";
    if (content_type == "text/plain")
        return "Text document";
    return "Written file";
}

/// Summary for binary / path-only outputs (image_create, screenshot, video, …)
/// where we have no content buffer to inspect. Tool name drives the verb;
/// content_type may add detail (e.g. "image/jpeg").
std::string summarize_path_output(std::string_view tool,
                                  std::string_view content_type)
{
    std::string verb;
    if (tool == "image_create")      verb = "Generated image";
    else if (tool == "image_transform") verb = "Transformed image";
    else if (tool == "image_resize")    verb = "Resized image";
    else if (tool == "image_crop")      verb = "Cropped image";
    else if (tool == "create_video")    verb = "Generated video";
    else if (tool == "app_screenshot")  verb = "Screenshot";
    else if (tool == "image_from_camera") verb = "Captured frame";
    else if (tool == "file_str_replace")  verb = "Edited file";
    else if (tool == "file_compose")      verb = "Composed file";
    else if (tool == "download_url")      verb = "Downloaded file";
    else                                  verb = "Written file";
    if (!content_type.empty())
        verb += " (" + std::string(content_type) + ")";
    return verb;
}

/// Tools whose successful results produce a path-based artifact we want to
/// remember in session memory. write_file is the textual case; the rest are
/// binary outputs (image / video / screenshot) where we capture the path,
/// probe the MIME, and use the tool name to drive role and summary.
bool tool_produces_path_artifact(std::string_view name)
{
    return name == "write_file"
        || name == "file_compose"
        || name == "download_url"
        || name == "file_str_replace"
        || name == "image_create"
        || name == "image_transform"
        || name == "image_resize"
        || name == "image_crop"
        || name == "create_video"
        || name == "image_from_camera"
        || name == "app_screenshot";
}

/// Session trace for a finished search_index. Items only: the index id and
/// the source paths stay available on later turns. The vector store itself
/// is temporary and is not a user-visible file like a generated picture.
nlohmann::json search_index_session_item(const nlohmann::json& args,
                                         const nlohmann::json& env,
                                         const std::string& now)
{
    if (!env.is_object() || !env.value("ok", false))
        return nlohmann::json();
    const std::string index_id = json_str(env, "index");
    if (index_id.empty())
        return nlohmann::json();

    nlohmann::json paths = nlohmann::json::array();
    auto push_path = [&](const nlohmann::json& p) {
        if (!p.is_string() || paths.size() >= 8) return;
        const std::string s = p.get<std::string>();
        if (!s.empty()) paths.push_back(s);
    };
    if (args.contains("path")) push_path(args["path"]);
    if (args.contains("paths") && args["paths"].is_array()) {
        for (const auto& p : args["paths"]) push_path(p);
    }

    const std::string primary = paths.empty() ? std::string{} : paths[0].get<std::string>();
    const int files = env.contains("indexed_files") && env["indexed_files"].is_number_integer()
        ? env["indexed_files"].get<int>() : 0;
    const int docs = env.contains("doc_count") && env["doc_count"].is_number_integer()
        ? env["doc_count"].get<int>() : 0;
    const std::string summary =
        "Indexed " + std::to_string(files) + " file(s) into index " + index_id
        + ". Follow up with search_query.";

    return {
        {"id", "searchidx_" + index_id},
        {"kind", to_string(ArtifactKind::ExternalHandle)},
        {"role", "search_index"},
        {"origin", {
            {"type", "tool"},
            {"tool_name", "search_index"},
            {"source", to_string(ArtifactSource::ReturnedByTool)},
        }},
        {"locator", {{"path", primary}, {"index", index_id}}},
        {"created_at", now},
        {"last_used_at", now},
        {"use_count", 1},
        {"summary", summary},
        {"metadata", {
            {"index", index_id},
            {"indexed_files", files},
            {"doc_count", docs},
            {"paths", std::move(paths)},
        }},
    };
}

std::string summarize_mcp_observation(const std::string& server,
                                      const std::string& tool,
                                      const nlohmann::json& env,
                                      bool ok)
{
    if (env.is_object()) {
        if (env.contains("summary") && env["summary"].is_string())
            return env["summary"].get<std::string>();
        if (env.contains("results") && env["results"].is_array())
            return "MCP " + server + "." + tool + " returned " + std::to_string(env["results"].size()) + " result item(s).";
        if (env.contains("content") && env["content"].is_array())
            return "MCP " + server + "." + tool + " returned " + std::to_string(env["content"].size()) + " content block(s).";
    }
    return ok
        ? "MCP " + server + "." + tool + " completed successfully."
        : "MCP " + server + "." + tool + " failed.";
}

std::string norm_path_key(const std::string& path)
{
    if (path.empty()) return {};
    std::error_code ec;
    fs::path p = fs::weakly_canonical(media::path::path_from_utf8(path), ec);
    if (ec) p = media::path::path_from_utf8(path);
    std::string out = media::path::path_to_generic_utf8(p);
    out = media::strutil::lower_ascii(std::move(out));
    return out;
}

std::unordered_set<std::string> collect_item_path_keys(const nlohmann::json& memory_state)
{
    std::unordered_set<std::string> out;
    if (!memory_state.is_object() || !memory_state.contains("items") ||
        !memory_state["items"].is_object())
        return out;
    for (const auto& [id, item] : memory_state["items"].items()) {
        (void)id;
        if (!item.is_object() || !item.contains("locator") || !item["locator"].is_object())
            continue;
        const auto& loc = item["locator"];
        if (loc.contains("path") && loc["path"].is_string()) {
            const std::string key = norm_path_key(loc["path"].get<std::string>());
            if (!key.empty()) out.insert(key);
        }
    }
    return out;
}

std::unordered_set<std::string> collect_recovered_call_ids(const nlohmann::json& memory_state)
{
    std::unordered_set<std::string> out;
    if (!memory_state.is_object() || !memory_state.contains("journal_recovery") ||
        !memory_state["journal_recovery"].is_object())
        return out;
    const auto& jr = memory_state["journal_recovery"];
    if (!jr.contains("recovered_tool_call_ids") || !jr["recovered_tool_call_ids"].is_array())
        return out;
    for (const auto& id : jr["recovered_tool_call_ids"]) {
        if (id.is_string() && !id.get<std::string>().empty())
            out.insert(id.get<std::string>());
    }
    return out;
}

bool journal_has_incomplete_turn(const nlohmann::json& journal)
{
    bool open = false;
    if (!journal.is_array()) return false;
    for (const auto& e : journal) {
        if (!e.is_object()) continue;
        const std::string kind = e.value("kind", std::string{});
        if (kind == "turn_started")  open = true;
        if (kind == "turn_completed") open = false;
    }
    return open;
}

nlohmann::json build_item_from_journal_path(const std::string& tool_name,
                                            const std::string& tool_call_id,
                                            const std::string& path,
                                            const std::string& now,
                                            bool truncate_summaries)
{
    std::error_code ec;
    if (!fs::exists(media::path::path_from_utf8(path), ec) || ec)
        return nlohmann::json();

    const std::string content_type = infer_content_type(path, std::nullopt);
    const ArtifactKind kind = infer_artifact_kind(
        path,
        content_type.empty() ? std::optional<std::string_view>{} : std::optional<std::string_view>{content_type},
        tool_name,
        std::string_view{});
    const ArtifactRole role = infer_artifact_role(
        kind, content_type, nullptr, path, tool_name);

    nlohmann::json metadata = nlohmann::json::object();
    const auto sz = fs::file_size(media::path::path_from_utf8(path), ec);
    if (!ec) metadata["bytes_written"] = static_cast<long long>(sz);
    if (!content_type.empty()) metadata["content_type"] = content_type;
    metadata["source"] = to_string(ArtifactSource::WrittenByTool);
    metadata["file_ext"] = lowercase_extension(path);
    metadata["transcript_tool_call_id"] = tool_call_id;
    metadata["recovered_from_journal"] = true;

    {
        std::error_code ec1, ec2;
        const auto size_bytes = fs::file_size(media::path::path_from_utf8(path), ec1);
        const auto mt = fs::last_write_time(media::path::path_from_utf8(path), ec2);
        if (!ec1) metadata["size_bytes"] = static_cast<long long>(size_bytes);
        if (!ec2) {
            const auto sys_t = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                mt - decltype(mt)::clock::now() + std::chrono::system_clock::now());
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                sys_t.time_since_epoch()).count();
            metadata["mtime_epoch"] = static_cast<long long>(secs);
        }
    }

    std::string summary = summarize_path_output(tool_name, content_type);
    if (truncate_summaries)
        summary = clamp_summary(std::move(summary), pm::llm::k_session_item_summary_chars);

    const std::string id = infer_artifact_id(path, role, kind);
    nlohmann::json item = {
        {"id", id},
        {"kind", to_string(kind)},
        {"role", to_string(role)},
        {"origin", {
            {"type", "tool"},
            {"tool_name", tool_name},
            {"source", to_string(ArtifactSource::WrittenByTool)},
        }},
        {"locator", {{"path", path}}},
        {"created_at", now},
        {"last_used_at", now},
        {"use_count", 1},
        {"summary", summary},
        {"metadata", metadata},
    };
    if (!content_type.empty())
        item["content_type"] = content_type;
    return item;
}

} // namespace

nlohmann::json session_persist_artifacts_from_result(const std::string& session_id,
                                                     const std::string& user_prompt,
                                                     const Result& result,
                                                     pm::llm::StructuredMemoryFeatures features,
                                                     const std::vector<std::string>& context_files,
                                                     const std::string& source_log_session_id,
                                                     const std::string& source_log_id,
                                                     const std::string& source_log_path,
                                                     const nlohmann::json& planner_meta)
{
    using SMF = pm::llm::StructuredMemoryFeature;

    if (features == 0 || session_id.empty() || !result.ok || !result.transcript.is_object() ||
        !result.transcript.contains("messages") || !result.transcript["messages"].is_array())
        return nlohmann::json::object();

    const bool capture_artifacts   = pm::llm::has_feature(features, SMF::CaptureWriteArtifacts);
    const bool capture_mcp_obs     = pm::llm::has_feature(features, SMF::CaptureMcpObservations);
    const bool synthesize_rules    = pm::llm::has_feature(features, SMF::SynthesizeRules);
    const bool dedup_on_write      = pm::llm::has_feature(features, SMF::DedupOnWrite);
    const bool truncate_summaries  = pm::llm::has_feature(features, SMF::TruncateSummaries);
    const bool evict_oldest        = pm::llm::has_feature(features, SMF::EvictOldest);
    const bool capture_turn_pairs  = pm::llm::has_feature(features, SMF::CaptureTurnPairs);
    (void)dedup_on_write; // stable ids already supersede in the merge-patch model

    std::unordered_map<std::string, nlohmann::json> tool_calls_by_id;
    for (const auto& m : result.transcript["messages"]) {
        if (!m.is_object() || m.value("role", std::string{}) != "assistant" ||
            !m.contains("tool_calls") || !m["tool_calls"].is_array())
            continue;
        for (const auto& tc : m["tool_calls"]) {
            if (!tc.is_object()) continue;
            const std::string id = json_str(tc, "id");
            if (id.empty() || !tc.contains("function") || !tc["function"].is_object()) continue;
            const auto& fn = tc["function"];
            nlohmann::json call;
            call["name"] = json_str(fn, "name");
            std::string arg_s = json_str(fn, "arguments");
            if (arg_s.empty() && fn.contains("arguments") && fn["arguments"].is_object())
                arg_s = fn["arguments"].dump();
            call["arguments"] = media::json::parse_or(arg_s, nlohmann::json::object());
            tool_calls_by_id[id] = std::move(call);
        }
    }

    nlohmann::json items_patch = nlohmann::json::object();
    nlohmann::json artifacts_patch = nlohmann::json::object();
    nlohmann::json latest_file_glob_origin = nlohmann::json::object();
    const std::string now = now_iso8601();

    // Tool names called this turn, in order (deduped) — used for the turn_pair.
    std::vector<std::string> tools_called_this_turn;
    // Ordered call ids (all, not deduped) — used to build tool_call_details.
    std::vector<std::string> ordered_call_ids;
    {
        std::unordered_set<std::string> seen_names;
        for (const auto& m : result.transcript["messages"]) {
            if (!m.is_object() || m.value("role", std::string{}) != "assistant" ||
                !m.contains("tool_calls") || !m["tool_calls"].is_array())
                continue;
            for (const auto& tc : m["tool_calls"]) {
                if (!tc.is_object() || !tc.contains("function") || !tc["function"].is_object()) continue;
                std::string name = json_str(tc["function"], "name");
                if (name.empty()) continue;
                const std::string id = json_str(tc, "id");
                if (!id.empty()) ordered_call_ids.push_back(id);
                if (!seen_names.count(name)) {
                    seen_names.insert(name);
                    tools_called_this_turn.push_back(name);
                }
            }
        }
    }

    // Compact result envelopes keyed by call id — used for tool_call_details.
    std::unordered_map<std::string, nlohmann::json> result_by_call_id;
    for (const auto& m : result.transcript["messages"]) {
        if (!m.is_object() || m.value("role", std::string{}) != "tool") continue;
        const std::string id = json_str(m, "tool_call_id");
        if (!id.empty()) result_by_call_id[id] = m.value("content", nlohmann::json{});
    }

    for (const auto& m : result.transcript["messages"]) {
        if (!m.is_object() || m.value("role", std::string{}) != "tool")
            continue;
        const std::string tcid = json_str(m, "tool_call_id");
        auto call_it = tool_calls_by_id.find(tcid);
        if (call_it == tool_calls_by_id.end())
            continue;
        const nlohmann::json& call = call_it->second;
        const nlohmann::json& args = call.value("arguments", nlohmann::json::object());
        if (!args.is_object())
            continue;

        const std::string tool_name = json_str(call, "name");
        const nlohmann::json& env = m.contains("content") ? m["content"] : nlohmann::json::object();

        std::string mcp_server;
        std::string mcp_tool;
        if (parse_mcp_tool_name(tool_name, mcp_server, mcp_tool)) {
            if (!capture_mcp_obs)
                continue;
            const bool ok = !env.is_object() || env.value("ok", true);
            const std::string hash = args_hash(args);
            const std::string id = sanitize_artifact_id("obs_" + mcp_server + "_" + mcp_tool + "_" + hash.substr(0, 10));
            std::string obs_summary = summarize_mcp_observation(mcp_server, mcp_tool, env, ok);
            if (truncate_summaries)
                obs_summary = clamp_summary(std::move(obs_summary), pm::llm::k_session_item_summary_chars);
            items_patch[id] = {
                {"id", id},
                {"kind", to_string(ArtifactKind::ToolObservation)},
                {"role", contains_sv(mcp_tool, "search") ? "search_results" : "unknown"},
                {"origin", {
                    {"type", "mcp"},
                    {"server_name", mcp_server},
                    {"tool_name", mcp_tool},
                }},
                {"status", ok ? "ok" : "failed"},
                {"args_hash", hash},
                {"created_at", now},
                {"last_used_at", now},
                {"use_count", 1},
                {"summary", std::move(obs_summary)},
                {"metadata", {
                    {"raw_chars", static_cast<int>(env.dump().size())},
                    {"transcript_tool_call_id", tcid},
                    {"payload_available",
                     tool_payload_retrieval_allowed(tool_name) &&
                     (!source_log_id.empty() || !source_log_path.empty())},
                    {"payload_denied", !tool_payload_retrieval_allowed(tool_name)},
                    {"source_log", {
                        {"session_id", source_log_session_id},
                        {"log_id", source_log_id},
                        {"path", source_log_path},
                    }},
                }},
            };
            continue;
        }

        if (tool_name == "file_glob" && env.is_object() && env.value("ok", false)) {
            latest_file_glob_origin = {
                {"tool", "file_glob"},
                {"pattern", args.value("pattern", std::string{})},
            };
            continue;
        }
        if (tool_name == "search_index") {
            nlohmann::json item = search_index_session_item(args, env, now);
            if (item.is_object() && item.contains("id") && item["id"].is_string())
                items_patch[item["id"].get<std::string>()] = std::move(item);
            continue;
        }
        if (!tool_produces_path_artifact(tool_name) || !capture_artifacts)
            continue;

        // write_file has the bytes available as the `content` argument so
        // textual artifacts get a real summary. The image / video / screenshot
        // family writes binary blobs to disk with no inline payload; for those
        // we leave content empty and let probe_file determine the MIME.
        const bool has_content_arg = (tool_name == "write_file");
        const std::string content = has_content_arg ? json_str(args, "content") : std::string{};
        if (!env.is_object() || !env.value("ok", false) || !env.contains("results") || !env["results"].is_array())
            continue;

        for (const auto& r : env["results"]) {
            if (!r.is_object() || !r.value("ok", false))
                continue;
            std::string path = json_str(r, "output_path");
            if (path.empty()) path = json_str(r, "path");
            if (path.empty()) continue;

            const std::string content_type = infer_content_type(
                path,
                has_content_arg ? std::optional<std::string_view>{std::string_view(content)}
                                : std::optional<std::string_view>{});
            const ArtifactKind kind = infer_artifact_kind(path,
                                                          content_type.empty()
                                                              ? std::optional<std::string_view>{}
                                                              : std::optional<std::string_view>{content_type},
                                                          tool_name,
                                                          std::string_view(content));
            nlohmann::json parsed_content;
            const nlohmann::json* parsed_ptr = nullptr;
            if (has_content_arg && content_type == "application/json") {
                try {
                    parsed_content = nlohmann::json::parse(content);
                    parsed_ptr = &parsed_content;
                } catch (...) {}
            }
            const ArtifactRole role = infer_artifact_role(kind, content_type, parsed_ptr, path, tool_name);
            nlohmann::json metadata;
            std::string summary;
            if (has_content_arg) {
                summary = summarize_written_content(content, content_type, role, path, metadata);
            } else {
                metadata = nlohmann::json::object();
                // Probe the file size from disk for the artifact metadata —
                // binary tools don't echo a byte count in the result envelope.
                std::error_code sec;
                const auto sz = fs::file_size(media::path::path_from_utf8(path), sec);
                if (!sec)
                    metadata["bytes_written"] = static_cast<long long>(sz);
                summary = summarize_path_output(tool_name, content_type);
            }
            if (truncate_summaries)
                summary = clamp_summary(std::move(summary), pm::llm::k_session_item_summary_chars);
            if (!content_type.empty())
                metadata["content_type"] = content_type;
            metadata["source"] = to_string(ArtifactSource::WrittenByTool);
            metadata["file_ext"] = lowercase_extension(path);
            if (r.contains("result") && r["result"].is_object()) {
                if (r["result"].contains("bytes_written"))
                    metadata["bytes_written"] = r["result"]["bytes_written"];
                if (r["result"].contains("append"))
                    metadata["append"] = r["result"]["append"];
            }

            // #10 staleness fingerprint. We snapshot the file's size + last
            // write time *right after* the writing tool returned so that a
            // later prompt-build can re-stat and tag the item as stale when
            // the user (or another process) edits / deletes it behind our
            // back. mtime is serialized as a portable seconds-since-epoch.
            {
                std::error_code ec1, ec2;
                const auto sz = fs::file_size(media::path::path_from_utf8(path), ec1);
                const auto mt = fs::last_write_time(media::path::path_from_utf8(path), ec2);
                if (!ec1) metadata["size_bytes"] = static_cast<long long>(sz);
                if (!ec2) {
                    const auto sys_t = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        mt - decltype(mt)::clock::now() + std::chrono::system_clock::now());
                    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(sys_t.time_since_epoch()).count();
                    metadata["mtime_epoch"] = static_cast<long long>(secs);
                }
            }

            nlohmann::json created_from = nlohmann::json::object();
            if (!latest_file_glob_origin.empty())
                created_from = latest_file_glob_origin;

            const std::string id = infer_artifact_id(path, role, kind);
            nlohmann::json item = {
                {"id", id},
                {"kind", to_string(kind)},
                {"role", to_string(role)},
                {"origin", {
                    {"type", "tool"},
                    {"tool_name", tool_name},
                    {"source", to_string(ArtifactSource::WrittenByTool)},
                }},
                {"locator", {{"path", path}}},
                {"created_at", now},
                {"last_used_at", now},
                {"use_count", 1},
                {"summary", summary},
                {"metadata", metadata},
            };
            if (!content_type.empty())
                item["content_type"] = content_type;
            if (role == ArtifactRole::DirectoryListing)
                item["schema_hint"] = "cwd,pattern,count,files[]";
            if (!created_from.empty())
                item["created_from"] = created_from;
            items_patch[id] = item;

            artifacts_patch[id] = {
                {"id", id},
                {"path", path},
                {"kind", content_type == "application/json" ? "json" : to_string(kind)},
                {"role", to_string(role)},
                {"schema", role == ArtifactRole::DirectoryListing ? "cwd,pattern,count,files[]" : ""},
                {"created_by_tool", tool_name},
                {"created_from", created_from},
                {"created_at", now},
                {"summary", summary},
                {"metadata", metadata},
            };
        }
    }

    // ── Conversation turn pair ───────────────────────────────────────────────
    // Bridges chat continuity once raw transcript replay is retired: stores
    // the user prompt, the final assistant reply, and which tools ran — under
    // the same caps/eviction as every other session item.
    if (capture_turn_pairs && (!user_prompt.empty() || !result.final_text.empty())) {
        const std::string short_hash = args_hash({{"u", user_prompt}, {"t", now}}).substr(0, 10);
        const std::string id = "turn_" + short_hash;
        std::string user_text = user_prompt;
        std::string asst_text = result.final_text;
        if (truncate_summaries) {
            user_text = clamp_summary(std::move(user_text), pm::llm::k_session_item_summary_chars);
            asst_text = clamp_summary(std::move(asst_text), pm::llm::k_session_item_summary_chars);
        }
        std::string compact_summary;
        if (!user_text.empty()) compact_summary += "user: " + user_text;
        if (!asst_text.empty()) {
            if (!compact_summary.empty()) compact_summary += " | ";
            compact_summary += "assistant: " + asst_text;
        }
        if (truncate_summaries)
            compact_summary = clamp_summary(std::move(compact_summary), pm::llm::k_session_item_summary_chars);

        nlohmann::json tools_json = nlohmann::json::array();
        for (const auto& t : tools_called_this_turn) tools_json.push_back(t);

        // Compact per-call detail — args + output paths for file/image ops.
        // Keep the newest eight compact locators. Four proved too small for
        // long coding turns: a few post-validation searches could displace the
        // successful build/test call that the next turn needed. These are only
        // bounded IDs/previews, never duplicated payload bodies.
        static constexpr int k_max_call_details = 8;
        static constexpr std::size_t k_prompt_preview_chars = 80;
        static constexpr std::size_t k_cmd_preview_chars    = 120;

        nlohmann::json call_details_json = nlohmann::json::array();
        const auto details_begin = ordered_call_ids.size() >
                static_cast<std::size_t>(k_max_call_details)
            ? ordered_call_ids.end() - k_max_call_details
            : ordered_call_ids.begin();
        for (auto cid_it = details_begin; cid_it != ordered_call_ids.end(); ++cid_it) {
            const auto& cid = *cid_it;
            const auto cit = tool_calls_by_id.find(cid);
            if (cit == tool_calls_by_id.end()) continue;
            const std::string& name = cit->second.value("name", std::string{});
            const nlohmann::json& args = cit->second.value("arguments", nlohmann::json::object());
            const nlohmann::json* env_ptr = nullptr;
            {
                const auto rit = result_by_call_id.find(cid);
                if (rit != result_by_call_id.end()) env_ptr = &rit->second;
            }

            nlohmann::json d;
            d["name"] = name;
            d["id"] = cid;
            if (env_ptr) {
                d["payload_chars"] = static_cast<long long>(env_ptr->dump().size());
                d["status"] = !env_ptr->is_object() || env_ptr->value("ok", true)
                    ? "ok" : "failed";
                d["payload_available"] =
                    tool_payload_retrieval_allowed(name) &&
                    (!source_log_id.empty() || !source_log_path.empty());
                if (!tool_payload_retrieval_allowed(name))
                    d["payload_denied"] = true;
            }

            // ── image_transform / image_resize / image_understand ────────────
            if (name == "image_transform" || name == "image_resize" || name == "image_crop"
                || name == "image_understand") {
                if (args.contains("paths") && args["paths"].is_array()) {
                    nlohmann::json in = nlohmann::json::array();
                    for (const auto& p : args["paths"])
                        in.push_back(media::path::path_to_utf8(media::path::path_from_utf8(p.get<std::string>()).filename()));
                    d["input_files"] = std::move(in);
                }
                if (args.contains("prompt")) {
                    std::string pr = args["prompt"].get<std::string>();
                    pr = media::str_san::utf8_safe_ellipsize(pr, k_prompt_preview_chars, "\u2026");
                    d["prompt"] = std::move(pr);
                }
                if (env_ptr && env_ptr->is_object() && env_ptr->contains("results")) {
                    nlohmann::json out = nlohmann::json::array();
                    for (const auto& r : (*env_ptr)["results"]) {
                        if (!r.is_object()) continue;
                        std::string op = r.value("output_path", std::string{});
                        if (!op.empty()) out.push_back(media::path::path_to_utf8(media::path::path_from_utf8(op).filename()));
                    }
                    if (!out.empty()) d["output_files"] = std::move(out);
                }
            }
            // ── image_create ─────────────────────────────────────────────────
            else if (name == "image_create") {
                if (args.contains("prompt")) {
                    std::string pr = args["prompt"].get<std::string>();
                    pr = media::str_san::utf8_safe_ellipsize(pr, k_prompt_preview_chars, "\u2026");
                    d["prompt"] = std::move(pr);
                }
                if (env_ptr && env_ptr->is_object() && env_ptr->contains("results")) {
                    nlohmann::json out = nlohmann::json::array();
                    for (const auto& r : (*env_ptr)["results"]) {
                        std::string op = r.value("output_path", std::string{});
                        if (!op.empty()) out.push_back(media::path::path_to_utf8(media::path::path_from_utf8(op).filename()));
                    }
                    if (!out.empty()) d["output_files"] = std::move(out);
                }
            }
            // ── write_file ───────────────────────────────────────────────────
            else if (name == "write_file") {
                if (args.contains("path"))
                    d["path"] = media::path::path_to_utf8(
                        media::path::path_from_utf8(args["path"].get<std::string>()).filename());
                if (args.contains("content")) {
                    const auto& c = args["content"];
                    d["chars"] = static_cast<int>(c.is_string() ? c.get<std::string>().size() : 0);
                }
            }
            else if (name == "file_str_replace") {
                if (args.contains("path"))
                    d["path"] = media::path::path_to_utf8(
                        media::path::path_from_utf8(args["path"].get<std::string>()).filename());
                if (args.contains("old_string") && args["old_string"].is_string())
                    d["old_chars"] = static_cast<int>(args["old_string"].get<std::string>().size());
                if (args.contains("new_string") && args["new_string"].is_string())
                    d["new_chars"] = static_cast<int>(args["new_string"].get<std::string>().size());
            }
            else if (name == "file_compose") {
                if (args.contains("output") && args["output"].is_string())
                    d["path"] = media::path::path_to_utf8(
                        media::path::path_from_utf8(args["output"].get<std::string>()).filename());
                if (args.contains("parts") && args["parts"].is_array())
                    d["parts"] = static_cast<int>(args["parts"].size());
            }
            else if (name == "file_delete") {
                if (args.contains("path"))
                    d["path"] = media::path::path_to_utf8(
                        media::path::path_from_utf8(args["path"].get<std::string>()).filename());
            }
            // ── file_read ────────────────────────────────────────────────────
            else if (name == "file_read") {
                const std::string read_path = json_str(args, "path");
                if (!read_path.empty())
                    d["path"] = read_path;
                if (args.contains("offset")) d["offset"] = args["offset"];
                if (args.contains("limit")) d["limit"] = args["limit"];
            }
            // ── file_glob ────────────────────────────────────────────────────
            else if (name == "file_glob") {
                if (args.contains("pattern")) d["pattern"] = args["pattern"];
                if (env_ptr && env_ptr->is_object() && env_ptr->contains("results")) {
                    for (const auto& r : (*env_ptr)["results"]) {
                        if (r.is_object() && r.contains("result") && r["result"].contains("count"))
                            d["count"] = r["result"]["count"];
                    }
                }
            }
            // ── run ──────────────────────────────────────────────────────────
            else if (name == "run") {
                std::string cmd = json_str(args, "command");
                if (!cmd.empty()) {
                    if (cmd.size() > k_cmd_preview_chars) {
                        // Absolute workspace paths can consume the whole prefix,
                        // hiding the discriminating command (for example
                        // `npm test`) from exact-payload selection. Preserve
                        // both ends within the same bounded prompt budget.
                        const std::size_t side = (k_cmd_preview_chars - 3) / 2;
                        cmd = cmd.substr(0, side) + "…" +
                              cmd.substr(cmd.size() - (k_cmd_preview_chars - side - 3));
                    }
                    d["command"] = std::move(cmd);
                }
            }
            // ── run_sequence ─────────────────────────────────────────────────
            else if (name == "run_sequence") {
                if (args.contains("steps") && args["steps"].is_array())
                    d["steps"] = static_cast<int>(args["steps"].size());
            }
            // ── info_lookup ──────────────────────────────────────────────────
            // Every lookup keeps query + kind. App lookups also keep the
            // resolved name + exe so "open it" does not look the app up again.
            else if (name == "info_lookup") {
                const std::string kind = json_str(args, "kind");
                if (!kind.empty())
                    d["kind"] = kind;
                std::string query = json_str(args, "query");
                if (!query.empty()) {
                    query = media::str_san::utf8_safe_ellipsize(query, k_cmd_preview_chars, "\u2026");
                    d["query"] = std::move(query);
                }
                if (kind == "app" && env_ptr && env_ptr->is_object() && env_ptr->contains("results")
                        && env_ptr->at("results").is_array()) {
                    nlohmann::json apps = nlohmann::json::array();
                    for (const auto& r : env_ptr->at("results")) {
                        if (!r.is_object()) continue;
                        nlohmann::json app;
                        if (r.contains("name")) app["name"] = r["name"];
                        if (r.contains("exe"))  app["exe"]  = r["exe"];
                        apps.push_back(std::move(app));
                        if (apps.size() >= 3) break; // top-3 sufficient for follow-up
                    }
                    if (!apps.empty()) d["apps"] = std::move(apps);
                }
            }
            else {
                // Keep only generic call identity/status/payload metadata. This
                // makes exact evidence discoverable without copying the result.
            }

            call_details_json.push_back(std::move(d));
        }

        // Explorer selection is leftover panel state, not the conversation
        // subject. Do not stamp it onto the turn_pair — the planner already
        // sees selected files live, and tagging them here made "go" latch onto
        // the wrong PNG. Files the turn actually touched live on tool_calls.
        (void)context_files;

        nlohmann::json metadata = {
            {"user", std::move(user_text)},
            {"assistant", std::move(asst_text)},
            {"assistant_chars", static_cast<long long>(result.final_text.size())},
            {"tools", std::move(tools_json)},
            {"tool_calls", std::move(call_details_json)},
            {"iterations", result.iterations},
        };
        if (!source_log_id.empty() || !source_log_path.empty()) {
            metadata["body_available"] = true;
            metadata["source_log"] = {
                {"session_id", source_log_session_id},
                {"log_id", source_log_id},
                {"path", source_log_path},
            };
        }
        if (planner_meta.is_object()) {
            nlohmann::json proposed = nlohmann::json::object();
            for (const char* field : {"intent", "action", "subject"}) {
                if (!planner_meta.contains(field) || !planner_meta[field].is_string())
                    continue;
                const std::string v = planner_meta[field].get<std::string>();
                if (!v.empty())
                    proposed[field] = v;
            }
            if (!proposed.empty())
                metadata["proposed"] = std::move(proposed);
        }

        items_patch[id] = {
            {"id", id},
            {"kind", to_string(ArtifactKind::Conversation)},
            {"role", "turn_pair"},
            {"origin", {{"type", "turn"}}},
            {"created_at", now},
            {"last_used_at", now},
            {"use_count", 1},
            {"summary", std::move(compact_summary)},
            {"metadata", std::move(metadata)},
        };
    }

    nlohmann::json rules = nlohmann::json::object();
    if (synthesize_rules && !items_patch.empty()) {
        rules["rule_use_prior_session_items"] = {
            {"id", "rule_use_prior_session_items"},
            {"text",
             "Resolve prior generated outputs from session items by role, origin, and recency. "
             "Do not ask the user for paths, filenames, or 'what content' when the most recent "
             "session item already answers the reference; read or transform it instead."},
            {"trigger", "user references a previous file, JSON, markdown, result, artifact, page, search, or observation"},
            {"action", "lookup matching session item by role, origin, locator, and recency"},
            {"scope", "session"},
            {"priority", "hard"},
            {"source_turn_id", session_id},
            {"created_at", now},
            {"active", true},
        };

        // Dedicated rule for terse follow-ups ("now as X", "save it as Y",
        // "convert to Z", "as <ext>"). Without this the model frequently
        // asks a clarifying question instead of operating on the newest
        // session item, even though the previous rule says not to.
        rules["rule_terse_followup_acts_on_latest"] = {
            {"id", "rule_terse_followup_acts_on_latest"},
            {"text",
             "When the user's prompt is a brief modifier with no explicit subject "
             "(e.g. 'now as <ext>', 'save it as <name>', 'convert to <fmt>', "
             "'as <type>', 'and now <ext>'), treat it as a transformation of the "
             "newest session item (newest first). Act with the available tools "
             "immediately — do NOT ask the user for clarification, content, "
             "source paths, or confirmation. The newest item IS the source."},
            {"trigger", "terse follow-up modifier with no explicit subject or content"},
            {"action", "transform newest session item using the requested format/target without asking"},
            {"scope", "session"},
            {"priority", "hard"},
            {"source_turn_id", session_id},
            {"created_at", now},
            {"active", true},
        };

        // Filename-with-extension in the user prompt is a strong write signal.
        // Without this, models sometimes emit the content inline (in chat)
        // instead of calling write_file, especially for terse follow-ups
        // like "now as dir.json" or "save report.md".
        rules["rule_filename_means_write"] = {
            {"id", "rule_filename_means_write"},
            {"text",
             "If the user names a target file with an extension (e.g. dir.json, "
             "report.md, notes.txt, summary.csv), produce that file on disk via a "
             "tool call. Use write_file when the content actually changes. When "
             "the content is unchanged from an existing session item (rename / "
             "duplicate / passthrough), prefer the run tool with the host's copy "
             "command (Windows: `cmd /c copy SRC DST`, POSIX: `cp SRC DST`) to "
             "avoid round-tripping the file body through the model. Do NOT emit "
             "the content inline in the chat reply as a substitute for the file. "
             "Confirm the action briefly."},
            {"trigger", "user prompt names a file (token of the form name.ext)"},
            {"action", "invoke write_file (new content) or run (passthrough copy)"},
            {"scope", "session"},
            {"priority", "hard"},
            {"source_turn_id", session_id},
            {"created_at", now},
            {"active", true},
        };

        const std::string lower_prompt = lower_ascii(user_prompt);
        if (lower_prompt.find("reply exactly") != std::string::npos ||
            lower_prompt.find("answer exactly") != std::string::npos) {
            rules["rule_exact_final_reply"] = {
                {"id", "rule_exact_final_reply"},
                {"text", "When the active goal asks for an exact final reply, output exactly that requested string after required tools succeed."},
                {"trigger", "active goal requests exact final wording"},
                {"action", "avoid extra prose in final answer"},
                {"scope", "goal"},
                {"priority", "hard"},
                {"source_turn_id", session_id},
                {"created_at", now},
                {"active", true},
            };
        }
    }

    if (items_patch.empty() && rules.empty())
        return nlohmann::json::object();

    // ── Load existing memory once (used by usage-bump and eviction) ──────────
    nlohmann::json existing_state = nlohmann::json::object();
    {
        nlohmann::json discarded_events = nlohmann::json::array();
        session_load(session_id, existing_state, discarded_events, /*max_events=*/0);
    }

    // ── Usage bump: items referenced by this turn's tool calls / final text ─
    // Cheap substring match on item.locator.{path,url}. New items written this
    // turn are skipped (they already carry use_count=1, last_used_at=now).
    //
    // Backslashes are normalised to forward slashes on both sides so that a
    // Windows locator like `C:\foo\bar.json` still matches the JSON-escaped
    // tool-arg dump `C:\\foo\\bar.json`.
    int bumped_items = 0;
    // Normalise all backslashes to forward slashes, then collapse runs of
    // slashes. Required because `nlohmann::json::dump()` escapes a single
    // backslash to `\\` in the rendered text — so a Windows locator like
    // `C:\foo\bar.json` shows up as `C:\\foo\\bar.json` inside tool-arg
    // dumps, and after slash-normalisation as `C://foo//bar.json`.
    auto normalize_slashes = [](std::string s) {
        for (char& c : s) if (c == '\\') c = '/';
        std::string out;
        out.reserve(s.size());
        bool prev_slash = false;
        for (char c : s) {
            const bool is_slash = (c == '/');
            if (is_slash && prev_slash) continue;
            out.push_back(c);
            prev_slash = is_slash;
        }
        return out;
    };
    if (existing_state.is_object() && existing_state.contains("items") &&
        existing_state["items"].is_object()) {
        std::string refs;
        refs.reserve(2048);
        for (const auto& kv : tool_calls_by_id) {
            const auto& call = kv.second;
            if (call.is_object() && call.contains("arguments"))
                refs += call["arguments"].dump();
            refs += '\n';
        }
        refs += result.final_text;
        const std::string refs_norm = normalize_slashes(std::move(refs));

        for (auto it = existing_state["items"].begin(); it != existing_state["items"].end(); ++it) {
            const std::string id = it.key();
            if (items_patch.contains(id)) continue;
            const auto& item = it.value();
            if (!item.is_object()) continue;

            std::string needle;
            if (item.contains("locator") && item["locator"].is_object()) {
                const auto& loc = item["locator"];
                if (loc.contains("path") && loc["path"].is_string() && loc["path"].get_ref<const std::string&>().size() >= 4)
                    needle = loc["path"].get<std::string>();
                else if (loc.contains("index") && loc["index"].is_string() && loc["index"].get_ref<const std::string&>().size() >= 8)
                    needle = loc["index"].get<std::string>();
                else if (loc.contains("url") && loc["url"].is_string() && loc["url"].get_ref<const std::string&>().size() >= 4)
                    needle = loc["url"].get<std::string>();
            }
            if (needle.empty()) continue;
            const std::string needle_norm = normalize_slashes(std::move(needle));
            if (refs_norm.find(needle_norm) == std::string::npos)
                continue;

            const int prev = item.value("use_count", 0);
            items_patch[id] = {
                {"last_used_at", now},
                {"use_count", prev + 1},
            };
            ++bumped_items;
        }
    }

    // ── Eviction: LRU on last_used_at (fallback created_at) ─────────────────
    int evicted_items = 0;
    int evicted_rules = 0;
    nlohmann::json evicted_item_ids = nlohmann::json::array();
    if (evict_oldest) {
        // Items: oldest-by-recency first eviction down to k_session_items_max.
        if (existing_state.is_object() && existing_state.contains("items") && existing_state["items"].is_object()) {
            std::unordered_map<std::string, std::string> after; // id → recency_at
            for (auto it = existing_state["items"].begin(); it != existing_state["items"].end(); ++it)
                after.emplace(it.key(), item_recency_at(it.value()));
            for (auto it = items_patch.begin(); it != items_patch.end(); ++it)
                after[it.key()] = item_recency_at(it.value());

            if (after.size() > pm::llm::k_session_items_max) {
                std::vector<std::pair<std::string, std::string>> ranked(after.begin(), after.end());
                std::sort(ranked.begin(), ranked.end(),
                          [](const auto& a, const auto& b) { return a.second < b.second; });
                const std::size_t drop = after.size() - pm::llm::k_session_items_max;
                for (std::size_t i = 0; i < drop && i < ranked.size(); ++i) {
                    const std::string& victim = ranked[i].first;
                    if (items_patch.contains(victim))
                        continue;
                    items_patch[victim]     = nullptr;  // merge-null = delete
                    artifacts_patch[victim] = nullptr;
                    evicted_item_ids.push_back(victim);
                    ++evicted_items;
                }
            }
        }

        // Rules: drop the oldest active rules over the cap (rules don't have
        // last_used_at; created_at fallback inside item_recency_at handles them).
        if (existing_state.is_object() && existing_state.contains("instructions") && existing_state["instructions"].is_object()) {
            std::unordered_map<std::string, std::string> after_rules;
            for (auto it = existing_state["instructions"].begin(); it != existing_state["instructions"].end(); ++it) {
                if (it.value().is_object() && it.value().value("active", true))
                    after_rules.emplace(it.key(), item_recency_at(it.value()));
            }
            for (auto it = rules.begin(); it != rules.end(); ++it)
                after_rules[it.key()] = item_recency_at(it.value());

            if (after_rules.size() > pm::llm::k_session_rules_max) {
                std::vector<std::pair<std::string, std::string>> ranked(after_rules.begin(), after_rules.end());
                std::sort(ranked.begin(), ranked.end(),
                          [](const auto& a, const auto& b) { return a.second < b.second; });
                const std::size_t drop = after_rules.size() - pm::llm::k_session_rules_max;
                for (std::size_t i = 0; i < drop && i < ranked.size(); ++i) {
                    const std::string& victim = ranked[i].first;
                    if (rules.contains(victim))
                        continue;
                    rules[victim] = nullptr;
                    ++evicted_rules;
                }
            }
        }
    }

    nlohmann::json patch;
    patch["items"] = std::move(items_patch);
    patch["artifacts"] = std::move(artifacts_patch);
    patch["instructions"] = std::move(rules);
    const int item_count = patch["items"].is_object() ? static_cast<int>(patch["items"].size()) : 0;
    const int artifact_count = patch["artifacts"].is_object() ? static_cast<int>(patch["artifacts"].size()) : 0;
    const int instruction_count = patch["instructions"].is_object() ? static_cast<int>(patch["instructions"].size()) : 0;
    const bool applied = task_store_write_memory(session_id, patch);
    return {
        {"session_id", session_id},
        {"applied", applied},
        {"item_count", item_count},
        {"artifact_count", artifact_count},
        {"instruction_count", instruction_count},
        {"bumped_items", bumped_items},
        {"evicted_items", evicted_items},
        {"evicted_item_ids", std::move(evicted_item_ids)},
        {"evicted_rules", evicted_rules},
        {"features", static_cast<std::uint32_t>(features)},
        {"patch", std::move(patch)},
    };
}

nlohmann::json session_persist_tool_checkpoints_from_result(
    const std::string& session_id,
    const Result& result,
    const std::string& source_log_session_id,
    const std::string& source_log_id,
    const std::string& source_log_path)
{
    if (session_id.empty() || !result.transcript.is_object() ||
        !result.transcript.contains("messages") ||
        !result.transcript["messages"].is_array())
        return nlohmann::json::object();

    std::unordered_map<std::string, nlohmann::json> calls;
    for (const auto& message : result.transcript["messages"]) {
        if (!message.is_object() ||
            message.value("role", std::string{}) != "assistant" ||
            !message.contains("tool_calls") ||
            !message["tool_calls"].is_array())
            continue;
        for (const auto& tc : message["tool_calls"]) {
            if (!tc.is_object() || !tc.contains("function") ||
                !tc["function"].is_object())
                continue;
            const std::string id = tc.value("id", std::string{});
            const auto& fn = tc["function"];
            if (id.empty() || fn.value("name", std::string{}) != "run")
                continue;
            nlohmann::json args = nlohmann::json::object();
            try {
                args = nlohmann::json::parse(
                    fn.value("arguments", std::string{"{}"}));
            } catch (...) {}
            calls[id] = std::move(args);
        }
    }
    if (calls.empty())
        return nlohmann::json::object();

    const std::string now = now_iso8601();
    nlohmann::json items_patch = nlohmann::json::object();
    for (const auto& message : result.transcript["messages"]) {
        if (!message.is_object() || message.value("role", std::string{}) != "tool")
            continue;
        const std::string call_id = message.value("tool_call_id", std::string{});
        auto call = calls.find(call_id);
        if (call == calls.end() || !call->second.is_object())
            continue;

        const std::string command = call->second.value("command", std::string{});
        const std::string lower = lower_ascii(command);
        std::string checkpoint;
        std::string role;
        if (lower.find("npm test") != std::string::npos ||
            lower.find("npm.cmd test") != std::string::npos ||
            lower.find("npm run test") != std::string::npos) {
            checkpoint = "test";
            role = "validation";
        } else if (lower.find("npm install") != std::string::npos ||
                   lower.find("npm.cmd install") != std::string::npos ||
                   lower.find("npm ci") != std::string::npos) {
            checkpoint = "install";
            role = "dependency_state";
        } else if (lower.find("npm run build") != std::string::npos ||
                   lower.find("npm.cmd run build") != std::string::npos ||
                   lower.find("tsc") != std::string::npos) {
            checkpoint = "build";
            role = "validation";
        } else {
            continue;
        }

        nlohmann::json env = message.value("content", nlohmann::json::object());
        if (env.is_string()) {
            try { env = nlohmann::json::parse(env.get<std::string>()); }
            catch (...) {}
        }
        const bool ok = !env.is_object() || env.value("ok", true);
        const int exit_code = env.is_object() ? env.value("exit_code", ok ? 0 : -1)
                                              : (ok ? 0 : -1);
        const std::string id = "checkpoint_run_" + checkpoint;
        std::string summary = "Last npm " + checkpoint + " checkpoint: ";
        summary += ok ? "passed" : "failed";
        if (exit_code >= 0)
            summary += " (exit " + std::to_string(exit_code) + ")";

        items_patch[id] = {
            {"id", id},
            {"kind", "tool_checkpoint"},
            {"role", role},
            {"origin", {{"type", "tool"}, {"tool_name", "run"}}},
            {"status", ok ? "ok" : "failed"},
            {"created_at", now},
            {"last_used_at", now},
            {"use_count", 1},
            {"summary", std::move(summary)},
            {"metadata", {
                {"checkpoint", checkpoint},
                {"command", clamp_summary(command, 240)},
                {"exit_code", exit_code},
                {"raw_chars", static_cast<int>(env.dump().size())},
                {"transcript_tool_call_id", call_id},
                {"payload_available",
                 !source_log_id.empty() || !source_log_path.empty()},
                {"source_log", {
                    {"session_id", source_log_session_id},
                    {"log_id", source_log_id},
                    {"path", source_log_path},
                }},
            }},
        };
    }
    if (items_patch.empty())
        return nlohmann::json::object();
    const int checkpoint_count = static_cast<int>(items_patch.size());

    nlohmann::json existing_state = nlohmann::json::object();
    {
        nlohmann::json discarded_events = nlohmann::json::array();
        session_load(session_id, existing_state, discarded_events, /*max_events=*/0);
    }

    nlohmann::json artifacts_patch = nlohmann::json::object();
    nlohmann::json evicted_item_ids = nlohmann::json::array();
    if (existing_state.contains("items") && existing_state["items"].is_object()) {
        std::unordered_map<std::string, std::string> after;
        for (auto it = existing_state["items"].begin();
             it != existing_state["items"].end(); ++it)
            after.emplace(it.key(), item_recency_at(it.value()));
        for (auto it = items_patch.begin(); it != items_patch.end(); ++it)
            after[it.key()] = item_recency_at(it.value());

        if (after.size() > pm::llm::k_session_items_max) {
            std::vector<std::pair<std::string, std::string>> ranked(
                after.begin(), after.end());
            std::sort(ranked.begin(), ranked.end(),
                [](const auto& a, const auto& b) {
                    if (a.second != b.second) return a.second < b.second;
                    return a.first < b.first;
                });
            std::size_t remaining =
                after.size() - pm::llm::k_session_items_max;
            for (const auto& [victim, unused] : ranked) {
                (void)unused;
                if (remaining == 0) break;
                if (items_patch.contains(victim)) continue;
                items_patch[victim] = nullptr;
                artifacts_patch[victim] = nullptr;
                evicted_item_ids.push_back(victim);
                --remaining;
            }
        }
    }

    nlohmann::json patch = {
        {"items", std::move(items_patch)},
        {"artifacts", std::move(artifacts_patch)},
    };
    const bool applied = task_store_write_memory(session_id, patch);
    return {
        {"session_id", session_id},
        {"applied", applied},
        {"checkpoint_count", checkpoint_count},
        {"evicted_items", static_cast<int>(evicted_item_ids.size())},
        {"evicted_item_ids", std::move(evicted_item_ids)},
        {"patch", std::move(patch)},
    };
}

nlohmann::json truncate_memory_update_for_log(nlohmann::json payload)
{
    if (!payload.is_object() || !payload.contains("patch"))
        return payload;
    const std::string patch_dump = payload["patch"].dump();
    if (patch_dump.size() <= pm::llm::k_memory_update_log_chars)
        return payload;
    nlohmann::json digest = nlohmann::json::array();
    if (payload["patch"].is_object() && payload["patch"].contains("items") &&
        payload["patch"]["items"].is_object()) {
        for (auto it = payload["patch"]["items"].begin();
             it != payload["patch"]["items"].end(); ++it) {
            digest.push_back(it.key());
            if (digest.size() >= 32) break;
        }
    }
    payload.erase("patch");
    payload["patch_omitted"]   = true;
    payload["patch_chars"]     = static_cast<int>(patch_dump.size());
    payload["patch_item_ids"]  = std::move(digest);
    return payload;
}

nlohmann::json session_persist_security_review(const std::string& session_id,
                                               const std::string& req_id,
                                               const nlohmann::json& consent_request,
                                               const nlohmann::json& review,
                                               const std::string& router,
                                               const std::string& model)
{
    if (session_id.empty() || !review.is_object())
        return nlohmann::json::object();

    const std::string now = now_iso8601();
    const std::string raw_id = req_id.empty()
        ? "security_review_" + args_hash(nlohmann::json{{"request", consent_request}, {"review", review}, {"ts", now}})
        : "security_review_" + req_id;
    const std::string id = sanitize_artifact_id(raw_id);

    std::string tool;
    if (consent_request.contains("tool") && consent_request["tool"].is_object())
        tool = consent_request["tool"].value("toolId", std::string{});
    const std::string action = consent_request.value("action", std::string{});
    const std::string input_risk = consent_request.value("risk", std::string{});
    const std::string assessed_risk = review.value("assessedRisk", std::string{});
    const std::string decision = review.value("decision", std::string{"ask"});
    std::string reason = review.value("reason", std::string{});
    if (reason.empty())
        reason = "AI security review returned no reason.";

    std::string summary = "AI security review";
    if (!tool.empty()) summary += " for " + tool;
    if (!action.empty()) summary += " " + action;
    if (!assessed_risk.empty()) summary += ": " + assessed_risk + " risk";
    summary += " / " + decision + ". " + reason;
    summary = clamp_summary(std::move(summary), pm::llm::k_session_item_summary_chars);

    nlohmann::json item{
        {"id", id},
        {"kind", "security_review"},
        {"role", "security_review"},
        {"origin", {{"type", "security"}, {"surface", "ai_review"}}},
        {"created_at", now},
        {"last_used_at", now},
        {"use_count", 1},
        {"summary", std::move(summary)},
        {"metadata", {
            {"req_id", req_id},
            {"tool", tool},
            {"action", action},
            {"input_risk", input_risk},
            {"assessed_risk", assessed_risk},
            {"risk_raised", review.value("riskRaised", false)},
            {"decision", decision},
            {"reason", reason},
            {"router", router},
            {"model", model},
            {"consent_request", consent_request},
            {"review", review},
        }},
    };

    nlohmann::json patch;
    patch["items"][id] = std::move(item);
    const bool applied = task_store_write_memory(session_id, patch);
    return {
        {"session_id", session_id},
        {"applied", applied},
        {"item_count", 1},
        {"artifact_count", 0},
        {"instruction_count", 0},
        {"patch", std::move(patch)},
    };
}

nlohmann::json session_persist_security_decision(const std::string& session_id,
                                                 const std::string& req_id,
                                                 const pm::security::ToolRequest& request,
                                                 const pm::security::PolicyDecision& decision)
{
    if (session_id.empty())
        return nlohmann::json::object();

    // Skip silent policy auto-allows (e.g. DEVELOPER workspace reads). Those
    // never showed a consent card; logging every call as security_consent
    // floods session memory and looks like repeated prompts.
    if (decision.kind == pm::security::DecisionKind::Allow && !decision.grant)
        return nlohmann::json::object({{"session_id", session_id},
                                       {"applied", false},
                                       {"item_count", 0},
                                       {"skipped", "silent_allow"}});

    const std::string now = now_iso8601();
    nlohmann::json target = nlohmann::json::object();
    if (request.target) {
        target["uri"] = request.target->uri;
        if (!request.target->path.empty())
            target["path"] = media::path::path_to_generic_utf8(request.target->path);
    }

    nlohmann::json grant = nlohmann::json::object();
    if (decision.grant) {
        grant = {
            {"id", decision.grant->id},
            {"lifetime", std::string(pm::security::to_string(decision.grant->lifetime))},
            {"scopes", decision.grant->resourceScopes},
            {"sandboxBackend", decision.grant->sandboxBackend},
            {"aiDelegated", decision.grant->aiDelegated},
            {"approvedBy", decision.grant->approvedBy},
        };
    }

    nlohmann::json key_material{
        {"req_id", req_id},
        {"tool", request.tool.toolId},
        {"action", std::string(pm::security::to_string(request.action))},
        {"decision", std::string(pm::security::to_string(decision.kind))},
        {"target", target},
        {"ts", now},
    };
    const std::string raw_id = req_id.empty()
        ? "security_decision_" + args_hash(key_material)
        : "security_decision_" + req_id;
    const std::string id = sanitize_artifact_id(raw_id);

    const std::string decision_s = std::string(pm::security::to_string(decision.kind));
    const std::string risk_s = std::string(pm::security::to_string(decision.risk));
    std::string summary = "Security consent " + decision_s + " for " + request.tool.toolId
        + " " + std::string(pm::security::to_string(request.action))
        + " (" + risk_s + " risk).";
    if (!decision.reason.empty())
        summary += " " + decision.reason;
    summary = clamp_summary(std::move(summary), pm::llm::k_session_item_summary_chars);

    nlohmann::json item{
        {"id", id},
        {"kind", "security_consent"},
        {"role", "security_decision"},
        {"origin", {{"type", "security"}, {"surface", "consent"}}},
        {"created_at", now},
        {"last_used_at", now},
        {"use_count", 1},
        {"summary", std::move(summary)},
        {"metadata", {
            {"req_id", req_id},
            {"tool", request.tool.toolId},
            {"provider", request.tool.provider},
            {"action", std::string(pm::security::to_string(request.action))},
            {"risk", risk_s},
            {"decision", decision_s},
            {"reason", decision.reason},
            {"target", std::move(target)},
            {"workspace_id", request.workspaceId},
            {"session_id", request.sessionId},
            {"task_id", request.taskId},
            {"grant", std::move(grant)},
        }},
    };

    nlohmann::json patch;
    patch["items"][id] = std::move(item);
    const bool applied = task_store_write_memory(session_id, patch);
    return {
        {"session_id", session_id},
        {"applied", applied},
        {"item_count", 1},
        {"artifact_count", 0},
        {"instruction_count", 0},
        {"patch", std::move(patch)},
    };
}

// ── Feature-mask parsing (decimal / 0xHEX / named tokens) ────────────────────
namespace {

using SMF  = pm::llm::StructuredMemoryFeature;
using SMFs = pm::llm::StructuredMemoryFeatures;

struct FeatureName {
    std::string_view name;  // canonical CamelCase
    SMF              bit;
};

// Keep in sync with pm::llm::StructuredMemoryFeature in llm_constants.hpp.
constexpr std::array<FeatureName, 17> k_feature_table = {{
    {"CaptureWriteArtifacts",  SMF::CaptureWriteArtifacts},
    {"CaptureMcpObservations", SMF::CaptureMcpObservations},
    {"SynthesizeRules",        SMF::SynthesizeRules},
    {"DedupOnWrite",           SMF::DedupOnWrite},
    {"TruncateSummaries",      SMF::TruncateSummaries},
    {"EvictOldest",            SMF::EvictOldest},
    {"InjectItems",            SMF::InjectItems},
    {"InjectRules",            SMF::InjectRules},
    {"InjectInPlanner",        SMF::InjectInPlanner},
    {"EmitMemoryUpdate",       SMF::EmitMemoryUpdate},
    {"CaptureTurnPairs",       SMF::CaptureTurnPairs},
    {"ReplayTranscript",       SMF::ReplayTranscript},
    {"CaptureErrorReflexions", SMF::CaptureErrorReflexions},
    {"CaptureSelfCheck",       SMF::CaptureSelfCheck},
    {"IndexMemoryEntries",     SMF::IndexMemoryEntries},
    {"HybridRecallSearch",     SMF::HybridRecallSearch},
    {"InjectPendingRecall",    SMF::InjectPendingRecall},
}};

/// Lowercase + strip underscores. "Inject_Items" / "INJECTITEMS" / "inject_items"
/// all collapse to the same canonical key for matching.
std::string canon_feature_token(std::string_view t) {
    std::string out;
    out.reserve(t.size());
    for (char c : t) {
        if (c == '_') continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::optional<SMFs> mask_from_name(std::string_view token) {
    const std::string key = canon_feature_token(token);
    if (key == "all")     return static_cast<SMFs>(SMF::All);
    if (key == "default") return pm::llm::k_default_structured_memory_features;
    if (key == "none")    return static_cast<SMFs>(0);
    for (const auto& e : k_feature_table) {
        if (canon_feature_token(e.name) == key)
            return static_cast<SMFs>(e.bit);
    }
    return std::nullopt;
}

std::optional<SMFs> mask_from_numeric(std::string_view token) {
    if (token.empty()) return std::nullopt;
    const std::string s(token);
    const int base = (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10;
    if (base == 10) {
        for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }
    try {
        return static_cast<SMFs>(std::stoul(s, nullptr, base));
    } catch (...) {
        return std::nullopt;
    }
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.remove_suffix(1);
    return s;
}

} // namespace

std::string_view structured_memory_feature_name(pm::llm::StructuredMemoryFeature f) {
    for (const auto& e : k_feature_table)
        if (e.bit == f) return e.name;
    return {};
}

pm::llm::StructuredMemoryFeatures apply_structured_memory_features_env(
    pm::llm::StructuredMemoryFeatures fallback, const char* origin)
{
    const char* raw = std::getenv("PM_LLM_STRUCTURED_MEMORY_FEATURES");
    if (!raw || !*raw) return fallback;
    auto parsed = parse_structured_memory_features(raw);
    if (!parsed) {
        logger::warn(std::string("structured_memory_features: ignoring malformed ")
                     + (origin ? origin : "env") + " = " + raw);
        return fallback;
    }
    std::string hex;
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%x", static_cast<unsigned>(*parsed));
        hex = buf;
    }
    logger::info(std::string("structured_memory_features ") + (origin ? origin : "env")
                 + " = " + raw + " (resolved " + hex + ")");
    return *parsed;
}

std::optional<pm::llm::StructuredMemoryFeatures>
parse_structured_memory_features(std::string_view spec)
{
    spec = trim(spec);
    if (spec.empty()) return std::nullopt;

    SMFs mask = 0;
    bool parsed_any = false;

    std::size_t cursor = 0;
    while (cursor <= spec.size()) {
        const std::size_t comma = spec.find(',', cursor);
        const std::size_t end   = (comma == std::string_view::npos) ? spec.size() : comma;
        std::string_view token  = trim(spec.substr(cursor, end - cursor));
        cursor = (comma == std::string_view::npos) ? spec.size() + 1 : comma + 1;
        if (token.empty()) continue;

        char op = '|';
        if (token.front() == '+' || token.front() == '-') {
            op = token.front();
            token.remove_prefix(1);
            token = trim(token);
            if (token.empty()) continue;
        }

        std::optional<SMFs> bits = mask_from_numeric(token);
        if (!bits) bits = mask_from_name(token);
        if (!bits) continue; // unknown token: silently skip (caller may want to warn)

        if (op == '-') mask &= ~(*bits);
        else           mask |=  (*bits);
        parsed_any = true;
    }

    if (!parsed_any) return std::nullopt;
    return mask;
}

// ── P5: error-reflexion sink ──────────────────────────────────────────────────

nlohmann::json session_persist_error_reflexion(
    const std::string& session_id,
    const std::string& user_prompt,
    const Result&      result,
    pm::llm::StructuredMemoryFeatures features)
{
    using SMF = pm::llm::StructuredMemoryFeature;

    if (!pm::llm::has_feature(features, SMF::CaptureErrorReflexions))
        return nlohmann::json::object();

    // Only run when the turn ended badly (ok=false OR cancelled).
    if (result.ok)
        return nlohmann::json::object();

    if (session_id.empty())
        return nlohmann::json::object();

    // Scan the transcript for tool results that returned ok=false.
    // We record the first failing tool call — that is usually the root cause.
    std::string error_tool;
    std::string error_message;

    if (result.transcript.is_object() && result.transcript.contains("messages") &&
        result.transcript["messages"].is_array())
    {
        // Build a map of tool_call_id → tool_name from assistant messages.
        std::unordered_map<std::string, std::string> id_to_tool;
        for (const auto& m : result.transcript["messages"]) {
            if (!m.is_object() || m.value("role", std::string{}) != "assistant" ||
                !m.contains("tool_calls") || !m["tool_calls"].is_array())
                continue;
            for (const auto& tc : m["tool_calls"]) {
                if (!tc.is_object()) continue;
                const std::string id = tc.value("id", std::string{});
                if (id.empty() || !tc.contains("function")) continue;
                id_to_tool[id] = tc["function"].value("name", std::string{});
            }
        }

        // Find the first tool message whose envelope has ok=false.
        for (const auto& m : result.transcript["messages"]) {
            if (!m.is_object() || m.value("role", std::string{}) != "tool")
                continue;
            const std::string tcid = m.value("tool_call_id", std::string{});
            auto it = id_to_tool.find(tcid);
            if (it == id_to_tool.end()) continue;

            const nlohmann::json& env = m.contains("content")
                ? m["content"] : nlohmann::json::object();
            const bool tool_ok = !env.is_object() || env.value("ok", true);
            if (tool_ok) continue;

            error_tool    = it->second;
            error_message = json_str(env, "error");
            if (error_message.empty())
                error_message = json_str(env, "stderr");
            if (error_message.empty() && env.is_object())
                error_message = media::str_san::utf8_safe_ellipsize(
                    detail::safe_json_dump(env), 200);
            break;
        }
    }

    // If no tool failed explicitly, but the turn failed (e.g. iterations
    // exhausted), still persist with error_tool = "(none)".
    if (error_tool.empty() && !result.cancelled)
        error_tool = "(none)";

    // Cancelled turns without a failed tool are not interesting to record.
    if (error_tool.empty())
        return nlohmann::json::object();

    // Build a short, stable id from the tool name + prompt prefix.
    const std::string prompt_prefix = user_prompt.substr(0, 40);
    const std::string raw_id = "err_" + error_tool + "_" + prompt_prefix;
    const std::string id = sanitize_artifact_id(raw_id);

    const std::string now = now_iso8601();
    const std::string prompt_summary = user_prompt.size() <= 120
        ? user_prompt
        : user_prompt.substr(0, 120) + "…";

    std::string summary = "Turn failed";
    if (result.cancelled) summary = "Turn cancelled";
    if (!error_tool.empty() && error_tool != "(none)")
        summary += " at " + error_tool;
    if (!error_message.empty())
        summary += ": " + error_message.substr(0, 120);

    nlohmann::json item = {
        {"id",                  id},
        {"kind",                "error_reflexion"},
        {"error_tool",          error_tool},
        {"error_message",       error_message.substr(0, 300)},
        {"user_prompt_summary", prompt_summary},
        {"iterations",          result.iterations},
        {"outcome",             result.cancelled ? "cancelled" : "failed"},
        {"created_at",          now},
        {"last_used_at",        now},
        {"use_count",           1},
        {"summary",             std::move(summary)},
    };

    // Write the item to session memory using the same patch mechanism as the
    // normal artifact sink.  patch shape: {items:{…}, artifacts:{}, instructions:{}}
    nlohmann::json patch;
    patch["items"][id]    = item;
    patch["artifacts"]    = nlohmann::json::object();
    patch["instructions"] = nlohmann::json::object();
    const bool applied = task_store_write_memory(session_id, patch);

    nlohmann::json update;
    update["session_id"]               = session_id;
    update["applied"]                  = applied;
    update["item_count"]               = 1;
    update["error_reflexion_written"]  = id;
    update["patch"]                    = std::move(patch);
    return update;
}

nlohmann::json session_recover_from_journal(const std::string& session_id,
                                            pm::llm::StructuredMemoryFeatures features)
{
    using SMF = pm::llm::StructuredMemoryFeature;

    if (session_id.empty() || features == 0)
        return nlohmann::json::object();
    if (!pm::llm::has_feature(features, SMF::CaptureWriteArtifacts))
        return nlohmann::json::object();

    const nlohmann::json journal = session_journal_read(session_id);
    if (!journal.is_array() || journal.empty())
        return nlohmann::json::object();

    nlohmann::json existing_state = nlohmann::json::object();
    if (!session_read_memory_state(session_id, existing_state))
        existing_state = nlohmann::json::object();

    const bool truncate_summaries = pm::llm::has_feature(features, SMF::TruncateSummaries);
    const auto recovered_ids = collect_recovered_call_ids(existing_state);
    auto existing_paths = collect_item_path_keys(existing_state);

    nlohmann::json items_patch = nlohmann::json::object();
    nlohmann::json artifacts_patch = nlohmann::json::object();
    nlohmann::json newly_recovered_ids = nlohmann::json::array();
    nlohmann::json promoted_paths = nlohmann::json::array();
    int promoted = 0;
    const std::string now = now_iso8601();

    for (const auto& entry : journal) {
        if (!entry.is_object()) continue;

        const std::string kind = entry.value("kind", std::string{});
        if (kind == "turn_started" || kind == "turn_completed")
            continue;

        const std::string tool_call_id = entry.value("tool_call_id", std::string{});
        if (tool_call_id.empty() || recovered_ids.count(tool_call_id))
            continue;

        bool ok_value = false;
        try { ok_value = entry.value("ok", false); } catch (...) {}
        if (!ok_value)
            continue;

        const std::string tool_name = entry.value("tool", std::string{});
        if (!tool_produces_path_artifact(tool_name))
            continue;

        if (!entry.contains("output_paths") || !entry["output_paths"].is_array())
            continue;

        bool any_path_promoted = false;
        for (const auto& raw_path : entry["output_paths"]) {
            if (!raw_path.is_string()) continue;
            const std::string path = raw_path.get<std::string>();
            if (path.empty()) continue;

            const std::string path_key = norm_path_key(path);
            if (path_key.empty() || existing_paths.count(path_key))
                continue;

            nlohmann::json item = build_item_from_journal_path(
                tool_name, tool_call_id, path, now, truncate_summaries);
            if (item.empty() || !item.contains("id"))
                continue;

            const std::string id = item["id"].get<std::string>();
            if (items_patch.contains(id))
                continue;

            items_patch[id] = item;
            artifacts_patch[id] = {
                {"id", id},
                {"path", path},
                {"kind", item.contains("content_type") && item["content_type"].is_string()
                            ? item["content_type"].get<std::string>() : "file"},
                {"role", item.value("role", std::string{})},
                {"created_by_tool", tool_name},
                {"created_at", now},
                {"summary", item.value("summary", std::string{})},
                {"metadata", item.value("metadata", nlohmann::json::object())},
            };
            promoted_paths.push_back(path);
            existing_paths.insert(path_key);
            any_path_promoted = true;
        }

        newly_recovered_ids.push_back(tool_call_id);
        if (any_path_promoted)
            ++promoted;
    }

    if (newly_recovered_ids.empty())
        return nlohmann::json::object();

    nlohmann::json recovery_meta = nlohmann::json::object();
    if (existing_state.contains("journal_recovery") && existing_state["journal_recovery"].is_object())
        recovery_meta = existing_state["journal_recovery"];
    if (!recovery_meta.contains("recovered_tool_call_ids") ||
        !recovery_meta["recovered_tool_call_ids"].is_array())
        recovery_meta["recovered_tool_call_ids"] = nlohmann::json::array();
    for (const auto& id : newly_recovered_ids)
        recovery_meta["recovered_tool_call_ids"].push_back(id);
    recovery_meta["last_at"] = now;

    nlohmann::json patch;
    if (!items_patch.empty())
        patch["items"] = items_patch;
    if (!artifacts_patch.empty())
        patch["artifacts"] = artifacts_patch;
    patch["journal_recovery"] = std::move(recovery_meta);

    const bool applied = task_store_write_memory(session_id, patch);

    nlohmann::json recovery_event = {
        {"kind", "journal_recovery"},
        {"ts", now},
        {"recovered", promoted},
        {"paths", promoted_paths},
        {"incomplete_turn", journal_has_incomplete_turn(journal)},
    };
    if (promoted > 0)
        task_store_append_event(session_id, recovery_event);

    if (promoted > 0) {
        logger::info(std::string("[memory] journal recovery session=") + session_id
                     + " promoted=" + std::to_string(promoted)
                     + " applied=" + (applied ? "true" : "false"));
    }

    if (promoted == 0)
        return nlohmann::json::object();

    return {
        {"session_id", session_id},
        {"recovered", promoted},
        {"applied", applied},
        {"paths", std::move(promoted_paths)},
        {"recovery_event", std::move(recovery_event)},
    };
}

} // namespace media::llm::agent

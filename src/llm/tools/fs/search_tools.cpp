// Copyright PolyMech / pm-image. Internal use only.
//
// Agent tools over the existing search index. Stores live under
// {config}/temp/search-index and are removed by the idle app task after one day.
#include "llm/tools/fs/FsTools.hpp"

#include "core/cli_cancel.hpp"
#include "search/search_facade.hpp"
#include "tasks/search_index_cleanup.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

namespace media::llm::path::detail {
namespace {

namespace fs = std::filesystem;

constexpr int k_query_default = 8;
constexpr int k_query_max = 12;
constexpr std::size_t k_detail_max_bytes = 8 * 1024;
constexpr int k_outline_files = 48;
constexpr int k_outline_headings = 12;

bool cancelled()
{
    media::cli::yield_to_interrupt();
    return media::cli::cancel_requested();
}

std::string fnv_hex(std::string_view text)
{
    std::uint64_t h = 14695981039346656037ull;
    for (unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << h;
    return out.str();
}

bool valid_index_id(std::string_view id)
{
    if (id.size() < 8 || id.size() > 32)
        return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

fs::path store_dir(const std::string& id)
{
    return pm::tasks::agent_search_index_root() / id / "index.vstore";
}

void touch_stamp(const std::string& id)
{
    const fs::path dir = pm::tasks::agent_search_index_root() / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream out(dir / "indexed-at", std::ios::trunc);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    if (out)
        out << now << '\n';
}

std::string newest_index_id()
{
    const fs::path root = pm::tasks::agent_search_index_root();
    std::error_code ec;
    if (!fs::is_directory(root, ec))
        return {};
    std::string best_id;
    std::int64_t best = 0;
    for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_directory())
            continue;
        const std::string id = media::path::path_to_utf8(it->path().filename());
        if (!valid_index_id(id))
            continue;
        std::ifstream in(it->path() / "indexed-at");
        std::int64_t stamp = 0;
        if (!(in >> stamp) || stamp < best)
            continue;
        best = stamp;
        best_id = id;
    }
    return best_id;
}

std::string require_index_id(const nlohmann::json& args, std::string* error)
{
    std::string id;
    if (auto given = get_nonempty_string(args, "index"))
        id = *given;
    else
        id = newest_index_id();
    if (!valid_index_id(id)) {
        *error = "search: pass index from search_index, or index a folder first";
        return {};
    }
    std::error_code ec;
    if (!fs::is_directory(store_dir(id), ec)) {
        *error = "search: index '" + id + "' is not on disk";
        return {};
    }
    return id;
}

std::vector<std::string> input_paths(const nlohmann::json& args, std::string* error)
{
    std::vector<std::string> raw = get_string_array(args, "paths");
    if (raw.empty()) {
        if (auto one = get_nonempty_string(args, "path"))
            raw.push_back(*one);
    }
    if (raw.empty()) {
        *error = "search_index: 'path' or 'paths' is required";
        return {};
    }
    std::vector<std::string> out;
    out.reserve(raw.size());
    for (const std::string& item : raw) {
        const std::string resolved = resolve_tool_path_string(item);
        if (const std::string deny = llm_path_fs_deny(resolved); !deny.empty()) {
            *error = "search_index: " + deny;
            return {};
        }
        std::error_code ec;
        if (!fs::exists(media::path::path_from_utf8(resolved), ec)) {
            *error = "search_index: path does not exist: " + item;
            return {};
        }
        out.push_back(media::path::path_to_generic_utf8(media::path::path_from_utf8(resolved)));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string norm_path(const fs::path& path)
{
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(path, ec);
    if (ec)
        canon = path;
    std::string text = media::path::path_to_generic_utf8(canon);
#if defined(_WIN32)
    text = media::strutil::lower_ascii(std::move(text));
#endif
    return text;
}

std::mutex g_indexed_mu;
std::unordered_map<std::string, std::string> g_indexed_paths;
std::mutex g_budget_mu;
std::unordered_map<std::string, int> g_query_used;
std::unordered_map<std::string, int> g_detail_used;

constexpr int k_query_budget = 1;
constexpr int k_detail_budget = 2;

bool take_budget(std::unordered_map<std::string, int>& used, const std::string& id, int limit)
{
    std::lock_guard<std::mutex> lock(g_budget_mu);
    int& n = used[id];
    if (n >= limit)
        return false;
    ++n;
    return true;
}

void reset_budget(const std::string& id)
{
    std::lock_guard<std::mutex> lock(g_budget_mu);
    g_query_used.erase(id);
    g_detail_used.erase(id);
}

void remember_indexed(const std::string& id, const nlohmann::json& outline)
{
    std::lock_guard<std::mutex> lock(g_indexed_mu);
    if (!outline.is_array())
        return;
    for (const auto& file : outline) {
        const std::string path = file.value("path", std::string{});
        if (path.empty())
            continue;
        g_indexed_paths[norm_path(media::path::path_from_utf8(path))] = id;
    }
}

nlohmann::json outline_for(const fs::path& index_dir)
{
    nlohmann::json files = nlohmann::json::array();
    std::ifstream in(index_dir / "fingerprints.jsonl");
    std::string line;
    while (in && std::getline(in, line) && static_cast<int>(files.size()) < k_outline_files) {
        if (line.empty())
            continue;
        try {
            const nlohmann::json rec = nlohmann::json::parse(line);
            const nlohmann::json structure = rec.value("structure", nlohmann::json::object());
            nlohmann::json headings = nlohmann::json::array();
            if (structure.contains("headings") && structure["headings"].is_array()) {
                for (const auto& h : structure["headings"]) {
                    if (!h.is_string())
                        continue;
                    headings.push_back(h.get<std::string>());
                    if (static_cast<int>(headings.size()) >= k_outline_headings)
                        break;
                }
            }
            files.push_back({
                {"path", rec.value("path", std::string{})},
                {"title", rec.value("titleGuess", std::string{})},
                {"headings", std::move(headings)},
            });
        } catch (...) {
        }
    }
    return files;
}

nlohmann::json hit_json(const media::search::Hit& hit)
{
    std::uint64_t doc_id = 0;
    if (hit.metadata.contains("doc_id") && hit.metadata["doc_id"].is_number())
        doc_id = hit.metadata["doc_id"].get<std::uint64_t>();
    return {
        {"doc_id", doc_id},
        {"path", hit.path},
        {"title", hit.title},
        {"score", hit.score},
        {"snippet", hit.snippet},
    };
}

} // namespace

std::string indexed_read_block(const std::string& abs_path)
{
    std::lock_guard<std::mutex> lock(g_indexed_mu);
    const auto it = g_indexed_paths.find(norm_path(media::path::path_from_utf8(abs_path)));
    if (it == g_indexed_paths.end())
        return {};
    return "file_read: this file is in search index " + it->second
        + ". Call search_query, then search_detail(doc_id) for one passage.";
}

std::string converted_part_read_block(const std::string& abs_path)
{
    const fs::path p(abs_path);
    const std::string name = media::path::path_to_utf8(p.filename());
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || dot < 3)
        return {};
    std::string ext = name.substr(dot);
    ext = media::strutil::lower_ascii(std::move(ext));
    if (ext != ".md")
        return {};
    std::size_t i = dot;
    while (i > 0 && std::isdigit(static_cast<unsigned char>(name[i - 1])))
        --i;
    if (i == dot || i == 0 || name[i - 1] != '_')
        return {};
    std::string parent = media::path::path_to_utf8(p.parent_path().filename());
    parent = media::strutil::lower_ascii(std::move(parent));
    if (parent.size() < 4 || parent.compare(parent.size() - 3, 3, "_md") != 0)
        return {};
    return "file_read: this is one part of a converted document. Call search_index on \""
        + media::path::path_to_generic_utf8(p.parent_path())
        + "\", then search_query. Do not read each part. If search_index is not in your "
          "tool list, call request_tools for search_index, search_query, and search_detail.";
}

ExecuteResult do_search_index(const nlohmann::json& args)
{
    std::string error;
    const std::vector<std::string> inputs = input_paths(args, &error);
    if (!error.empty())
        return fatal(error);
    if (cancelled())
        return fatal("search_index: cancelled");

    std::string joined;
    for (const std::string& p : inputs) {
        joined += p;
        joined.push_back('\n');
    }
    const std::string id = fnv_hex(joined);

    media::search::Request req;
    req.inputs = inputs;
    req.content = media::search::Content::Any;
    req.backend = media::search::Backend::Vector;
    req.mode = media::search::Mode::Semantic;
    req.index_policy = media::search::IndexPolicy::SpecificDir;
    req.index_dir = media::path::path_to_utf8(store_dir(id));
    req.index_dirs = {req.index_dir};
    req.create_index = true;
    req.recursive = true;
    req.fingerprint = "cheap";
    req.gpu_layers = media::search::k_default_gpu_layers;
    req.cancel_requested = [] { return cancelled(); };

    const media::search::Result indexed = media::search::unified_index(req);
    if (!indexed.ok)
        return fatal(indexed.error.empty() ? "search_index: failed" : indexed.error);
    touch_stamp(id);

    const nlohmann::json& stats = indexed.stats;
    nlohmann::json outline = outline_for(store_dir(id));
    remember_indexed(id, outline);
    reset_budget(id);
    nlohmann::json payload = {
        {"tool", "search_index"},
        {"index", id},
        {"indexed_files", stats.value("indexed_files", 0)},
        {"indexed_chunks", stats.value("indexed_chunks", 0)},
        {"unchanged_files", stats.value("unchanged_files", 0)},
        {"doc_count", stats.value("doc_count", 0)},
        {"skipped_files", stats.value("skipped_files", 0)},
        {"outline", std::move(outline)},
        {"next", "Call search_query once, then write_file the summary from the outline and the hits. Do not file_read the indexed files."},
    };
    return simple_ok(std::move(payload));
}

ExecuteResult do_search_query(const nlohmann::json& args)
{
    const auto query = get_nonempty_string(args, "query");
    if (!query)
        return fatal("search_query: 'query' is required");
    std::string error;
    const std::string id = require_index_id(args, &error);
    if (id.empty())
        return fatal(error.empty() ? "search_query: index is required" : error);

    int limit = k_query_default;
    if (args.contains("limit") && args["limit"].is_number_integer())
        limit = args["limit"].get<int>();
    if (limit < 1)
        limit = 1;
    if (limit > k_query_max)
        limit = k_query_max;
    if (!take_budget(g_query_used, id, k_query_budget))
        return fatal("search_query: this index was already searched. Call write_file with the summary. Do not search again.");
    if (cancelled())
        return fatal("search_query: cancelled");

    media::search::Request req;
    req.query = *query;
    req.content = media::search::Content::Text;
    req.mode = media::search::Mode::Semantic;
    req.backend = media::search::Backend::Vector;
    req.index_policy = media::search::IndexPolicy::SpecificDir;
    req.index_dir = media::path::path_to_utf8(store_dir(id));
    req.index_dirs = {req.index_dir};
    req.create_index = false;
    req.limit = limit;
    req.cancel_requested = [] { return cancelled(); };

    const media::search::Result found = media::search::unified_search(req);
    if (!found.ok)
        return fatal(found.error.empty() ? "search_query: failed" : found.error);

    nlohmann::json hits = nlohmann::json::array();
    for (const media::search::Hit& hit : found.hits)
        hits.push_back(hit_json(hit));
    return simple_ok({
        {"tool", "search_query"},
        {"index", id},
        {"query", *query},
        {"hits", std::move(hits)},
        {"next", "Write the markdown now with write_file, using these hits and the search_index outline. "
                  "search_detail at most once. Do not call search_query again."},
    });
}

ExecuteResult do_search_detail(const nlohmann::json& args)
{
    if (!args.contains("doc_id") || !args["doc_id"].is_number_integer())
        return fatal("search_detail: 'doc_id' from a search_query hit is required");
    const std::uint64_t doc_id = args["doc_id"].get<std::uint64_t>();
    std::string error;
    const std::string id = require_index_id(args, &error);
    if (id.empty())
        return fatal(error.empty() ? "search_detail: index is required" : error);
    if (!take_budget(g_detail_used, id, k_detail_budget))
        return fatal("search_detail: passage budget used. Call write_file with the summary. Do not search again.");

    std::ifstream in(store_dir(id) / "docs.jsonl");
    if (!in)
        return fatal("search_detail: index has no documents");
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        try {
            const nlohmann::json doc = nlohmann::json::parse(line);
            if (doc.value("id", std::uint64_t{0}) != doc_id)
                continue;
            std::string text = doc.value("chunk", std::string{});
            bool truncated = false;
            if (text.size() > k_detail_max_bytes) {
                text.resize(k_detail_max_bytes);
                truncated = true;
            }
            return simple_ok({
                {"tool", "search_detail"},
                {"index", id},
                {"doc_id", doc_id},
                {"path", doc.value("path", std::string{})},
                {"title", doc.value("title", std::string{})},
                {"text", std::move(text)},
                {"truncated", truncated},
                {"next", "Write the markdown now with write_file. Do not call search_query or search_detail again."},
            });
        } catch (...) {
        }
    }
    return fatal("search_detail: doc_id " + std::to_string(doc_id) + " was not found");
}

ToolDef make_search_index()
{
    return ToolDef{
        "search_index",
        "Index a file or folder into a temporary document index (markdown headings become "
        "chunks). Call this after download or pdf-to-md, before reading. Returns index id "
        "and a heading outline. Does not return file bodies. The index lives in the app "
        "temp directory and is deleted after one day. Then call search_query.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"},
                          {"description", "File or folder to index. Relative paths use the Explorer folder."}}},
                {"paths", {{"type", "array"},
                           {"items", {{"type", "string"}}},
                           {"description", "Several files or folders. Use path for a single folder."}}},
            }},
        }};
}

ToolDef make_search_query()
{
    return ToolDef{
        "search_query",
        "Search a temporary document index. Returns a few hits (doc_id, path, title, snippet). "
        "Pass index from search_index. Do not file_read the indexed parts. "
        "Call search_detail with a hit's doc_id to open that one passage.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"query", {{"type", "string"},
                           {"description", "What to find. A section name, claim, or short question."}}},
                {"index", {{"type", "string"},
                           {"description", "Index id from search_index. Omit to use the newest index."}}},
                {"limit", {{"type", "integer"},
                           {"minimum", 1},
                           {"maximum", k_query_max},
                           {"description", "Max hits. Default 8."}}},
            }},
            {"required", nlohmann::json::array({"query"})},
        }};
}

ToolDef make_search_detail()
{
    return ToolDef{
        "search_detail",
        "Return the text of one indexed passage. doc_id comes from search_query. "
        "This is the read for an indexed document. Do not file_read that file instead.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"doc_id", {{"type", "integer"},
                            {"minimum", 1},
                            {"description", "doc_id from a search_query hit."}}},
                {"index", {{"type", "string"},
                           {"description", "Index id from search_index. Omit to use the newest index."}}},
            }},
            {"required", nlohmann::json::array({"doc_id"})},
        }};
}

} // namespace media::llm::path::detail

#include "llm/huggingface/hf_ref.hpp"

#include "llm/huggingface/hf_catalog.hpp"
#include "net/uri.hpp"

#include <algorithm>
#include <cctype>
#include "string/string_utils.hpp"

namespace pm::hf {
namespace {

std::string trim_ascii(std::string s)
{
    auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](char c) {
        return !is_ws(static_cast<unsigned char>(c));
    }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [&](char c) {
        return !is_ws(static_cast<unsigned char>(c));
    }).base(), s.end());
    return s;
}

using media::strutil::lower_ascii;

std::string strip_query_fragment(std::string s)
{
    const auto q = s.find('?');
    if (q != std::string::npos)
        s.erase(q);
    const auto h = s.find('#');
    if (h != std::string::npos)
        s.erase(h);
    return s;
}

std::vector<std::string> path_segments(std::string path)
{
    while (!path.empty() && (path.front() == '/' || path.back() == '/')) {
        if (path.front() == '/')
            path.erase(path.begin());
        if (!path.empty() && path.back() == '/')
            path.pop_back();
    }
    std::vector<std::string> out;
    size_t b = 0;
    while (b < path.size()) {
        const size_t e = path.find('/', b);
        const std::string seg = path.substr(b, e == std::string::npos ? std::string::npos : e - b);
        if (!seg.empty())
            out.push_back(seg);
        if (e == std::string::npos)
            break;
        b = e + 1;
    }
    return out;
}

bool is_reserved_hf_segment(const std::string& seg)
{
    static const char* k_reserved[] = {
        "models", "datasets", "spaces", "docs", "enterprise", "pricing",
        "login", "signup", "settings", "api", "resolve", "tree", "blob",
    };
    const std::string lower = lower_ascii(seg);
    for (const char* r : k_reserved) {
        if (lower == r)
            return true;
    }
    return false;
}

std::optional<ModelEntry> catalog_match(const std::string& key)
{
    for (const auto& e : catalog_entries()) {
        if (lower_ascii(e.alias) == key || lower_ascii(e.repo_id) == key || lower_ascii(e.store_name) == key)
            return e;
    }
    return std::nullopt;
}

} // namespace

ParsedRef parse_hf_ref(std::string input)
{
    ParsedRef out;
    out.raw = trim_ascii(std::move(input));
    if (out.raw.empty())
        return out;

    // HF URL → repo or org
    if (const auto uri = pm::uri::parse_http_uri(out.raw)) {
        if (uri->host != "huggingface.co" && uri->host != "www.huggingface.co")
            return out;
        const auto segs = path_segments(uri->path);
        if (segs.size() >= 2 && !is_reserved_hf_segment(segs[0])) {
            out.kind    = RefKind::Repo;
            out.repo_id = segs[0] + "/" + segs[1];
            out.org     = segs[0];
            return out;
        }
        if (segs.size() == 1 && !is_reserved_hf_segment(segs[0])) {
            out.kind = RefKind::Org;
            out.org  = segs[0];
            return out;
        }
        return out;
    }

    const std::string raw_lower = lower_ascii(out.raw);
    if (raw_lower.starts_with("http://") || raw_lower.starts_with("https://"))
        return out;

    std::string work = strip_query_fragment(out.raw);
    if (work.rfind("huggingface.co/", 0) == 0) {
        return parse_hf_ref("https://" + work);
    }

    const auto slash = work.find('/');
    if (slash != std::string::npos) {
        out.kind    = RefKind::Repo;
        out.repo_id = work;
        out.org     = work.substr(0, slash);
        return out;
    }

    const std::string key = lower_ascii(work);
    if (catalog_match(key))
        return out; // caller resolves via catalog

    out.kind = RefKind::Org;
    out.org  = work;
    return out;
}

bool looks_like_repo_id(const std::string& s)
{
    const auto slash = s.find('/');
    return slash != std::string::npos && slash > 0 && slash + 1 < s.size();
}

ModelEntry entry_from_repo_id(const std::string& repo_id)
{
    ModelEntry e;
    e.repo_id    = repo_id;
    e.store_name = repo_id;
    if (const auto slash = repo_id.rfind('/'); slash != std::string::npos && slash + 1 < repo_id.size())
        e.store_name = repo_id.substr(slash + 1);
    e.format = "gguf";
    return e;
}

std::optional<ModelEntry> resolve_model_ref(const std::string& input)
{
    const std::string trimmed = trim_ascii(input);
    if (trimmed.empty())
        return std::nullopt;

    if (auto cat = catalog_match(lower_ascii(trimmed)))
        return cat;

    const ParsedRef parsed = parse_hf_ref(trimmed);
    if (parsed.kind == RefKind::Repo)
        return entry_from_repo_id(parsed.repo_id);

    if (looks_like_repo_id(trimmed))
        return entry_from_repo_id(trimmed);

    return std::nullopt;
}

} // namespace pm::hf

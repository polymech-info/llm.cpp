#include "llm/huggingface/hf_gguf.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

namespace fs = std::filesystem;

namespace pm::hf {
namespace {

using media::strutil::lower_ascii;

bool is_mmproj(const std::string& name)
{
    return lower_ascii(name).find("mmproj") != std::string::npos;
}

bool is_alnum_ascii(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

bool variant_boundary_match(std::string_view haystack_lower, std::string_view variant_lower)
{
    if (variant_lower.empty())
        return false;
    std::size_t pos = 0;
    while ((pos = haystack_lower.find(variant_lower, pos)) != std::string_view::npos) {
        const bool before_ok = pos == 0 || !is_alnum_ascii(haystack_lower[pos - 1]);
        const std::size_t after_pos = pos + variant_lower.size();
        const bool after_ok = after_pos >= haystack_lower.size() || !is_alnum_ascii(haystack_lower[after_pos]);
        if (before_ok && after_ok)
            return true;
        pos += 1;
    }
    return false;
}

bool is_label_token(const std::string& token)
{
    if (token.empty())
        return false;
    for (char c : token) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
            return false;
    }
    return true;
}

std::optional<std::string> extract_variant_label(const std::string& filename)
{
    const std::string name = media::path::path_to_utf8(media::path::path_from_utf8(filename).filename());
    if (name.size() < 6 || name.rfind(".gguf") != name.size() - 5 || is_mmproj(name))
        return std::nullopt;

    const std::string stem = name.substr(0, name.size() - 5);
    if (stem.find("-Instruct-") != std::string::npos || std::count(stem.begin(), stem.end(), '-') >= 2) {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : stem) {
            if (c == '-') {
                if (!cur.empty())
                    parts.push_back(cur);
                cur.clear();
            } else
                cur.push_back(c);
        }
        if (!cur.empty())
            parts.push_back(cur);
        for (int idx = static_cast<int>(parts.size()) - 1; idx > 0; --idx) {
            std::string candidate;
            for (int i = idx; i < static_cast<int>(parts.size()); ++i) {
                if (i > idx)
                    candidate.push_back('-');
                candidate += parts[static_cast<size_t>(i)];
            }
            if (is_label_token(candidate))
                return candidate;
        }
    }
    if (is_label_token(stem))
        return stem;
    return std::nullopt;
}

bool parse_shard_name(const std::string& leaf, std::string& prefix, int& total)
{
    if (leaf.size() < 14 || leaf.rfind(".gguf") != leaf.size() - 5)
        return false;
    const std::string stem = leaf.substr(0, leaf.size() - 5);
    const auto of_pos = stem.rfind("-of-");
    if (of_pos == std::string::npos || of_pos < 6)
        return false;
    const auto dash_before_of = stem.rfind('-', of_pos - 1);
    if (dash_before_of == std::string::npos || of_pos - dash_before_of != 6)
        return false;
    for (std::size_t i = dash_before_of + 1; i < of_pos; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(stem[i])))
            return false;
    }
    const std::string total_str = stem.substr(of_pos + 4);
    for (char c : total_str) {
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    }
    prefix = stem.substr(0, dash_before_of);
    total  = std::stoi(total_str);
    return total > 0;
}

bool shard_name_matches(const std::string& leaf, const std::string& prefix, int total)
{
    int parsed_total = 0;
    std::string parsed_prefix;
    if (!parse_shard_name(leaf, parsed_prefix, parsed_total))
        return false;
    return parsed_prefix == prefix && parsed_total == total;
}

} // namespace

std::vector<std::string> list_gguf_variants(const std::vector<RepoFileInfo>& repo_files)
{
    std::set<std::string> variants;
    for (const auto& f : repo_files) {
        if (auto label = extract_variant_label(f.name.empty() ? f.path : f.name))
            variants.insert(*label);
    }
    std::vector<std::string> out(variants.begin(), variants.end());
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return lower_ascii(a) < lower_ascii(b);
    });
    return out;
}

std::vector<std::string> pick_gguf_filenames(const std::vector<RepoFileInfo>& repo_files,
                                             const std::string& variant,
                                             std::string* err_out)
{
    if (err_out)
        err_out->clear();

    const std::string variant_lower = lower_ascii(variant);
    std::vector<std::string> matches;
    for (const auto& f : repo_files) {
        const std::string name = f.name.empty() ? f.path : f.name;
        if (name.size() < 5 || name.rfind(".gguf") != name.size() - 5 || is_mmproj(name))
            continue;
        const std::string leaf_lower = lower_ascii(media::path::path_to_utf8(media::path::path_from_utf8(name).filename()));
        if (variant_boundary_match(leaf_lower, variant_lower))
            matches.push_back(name);
    }
    std::sort(matches.begin(), matches.end());
    if (matches.empty()) {
        if (err_out)
            *err_out = "No GGUF file matching variant '" + variant + "'";
        return {};
    }

    const std::string primary = matches.front();
    const std::string leaf    = media::path::path_to_utf8(media::path::path_from_utf8(primary).filename());
    std::string prefix;
    int shard_total = 0;
    if (!parse_shard_name(leaf, prefix, shard_total))
        return {primary};

    std::vector<std::string> shards = {primary};
    for (size_t i = 1; i < matches.size(); ++i) {
        const std::string sib_leaf = media::path::path_to_utf8(media::path::path_from_utf8(matches[i]).filename());
        if (shard_name_matches(sib_leaf, prefix, shard_total))
            shards.push_back(matches[i]);
    }
    std::sort(shards.begin(), shards.end());
    shards.erase(std::unique(shards.begin(), shards.end()), shards.end());
    return shards;
}

std::optional<VariantInfo> build_variant_info(const std::vector<RepoFileInfo>& repo_files,
                                              const std::string& variant,
                                              std::string* err_out)
{
    const auto names = pick_gguf_filenames(repo_files, variant, err_out);
    if (names.empty())
        return std::nullopt;

    VariantInfo info;
    info.label = variant;
    for (const auto& name : names) {
        RepoFileInfo f;
        f.path = f.name = name;
        for (const auto& src : repo_files) {
            const std::string src_name = src.name.empty() ? src.path : src.name;
            if (src_name == name || media::path::path_to_utf8(media::path::path_from_utf8(src_name).filename()) == media::path::path_to_utf8(media::path::path_from_utf8(name).filename())) {
                f.size_bytes = src.size_bytes;
                f.lfs        = src.lfs;
                break;
            }
        }
        info.total_bytes += f.size_bytes;
        info.files.push_back(std::move(f));
    }
    return info;
}

std::vector<VariantInfo> all_variant_infos(const std::vector<RepoFileInfo>& repo_files)
{
    std::vector<VariantInfo> out;
    for (const auto& label : list_gguf_variants(repo_files)) {
        if (auto info = build_variant_info(repo_files, label))
            out.push_back(std::move(*info));
    }
    return out;
}

} // namespace pm::hf

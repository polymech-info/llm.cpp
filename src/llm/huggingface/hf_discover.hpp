#pragma once

#include "llm/huggingface/hf_hub_client.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "string/string_utils.hpp"

namespace pm::hf {

inline std::uint64_t shoehorn_overhead_bytes(std::uint64_t ctx)
{
    return (512ull << 20) + (320ull << 20) + ctx * 160ull * 1024ull;
}

inline std::uint64_t shoehorn_usable_bytes(double gpu_gb, bool metal, double sys_gb)
{
    constexpr double k_gib = static_cast<double>(1ull << 30);
    if (metal && sys_gb > 0.0)
        return static_cast<std::uint64_t>(sys_gb * 1e9 * 0.74);
    if (gpu_gb > 0.0)
        return static_cast<std::uint64_t>((std::max)(0.0, gpu_gb - 1.5) * k_gib);
    if (sys_gb > 0.0)
        return static_cast<std::uint64_t>(sys_gb * k_gib * 0.45);
    return 0;
}

inline const char* shoehorn_verdict(double bpw, bool* tight_out)
{
    if (tight_out)
        *tight_out = bpw < 4.5;
    if (bpw >= 16.0)
        return "runs at full precision";
    if (bpw >= 8.5)
        return "near-lossless (Q8-class)";
    if (bpw >= 6.0)
        return "excellent (Q6-class)";
    if (bpw >= 4.5)
        return "good (Q4/Q5-class)";
    if (bpw >= 3.0)
        return "tight - worthwhile for a big model";
    return "at the floor, heavily degraded";
}

inline std::optional<double> name_params_hint(const std::string& repo)
{
    std::optional<double> best;
    for (std::size_t i = 0; i < repo.size();) {
        const unsigned char c = static_cast<unsigned char>(repo[i]);
        if (std::isdigit(c) && (i == 0 || !std::isalpha(static_cast<unsigned char>(repo[i - 1])))) {
            const std::size_t start = i;
            while (i < repo.size()
                   && (std::isdigit(static_cast<unsigned char>(repo[i])) || repo[i] == '.'))
                ++i;
            if (i < repo.size() && (repo[i] == 'b' || repo[i] == 'B')
                && (i + 1 == repo.size() || !std::isalnum(static_cast<unsigned char>(repo[i + 1])))) {
                try {
                    const double v = std::stod(repo.substr(start, i - start));
                    best = best ? (std::max)(*best, v) : v;
                } catch (...) {
                }
            }
        } else {
            ++i;
        }
    }
    return best;
}

inline std::string file_basename_lower(const std::string& path)
{
    const auto slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    name = media::strutil::lower_ascii(std::move(name));
    return name;
}

inline bool select_source_files(const std::vector<RepoFileInfo>& files, std::uint64_t* bytes_out,
                                int* per_param_out)
{
    struct Cand {
        const RepoFileInfo* file = nullptr;
        std::string lower;
    };
    std::vector<Cand> usable;
    usable.reserve(files.size());
    for (const auto& file : files) {
        std::string lower = file.path;
        lower = media::strutil::lower_ascii(std::move(lower));
        if (lower.size() < 5 || lower.compare(lower.size() - 5, 5, ".gguf") != 0)
            continue;
        if (file_basename_lower(file.path).find("mmproj") != std::string::npos)
            continue;
        usable.push_back({&file, std::move(lower)});
    }
    auto is_draft = [](const Cand& c) {
        const std::string name = file_basename_lower(c.file->path);
        return name.find("draft") != std::string::npos || name.find("dspark") != std::string::npos;
    };
    auto pick_in = [](const std::vector<Cand>& pool, const char* tag) -> const Cand* {
        const Cand* best = nullptr;
        for (const auto& c : pool) {
            if (c.lower.find(tag) == std::string::npos)
                continue;
            if (std::string(tag) == "f16" && c.lower.find("bf16") != std::string::npos)
                continue;
            if (!best || c.file->size_bytes > best->file->size_bytes)
                best = &c;
        }
        return best;
    };
    auto tiers = [&](const std::vector<Cand>& pool) -> const Cand* {
        if (const Cand* c = pick_in(pool, "bf16"))
            return c;
        if (const Cand* c = pick_in(pool, "f16"))
            return c;
        return pick_in(pool, "f32");
    };
    std::vector<Cand> clean;
    for (const auto& c : usable) {
        if (!is_draft(c))
            clean.push_back(c);
    }
    const Cand* best = tiers(clean);
    if (!best)
        best = tiers(usable);
    if (!best || !best->file || best->file->size_bytes == 0)
        return false;

    std::uint64_t total = best->file->size_bytes;
    const auto of = best->file->path.find("-of-");
    if (of != std::string::npos) {
        const auto dash = best->file->path.rfind('-', of > 0 ? of - 1 : 0);
        if (dash != std::string::npos) {
            const std::string prefix = best->file->path.substr(0, dash);
            total = 0;
            for (const auto& c : usable) {
                if (c.file->path.rfind(prefix, 0) == 0 && c.file->path.find("-of-") != std::string::npos)
                    total += c.file->size_bytes;
            }
        }
    }
    const bool is_f32 = best->lower.find("f32") != std::string::npos
        && best->lower.find("f16") == std::string::npos;
    if (bytes_out)
        *bytes_out = total;
    if (per_param_out)
        *per_param_out = is_f32 ? 4 : 2;
    return total > 0;
}

inline nlohmann::json analyze_discover_repo(const std::string& repo_id,
                                            const std::vector<RepoFileInfo>& files,
                                            std::uint64_t weight_budget)
{
    std::uint64_t src_bytes = 0;
    int per_param = 2;
    if (!select_source_files(files, &src_bytes, &per_param) || src_bytes == 0)
        return nullptr;
    const double params = static_cast<double>(src_bytes) / static_cast<double>(per_param);
    if (const auto hint = name_params_hint(repo_id)) {
        if (params / 1e9 < *hint * 0.3)
            return nullptr;
    }
    const double bpw = (std::min)((static_cast<double>(weight_budget) * 8.0) / params, 16.0);
    if (bpw < 2.06)
        return nullptr;

    const RepoFileInfo* native = nullptr;
    for (const auto& file : files) {
        const std::string name = file_basename_lower(file.path);
        std::string lower = file.path;
        lower = media::strutil::lower_ascii(std::move(lower));
        const double file_bpw = (static_cast<double>(file.size_bytes) * 8.0) / params;
        if (lower.size() < 5 || lower.compare(lower.size() - 5, 5, ".gguf") != 0)
            continue;
        if (name.find("mmproj") != std::string::npos || name.find("draft") != std::string::npos
            || name.find("dspark") != std::string::npos)
            continue;
        if (file_bpw < 0.8 || file_bpw >= 1.5 || file.size_bytes > weight_budget)
            continue;
        if (!native || file.size_bytes < native->size_bytes)
            native = &file;
    }

    bool tight = false;
    const char* verdict = shoehorn_verdict(bpw, &tight);
    nlohmann::json row = {
        {"repoId", repo_id},
        {"paramsB", params / 1e9},
        {"bpw", bpw},
        {"verdict", verdict},
        {"tight", tight},
    };
    if (native) {
        const auto slash = native->path.find_last_of("/\\");
        const std::string name =
            slash == std::string::npos ? native->path : native->path.substr(slash + 1);
        const double gib = static_cast<double>(native->size_bytes) / static_cast<double>(1ull << 30);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "also ships %s (%.1f GiB QAT)", name.c_str(), gib);
        row["native"] = buf;
    }
    return row;
}

inline nlohmann::json discover_popular_gguf(double gpu_gb, bool metal, double sys_gb, int ctx, int scan,
                                           const std::string& token = {})
{
    const std::uint64_t usable = shoehorn_usable_bytes(gpu_gb, metal, sys_gb);
    const int context = ctx > 0 ? ctx : 8192;
    const std::uint64_t weight_budget = usable > shoehorn_overhead_bytes(static_cast<std::uint64_t>(context))
        ? usable - shoehorn_overhead_bytes(static_cast<std::uint64_t>(context))
        : 0;
    nlohmann::json out = {
        {"usableGb", static_cast<double>(usable) / static_cast<double>(1ull << 30)},
        {"weightBudgetGb", static_cast<double>(weight_budget) / static_cast<double>(1ull << 30)},
        {"contextTokens", context},
        {"scanned", 0},
        {"suggestions", nlohmann::json::array()},
    };
    if (weight_budget == 0)
        return out;

    const HubRequestResult list = fetch_popular_gguf_models(scan > 0 ? scan : 40, token);
    if (!list.ok || !list.json.is_array()) {
        throw std::runtime_error(list.err.empty() ? "Hugging Face catalog request failed" : list.err);
    }

    std::vector<std::string> ids;
    for (const auto& row : list.json) {
        if (!row.is_object())
            continue;
        const std::string id = row.value("id", std::string{});
        if (!id.empty())
            ids.push_back(id);
    }
    out["scanned"] = static_cast<int>(ids.size());

    std::mutex mu;
    std::vector<nlohmann::json> suggestions;
    std::atomic<std::size_t> next{0};
    auto worker = [&]() {
        while (true) {
            const std::size_t i = next.fetch_add(1);
            if (i >= ids.size())
                return;
            std::string err;
            const auto files = list_repo_files(ids[i], token, &err);
            if (files.empty())
                continue;
            nlohmann::json row = analyze_discover_repo(ids[i], files, weight_budget);
            if (!row.is_object())
                continue;
            std::lock_guard<std::mutex> lock(mu);
            suggestions.push_back(std::move(row));
        }
    };

    const int lanes = (std::min)(8, (std::max)(1, static_cast<int>(ids.size())));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(lanes));
    for (int i = 0; i < lanes; ++i)
        threads.emplace_back(worker);
    for (auto& t : threads)
        t.join();

    std::sort(suggestions.begin(), suggestions.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        const auto tier = [](const nlohmann::json& row) {
            const double bpw = row.value("bpw", 0.0);
            if (bpw >= 4.5)
                return 2;
            if (bpw >= 3.0)
                return 1;
            return 0;
        };
        const int ta = tier(a);
        const int tb = tier(b);
        if (ta != tb)
            return ta > tb;
        return a.value("paramsB", 0.0) > b.value("paramsB", 0.0);
    });
    if (suggestions.size() > 10)
        suggestions.resize(10);
    out["suggestions"] = std::move(suggestions);
    return out;
}

} // namespace pm::hf

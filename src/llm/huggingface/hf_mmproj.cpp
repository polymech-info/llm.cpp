#include "llm/huggingface/hf_mmproj.hpp"

#include <algorithm>
#include <cctype>
#include "string/string_utils.hpp"

namespace pm::hf {
namespace {

using media::strutil::lower_ascii;

bool is_mmproj_name(const std::string& name)
{
    const std::string lower = lower_ascii(name);
    return lower.find("mmproj") != std::string::npos;
}

int mmproj_rank(const std::string& name)
{
    const std::string lower = lower_ascii(name);
    if (lower.find("f16") != std::string::npos || lower.find("fp16") != std::string::npos)
        return 0;
    if (lower.find("bf16") != std::string::npos)
        return 1;
    if (lower.find("q8") != std::string::npos)
        return 2;
    if (lower.find("q4") != std::string::npos)
        return 3;
    return 4;
}

} // namespace

RepoCapabilities detect_repo_capabilities(const std::vector<RepoFileInfo>& repo_files)
{
    RepoCapabilities caps;
    for (const auto& f : repo_files) {
        const std::string name = f.name.empty() ? f.path : f.name;
        if (name.size() < 5 || name.rfind(".gguf") != name.size() - 5)
            continue;
        if (is_mmproj_name(name)) {
            caps.has_vision = true;
            caps.mmproj_files.push_back(f);
        }
    }
    if (!caps.mmproj_files.empty()) {
        auto best = caps.mmproj_files.front();
        int best_rank = mmproj_rank(best.name.empty() ? best.path : best.name);
        for (const auto& f : caps.mmproj_files) {
            const std::string name = f.name.empty() ? f.path : f.name;
            const int rank = mmproj_rank(name);
            if (rank < best_rank) {
                best = f;
                best_rank = rank;
            }
        }
        caps.recommended_mmproj = best;
    }
    return caps;
}

} // namespace pm::hf

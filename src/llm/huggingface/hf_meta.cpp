#include "llm/huggingface/hf_meta.hpp"

#include "llm/huggingface/hf_gguf.hpp"
#include "llm/huggingface/hf_gguf_header.hpp"
#include "llm/huggingface/hf_mmproj.hpp"
#include "llm/huggingface/hf_quant.hpp"
#include "llm/huggingface/hf_sidecar.hpp"
#include "llm/huggingface/hf_vram.hpp"
#include "fs/path_sanitizer.hpp"

namespace fs = std::filesystem;

namespace pm::hf {

void enrich_variant_info(VariantInfo& variant, const GgufHeaderMeta* local_header)
{
    if (is_spurious_variant_label(variant.label))
        return;

    variant.quant_bits = quant_bits_from_label(variant.label);
    variant.size_gb    = bytes_to_decimal_gb(variant.total_bytes);
    variant.sharded    = variant.files.size() > 1;

    VramEstimate est;
    if (local_header && local_header->valid && !local_header->is_mmproj)
        est = estimate_vram(*local_header, variant.total_bytes, variant.vram_context_assumed);
    else
        est = estimate_vram_heuristic(variant.total_bytes, variant.vram_context_assumed);

    variant.kv_cache_bytes_est   = est.kv_cache_bytes_est;
    variant.total_vram_bytes_est = est.total_bytes_est;
    variant.total_vram_gb_est    = est.total_gb_est;
    variant.vram_heuristic       = est.heuristic;
}

void enrich_probe_result(ProbeResult& probe, const std::vector<RepoFileInfo>& repo_files)
{
    probe.repo = detect_repo_capabilities(repo_files);

    std::vector<VariantInfo> filtered;
    std::vector<std::string> labels;
    filtered.reserve(probe.variant_infos.size());
    labels.reserve(probe.variant_infos.size());

    for (auto& variant : probe.variant_infos) {
        if (is_spurious_variant_label(variant.label))
            continue;
        enrich_variant_info(variant);
        filtered.push_back(std::move(variant));
        labels.push_back(filtered.back().label);
    }

    probe.variant_infos = std::move(filtered);
    probe.variants      = std::move(labels);
    probe.summary       = build_probe_summary(probe, probe.repo);
}

GgufHeaderMeta read_header_for_variant_dir(const fs::path& variant_dir)
{
    std::error_code ec;
    if (!fs::is_directory(variant_dir, ec))
        return {};

    GgufHeaderMeta best;
    for (fs::directory_iterator it(variant_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec)
            continue;
        const fs::path p = it->path();
        const std::string name = media::path::path_to_utf8(p.filename());
        if (name.size() < 6 || name.rfind(".gguf") != name.size() - 5)
            continue;
        if (name.find("mmproj") != std::string::npos || name.find("MMProj") != std::string::npos)
            continue;
        const GgufHeaderMeta header = read_gguf_header(p);
        if (header.valid)
            return header;
        if (best.error.empty())
            best = header;
    }
    return best;
}

} // namespace pm::hf

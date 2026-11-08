#include "llm/huggingface/hf_sidecar.hpp"

#include "llm/huggingface/hf_manifest.hpp"
#include "llm/huggingface/hf_meta.hpp"
#include "llm/huggingface/hf_mmproj.hpp"
#include "llm/huggingface/hf_quant.hpp"
#include "llm/huggingface/hf_vram.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include "fs/path_sanitizer.hpp"

namespace fs = std::filesystem;

namespace pm::hf {
namespace {

std::string utc_now_iso()
{
    const auto now = std::chrono::system_clock::now();
    const auto t   = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

nlohmann::json file_info_json(const RepoFileInfo& f)
{
    return {
        {"path", f.path},
        {"name", f.name},
        {"bytes", f.size_bytes},
        {"sizeGb", bytes_to_decimal_gb(f.size_bytes)},
        {"lfs", f.lfs},
    };
}

std::vector<std::string> weight_paths(const VariantInfo& variant)
{
    std::vector<std::string> out;
    for (const auto& f : variant.files) {
        const std::string name = f.name.empty() ? f.path : f.name;
        if (!name.empty())
            out.push_back(media::path::path_to_utf8(media::path::path_from_utf8(name).filename()));
    }
    return out;
}

std::vector<std::string> inference_slots(const RepoCapabilities& caps, const GgufHeaderMeta& header)
{
    if (header.is_mmproj || caps.has_vision)
        return {"vision", "image", "video", "ocr"};
    return {"text", "planner"};
}

} // namespace

nlohmann::json gguf_header_to_json(const GgufHeaderMeta& header)
{
    if (!header.valid)
        return nullptr;
    return nlohmann::json{
        {"architecture", header.architecture},
        {"generalType", header.general_type},
        {"basename", header.basename},
        {"organization", header.organization},
        {"baseModelRepoUrl", header.base_model_repo_url},
        {"contextLength", header.context_length},
        {"blockCount", header.block_count},
        {"nKvHeads", header.n_kv_heads},
        {"nHeads", header.n_heads},
        {"embeddingLength", header.embedding_length},
        {"kvKeyLength", header.kv_key_length},
        {"kvValueLength", header.kv_value_length},
        {"kvLoraRank", header.kv_lora_rank},
        {"slidingWindow", header.sliding_window},
        {"hasChatTemplate", header.has_chat_template},
        {"chatTemplateChars", header.chat_template_chars},
        {"isMmproj", header.is_mmproj},
    };
}

nlohmann::json vram_estimate_to_json(const VramEstimate& est)
{
    return {
        {"weightsBytes", est.weights_bytes},
        {"kvCacheBytesEst", est.kv_cache_bytes_est},
        {"totalBytesEst", est.total_bytes_est},
        {"totalGbEst", est.total_gb_est},
        {"contextAssumed", est.context_assumed},
        {"heuristic", est.heuristic},
        {"note", est.heuristic ? "rough heuristic; read GGUF header after download for better estimate"
                               : "architecture-based KV estimate"},
    };
}

nlohmann::json variant_info_to_json(const VariantInfo& variant)
{
    nlohmann::json files = nlohmann::json::array();
    for (const auto& f : variant.files)
        files.push_back(file_info_json(f));
    return {
        {"label", variant.label},
        {"displayLabel", display_quant_label(variant.label)},
        {"bits", variant.quant_bits},
        {"totalBytes", variant.total_bytes},
        {"sizeGb", variant.size_gb},
        {"sharded", variant.sharded},
        {"files", files},
        {"vram", {
            {"kvCacheBytesEst", variant.kv_cache_bytes_est},
            {"totalBytesEst", variant.total_vram_bytes_est},
            {"totalGbEst", variant.total_vram_gb_est},
            {"contextAssumed", variant.vram_context_assumed},
            {"heuristic", variant.vram_heuristic},
        }},
    };
}

nlohmann::json repo_capabilities_to_json(const RepoCapabilities& caps)
{
    nlohmann::json mmproj = nlohmann::json::array();
    for (const auto& f : caps.mmproj_files)
        mmproj.push_back(file_info_json(f));
    nlohmann::json out = {
        {"hasVision", caps.has_vision},
        {"mmprojFiles", mmproj},
    };
    if (caps.recommended_mmproj)
        out["recommendedMmproj"] = file_info_json(*caps.recommended_mmproj);
    return out;
}

nlohmann::json llama_server_hints_to_json(const VariantInfo& variant, const RepoCapabilities& caps,
                                          const GgufHeaderMeta& header)
{
    nlohmann::json args = {
        {"-c", header.context_length > 0 ? static_cast<std::int64_t>(header.context_length) : 8192},
        {"-ngl", -1},
        {"--flash-attn", "on"},
        {"--no-context-shift", true},
        {"--threads", -1},
        {"--jinja", true},
    };
    if (variant.total_vram_bytes_est > 12'000'000'000ULL)
        args["--fit"] = "on";

    nlohmann::json out = nlohmann::json::object();
    out["backend"]          = "llama-server";
    out["suggestedArgs"]    = std::move(args);
    out["needsMmproj"]      = caps.has_vision;
    if (caps.recommended_mmproj) {
        const std::string mmproj_name = caps.recommended_mmproj->name.empty()
            ? caps.recommended_mmproj->path
            : caps.recommended_mmproj->name;
        out["mmprojArg"] = media::path::path_to_utf8(media::path::path_from_utf8(mmproj_name).filename());
    } else {
        out["mmprojArg"] = nullptr;
    }
    out["openAiBasePath"]   = "/v1";
    out["managedDenylist"]  = nlohmann::json::array({"-m", "--mmproj", "--port", "--host"});
    if (header.has_chat_template)
        out["chatTemplateKwargs"] = nlohmann::json::object();
    return out;
}

nlohmann::json build_probe_summary(const ProbeResult& probe, const RepoCapabilities& caps)
{
    nlohmann::json variants = nlohmann::json::array();
    for (const auto& v : probe.variant_infos)
        variants.push_back(variant_info_to_json(v));

    nlohmann::json hf_gguf = nullptr;
    if (probe.model_info.contains("gguf") && probe.model_info["gguf"].is_object())
        hf_gguf = probe.model_info["gguf"];

    return {
        {"schema", k_local_sidecar_schema},
        {"repoId", probe.entry.repo_id},
        {"storeName", probe.entry.store_name},
        {"defaultVariant", probe.entry.default_variant},
        {"repo", repo_capabilities_to_json(caps)},
        {"variants", variants},
        {"hfGgufApi", hf_gguf},
        {"pipelineTag", probe.model_info.value("pipeline_tag", std::string{})},
    };
}

nlohmann::json build_sidecar(const ModelEntry& entry, const std::string& variant, const VariantInfo& weights,
                             const RepoCapabilities& caps, const GgufHeaderMeta& header,
                             const nlohmann::json& hf_model_info)
{
    const auto slots = inference_slots(caps, header);
    const VramEstimate vram = header.valid && !header.is_mmproj
        ? estimate_vram(header, weights.total_bytes, weights.vram_context_assumed)
        : estimate_vram_heuristic(weights.total_bytes, weights.vram_context_assumed);

    nlohmann::json mmproj_file = nullptr;
    if (caps.recommended_mmproj) {
        const std::string mmproj_name = caps.recommended_mmproj->name.empty()
            ? caps.recommended_mmproj->path
            : caps.recommended_mmproj->name;
        mmproj_file = media::path::path_to_utf8(media::path::path_from_utf8(mmproj_name).filename());
    }

    nlohmann::json sidecar = nlohmann::json::object();
    sidecar["schema"]       = k_local_sidecar_schema;
    sidecar["generated_at"] = utc_now_iso();
    sidecar["repo_id"]      = entry.repo_id;
    sidecar["store_name"]   = entry.store_name;
    sidecar["variant"]      = variant;
    sidecar["format"]       = entry.format;
    sidecar["description"]  = entry.description;

    nlohmann::json inference = nlohmann::json::object();
    inference["runtime"] = "llama.cpp";
    inference["backend"] = "llama-server";
    inference["slots"]   = slots;
    inference["task"]    = caps.has_vision ? "vision" : "chat";
    sidecar["inference"] = std::move(inference);

    nlohmann::json quant = nlohmann::json::object();
    quant["label"]         = variant;
    quant["display_label"] = display_quant_label(variant);
    quant["bits"]          = weights.quant_bits;
    quant["size_bytes"]    = weights.total_bytes;
    quant["size_gb"]       = weights.size_gb;
    quant["sharded"]       = weights.sharded;
    sidecar["quant"]       = std::move(quant);

    nlohmann::json files = nlohmann::json::object();
    files["weights"]     = weight_paths(weights);
    files["mmproj"]      = mmproj_file;
    sidecar["files"]     = std::move(files);

    sidecar["vision"]       = repo_capabilities_to_json(caps);
    sidecar["gguf"]         = gguf_header_to_json(header);
    sidecar["vram"]         = vram_estimate_to_json(vram);
    sidecar["llama_server"] = llama_server_hints_to_json(weights, caps, header);

    nlohmann::json hf = nlohmann::json::object();
    hf["pipeline_tag"] = hf_model_info.value("pipeline_tag", std::string{});
    hf["model_id"]     = hf_model_info.value("id", entry.repo_id);
    sidecar["hf"]      = std::move(hf);

    if (!entry.alias.empty())
        sidecar["alias"] = entry.alias;
    return sidecar;
}

fs::path write_sidecar(const ModelEntry& entry, const fs::path& output_dir, const std::string& variant,
                       const VariantInfo& weights, const RepoCapabilities& caps, const GgufHeaderMeta& header,
                       const nlohmann::json& hf_model_info)
{
    const nlohmann::json sidecar = build_sidecar(entry, variant, weights, caps, header, hf_model_info);
    const fs::path sidecar_path  = output_dir / k_local_sidecar_filename;
    std::error_code ec;
    fs::create_directories(output_dir, ec);
    std::ofstream out(sidecar_path, std::ios::binary | std::ios::trunc);
    out << sidecar.dump(2) << '\n';
    return sidecar_path;
}

fs::path rebuild_sidecar_from_dir(const fs::path& variant_dir, const ModelEntry& entry, const std::string& variant)
{
    std::error_code ec;
    if (!fs::is_directory(variant_dir, ec))
        return {};

    std::vector<RepoFileInfo> local_files;
    std::vector<RepoFileInfo> mmproj_local;
    VariantInfo weights;
    weights.label = variant;

    for (fs::directory_iterator it(variant_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec)
            continue;
        const fs::path p = it->path();
        const std::string name = media::path::path_to_utf8(p.filename());
        if (name.size() < 6 || name.rfind(".gguf") != name.size() - 5)
            continue;
        RepoFileInfo f;
        f.path = f.name = name;
        f.size_bytes    = static_cast<std::uint64_t>(fs::file_size(p, ec));
        if (name.find("mmproj") != std::string::npos) {
            mmproj_local.push_back(std::move(f));
            continue;
        }
        local_files.push_back(f);
        weights.files.push_back(std::move(f));
        weights.total_bytes += weights.files.back().size_bytes;
    }

    if (local_files.empty())
        return {};

    enrich_variant_info(weights);
    const GgufHeaderMeta header = read_header_for_variant_dir(variant_dir);
    if (header.valid && !header.is_mmproj)
        enrich_variant_info(weights, &header);

    std::vector<RepoFileInfo> caps_files = local_files;
    caps_files.insert(caps_files.end(), mmproj_local.begin(), mmproj_local.end());
    const RepoCapabilities caps = detect_repo_capabilities(caps_files);
    return write_sidecar(entry, variant_dir, variant, weights, caps, header);
}

} // namespace pm::hf

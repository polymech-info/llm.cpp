#pragma once

#include "llm/huggingface/hf_gguf_header.hpp"
#include "llm/huggingface/hf_types.hpp"
#include "llm/huggingface/hf_vram.hpp"

#include <nlohmann/json.hpp>

namespace pm::hf {

inline constexpr const char* k_local_sidecar_filename = "model.local.json";
inline constexpr const char* k_local_sidecar_schema   = "pixlwiz.hg.local/1";

nlohmann::json gguf_header_to_json(const GgufHeaderMeta& header);
nlohmann::json vram_estimate_to_json(const VramEstimate& est);
nlohmann::json variant_info_to_json(const VariantInfo& variant);
nlohmann::json repo_capabilities_to_json(const RepoCapabilities& caps);
nlohmann::json llama_server_hints_to_json(const VariantInfo& variant, const RepoCapabilities& caps,
                                          const GgufHeaderMeta& header);

nlohmann::json build_probe_summary(const ProbeResult& probe, const RepoCapabilities& caps);

nlohmann::json build_sidecar(const ModelEntry& entry, const std::string& variant, const VariantInfo& weights,
                             const RepoCapabilities& caps, const GgufHeaderMeta& header,
                             const nlohmann::json& hf_model_info = nlohmann::json::object());

std::filesystem::path write_sidecar(const ModelEntry& entry, const std::filesystem::path& output_dir,
                                      const std::string& variant, const VariantInfo& weights,
                                      const RepoCapabilities& caps, const GgufHeaderMeta& header,
                                      const nlohmann::json& hf_model_info = nlohmann::json::object());

std::filesystem::path rebuild_sidecar_from_dir(const std::filesystem::path& variant_dir, const ModelEntry& entry,
                                               const std::string& variant);

} // namespace pm::hf

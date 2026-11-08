#include "llm/huggingface/hf_catalog.hpp"

#include "core/settings_store.hpp"

#include <algorithm>
#include <cctype>
#include "fs/path_sanitizer.hpp"

namespace fs = std::filesystem;

namespace pm::hf {
namespace {

std::vector<ModelEntry> builtin_catalog()
{
    return {
        {
            "qwen2.5-7b-instruct",
            "bartowski/Qwen2.5-7B-Instruct-GGUF",
            "Qwen2.5-7B-Instruct",
            "gguf",
            "Q4_K_M",
            "Qwen 2.5 7B Instruct GGUF for llama.cpp",
        },
        {
            "qwen2.5-7b",
            "bartowski/Qwen2.5-7B-GGUF",
            "Qwen2.5-7B",
            "gguf",
            "Q4_K_M",
            "Qwen 2.5 7B base GGUF for llama.cpp",
        },
        {
            "deepfilternet3",
            "Rikorose/DeepFilterNet",
            "deepfilter",
            "onnx-tar",
            "DeepFilterNet3_onnx.tar.gz",
            "DeepFilterNet3 48 kHz ONNX tar for in-process libDF",
            "https://github.com/Rikorose/DeepFilterNet/raw/main/models/DeepFilterNet3_onnx.tar.gz",
        },
        {
            "gtcrn",
            "Xiaobin-Rong/gtcrn",
            "gtcrn",
            "onnx",
            "gtcrn_simple.onnx",
            "GTCRN 16 kHz streaming ONNX for RealtimeVoiceSession",
            "https://github.com/Xiaobin-Rong/gtcrn/raw/main/stream/onnx_models/gtcrn_simple.onnx",
        },
        {
            "moss-tts-nano-model-source",
            "OpenMOSS-Team/MOSS-TTS-Nano-100M",
            "moss-tts-nano",
            "pytorch-bin",
            "pytorch_model.bin",
            "Official MOSS-TTS-Nano weights (convert with npm run download:moss)",
        },
        {
            "moss-tts-nano-codec-source",
            "OpenMOSS-Team/MOSS-Audio-Tokenizer-Nano",
            "moss-tts-nano",
            "safetensors",
            "model-00001-of-00001.safetensors",
            "Official MOSS Audio Tokenizer Nano weights (convert with npm run download:moss)",
        },
        {
            "moss-tts-nano-tokenizer-source",
            "OpenMOSS-Team/MOSS-TTS-Nano-100M",
            "moss-tts-nano",
            "sentencepiece",
            "tokenizer.model",
            "Official MOSS-TTS-Nano SentencePiece source (convert to moss-nano-tokenizer.gguf)",
        },
    };
}

} // namespace

std::vector<ModelEntry> catalog_entries()
{
    auto entries = builtin_catalog();
    std::sort(entries.begin(), entries.end(),
              [](const ModelEntry& a, const ModelEntry& b) { return a.alias < b.alias; });
    return entries;
}

fs::path default_models_root()
{
    return media::settings::get_models_dir();
}

fs::path resolve_models_root(const std::string& override_dir)
{
    if (!override_dir.empty())
        return fs::absolute(media::path::path_from_utf8(override_dir));
    return default_models_root();
}

fs::path resolve_output_dir(const fs::path& output_root, const ModelEntry& entry, const std::string& variant)
{
    const fs::path root = output_root.empty() ? default_models_root() : fs::absolute(output_root);
    return (root / entry.store_name / variant).lexically_normal();
}

} // namespace pm::hf

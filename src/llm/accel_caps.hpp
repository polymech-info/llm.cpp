#pragma once

#include "core/app_exe_directory.hpp"
#include "core/library_utils.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include "fs/path_sanitizer.hpp"

namespace pm::llm {

/// Same acceleration probe as `llm info` — CPU llama.cpp plus optional GPU/ORT backends.
struct AccelCaps {
    bool cpu        = true;   // always — llama.cpp CPU embedded
    bool metal      = false;  // ggml-metal (Apple Silicon GPU, macOS only)
    bool nvcuda     = false;  // nvcuda.dll / libcuda.so → NVIDIA driver installed
    bool llama_cuda = false;  // ggml-cuda beside exe, or statically linked (FEATURE_LLAMA_CUDA)
    bool vulkan     = false;  // Vulkan loader/runtime available
    bool llama_vk   = false;  // llama Vulkan backend library present
    bool ort_cpu    = false;  // onnxruntime → ONNX Runtime CPU EP
    bool ort_cuda   = false;  // onnxruntime_providers_cuda → ORT CUDA EP
};

inline bool load_backend(const char* name, const std::string& search_dir) noexcept
{
    // Pin the module for process lifetime. ggml-base.dll installs a
    // process-global std::terminate handler in static init; FreeLibrary
    // then a later LoadLibrary re-runs that init and aborts:
    //   GGML_ASSERT(prev != ggml_uncaught_exception)
    struct Pin {
        void* handle = nullptr;
        bool ok = false;
    };
    static std::mutex mu;
    static std::unordered_map<std::string, Pin> pins;
    const std::string key = search_dir + '\n' + (name ? name : "");
    std::lock_guard<std::mutex> lock(mu);
    const auto it = pins.find(key);
    if (it != pins.end())
        return it->second.ok;

    Pin pin;
    pin.handle = media::library::load(name, search_dir);
    if (!pin.handle && !search_dir.empty())
        pin.handle = media::library::load(name);
    pin.ok = pin.handle != nullptr;
    pins.emplace(key, pin);
    return pin.ok;
}

inline bool backend_file_beside_exe(const char* name, const std::filesystem::path& exe_dir) noexcept
{
    if (exe_dir.empty())
        return false;
    std::error_code ec;
    return std::filesystem::exists(exe_dir / name, ec);
}

inline AccelCaps probe_acceleration_caps() noexcept
{
    AccelCaps c;
    using media::library::probe;
    const std::filesystem::path exe_dir = media::app::exe_parent_directory();
    const std::string exe_dir_u8 = exe_dir.empty() ? std::string{} : media::path::path_to_utf8(exe_dir);
    const std::string llama_plugin_dir = exe_dir.empty() ? std::string{} : media::path::path_to_utf8(exe_dir / "plugins" / "llama");
    const std::string whisper_plugin_dir = exe_dir.empty() ? std::string{} : media::path::path_to_utf8(exe_dir / "plugins" / "whisper");
    const std::string vibevoice_plugin_dir = exe_dir.empty() ? std::string{} : media::path::path_to_utf8(exe_dir / "plugins" / "vibevoice");
    const std::string onnx_plugin_dir = exe_dir.empty() ? std::string{} : media::path::path_to_utf8(exe_dir / "plugins" / "onnx");
#if defined(FEATURE_LLAMA) && FEATURE_LLAMA
#if defined(_WIN32)
    c.nvcuda = probe("nvcuda.dll");
    // LOAD_LIBRARY_SEARCH_DEFAULT_DIRS does not find GGML_BACKEND_DL modules
    // beside the exe (and their statically-linked CUDA deps). Load from the
    // exe directory with LOAD_WITH_ALTERED_SEARCH_PATH.
    c.llama_cuda = load_backend("ggml-cuda.dll", exe_dir_u8)
                || load_backend("ggml-cuda.dll", llama_plugin_dir)
                || load_backend("ggml-cuda.dll", whisper_plugin_dir)
                || load_backend("ggml-cuda.dll", vibevoice_plugin_dir);
    c.vulkan   = probe("vulkan-1.dll");
    c.llama_vk = load_backend("ggml-vulkan.dll", exe_dir_u8)
              || load_backend("ggml-vulkan.dll", llama_plugin_dir)
              || load_backend("ggml-vulkan.dll", whisper_plugin_dir)
              || load_backend("ggml-vulkan.dll", vibevoice_plugin_dir)
              || probe("ggml-vulkan.dll");
#elif defined(__APPLE__)
    // Metal is always present on Apple Silicon when llama.cpp is built with
    // GGML_METAL=ON (the default for our macOS prebuilt).
    c.metal = true;
#else
    c.nvcuda     = probe("libcuda.so") || probe("libcuda.so.1");
    c.llama_cuda = load_backend("ggml-cuda.so", exe_dir_u8) || probe("ggml-cuda.so");
    c.vulkan     = probe("libvulkan.so") || probe("libvulkan.so.1");
    c.llama_vk   = load_backend("ggml-vulkan.so", exe_dir_u8) || probe("ggml-vulkan.so");
#endif
#if defined(FEATURE_LLAMA_CUDA) && FEATURE_LLAMA_CUDA
    // Static / compiled-in CUDA: no standalone ggml-cuda shared object.
    // Require the NVIDIA driver so CPU-only machines are not reported as CUDA.
    if (!c.llama_cuda && c.nvcuda && !backend_file_beside_exe(
#if defined(_WIN32)
            "ggml-cuda.dll"
#else
            "ggml-cuda.so"
#endif
            , exe_dir))
        c.llama_cuda = true;
#endif
#if defined(FEATURE_LLAMA_VULKAN) && FEATURE_LLAMA_VULKAN
    // Linux/Windows static path merges ggml-vulkan into libggml, so there may be
    // no standalone ggml-vulkan shared object to probe.
    c.llama_vk = true;
#endif
#endif

#if defined(FEATURE_ONNX) && FEATURE_ONNX
#if defined(_WIN32)
    // ORT provider DLLs must never be loaded via load_backend() during capability
    // detection. onnxruntime_providers_cuda.dll's DllMain reaches into CUDA at load
    // time and crashes (null-deref) when the CUDA context is not yet initialised.
    // Use probe() (LOAD_LIBRARY_AS_DATAFILE — no DllMain, no code execution) or a
    // plain filesystem check for all ORT provider presence tests.
    c.ort_cpu  = probe("onnxruntime.dll")
              || backend_file_beside_exe("onnxruntime.dll", exe_dir)
              || backend_file_beside_exe("onnxruntime.dll", media::path::path_from_utf8(onnx_plugin_dir));
    c.ort_cuda = c.ort_cpu && (probe("onnxruntime_providers_cuda.dll")
                            || backend_file_beside_exe("onnxruntime_providers_cuda.dll", exe_dir)
                            || backend_file_beside_exe("onnxruntime_providers_cuda.dll", media::path::path_from_utf8(onnx_plugin_dir)));
#elif defined(__APPLE__)
    {
        namespace fs = std::filesystem;
        c.ort_cpu = fs::exists("/opt/homebrew/lib/libonnxruntime.dylib")
                 || fs::exists("/usr/local/lib/libonnxruntime.dylib")
                 || probe("libonnxruntime.dylib");
    }
#else
    c.ort_cpu  = probe("libonnxruntime.so") || probe("libonnxruntime.so.1");
    // probe() uses RTLD_LAZY|RTLD_LOCAL for existence check without executing init code.
    c.ort_cuda = c.ort_cpu && (probe("libonnxruntime_providers_cuda.so")
                            || backend_file_beside_exe("libonnxruntime_providers_cuda.so", exe_dir));
#endif
    // Import-linked / static ORT: if the process started, the CPU EP is present.
    if (!c.ort_cpu)
        c.ort_cpu = true;
#if defined(FEATURE_ONNX_CUDA) && FEATURE_ONNX_CUDA
    if (!c.ort_cuda && c.nvcuda && !backend_file_beside_exe(
#if defined(_WIN32)
            "onnxruntime_providers_cuda.dll"
#else
            "libonnxruntime_providers_cuda.so"
#endif
            , exe_dir))
        c.ort_cuda = true;
#endif
#endif
    return c;
}

inline nlohmann::json accel_caps_json(const AccelCaps& c)
{
    return {
        {"cpu_llama",     c.cpu},
        {"metal",         c.metal},
        {"nvcuda",        c.nvcuda},
        {"llama_cuda",    c.llama_cuda},
        {"vulkan",        c.vulkan},
        {"llama_vulkan",  c.llama_vk},
        {"ort_cpu",       c.ort_cpu},
        {"ort_cuda",      c.ort_cuda},
    };
}

} // namespace pm::llm

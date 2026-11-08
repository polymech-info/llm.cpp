For a **desktop app targeting users with 8–16 GB VRAM**, I'd focus on a few categories rather than trying to support every model on Hugging Face.

---

## Soft DLL Loading — Investigation Notes

### Decision

**Ship `onnxruntime.dll` (CPU) in the core app.  Never ship CUDA DLLs in the core app.**

Failed CUDA deps must mean **"CUDA disabled"**, not app failure.  The app starts CPU-only; GPU backends are attempted at runtime and silently dropped on any error.

```
app starts CPU-only
  ↓
try LoadLibraryExW("onnxruntime.dll")     → ORT CPU enabled, or "ORT unavailable"
try LoadLibraryExW("ggml-cuda.dll")       → llama CUDA enabled, or fall back to CPU/Vulkan
try ORT CUDA EP probe                     → ORT CUDA enabled, or fall back to ORT CPU
  ↓
report final capability state in Settings → Acceleration
```

```
Settings → Acceleration
CPU:        ✓ available
Vulkan:     ✓ available
ONNX CPU:   ✓ available
CUDA:       ✗ GPU pack not installed        → [Download GPU pack]
ONNX CUDA:  ✗ cuDNN version mismatch
```

### What ships where

| Component | Core install | GPU pack (download) |
|-----------|-------------|---------------------|
| llama.cpp CPU (static) | ✓ | |
| llama.cpp Vulkan | ✓ if stable | |
| `onnxruntime.dll` CPU | ✓ soft-loaded | |
| `ggml-cuda.dll` | | ✓ `GGML_BACKEND_DL` |
| `onnxruntime_providers_cuda.dll` | | ✓ |
| CUDA runtime (`cudart64_NN.dll`, `cublas64_NN.dll`) | | ✓ versioned |
| cuDNN | | ✓ version-matched to ORT |

### Current hard PE imports — the problem

Both of these are linked as hard PE imports today.  The Windows loader resolves them before `main()` runs; a missing DLL prevents the process from starting entirely.

| DLL | Hard import source |
|-----|--------------------|
| `onnxruntime.dll` | `onnxruntime_imported` SHARED IMPORTED → `onnxruntime.lib` → `pm-media` (`CMakeLists.txt:1346`) |
| `cublas64_13.dll` | `CUDA::cublas` → `prebuilt-ggml-cuda` → `prebuilt-ggml` → `pm-media` (`FindPrebuiltLlama.cmake:115–121`) |

`ensure_agent_process_path_has_exe_bin()` (`agent_factory.cpp:74`) and `try_append_cuda()` (`OnnxSession.cpp:53`) both run too late — they execute after the loader has already resolved (or failed to resolve) the import table.

---

### TODO 0 — `src/core/library_utils.hpp/.cpp` — shared DLL find/load utility

All the `LoadLibraryExW` / `GetProcAddress` / `AddDllDirectory` calls needed by TODOs 1–4 (and future callers elsewhere) should live in one place rather than being copy-pasted per subsystem.

#### Interface (`src/core/library_utils.hpp`)

```cpp
#pragma once
#include <string>

namespace media::library {

// Probe whether a named shared library can be located by the OS loader.
// Does not execute any code from the library.
// Win32: LoadLibraryExW(LOAD_LIBRARY_AS_DATAFILE); POSIX: dlopen(RTLD_LAZY|RTLD_LOCAL)+dlclose
bool probe(const std::string& name) noexcept;

// Load a shared library and return an opaque handle, or nullptr on failure.
// Searches: caller-supplied search_dir first (if non-empty), then default OS paths.
// Win32: LoadLibraryExW(LOAD_WITH_ALTERED_SEARCH_PATH | LOAD_LIBRARY_SEARCH_USER_DIRS)
// POSIX: dlopen(RTLD_LAZY|RTLD_LOCAL)
void* load(const std::string& name, const std::string& search_dir = {}) noexcept;

// Resolve a symbol from a handle returned by load().
// Win32: GetProcAddress; POSIX: dlsym
void* sym(void* handle, const std::string& symbol) noexcept;

// Release a handle returned by load().  No-op on nullptr.
void unload(void* handle) noexcept;

// Register a directory with the OS loader search path for the current process.
// Must be called before the first load() that needs the directory.
// Win32: AddDllDirectory; POSIX: no-op (use RPATH / LD_LIBRARY_PATH instead).
//
// dir may contain ${ENV:VAR} tokens (same syntax as command_variables.cpp).
// They are expanded via media::commands::resolve_variables before the path is
// registered.  Examples:
//   add_search_dir("${ENV:CUDA_PATH}\\bin")
//   add_search_dir("${ENV:CUDNN_PATH}")
//   add_search_dir("runtimes/cuda-12")   // plain path also fine
// Unknown / empty env vars expand to the empty string; the resulting path is
// silently skipped rather than registered.
void add_search_dir(const std::string& dir) noexcept;

} // namespace media::library
```

#### Implementation notes (`src/core/library_utils.cpp`)

- Win32 only for now (`#ifdef _WIN32`); POSIX stubs that compile but no-op / return nullptr.
- `probe()` — `LoadLibraryExW(name_w, nullptr, LOAD_LIBRARY_AS_DATAFILE)` + immediate `FreeLibrary`; catches `GetLastError` and returns false on failure.
- `load()` — if `search_dir` non-empty, resolves a full path (`search_dir/name`), calls `LoadLibraryExW(full_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)`; otherwise `LoadLibraryExW(name_w, nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS)`.
- `add_search_dir()` — resolves `${ENV:VAR}` tokens in `dir` via `media::commands::resolve_variables` (empty `VariableMap`, so only `ENV:` and `KNOWNFOLDER:` prefixes apply); if the resolved path is empty or does not exist, silently returns.  Otherwise calls `AddDllDirectory(resolved_w)` (Win8+); graceful no-op on older Windows (checks `GetProcAddress(kernel32, "AddDllDirectory")` at first call).
- Handles are `HMODULE` cast to `void*`.
- No logging inside the utility itself — callers log based on return values.

#### Known callers (after TODOs 1–4 land)

| Caller | Use |
|--------|-----|
| `src/lib/onnx/OnnxSession.cpp` | `load("onnxruntime.dll", exe_dir)` + `sym(h, "OrtGetApiBase")` |
| `src/lib/onnx/OnnxSession.cpp` | `probe("onnxruntime_providers_cuda.dll")`, `probe("cudart64_12.dll")`, `probe("cudnn64_9.dll")` in `try_append_cuda()` |
| `src/llm/agent_factory.cpp` | `add_search_dir(bin_dir)` alongside existing `SetEnvironmentVariableW` |
| `src/cli/pm_image_entry.cpp` | `add_search_dir(exe_dir)` at startup |
| GPU pack detection | `add_search_dir("${ENV:CUDA_PATH}\\bin")`, `add_search_dir("${ENV:CUDNN_PATH}")` — expand env vars set by CUDA/cuDNN installers before probing |
| Future: whisper / Vulkan / DirectML soft-load | `probe` + `load` |

**Files:**
- New `src/core/library_utils.hpp`
- New `src/core/library_utils.cpp`
- `CMakeLists.txt` — add both files to `pm-media` sources

---

### TODO 1 — Soft-load `onnxruntime.dll` in `OnnxSession::load()`

`onnxruntime.dll` ships beside the exe but must not be a hard PE import.  ORT's C API supports this: load the DLL, resolve `OrtGetApiBase`, call `Ort::InitApi()`.  Uses `library_utils` (TODO 0).

```cpp
// src/lib/onnx/OnnxSession.cpp — inside Impl::load()
static void* s_ort_dll = nullptr;
static std::once_flag s_ort_once;
std::call_once(s_ort_once, [] {
    s_ort_dll = media::library::load("onnxruntime.dll", exe_parent_directory_str());
    if (s_ort_dll) {
        auto* get_api = reinterpret_cast<decltype(&OrtGetApiBase)>(
            media::library::sym(s_ort_dll, "OrtGetApiBase"));
        if (get_api) Ort::InitApi(get_api()->GetApi(ORT_API_VERSION));
        else { media::library::unload(s_ort_dll); s_ort_dll = nullptr; }
    }
});
if (!s_ort_dll) {
    err = "onnxruntime.dll not found — ONNX inference unavailable";
    return false;
}
// ... existing Ort::Env construction continues unchanged
```

**Files:**
- `CMakeLists.txt:1346` — remove `target_link_libraries(pm-media PRIVATE onnxruntime_imported)`; keep target for `copy_if_different` dist step only
- `src/lib/onnx/OnnxSession.cpp` — replace raw `LoadLibraryExW` with `library_utils` calls as above
- `src/lib/onnx/OnnxSession.hpp` — no change (PIMPL hides all ORT types)

---

### TODO 2 — `add_search_dir` at entry point

Call `media::library::add_search_dir(exe_dir)` early in `main()` / CLI entry, before any ORT or llama call.  This registers the exe directory with the loader's search list for the current process (`AddDllDirectory` on Win32), so `onnxruntime.dll` and GPU pack DLLs beside the exe are found without relying on `%PATH%`.

`SetEnvironmentVariableW` (current `ensure_agent_process_path_has_exe_bin()`) only helps child processes.  `add_search_dir` helps the current process.

**Files:**
- `src/cli/pm_image_entry.cpp` (or wherever `main()` lives) — call `library::add_search_dir(exe_dir)` before any lib init
- `src/llm/agent_factory.cpp:82` — call `library::add_search_dir(bin_dir)` alongside the existing `SetEnvironmentVariableW` (covers GUI / agent paths)

---

### TODO 3 — llama.cpp CUDA: `GGML_BACKEND_DL`

llama.cpp supports building GPU backends as standalone DLLs via `GGML_BACKEND_DL`.  The main binary carries zero CUDA hard imports; `ggml-cuda.dll` is discovered and loaded by llama's backend registry at runtime.  If absent → CPU/Vulkan automatically.

```cmake
# in CMakeLists.txt llama.cpp subdir config
set(GGML_BACKEND_DL ON  CACHE BOOL "" FORCE)
set(GGML_CUDA       ON  CACHE BOOL "" FORCE)
# output: ggml-cuda.dll  (ships in GPU pack, not core)
```

```cpp
// no change needed in llama_agent_client.cpp
// llama.cpp's ggml_backend_load_all() handles discovery + CPU fallback
```

**Files:**
- `CMakeLists.txt` / `FindPrebuiltLlama.cmake:115–121` — set `GGML_BACKEND_DL=ON`; remove `CUDA::cublas`, `CUDA::cublasLt`, `CUDA::cudart`, `CUDA::cuda_driver` from `prebuilt-ggml-cuda` link (they move inside `ggml-cuda.dll`)
- `CMakePresets.json` — add `"GGML_BACKEND_DL": true` to GPU preset; CPU preset unchanged

---

### TODO 4 — ORT CUDA EP: probe before enabling

`try_append_cuda()` (`OnnxSession.cpp:53`) already wraps `AppendExecutionProvider_CUDA` in a try/catch.  Extend it with explicit DLL probes before the call so the failure reason is surfaced clearly rather than relying on ORT's internal exception.

```cpp
bool try_append_cuda(Ort::SessionOptions& opts) noexcept
{
#if defined(FEATURE_ONNX_CUDA) && FEATURE_ONNX_CUDA
    // Probe required DLLs before handing off to ORT.
    // Avoids cryptic ORT exceptions when the GPU pack is partially present.
    using media::library::probe;
    if (!probe("onnxruntime_providers_cuda.dll")) return false;
    if (!probe("cudart64_12.dll") && !probe("cudart64_11.dll")) return false;
    // cuDNN: try v9 then v8 filenames
    const bool cudnn_ok = probe("cudnn64_9.dll") || probe("cudnn64_8.dll")
                       || probe("cudnn_ops64_9.dll");
    if (!cudnn_ok) return false;
    try {
        OrtCUDAProviderOptions cuda_opts{};
        // ... existing options ...
        opts.AppendExecutionProvider_CUDA(cuda_opts);
        return true;
    } catch (...) { return false; }
#else
    (void)opts; return false;
#endif
}
```

**Files:**
- `src/lib/onnx/OnnxSession.cpp:53` — replace current `try_append_cuda` body with the probed version above

---

### CUDA detection strategy (ideas)

#### Don't rely on env vars

Env vars are often absent, stale, or left behind by old installs.  Useful as hints only — never as the authority for enabling a feature.

Common candidates that may or may not exist:
```
CUDA_PATH, CUDA_HOME
CUDA_PATH_V13_0, CUDA_PATH_V12_9, CUDA_PATH_V12_8, …
CUDNN_PATH
```

Many users with a working NVIDIA driver and a bundled CUDA runtime will have none of these set.

#### Preferred detection order

**1. Private runtime beside the exe** — check first, trust most.
```
app.exe
runtimes/cuda/
runtimes/ort-cuda/
```
If the GPU pack was installed here, use it.  No env vars needed.

**2. NVIDIA driver presence** — `library::probe("nvcuda.dll")`.  If it loads, the driver is installed.  CUDA Toolkit is not required for this step.

**3. Driver version** — after confirming `nvcuda.dll` is present, optionally call `cuInit(0)` + `cuDriverGetVersion()`, or load `nvml.dll` and call `nvmlInit` + `nvmlSystemGetDriverVersion`.  Tells you whether the driver is new enough for the CUDA version in the GPU pack.

**4. Actual backend init** — the real answer.  For llama.cpp: `ggml_backend_cuda_init()` (via `GGML_BACKEND_DL` loaded handle).  For ORT: `AppendExecutionProvider_CUDA` inside a try/catch (current `try_append_cuda()`).  If this succeeds, CUDA is available — no env var or registry check matters.

#### Registry — diagnostics only

```
HKLM\SOFTWARE\NVIDIA Corporation\GPU Computing Toolkit\CUDA
```

Not reliable enough for feature-gating.  Useful only for surfacing a version string in the UI / logs.

#### Capability table to build at startup

```
GPU:
  NVIDIA driver:    Yes  (nvcuda.dll found)
  Driver version:   585.xx

CUDA runtime:
  Private pack:     Yes  (runtimes/cuda-12/)
  Version:          12.x

llama.cpp CUDA:     Available   (ggml-cuda.dll loaded + init OK)
ONNX CUDA:          Failed      (cudnn64_9.dll missing)

Active backend:     CPU / CUDA / Vulkan
```

#### Rule for `add_search_dir` + env vars

Use env vars **only** to extend the search list before probing — never to conclude CUDA is available.

```cpp
// Add known env-var-based locations as hints so probe() can find DLLs there.
// Actual capability is determined by probe() + backend init, not by these vars existing.
library::add_search_dir("${ENV:CUDA_PATH}\\bin");
library::add_search_dir("${ENV:CUDNN_PATH}");
library::add_search_dir("runtimes/cuda-12");  // private pack wins if present
```

Resolving `${ENV:CUDA_PATH}` to an empty string (env var absent) silently skips the dir — no false positive.

---

### Discarded options

| Option | Why discarded |
|--------|--------------|
| CPU-only default CMake preset | Keep CUDA in the default build; soft-loading handles the runtime fallback |
| `/DELAYLOAD:cublas64_13.dll` | CUDA DLL names are version-suffixed; exact name required at link time; fragile across toolkit versions |
| `/DELAYLOAD:onnxruntime.dll` | Superseded by explicit `LoadLibraryExW` in TODO 1, which gives cleaner error reporting |
| Separate `pm_onnx_cpu.dll` plugin | Unnecessary now that `Ort::InitApi` allows soft-loading ORT without a wrapping plugin DLL |

---

### Note: ONNX Runtime GenAI (`onnxruntime-genai`)

[onnxruntime.ai](https://onnxruntime.ai/) now promotes a separate **ONNX Runtime GenAI** package targeting LLM / generative workloads (Llama-2, image synthesis, text generation).  This is distinct from the classic `onnxruntime` inference package used here for YOLO / OCR / embeddings.

`onnxruntime-genai` is a potential future alternative to llama.cpp for local LLM inference — same CUDA/cuDNN soft-load concerns apply.  Not in scope for current work; noted here as a future option if llama.cpp proves inadequate.

---

## OS Matrix

Windows:
  CPU baseline
  Vulkan optional
  CUDA optional plugin pack

macOS:
  CPU baseline
  Metal preferred
  CoreML optional for ONNX

Linux:
  CPU baseline
  Vulkan optional
  CUDA optional plugin pack


## 1. General Chat / Agent Models

### Best overall

* Qwen **Qwen3 8B**

  * Current sweet spot.
  * Strong coding.
  * Good tool use.
  * Fits comfortably in 8–12 GB VRAM with quantization.
  * Excellent llama.cpp support.

* Google **Gemma 3 12B**

  * Very strong reasoning.
  * Good multilingual.
  * Nice vision variants.
  * Better with 12–16 GB VRAM.

### Lightweight

* **Qwen3 4B**
* **Gemma 3 4B**

Good for laptops and SBCs.

---

## 2. Coding Models

### Recommended

* Qwen **Qwen3-Coder**
* DeepSeek **DeepSeek-Coder-V2 Lite**

Use for:

* C++
* Rust
* TypeScript
* Python

For your PolyMech stack I'd probably default to:

* Qwen3 8B → chat
* Qwen3-Coder → code generation

---

## 3. Vision Models

### Practical local vision

* Qwen **Qwen2.5-VL 7B**
* Google **Gemma 3 Vision**

Use for:

* screenshots
* UI automation
* receipts
* PDFs
* webcam analysis

For your app-use assistant:

Qwen2.5-VL is probably the most useful today.

---

## 4. OCR

Skip giant VLMs when OCR is the primary task.

### Better pipeline

1. detector
2. OCR model
3. LLM

Models:

* PaddlePaddle PaddleOCR
* Microsoft TrOCR
* Surya OCR Surya

For invoices and receipts:

**PaddleOCR is still hard to beat.**

---

## 5. Embeddings

Almost every local app should ship embeddings.

### Baseline

* all-MiniLM-L6-v2 all-MiniLM-L6-v2

  * tiny
  * fast
  * ~80 MB

### Better

* Alibaba Cloud bge-small
* Alibaba Cloud bge-base
* Alibaba Cloud bge-m3

For tool search, memories, skills, commands:

**bge-small** is a great default.

---

## 6. Object Detection

For robotics and camera feeds:

### Current defaults

* Ultralytics YOLOv11
* Ultralytics YOLOv12 (where available)
* RT-DETR RT-DETR

For realtime:

* YOLOv11n
* YOLOv11s

For accuracy:

* YOLOv11m

---

## 7. Speech

### STT

* OpenAI Whisper Large V3
* Systran Faster-Whisper

### TTS

* Hexgrad Kokoro
* Coqui XTTS-v2

---

## 8. Rerankers (often forgotten)

Huge win for agent systems.

### Models

* Alibaba Cloud bge-reranker-base
* Alibaba Cloud bge-reranker-v2

Useful for:

* command search
* memory retrieval
* tool retrieval
* file search

https://huggingface.co/BAAI/bge-reranker-v2-m3

---

## If I were shipping PolyMech today

| Feature        | Model             |
| -------------- | ----------------- |
| Chat           | Qwen3 8B          |
| Code           | Qwen3-Coder       |
| Vision         | Qwen2.5-VL 7B     |
| OCR            | PaddleOCR         |
| Embeddings     | bge-small         |
| Reranker       | bge-reranker-base |
| Detection      | YOLOv11           |
| Speech-to-text | Faster-Whisper    |
| TTS            | Kokoro            |

That stack covers about **90% of an offline desktop assistant** while remaining practical on a machine with **8–16 GB VRAM** and integrates well with your existing C++/llama.cpp/ONNX approach.

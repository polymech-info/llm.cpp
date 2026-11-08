# FEATURE_ONNX — ONNX Runtime inference

General-purpose [ONNX Runtime](https://onnxruntime.ai/docs/get-started/with-cpp.html#api-reference)
inference backend. Loads any `.onnx` model and runs CPU inference via the
stable C++ API (`onnxruntime_cxx_api.h`), exposed through a thin PIMPL wrapper
that keeps all ORT headers out of the rest of the codebase.

---

## Why

| | llama.cpp (`FEATURE_LLAMA`) | ONNX Runtime (`FEATURE_ONNX`) |
|:--|:--|:--|
| **Format** | `.gguf` | `.onnx` |
| **Model source** | llama.cpp-compatible GGUF exports | PyTorch `torch.onnx.export`, TF `tf2onnx`, sklearn `skl2onnx`, … |
| **API surface** | Embedding-specific (`embed`, `rank`, `nearest_intent`) | Generic tensor I/O (`load`, `run`) |
| **Build method** | FetchContent (source build) | Pre-built DLL / `.lib` (auto-downloaded zip) |
| **Typical use** | Semantic similarity, tool routing | Classification, detection, regression, custom NLP |

---

## Architecture

```
src/
  lib/onnx/
    OnnxSession.hpp    ← public API (PIMPL; zero onnxruntime.h leakage)
    OnnxSession.cpp    ← onnxruntime_cxx_api.h confined here; load + run

  cli/
    pm_image_cmd_onnx.hpp/.cpp   ← tanit-cli onnx subcommand
    pm_image_register_cli.cpp    ← wires Cmd::Onnx into CLI11 tree
    pm_image_dispatch_remaining.cpp ← dispatch: onnx_cmd → pm_image_cmd_onnx
    pm_image_cli_state.hpp       ← onnx_cmd / onnx_info_cmd / onnx_run_cmd fields
    pm_image_cmd_handlers.hpp    ← includes pm_image_cmd_onnx.hpp under guard

constants.hpp                    ← Cmd::Onnx = 1ULL<<31; k_cmds_all; defaults
```

### PIMPL boundary

`onnxruntime_cxx_api.h` (and all `Ort::*` symbols) are confined to
`OnnxSession.cpp`. Consumers only include `OnnxSession.hpp` and never see ORT
types directly, insulating them from ONNX Runtime API churn.

---

## C++ API — `OnnxSession`

### Types

```cpp
namespace media::onnx {

struct TensorInfo {
    std::string          name;       // ONNX node name ("Input3", "output0", …)
    std::string          elem_type;  // "float32" | "int64" | "uint8" | …
    std::vector<int64_t> shape;      // -1 = dynamic / symbolic dimension
};

struct RunResult {
    bool                            ok = false;
    std::string                     error;
    std::vector<TensorInfo>         output_info;   // mirrors session.output_info()
    std::vector<std::vector<float>> outputs_f32;   // one flat float32 vec per output
};

}
```

### Class

```cpp
class OnnxSession {
public:
    // Load an ONNX model from disk.
    // n_threads = intra-op CPU thread count (default 4).
    // Returns false and populates error() on failure.
    bool load(const std::string& model_path, int n_threads = 4);

    bool               loaded() const noexcept;
    const std::string& error()  const noexcept;

    std::vector<TensorInfo> input_info()  const;
    std::vector<TensorInfo> output_info() const;

    // Run inference.
    // inputs: one flat float32 buffer per model input tensor.
    // Pass {} (default) to fill all inputs with zeros using the model's
    // static shapes (dynamic dims resolved to 1).
    RunResult run(const std::vector<std::vector<float>>& inputs = {});
};
```

### Usage examples

```cpp
// 1. Inspect a model
media::onnx::OnnxSession s;
if (!s.load("dist/models/mnist-12.onnx", 4)) {
    fprintf(stderr, "load failed: %s\n", s.error().c_str());
}
for (const auto& t : s.input_info())
    printf("  input  %s  %s  [%td dims]\n",
           t.name.c_str(), t.elem_type.c_str(), t.shape.size());
for (const auto& t : s.output_info())
    printf("  output %s  %s  [%td dims]\n",
           t.name.c_str(), t.elem_type.c_str(), t.shape.size());

// 2. Run with zero inputs (smoke-test / shape probe)
auto r = s.run();
if (r.ok)
    printf("output[0] has %zu elements\n", r.outputs_f32[0].size());

// 3. Run with explicit float32 data
std::vector<float> img(1 * 1 * 28 * 28, 0.f);  // one blank MNIST image
auto r2 = s.run({ img });
// r2.outputs_f32[0] → 10 logits
```

### elem_type strings

| C++ ORT enum | `TensorInfo::elem_type` |
|:------------|:----------------------|
| `FLOAT`      | `"float32"` |
| `DOUBLE`     | `"float64"` |
| `INT8`       | `"int8"` |
| `INT16`      | `"int16"` |
| `INT32`      | `"int32"` |
| `INT64`      | `"int64"` |
| `UINT8`      | `"uint8"` |
| `UINT16`     | `"uint16"` |
| `UINT32`     | `"uint32"` |
| `UINT64`     | `"uint64"` |
| `BOOL`       | `"bool"` |
| `STRING`     | `"string"` |
| `FLOAT16`    | `"float16"` |

### Dynamic shapes

Dimensions reported as `-1` in `TensorInfo::shape` are symbolic (dynamic).
`OnnxSession::run()` replaces every `-1` with `1` when building the zero-input
tensor. Pass explicit `inputs` with the desired concrete sizes to override.

---

## Implementation notes

### OnnxSession::Impl

```
Ort::Env          env{ORT_LOGGING_LEVEL_WARNING, "pm-image-onnx"}
Ort::Session      session          // created on load()
vector<TensorInfo> in_info          // populated after load()
vector<TensorInfo> out_info
string             err
```

`load()` sequence:
1. Create `Ort::SessionOptions` — `SetIntraOpNumThreads(n_threads)`,
   `SetGraphOptimizationLevel(ORT_ENABLE_ALL)`.
2. Construct `Ort::Session` — wchar_t path on Windows (`ORTCHAR_T = wchar_t`),
   char path on Linux/macOS.
3. Walk `GetInputCount()` / `GetOutputCount()` to populate `in_info` /
   `out_info` via `GetInputNameAllocated` + `GetInputTypeInfo`.

`run()` sequence:
1. Clone `in_info` shapes; replace `-1` → `1`.
2. Allocate flat `vector<float>` per input (zero or user-provided).
3. Wrap each buffer in `Ort::Value::CreateTensor<float>`.
4. Call `session->Run(RunOptions{nullptr}, in_names, …, out_names, …)`.
5. Copy `GetTensorData<float>()` into `RunResult::outputs_f32`.

### FEATURE_ONNX=0 stub

When compiled without `FEATURE_ONNX`, `OnnxSession.cpp` provides
no-op implementations of all methods so any translation unit that
unconditionally includes `OnnxSession.hpp` still compiles and links.
`load()` returns `false`; `error()` returns `"FEATURE_ONNX=0"`;
`run()` returns `RunResult{ok=false}`.

---

## CMake feature flags

| Flag | Default | Description |
|:-----|:-------:|:------------|
| `FEATURE_ONNX` | **ON** | Download ORT pre-built package and compile `OnnxSession` |
| `FEATURE_COMMAND_ONNX` | **ON** | Build `tanit-cli onnx` subcommand (requires `FEATURE_ONNX`) |

Set in `CMakePresets.json` (`base` preset) and propagated to all targets as
compile definitions via `target_compile_definitions`.

---

## Dependency — ONNX Runtime pre-built package

### Windows (auto-download)

No Git submodule, no manual step. CMake downloads and extracts on first
configure if `third_party/onnxruntime-win-x64-1.20.1/` is absent:

```cmake
set(_ORT_VERSION "1.20.1")
file(DOWNLOAD
    "https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip"
    "${CMAKE_SOURCE_DIR}/third_party/onnxruntime-win-x64-1.20.1.zip"
    SHOW_PROGRESS TLS_VERIFY ON)
execute_process(COMMAND cmake -E tar xf ... WORKING_DIRECTORY third_party/)
```

After extraction an `onnxruntime_imported` SHARED IMPORTED CMake target is
created:

```cmake
add_library(onnxruntime_imported SHARED IMPORTED)
set_target_properties(onnxruntime_imported PROPERTIES
    IMPORTED_IMPLIB           "…/lib/onnxruntime.lib"
    IMPORTED_LOCATION         "…/lib/onnxruntime.dll"
    INTERFACE_INCLUDE_DIRECTORIES "…/include"
)
```

`onnxruntime.dll` is copied to `dist/win-x64/` post-build alongside the
application executables.

### Extracted layout

```
third_party/onnxruntime-win-x64-1.20.1/
  include/
    onnxruntime_cxx_api.h          ← C++ wrapper (used by OnnxSession.cpp)
    onnxruntime_cxx_inline.h
    onnxruntime_c_api.h            ← raw C API
    onnxruntime_run_options_config_keys.h
    …
  lib/
    onnxruntime.lib                ← MSVC import library
    onnxruntime.dll                ← runtime DLL (copied to dist/win-x64/)
    onnxruntime.pdb                ← PDB symbols (optional)
```

### Linux / macOS

System-package path (`find_library` / `find_path`). Checked paths:

```
/usr/local/lib          /usr/local/include/onnxruntime
/opt/homebrew/lib       /opt/homebrew/include/onnxruntime
$HOME/.pm-media-fetchcontent/onnxruntime/...
```

If not found, `FEATURE_ONNX` is automatically disabled with a status message.
Install via:

```bash
# Ubuntu / Debian
pip install onnxruntime          # Python wheels only; for C++ use:
apt install libonnxruntime-dev   # (if packaged by distro)

# macOS
brew install onnxruntime
```

---

## CLI — `tanit-cli onnx`

```
tanit-cli onnx <subcommand> --model <path.onnx> [options]
```

### Subcommands

#### `info` — Inspect model metadata

Loads the model and prints input/output tensor names, element types, and shapes
as JSON. Does **not** run inference.

```powershell
tanit-cli onnx info --model dist/models/mnist-12.onnx
```

Output:
```json
{
  "model": "mnist-12.onnx",
  "inputs": [
    { "name": "Input3",           "elem_type": "float32", "shape": [1, 1, 28, 28] }
  ],
  "outputs": [
    { "name": "Plus214_Output_0", "elem_type": "float32", "shape": [1, 10] }
  ]
}
```

#### `run` — Run zero-input inference

Loads the model, fills all inputs with zeros (dynamic dims → 1), runs one
forward pass, and prints output shapes and element counts as JSON.

```powershell
tanit-cli onnx run --model dist/models/mnist-12.onnx
```

Output:
```json
{
  "ok": true,
  "outputs": [
    { "name": "Plus214_Output_0", "elem_type": "float32", "shape": [1, 10], "size": 10 }
  ]
}
```

Add `--full-vec` to include the raw float32 values:

```powershell
tanit-cli onnx run --model dist/models/mnist-12.onnx --full-vec
```

```json
{
  "ok": true,
  "outputs": [
    {
      "name": "Plus214_Output_0",
      "elem_type": "float32",
      "shape": [1, 10],
      "size": 10,
      "values": [0.032, -0.107, 0.441, …]
    }
  ]
}
```

### Options

| Option | Default | Description |
|:-------|:-------:|:------------|
| `--model, -m` | *(required)* | Path to `.onnx` model file |
| `--threads, -j` | `4` | Intra-op CPU thread count (range 1–256) |
| `--full-vec` | `false` | Include raw float32 output values in `run` JSON |

---

## Models

Test model assets live in `dist/models/`. The directory is `.gitignore`d.

### Smoke-test default — MNIST digit classifier

| Model | Input | Output | Size |
|:------|:------|:-------|:-----|
| `mnist-12.onnx` | `float32[1,1,28,28]` | `float32[1,10]` | **26 KB** |

```powershell
# PowerShell — download MNIST model
Invoke-WebRequest `
  -Uri "https://media.githubusercontent.com/media/onnx/models/main/validated/vision/classification/mnist/model/mnist-12.onnx" `
  -OutFile "dist/models/mnist-12.onnx"
```

The `npm run test:onnx` script downloads this automatically when absent.

### Model compatibility

Any ONNX model exported from:

| Framework | Export tool | Notes |
|:----------|:------------|:------|
| PyTorch   | `torch.onnx.export` | opset ≥ 9 |
| TensorFlow | `tf2onnx` | `python -m tf2onnx.convert --saved-model …` |
| scikit-learn | `skl2onnx` | regression, classification |
| Hugging Face Transformers | `optimum-cli export onnx` | BERT, GPT-2, etc. |

All are drop-in as `--model` arguments.

### Example model table

| Model | Task | Input | Output | Size |
|:------|:-----|:------|:-------|:-----|
| `mnist-12.onnx` | Digit classification | `float32[1,1,28,28]` | `float32[1,10]` | 26 KB |
| `squeezenet1.0-12.onnx` | ImageNet classification | `float32[1,3,224,224]` | `float32[1,1000,1,1]` | ~5 MB |
| `resnet50-v2-7.onnx` | ImageNet classification | `float32[1,3,224,224]` | `float32[1,1000]` | ~98 MB |

---

## Tests

```
npm run test:onnx           # smoke: info + run + run --full-vec
npm run test:onnx:model     # same, with --model <path>
```

### `test:onnx`

**File**: `tests/orchestrator/test-onnx.mjs`

**Auto-download**: downloads `dist/models/mnist-12.onnx` (~26 KB) on first run
via HTTPS redirect chain; skips gracefully on network failure.

**Skips gracefully** when:
- `tanit-cli.exe` is not built yet.
- Model file absent and download fails.
- `FEATURE_ONNX=0` build (binary exits 1 with a message; tests skip).

**Cases**:

| # | Test | Assertions |
|:--|:-----|:-----------|
| 1 | `onnx info` | exit 0; valid JSON; `inputs`/`outputs` arrays present; MNIST: `Input3`, rank-4 input, `float32` output |
| 2 | `onnx run` | exit 0; `ok=true`; outputs present; MNIST: `size=10` |
| 3 | `onnx run --full-vec` | exit 0; `ok=true`; `values` array with `length=10` |

**Typical result**: **19/19 passed** in ~940 ms (3 subprocesses × ~70 ms each,
plus model load on first run).

---

## File map

| Path | Role |
|:-----|:-----|
| `src/lib/onnx/OnnxSession.hpp` | Public PIMPL API — consumers include only this |
| `src/lib/onnx/OnnxSession.cpp` | ORT implementation (onnxruntime_cxx_api.h confined here) |
| `src/cli/pm_image_cmd_onnx.hpp` | CLI function declarations (`pm_image_cmd_onnx`, `pm_image_register_onnx`) |
| `src/cli/pm_image_cmd_onnx.cpp` | CLI implementation (`onnx info`, `onnx run`) |
| `src/cli/pm_image_cmd_handlers.hpp` | Includes `pm_image_cmd_onnx.hpp` under `FEATURE_ONNX` guard |
| `src/cli/pm_image_dispatch_remaining.cpp` | Dispatch: `onnx_cmd->parsed()` → `pm_image_cmd_onnx` |
| `src/cli/pm_image_register_cli.cpp` | Registration: `pm_image_register_onnx` + `Cmd::Onnx` in table |
| `src/cli/pm_image_cli_state.hpp` | State fields: `onnx_cmd`, `onnx_info_cmd`, `onnx_run_cmd`, `onnx_model_path`, `onnx_threads`, `onnx_full_vec` |
| `src/constants.hpp` | `Cmd::Onnx = 1ULL<<31`; `k_cmds_all`; `FEATURE_ONNX` / `FEATURE_COMMAND_ONNX` defaults; `cmd_available_at_compile_time` entry |
| `CMakeLists.txt` | `FEATURE_ONNX` / `FEATURE_COMMAND_ONNX` options; ORT download block; `onnxruntime_imported` target; `OnnxSession.cpp` / CLI sources; compile defs; DLL post-build copy |
| `CMakePresets.json` | `FEATURE_ONNX: ON`, `FEATURE_COMMAND_ONNX: ON` in base preset + vendor doc entries |
| `tests/orchestrator/test-onnx.mjs` | Node.js smoke test (auto-downloads MNIST, 3 test cases) |
| `third_party/onnxruntime-win-x64-1.20.1/` | Auto-downloaded ORT pre-built package (gitignored) |
| `dist/models/mnist-12.onnx` | Smoke-test model (gitignored; auto-downloaded by test script) |
| `dist/win-x64/onnxruntime.dll` | ORT runtime DLL (copied post-build) |

---

## Known limitations

### float32-only `run()` inputs

`OnnxSession::run()` currently builds all input tensors as `float32`. Models
with non-float inputs (e.g. `int64` token IDs in NLP models) require caller-side
casting or an extended API that accepts typed input buffers. This is sufficient
for image and regression models; NLP embedding models typically use the llama.cpp
path (`FEATURE_LLAMA`) instead.

### No execution provider selection

The session always uses the CPU provider
(`OrtArenaAllocator / OrtMemTypeDefault`). DirectML, CUDA, and other execution
providers are not wired up. To add DirectML on Windows:

```cpp
// In Impl::load(), after creating opts:
Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(opts, 0));
```

This requires linking against `Microsoft.AI.DirectML` (NuGet) and the
`onnxruntime-directml` package variant.

### Windows path encoding

The `wchar_t` path conversion (`std::wstring(path.begin(), path.end())`) is
ASCII/Latin-1 safe but does not correctly handle arbitrary Unicode paths. For
Unicode model paths on Windows, use `MultiByteToWideChar(CP_UTF8, …)` instead.

---

## Next steps

| # | Task | Effort |
|:--|:-----|:------:|
| 1 | Add typed input support to `run()` (int64, uint8, …) for NLP / quantised models | S |
| 2 | DirectML execution provider option (`FEATURE_ONNX_DIRECTML`) for GPU on Windows | M |
| 3 | Pre/post-processing helpers: image normalise → float32 tensor, softmax, argmax | S |
| 4 | `onnx run --input name:type:shape:values` flag for CLI-specified tensor data | M |
| 5 | Unicode model path fix on Windows (`MultiByteToWideChar`) | XS |
| 6 | Pin ORT version in `CMakePresets.json` vendor map (currently hardcoded in `CMakeLists.txt`) | XS |
| 7 | `onnx benchmark` subcommand (warm-up + N-run latency measurement, JSON report) | S |


Yes, but surprisingly few are worth it if you already have UIAutomation.

Most "computer use" models are solving the problem you already solved:

```text
Screenshot
  ↓
Vision model
  ↓
Find button
  ↓
Click button
```

You already have:

```text
UIAutomation
  ↓
Exact button
  ↓
Click button
```

which is vastly better.

For your stack I'd look at models for **structure extraction**, not UI understanding.

## Microsoft OmniParser

Microsoft's:

[OmniParser](https://github.com/microsoft/OmniParser?utm_source=chatgpt.com)

Turns screenshots into:

```json
[
  {"type":"button","text":"Save"},
  {"type":"textbox","text":"Filename"}
]
```

Useful when:

```text
Canvas
Games
Remote desktop
Custom OpenGL apps
```

Not needed for Word, Notepad, Office, Chrome, etc.

---

## UI-TARS

[UI-TARS](https://github.com/bytedance/UI-TARS?utm_source=chatgpt.com)

Agent-oriented UI model.

Can reason:

```text
Open settings
Navigate menu
Click X
```

Again:

* slower
* screenshot based

Useful only when UIA unavailable.

---

## ScreenSpot

Benchmark, not really a product.

Useful for evaluating:

```text
How well does a model find controls?
```

---

## Florence-2

[Florence-2](https://huggingface.co/microsoft/Florence-2-large?utm_source=chatgpt.com)

Interesting because it can:

```text
OCR
Captioning
Region detection
Grounding
```

Small enough to run locally.

Could be a nice fallback.

---

## PaddleOCR

Probably the most useful addition.

```text
Screenshot
 ↓
Text + bounding boxes
```

Fast.

Reliable.

ONNX friendly.

---

## Docling

[Docling](https://github.com/docling-project/docling?utm_source=chatgpt.com)

Very relevant.

Can turn:

```text
PDF
DOCX
PPTX
HTML
```

into structured markdown.

For your file-search agent this is arguably more useful than any UI model.

---

## What I'd build

```text
Level 1
-------
UIAutomation

Level 2
-------
DOM/CDP

Level 3
-------
PaddleOCR

Level 4
-------
Florence-2

Level 5
-------
Vision LLM
```

Routing:

```cpp
if (uia_found)
    use_uia();

else if (browser)
    use_dom();

else if (ocr_enough)
    use_paddleocr();

else if (need_visual_understanding)
    use_florence();

else
    use_gpt_vision();
```

For the agent you're building, I'd spend effort on:

1. UIAutomation patterns database
2. Browser DOM extraction
3. PaddleOCR
4. Embeddings + memory

before touching UI-TARS, OmniParser, or other "computer use" models. Your UIA route is already stronger, faster, and more deterministic than what those models provide on standard desktop applications.

# VLM / Vision Models — Local Inference Overview

> Where we are running models locally as of Jun 2026.  
> Current VLM numbers are measured on this machine with CUDA enabled (all layers offloaded, 8 threads).

---

## Disk footprint (~12 GB total)

### VLM / LLM (llama.cpp · GGUF)

| Model | File | Size | Notes |
|---|---|---|---|
| MiniCPM-V 4.5 (vision) | `MiniCPM-V-4_5-Q4_K_M.gguf` | 4.7 GB | main weights, Q4_K_M |
| MiniCPM-V 4.5 mmproj | `MiniCPM-V-4_5-mmproj-f16.gguf` | 1.0 GB | vision projector, f16 |
| MiniCPM-o 4.5 (omni) | `MiniCPM-o-4_5-Q4_K_M.gguf` | 4.7 GB | same size, adds audio + M-RoPE video |
| MiniCPM-o 4.5 vision proj | `MiniCPM-o-4_5-vision-F16.gguf` | 1.0 GB | separate projector for omni model |
| PaddleOCR-VL 1.5 | `PaddleOCR-VL-1.5.gguf` | 892 MB | doc/table OCR specialist |
| PaddleOCR-VL 1.5 mmproj | `PaddleOCR-VL-1.5-mmproj.gguf` | 841 MB | |
| MiniLM-L6 (embeddings) | `all-MiniLM-L6-v2.Q4_K_M.gguf` | 20 MB | text embeddings |

### ONNX models (ONNX Runtime 1.26.0)

| Model | File | Size | Use |
|---|---|---|---|
| YOLO 11 / 26 / v8 (detect) | `yolo*.onnx` | 9–265 MB each | object detection |
| YOLO 11 / 26 / v8 (cls) | `yolo*-cls.onnx` | 10–219 MB | image classification |
| YOLO 11 / 26 / v8 (seg) | `yolo*-seg.onnx` | 11–274 MB | instance segmentation |
| YOLO 11 / 26 / v8 (pose) | `yolo*-pose.onnx` | 11–265 MB | keypoint / pose |
| YOLO 11 / 26 (obb) | `yolo*-obb.onnx` | 10–224 MB | oriented bounding boxes |
| YOLO 26 (semantic) | `yolo26*-sem.onnx` | 6–143 MB | semantic segmentation |
| PP-DocLayoutV3 | `PP-DocLayoutV3.onnx` | 124 MB | document layout analysis |
| PaddleOCR det | `en_PP-OCRv3_det_infer.onnx` | 2 MB | text region detection |
| PaddleOCR rec | `en_PP-OCRv3_rec_infer.onnx` | 9 MB | text recognition |
| Icon detect | `icon_detect.onnx` | 77 MB | UI icon detection |
| Whisper base.en | `ggml-base.en.bin` | 141 MB | speech-to-text |
| Whisper tiny.en | `ggml-tiny.en.bin` | 74 MB | speech-to-text (fast) |

---

## Capabilities by case

| Case | Model | Backend | Notes |
|---|---|---|---|
| **Screenshot / UI analysis** | MiniCPM-V 4.5 | Vulkan VLM | menus, text, layout description |
| **Document / receipt OCR** | MiniCPM-V 4.5 | Vulkan VLM | structured extraction, tables |
| **Document OCR specialist** | PaddleOCR-VL 1.5 | Vulkan VLM | higher accuracy on dense docs |
| **Multi-frame / video** | MiniCPM-V 4.5 | Vulkan VLM | N frames as array (pseudo-temporal) |
| **Video (temporal ordering)** | MiniCPM-o 4.5 | Vulkan VLM | M-RoPE `(t,x,y)` positions per frame |
| **Audio + video (omni)** | MiniCPM-o 4.5 | Vulkan VLM | audio projector + vision projector |
| **Object detection** | YOLO 11/26/v8 | ONNX (CUDA auto / CPU) | n/s/m/l/x sizes |
| **Image classification** | YOLO cls | ONNX | 1000-class ImageNet |
| **Pose / keypoints** | YOLO pose | ONNX | 17-keypoint body |
| **Instance segmentation** | YOLO seg | ONNX | pixel masks |
| **Document layout** | PP-DocLayoutV3 | ONNX | regions: title/table/figure/text |
| **Speech transcription** | Whisper base/tiny | CPU | English, on-device |
| **Text embeddings** | MiniLM-L6 | llama.cpp | semantic search / similarity |

---

## Performance — measured (Jun 4 2026)

**Platform:** Windows 10, NVIDIA GPU, Vulkan backend (GGML_VULKAN, all layers to GPU), 8 threads.

### VLM inference — MiniCPM-V 4.5 Q4_K_M

CUDA build status: `FEATURE_LLAMA_CUDA=ON`, `GGML_CUDA=ON`, `FEATURE_LLAMA_VULKAN=OFF`; `tanit-cli.exe` links `cublas64_13.dll`.

| Case | Input | n_predict | Time | Peak VRAM | vs CPU |
|---|---|---|---|---|---|
| UI screenshot describe | `xblox.png` | 256 | **9.8 s** CUDA | **7.54 GB** (+6.62 GB) | ~6-8× faster |
| Receipt OCR extraction | `ocr-reciept-1.png` | 256 | **9.3 s** CUDA | **7.54 GB** (+6.63 GB) | ~5-7× faster |
| 2-frame multi-image | `xblox.png` + receipt | 256 | **10.4 s** CUDA | **8.14 GB** (+7.20 GB) | +0.6 GB VRAM vs single |
| CPU baseline | single image | ~64 | 60–90 s | system RAM | — |

> VRAM measured with `nvidia-smi` polling at 500 ms intervals on an RTX 3060 12 GB.  
> Latest CUDA smoke: `node tests/orchestrator/test-minicpm.mjs --skip-download --ngl -1 --threads 8` passed 2/2 cases in 19.1 s total.  
> CUDA multi-frame smoke (`--image xblox.png --frame ocr-reciept-1.png`) passed in 10.4 s and peaked at 8.14 GB VRAM.

### ONNX detection — YOLO

| Model | Size | Latency |
|---|---|---|
| yolo11n (nano) | 10 MB | < 20 ms |
| yolo11s (small) | 36 MB | ~40 ms |
| yolo11m (medium) | 77 MB | ~80 ms |
| yolo11l (large) | 97 MB | ~150 ms |
| yolo11x (xlarge) | 217 MB | ~300 ms |

`video detect` and the XBlox `videoDetect` block now use `provider=auto` by default:
try ONNX CUDA EP first, then fall back to CPU. `provider=cuda` is fail-fast and
`provider=cpu` forces CPU. With `--gpu-stats` / `gpuStats: true`, summaries include
best-effort `nvidia-smi` VRAM telemetry.

Smoke on `yolo11n.onnx` + `screen:0`:

| Surface | Provider request | Active provider | Peak VRAM delta | Notes |
|---|---|---|---|---|
| CLI `video detect` | `auto` | `cuda` | +243 MiB | summary emits `requested_provider`, `active_provider`, `gpu_memory` |
| CLI `video detect` | `cpu` | `cpu` | n/a | explicit CPU path |
| CLI `video detect` | `cuda` | `cuda` | +229 MiB | fail-fast CUDA path |
| XBlox `videoDetect` | `auto` | `cuda` | +236 MiB | result emits `provider`, `activeProvider`, `gpuMemory` |

---

## Tech stack

| Layer | Technology | Config flag |
|---|---|---|
| VLM runtime | **llama.cpp** (GGUF) + **mtmd** multimodal | `FEATURE_VLM=ON` |
| GPU backend (VLM) | **GGML CUDA** | `FEATURE_LLAMA_CUDA=ON` |
| GPU backend (fallback) | GGML Vulkan | `FEATURE_LLAMA_VULKAN=OFF` in current release preset |
| ONNX runtime | **ONNX Runtime 1.26.0** | `FEATURE_ONNX=ON` |
| ONNX GPU | CUDA EP (dynamic load, auto-detect) | `FEATURE_ONNX_CUDA=ON` |
| Build system | CMake + MSVC / MSBuild | `CMakePresets.json → release` |
| Multi-frame API | `VlmModel::generate_frames()` | — |

**CUDA/MSBuild notes:** CUDA now builds under the Visual Studio generator, without Ninja. The required fixes are:
strip CMake's `CMAKE_INTDIR="Release"` define before NVIDIA's MSBuild CUDA task expands `nvcc`, and scope the root MSVC `/utf-8` compile option to C/C++ so it is not passed directly to `.cu` compilation.

---

## CLI one-liners

```powershell
# Single image — UI analysis
tanit-cli llama vlm `
  --model dist/models/MiniCPM-V-4_5-Q4_K_M.gguf `
  --mmproj dist/models/MiniCPM-V-4_5-mmproj-f16.gguf `
  --image tests/onnx/xblox.png `
  --prompt "Describe all UI elements, menus, and text you see." `
  --ngl -1 --threads 8 --n-predict 256

# Receipt / document OCR
tanit-cli llama vlm `
  --model dist/models/MiniCPM-V-4_5-Q4_K_M.gguf `
  --mmproj dist/models/MiniCPM-V-4_5-mmproj-f16.gguf `
  --image tests/onnx/ocr-reciept-1.png `
  --prompt "Extract all text: vendor, items, prices, total." `
  --ngl -1 --n-predict 256

# Multi-frame / video (pass frames explicitly)
tanit-cli llama vlm `
  --model dist/models/MiniCPM-V-4_5-Q4_K_M.gguf `
  --mmproj dist/models/MiniCPM-V-4_5-mmproj-f16.gguf `
  --image frame0.jpg --frame frame1.jpg --frame frame2.jpg `
  --prompt "What is happening across these video frames in sequence?" `
  --ngl -1 --ctx 8192 --n-predict 256

# MiniCPM-o omni (full temporal video + audio)
tanit-cli llama vlm `
  --model dist/models/MiniCPM-o-4_5-Q4_K_M.gguf `
  --mmproj dist/models/MiniCPM-o-4_5-vision-F16.gguf `
  --image frame0.jpg --frame frame1.jpg `
  --prompt "Describe the sequence of events." `
  --ngl -1 --ctx 8192

# CPU-only fallback (no GPU)
tanit-cli llama vlm --ngl 0 ...

# Object detection (YOLO, ONNX)
tanit-cli onnx detect `
  --model dist/models/yolo11m.onnx `
  --image tests/onnx/xblox.png

# PaddleOCR pipeline (text detection + recognition)
tanit-cli onnx ocr --image tests/onnx/ocr-reciept-1.png
```

## Test harness

```powershell
# Default benchmark (2 images, GPU, 256 tokens) → report-minicpm.json + .md
npm run test:minicpm

# Screen capture → VLM
npm run test:minicpm:screen

# Omni model (downloads MiniCPM-o on first run)
npm run test:minicpm:omni

# Video from file (requires ffmpeg — extracts 1 fps, max 6 frames)
npm run test:minicpm:video -- path/to/clip.mp4

# Ad-hoc: specific image + prompt, GPU
node tests/orchestrator/test-minicpm.mjs --skip-download `
  --image path/to/image.png `
  --prompt "What is shown here?" `
  --ngl -1 --n-predict 128

# Multi-frame explicit
node tests/orchestrator/test-minicpm.mjs --skip-download `
  --image f0.png --frame f1.png --frame f2.png `
  --prompt "What happens in this sequence?"

# CPU-only comparison
node tests/orchestrator/test-minicpm.mjs --skip-download --ngl 0
```

Reports are written to `tests/onnx/report-minicpm.json` (machine-readable) and
`tests/onnx/report-minicpm.md` (human-readable) with per-case elapsed times,
model metadata, and backend info.

---

## Next steps

- [x] CUDA for VLM: build and smoke-test MiniCPM-V with `GGML_CUDA=ON`
- [ ] MiniCPM-o audio: wire `mtmd_bitmap_init_from_audio()` for speech input to the omni model
- [ ] Token throughput: measure tok/s (currently `tok_per_sec` field is `null` in reports)
- [ ] Quantized mmproj: test `mmproj-Q8_0` variant — smaller VRAM, comparable quality
- [ ] Streaming output: expose `tanit-cli llama vlm --stream` (token-by-token via IPC)

### Reference

- [40GB VRAM - Video Model, understanding & edit](https://huggingface.co/bytedance-research/Lance)

- [ESP32 Mini Drone](https://github.com/fumimaker/drone_meishi)

- [world gen - 3d img23d : 16gb +](https://github.com/SkyworkAI/Matrix-3D)

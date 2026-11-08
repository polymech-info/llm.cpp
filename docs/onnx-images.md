# ONNX Image Inference — `src/lib/onnx/`

Extension of the base ONNX Runtime integration (`docs/onnx.md`) to support real image
inputs: preprocessing via libvips, classification, and text detection.

---

## Library overview

| File | Purpose | Status |
|:-----|:--------|:------:|
| `OnnxSession.hpp/.cpp`    | Core inference session (PIMPL, load, run) | ✅ done |
| `OnnxImagePrep.hpp/.cpp`  | libvips → float32 CHW tensor preprocessing | ✅ done |
| `OnnxClassify.hpp/.cpp`   | Classification wrapper (SqueezeNet, MobileNetV2, …) | ✅ done |
| `OnnxOcr.hpp/.cpp`        | Text detection (PP-OCRv4-det); recognition TODO | 🔶 detection only |

All files are in `src/lib/onnx/` and compiled under `FEATURE_ONNX=1`.  They are
designed as **library code** — usable from the agent pipeline, LLM tools, CLI, and
future UI hooks — not just from the CLI subcommand.

---

## Image preprocessing — `OnnxImagePrep`

```cpp
#include "lib/onnx/OnnxImagePrep.hpp"

media::onnx::ImagePrepOptions opts;
opts.target_w   = 224;          // resize width
opts.target_h   = 224;          // resize height
opts.force_size = true;         // stretch to exact WxH (false = keep aspect ratio)
opts.normalize  = true;         // (pixel/255 - mean) / std  [ImageNet defaults]

std::string err;
int w, h;
auto tensor = media::onnx::prepare_image_tensor("photo.jpg", opts, err, &w, &h);
// tensor: flat float32 [3 × h × w], channel-first (C, H, W)
```

**Under the hood:** `vips_thumbnail` → `vips_colourspace(sRGB)` → flatten alpha → cast
to UCHAR → copy to memory → convert HWC uint8 → CHW float32.

**Normalisations supported:**

| Use case | `mean` | `std_dev` | `bgr` |
|:---------|:-------|:---------|:------|
| ImageNet pretrained (default) | `{0.485, 0.456, 0.406}` | `{0.229, 0.224, 0.225}` | false |
| OpenCV-trained model | same mean/std | same | **true** |
| No normalisation (raw [0,1]) | — | — | `normalize=false` |

---

## Classification — `OnnxClassify`

Works with any single-input float32[1,3,H,W] model that produces float32[1,N_CLASSES]
logits.  Tested with SqueezeNet 1.1 and MobileNetV2.

```cpp
#include "lib/onnx/OnnxClassify.hpp"
#include "lib/onnx/OnnxSession.hpp"

media::onnx::OnnxSession session;
session.load("squeezenet1.1-7.onnx", 4);

auto result = media::onnx::onnx_classify(
    session,
    "photo.jpg",
    /* top_n */     5,
    /* labels */    "imagenet-classes.txt"   // optional
);

for (const auto& p : result.top_n)
    printf("[%d] %s  score=%.3f\n", p.class_idx, p.label.c_str(), p.score);
```

**Output struct:**
```cpp
struct ClassifyResult {
    bool ok;
    std::string error;
    int input_w, input_h;                  // actual preprocessed dimensions
    std::vector<ClassifyPrediction> top_n; // sorted by score descending
};
struct ClassifyPrediction {
    int class_idx; float score; std::string label;
};
```

### CLI

```bash
tanit-cli onnx classify \
  --model dist/models/squeezenet1.1-7.onnx \
  --image photo.jpg \
  --labels dist/models/imagenet-classes.txt \
  --top 5
```

```json
{
  "ok": true,
  "model": "squeezenet1.1-7.onnx",
  "image": "photo.jpg",
  "input_w": 224, "input_h": 224,
  "predictions": [
    { "class_idx": 281, "score": 8.12, "label": "tabby cat" },
    { "class_idx": 285, "score": 7.34, "label": "Egyptian cat" }
  ]
}
```

---

## Text detection — `OnnxOcr`

Phase 1 of a two-stage OCR pipeline using PP-OCRv4-compatible models.

### Architecture

```
image
  → OnnxImagePrep (resize ≤960px, divisible by 32, ImageNet norm)
  → PP-OCRv4-det.onnx  [float32(1,3,H,W) → float32(1,1,H,W)]
  → threshold (default 0.3) + span-merge → OcrBox[]

                         ┌── TODO ──────────────────────────────────────────┐
each detected crop        │ OnnxImagePrep (48×W)                             │
  → PP-OCRv4-rec.onnx    │ [float32(1,3,48,W) → float32(T,1,N_CHARS)]       │
  → CTC greedy decode    │ + char dictionary → "TOTAL $24.99"               │
                         └──────────────────────────────────────────────────┘
```

### Current detection API

```cpp
#include "lib/onnx/OnnxOcr.hpp"

media::onnx::OnnxSession det;
det.load("en_PP-OCRv4_det.onnx", 4);

auto result = media::onnx::onnx_ocr_detect(
    det, "receipt.jpg",
    /* threshold */ 0.3f,
    /* max_size  */ 960
);

printf("map_max=%.3f  boxes=%zu\n", result.map_max, result.boxes.size());
for (const auto& b : result.boxes)
    printf("  box [%.0f,%.0f,%.0f,%.0f] score=%.2f\n",
           b.x, b.y, b.w, b.h, b.score);
```

### CLI

```bash
tanit-cli onnx ocr \
  --det-model dist/models/en_PP-OCRv4_det.onnx \
  --image     receipt.jpg \
  --threshold 0.3 \
  --max-size  960
```

```json
{
  "ok": true,
  "image": "receipt.jpg",
  "image_w": 640, "image_h": 480,
  "map_w": 640,   "map_h": 480,
  "map_max": 0.94,
  "map_mean": 0.08,
  "box_count": 7,
  "boxes": [
    { "x": 18, "y": 24, "w": 210, "h": 18, "score": 0.81 }
  ],
  "note": "detection only; recognition (text readout) requires --rec-model (TODO)"
}
```

---

## `onnx run --image` (generic image input)

The existing `onnx run` subcommand now accepts `--image` to preprocess any image
and feed it as the first input tensor, replacing the previous zero-input default:

```bash
tanit-cli onnx run \
  --model dist/models/squeezenet1.1-7.onnx \
  --image photo.jpg
```

Dynamic-shape models (e.g. PP-OCR detection with `float32[1,3,-1,-1]`) work correctly:
the tensor shape `[1, 3, H, W]` is derived from the preprocessed image size and passed
via `OnnxSession::run_with_shapes()`.

---

## Models

### Classification

| Model | Size | Input | Classes | Download |
|:------|:-----|:------|:--------|:---------|
| **SqueezeNet 1.1** | ~5 MB | 224×224 | 1000 (ImageNet) | [ONNX Model Zoo](https://media.githubusercontent.com/media/onnx/models/main/validated/vision/classification/squeezenet/model/squeezenet1.1-7.onnx) |
| **MobileNetV2** | ~14 MB | 224×224 | 1000 (ImageNet) | [ONNX Model Zoo](https://media.githubusercontent.com/media/onnx/models/main/validated/vision/classification/mobilenet/model/mobilenetv2-12.onnx) |
| **EfficientNet-Lite4** | ~20 MB | 300×300 | 1000 (ImageNet) | [ONNX Model Zoo](https://media.githubusercontent.com/media/onnx/models/main/validated/vision/classification/efficientnet-lite4/model/efficientnet-lite4-11.onnx) |

**ImageNet labels file (1000 lines):**
```
https://raw.githubusercontent.com/pytorch/hub/master/imagenet_classes.txt
```

### OCR

| Model | Size | Input | Output | Source |
|:------|:-----|:------|:-------|:-------|
| **en_PP-OCRv3_det_infer** | ~2.4 MB | float32[1,3,-1,-1] | prob map [1,1,H,W] | [SWHL/RapidOCR](https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv4/en_PP-OCRv3_det_infer.onnx?download=true) |
| **en_PP-OCRv4_rec_infer** | ~7.7 MB | float32[1,3,48,W] | logits [T,1,97] | [breezedeus/cnocr-ppocr-en_PP-OCRv4](https://huggingface.co/breezedeus/cnocr-ppocr-en_PP-OCRv4/resolve/main/en_PP-OCRv4_rec_infer.onnx?download=true) |
| **ch_PP-OCRv4_det_infer** | ~4.75 MB | float32[1,3,-1,-1] | prob map [1,1,H,W] | [Desperado-JT/CH-PP-OCRv4](https://huggingface.co/Desperado-JT/CH-PP-OCRv4/resolve/main/ch_PP-OCRv4_det_infer.onnx?download=true) |

---

## Testing

```bash
npm run build:cpp
npm run test:onnx:images             # classify + run --image + ocr (downloads models)
npm run test:onnx:images:skip-ocr    # classification only (faster CI)
npm run test:onnx                    # baseline MNIST test (unchanged)
```

The test script auto-downloads all models and test images to `dist/models/` and
`tests/assets/vision/` on the first run.  Subsequent runs use the cached files.

---

## TODOs

### High priority

- [ ] **OCR recognition** (`OnnxOcr.cpp`): implement `onnx_ocr_recognise()` using
  `en_PP-OCRv4_rec.onnx`.  Requires:
  - Per-box crop + warp → float32[1,3,48,W] via `OnnxImagePrep`
  - CTC greedy decode (`argmax` over time steps, collapse blanks/repeats)
  - Character dictionary file (`en_dict.txt` — 96 Latin chars + blank)
  - CLI: `tanit-cli onnx ocr --rec-model <path> [--char-dict <path>]`

- [ ] **DB post-processing** (`OnnxOcr.cpp`): replace span-merge with proper
  Differentiable Binarisation post-processing:
  - Binary threshold on probability map
  - Connected-component contour extraction (without OpenCV: either port a small
    contour finder or add stb_image as a dependency for this)
  - Minkowski polygon unclip for tight bounding boxes
  - Rotated-box support via minimum-area-rectangle fitting

- [ ] **`image_ocr` agent tool** (`src/llm/tools/pictures/`): expose OCR as a
  new agent-callable tool.  Offline alternative to `image_understand` for
  text-heavy documents.  Return value: `{ text_lines: [...], boxes: [...] }`.

### Medium priority

- [ ] **`image_classify` agent tool**: expose classification as a tool for
  offline image tagging without an LLM call.

- [ ] **Object detection** (`OnnxDetect.hpp/.cpp`): YOLOv8n integration.
  - Input: float32[1,3,640,640]
  - Output: float32[1,84,8400] → NMS → bounding boxes + class labels
  - Model: `yolov8n.onnx` (~6 MB, 80 COCO classes)
  - `image_detect` agent tool exposing offline bounding-box detection

- [ ] **Background removal** (`OnnxRmbg.hpp/.cpp`): RMBG-1.4 ONNX.
  - Output: soft alpha mask → composite with libvips → PNG with transparency
  - New CLI verb: `tanit-cli onnx rmbg --image photo.jpg --output out.png`

- [ ] **`int64` input tensor support** in `OnnxSession`: required for BERT-family
  NLP models (token IDs).  Extend `run_with_shapes()` to accept a typed-tensor
  variant instead of `vector<float>`.

### Low priority

- [ ] **Batch image classification**: `onnx classify --src *.jpg --dst results.json`
- [ ] **CLIP image encoder** (`OnnxClip.hpp`): float32[1,3,224,224] → float32[1,512]
  embedding.  Alternative to GGUF for offline semantic image search.
- [ ] **Quantised (INT8/UINT8) models**: test pipeline with quantised SqueezeNet;
  current `prepare_image_tensor` outputs float32 which ORT auto-converts.

---

## Architecture notes

### Why lib, not just CLI

The three new source files (`OnnxImagePrep`, `OnnxClassify`, `OnnxOcr`) are in
`src/lib/onnx/` and linked into `pm-media` — the same shared/static library used by
the REST server, IPC server, and agent pipeline.  This means:

- The agent can call `onnx_classify()` directly in a new `image_classify` tool
  without spawning a subprocess.
- The REST server can expose `/v1/classify` and `/v1/ocr` using the same code paths.
- Future UI work (e.g. live camera classification) can call the lib directly.

### `OnnxSession::run_with_shapes()`

Added alongside the existing `run()`.  Needed because detection models like
PP-OCRv4-det declare `float32[1,3,-1,-1]` (fully dynamic spatial dims).  If you call
`run()` with a precomputed tensor, ORT would receive the tensor with the correct data
but the wrong symbolic shape — `run_with_shapes()` passes the actual `[1,3,H,W]`
shape to `Ort::Value::CreateTensor` instead of resolving `-1 → 1`.

### Image preprocessing pipeline

```
path  →  vips_thumbnail (shrink-on-load, aspect-preserving or forced)
      →  vips_colourspace(sRGB)      // greyscale, CMYK, LAB → RGB
      →  vips_flatten (white bg)     // PNG RGBA, WebP alpha → RGB
      →  vips_cast(UCHAR)            // ensure 0-255 bytes
      →  vips_image_copy_memory      // resident pixel array
      →  HWC uint8 → CHW float32     // (pixel/255 − mean) / std
```

`vips_thumbnail` is the key efficiency win: libvips uses shrink-on-load for JPEG
(libjpeg DCT shrink), PNG (pre-shrink), and TIFF (sub-file), so a 12 MP RAW photo
resized to 224×224 costs a fraction of a full decode.

# YOLO tasks in PixlWiz

How the `videoDetect` block / `onnx_detect` pipeline maps onto the YOLO model zoo
now sitting in `dist/models`, what each task variant emits, and what we still need
to build. **detect, classify, obb and pose** now decode through the shared
`src/video/detect/` library (CLI `--task`); **seg** and **sem** are still
unimplemented (planned with yolo26). See [Implementation](#implementation--src-video-detect-).

## Models on disk (`dist/models`)

Three families, five sizes (`n` < `s` < `m` < `l` < `x`), six task suffixes:

| Family   | detect | `-cls` | `-seg` | `-obb` | `-pose` | `-sem` |
|----------|:------:|:------:|:------:|:------:|:-------:|:------:|
| yolov8   |  ✅(l,n) |  l     |  l,n   |  l     |  l,n    |  —     |
| yolo11   |  ✅     | n…x    | n…x    | n…x    | n…x     |  —     |
| yolo26   |  ✅     | n…x    | n…x    | n…x    | n…x     | n…x    |

`yolov8n.onnx` is the one the `video-detect.xblox` fixture points at today.

> ⚠️ **Family caveat — YOLO26 is end-to-end / NMS-free.** v8 & v11 emit the
> classic transposed head `[1, 4+nc, 8400]` that our decoder + greedy NMS expects.
> YOLO26 (and v10) export an **NMS-free** head — typically `[1, 300, 6]`
> (`x1,y1,x2,y2,score,cls`, already top-k filtered). Feeding a yolo26 model
> through the current decoder will misparse. Decode must branch on output rank/shape
> per family (see TODOs).

## Task reference

| Task | Suffix | ONNX output(s) (640 input) | Decode | Overlay | Status |
|------|--------|----------------------------|--------|---------|--------|
| Detect       | *(none)* | `[1, 4+nc, 8400]` (v8/11) · `[1, 300, 6]` (v26) | box decode + per-class NMS (v8/11) / passthrough (v26) | rect | ✅ v8/11 (`decode_detect`) |
| Classify     | `-cls`   | `[1, nc]` (probabilities), **224×224 input** | argmax / top-k, softmax | label/badge | ✅ `decode_classify` |
| Segment      | `-seg`   | `out0 [1, 4+nc+32, 8400]` + `proto [1, 32, 160, 160]` | detect head → 32 mask coeffs · proto → per-box mask → threshold → upsample | mask fill + **contours** | ❌ (yolo26 later) |
| OBB          | `-obb`   | `[1, 4+nc+1, N]` (+1 = angle θ rad), **1024 input** | box+θ decode, **rotated NMS** (probIoU) | rotated quad | ✅ `decode_obb` |
| Pose         | `-pose`  | `[1, 4+1+K*3, N]` (nc=1 person, K=17 kpts `x,y,vis`) | box decode + keypoint unmap | skeleton | ✅ `decode_pose` |
| Semantic seg | `-sem`   | yolo26 only — per-pixel class map `[1, nc, h, w]` *(confirm)* | argmax per pixel | class-colored layer | ❌ (yolo26 later) |

`nc` = number of classes (80 for COCO detect/seg/pose-as-1, 1000 for cls/ImageNet,
15 for DOTA obb). `N` = anchors (8400 at 640).

## What each task does

### `-cls` — image classification (the one you weren't sure about)
**No boxes.** It answers "what is this *whole image*?" — a single probability
vector over the class set (ImageNet-1k for the stock weights → `imagenet-classes.txt`
is already in `dist/models`). Decode = softmax + argmax (or top-k). Two gotchas
vs detect:
- **Input is 224×224**, not 640, and uses center-crop/resize, **no letterbox**.
- No NMS, no anchors. Output is just `[1, nc]`.

Use case: tag a frame/region ("cat", "laptop") cheaply. Best surfaced as its own
block (`videoClassify`) or a `task: classify` mode, since the prep + result shape
differ from detect.

### `-seg` — instance segmentation (+ contours)
Detect head **plus** 32 mask coefficients per box, and a separate `proto` tensor
of 32 mask prototypes at 160×160. Per kept box: `mask = sigmoid(coeffs · protos)`
→ crop to the box → threshold @0.5 → upsample to image size. That gives a binary
mask per instance. **Contours** = polygon outlines of those masks.

### contours — yes, effectively OpenCV-only
We turn the binary mask into polygons with `cv::findContours` (OpenCV `imgproc`),
then optionally simplify with `cv::approxPolyDP`. OpenCV is already linked into
`pm-media` behind **`FEATURE_OPENCV`** (`CMakeLists.txt` → `OpenCVPipeline.cpp`), so
no new dep. A hand-rolled marching-squares is the only non-OpenCV alternative and
isn't worth it. → Contours should be gated on `FEATURE_OPENCV`.

### `-obb` — oriented bounding boxes
Adds one channel: the rotation angle θ. Boxes are `(cx, cy, w, h, θ)` rather than
axis-aligned. Needs **rotated-IoU NMS** (probIoU) instead of the axis-aligned IoU
we have. Trained on DOTA (aerial) — 15 classes, not COCO. Overlay draws a rotated
quad (4 corners) instead of a rect.

### `-pose` — keypoints
Single class (person) + K=17 COCO keypoints, each `(x, y, visibility)`. Decode is
detect + unmap the keypoints from model space. Overlay = skeleton (keypoints +
COCO bone pairs).

### `-sem` — semantic segmentation (yolo26 only)
Per-pixel class labels (no instances) — one big class map for the frame. Decode =
argmax over the class channel per pixel; render as a translucent class-colored
layer. Output layout needs confirming against a real `-sem` export.

## Implementation (`src/video/detect/`)

A task-agnostic decode library lives in **`src/video/detect/`** (namespace
`media::vdetect`), struct-based (no JSON in the hot path):

| File | Role |
|------|------|
| `types.hpp`        | `Task` enum, `Detection` (+`angle`/`keypoints`), `ClassScore`, `DecodeResult` |
| `commons.{hpp,cpp}`| IoU, **probIoU** + rotated NMS, sigmoid/softmax, head-shape parse, `autodetect_task`, coord unmap, label loaders (`load_label_lines`, `dota15_labels`, COCO pose skeleton) |
| `boxes.{hpp,cpp}`  | `decode_detect` — axis-aligned head |
| `cls.{hpp,cpp}`    | `decode_classify` — top-k |
| `obb.{hpp,cpp}`    | `decode_obb` — oriented boxes + rotated NMS |
| `pose.{hpp,cpp}`   | `decode_pose` — person box + keypoints |
| `seg.{hpp,cpp}`    | `decode_segment` — proto·coeff mask → pure-C++ contour |
| `serialize.{hpp,cpp}` | JSON boundary (`result_to_json`) shared by CLI + block |
| `render.{hpp,cpp}` | **shared software rasterizer** — `Canvas` + primitives (line/poly/disc/rect) + embedded 8×8 font + per-task dispatcher (`draw_detection`). Zero-dep; drives stills, burn-in video and (future) stream. |

- **CLI** (`tanit-cli video detect`): `--task auto|detect|classify|obb|pose`
  (auto = detect vs classify from output shape; obb/pose explicit), `--top-k` for
  classify. Input size auto-derives from the model's static dim (detect 640,
  cls 224, **obb 1024**), so no `--input-size` needed. Decoders + JSON come from
  the shared lib; the old inline `decode_yolo`/`nms_`/`iou_` are gone.
- **`OnnxDetect.{hpp,cpp}`** now delegates its detect decode to
  `vdetect::decode_detect` (single source of truth; legacy `DetectResult` surface
  preserved for the xblox block + existing CLI image commands).
- **`videoDetect` xblox block**: now task-aware via a `task` param
  (`auto|detect|classify|obb|pose`, dropdown) + `topK` (classify). Captures one
  frame (webcam/screen), prepares per-task (letterbox vs stretch), runs the model,
  and dispatches to the shared decoders; result is serialized with
  `vdetect::result_to_json` under the same `{ok, image_w, image_h, infer_ms, …}`
  envelope (`boxes` for detect/obb/pose, `classes` for classify). Class filter +
  Win32 overlay/smoother apply to the box tasks; container children fire when the
  pass yields ≥1 result. The cached `VideoDetectInstance` rebuilds its default
  label set when the task changes (COCO / DOTA-15 / ImageNet). Default labels for
  classify load `imagenet-classes.txt` from beside the model. Fixtures:
  `tests/xblox/video-detect{,-obb,-cls,-pose}.xblox`.

## Done
- [x] `src/video/detect/` lib: `Task` enum + `autodetect_task` (detect vs classify).
- [x] `decode_detect`, `decode_classify`, `decode_obb` (probIoU NMS), `decode_pose`.
- [x] DOTA-15 + ImageNet (auto-loaded `imagenet-classes.txt`) + COCO label sets.
- [x] CLI `--task` / `--top-k`; per-task prep (letterbox vs stretch) + input-size autoselect.
- [x] `OnnxDetect` dedup → delegates to the lib.
- [x] xblox `videoDetect` block: `task` dropdown + `topK`, routed through the
      shared decoders; `boxes`/`classes` result via `result_to_json`; children fire
      on ≥1 result; task-aware default labels. Smoke-tested looped (cls/obb/pose).
- [x] Overlay fidelity (Win32 `DetectOverlay`): OBB draws the rotated quad from
      `(cx,cy,w,h,angle)`; pose draws keypoints + COCO skeleton; segment fills +
      strokes the mask polygon. Smoother carries angle/keypoints/contour.
- [x] **YOLO26 NMS-free heads**: `parse_head` classifies layout (classic
      `[1,attrs,A]` transposed vs yolo26 `[1,N,attrs]` end-to-end, attrs = smaller
      trailing dim). `decode_detect/obb/pose/segment` branch on it; the end-to-end
      path reads `[x1,y1,x2,y2,score,cls,…tail]` and skips NMS (already filtered).
      OBB e2e order confirmed empirically: `[cx,cy,w,h,score,cls,angle]`. Verified
      on `yolo26n{,-obb,-pose,-seg}` via xblox.
- [x] **Segmentation without OpenCV**: `decode_segment` reads `proto [1,32,160,160]`
      + 32 per-box coeffs → `sigmoid(Σ coeff·proto)` → threshold 0.5 → crop →
      Moore-neighbor boundary trace + Douglas-Peucker → `Detection::contour`
      (original px polygon). Handles classic (NMS) + yolo26 (e2e) heads. Serialized
      as `boxes[].contour:[[x,y],…]`; overlay fills + strokes it. No deps.
- [x] Per-task xblox fixtures: `video-detect{,-obb,-cls,-pose}.xblox` (v8/v11) and
      `video-detect-yolo26{,-cls,-obb,-pose,-seg}.xblox` (yolo26 end-to-end).
- [x] **Shared rasterizer** (`render.{hpp,cpp}`): one per-task drawer (box / rotated
      quad / pose skeleton+dots / segment contour + label chip) onto a generic
      packed `Canvas` (BGR/RGB, 3/4-ch), pure C++ + embedded 8×8 font. The vips
      **still** path (`draw_detections_to_file`, replacing the boxes-only overlay)
      and the **burn-in video** both go through it; the Win32 GDI+ overlay shares
      its geometry (`obb_corners`, `coco_pose_skeleton`, palette). Verified: stills
      now render full per-task geometry + labels (not boxes-only).
- [x] **Burn-in `.mp4`** (`core/video_encoder.{hpp,cpp}`): frame-fed H.264 MP4 via
      Media Foundation `IMFSinkWriter` (RGB32→H.264; non-Win32 stub). `videoDetect`
      block gains `recordPath` + `recordFps`: each captured frame is annotated by
      the shared rasterizer and appended; the encoder lives in the cached
      `VideoDetectInstance` so a `--loop` run accumulates one file, finalized when
      the instance is released (loop end / `xblox session stop`). Verified: valid
      `ftyp/mp42` MP4, single- and multi-frame, with drawn boxes. Fixture:
      `tests/xblox/video-detect-record.xblox`.

## Next TODOs

### 1. Semantic seg (`-sem`, yolo26)
- [ ] `-sem` output is `[1, 19, 1024, 2048]` (Cityscapes-19 logits). argmax per
      pixel → class map; translucent colored overlay layer; optional PNG out.
      Needs a new `DecodeResult` surface (class map, not boxes) + overlay path.

### 2. OpenCV-gated extras (deferred — opencv is a publishing nightmare)
- [ ] Concrete design TBD. Current contour tracer is pure C++ and ships everywhere;
      revisit only if we need richer mask ops (sub-pixel polygons, holes, morphology).
      vips is available as an alternative for raster mask work if needed.

### 3. Output / streaming sinks (`frame_source.hpp`)
- [x] `RecordSink` (annotated frames → `.mp4`) — done via `VideoEncoder` + block
      `recordPath` (see above).
- [ ] CLI parity: `video detect --record out.mp4` (the encoder + rasterizer are
      ready; only the CLI loop wiring is left).
- [ ] `StreamSink` (Sunshine-style, window/screen level) — tap the *same* annotated
      frames produced for burn-in and packetise (RTP/WebRTC) so an LLM can watch /
      drive this machine. The rasterizer + frame-fed pipeline already make the
      annotated `VideoFrame` the single fan-out point; only the network sink is new.

### 4. Cross-cutting
- [ ] Test harness: assert per-task result shape in `tests/orchestrator`.
- [ ] Future "filters / dictionaries" layer on top of `DecodeResult` (temporal
      memory / tracking, frame diffs) — design TBD.
- [ ] Document result schemas in `docs/video-detection.md` once block wiring lands.

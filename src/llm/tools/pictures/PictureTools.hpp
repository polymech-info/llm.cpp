// Copyright PolyMech / pm-image. Internal use only.
//
// Image-oriented agent tools:
//   - image_resize       — libvips resize / re-encode (exposed)
//   - image_crop         — libvips pixel crop (exposed)
//   - image_compress     — MozJPEG / oxipng (internal)
//   - image_transform    — AI edit (exposed)
//   - image_create       — AI text→image (exposed)
//   - image_understand   — vision Q&A (exposed)
//   - image_meta         — sidecar generation (internal — used by image_find)
//   - image_from_camera  — webcam capture (exposed; FEATURE_VIDEO)
//
// Plus the thread-local file-progress hook used by image_transform to surface
// per-file results to the UI before the whole batch finishes.
#pragma once

#include "llm/tools/commons/PathToolCommons.hpp"

namespace media::llm::path {
struct ToolDef;
}

namespace media::llm::path::detail {

ExecuteResult do_resize(const nlohmann::json& args);
ExecuteResult do_crop(const nlohmann::json& args);
ExecuteResult do_compress(const nlohmann::json& args);
ExecuteResult do_transform(const nlohmann::json& args);
ExecuteResult do_create_image(const nlohmann::json& args);
ExecuteResult do_meta(const nlohmann::json& args);
ExecuteResult do_understand(const nlohmann::json& args);
ExecuteResult do_ocr(const nlohmann::json& args);
ExecuteResult do_camera(const nlohmann::json& args);

ToolDef make_resize();
ToolDef make_crop();
ToolDef make_transform();
ToolDef make_image_create();
ToolDef make_understand();
ToolDef make_ocr_text();
ToolDef make_camera();

// Public API plumbing — called from path_tool_executor.cpp's exported
// `set_image_transform_file_progress_hook` / `clear_*`.
void set_image_transform_file_progress(std::function<void(const nlohmann::json&)> f);
void clear_image_transform_file_progress();

} // namespace media::llm::path::detail

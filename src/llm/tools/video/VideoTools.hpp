// Copyright PolyMech / pm-image. Internal use only.
//
// `create_video` agent tool — Tanit `/v1/videos` (OpenRouter Veo) or Replicate
// text-to-video / image-to-video. The catalog schema is enriched at build time
// from Tanit `model_info.meta` or the Replicate OpenAPI Input of the currently
// selected Chat → Video model (see `enrich_create_video_from_replicate_cache`).
#pragma once

#include "llm/tools/commons/PathToolCommons.hpp"

namespace media::llm::path {
struct ToolDef;
}

namespace media::llm::path::detail {

ExecuteResult do_create_video(const nlohmann::json& args, const std::string& tool);

ToolDef make_create_video();

/// Replace `create_video` `input_schema` from the current Chat → Video model:
/// Tanit video aliases use catalog meta; Replicate slugs use OpenAPI Input.
void enrich_create_video_from_replicate_cache(std::vector<ToolDef>& tools);

} // namespace media::llm::path::detail

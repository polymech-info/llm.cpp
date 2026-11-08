#pragma once
//
// media::llm::tool_catalog — JSON-Schema descriptions of the pm-image
// operations as agent-callable tools.
//
// The schemas are derived by hand from the existing
// `media::apply_<op>_options_from_json` mappers (see src/core/*.hpp). Adding
// a new option is a two-place change: the mapper, and the schema below.
//
// See docs/llm-tools.md for the full design / consumer story.
//
#include "polymech_export.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace media::llm {

struct ToolDef {
    std::string    name;          // e.g. "image_resize"
    std::string    description;   // one-liner shown to the agent
    nlohmann::json input_schema;  // JSON Schema (object)
};

/// All tools available in this build.
PM_CHAT_API std::vector<ToolDef> tool_catalog();

/// Same data, JSON-encoded as `{ "tools": [ {name, description, input_schema}, … ] }`.
PM_CHAT_API nlohmann::json tool_catalog_json();

} // namespace media::llm

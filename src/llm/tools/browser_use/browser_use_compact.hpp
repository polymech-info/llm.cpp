#pragma once

#include "polymech_export.h"

#include <nlohmann/json.hpp>

#include <string>

namespace media::llm::browser_use {

POLYMECH_API bool is_browser_history_tool(const std::string& tool);

/// Slim browser_read/browser_find `data` for model history. When keep_items is false,
/// drops items/pageText and keeps count/scope stubs.
POLYMECH_API nlohmann::json compact_read_data(const nlohmann::json& data, bool keep_items);

/// Slim browser_batch payload (top-level envelope fields, not nested under data).
POLYMECH_API nlohmann::json compact_batch_envelope(nlohmann::json envelope, bool keep_steps);

/// Slim click/type/select/scope/navigate envelopes. keep_detail=false → stub only.
POLYMECH_API nlohmann::json compact_action_envelope(nlohmann::json envelope, bool keep_detail);

/// Compact a browser tool result envelope before it enters agent history.
POLYMECH_API nlohmann::json compact_tool_envelope_for_history(nlohmann::json envelope);

} // namespace media::llm::browser_use

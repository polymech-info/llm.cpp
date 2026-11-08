#pragma once
//
// media::llm::execute — invoke a tool by name with a JSON arguments envelope.
// Decodes base64 image inputs, dispatches to the appropriate buffer worker
// from src/core/, and returns the standard envelope:
//
//   binary out: { "ok": true, "mime": "image/...", "bytes": <int>, "b64": "..." }
//   json   out: { "ok": true, "result": { ... } }
//   failure   : { "ok": false, "error": "..." }
//
// See docs/llm-tools.md §4 for the envelope contract.
//
#include "polymech_export.h"

#include <nlohmann/json.hpp>

#include <string>

namespace media::llm {

struct ExecuteResult {
    bool           ok = false;
    std::string    error;     // populated on failure (also mirrored into envelope.error)
    nlohmann::json envelope;  // always populated; safe to ship verbatim
};

/// Run a tool by catalog name (e.g. "image_compress") with the given arguments.
/// Returns an ExecuteResult whose `envelope` follows the §4 contract.
PM_CHAT_API ExecuteResult execute(const std::string& name, const nlohmann::json& arguments);

} // namespace media::llm

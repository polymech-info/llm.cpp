#pragma once
//
// JSON structure handler (ported from headroom's
// compression/handlers/json_handler.py).
//
// Produces a byte-aligned StructureMask over JSON content that PRESERVES the
// navigational structure (keys, brackets, colons, commas, booleans, nulls,
// short numbers, short string values, high-entropy identifier values) and marks
// the rest (long string values, whitespace, deep array items) as compressible.
//
// This lets an LLM see the full schema while bulky values are reduced.
//
// Requires: FEATURE_LLM_COMPRESSION=1.
//
#include <cstddef>
#include <string_view>

#include "structure_mask.hpp"

namespace media::llm::compress {

struct JsonHandlerOptions {
    bool        preserve_short_values  = true;
    std::size_t short_value_threshold  = 20;   ///< max length (incl. quotes) for a "short" value
    bool        preserve_high_entropy  = true;
    double      entropy_threshold      = 0.85;
    std::size_t max_array_items_full   = 3;     ///< keep first N array items fully
    std::size_t max_number_digits      = 10;    ///< preserve numbers up to N chars (often IDs)
};

struct JsonHandlerResult {
    StructureMask mask;
    std::size_t   token_count = 0;
    std::size_t   key_count   = 0;
};

/// True if `content` starts with '{' or '[' and parses as valid JSON.
bool json_can_handle(std::string_view content);

/// Build the structural preservation mask for JSON content.
JsonHandlerResult json_structure_mask(std::string_view content,
                                      const JsonHandlerOptions& opts = {});

} // namespace media::llm::compress

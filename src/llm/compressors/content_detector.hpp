#pragma once
//
// Heuristic content-type detection for compression routing (ported from the
// FallbackDetector in headroom's compression/detector.py).
//
// The headroom MagikaDetector (ML model) is intentionally NOT ported: the
// fallback heuristics are dependency-free and sufficient for routing JSON vs
// text, which is the scope of the first compression slice.
//
// Requires: FEATURE_LLM_COMPRESSION=1.
//
#include <string>
#include <string_view>

namespace media::llm::compress {

/// High-level content categories for compression routing.
/// `Auto` is an input-only sentinel meaning "detect"; detect() never returns it.
enum class ContentType {
    Auto = 0,   ///< request-only: run detection
    Json,
    Code,
    Log,
    Markdown,
    Diff,
    Text,
    Unknown,
};

const char* content_type_name(ContentType t) noexcept;

/// Parse a CLI-friendly type string ("json", "text", "auto", …).
ContentType parse_content_type(std::string_view s) noexcept;

struct DetectionResult {
    ContentType content_type   = ContentType::Unknown;
    double      confidence      = 0.0;   ///< 0..1
    std::string raw_label       = "unknown";
};

/// Detect the content type using cheap heuristics:
///  - JSON: starts with '{' or '[' AND parses as valid JSON.
///  - Code: contains a common code keyword ("def ", "class ", "fn ", …).
///  - Log:  contains a level token (ERROR / WARN / INFO / DEBUG / FATAL).
///  - else: Text.
DetectionResult detect_content(std::string_view content);

} // namespace media::llm::compress

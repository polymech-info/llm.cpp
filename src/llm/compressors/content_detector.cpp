#include "content_detector.hpp"

#include <array>
#include <cctype>
#include <nlohmann/json.hpp>

namespace media::llm::compress {

const char* content_type_name(ContentType t) noexcept
{
    switch (t) {
        case ContentType::Auto:     return "auto";
        case ContentType::Json:     return "json";
        case ContentType::Code:     return "code";
        case ContentType::Log:      return "log";
        case ContentType::Markdown: return "markdown";
        case ContentType::Diff:     return "diff";
        case ContentType::Text:     return "text";
        case ContentType::Unknown:
        default:                    return "unknown";
    }
}

ContentType parse_content_type(std::string_view s) noexcept
{
    if (s == "json")     return ContentType::Json;
    if (s == "code")     return ContentType::Code;
    if (s == "log")      return ContentType::Log;
    if (s == "markdown") return ContentType::Markdown;
    if (s == "diff")     return ContentType::Diff;
    if (s == "text")     return ContentType::Text;
    if (s == "auto" || s.empty()) return ContentType::Auto;
    return ContentType::Unknown;
}

namespace {

std::string_view lstrip(std::string_view s) noexcept
{
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            ++i;
        } else {
            break;
        }
    }
    return s.substr(i);
}

std::string_view strip(std::string_view s) noexcept
{
    s = lstrip(s);
    while (!s.empty()) {
        const unsigned char c = static_cast<unsigned char>(s.back());
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            s.remove_suffix(1);
        } else {
            break;
        }
    }
    return s;
}

bool contains(std::string_view hay, std::string_view needle) noexcept
{
    return hay.find(needle) != std::string_view::npos;
}

} // namespace

DetectionResult detect_content(std::string_view content)
{
    const std::string_view stripped = strip(content);
    if (stripped.empty()) {
        return DetectionResult{ContentType::Unknown, 0.0, "empty"};
    }

    // JSON: cheap prefix check, then a no-throw validity probe.
    const char first = stripped.front();
    if (first == '{' || first == '[') {
        if (nlohmann::json::accept(stripped)) {
            return DetectionResult{ContentType::Json, 1.0, "json"};
        }
    }

    // Code indicators (same set as headroom FallbackDetector).
    static constexpr std::array<std::string_view, 11> kCode = {
        "def ", "class ", "function ", "import ", "const ", "let ",
        "var ", "func ", "fn ", "pub ", "package ",
    };
    for (std::string_view ind : kCode) {
        if (contains(content, ind)) {
            return DetectionResult{ContentType::Code, 0.7, "code"};
        }
    }

    // Log level tokens.
    static constexpr std::array<std::string_view, 5> kLog = {
        "ERROR", "WARN", "INFO", "DEBUG", "FATAL",
    };
    for (std::string_view ind : kLog) {
        if (contains(content, ind)) {
            return DetectionResult{ContentType::Log, 0.6, "log"};
        }
    }

    return DetectionResult{ContentType::Text, 0.5, "text"};
}

} // namespace media::llm::compress

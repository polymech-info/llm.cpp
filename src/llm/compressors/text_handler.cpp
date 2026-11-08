#include "text_handler.hpp"

#include <string>
#include <vector>

namespace media::llm::compress {

namespace {

constexpr bool is_space_or_tab(char c) noexcept
{
    return c == ' ' || c == '\t';
}

// Collapse intra-line space/tab runs to a single space and trim trailing
// horizontal whitespace, preserving the line's newline terminator. Returns the
// processed line WITHOUT the trailing '\n'.
std::string collapse_line(std::string_view line)
{
    std::string out;
    out.reserve(line.size());
    bool prev_space = false;
    for (char c : line) {
        if (is_space_or_tab(c)) {
            prev_space = true;
            continue;
        }
        if (prev_space && !out.empty()) out.push_back(' ');
        prev_space = false;
        out.push_back(c);
    }
    return out;
}

// True for UTF-8 continuation bytes (0b10xxxxxx). A valid codepoint boundary is
// any position whose byte is NOT a continuation byte (or the string end).
constexpr bool is_utf8_continuation(unsigned char c) noexcept
{
    return (c & 0xC0u) == 0x80u;
}

// Move `idx` left to the nearest UTF-8 codepoint boundary at or before it, so a
// prefix [0, idx) never ends inside a multibyte sequence. Bounded by 4 steps
// (max UTF-8 length) so malformed input can't loop unexpectedly.
std::size_t snap_boundary_left(std::string_view text, std::size_t idx) noexcept
{
    if (idx >= text.size()) return text.size();
    std::size_t steps = 0;
    while (idx > 0 && steps < 4 && is_utf8_continuation(static_cast<unsigned char>(text[idx]))) {
        --idx;
        ++steps;
    }
    return idx;
}

} // namespace

std::string reduce_text(std::string_view text, const TextReduceOptions& opts)
{
    if (text.empty()) return std::string(text);
    if (!opts.collapse_whitespace && !opts.dedup_consecutive_lines) {
        return std::string(text);
    }

    // Split into lines, keeping track of whether the input ended with a newline
    // so we can faithfully restore the trailing terminator.
    std::vector<std::string_view> raw_lines;
    {
        std::size_t start = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n') {
                raw_lines.push_back(text.substr(start, i - start));
                start = i + 1;
            }
        }
        raw_lines.push_back(text.substr(start));
    }
    const bool trailing_newline = !text.empty() && text.back() == '\n';
    // When the text ends in '\n', the split above produced a trailing empty
    // segment; drop it so we don't emit a spurious blank line.
    if (trailing_newline && !raw_lines.empty()) raw_lines.pop_back();

    std::vector<std::string> lines;
    lines.reserve(raw_lines.size());

    std::string  prev_emitted;
    bool         have_prev    = false;
    bool         prev_blank   = false;

    for (std::string_view rawv : raw_lines) {
        // strip a trailing '\r' (CRLF inputs)
        if (!rawv.empty() && rawv.back() == '\r') rawv.remove_suffix(1);

        std::string line = opts.collapse_whitespace ? collapse_line(rawv) : std::string(rawv);
        const bool blank = line.empty();

        // Collapse runs of blank lines to a single blank line.
        if (opts.collapse_whitespace && blank && prev_blank) {
            continue;
        }

        // Drop a non-blank line identical to the previously emitted one.
        if (opts.dedup_consecutive_lines && !blank && have_prev && line == prev_emitted) {
            continue;
        }

        if (!blank) {
            prev_emitted = line;
            have_prev    = true;
        }
        prev_blank = blank;
        lines.push_back(std::move(line));
    }

    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size()) out.push_back('\n');
    }
    if (trailing_newline) out.push_back('\n');
    return out;
}

std::string simple_truncate(std::string_view text, double ratio_target)
{
    if (ratio_target <= 0.0) ratio_target = 0.0;
    if (ratio_target >= 1.0) return std::string(text);

    const std::size_t target_len =
        static_cast<std::size_t>(static_cast<double>(text.size()) * ratio_target);
    if (text.size() <= target_len) return std::string(text);

    // Snap both cut points to UTF-8 codepoint boundaries so we never emit a
    // truncated multibyte sequence (invalid UTF-8 corrupts downstream consumers
    // and trips strict JSON serializers).
    const std::size_t keep_start = snap_boundary_left(text, (target_len * 2) / 3);
    std::size_t       keep_end   = target_len / 3;
    if (keep_end > text.size()) keep_end = text.size();
    // The suffix begins at size-keep_end; move that start to a boundary by
    // dropping the partial leading codepoint (shrinking keep_end).
    const std::size_t suffix_start = snap_boundary_left(text, text.size() - keep_end);
    keep_end                       = text.size() - suffix_start;

    std::string out;
    out.reserve(keep_start + keep_end + 24);
    out.append(text.substr(0, keep_start));
    out.append("\n...[compressed]...\n");
    if (keep_end > 0) {
        out.append(text.substr(text.size() - keep_end, keep_end));
    }
    return out;
}

} // namespace media::llm::compress

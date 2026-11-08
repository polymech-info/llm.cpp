#include "json_handler.hpp"

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "content_detector.hpp"

namespace media::llm::compress {

namespace {

enum class JsonTokenType {
    Key,
    StringValue,
    Number,
    Boolean,
    Null,
    Bracket,
    Colon,
    Comma,
    Whitespace,
};

struct JsonToken {
    std::string_view text;
    JsonTokenType    type;
    std::size_t      start;
    std::size_t      end;   ///< exclusive

    bool is_structural() const noexcept
    {
        switch (type) {
            case JsonTokenType::Key:
            case JsonTokenType::Boolean:
            case JsonTokenType::Null:
            case JsonTokenType::Bracket:
            case JsonTokenType::Colon:
            case JsonTokenType::Comma:
                return true;
            default:
                return false;
        }
    }
};

constexpr bool is_ws(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

constexpr bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

// Char-level JSON tokenizer — not a full parser, just enough to classify keys
// vs values and track structural punctuation (mirrors _tokenize_json).
std::vector<JsonToken> tokenize_json(std::string_view content)
{
    std::vector<JsonToken> tokens;
    const std::size_t      n = content.size();
    std::size_t            i = 0;

    bool                     expect_key = false;
    std::vector<char>        brace_stack;

    while (i < n) {
        const char ch = content[i];

        // Whitespace run
        if (is_ws(ch)) {
            const std::size_t start = i;
            while (i < n && is_ws(content[i])) ++i;
            tokens.push_back({content.substr(start, i - start), JsonTokenType::Whitespace, start, i});
            continue;
        }

        // Brackets
        if (ch == '{' || ch == '}' || ch == '[' || ch == ']') {
            tokens.push_back({content.substr(i, 1), JsonTokenType::Bracket, i, i + 1});
            if (ch == '{') {
                brace_stack.push_back('{');
                expect_key = true;
            } else if (ch == '}') {
                if (!brace_stack.empty() && brace_stack.back() == '{') brace_stack.pop_back();
                expect_key = false;
            } else if (ch == '[') {
                brace_stack.push_back('[');
                expect_key = false;
            } else { // ']'
                if (!brace_stack.empty() && brace_stack.back() == '[') brace_stack.pop_back();
            }
            ++i;
            continue;
        }

        // Colon
        if (ch == ':') {
            tokens.push_back({content.substr(i, 1), JsonTokenType::Colon, i, i + 1});
            expect_key = false;
            ++i;
            continue;
        }

        // Comma
        if (ch == ',') {
            tokens.push_back({content.substr(i, 1), JsonTokenType::Comma, i, i + 1});
            if (!brace_stack.empty() && brace_stack.back() == '{') expect_key = true;
            ++i;
            continue;
        }

        // String (key or value)
        if (ch == '"') {
            const std::size_t start = i;
            ++i;
            while (i < n && content[i] != '"') {
                if (content[i] == '\\') i += 2; // skip escaped char
                else ++i;
            }
            if (i < n) ++i; // include closing quote

            const std::string_view text = content.substr(start, i - start);

            // Look ahead (skipping whitespace) for a colon to decide key vs value.
            std::size_t j = i;
            while (j < n && is_ws(content[j])) ++j;
            const bool is_key = j < n && content[j] == ':' && expect_key;

            if (is_key) {
                tokens.push_back({text, JsonTokenType::Key, start, i});
                expect_key = false;
            } else {
                tokens.push_back({text, JsonTokenType::StringValue, start, i});
            }
            continue;
        }

        // Number
        if (ch == '-' || is_digit(ch)) {
            const std::size_t start = i;
            if (ch == '-') ++i;
            while (i < n && is_digit(content[i])) ++i;
            if (i < n && content[i] == '.') {
                ++i;
                while (i < n && is_digit(content[i])) ++i;
            }
            if (i < n && (content[i] == 'e' || content[i] == 'E')) {
                ++i;
                if (i < n && (content[i] == '+' || content[i] == '-')) ++i;
                while (i < n && is_digit(content[i])) ++i;
            }
            tokens.push_back({content.substr(start, i - start), JsonTokenType::Number, start, i});
            continue;
        }

        // Boolean / null literals
        if (content.substr(i, 4) == "true") {
            tokens.push_back({content.substr(i, 4), JsonTokenType::Boolean, i, i + 4});
            i += 4;
            continue;
        }
        if (content.substr(i, 5) == "false") {
            tokens.push_back({content.substr(i, 5), JsonTokenType::Boolean, i, i + 5});
            i += 5;
            continue;
        }
        if (content.substr(i, 4) == "null") {
            tokens.push_back({content.substr(i, 4), JsonTokenType::Null, i, i + 4});
            i += 4;
            continue;
        }

        // Unknown character — skip.
        ++i;
    }

    return tokens;
}

std::string_view strip_quotes(std::string_view s) noexcept
{
    while (!s.empty() && s.front() == '"') s.remove_prefix(1);
    while (!s.empty() && s.back() == '"')  s.remove_suffix(1);
    return s;
}

bool should_preserve(const JsonToken& token,
                     std::size_t      array_depth,
                     std::size_t      array_item_index,
                     const JsonHandlerOptions& opts)
{
    if (token.is_structural()) return true;
    if (token.type == JsonTokenType::Whitespace) return false;

    if (token.type == JsonTokenType::Number) {
        return token.text.size() <= opts.max_number_digits;
    }

    if (token.type == JsonTokenType::StringValue) {
        // Past the array-items threshold: be aggressive.
        if (array_depth > 0 && array_item_index >= opts.max_array_items_full) {
            return false;
        }
        if (opts.preserve_short_values && token.text.size() <= opts.short_value_threshold) {
            return true;
        }
        if (opts.preserve_high_entropy) {
            const std::string_view value = strip_quotes(token.text);
            // Entropy targets identifiers; English prose can self-normalize
            // above 0.85 too, so gate on the cheapest identifier signal: no
            // spaces (mirrors headroom).
            if (value.find(' ') == std::string_view::npos) {
                if (EntropyScore::compute(value, opts.entropy_threshold).should_preserve) {
                    return true;
                }
            }
        }
        return false;
    }

    return false;
}

} // namespace

bool json_can_handle(std::string_view content)
{
    // strip leading whitespace
    std::size_t i = 0;
    while (i < content.size() && is_ws(content[i])) ++i;
    const std::string_view stripped = content.substr(i);
    if (stripped.empty()) return false;
    const char c = stripped.front();
    if (c != '{' && c != '[') return false;
    return nlohmann::json::accept(stripped);
}

JsonHandlerResult json_structure_mask(std::string_view content, const JsonHandlerOptions& opts)
{
    const std::vector<JsonToken> tokens = tokenize_json(content);

    std::vector<bool> mask(content.size(), false);

    // Track enclosing containers so only commas whose innermost container is an
    // array advance that array's item index (mirrors the Python container/array
    // stacks).
    std::vector<char>        container_stack;
    std::vector<std::size_t> array_item_stack;
    std::size_t              key_count = 0;

    for (const JsonToken& token : tokens) {
        if (token.type == JsonTokenType::Key) ++key_count;

        if (token.type == JsonTokenType::Bracket) {
            const char c = token.text.front();
            if (c == '{' || c == '[') {
                container_stack.push_back(c);
                if (c == '[') array_item_stack.push_back(0);
            } else if (c == '}') {
                if (!container_stack.empty() && container_stack.back() == '{') container_stack.pop_back();
            } else if (c == ']') {
                if (!container_stack.empty() && container_stack.back() == '[') {
                    container_stack.pop_back();
                    if (!array_item_stack.empty()) array_item_stack.pop_back();
                }
            }
        }

        if (token.type == JsonTokenType::Comma && !container_stack.empty() &&
            container_stack.back() == '[' && !array_item_stack.empty()) {
            array_item_stack.back() += 1;
        }

        const bool preserve = should_preserve(
            token,
            array_item_stack.size(),
            array_item_stack.empty() ? 0 : array_item_stack.back(),
            opts);

        if (preserve) {
            for (std::size_t k = token.start; k < token.end && k < mask.size(); ++k) {
                mask[k] = true;
            }
        }
    }

    return JsonHandlerResult{StructureMask(std::move(mask)), tokens.size(), key_count};
}

} // namespace media::llm::compress

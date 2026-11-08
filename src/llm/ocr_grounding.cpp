#include "ocr_grounding.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>
#include "string/string_utils.hpp"

namespace media::llm {
namespace {

bool known_layout_label(std::string_view label);

bool is_space(unsigned char c)
{
    return std::isspace(c) != 0;
}

std::string_view trim_view(std::string_view s)
{
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

std::string trim_copy(std::string_view s)
{
    s = trim_view(s);
    return std::string(s);
}

void replace_all(std::string& s, std::string_view from, std::string_view to)
{
    if (from.empty())
        return;
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t at = s.find(from, i);
        if (at == std::string::npos) {
            out.append(s, i, std::string::npos);
            break;
        }
        out.append(s, i, at - i);
        out.append(to);
        i = at + from.size();
    }
    s.swap(out);
}

// Turn the token form into the same "label [x, y, x, y]" surface the
// decoded chat path already emits, so one scanner handles both.
std::string normalize_ref_det(std::string s)
{
    const std::string_view ref_open = "<|ref|>";
    const std::string_view ref_close = "<|/ref|>";
    const std::string_view det_open = "<|det|>";
    const std::string_view det_close = "<|/det|>";
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t ref = s.find(ref_open, i);
        if (ref == std::string::npos) {
            out.append(s, i, std::string::npos);
            break;
        }
        out.append(s, i, ref - i);
        const std::size_t label_b = ref + ref_open.size();
        const std::size_t label_e = s.find(ref_close, label_b);
        if (label_e == std::string::npos) {
            out.append(s, ref, std::string::npos);
            break;
        }
        const std::size_t det = s.find(det_open, label_e + ref_close.size());
        const std::size_t det_e = det == std::string::npos ? std::string::npos
                                                           : s.find(det_close, det + det_open.size());
        if (det == std::string::npos || det_e == std::string::npos
            || det > label_e + ref_close.size() + 8) {
            out.append(s, ref, label_e + ref_close.size() - ref);
            i = label_e + ref_close.size();
            continue;
        }
        std::string box = trim_copy(s.substr(det + det_open.size(), det_e - (det + det_open.size())));
        if (box.size() >= 2 && box.front() == '[' && box.back() == ']') {
            const std::string inner = trim_copy(std::string_view(box).substr(1, box.size() - 2));
            if (!inner.empty() && inner.front() == '[')
                box = inner;
        }
        out.push_back('\n');
        out.append(s, label_b, label_e - label_b);
        out.push_back(' ');
        if (!box.empty() && box.front() == '[')
            out.append(box);
        else {
            out.push_back('[');
            out.append(box);
            out.push_back(']');
        }
        out.push_back('\n');
        i = det_e + det_close.size();
    }
    return out;
}

bool parse_int(std::string_view s, std::size_t& i, int& out)
{
    while (i < s.size() && is_space(static_cast<unsigned char>(s[i])))
        ++i;
    if (i >= s.size())
        return false;
    const bool neg = s[i] == '-';
    if (neg || s[i] == '+')
        ++i;
    if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i])))
        return false;
    int v = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        v = v * 10 + (s[i] - '0');
        ++i;
    }
    out = neg ? -v : v;
    return true;
}

// [x1, y1, x2, y2] at s[open]. close is one past ']'.
bool parse_box(std::string_view s, std::size_t open, std::size_t& close)
{
    if (open >= s.size() || s[open] != '[')
        return false;
    std::size_t i = open + 1;
    int n = 0;
    for (int k = 0; k < 4; ++k) {
        if (!parse_int(s, i, n))
            return false;
        while (i < s.size() && is_space(static_cast<unsigned char>(s[i])))
            ++i;
        if (k < 3) {
            if (i >= s.size() || s[i] != ',')
                return false;
            ++i;
        }
    }
    if (i >= s.size() || s[i] != ']')
        return false;
    close = i + 1;
    return true;
}

bool is_ident(unsigned char c)
{
    return std::isalnum(c) || c == '_';
}

struct Block {
    std::string label;
    std::string body;
};

std::vector<Block> scan_blocks(std::string_view s)
{
    struct Hit {
        std::size_t start = 0; // label or '['
        std::size_t body  = 0; // after ']'
        std::string label;
    };
    std::vector<Hit> hits;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '[')
            continue;
        std::size_t close = 0;
        if (!parse_box(s, i, close))
            continue;
        std::size_t start = i;
        std::string label;
        if (i > 0) {
            std::size_t j = i;
            while (j > 0 && is_space(static_cast<unsigned char>(s[j - 1])))
                --j;
            std::size_t end = j;
            while (j > 0 && is_ident(static_cast<unsigned char>(s[j - 1])))
                --j;
            if (j < end && (j == 0 || is_space(static_cast<unsigned char>(s[j - 1])))) {
                label.assign(s.substr(j, end - j));
                if (known_layout_label(label))
                    start = j;
                else
                    label.clear();
            }
        }
        hits.push_back(Hit{start, close, std::move(label)});
        i = close - 1;
    }
    std::vector<Block> blocks;
    if (hits.size() < 2)
        return blocks;
    blocks.reserve(hits.size());
    for (std::size_t n = 0; n < hits.size(); ++n) {
        const std::size_t body_end = (n + 1 < hits.size()) ? hits[n + 1].start : s.size();
        Block b;
        b.label = hits[n].label;
        b.body = trim_copy(s.substr(hits[n].body, body_end - hits[n].body));
        blocks.push_back(std::move(b));
    }
    return blocks;
}

std::string decode_entities(std::string s)
{
    replace_all(s, "&amp;", "&");
    replace_all(s, "&lt;", "<");
    replace_all(s, "&gt;", ">");
    replace_all(s, "&quot;", "\"");
    replace_all(s, "&#39;", "'");
    replace_all(s, "&nbsp;", " ");
    return s;
}

void replace_ci_br(std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '<' && i + 3 < s.size()
            && (s[i + 1] == 'b' || s[i + 1] == 'B')
            && (s[i + 2] == 'r' || s[i + 2] == 'R')) {
            std::size_t j = i + 3;
            while (j < s.size() && s[j] != '>')
                ++j;
            if (j < s.size()) {
                out.push_back('\n');
                i = j + 1;
                continue;
            }
        }
        out.push_back(s[i]);
        ++i;
    }
    s.swap(out);
}

std::string strip_tags(std::string s)
{
    std::string out;
    out.reserve(s.size());
    bool in_tag = false;
    for (char c : s) {
        if (c == '<') {
            in_tag = true;
            continue;
        }
        if (c == '>' && in_tag) {
            in_tag = false;
            continue;
        }
        if (!in_tag)
            out.push_back(c);
    }
    return out;
}

bool starts_q_code(std::string_view s, std::size_t i)
{
    if (i >= s.size() || (s[i] != 'Q' && s[i] != 'q'))
        return false;
    if (i > 0) {
        const unsigned char prev = static_cast<unsigned char>(s[i - 1]);
        // "OUTSIDEQ273=+50" is one token in the cell. A digit or '_' before
        // Q is a different token and must stay intact.
        if (std::isdigit(prev) || prev == '_')
            return false;
    }
    std::size_t j = i + 1;
    if (j >= s.size() || !std::isdigit(static_cast<unsigned char>(s[j])))
        return false;
    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])))
        ++j;
    return j < s.size() && s[j] == '=';
}

// One HTML cell often glues "OUTSIDE" onto "Q273=+50" with no separator.
std::string split_q_codes(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 32);
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i > 0 && starts_q_code(s, i) && out.back() != '\n')
            out.push_back('\n');
        out.push_back(s[i]);
    }
    return out;
}

std::vector<std::string> split_on(std::string_view s, std::string_view needle)
{
    std::vector<std::string> parts;
    std::size_t i = 0;
    while (i <= s.size()) {
        const std::size_t at = s.find(needle, i);
        if (at == std::string_view::npos) {
            parts.emplace_back(s.substr(i));
            break;
        }
        parts.emplace_back(s.substr(i, at - i));
        i = at + needle.size();
    }
    return parts;
}

std::string lower_copy(std::string_view s)
{
    std::string o(s);
    o = media::strutil::lower_ascii(std::move(o));
    return o;
}

std::string html_table_to_md(std::string html)
{
    html = decode_entities(std::move(html));
    replace_ci_br(html);
    const std::string low = lower_copy(html);
    std::vector<std::vector<std::string>> rows;
    const bool has_tr = low.find("<tr") != std::string::npos;
    const std::vector<std::string> row_html = has_tr ? split_on(html, "</tr>")
                                                     : std::vector<std::string>{html};
    for (const std::string& row : row_html) {
        const std::string row_low = lower_copy(row);
        if (row_low.find("<td") == std::string::npos && row_low.find("<th") == std::string::npos)
            continue;
        std::vector<std::string> cells;
        std::string cur;
        bool in_tag = false;
        std::string tag;
        auto flush_cell = [&] {
            std::string text = trim_copy(split_q_codes(trim_copy(strip_tags(decode_entities(cur)))));
            if (!text.empty())
                cells.push_back(std::move(text));
            cur.clear();
        };
        for (std::size_t i = 0; i < row.size(); ++i) {
            const char c = row[i];
            if (c == '<') {
                in_tag = true;
                tag.clear();
                continue;
            }
            if (c == '>' && in_tag) {
                in_tag = false;
                const std::string t = lower_copy(tag);
                if (t.rfind("/td", 0) == 0 || t.rfind("/th", 0) == 0)
                    flush_cell();
                continue;
            }
            if (in_tag)
                tag.push_back(c);
            else
                cur.push_back(c);
        }
        if (!trim_view(cur).empty())
            flush_cell();
        if (!cells.empty())
            rows.push_back(std::move(cells));
    }
    if (rows.empty()) {
        std::string plain = trim_copy(split_q_codes(trim_copy(strip_tags(html))));
        if (plain.empty())
            return {};
        return "```\n" + plain + "\n```";
    }
    std::size_t cols = 1;
    for (const auto& r : rows)
        cols = std::max(cols, r.size());
    if (cols == 1) {
        std::string plain;
        for (const auto& r : rows) {
            if (!plain.empty())
                plain.push_back('\n');
            plain += r[0];
        }
        return "```\n" + plain + "\n```";
    }
    auto esc = [](std::string cell) {
        replace_all(cell, "\n", " ");
        replace_all(cell, "|", "\\|");
        return cell;
    };
    std::string md;
    md += "|";
    for (std::size_t c = 0; c < cols; ++c) {
        md += " ";
        md += c < rows[0].size() ? esc(rows[0][c]) : "";
        md += " |";
    }
    md += "\n|";
    for (std::size_t c = 0; c < cols; ++c)
        md += " --- |";
    for (std::size_t r = 1; r < rows.size(); ++r) {
        md += "\n|";
        for (std::size_t c = 0; c < cols; ++c) {
            md += " ";
            md += c < rows[r].size() ? esc(rows[r][c]) : "";
            md += " |";
        }
    }
    md += "\n";
    return md;
}

bool is_icon_noise(std::string_view text)
{
    text = trim_view(text);
    return text == "i" || text == "\xE2\x84\xB9" || text == "\xE2\x93\x98";
}

// A list marker is the first glyph on the line. The words after it are the item.
// Squares and discs from the page, and the hyphen this model sometimes writes
// instead, all become the same markdown bullet.
bool consume_bullet_marker(std::string_view line, std::size_t& content_at)
{
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    if (i >= line.size())
        return false;
    static constexpr std::string_view glyphs[] = {
        "\xE2\x96\xA0", // black square
        "\xE2\x96\xA1", // white square
        "\xE2\x96\xAA", // small black square
        "\xE2\x96\xAB", // small white square
        "\xE2\x97\x8F", // black circle
        "\xE2\x97\x8B", // white circle
        "\xE2\x80\xA2", // bullet
        "\xE2\x97\xA6", // white bullet
        "\xE2\x80\xA3", // triangular bullet
    };
    for (const std::string_view g : glyphs) {
        if (line.size() >= i + g.size() && line.substr(i, g.size()) == g) {
            i += g.size();
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
                ++i;
            content_at = i;
            return true;
        }
    }
    const unsigned char c = static_cast<unsigned char>(line[i]);
    if ((c == '-' || c == '*' || c == '+') && i + 1 < line.size()
        && (line[i + 1] == ' ' || line[i + 1] == '\t')) {
        ++i;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            ++i;
        content_at = i;
        return true;
    }
    return false;
}

std::string bullets_to_markdown(std::string_view body, bool force_list)
{
    struct Row {
        bool bullet = false;
        std::string text;
    };
    std::vector<Row> rows;
    bool any = false;
    std::size_t i = 0;
    while (i <= body.size()) {
        std::size_t nl = body.find('\n', i);
        if (nl == std::string_view::npos)
            nl = body.size();
        std::string_view line = body.substr(i, nl - i);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        Row row;
        std::size_t content = 0;
        if (consume_bullet_marker(line, content)) {
            row.bullet = true;
            row.text = std::string(trim_view(line.substr(content)));
            any = true;
        } else {
            row.text = std::string(line);
        }
        rows.push_back(std::move(row));
        if (nl == body.size())
            break;
        i = nl + 1;
    }
    if (!any && force_list) {
        for (Row& row : rows) {
            if (trim_view(row.text).empty())
                continue;
            row.bullet = true;
            row.text = std::string(trim_view(row.text));
            any = true;
            break;
        }
    }
    if (!any)
        return std::string(body);
    std::string out;
    bool in_item = false;
    for (const Row& row : rows) {
        if (row.bullet) {
            if (!out.empty())
                out.push_back('\n');
            out += "- ";
            out += row.text;
            in_item = true;
        } else if (trim_view(row.text).empty()) {
            if (!out.empty())
                out.push_back('\n');
            in_item = false;
        } else if (in_item) {
            out += "\n  ";
            out += std::string(trim_view(row.text));
        } else {
            if (!out.empty())
                out.push_back('\n');
            out += row.text;
        }
    }
    return out;
}

bool label_is(std::string_view label, std::string_view want)
{
    if (label.size() != want.size())
        return false;
    for (std::size_t i = 0; i < label.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(label[i])) != want[i])
            return false;
    }
    return true;
}

bool known_layout_label(std::string_view label)
{
    return label_is(label, "text") || label_is(label, "header")
        || label_is(label, "title") || label_is(label, "footer")
        || label_is(label, "table") || label_is(label, "page_number")
        || label_is(label, "image") || label_is(label, "sub_title")
        || label_is(label, "subtitle") || label_is(label, "caption")
        || label_is(label, "list") || label_is(label, "equation")
        || label_is(label, "formula") || label_is(label, "ref_text")
        || label_is(label, "aside_text");
}

bool looks_like_table(std::string_view body)
{
    const auto t = trim_view(body);
    return t.size() >= 6 && (t[0] == '<')
        && (lower_copy(t.substr(0, 6)) == "<table" || lower_copy(t.substr(0, 3)) == "<tr"
            || lower_copy(t.substr(0, 3)) == "<td");
}

std::string block_key(const Block& b)
{
    return b.label + "\n" + b.body;
}

// The tail of a greedy loop is the same regions repeated. Keep one copy.
std::vector<Block> cut_repeated_blocks(std::vector<Block> blocks)
{
    if (!blocks.empty()) {
        std::string& body = blocks.back().body;
        const auto nl = body.rfind('\n');
        const std::size_t start = nl == std::string::npos ? 0 : nl + 1;
        const std::string_view line(body.data() + start, body.size() - start);
        const auto br = line.find('[');
        if (br != std::string_view::npos && line.find(']') == std::string_view::npos && br > 0)
            body.resize(start == 0 ? 0 : start - 1);
    }
    const std::size_t n = blocks.size();
    if (n < 4)
        return blocks;
    const std::size_t max_span = n / 2 > 8 ? static_cast<std::size_t>(8) : n / 2;
    for (std::size_t span = 1; span <= max_span; ++span) {
        bool same = true;
        bool substantial = false;
        for (std::size_t i = 0; i < span; ++i) {
            if (block_key(blocks[n - span + i]) != block_key(blocks[n - 2 * span + i])) {
                same = false;
                break;
            }
            if (blocks[n - span + i].body.size() >= 24)
                substantial = true;
        }
        if (!same || !substantial)
            continue;
        std::size_t copies = 2;
        while (n >= (copies + 1) * span) {
            bool more = true;
            for (std::size_t i = 0; i < span; ++i) {
                if (block_key(blocks[n - (copies + 1) * span + i]) != block_key(blocks[n - span + i])) {
                    more = false;
                    break;
                }
            }
            if (!more)
                break;
            ++copies;
        }
        blocks.resize(n - (copies - 1) * span);
        break;
    }
    return blocks;
}

bool listish_label(std::string_view label)
{
    return label_is(label, "text") || label_is(label, "ref_text")
        || label_is(label, "list") || label_is(label, "caption") || label.empty();
}

bool body_starts_with_bullet(std::string_view body)
{
    const auto nl = body.find('\n');
    const std::string_view line = nl == std::string_view::npos ? body : body.substr(0, nl);
    std::size_t at = 0;
    return consume_bullet_marker(line, at);
}

// This model sometimes drops the square on the last item of a list.
// When a run of the same label is mostly bullets, the ones without a glyph
// are the same list.
void promote_bullet_runs(std::vector<Block>& blocks)
{
    const std::size_t n = blocks.size();
    for (std::size_t i = 0; i < n;) {
        if (!listish_label(blocks[i].label)) {
            ++i;
            continue;
        }
        std::size_t j = i + 1;
        while (j < n && blocks[j].label == blocks[i].label)
            ++j;
        int bullets = 0;
        int total = 0;
        for (std::size_t k = i; k < j; ++k) {
            if (blocks[k].body.empty() || is_icon_noise(blocks[k].body))
                continue;
            ++total;
            if (label_is(blocks[k].label, "list") || body_starts_with_bullet(blocks[k].body))
                ++bullets;
        }
        if (bullets >= 2 && bullets * 2 >= total) {
            for (std::size_t k = i; k < j; ++k) {
                if (blocks[k].body.empty() || is_icon_noise(blocks[k].body))
                    continue;
                if (body_starts_with_bullet(blocks[k].body))
                    continue;
                blocks[k].body.insert(0, "- ");
            }
        }
        i = j;
    }
}

std::string render_blocks(const std::vector<Block>& blocks)
{
    std::string out;
    bool any_heading = false;
    for (const Block& b : blocks) {
        if (label_is(b.label, "page_number") || label_is(b.label, "footer")
            || label_is(b.label, "aside_text"))
            continue;
        std::string piece;
        if (label_is(b.label, "table") || looks_like_table(b.body)) {
            piece = html_table_to_md(b.body);
        } else if (label_is(b.label, "header") || label_is(b.label, "title")) {
            piece = (any_heading ? "## " : "# ") + b.body;
            any_heading = true;
        } else if (label_is(b.label, "sub_title") || label_is(b.label, "subtitle")) {
            piece = "### " + b.body;
        } else if (b.body.empty() || is_icon_noise(b.body)) {
            continue;
        } else if (label_is(b.label, "image")) {
            piece = b.body;
        } else {
            piece = bullets_to_markdown(b.body, label_is(b.label, "list"));
        }
        if (piece.empty())
            continue;
        if (!out.empty() && out.back() != '\n')
            out.push_back('\n');
        if (!out.empty()
            && (label_is(b.label, "header") || label_is(b.label, "title")
                || label_is(b.label, "table") || looks_like_table(b.body)))
            out.push_back('\n');
        out += piece;
        out.push_back('\n');
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    if (!out.empty())
        out.push_back('\n');
    return out;
}

} // namespace

std::string clean_ocr_markup(const std::string& raw)
{
    if (raw.find('[') == std::string::npos && raw.find("<|ref|>") == std::string::npos)
        return raw;
    const std::string normalized = raw.find("<|ref|>") == std::string::npos
        ? raw
        : normalize_ref_det(raw);
    const std::vector<Block> blocks = scan_blocks(normalized);
    if (blocks.size() < 2)
        return raw;
    const bool known = std::any_of(blocks.begin(), blocks.end(), [](const Block& b) {
        return known_layout_label(b.label);
    });
    if (!known)
        return raw;
    std::vector<Block> kept = cut_repeated_blocks(blocks);
    promote_bullet_runs(kept);
    const std::string rendered = render_blocks(kept);
    return rendered.empty() ? raw : rendered;
}

} // namespace media::llm

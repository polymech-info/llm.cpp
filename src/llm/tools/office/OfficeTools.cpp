// Copyright PolyMech / pm-image. Internal use only.

#include "llm/tools/office/OfficeTools.hpp"

#if defined(FEATURE_OFFICE_LLM_TOOLS) && FEATURE_OFFICE_LLM_TOOLS

#include "fs/path_sanitizer.hpp"
#include "llm/llm_constants.hpp"
#include "office_edit.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <vector>

namespace media::llm::path::detail {
namespace {

std::optional<nlohmann::json> parse_result_json(const std::string& text) {
    if (text.empty()) return nlohmann::json::object();
    auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded()) return std::nullopt;
    return parsed;
}

// Picture bytes belong in the viewer. A data URI for one photo is hundreds of
// kilobytes of base64 and pushes the next model call past its context window.
int replace_embedded_image_uris(std::string& text) {
    const auto first = text.find("data:image/");
    if (first == std::string::npos) return 0;
    std::string out;
    out.reserve(first + 4096);
    int count = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto pos = text.find("data:image/", i);
        if (pos == std::string::npos) {
            out.append(text, i, std::string::npos);
            break;
        }
        out.append(text, i, pos - i);
        // Only a finished standard-base64 data URI is replaced. A mention of
        // "data:image/" in the document text, or a broken payload, stays put.
        const std::size_t header_limit = std::min(text.size(), pos + 64);
        const auto header_end = text.find(";base64,", pos);
        if (header_end == std::string::npos || header_end + 8 > header_limit) {
            out.append("data:image/");
            i = pos + 11;
            continue;
        }
        std::size_t end = header_end + 8;
        while (end < text.size()) {
            const unsigned char c = static_cast<unsigned char>(text[end]);
            const bool b64 = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
            if (!b64) break;
            ++end;
        }
        const bool boundary = end == text.size() || text[end] == ')' || text[end] == '"'
            || text[end] == '\'' || text[end] == ' ' || text[end] == '\n' || text[end] == '\r'
            || text[end] == '\t' || text[end] == '<' || text[end] == ']' || text[end] == ',';
        if (end == header_end + 8 || !boundary) {
            out.append(text, pos, (header_end + 8) - pos);
            i = header_end + 8;
            continue;
        }
        ++count;
        out.append("embedded-image");
        i = end;
    }
    text.swap(out);
    return count;
}

int omit_embedded_images(nlohmann::json& value) {
    if (value.is_string()) {
        const auto& existing = value.get_ref<const std::string&>();
        if (existing.find("data:image/") == std::string::npos) return 0;
        auto text = existing;
        const int count = replace_embedded_image_uris(text);
        if (count > 0) value = std::move(text);
        return count;
    }
    if (value.is_array()) {
        int count = 0;
        for (auto& item : value) count += omit_embedded_images(item);
        return count;
    }
    if (value.is_object()) {
        int count = 0;
        for (auto it = value.begin(); it != value.end(); ++it)
            count += omit_embedded_images(it.value());
        return count;
    }
    return 0;
}

ExecuteResult from_edit(const office::EditResult& result) {
    if (!result.ok) {
        ExecuteResult failed = simple_error(result.error.empty() ? "office tool failed" : result.error);
        if (!result.json.empty()) {
            auto parsed = parse_result_json(result.json);
            if (!parsed) return simple_error("office tool returned malformed JSON");
            failed.envelope["result"] = std::move(*parsed);
            omit_embedded_images(failed.envelope);
        }
        return failed;
    }
    auto parsed = result.plain ? std::optional<nlohmann::json>(nlohmann::json{{"text", result.text}}) : parse_result_json(result.json);
    if (!parsed) return simple_error("office tool returned malformed JSON");
    ExecuteResult ok = simple_ok(std::move(*parsed));
    const int omitted = omit_embedded_images(ok.envelope);
    if (omitted > 0) {
        ok.envelope["images_omitted"] = omitted;
        ok.envelope["images_note"] =
            "Embedded pictures were omitted so this result fits the context window. Each one is marked embedded-image.";
    }
    return ok;
}

struct LineSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
};

void collect_line_spans(const std::string& buf, std::vector<LineSpan>& out) {
    out.clear();
    const std::size_t n = buf.size();
    std::size_t i = 0;
    while (i < n) {
        const std::size_t start = i;
        while (i < n && buf[i] != '\n' && buf[i] != '\r') ++i;
        out.push_back({start, i});
        if (i >= n) break;
        if (buf[i] == '\r' && i + 1 < n && buf[i + 1] == '\n') i += 2;
        else ++i;
    }
}

constexpr int k_default_lines = 200;
constexpr int k_max_depth = 16;
constexpr std::size_t k_max_query_matches = 500;
constexpr std::size_t k_max_batch_commands = 100;
constexpr std::size_t k_max_batch_input = 256u * 1024u;
constexpr std::size_t k_max_response = 128u * 1024u;

bool json_int(const nlohmann::json& value, std::int64_t min, std::int64_t max, int& out, std::string& error, const char* name) {
    if (!value.is_number_integer()) {
        error = std::string(name) + " must be an integer";
        return false;
    }
    std::int64_t wide = 0;
    try {
        wide = value.get<std::int64_t>();
    } catch (...) {
        error = std::string(name) + " is out of range";
        return false;
    }
    if (wide < min || wide > max) {
        error = std::string(name) + " is out of range";
        return false;
    }
    out = static_cast<int>(wide);
    return true;
}

bool optional_int(const nlohmann::json& args, const char* key, std::int64_t min, std::int64_t max, int fallback, int& out, std::string& error) {
    if (!args.contains(key) || args[key].is_null()) {
        out = fallback;
        return true;
    }
    return json_int(args[key], min, max, out, error, key);
}

// A small unwindowed body is returned whole. A large one must be paged.
// A requested window defaults to 200 lines, never more than 400, and never
// more than 96 KiB. One line longer than that is an error: a line offset
// cannot resume in the middle of it.
bool window_body(std::string& text, const nlohmann::json& args, nlohmann::json& meta, std::string& error) {
    const bool asked = (args.contains("offset") && !args["offset"].is_null())
        || (args.contains("limit") && !args["limit"].is_null());
    int offset = 1;
    int limit = asked ? k_default_lines : pm::llm::k_file_read_window_lines;
    if (!optional_int(args, "offset", 1, std::numeric_limits<int>::max(), 1, offset, error)) return false;
    if (args.contains("limit") && !args["limit"].is_null()) {
        int asked = 0;
        if (!json_int(args["limit"], 1, std::numeric_limits<int>::max(), asked, error, "limit")) return false;
        limit = std::min(asked, pm::llm::k_file_read_window_lines);
    }

    std::vector<LineSpan> spans;
    collect_line_spans(text, spans);
    if (spans.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "office document has too many lines";
        return false;
    }
    const int total = static_cast<int>(spans.size());
    if (!asked) {
        if (total > pm::llm::k_file_read_window_lines || text.size() > pm::llm::k_file_read_large_bytes) {
            error = "office document is large (" + std::to_string(total)
                + " lines); call again with offset and limit";
            return false;
        }
        return true;
    }
    meta["total_lines"] = total;
    if (total == 0) {
        meta["line_start"] = 1;
        meta["line_end"] = 0;
        meta["truncated"] = false;
        return true;
    }
    if (offset > total) {
        error = "offset " + std::to_string(offset) + " past end (" + std::to_string(total) + " lines)";
        return false;
    }

    const int line_start = offset;
    const std::int64_t last = static_cast<std::int64_t>(offset) + static_cast<std::int64_t>(limit) - 1;
    int line_end = static_cast<int>(std::min<std::int64_t>(total, last));
    const std::size_t from = spans[static_cast<std::size_t>(line_start - 1)].begin;
    auto end_of = [&](int line) {
        std::size_t to = spans[static_cast<std::size_t>(line - 1)].end;
        if (to < text.size()) {
            if (text[to] == '\r' && to + 1 < text.size() && text[to + 1] == '\n') to += 2;
            else if (text[to] == '\n' || text[to] == '\r') ++to;
        }
        return to;
    };
    std::size_t to = end_of(line_end);
    while (line_end > line_start && (to - from) > pm::llm::k_file_read_window_bytes) {
        --line_end;
        to = end_of(line_end);
    }
    if ((to - from) > pm::llm::k_file_read_window_bytes) {
        error = "line " + std::to_string(line_start) + " is " + std::to_string(to - from)
            + " bytes; narrow the query";
        return false;
    }
    text = text.substr(from, to - from);
    const bool truncated = line_end < total;
    meta["line_start"] = line_start;
    meta["line_end"] = line_end;
    meta["truncated"] = truncated;
    if (truncated) meta["next_offset"] = line_end + 1;
    return true;
}

void trim_cells(nlohmann::json& envelope) {
    if (!envelope.contains("cells") || !envelope["cells"].is_array()) return;
    auto& cells = envelope["cells"];
    const auto total = static_cast<std::uint64_t>(cells.size());
    if (envelope.value("truncated", false)) {
        envelope.erase("cells");
        envelope["cells_total"] = total;
        envelope["cells_omitted"] = true;
        return;
    }
    nlohmann::json kept = nlohmann::json::array();
    std::size_t bytes = 2;
    std::string oversized_ref;
    for (const auto& cell : cells) {
        const auto piece = cell.dump();
        if (bytes + piece.size() + 1 > pm::llm::k_file_read_large_bytes) {
            if (kept.empty() && cell.is_object()) {
                if (cell.contains("ref") && cell["ref"].is_string()) oversized_ref = cell["ref"].get<std::string>();
                else if (cell.contains("path") && cell["path"].is_string()) oversized_ref = cell["path"].get<std::string>();
            }
            break;
        }
        bytes += piece.size() + 1;
        kept.push_back(cell);
    }
    const auto kept_count = kept.size();
    envelope["cells"] = std::move(kept);
    if (kept_count < total) {
        envelope["cells_total"] = total;
        envelope["cells_truncated"] = true;
        if (kept_count == 0) {
            envelope["cells_note"] = oversized_ref.empty()
                ? "the first cell exceeds the response budget; request it with office_get"
                : "cell " + oversized_ref + " exceeds the response budget; request it with office_get";
        }
    }
}

void cap_match_list(nlohmann::json& node) {
    if (!node.is_object() || !node.contains("children") || !node["children"].is_array()) return;
    auto& kids = node["children"];
    if (kids.size() <= k_max_query_matches) return;
    const auto total = static_cast<std::uint64_t>(kids.size());
    nlohmann::json kept = nlohmann::json::array();
    for (std::size_t i = 0; i < k_max_query_matches; ++i) kept.push_back(std::move(kids[i]));
    node["children"] = std::move(kept);
    node["children_total"] = total;
    node["children_truncated"] = true;
}

bool window_markdown_field(nlohmann::json& node, const nlohmann::json& args, std::string& error) {
    if (!node.is_object() || !node.contains("markdown") || !node["markdown"].is_string()) return true;
    auto text = node["markdown"].get<std::string>();
    nlohmann::json meta = nlohmann::json::object();
    if (!window_body(text, args, meta, error)) return false;
    node["markdown"] = std::move(text);
    for (auto it = meta.begin(); it != meta.end(); ++it) node[it.key()] = it.value();
    return true;
}

ExecuteResult fit_office_read(const std::string& name, ExecuteResult result, const nlohmann::json& args) {
    if (!result.envelope.is_object()) return result;
    std::string error;
    if (result.ok) {
        if (result.envelope.contains("markdown") && result.envelope["markdown"].is_string()) {
            if (!window_markdown_field(result.envelope, args, error)) return simple_error(name + ": " + error);
        } else if (result.envelope.contains("text") && result.envelope["text"].is_string()) {
            auto text = result.envelope["text"].get<std::string>();
            nlohmann::json meta = nlohmann::json::object();
            if (!window_body(text, args, meta, error)) return simple_error(name + ": " + error);
            result.envelope["text"] = std::move(text);
            for (auto it = meta.begin(); it != meta.end(); ++it) result.envelope[it.key()] = it.value();
        } else if (result.envelope.contains("results") && result.envelope["results"].is_array()) {
            for (auto& item : result.envelope["results"]) {
                if (!window_markdown_field(item, args, error)) return simple_error(name + ": " + error);
                cap_match_list(item);
            }
        }
        trim_cells(result.envelope);
        if (name == "office_query") cap_match_list(result.envelope);
    }
    auto dumped = result.envelope.dump();
    if (dumped.size() > k_max_response && result.envelope.contains("cells")) {
        if (!result.envelope.contains("cells_total")) {
            result.envelope["cells_total"] = result.envelope["cells"].is_array()
                ? static_cast<std::uint64_t>(result.envelope["cells"].size())
                : 0;
        }
        result.envelope.erase("cells");
        result.envelope["cells_omitted"] = true;
        dumped = result.envelope.dump();
    }
    if (dumped.size() > k_max_response) {
        return simple_error(name + ": result is " + std::to_string(dumped.size())
            + " bytes; narrow the node, selector, or batch");
    }
    return result;
}

std::string require_file(const nlohmann::json& args, bool write, std::filesystem::path& out, std::string& error) {
    const auto raw = get_nonempty_string(args, "path");
    if (!raw) {
        error = "path is required";
        return {};
    }
    const std::string abs = resolve_tool_path_string(*raw);
    const std::string denied = write ? llm_path_write_deny(abs) : llm_path_fs_deny(abs);
    if (!denied.empty()) {
        error = denied;
        return {};
    }
    out = media::path::path_from_utf8(abs);
    return abs;
}

office::PropMap props_from_json(const nlohmann::json& args, std::string& error) {
    office::PropMap props;
    if (!args.contains("props") || args["props"].is_null()) return props;
    if (!args["props"].is_object()) {
        error = "props must be an object";
        return {};
    }
    for (auto it = args["props"].begin(); it != args["props"].end(); ++it) {
        if (it.value().is_string()) props[it.key()] = it.value().get<std::string>();
        else if (it.value().is_boolean()) props[it.key()] = it.value().get<bool>() ? "true" : "false";
        else if (it.value().is_number()) props[it.key()] = it.value().dump();
        else {
            error = "prop values must be strings, numbers, or booleans";
            return {};
        }
    }
    return props;
}

std::string arg_or(const nlohmann::json& args, const char* key, const std::string& fallback = {}) {
    const auto value = get_nonempty_string(args, key);
    return value ? *value : fallback;
}

bool arg_index(const nlohmann::json& args, int& out, std::string& error) {
    return optional_int(args, "index", std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), -1, out, error);
}

const char* k_gap =
        "Read .docx, .docm, .xlsx, .xlsm, .pptx, .pptm, .odt, .ods, and .odp as markdown. Spreadsheets also return cells. "
        "Create and edit cover docx paragraph/run/table/row/cell, xlsx sheet/row/cell, and pptx slide/shape/picture. "
        "OpenDocument create and edit are not implemented. "
    "Slide background and transition, and shape animation (fade, fly, zoom, wipe, and the other entrance/exit effects), are properties. "
    "Charts, pivots, raw XML, morph, emphasis animations, and HTML preview are not available.";

nlohmann::json file_schema() {
    return {{"type", "string"}, {"description", "Filesystem path of a .docx, .docm, .xlsx, .xlsm, .pptx, .pptm, .odt, .ods, or .odp file. Create still needs .docx, .xlsx, or .pptx."}};
}

nlohmann::json props_schema() {
    return {
        {"type", "object"},
        {"description", "Properties: text, value, bold, italic, style, color, size, font, fill, preset, animation, background, transition, src, rows, cols, name, ref, x, y, w, h, title, or c1/c2 for an xlsx row. Values are strings, numbers, or booleans."},
        {"additionalProperties", {
            {"anyOf", nlohmann::json::array({
                nlohmann::json{{"type", "string"}},
                nlohmann::json{{"type", "number"}},
                nlohmann::json{{"type", "boolean"}},
            })},
        }},
    };
}

} // namespace

bool is_office_tool(std::string_view name) noexcept {
    return name == "office_create" || name == "office_view" || name == "office_get" || name == "office_query"
        || name == "office_add" || name == "office_set" || name == "office_remove" || name == "office_move"
        || name == "office_swap" || name == "office_batch" || name == "office_validate";
}

ExecuteResult do_office_tool(const std::string& name, const nlohmann::json& args) {
    const bool write = name != "office_view" && name != "office_get" && name != "office_query"
        && name != "office_validate";
    std::filesystem::path file;
    std::string error;
    if (require_file(args, write, file, error).empty() && !error.empty()) return simple_error(error);
    if (file.empty()) return simple_error("path is required");

    if (name == "office_create") return from_edit(office::createDocument(file));
    if (name == "office_validate") return from_edit(office::validateDocument(file));
    if (name == "office_view")
        return fit_office_read(name, from_edit(office::viewDocument(file, arg_or(args, "mode", "text"))), args);
    if (name == "office_get") {
        int depth = 1;
        if (!optional_int(args, "depth", 0, k_max_depth, 1, depth, error)) return simple_error(name + ": " + error);
        return fit_office_read(name, from_edit(office::documentGet(file, arg_or(args, "node", "/"), depth)), args);
    }
    if (name == "office_query") {
        const auto selector = arg_or(args, "selector");
        if (selector.empty()) return simple_error("selector is required");
        return fit_office_read(name, from_edit(office::documentQuery(file, selector)), args);
    }
    if (name == "office_remove") {
        const auto node = arg_or(args, "node");
        if (node.empty()) return simple_error("node is required");
        return from_edit(office::removeElement(file, node));
    }
    if (name == "office_swap") {
        const auto left = arg_or(args, "node");
        const auto right = arg_or(args, "node2");
        if (left.empty() || right.empty()) return simple_error("node and node2 are required");
        return from_edit(office::swapElements(file, left, right));
    }
    int index = -1;
    if ((name == "office_add" || name == "office_move") && !arg_index(args, index, error))
        return simple_error(name + ": " + error);
    if (name == "office_move") {
        const auto node = arg_or(args, "node");
        if (node.empty()) return simple_error("node is required");
        return from_edit(office::moveElement(file, node, arg_or(args, "to"), arg_or(args, "after"), arg_or(args, "before"), index));
    }
    const auto props = props_from_json(args, error);
    if (!error.empty()) return simple_error(error);
    if (name == "office_add") {
        const auto type = arg_or(args, "type");
        if (type.empty()) return simple_error("type is required");
        return from_edit(office::addElement(file, arg_or(args, "parent", "/"), type, props, arg_or(args, "after"), arg_or(args, "before"), index));
    }
    if (name == "office_set") {
        return from_edit(office::setElement(file, arg_or(args, "node", "/"), props, arg_or(args, "find"), arg_or(args, "replace")));
    }
    if (name == "office_batch") {
        if (!args.contains("commands") || !args["commands"].is_array()) return simple_error("commands must be an array");
        const auto& commands = args["commands"];
        if (commands.size() > k_max_batch_commands) return simple_error("office_batch: at most 100 commands");
        for (const auto& command : commands) {
            if (!command.is_object()) return simple_error("office_batch: each command must be an object");
        }
        const auto packed = commands.dump();
        if (packed.size() > k_max_batch_input) return simple_error("office_batch: commands exceed 256 KiB");
        bool stop = true;
        if (args.contains("stop_on_error") && !args["stop_on_error"].is_null()) {
            if (!args["stop_on_error"].is_boolean()) return simple_error("office_batch: stop_on_error must be a boolean");
            stop = args["stop_on_error"].get<bool>();
        }
        return fit_office_read(name, from_edit(office::runBatchJson(file, packed, stop)), args);
    }
    return simple_error(std::string("unknown office tool: ") + name);
}

std::vector<ToolDef> office_tool_catalog() {
    const auto path = file_schema();
    const auto props = props_schema();
    const std::string description_tail = std::string(" ") + k_gap;
    return {
        ToolDef{"office_create", "Create a blank .docx, .xlsx, or .pptx. Fails if the file already exists." + description_tail,
            {{"type", "object"}, {"properties", {{"path", path}}}, {"required", {"path"}}}},
        ToolDef{"office_view", "Read a document as markdown, plain text, an outline, or word/sheet/slide stats. Markdown on a spreadsheet includes cells. Embedded pictures come back as the placeholder embedded-image, not as image bytes. A document over 400 lines or 48 KiB must be read with offset and limit, the same window as file_read." + description_tail,
            {{"type", "object"}, {"properties", {
                {"path", path},
                {"mode", {{"type", "string"}, {"enum", {"text", "markdown", "outline", "stats"}}, {"description", "markdown returns the document and, for xlsx or ods, cells. text, outline, or stats are the other modes. Default text. OpenDocument text is markdown."}}},
                {"offset", {{"type", "integer"}, {"description", "1-based line to start at. Required with limit when the document is large. A string or out-of-range value is an error."}}},
                {"limit", {{"type", "integer"}, {"description", "Max lines when offset is set. Defaults to 200, capped at 400. The window is also at most 96 KiB. A single line over 96 KiB is an error."}}},
            }}, {"required", {"path"}}}},
        ToolDef{"office_get", "Read one office node as JSON. node is an office path such as /body/p[1], Sheet1!A1, or /slide[1]. For odt, ods, and odp, node / returns markdown and cells. Embedded pictures come back as the placeholder embedded-image, not as image bytes. A large markdown body uses offset and limit, the same window as file_read." + description_tail,
            {{"type", "object"}, {"properties", {
                {"path", path},
                {"node", {{"type", "string"}, {"description", "Office path. Default /."}}},
                {"depth", {{"type", "integer"}, {"minimum", 0}, {"maximum", 16}, {"description", "Child depth. Default 1. Maximum 16."}}},
                {"offset", {{"type", "integer"}, {"description", "1-based line, used when node / returns the whole document."}}},
                {"limit", {{"type", "integer"}, {"description", "Max lines when offset is set. Defaults to 200, capped at 400 and 96 KiB."}}},
            }}, {"required", {"path"}}}},
        ToolDef{"office_query", "Query a document with an office selector." + description_tail,
            {{"type", "object"}, {"properties", {
                {"path", path},
                {"selector", {{"type", "string"}, {"description", "Selector, for example paragraph[style=Heading1] or cell[value=Alice]."}}},
            }}, {"required", {"path", "selector"}}}},
        ToolDef{"office_add", "Add a paragraph, run, table, row, cell, sheet, slide, shape, or picture." + description_tail,
            {{"type", "object"}, {"properties", {
                {"path", path},
                {"parent", {{"type", "string"}, {"description", "Parent office path. Default /."}}},
                {"type", {{"type", "string"}, {"description", "paragraph, run, table, row, cell, sheet, slide, shape, or picture."}}},
                {"props", props},
                {"after", {{"type", "string"}}},
                {"before", {{"type", "string"}}},
                {"index", {{"type", "integer"}, {"description", "0-based for docx. 1-based row number for xlsx."}}},
            }}, {"required", {"path", "type"}}}},
        ToolDef{"office_set", "Set properties on a node, including pptx animation, transition, and background, or find and replace text." + description_tail,
            {{"type", "object"}, {"properties", {
                {"path", path},
                {"node", {{"type", "string"}, {"description", "Office path. Use / with find to search the document."}}},
                {"props", props},
                {"find", {{"type", "string"}}},
                {"replace", {{"type", "string"}}},
            }}, {"required", {"path"}}}},
        ToolDef{"office_remove", "Remove a paragraph, run, table, row, cell, sheet cell, slide, or shape." + description_tail,
            {{"type", "object"}, {"properties", {
                {"path", path},
                {"node", {{"type", "string"}, {"description", "Office path of the element to remove."}}},
            }}, {"required", {"path", "node"}}}},
        ToolDef{"office_move", "Move a docx paragraph or table." + description_tail,
            {{"type", "object"}, {"properties", {
                {"path", path},
                {"node", {{"type", "string"}}},
                {"after", {{"type", "string"}}},
                {"before", {{"type", "string"}}},
                {"index", {{"type", "integer"}}},
            }}, {"required", {"path", "node"}}}},
        ToolDef{"office_swap", "Swap two docx paragraphs or two docx tables." + description_tail,
            {{"type", "object"}, {"properties", {
                {"path", path},
                {"node", {{"type", "string"}}},
                {"node2", {{"type", "string"}}},
            }}, {"required", {"path", "node", "node2"}}}},
        ToolDef{"office_batch", "Run several add, set, remove, view, get, or query operations, then save once." + description_tail,
            {{"type", "object"}, {"properties", {
                {"path", path},
                {"commands", {{"type", "array"}, {"maxItems", 100}, {"description", "At most 100 objects, together at most 256 KiB. Each has command or op, plus path, parent, type, props, find, replace, index, mode."}}},
                {"stop_on_error", {{"type", "boolean"}, {"description", "Stop after the first failed command. Default true."}}},
            }}, {"required", {"path", "commands"}}}},
        ToolDef{"office_validate", "Open the package and parse its main part." + description_tail,
            {{"type", "object"}, {"properties", {{"path", path}}}, {"required", {"path"}}}},
    };
}

} // namespace media::llm::path::detail

#endif

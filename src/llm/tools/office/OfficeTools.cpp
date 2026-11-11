// Copyright PolyMech / pm-image. Internal use only.

#include "llm/tools/office/OfficeTools.hpp"

#if defined(FEATURE_OFFICE_LLM_TOOLS) && FEATURE_OFFICE_LLM_TOOLS

#include "fs/path_sanitizer.hpp"
#include "office_edit.h"

#include <filesystem>

namespace media::llm::path::detail {
namespace {

nlohmann::json parse_result_json(const std::string& text) {
    if (text.empty()) return nlohmann::json::object();
    auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (parsed.is_discarded()) return nlohmann::json{{"json", text}};
    return parsed;
}

ExecuteResult from_edit(const office::EditResult& result) {
    if (!result.ok) {
        ExecuteResult failed = simple_error(result.error.empty() ? "office tool failed" : result.error);
        if (!result.json.empty()) failed.envelope["result"] = parse_result_json(result.json);
        return failed;
    }
    if (result.plain) return simple_ok({{"text", result.text}});
    return simple_ok(parse_result_json(result.json));
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
        error = "props must be an object of strings";
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

int arg_index(const nlohmann::json& args) {
    if (!args.contains("index") || args["index"].is_null()) return -1;
    if (args["index"].is_number_integer()) return args["index"].get<int>();
    return -1;
}

const char* k_gap =
        "Read .docx, .xlsx, .pptx, .odt, .ods, and .odp as markdown. Spreadsheets also return cells. "
        "Create and edit cover docx paragraph/run/table/row/cell, xlsx sheet/row/cell, and pptx slide/shape/picture. "
        "OpenDocument create and edit are not implemented. "
    "Slide background and transition, and shape animation (fade, fly, zoom, wipe, and the other entrance/exit effects), are properties. "
    "Charts, pivots, raw XML, morph, emphasis animations, and HTML preview are not available.";

nlohmann::json file_schema() {
    return {{"type", "string"}, {"description", "Filesystem path of a .docx, .xlsx, or .pptx file."}};
}

nlohmann::json props_schema() {
    return {
        {"type", "object"},
        {"description", "String properties: text, value, bold, italic, style, color, size, font, fill, preset, animation, background, transition, src, rows, cols, name, ref, x, y, w, h, title, or c1/c2 for an xlsx row."},
        {"additionalProperties", {{"type", "string"}}},
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
    if (name == "office_view") return from_edit(office::viewDocument(file, arg_or(args, "mode", "text")));
    if (name == "office_get") {
        int depth = 1;
        if (args.contains("depth") && args["depth"].is_number_integer()) depth = args["depth"].get<int>();
        return from_edit(office::documentGet(file, arg_or(args, "node", "/"), depth));
    }
    if (name == "office_query") {
        const auto selector = arg_or(args, "selector");
        if (selector.empty()) return simple_error("selector is required");
        return from_edit(office::documentQuery(file, selector));
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
    if (name == "office_move") {
        const auto node = arg_or(args, "node");
        if (node.empty()) return simple_error("node is required");
        return from_edit(office::moveElement(file, node, arg_or(args, "to"), arg_or(args, "after"), arg_or(args, "before"), arg_index(args)));
    }
    const auto props = props_from_json(args, error);
    if (!error.empty()) return simple_error(error);
    if (name == "office_add") {
        const auto type = arg_or(args, "type");
        if (type.empty()) return simple_error("type is required");
        return from_edit(office::addElement(file, arg_or(args, "parent", "/"), type, props, arg_or(args, "after"), arg_or(args, "before"), arg_index(args)));
    }
    if (name == "office_set") {
        return from_edit(office::setElement(file, arg_or(args, "node", "/"), props, arg_or(args, "find"), arg_or(args, "replace")));
    }
    if (name == "office_batch") {
        if (!args.contains("commands") || !args["commands"].is_array()) return simple_error("commands must be an array");
        const bool stop = args.contains("stop_on_error") && args["stop_on_error"].is_boolean() && args["stop_on_error"].get<bool>();
        return from_edit(office::runBatchJson(file, args["commands"].dump(), stop));
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
        ToolDef{"office_view", "Read a document as markdown, plain text, an outline, or word/sheet/slide stats. Markdown on a spreadsheet includes cells." + description_tail,
            {{"type", "object"}, {"properties", {
                {"path", path},
                {"mode", {{"type", "string"}, {"enum", {"text", "markdown", "outline", "stats"}}, {"description", "markdown returns the document and, for xlsx or ods, cells. text, outline, or stats are the other modes. Default text. OpenDocument text is markdown."}}},
            }}, {"required", {"path"}}}},
        ToolDef{"office_get", "Read one office node as JSON. node is an office path such as /body/p[1], Sheet1!A1, or /slide[1]. For odt, ods, and odp, node / returns markdown and cells." + description_tail,
            {{"type", "object"}, {"properties", {
                {"path", path},
                {"node", {{"type", "string"}, {"description", "Office path. Default /."}}},
                {"depth", {{"type", "integer"}, {"description", "Child depth. Default 1."}}},
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
                {"commands", {{"type", "array"}, {"description", "Objects with command or op, plus path, parent, type, props, find, replace, index, mode."}}},
                {"stop_on_error", {{"type", "boolean"}}},
            }}, {"required", {"path", "commands"}}}},
        ToolDef{"office_validate", "Open the package and parse its main part." + description_tail,
            {{"type", "object"}, {"properties", {{"path", path}}}, {"required", {"path"}}}},
    };
}

} // namespace media::llm::path::detail

#endif

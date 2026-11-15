#pragma once

#include "llm/agent.hpp"

#include <string>

namespace media::llm::agent {

/// Kind names the phone voice transcript understands.
inline const char* surface_kind_name(Event::Kind kind)
{
    switch (kind) {
    case Event::ToolCall:         return "tool_call";
    case Event::ToolResult:       return "tool_result";
    case Event::ToolFileProgress: return "tool_file_progress";
    case Event::Thinking:         return "thinking";
    default:                      return "";
    }
}

inline bool surface_event_kind(Event::Kind kind)
{
    return kind == Event::ToolCall || kind == Event::ToolResult
        || kind == Event::ToolFileProgress || kind == Event::Thinking;
}

/// Drop inline bytes. Keep paths, flags, and short text a transcript can draw.
inline nlohmann::json surface_value(const nlohmann::json& value, int depth)
{
    if (depth > 5 || value.is_discarded())
        return nullptr;
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (text.size() <= 2000)
            return value;
        return text.substr(0, 2000);
    }
    if (value.is_number() || value.is_boolean() || value.is_null())
        return value;
    if (value.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        std::size_t n = 0;
        for (const auto& item : value) {
            if (++n > 32)
                break;
            auto child = surface_value(item, depth + 1);
            if (!child.is_null())
                out.push_back(std::move(child));
        }
        return out;
    }
    if (!value.is_object())
        return nullptr;
    nlohmann::json out = nlohmann::json::object();
    for (auto it = value.begin(); it != value.end(); ++it) {
        const std::string& key = it.key();
        if (key == "b64" || key == "base64" || key == "b64_json" || key == "bytes")
            continue;
        if ((key == "data" || key == "image" || key == "content" || key == "audio")
            && it.value().is_string() && it.value().get_ref<const std::string&>().size() > 500)
            continue;
        auto child = surface_value(it.value(), depth + 1);
        if (!child.is_null())
            out[key] = std::move(child);
    }
    return out;
}

/// Same files the desktop voice panel draws: every ok `output_path`.
/// A missing `ok` still counts. An explicit failure does not.
inline nlohmann::json surface_output_paths(const nlohmann::json& payload)
{
    nlohmann::json out = nlohmann::json::array();
    if (!payload.is_object())
        return out;
    const nlohmann::json* env = &payload;
    if (payload.contains("envelope") && payload["envelope"].is_object())
        env = &payload["envelope"];
    auto take = [&](const nlohmann::json& node) {
        if (!node.is_object())
            return;
        if (node.contains("ok") && node["ok"].is_boolean() && !node["ok"].get<bool>())
            return;
        const std::string path = node.value("output_path", std::string{});
        if (!path.empty())
            out.push_back(path);
    };
    if (env->contains("results") && (*env)["results"].is_array()) {
        for (const auto& row : (*env)["results"])
            take(row);
    }
    take(*env);
    return out;
}

inline nlohmann::json surface_event(const Event& event)
{
    std::string text = event.text;
    if (text.size() > 2000)
        text.resize(2000);
    nlohmann::json row = {
        {"kind", surface_kind_name(event.kind)},
        {"text", std::move(text)},
        {"tool_name", event.tool_name},
    };
    if (!event.payload.is_null()) {
        row["payload"] = surface_value(event.payload, 0);
        const auto paths = surface_output_paths(event.payload);
        if (!paths.empty())
            row["paths"] = paths;
    }
    return row;
}

} // namespace media::llm::agent

#pragma once
//
// media::llm::path::tool_catalog — JSON-Schema descriptions of pm-image
// operations as **path-mode** tools (sibling of media::llm).
//
// The chat surface (in-app + `pm-image llm agent` CLI) operates on the
// user's filesystem: the model takes a list of host paths, runs the
// chosen op via the existing *_file / *_image workers, and writes the
// outputs to disk. This is the "natural workflow" path — see
// docs/chat.md §2 for the zero-fs vs path-mode rationale.
//
// Same names + descriptions as media::llm::tool_catalog (so an LLM
// trained on either schema sees the same operations); only the
// per-tool input shape differs (`paths: string[]` instead of
// `image: {b64,mime}`).
//
#include "polymech_export.h"
#include "../constants.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace media::llm::path {

struct ToolDef {
    std::string    name;          // e.g. "image_resize"
    std::string    description;   // one-liner shown to the agent
    nlohmann::json input_schema;  // JSON Schema (object)
};

/// All path-mode tools available in this build.
PM_CHAT_API std::vector<ToolDef> tool_catalog();

/// Same data, JSON-encoded as `{ "tools": [ {name, description, input_schema}, … ] }`.
PM_CHAT_API nlohmann::json tool_catalog_json();

/// Same data, formatted as the OpenAI Chat Completions `tools[]` parameter:
/// `[ {"type":"function","function":{"name":..,"description":..,"parameters":{<schema>}}}, … ]`.
/// This is what `polymech::kbot::LLMClient::execute_chat_messages(messages, tools, …)` expects.
PM_CHAT_API nlohmann::json tool_catalog_openai();

/// `tool_catalog()` with entries removed whose `name` matches a string in `disabled` (case-insensitive,
/// after trim). `disabled` may contain extra whitespace or unknown names (ignored).
PM_CHAT_API std::vector<ToolDef> tool_catalog_excluding(const std::vector<std::string>& disabled);

/// `tool_catalog()` filtered to an allowlist, then with disabled entries removed.
/// Empty allowlist = same as `tool_catalog_excluding`.
PM_CHAT_API std::vector<ToolDef> tool_catalog_allowing(const std::vector<std::string>& allowed,
                                                        const std::vector<std::string>& disabled = {});

/// OpenAI `tools[]` array built from `tool_catalog_excluding` (empty array if everything disabled).
PM_CHAT_API nlohmann::json tool_catalog_openai_excluding(const std::vector<std::string>& disabled);

/// OpenAI `tools[]` array built from `tool_catalog_allowing`.
PM_CHAT_API nlohmann::json tool_catalog_openai_allowing(const std::vector<std::string>& allowed,
                                                         const std::vector<std::string>& disabled = {});

// ── Flag-based API ────────────────────────────────────────────────────────────
//
// Use pm::llm::AgentTool / pm::llm::AgentTools (from constants.hpp) to select
// which tools to include.  The default preset is pm::llm::k_agent_tools_all.
//
// Example — disable scheduler and memory tools:
//   constexpr pm::llm::AgentTools flags =
//       pm::llm::k_agent_tools_all
//       & ~static_cast<pm::llm::AgentTools>(pm::llm::AgentTool::ScheduleAt)
//       & ~static_cast<pm::llm::AgentTools>(pm::llm::AgentTool::ScheduleIn)
//       & ~static_cast<pm::llm::AgentTools>(pm::llm::AgentTool::ScheduleEvery)
//       & ~static_cast<pm::llm::AgentTools>(pm::llm::AgentTool::ScheduleCancel)
//       & ~static_cast<pm::llm::AgentTools>(pm::llm::AgentTool::ScheduleList)
//       & ~static_cast<pm::llm::AgentTools>(pm::llm::AgentTool::MemoryRead)
//       & ~static_cast<pm::llm::AgentTools>(pm::llm::AgentTool::MemoryWrite)
//       & ~static_cast<pm::llm::AgentTools>(pm::llm::AgentTool::MemoryAppendEvent);
//   auto tools = tool_catalog_openai_for_flags(flags);

/// Returns the tool name strings for any AgentTool bits that are CLEAR in `flags`
/// (i.e. the "disabled" list for tool_catalog_excluding).
PM_CHAT_API std::vector<std::string> agent_tools_disabled_names(pm::llm::AgentTools flags);

/// Convenience: `tool_catalog_openai_excluding(agent_tools_disabled_names(flags))`.
/// Pass `pm::llm::k_agent_tools_all` to get the full catalog.
PM_CHAT_API nlohmann::json tool_catalog_openai_for_flags(pm::llm::AgentTools flags);

/// Convenience: `tool_catalog_excluding(agent_tools_disabled_names(flags))`.
PM_CHAT_API std::vector<ToolDef> tool_catalog_for_flags(pm::llm::AgentTools flags);

} // namespace media::llm::path

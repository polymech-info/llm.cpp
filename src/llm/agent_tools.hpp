#pragma once
//
// Single source of truth for the agent tool registry.
//
// What lives here:
//   AgentToolGroup  — logical grouping (File, Image, Utility, Scheduler, Memory)
//   AgentToolInfo   — { flag, name, short_desc, group } per tool
//   agent_tool_registry_data() / agent_tool_registry_size()
//   agent_tool_find(flag) / agent_tool_find_by_name(name)
//
// What lives elsewhere:
//   constants.hpp          — AgentTool enum, AgentTools bitmask, k_agent_tools_all
//   path_tool_catalog.hpp  — full OpenAI JSON schemas (ToolDef, tool_catalog_openai, …)
//   tool_catalog.hpp       — REST / in-buffer schemas (leave alone)
//
#include "polymech_export.h"
#include "../constants.hpp"   // pm::llm::AgentTool, AgentTools

#include <cstddef>
#include <string_view>

namespace pm::llm {

enum class AgentToolGroup : uint8_t {
    File,       // list_images, file_glob, file_read, file_search, search_index, search_query, search_detail
    Image,      // image_resize, image_crop, image_transform, image_create, create_video,
                //   image_understand, image_from_camera
    Utility,    // write_file, file_compose, file_str_replace, file_delete, speak
    Scheduler,  // schedule_at, schedule_in, schedule_every, schedule_cancel, schedule_list
    Memory,     // memory_read, memory_write, memory_append_event
    Computer,   // app_inspect_dump, app_inspect_find, app_screenshot, app_click
    Browser,    // browser_read, browser_find, browser_scope, browser_click, browser_type, browser_select, browser_batch, browser_navigate
    Service,    // service_* Tanit CMS / VFS API tools
    App,        // app_command — Tanit UI/app verbs
    Network,    // ssh_exec, mqtt, modbus_read, modbus_write
};

struct AgentToolInfo {
    AgentTool       flag;
    const char*     name;        ///< canonical snake_case name used in JSON / tool_catalog
    const char*     short_desc;  ///< one-liner for llm info tables, sidebar summaries, …
    AgentToolGroup  group;
};

/// Number of entries in the registry (== number of AgentTool enumerators, excl. None).
POLYMECH_API std::size_t         agent_tool_registry_size() noexcept;

/// Pointer to the first AgentToolInfo in the registry.
/// Stable for the process lifetime; iterate with [0, agent_tool_registry_size()).
POLYMECH_API const AgentToolInfo* agent_tool_registry_data() noexcept;

/// Find by flag — O(n) over the registry (~20 entries).
/// Returns nullptr if flag is AgentTool::None or not found.
POLYMECH_API const AgentToolInfo* agent_tool_find(AgentTool flag) noexcept;

/// Find by name — case-insensitive exact match.
/// Returns nullptr if not found.
POLYMECH_API const AgentToolInfo* agent_tool_find_by_name(std::string_view name) noexcept;

} // namespace pm::llm

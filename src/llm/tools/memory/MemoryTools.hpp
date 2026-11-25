// Copyright PolyMech / pm-image. Internal use only.
//
// `memory_read` / `memory_write` / `memory_append_event` / `memory_find` agent tools.
//
// memory_read / memory_write route through:
//   * the current session when task_id and scope are omitted
//   * task_store_* when task_id names a scheduled task
//   * default_memory.doc only when scope is "global"
//
// memory_find searches the current session by default. searchPastChats
// adds other sessions. Global facts stay in the candidate pool.
#pragma once

#include "llm/tools/commons/PathToolCommons.hpp"

namespace media::llm::path {
struct ToolDef;
}

namespace media::llm::path::detail {

ExecuteResult do_memory_read(const nlohmann::json& args);
ExecuteResult do_memory_write(const nlohmann::json& args);
ExecuteResult do_memory_append_event(const nlohmann::json& args);
ExecuteResult do_memory_find(const nlohmann::json& args);

ToolDef make_memory_read();
ToolDef make_memory_write();
ToolDef make_memory_append_event();
ToolDef make_memory_find();

/// Load full `default_memory.doc` (secure storage or plain file). Missing doc → empty object, true.
bool load_global_memory_document(nlohmann::json& out_document);

} // namespace media::llm::path::detail

// Copyright PolyMech / pm-image. Internal use only.
//
// `memory_read` / `memory_write` / `memory_append_event` / `memory_find` agent tools.
//
// memory_read / memory_write route through:
//   * task_store_* (in-memory) for real scheduled tasks
//   * one global signed long-term document `default_memory.doc`
//     for regular chat sessions and CLI runs
//
// memory_find searches default long-term memory and optionally persisted
// past chat sessions.
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

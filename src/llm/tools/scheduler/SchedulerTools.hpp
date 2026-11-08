// Copyright PolyMech / pm-image. Internal use only.
//
// `schedule_at` / `schedule_in` / `schedule_every` / `schedule_cancel` /
// `schedule_list` agent tools. Backed by media::llm::agent task store.
#pragma once

#include "llm/tools/commons/PathToolCommons.hpp"

namespace media::llm::path {
struct ToolDef;
}

namespace media::llm::path::detail {

ExecuteResult do_schedule_at(const nlohmann::json& args);
ExecuteResult do_schedule_in(const nlohmann::json& args);
ExecuteResult do_schedule_every(const nlohmann::json& args);
ExecuteResult do_schedule_cancel(const nlohmann::json& args);
ExecuteResult do_schedule_list(const nlohmann::json& args);

ToolDef make_schedule_at();
ToolDef make_schedule_in();
ToolDef make_schedule_every();
ToolDef make_schedule_cancel();
ToolDef make_schedule_list();

} // namespace media::llm::path::detail

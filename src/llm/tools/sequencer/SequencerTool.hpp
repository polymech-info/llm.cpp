// Copyright PolyMech / pm-image. Internal use only.
//
// `run_sequence` — deterministic multi-step execution without extra LLM round-trips.
//
// The agent emits a JSON block list (steps).  Each step is either:
//   - a `shell` block  : run a CLI command and optionally capture output as a
//                        named context variable (`storeAs`)
//   - a `wait`  block  : sleep for a precise wall-clock interval (ms)
//   - a `log`   block  : emit a trace message for diagnostics
//
// All steps execute sequentially in C++ — no LLM inference between them —
// with full cancel/timeout support.  Context variables captured via `storeAs`
// are returned in the result so the agent can reference them in follow-up
// tool calls.
#pragma once

#include "llm/tools/commons/PathToolCommons.hpp"

namespace media::llm::path {
struct ToolDef;
}

namespace media::llm::path::detail {

ExecuteResult do_run_sequence(const nlohmann::json& args);
ToolDef       make_run_sequence();

} // namespace media::llm::path::detail

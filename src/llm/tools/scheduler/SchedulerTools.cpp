// Copyright PolyMech / pm-image. Internal use only.
#include "llm/tools/scheduler/SchedulerTools.hpp"

#include "llm/agent_memory.hpp"

#include <chrono>

namespace media::llm::path::detail {

ExecuteResult do_schedule_at(const nlohmann::json& args) {
    if (!args.contains("title") || !args["title"].is_string())
        return simple_error("schedule_at: 'title' (string) is required");
    if (!args.contains("prompt") || !args["prompt"].is_string())
        return simple_error("schedule_at: 'prompt' (string) is required");
    if (!args.contains("run_at") || !args["run_at"].is_string())
        return simple_error("schedule_at: 'run_at' (ISO 8601 string) is required");

    const auto tp = media::llm::agent::parse_iso8601(args["run_at"].get<std::string>());
    if (!tp) return simple_error("schedule_at: could not parse 'run_at' as ISO 8601 datetime");

    media::llm::agent::ScheduledTask task;
    task.title          = args["title"].get<std::string>();
    task.prompt         = args["prompt"].get<std::string>();
    task.schedule.kind  = media::llm::agent::ScheduleKind::At;
    task.schedule.run_at = tp;
    task.next_run_at    = tp;
    if (args.contains("initial_state") && args["initial_state"].is_object())
        task.memory_state = args["initial_state"];
    if (args.contains("folder_hint") && args["folder_hint"].is_string())
        task.folder_hint  = args["folder_hint"].get<std::string>();

    const std::string id = media::llm::agent::task_store_create(std::move(task));
    return simple_ok(nlohmann::json{{"task_id", id}});
}

ExecuteResult do_schedule_in(const nlohmann::json& args) {
    if (!args.contains("title") || !args["title"].is_string())
        return simple_error("schedule_in: 'title' (string) is required");
    if (!args.contains("prompt") || !args["prompt"].is_string())
        return simple_error("schedule_in: 'prompt' (string) is required");
    if (!args.contains("delay_seconds") || !args["delay_seconds"].is_number())
        return simple_error("schedule_in: 'delay_seconds' (integer) is required");

    const int secs = args["delay_seconds"].get<int>();
    if (secs < 1) return simple_error("schedule_in: delay_seconds must be >= 1");

    media::llm::agent::ScheduledTask task;
    task.title         = args["title"].get<std::string>();
    task.prompt        = args["prompt"].get<std::string>();
    task.schedule.kind  = media::llm::agent::ScheduleKind::In;
    task.schedule.delay = std::chrono::seconds(secs);
    if (args.contains("initial_state") && args["initial_state"].is_object())
        task.memory_state = args["initial_state"];
    if (args.contains("folder_hint") && args["folder_hint"].is_string())
        task.folder_hint  = args["folder_hint"].get<std::string>();

    const std::string id = media::llm::agent::task_store_create(std::move(task));
    return simple_ok(nlohmann::json{{"task_id", id}});
}

ExecuteResult do_schedule_every(const nlohmann::json& args) {
    if (!args.contains("title") || !args["title"].is_string())
        return simple_error("schedule_every: 'title' (string) is required");
    if (!args.contains("prompt") || !args["prompt"].is_string())
        return simple_error("schedule_every: 'prompt' (string) is required");
    if (!args.contains("interval_seconds") || !args["interval_seconds"].is_number())
        return simple_error("schedule_every: 'interval_seconds' (integer) is required");

    const int secs = args["interval_seconds"].get<int>();
    if (secs < 5) return simple_error("schedule_every: interval_seconds must be >= 5");

    media::llm::agent::ScheduledTask task;
    task.title              = args["title"].get<std::string>();
    task.prompt             = args["prompt"].get<std::string>();
    task.schedule.kind      = media::llm::agent::ScheduleKind::Every;
    task.schedule.interval  = std::chrono::seconds(secs);

    if (args.contains("start_at") && args["start_at"].is_string()) {
        if (const auto tp = media::llm::agent::parse_iso8601(args["start_at"].get<std::string>()))
            task.schedule.start_at = tp;
    }
    if (args.contains("max_runs") && args["max_runs"].is_number_integer())
        task.schedule.max_runs = args["max_runs"].get<int>();
    if (args.contains("initial_state") && args["initial_state"].is_object())
        task.memory_state      = args["initial_state"];
    if (args.contains("folder_hint") && args["folder_hint"].is_string())
        task.folder_hint       = args["folder_hint"].get<std::string>();

    const std::string id = media::llm::agent::task_store_create(std::move(task));
    return simple_ok(nlohmann::json{{"task_id", id}});
}

ExecuteResult do_schedule_cancel(const nlohmann::json& args) {
    if (!args.contains("task_id") || !args["task_id"].is_string())
        return simple_error("schedule_cancel: 'task_id' (string) is required");
    const std::string id = args["task_id"].get<std::string>();
    if (!media::llm::agent::task_store_cancel(id))
        return simple_error("schedule_cancel: task not found: " + id);
    return simple_ok(nlohmann::json{{"cancelled", id}});
}

ExecuteResult do_schedule_list(const nlohmann::json& /*args*/) {
    const auto tasks = media::llm::agent::task_store_list();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& t : tasks)
        arr.push_back(media::llm::agent::task_to_summary_json(t));
    ExecuteResult r;
    r.ok       = true;
    r.envelope = nlohmann::json{{"ok", true}, {"tasks", std::move(arr)}};
    return r;
}

// ── Catalog schemas ───────────────────────────────────────────────────────

ToolDef make_schedule_at() {
    return ToolDef{
        "schedule_at",
        "Schedule a one-shot agent task to run at an exact UTC time. "
        "Use when the user says 'at 18:00', 'tomorrow morning', or gives an explicit time. "
        "Saved on disk and reloaded after a restart. Runs while the app is open; "
        "if it comes due while the app is closed, it runs once the next time the app starts.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"title",         {{"type", "string"}, {"description", "Short human-readable name for the task."}}},
                {"prompt",        {{"type", "string"}, {"description", "The full prompt to run at fire time."}}},
                {"run_at",        {{"type", "string"}, {"description", "ISO 8601 UTC datetime, e.g. \"2026-05-10T18:30:00Z\"."}}},
                {"initial_state", {{"type", "object"}, {"description", "Optional initial memory_state JSON."}}},
                {"folder_hint",   {{"type", "string"}, {"description", "Optional folder context for the scheduled turn."}}},
            }},
            {"required", nlohmann::json::array({"title", "prompt", "run_at"})},
        }};
}

ToolDef make_schedule_in() {
    return ToolDef{
        "schedule_in",
        "Schedule a one-shot agent task to run after a delay. "
        "Use when the user says 'in 30 seconds', 'after 5 minutes', etc. "
        "Saved on disk and reloaded after a restart. Runs while the app is open; "
        "if it comes due while the app is closed, it runs once the next time the app starts.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"title",          {{"type", "string"}}},
                {"prompt",         {{"type", "string"}}},
                {"delay_seconds",  {{"type", "integer"}, {"minimum", 1}, {"description", "Seconds from now until the task fires."}}},
                {"initial_state",  {{"type", "object"}}},
                {"folder_hint",    {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"title", "prompt", "delay_seconds"})},
        }};
}

ToolDef make_schedule_every() {
    return ToolDef{
        "schedule_every",
        "Schedule a recurring agent task on a fixed interval. "
        "Use when the user says 'every 30 seconds', 'once per hour', 'monitor', or 'keep watching'. "
        "Saved on disk and reloaded after a restart. Runs while the app is open. "
        "A tick that comes due while the app is closed runs once on the next start; "
        "missed intervals are not replayed. Intervals of a few seconds only fire while the app is running.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"title",            {{"type", "string"}}},
                {"prompt",           {{"type", "string"}}},
                {"interval_seconds", {{"type", "integer"}, {"minimum", 5}, {"description", "Seconds between ticks."}}},
                {"start_at",         {{"type", "string"}, {"description", "Optional ISO 8601 UTC start time. Defaults to now + interval."}}},
                {"max_runs",         {{"type", "integer"}, {"minimum", 1}, {"description", "Optional cap on total runs; omit for unlimited."}}},
                {"initial_state",    {{"type", "object"}}},
                {"folder_hint",      {{"type", "string"}}},
            }},
            {"required", nlohmann::json::array({"title", "prompt", "interval_seconds"})},
        }};
}

ToolDef make_schedule_cancel() {
    return ToolDef{
        "schedule_cancel",
        "Disable a saved scheduled task by its id. It stays in the list and will not fire again.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"task_id", {{"type", "string"}, {"description", "The task id returned by schedule_at / schedule_in / schedule_every."}}},
            }},
            {"required", nlohmann::json::array({"task_id"})},
        }};
}

ToolDef make_schedule_list() {
    return ToolDef{
        "schedule_list",
        "List saved scheduled tasks (id, title, schedule, enabled, next_run_at, run_count).",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json::object()},
        }};
}

} // namespace media::llm::path::detail

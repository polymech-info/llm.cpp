// Copyright PolyMech / pm-image. Internal use only.
#include "llm/tools/scheduler/SchedulerTools.hpp"

#include "llm/agent_memory.hpp"

#include <cctype>
#include <chrono>

namespace media::llm::path::detail {

namespace {

std::string apply_schedule_policy(const nlohmann::json& args, media::llm::agent::ScheduledTask& task) {
    if (args.contains("execution") && !args["execution"].is_string())
        return "execution must be a string";
    if (args.contains("precision") && !args["precision"].is_string())
        return "precision must be a string";
    if (args.contains("sound") && !args["sound"].is_boolean())
        return "sound must be a boolean";
    const std::string execution = args.value("execution", std::string{"agent"});
    const std::string precision = args.value("precision", std::string{"best_effort"});
    if (execution != "agent" && execution != "notification" && execution != "tool")
        return "execution must be agent, notification, or tool";
    if (precision != "best_effort" && precision != "time_sensitive")
        return "precision must be best_effort or time_sensitive";
    if (precision == "time_sensitive" && execution != "notification")
        return "precision time_sensitive is only valid for a notification";
    if (args.contains("delivery") && args["delivery"].is_string()) {
        const std::string delivery = args["delivery"].get<std::string>();
        if (delivery != "inbox" && delivery != "notification" && delivery != "silent")
            return "delivery must be inbox, notification, or silent";
    }
    if (execution == "tool") {
        if (!args.contains("tool") || !args["tool"].is_string())
            return "tool name is required";
        const std::string name = args["tool"].get<std::string>();
        if (name.empty())
            return "tool name is required";
        if (!media::llm::agent::scheduled_tool_allowed(name))
            return "tool is not allowlisted";
        if (args.contains("arguments") && !args["arguments"].is_object())
            return "arguments must be an object";
        task.execution = media::llm::agent::ExecutionKind::Tool;
        task.tool_name = name;
        task.tool_args = args.contains("arguments") ? args["arguments"] : nlohmann::json::object();
        if (name == "http_request") {
            const std::string url = task.tool_args.value("url", std::string{});
            if (url.rfind("https://", 0) != 0)
                return "http_request requires an https url";
        }
        task.precision = media::llm::agent::SchedulePrecision::BestEffort;
        task.sound = false;
        task.delivery = media::llm::agent::DeliveryKind::Inbox;
        if (args.contains("delivery") && args["delivery"].is_string()) {
            const std::string delivery = args["delivery"].get<std::string>();
            if (delivery == "notification")
                task.delivery = media::llm::agent::DeliveryKind::Notification;
            else if (delivery == "silent")
                task.delivery = media::llm::agent::DeliveryKind::Silent;
        }
        return {};
    }
    task.execution = execution == "notification"
                         ? media::llm::agent::ExecutionKind::Notification
                         : media::llm::agent::ExecutionKind::Agent;
    task.precision = precision == "time_sensitive"
                         ? media::llm::agent::SchedulePrecision::TimeSensitive
                         : media::llm::agent::SchedulePrecision::BestEffort;
    task.sound = args.value("sound", false);
    if (task.execution == media::llm::agent::ExecutionKind::Notification)
        task.delivery = media::llm::agent::DeliveryKind::Notification;
    else if (args.contains("delivery") && args["delivery"].is_string()) {
        const std::string delivery = args["delivery"].get<std::string>();
        if (delivery == "notification")
            task.delivery = media::llm::agent::DeliveryKind::Notification;
        else if (delivery == "silent")
            task.delivery = media::llm::agent::DeliveryKind::Silent;
        else
            task.delivery = media::llm::agent::DeliveryKind::Inbox;
        if (task.delivery != media::llm::agent::DeliveryKind::Notification)
            task.sound = false;
    } else {
        task.delivery = media::llm::agent::DeliveryKind::Inbox;
        task.sound = false;
    }
    return {};
}

nlohmann::json schedule_extra_properties() {
    return {
        {"execution",
         {{"type", "string"},
          {"enum", nlohmann::json::array({"agent", "notification", "tool"})},
          {"description",
           "agent runs the prompt as a model turn. notification shows title and prompt "
           "as a reminder and does not call the model. tool runs one allowlisted call "
           "(write_file or https http_request) with arguments frozen at create and does not call the model. "
           "Default agent."}}},
        {"tool",
         {{"type", "string"},
          {"description", "Tool name for execution=tool. write_file or http_request."}}},
        {"arguments",
         {{"type", "object"},
          {"description", "Arguments for execution=tool. Stored as given and reused on each fire."}}},
        {"precision",
         {{"type", "string"},
          {"enum", nlohmann::json::array({"best_effort", "time_sensitive"})},
          {"description",
           "time_sensitive is only valid for notification. The phone uses an exact alarm "
           "when that permission is granted. Default best_effort."}}},
        {"sound",
         {{"type", "boolean"},
          {"description", "Play a sound for a notification reminder. Default false."}}},
    };
}

void add_schedule_extras(nlohmann::json& properties) {
    const auto extras = schedule_extra_properties();
    for (auto it = extras.begin(); it != extras.end(); ++it)
        properties[it.key()] = it.value();
}

} // namespace

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
    if (const std::string policy = apply_schedule_policy(args, task); !policy.empty())
        return simple_error(std::string("schedule_at: ") + policy);

    const std::string id = media::llm::agent::task_store_create(std::move(task));
    if (id.empty())
        return simple_error("could not save the scheduled task");
    const auto saved = media::llm::agent::task_store_get(id);
    if (!saved)
        return simple_ok(nlohmann::json{{"task_id", id}});
    return simple_ok(media::llm::agent::task_saved_json(*saved));
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
    if (const std::string policy = apply_schedule_policy(args, task); !policy.empty())
        return simple_error(std::string("schedule_in: ") + policy);

    const std::string id = media::llm::agent::task_store_create(std::move(task));
    if (id.empty())
        return simple_error("could not save the scheduled task");
    const auto saved = media::llm::agent::task_store_get(id);
    if (!saved)
        return simple_ok(nlohmann::json{{"task_id", id}});
    return simple_ok(media::llm::agent::task_saved_json(*saved));
}

ExecuteResult do_schedule_every(const nlohmann::json& args) {
    if (!args.contains("title") || !args["title"].is_string())
        return simple_error("schedule_every: 'title' (string) is required");
    if (!args.contains("prompt") || !args["prompt"].is_string())
        return simple_error("schedule_every: 'prompt' (string) is required");
    const bool daily = args.contains("time") && args["time"].is_string();
    if (!daily && (!args.contains("interval_seconds") || !args["interval_seconds"].is_number()))
        return simple_error("schedule_every: 'interval_seconds' (integer) is required");

    media::llm::agent::ScheduledTask task;
    task.title = args["title"].get<std::string>();
    task.prompt = args["prompt"].get<std::string>();
    if (daily) {
        const std::string clock = args["time"].get<std::string>();
        int hour = 0;
        int minute = 0;
        if (clock.size() != 5 || clock[2] != ':'
            || !std::isdigit(static_cast<unsigned char>(clock[0]))
            || !std::isdigit(static_cast<unsigned char>(clock[1]))
            || !std::isdigit(static_cast<unsigned char>(clock[3]))
            || !std::isdigit(static_cast<unsigned char>(clock[4])))
            return simple_error("schedule_every: time must be HH:MM");
        hour = std::stoi(clock.substr(0, 2));
        minute = std::stoi(clock.substr(3, 2));
        if (hour > 23 || minute > 59)
            return simple_error("schedule_every: time must be HH:MM");
        task.schedule.kind = media::llm::agent::ScheduleKind::Daily;
        task.schedule.hour = hour;
        task.schedule.minute = minute;
        if (args.contains("timezone") && args["timezone"].is_string())
            task.schedule.timezone = args["timezone"].get<std::string>();
    } else {
        const int secs = args["interval_seconds"].get<int>();
        if (secs < 5) return simple_error("schedule_every: interval_seconds must be >= 5");
        task.schedule.kind = media::llm::agent::ScheduleKind::Every;
        task.schedule.interval = std::chrono::seconds(secs);
    }

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
    if (const std::string policy = apply_schedule_policy(args, task); !policy.empty())
        return simple_error(std::string("schedule_every: ") + policy);

    const std::string id = media::llm::agent::task_store_create(std::move(task));
    if (id.empty())
        return simple_error("could not save the scheduled task");
    const auto saved = media::llm::agent::task_store_get(id);
    if (!saved)
        return simple_ok(nlohmann::json{{"task_id", id}});
    return simple_ok(media::llm::agent::task_saved_json(*saved));
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
    nlohmann::json properties = {
        {"title",         {{"type", "string"}, {"description", "Short human-readable name for the task."}}},
        {"prompt",        {{"type", "string"}, {"description", "The full prompt to run at fire time. For a notification this is the reminder text."}}},
        {"run_at",        {{"type", "string"}, {"description", "ISO 8601 UTC datetime, e.g. \"2026-05-10T18:30:00Z\"."}}},
        {"initial_state", {{"type", "object"}, {"description", "Optional initial memory_state JSON."}}},
        {"folder_hint",   {{"type", "string"}, {"description", "Optional folder context for the scheduled turn."}}},
    };
    add_schedule_extras(properties);
    return ToolDef{
        "schedule_at",
        "Schedule a one-shot task at an exact UTC time. "
        "Use when the user says 'at 18:00', 'tomorrow morning', or gives an explicit time. "
        "execution=agent (default) runs the prompt. execution=notification shows the title and "
        "prompt as a reminder and does not call the model. precision=time_sensitive is only "
        "valid for notification; Android uses an exact alarm when that permission is granted. "
        "Saved on disk. An agent task runs while the app is open, and once on the next start "
        "if it came due while the app was closed.",
        {
            {"type", "object"},
            {"properties", std::move(properties)},
            {"required", nlohmann::json::array({"title", "prompt", "run_at"})},
        }};
}

ToolDef make_schedule_in() {
    nlohmann::json properties = {
        {"title",          {{"type", "string"}}},
        {"prompt",         {{"type", "string"}, {"description", "Prompt for an agent turn, or the reminder text for a notification."}}},
        {"delay_seconds",  {{"type", "integer"}, {"minimum", 1}, {"description", "Seconds from now until the task fires."}}},
        {"initial_state",  {{"type", "object"}}},
        {"folder_hint",    {{"type", "string"}}},
    };
    add_schedule_extras(properties);
    return ToolDef{
        "schedule_in",
        "Schedule a one-shot task to run after a delay. "
        "Use when the user says 'in 30 seconds', 'after 5 minutes', or 'remind me'. "
        "execution=agent (default) runs the prompt. execution=notification is the reminder "
        "and does not call the model. execution=tool runs write_file with arguments frozen "
        "at create and does not call the model. precision=time_sensitive is only valid for notification. "
        "Saved on disk. An agent task runs while the app is open, and once on the next start "
        "if it came due while the app was closed.",
        {
            {"type", "object"},
            {"properties", std::move(properties)},
            {"required", nlohmann::json::array({"title", "prompt", "delay_seconds"})},
        }};
}

ToolDef make_schedule_every() {
    nlohmann::json properties = {
        {"title",            {{"type", "string"}}},
        {"prompt",           {{"type", "string"}}},
        {"interval_seconds", {{"type", "integer"}, {"minimum", 5}, {"description", "Seconds between ticks. Omit when time is set."}}},
        {"time", {{"type", "string"}, {"description", "Daily local clock HH:MM. Uses timezone, or UTC when timezone is omitted."}}},
        {"timezone", {{"type", "string"}, {"description", "IANA zone for time, for example Europe/Madrid."}}},
        {"start_at",         {{"type", "string"}, {"description", "Optional ISO 8601 UTC start time. Defaults to now + interval."}}},
        {"max_runs",         {{"type", "integer"}, {"minimum", 1}, {"description", "Optional cap on total runs; omit for unlimited."}}},
        {"initial_state",    {{"type", "object"}}},
        {"folder_hint",      {{"type", "string"}}},
    };
    add_schedule_extras(properties);
    return ToolDef{
        "schedule_every",
        "Schedule a recurring task on a fixed interval. "
        "Use when the user says 'every 30 seconds', 'once per hour', 'monitor', or 'keep watching'. "
        "execution=agent (default) runs the prompt each tick. execution=notification repeats the "
        "reminder and does not call the model. precision=time_sensitive is only valid for notification. "
        "Saved on disk. A tick that comes due while the app is closed runs once on the next start; "
        "missed intervals are not replayed. Intervals of a few seconds only fire while the app is running.",
        {
            {"type", "object"},
            {"properties", std::move(properties)},
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
        "List saved scheduled tasks (id, title, schedule, execution, enabled, next_run_at, run_count).",
        nlohmann::json{
            {"type", "object"},
            {"properties", nlohmann::json::object()},
        }};
}

} // namespace media::llm::path::detail

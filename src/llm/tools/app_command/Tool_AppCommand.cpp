#include "llm/tools/app_command/Tool_AppCommand.hpp"

#include "commands/app_invocation.hpp"

#if defined(_WIN32)
#include "core/host_backends.hpp"
#if defined(FEATURE_WINDOWS_GPO) && FEATURE_WINDOWS_GPO
#include "policy/app_policy.hpp"
#endif
#endif

#include <string>

namespace media::llm::app_command {
namespace {

using media::llm::path::ExecuteResult;
using media::llm::path::ToolDef;

ExecuteResult fatal(const std::string& message)
{
    ExecuteResult result;
    result.ok = false;
    result.error = message;
    result.envelope = nlohmann::json{{"ok", false}, {"error", message}};
    return result;
}

ExecuteResult tool_ok(nlohmann::json payload)
{
    payload["ok"] = true;
    payload["tool"] = "app_command";
    ExecuteResult result;
    result.ok = true;
    result.envelope = std::move(payload);
    return result;
}

ToolDef make_app_command()
{
    return {
        "app_command",
        "Run a Tanit app command on the running UI instance (same verbs as the command "
        "palette). Discover ids with info_lookup kind=app_command (or kind=command). "
        "Use openurl to load a page in the centre browser, open/edit/browse for files, "
        "togglechat/togglecenterview for layout, takescreenshot for capture. Prefer "
        "browser_navigate for in-page back/reload after a page is already open.",
        {
            {"type", "object"},
            {"properties", {
                {"command", {
                    {"type", "string"},
                    {"description", "App command id or alias. Look up ids with info_lookup kind=app_command."},
                }},
                {"url", {
                    {"type", "string"},
                    {"description", "For openurl. Absolute http(s) or CMS-relative path such as /user/…"},
                }},
                {"path", {
                    {"type", "string"},
                    {"description", "For open, edit, browse, replay. Supports ${CURRENT_FILE} and ${KNOWNFOLDER:…}."},
                }},
                {"paths", {
                    {"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"description", "Multiple paths for open/edit/browse (semicolon-joined for the app bridge)."},
                }},
                {"extra", {
                    {"type", "string"},
                    {"description", "Raw pipe payload appended after command| when none of url/path/paths apply."},
                }},
            }},
            {"required", nlohmann::json::array({"command"})},
        },
    };
}

#if defined(_WIN32)
std::string build_payload(const nlohmann::json& args, std::string& err)
{
    if (!args.contains("command") || !args["command"].is_string()) {
        err = "app_command: 'command' is required";
        return {};
    }

    const pm::host::app_cmd::Command cmd =
        pm::host::app_cmd::parse(args["command"].get<std::string>());
    if (cmd == pm::host::app_cmd::Command::Unknown) {
        err = "app_command: unknown command '" + args["command"].get<std::string>() + "'";
        return {};
    }
    if (cmd == pm::host::app_cmd::Command::SwitchWorkbench) {
        err = "app_command: switchworkbench is not available via this tool";
        return {};
    }
    if (!pm::host::app_cmd::is_available(cmd)) {
        err = "app_command: command is unavailable in this build";
        return {};
    }
#if defined(FEATURE_WINDOWS_GPO) && FEATURE_WINDOWS_GPO
    if (!pm::policy::app_cmd_allowed(static_cast<int>(cmd))) {
        err = "app_command: command is blocked by policy";
        return {};
    }
#endif

    return media::commands::encode_invocation(pm::host::app_cmd::name(cmd), args);
}
#endif

} // namespace

std::vector<ToolDef> tool_defs()
{
#if defined(_WIN32) && defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT
    return {make_app_command()};
#else
    return {};
#endif
}

bool is_app_command_tool(const std::string& name)
{
    return name == "app_command";
}

ExecuteResult execute(const std::string& name, const nlohmann::json& args)
{
    if (!is_app_command_tool(name))
        return fatal("unknown app command tool: " + name);
    if (!args.is_object())
        return fatal(name + ": arguments must be an object");
#if !defined(_WIN32)
    return fatal(name + ": app_command requires Windows");
#else
    std::string err;
    const std::string payload = build_payload(args, err);
    if (payload.empty())
        return fatal(err.empty() ? "app_command: invalid arguments" : err);

    if (!pm::host::forward_app_command_to_primary(payload))
        return fatal("app_command: no running UI instance to receive the command");

    const pm::host::app_cmd::Command cmd =
        pm::host::app_cmd::parse(args["command"].get<std::string>());
    nlohmann::json out{
        {"command", args.value("command", std::string{})},
        {"payload", payload},
        {"dispatched", true},
    };
    if (const char* lbl = pm::host::app_cmd::label(cmd))
        out["label"] = lbl;
    return tool_ok(std::move(out));
#endif
}

} // namespace media::llm::app_command

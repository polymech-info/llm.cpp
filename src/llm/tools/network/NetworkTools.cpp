// Copyright PolyMech / pm-image. Internal use only.
#include "llm/tools/network/NetworkTools.hpp"
#include "fs/path_sanitizer.hpp"

#include "core/cli_cancel.hpp"
#include "lib/net/mqtt_client.hpp"
#include "lib/net/ssh_exec.hpp"
#include "logging/pm_log.hpp"

#if defined(FEATURE_MODBUS) && FEATURE_MODBUS
#include "modbus/modbus_args.hpp"
#include "modbus/modbus_client.hpp"
#endif

#if defined(__ANDROID__)
#include "core/settings_store.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <optional>
#include "string/string_utils.hpp"

namespace media::llm::path::detail {

namespace {

constexpr const char k_log_ch[] = "llm_network_tools";
constexpr std::size_t k_stream_cap = 32 * 1024;

std::string arg_str(const nlohmann::json& args, const char* key, std::string fallback = {})
{
    if (args.contains(key) && args[key].is_string())
        return args[key].get<std::string>();
    return fallback;
}

std::optional<long long> arg_int_opt(const nlohmann::json& args, const char* key)
{
    if (!args.contains(key))
        return std::nullopt;
    const auto& v = args[key];
    if (v.is_number_integer() || v.is_number_unsigned())
        return v.get<long long>();
    if (v.is_number_float())
        return static_cast<long long>(v.get<double>());
    if (v.is_string()) {
        try { return std::stoll(v.get<std::string>()); } catch (...) {}
    }
    return std::nullopt;
}

int arg_int(const nlohmann::json& args, const char* key, long long fallback, long long lo, long long hi)
{
    return static_cast<int>(std::clamp(arg_int_opt(args, key).value_or(fallback), lo, hi));
}

bool arg_bool(const nlohmann::json& args, const char* key, bool fallback)
{
    if (args.contains(key) && args[key].is_boolean())
        return args[key].get<bool>();
    return fallback;
}


ExecuteResult envelope_result(bool ok, nlohmann::json env, const std::string& error = {})
{
    ExecuteResult r;
    r.ok = ok;
    env["ok"] = ok;
    if (!ok) {
        r.error = error;
        env["error"] = error;
    }
    r.envelope = std::move(env);
    return r;
}

void put_clipped(nlohmann::json& env, const char* key, const char* clipped_key, const std::string& text)
{
    const bool clipped = text.size() > k_stream_cap;
    env[key] = clipped ? text.substr(0, k_stream_cap) : text;
    env[clipped_key] = clipped;
}

// ── ssh_exec ─────────────────────────────────────────────────────────────────

ExecuteResult do_ssh_exec(const nlohmann::json& args)
{
    media::net::SshExecOptions o;
    o.host                = arg_str(args, "host");
    o.command             = arg_str(args, "command");
    o.username            = arg_str(args, "username");
    o.private_key         = arg_str(args, "private_key");
    o.accept_unknown_host = arg_bool(args, "accept_unknown_host", false);
    o.timeout_ms          = arg_int(args, "timeout_ms", 30000, 1000, 120000);
    if (auto p = arg_int_opt(args, "port")) {
        o.port = static_cast<int>(std::clamp<long long>(*p, 1, 65535));
        o.port_set = true;
    }
#if defined(__ANDROID__)
    o.ssh_dir = media::path::path_to_utf8((media::settings::get_config_dir() / "ssh"));
#endif
    o.cancelled = [] { return media::cli::cancel_requested(); };

    if (o.host.empty())
        return simple_error("ssh_exec: 'host' is required");
    if (o.command.empty())
        return simple_error("ssh_exec: 'command' is required");

    pm::log::info(k_log_ch, "ssh_exec host=" + o.host);
    const auto r = media::net::ssh_exec(o);

    nlohmann::json env{{"host", o.host}};
    if (o.port_set)
        env["port"] = o.port;
    if (r.ran) {
        env["exit_status"] = r.exit_status;
        put_clipped(env, "stdout", "stdout_clipped", r.stdout_text);
        put_clipped(env, "stderr", "stderr_clipped", r.stderr_text);
    }
    return envelope_result(r.ok, std::move(env), r.error);
}

// ── mqtt ─────────────────────────────────────────────────────────────────────

ExecuteResult do_mqtt(const nlohmann::json& args)
{
    media::net::MqttClientOptions o;
    o.action     = media::strutil::lower_ascii(arg_str(args, "action"));
    o.topic      = arg_str(args, "topic");
    o.payload    = arg_str(args, "payload");
    o.host       = arg_str(args, "host", "127.0.0.1");
    o.port       = arg_int(args, "port", 1883, 1, 65535);
    o.qos        = arg_int(args, "qos", 0, 0, 2);
    o.client_id  = arg_str(args, "client_id", "tanit-mqtt");
    o.timeout_ms = arg_int(args, "timeout_ms", 5000, 100, 60000);
    o.cancelled  = [] { return media::cli::cancel_requested(); };

    if (o.action != "publish" && o.action != "subscribe" && o.action != "roundtrip")
        return simple_error("mqtt: 'action' must be publish, subscribe, or roundtrip");
    if (o.topic.empty())
        return simple_error("mqtt: 'topic' is required");
    if (o.host.empty())
        o.host = "127.0.0.1";

    pm::log::info(k_log_ch, "mqtt " + o.action + " " + o.host + ":" + std::to_string(o.port) + " topic=" + o.topic);
    const auto r = media::net::mqtt_client_run(o);

    nlohmann::json messages = nlohmann::json::array();
    for (const auto& [topic, payload] : r.messages)
        messages.push_back({{"topic", topic}, {"payload", payload}});
    nlohmann::json env{
        {"action", o.action},
        {"host", o.host},
        {"port", o.port},
        {"topic", o.topic},
        {"connected", r.connected},
        {"published", r.published},
        {"subscribed", r.subscribed},
        {"messages", std::move(messages)},
    };
    return envelope_result(r.ok, std::move(env), r.error);
}

// ── modbus_read / modbus_write ───────────────────────────────────────────────

#if defined(FEATURE_MODBUS) && FEATURE_MODBUS

nlohmann::json values_json(const std::vector<std::uint16_t>& v)
{
    nlohmann::json a = nlohmann::json::array();
    for (auto x : v) a.push_back(x);
    return a;
}

nlohmann::json values_json(const std::vector<std::uint8_t>& v)
{
    nlohmann::json a = nlohmann::json::array();
    for (auto x : v) a.push_back(x ? 1 : 0);
    return a;
}

struct ModbusTarget {
    std::string url;
    std::string area;
    int address = 0;
    int slave = 0;
    int timeout_ms = 5000;
};

std::optional<std::string> modbus_target(const nlohmann::json& args, bool write, ModbusTarget& t)
{
    t.url = arg_str(args, "url");
    if (t.url.empty())
        return std::string("'url' is required (tcp:host:port[:unit])");
    media::modbus::Endpoint ep;
    std::string err;
    if (!media::modbus::parse_endpoint(t.url, ep, err))
        return err;
    const std::string area = arg_str(args, "area");
    t.area = write ? media::modbus::normalize_write_area(area) : media::modbus::normalize_area(area);
    t.address = media::modbus::normalize_address(arg_int(args, "address", 0, 0, 65535), t.area,
                                                 arg_str(args, "address_base", "zero"));
    t.slave = arg_int(args, "slave", 0, 0, 247);
    t.timeout_ms = arg_int(args, "timeout_ms", 5000, 100, 30000);
    return std::nullopt;
}

nlohmann::json modbus_env(const ModbusTarget& t)
{
    return {{"url", t.url}, {"area", t.area}, {"address", t.address}};
}

ExecuteResult do_modbus_read(const nlohmann::json& args)
{
    ModbusTarget t;
    if (auto e = modbus_target(args, false, t))
        return simple_error("modbus_read: " + *e);
    const bool bits = t.area == "coils" || t.area == "discreteInputs";
    const int count = arg_int(args, "count", 1, 1, bits ? 2000 : 125);

    pm::log::info(k_log_ch, "modbus_read " + t.url + " " + t.area + "@" + std::to_string(t.address));
    std::string err;
    nlohmann::json env = modbus_env(t);
    env["count"] = count;
    bool ok = false;
    if (bits) {
        std::vector<std::uint8_t> v;
        ok = t.area == "coils"
            ? media::modbus::read_coils(t.url, t.address, count, t.slave, t.timeout_ms, v, err)
            : media::modbus::read_discrete_inputs(t.url, t.address, count, t.slave, t.timeout_ms, v, err);
        if (ok) env["values"] = values_json(v);
    } else {
        std::vector<std::uint16_t> v;
        ok = t.area == "inputRegisters"
            ? media::modbus::read_input_registers(t.url, t.address, count, t.slave, t.timeout_ms, v, err)
            : media::modbus::read_holding_registers(t.url, t.address, count, t.slave, t.timeout_ms, v, err);
        if (ok) env["values"] = values_json(v);
    }
    return envelope_result(ok, std::move(env), err.empty() ? "modbus read failed" : err);
}

ExecuteResult do_modbus_write(const nlohmann::json& args)
{
    ModbusTarget t;
    if (auto e = modbus_target(args, true, t))
        return simple_error("modbus_write: " + *e);
    const nlohmann::json& raw = args.contains("values") ? args["values"]
                              : args.contains("value")  ? args["value"]
                              : nlohmann::json();

    pm::log::info(k_log_ch, "modbus_write " + t.url + " " + t.area + "@" + std::to_string(t.address));
    std::string err;
    nlohmann::json env = modbus_env(t);
    bool ok = false;
    if (t.area == "coils") {
        const auto v = media::modbus::bit_values(raw);
        if (v.empty())
            return simple_error("modbus_write: 'value' or 'values' is required");
        ok = v.size() == 1
            ? media::modbus::write_coil(t.url, t.address, v[0] != 0, t.slave, t.timeout_ms, err)
            : media::modbus::write_coils(t.url, t.address, v, t.slave, t.timeout_ms, err);
        env["values"] = values_json(v);
    } else {
        const auto v = media::modbus::uint16_values(raw);
        if (v.empty())
            return simple_error("modbus_write: 'value' or 'values' is required");
        ok = v.size() == 1
            ? media::modbus::write_register(t.url, t.address, v[0], t.slave, t.timeout_ms, err)
            : media::modbus::write_registers(t.url, t.address, v, t.slave, t.timeout_ms, err);
        env["values"] = values_json(v);
    }
    return envelope_result(ok, std::move(env), err.empty() ? "modbus write failed" : err);
}

#endif

// ── Schemas ──────────────────────────────────────────────────────────────────

nlohmann::json int_prop(const char* desc, long long lo, long long hi)
{
    return {{"type", "integer"}, {"minimum", lo}, {"maximum", hi}, {"description", desc}};
}

nlohmann::json str_prop(const char* desc)
{
    return {{"type", "string"}, {"description", desc}};
}

ToolDef make_ssh_exec()
{
    return ToolDef{
        "ssh_exec",
        "Run one command on a remote host over SSH (key auth) and return exit_status, stdout, stderr. "
        "Use the host the user named: an ~/.ssh/config alias or a hostname. "
        "One connection per call; no interactive shell, no PTY. For local commands use run.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"host", str_prop("SSH config alias or hostname, exactly as the user gave it.")},
                {"command", str_prop("One non-interactive remote command, e.g. uptime.")},
                {"username", str_prop("Optional. Default comes from the SSH config.")},
                {"port", int_prop("Optional. Default comes from the SSH config, else 22.", 1, 65535)},
                {"private_key", str_prop("Optional absolute path to a private key. Default: the user's SSH keys.")},
                {"accept_unknown_host", {{"type", "boolean"},
                    {"description", "Add an unknown host key to known_hosts. Only when the user asked. Default false."}}},
                {"timeout_ms", int_prop("Connect and command timeout. Default 30000.", 1000, 120000)},
            }},
            {"required", nlohmann::json::array({"host", "command"})},
        }};
}

ToolDef make_mqtt()
{
    return ToolDef{
        "mqtt",
        "One MQTT exchange with a broker. publish sends a message; subscribe waits for one message on the topic; "
        "roundtrip subscribes, publishes, and returns the echo. Use the broker host the user named.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"action", {{"type", "string"}, {"enum", {"publish", "subscribe", "roundtrip"}}}},
                {"topic", str_prop("MQTT topic.")},
                {"payload", str_prop("Message body for publish and roundtrip.")},
                {"host", str_prop("Broker host. Default 127.0.0.1.")},
                {"port", int_prop("Broker port. Default 1883.", 1, 65535)},
                {"qos", int_prop("QoS 0-2. Default 0.", 0, 2)},
                {"client_id", str_prop("Optional client id. Default tanit-mqtt.")},
                {"timeout_ms", int_prop("Default 5000.", 100, 60000)},
            }},
            {"required", nlohmann::json::array({"action", "topic"})},
        }};
}

#if defined(FEATURE_MODBUS) && FEATURE_MODBUS
nlohmann::json modbus_common_props()
{
    return {
        {"url", str_prop("Device endpoint: tcp:<host>:<port>[:<unit>], e.g. tcp:192.168.1.20:502.")},
        {"address", int_prop("Zero-based address. 40001/30001/10001 reference forms are accepted.", 0, 65535)},
        {"slave", int_prop("Unit id. 0 keeps the unit from the url.", 0, 247)},
        {"timeout_ms", int_prop("Default 5000.", 100, 30000)},
    };
}

ToolDef make_modbus_read()
{
    auto props = modbus_common_props();
    props["area"] = {{"type", "string"},
                     {"enum", {"holdingRegisters", "inputRegisters", "coils", "discreteInputs"}},
                     {"description", "Default holdingRegisters."}};
    props["count"] = int_prop("Number of registers (max 125) or bits (max 2000). Default 1.", 1, 2000);
    return ToolDef{
        "modbus_read",
        "Read Modbus registers or bits from a device (Modbus TCP). Returns values as an array. "
        "One connection per call.",
        nlohmann::json{{"type", "object"}, {"properties", props},
                       {"required", nlohmann::json::array({"url"})}}};
}

ToolDef make_modbus_write()
{
    auto props = modbus_common_props();
    props["area"] = {{"type", "string"}, {"enum", {"holdingRegisters", "coils"}},
                     {"description", "Default holdingRegisters."}};
    props["value"] = int_prop("One register value (0-65535), or a coil state 0/1.", 0, 65535);
    props["values"] = {{"type", "array"}, {"items", {{"type", "integer"}}},
                       {"description", "Several consecutive values starting at address. Use instead of value."}};
    return ToolDef{
        "modbus_write",
        "Write Modbus holding registers or coils on a device (Modbus TCP). This changes device state; "
        "only write what the user asked for.",
        nlohmann::json{{"type", "object"}, {"properties", props},
                       {"required", nlohmann::json::array({"url"})}}};
}
#endif

} // namespace

bool is_network_tool(std::string_view name) noexcept
{
    return name == "ssh_exec" || name == "mqtt" || name == "modbus_read" || name == "modbus_write";
}

ExecuteResult do_network_tool(const std::string& name, const nlohmann::json& args)
{
    if (name == "ssh_exec") {
        if (!media::net::ssh_exec_available())
            return fatal("ssh_exec is not available in this build");
        return do_ssh_exec(args);
    }
    if (name == "mqtt") {
        if (!media::net::mqtt_client_available())
            return fatal("mqtt is not available in this build");
        return do_mqtt(args);
    }
#if defined(FEATURE_MODBUS) && FEATURE_MODBUS
    if (name == "modbus_read")  return do_modbus_read(args);
    if (name == "modbus_write") return do_modbus_write(args);
#endif
    return fatal("network tool is not available in this build: " + name);
}

std::vector<ToolDef> network_tool_catalog()
{
    std::vector<ToolDef> out;
    if (media::net::ssh_exec_available())
        out.push_back(make_ssh_exec());
    if (media::net::mqtt_client_available())
        out.push_back(make_mqtt());
#if defined(FEATURE_MODBUS) && FEATURE_MODBUS
    out.push_back(make_modbus_read());
    out.push_back(make_modbus_write());
#endif
    return out;
}

} // namespace media::llm::path::detail

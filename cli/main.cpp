// pm-llm — standalone agent CLI.
// Opens the same encrypted settings profile Tanit uses
// (pm://config/settings.json under the brand config directory) and applies
// the shared llm-agent flags. The turn runner still lives in tanit until the
// agent loop is linked here; `agent` resolves the profile and exits.

#include "llm/agent_cli.hpp"
#include "host/services.hpp"
#include "pm_branding_config.hpp"
#include "storage/secure_storage.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <shlobj.h>
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr const char* k_settings_uri = "pm://config/settings.json";

void emit(const nlohmann::json& body, int code)
{
    std::cout << body.dump(2) << "\n";
    std::exit(code);
}

void emit_err(const std::string& message, int code = 1)
{
    emit({{"ok", false}, {"error", message}}, code);
}

std::string lower_ascii(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

void redact(nlohmann::json& node)
{
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            const std::string key = lower_ascii(it.key());
            const bool secret = key.find("api_key") != std::string::npos
                || key.find("apikey") != std::string::npos
                || key.find("token") != std::string::npos
                || key.find("secret") != std::string::npos
                || key.find("password") != std::string::npos
                || key == "key";
            if (secret && it.value().is_string()) {
                it.value() = it.value().get<std::string>().empty() ? "" : "***";
                continue;
            }
            redact(it.value());
        }
        return;
    }
    if (node.is_array()) {
        for (auto& item : node)
            redact(item);
    }
}

fs::path roaming_config_dir()
{
#if defined(_WIN32)
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) || !raw)
        return {};
    fs::path dir(raw);
    CoTaskMemFree(raw);
    return dir / pm::brand::k_config_subpath_w;
#else
    return {};
#endif
}

fs::path resolve_config_dir(const std::string& flag)
{
    if (!flag.empty())
        return fs::path(flag);
    if (const char* env = std::getenv("PM_LLM_CONFIG_DIR"); env && *env)
        return fs::path(env);
    return roaming_config_dir();
}

bool read_file(const fs::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

nlohmann::json load_profile(const fs::path& config_dir, std::string& source, std::string& err)
{
    source.clear();
    err.clear();
    const auto paths = pm::storage::resolve_storage_paths(config_dir);
    pm::storage::DocumentLoadPolicy policy;
    policy.require_signature = true;
    policy.require_encryption = true;
    policy.allowed_trust = {pm::model::TrustLevel::UserLocal};
    const auto loaded = pm::storage::load_or_import_seed_document(paths, k_settings_uri, policy);
    if (loaded.ok) {
        source = "encrypted";
        const std::string text(reinterpret_cast<const char*>(loaded.document.data.data()),
                               loaded.document.data.size());
        auto parsed = nlohmann::json::parse(text, nullptr, false);
        if (parsed.is_discarded())
            err = "encrypted settings profile is not valid JSON";
        return parsed;
    }
    if (loaded.error.rfind("document not found:", 0) != 0 && !loaded.error.empty()) {
        err = loaded.error;
        return {};
    }
    const fs::path plain = config_dir / "settings.json";
    std::string text;
    if (!read_file(plain, text)) {
        err = "no encrypted settings profile and no settings.json under " + config_dir.string();
        return {};
    }
    source = "settings.json";
    return nlohmann::json::parse(text, nullptr, false);
}

const nlohmann::json* preset_list(const nlohmann::json& root)
{
    if (!root.is_object() || !root.contains("custom") || !root["custom"].is_object())
        return nullptr;
    const auto& custom = root["custom"];
    if (!custom.contains("settings_chat_presets") || !custom["settings_chat_presets"].is_array())
        return nullptr;
    return &custom["settings_chat_presets"];
}

const nlohmann::json* find_preset(const nlohmann::json& root, const std::string& name)
{
    const auto* list = preset_list(root);
    if (!list)
        return nullptr;
    const std::string needle = lower_ascii(name);
    const nlohmann::json* marked_default = nullptr;
    const nlohmann::json* named_default = nullptr;
    int objects = 0;
    const nlohmann::json* only = nullptr;
    for (const auto& item : *list) {
        if (!item.is_object())
            continue;
        ++objects;
        only = &item;
        const bool is_default = (item.contains("is_default") && item["is_default"].is_boolean() && item["is_default"].get<bool>())
            || (item.contains("isDefault") && item["isDefault"].is_boolean() && item["isDefault"].get<bool>());
        const std::string id = lower_ascii(item.value("id", std::string{}));
        const std::string preset_name = lower_ascii(item.value("name", std::string{}));
        if (!needle.empty()) {
            if (id == needle || preset_name == needle)
                return &item;
            continue;
        }
        if (is_default && !marked_default)
            marked_default = &item;
        if ((id == "default" || preset_name == "default") && !named_default)
            named_default = &item;
    }
    if (!needle.empty())
        return nullptr;
    if (marked_default)
        return marked_default;
    if (named_default)
        return named_default;
    if (objects == 1)
        return only;
    return nullptr;
}

nlohmann::json chat_from(const nlohmann::json& root, const nlohmann::json* preset)
{
    nlohmann::json chat = nlohmann::json::object();
    if (preset && preset->contains("chat") && (*preset)["chat"].is_object())
        chat = (*preset)["chat"];
    else if (root.contains("chat") && root["chat"].is_object())
        chat = root["chat"];
    return chat;
}

void overlay_string(nlohmann::json& chat, const char* key, const std::string& value)
{
    if (!value.empty())
        chat[key] = value;
}

void add_agent_flags(CLI::App* cmd, media::llm::agent::cliopt::AgentCliOptions& opt)
{
    cmd->add_option("-p,--prompt", opt.prompt, "User prompt");
    cmd->add_option("--preset", opt.preset, "Chat settings preset name or id");
    cmd->add_option("--runner", opt.runner, "native | claude-code | codex");
    cmd->add_option("--router", opt.router, "Provider router");
    cmd->add_option("--model", opt.model, "Model id");
    cmd->add_option("--api-key", opt.api_key, "API key override");
    cmd->add_option("--base-url", opt.base_url, "OpenAI-compatible base URL");
    cmd->add_option("--timeout-ms", opt.timeout_ms, "HTTP timeout per round");
    cmd->add_option("--max-iter", opt.max_iterations, "Tool-call iteration cap");
    cmd->add_option("--type", opt.api_mode, "completion | responses | realtime");
    cmd->add_flag("--no-tools", opt.no_tools, "Text turn only");
    cmd->add_flag("--no-mcp", opt.no_mcp);
    cmd->add_flag("--no-skills", opt.no_skills);
    cmd->add_flag("--skills", opt.skills);
    cmd->add_flag("--planner", opt.planner);
    cmd->add_flag("--no-planner", opt.no_planner);
    cmd->add_option("--session-id", opt.session_id);
    cmd->add_option("--include", opt.include_paths)->expected(-1);
}

} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"pm-llm"};
    app.require_subcommand(1);
    std::string config_flag;
    app.add_option("--config-dir", config_flag,
        "Tanit config directory. Default: %APPDATA%/<vendor>/<config-slug>, or PM_LLM_CONFIG_DIR.");

    auto* settings = app.add_subcommand("settings", "Read the encrypted settings profile");
    settings->require_subcommand(1);
    auto* show = settings->add_subcommand("show", "Print the chat section and preset names");

    media::llm::agent::cliopt::AgentCliOptions opt;
    auto* agent = app.add_subcommand("agent",
        "Resolve llm-agent flags against the Tanit settings profile");
    add_agent_flags(agent, opt);

    auto finish_profile = [&](bool with_agent) {
        const fs::path config_dir = resolve_config_dir(config_flag);
        if (config_dir.empty())
            emit_err("config directory is empty");
        pm::commons::host::set_config_dir([config_dir] { return config_dir; });
        pm::commons::host::set_policy_registry_root(std::wstring(pm::brand::k_reg_policy_root_w));
        std::string source;
        std::string err;
        nlohmann::json root;
        try {
            root = load_profile(config_dir, source, err);
        } catch (const std::exception& ex) {
            emit_err(ex.what());
        }
        if (!err.empty() || !root.is_object())
            emit_err(err.empty() ? "settings profile is not a JSON object" : err);

        const nlohmann::json* preset = nullptr;
        if (with_agent) {
            preset = find_preset(root, opt.preset);
            if (!opt.preset.empty() && !preset)
                emit_err("chat preset not found: " + opt.preset);
        }
        nlohmann::json chat = chat_from(root, preset);
        if (with_agent) {
            overlay_string(chat, "router", opt.router);
            overlay_string(chat, "model", opt.model);
            overlay_string(chat, "base_url", opt.base_url);
            if (!opt.api_key.empty())
                chat["api_key"] = opt.api_key;
            if (opt.timeout_ms > 0)
                chat["timeout_ms"] = opt.timeout_ms;
            if (opt.max_iterations > 0)
                chat["max_iterations"] = opt.max_iterations;
        }
        redact(chat);
        nlohmann::json presets = nlohmann::json::array();
        if (const auto* list = preset_list(root)) {
            for (const auto& item : *list) {
                if (!item.is_object())
                    continue;
                presets.push_back({
                    {"id", item.value("id", std::string{})},
                    {"name", item.value("name", std::string{})},
                    {"runner", item.value("runner", std::string{"native"})},
                });
            }
        }
        nlohmann::json keys = nlohmann::json::array();
        nlohmann::json outline = nlohmann::json::object();
        for (auto it = root.begin(); it != root.end(); ++it) {
            keys.push_back(it.key());
            if (it.value().is_object()) {
                nlohmann::json children = nlohmann::json::array();
                for (auto child = it.value().begin(); child != it.value().end(); ++child)
                    children.push_back(child.key());
                outline[it.key()] = std::move(children);
            } else if (it.value().is_array()) {
                outline[it.key()] = it.value().size();
            } else {
                outline[it.key()] = it.value().type_name();
            }
        }
        nlohmann::json body = {
            {"ok", true},
            {"source", source},
            {"config_dir", config_dir.string()},
            {"keys", keys},
            {"outline", outline},
            {"chat", chat},
            {"presets", presets},
        };
        if (with_agent) {
            body["agent"] = {
                {"prompt", opt.prompt},
                {"preset", opt.preset},
                {"runner", opt.runner},
                {"no_tools", opt.no_tools},
                {"session_id", opt.session_id},
                {"include", opt.include_paths},
            };
            body["run"] = "resolve";
        }
        emit(body, 0);
    };

    show->callback([&] { finish_profile(false); });
    agent->callback([&] { finish_profile(true); });

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& err) {
        const int code = app.exit(err);
        return code == 0 ? 0 : 2;
    }
    return 2;
}

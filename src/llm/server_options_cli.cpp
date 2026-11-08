#include "llm/server_options_cli.hpp"

#include "fs/path_sanitizer.hpp"
#include "logging/logger.h"
#include "string/string_utils.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#if !defined(_WIN32)
#include <cstdlib>
#endif

namespace fs = std::filesystem;

namespace media::llm::agent::cliopt {
namespace {

std::string image_part_data_url(const nlohmann::json& part)
{
    if (!part.is_object())
        return {};
    const std::string type = part.value("type", "");
    if (type != "image_url" && type != "input_image")
        return {};
    if (!part.contains("image_url"))
        return {};
    const auto& image = part["image_url"];
    if (image.is_string())
        return image.get<std::string>();
    if (image.is_object() && image.contains("url") && image["url"].is_string())
        return image["url"].get<std::string>();
    return {};
}

bool message_has_image(const nlohmann::json& message)
{
    const auto it = message.find("content");
    if (it == message.end() || !it->is_array())
        return false;
    for (const auto& part : *it) {
        if (!image_part_data_url(part).empty())
            return true;
    }
    return false;
}

std::string message_content_to_text(const nlohmann::json& content)
{
    if (content.is_string())
        return content.get<std::string>();
    if (!content.is_array())
        return {};
    std::ostringstream oss;
    for (const auto& part : content) {
        if (!part.is_object())
            continue;
        const std::string type = part.value("type", "");
        if (type == "text" || type.empty()) {
            if (part.contains("text") && part["text"].is_string())
                oss << part["text"].get<std::string>();
        }
    }
    return oss.str();
}

std::string message_content_field(const nlohmann::json& m)
{
    const auto it = m.find("content");
    if (it == m.end() || it->is_null())
        return {};
    return message_content_to_text(*it);
}

int base64_digit(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+' || c == '-')
        return 62;
    if (c == '/' || c == '_')
        return 63;
    return -1;
}

std::vector<std::uint8_t> decode_base64(std::string_view in)
{
    std::vector<std::uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    int val = 0;
    int val_b = -8;
    for (unsigned char c : in) {
        if (c == '=' || std::isspace(c))
            continue;
        const int d = base64_digit(c);
        if (d < 0)
            return {};
        val = (val << 6) + d;
        val_b += 6;
        if (val_b >= 0) {
            out.push_back(static_cast<std::uint8_t>((val >> val_b) & 0xFF));
            val_b -= 8;
        }
    }
    return out;
}

const char* ext_for_image_mime(std::string_view mime)
{
    if (mime.find("png") != std::string_view::npos)
        return ".png";
    if (mime.find("webp") != std::string_view::npos)
        return ".webp";
    if (mime.find("gif") != std::string_view::npos)
        return ".gif";
    return ".jpg";
}

bool safe_mobile_relative_path(const std::string& raw, fs::path& out)
{
    std::string normalized = trim_ascii(raw);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.empty())
        return false;
    out = media::path::path_from_utf8(normalized).lexically_normal();
    if (out.empty() || out.is_absolute() || out.has_root_name() || out.has_root_directory())
        return false;
    for (const auto& part : out) {
        if (part == ".." || part == ".")
            return false;
    }
    return true;
}

void set_process_env(const char* name, const std::string& value)
{
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    if (value.empty())
        unsetenv(name);
    else
        setenv(name, value.c_str(), 1);
#endif
}

std::string get_process_env(const char* name)
{
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string{};
}

std::mutex& browser_env_mutex()
{
    static std::mutex mu;
    return mu;
}

bool ieq_prefix(std::string_view s, std::string_view pfx)
{
    if (s.size() < pfx.size())
        return false;
    for (size_t i = 0; i < pfx.size(); ++i) {
        const auto a = static_cast<unsigned char>(s[i]);
        const auto b = static_cast<unsigned char>(pfx[i]);
        if (std::tolower(a) != std::tolower(b))
            return false;
    }
    return true;
}

void take_header_token(std::string& dest, std::string_view raw)
{
    const std::string v = trim_ascii(std::string(raw));
    if (!v.empty())
        dest = v;
}

} // namespace

std::string validate_server_options(AgentServerOptions& o)
{
    o.host = trim_ascii(std::move(o.host));
    if (o.host.empty())
        o.host = "127.0.0.1";
    if (o.port <= 0 || o.port > 65535)
        return "invalid --port";
    if (o.concurrency < 1)
        o.concurrency = 1;
    if (!is_loopback_host(o.host) && o.serve_api_key.empty())
        return "refusing non-loopback bind (" + o.host
            + ") without --serve-api-key / TANIT_SERVE_API_KEY";
    if (o.lan_port != 0
        && (o.lan_port < 1 || o.lan_port > 65535 || o.lan_port == o.port))
        return "--lan-port must be 1-65535 and differ from --port";
    return {};
}

void overlay_server_chat(const nlohmann::json& chat, AgentServerOptions& o)
{
    if (!chat.is_object())
        return;
    auto take = [&](const char* key, std::string& dest) {
        if (!dest.empty() || !chat.contains(key) || !chat[key].is_string())
            return;
        dest = chat[key].get<std::string>();
    };
    take("router", o.router);
    take("model", o.model);
    take("base_url", o.base_url);
    take("api_key", o.api_key);
}

std::string trim_ascii(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

std::string openai_completion_id()
{
    using clock = std::chrono::system_clock;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        clock::now().time_since_epoch())
                        .count();
    return "chatcmpl-tanit-" + std::to_string(ms);
}

long openai_created_unix()
{
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

nlohmann::json openai_error(const std::string& message,
                            const std::string& type,
                            const std::string& code)
{
    return nlohmann::json{{"error",
                           {{"message", message},
                            {"type", type},
                            {"code", code},
                            {"param", nullptr}}}};
}

bool parse_chat_request(const nlohmann::json& body, ParsedChatRequest& out, std::string& err)
{
    if (!body.is_object()) {
        err = "body must be a JSON object";
        return false;
    }
    if (!body.contains("messages") || !body["messages"].is_array() || body["messages"].empty()) {
        err = "messages array is required";
        return false;
    }
    const auto& messages = body["messages"];

    std::size_t i = 0;
    for (; i < messages.size(); ++i) {
        const auto& m = messages[i];
        if (!m.is_object())
            break;
        if (m.value("role", "") != "system")
            break;
        out.system_prompt = message_content_field(m);
    }

    int last_user = -1;
    for (std::size_t j = i; j < messages.size(); ++j) {
        const auto& m = messages[j];
        if (!m.is_object())
            continue;
        if (m.value("role", "") == "user")
            last_user = static_cast<int>(j);
    }
    if (last_user < 0) {
        err = "no user message found";
        return false;
    }

    out.user_prompt = message_content_field(messages[static_cast<std::size_t>(last_user)]);
    if (out.user_prompt.empty() && !message_has_image(messages[static_cast<std::size_t>(last_user)])) {
        err = "last user message has empty content";
        return false;
    }

    out.seed_chat_messages = nlohmann::json::array();
    for (std::size_t j = i; j < static_cast<std::size_t>(last_user); ++j) {
        const auto& m = messages[j];
        if (!m.is_object())
            continue;
        const std::string role = m.value("role", "");
        if (role != "user" && role != "assistant")
            continue;
        const std::string content = message_content_field(m);
        if (content.empty())
            continue;
        out.seed_chat_messages.push_back({{"role", role}, {"content", content}});
    }
    return true;
}

bool looks_like_loopback_script(const std::string& model)
{
    return !model.empty() && (model.front() == '[' || model.front() == '@');
}

std::string resolve_body_cwd(const nlohmann::json& body,
                             std::string_view cwd_header,
                             const AgentServerOptions& opt)
{
    const std::string header = trim_ascii(std::string(cwd_header));
    if (!header.empty())
        return header;
    if (body.contains("cwd") && body["cwd"].is_string()) {
        const std::string c = trim_ascii(body["cwd"].get<std::string>());
        if (!c.empty())
            return c;
    }
    if (body.contains("tanit") && body["tanit"].is_object()
        && body["tanit"].contains("cwd") && body["tanit"]["cwd"].is_string()) {
        const std::string c = trim_ascii(body["tanit"]["cwd"].get<std::string>());
        if (!c.empty())
            return c;
    }
    return opt.cwd;
}

std::string resolve_body_session(const nlohmann::json& body, std::string_view session_header)
{
    const std::string header = trim_ascii(std::string(session_header));
    if (!header.empty())
        return header;
    if (body.contains("session_id") && body["session_id"].is_string()) {
        const std::string s = trim_ascii(body["session_id"].get<std::string>());
        if (!s.empty())
            return s;
    }
    if (body.contains("tanit") && body["tanit"].is_object()
        && body["tanit"].contains("session_id") && body["tanit"]["session_id"].is_string()) {
        const std::string s = trim_ascii(body["tanit"]["session_id"].get<std::string>());
        if (!s.empty())
            return s;
    }
    return {};
}

void apply_body_provider_overrides(const nlohmann::json& body,
                                   std::string_view provider_key_header,
                                   std::string_view provider_base_header,
                                   std::string& api_key,
                                   std::string& base_url)
{
    auto take_json_str = [&](std::string& dest, const nlohmann::json& o, const char* a,
                             const char* b = nullptr) {
        if (o.contains(a) && o[a].is_string())
            take_header_token(dest, o[a].get<std::string>());
        else if (b && o.contains(b) && o[b].is_string())
            take_header_token(dest, o[b].get<std::string>());
    };

    if (body.is_object()) {
        take_json_str(api_key, body, "apiKey", "api_key");
        take_json_str(base_url, body, "baseUrl", "base_url");
        if (body.contains("tanit") && body["tanit"].is_object()) {
            const auto& t = body["tanit"];
            take_json_str(api_key, t, "apiKey", "api_key");
            take_json_str(base_url, t, "baseUrl", "base_url");
        }
    }
    take_header_token(api_key, provider_key_header);
    take_header_token(base_url, provider_base_header);
}

std::vector<std::string> resolve_request_selection(const nlohmann::json& body)
{
    nlohmann::json arr = nlohmann::json::array();
    if (body.contains("tanit") && body["tanit"].is_object()
        && body["tanit"].contains("selection") && body["tanit"]["selection"].is_array()) {
        arr = body["tanit"]["selection"];
    } else if (body.contains("selection") && body["selection"].is_array()) {
        arr = body["selection"];
    }
    std::vector<std::string> out;
    for (const auto& v : arr) {
        if (!v.is_string())
            continue;
        const std::string s = trim_ascii(v.get<std::string>());
        if (!s.empty())
            out.push_back(s);
    }
    return out;
}

std::string stage_message_images(const nlohmann::json& body,
                                 const std::string& folder,
                                 std::vector<std::string>& paths)
{
    if (!body.contains("messages") || !body["messages"].is_array())
        return {};
    const nlohmann::json* last_user = nullptr;
    for (const auto& m : body["messages"]) {
        if (m.is_object() && m.value("role", "") == "user")
            last_user = &m;
    }
    if (!last_user)
        return {};
    const auto content = last_user->find("content");
    if (content == last_user->end() || !content->is_array())
        return {};

    fs::path dir = folder.empty() ? fs::temp_directory_path() / "tanit-lan-images"
                                  : media::path::path_from_utf8(folder) / "lan-images";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        return std::string("could not create image folder: ") + ec.message();

    constexpr std::size_t k_max_images = 8;
    constexpr std::size_t k_max_bytes = 12u * 1024u * 1024u;
    std::size_t n = 0;
    for (const auto& part : *content) {
        const std::string url = image_part_data_url(part);
        if (url.empty())
            continue;
        if (n >= k_max_images)
            return "too many attached images";
        const auto comma = url.find(',');
        if (url.rfind("data:", 0) != 0 || comma == std::string::npos)
            return "attached image must be a data URL";
        const std::string header = url.substr(0, comma);
        if (header.find("base64") == std::string::npos)
            return "attached image is not base64";
        const auto semi = header.find(';');
        const std::string mime =
            semi == std::string::npos ? header.substr(5) : header.substr(5, semi - 5);
        const auto bytes = decode_base64(std::string_view(url).substr(comma + 1));
        if (bytes.empty())
            return "attached image could not be decoded";
        if (bytes.size() > k_max_bytes)
            return "attached image is too large";
        static std::atomic<std::uint64_t> seq{0};
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        const fs::path file =
            dir / (std::string("lan-") + std::to_string(stamp) + "-"
                   + std::to_string(++seq) + ext_for_image_mime(mime));
        std::ofstream ofs(file, std::ios::binary);
        if (!ofs)
            return "could not write attached image";
        ofs.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!ofs.good())
            return "could not write attached image";
        paths.push_back(media::path::path_to_utf8(file));
        ++n;
    }
    if (n > 0)
        logger::info(std::string("llm-server staged ") + std::to_string(n)
                     + " image(s) into selection " + paths.back());
    return {};
}

std::string stage_mobile_attachments(const nlohmann::json& body,
                                     const std::string& folder,
                                     std::vector<std::string>& paths)
{
    if (!body.contains("tanit") || !body["tanit"].is_object())
        return {};
    const auto& tanit = body["tanit"];
    if (!tanit.contains("mobile_attachments") || !tanit["mobile_attachments"].is_array())
        return {};
    const auto& attachments = tanit["mobile_attachments"];
    if (attachments.size() > 64)
        return "too many mobile attachment files (64 max)";

    static std::atomic<std::uint64_t> seq{0};
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    fs::path root = folder.empty() ? fs::temp_directory_path() / "tanit-lan-files"
                                   : media::path::path_from_utf8(folder) / "lan-files";
    root /= std::string("request-") + std::to_string(stamp) + "-" + std::to_string(++seq);
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec)
        return std::string("could not create mobile attachment folder: ") + ec.message();

    constexpr std::size_t k_max_file_bytes = 12u * 1024u * 1024u;
    constexpr std::size_t k_max_total_bytes = 32u * 1024u * 1024u;
    std::size_t total_bytes = 0;
    for (const auto& item : attachments) {
        if (!item.is_object() || !item.contains("path") || !item["path"].is_string()
            || !item.contains("data") || !item["data"].is_string())
            return "invalid mobile attachment";
        fs::path relative;
        if (!safe_mobile_relative_path(item["path"].get<std::string>(), relative))
            return "invalid mobile attachment path";
        const std::string encoded = item["data"].get<std::string>();
        const auto bytes = decode_base64(encoded);
        if (!encoded.empty() && bytes.empty())
            return "mobile attachment could not be decoded";
        if (bytes.size() > k_max_file_bytes)
            return "mobile attachment is too large (12 MB max per file)";
        total_bytes += bytes.size();
        if (total_bytes > k_max_total_bytes)
            return "mobile attachments are too large (32 MB max)";
        const fs::path destination = root / relative;
        fs::create_directories(destination.parent_path(), ec);
        if (ec)
            return std::string("could not create mobile attachment path: ") + ec.message();
        std::ofstream ofs(destination, std::ios::binary);
        if (!ofs)
            return "could not write mobile attachment";
        if (!bytes.empty())
            ofs.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        if (!ofs.good())
            return "could not write mobile attachment";
    }

    if (tanit.contains("mobile_selection") && tanit["mobile_selection"].is_array()) {
        for (const auto& selected : tanit["mobile_selection"]) {
            if (!selected.is_object() || !selected.contains("path")
                || !selected["path"].is_string())
                return "invalid mobile selection";
            fs::path relative;
            if (!safe_mobile_relative_path(selected["path"].get<std::string>(), relative))
                return "invalid mobile selection path";
            const fs::path staged = root / relative;
            if (selected.value("is_folder", false)) {
                fs::create_directories(staged, ec);
                if (ec)
                    return std::string("could not create selected folder: ") + ec.message();
            }
            if (!fs::exists(staged, ec) || ec)
                return "mobile selection was not uploaded";
            paths.push_back(media::path::path_to_utf8(staged));
        }
    }
    logger::info(std::string("llm-server staged ") + std::to_string(attachments.size())
                 + " mobile file(s) into " + media::path::path_to_utf8(root));
    return {};
}

nlohmann::json usage_fields(const nlohmann::json& raw)
{
    nlohmann::json usage = nlohmann::json::object();
    if (!raw.is_object())
        return usage;
    for (const char* key : {"prompt_tokens", "completion_tokens", "total_tokens", "cost"}) {
        if (raw.contains(key) && raw[key].is_number())
            usage[key] = raw[key];
    }
    if (raw.contains("llm_rounds") && raw["llm_rounds"].is_array()) {
        nlohmann::json rounds = nlohmann::json::array();
        for (const auto& row : raw["llm_rounds"]) {
            if (!row.is_object())
                continue;
            nlohmann::json slim = nlohmann::json::object();
            if (row.contains("duration_ms") && row["duration_ms"].is_number())
                slim["duration_ms"] = row["duration_ms"];
            if (row.contains("cost") && row["cost"].is_number())
                slim["cost"] = row["cost"];
            else if (row.contains("usage") && row["usage"].is_object()
                     && row["usage"].contains("cost") && row["usage"]["cost"].is_number())
                slim["cost"] = row["usage"]["cost"];
            if (!slim.empty())
                rounds.push_back(std::move(slim));
        }
        if (!rounds.empty())
            usage["llm_rounds"] = std::move(rounds);
    }
    return usage;
}

nlohmann::json completion_success_json(const TurnOutcome& out)
{
    nlohmann::json usage = usage_fields(out.usage);
    if (out.exec_ms > 0)
        usage["wall_ms"] = out.exec_ms;
    return nlohmann::json{
        {"id", openai_completion_id()},
        {"object", "chat.completion"},
        {"created", openai_created_unix()},
        {"model", out.model},
        {"choices",
         nlohmann::json::array(
             {{{"index", 0},
               {"message", {{"role", "assistant"}, {"content", out.final_text}}},
               {"finish_reason", "stop"}}})},
        {"usage", std::move(usage)},
        {"tanit",
         {{"queue_wait_ms", out.queue_wait_ms},
          {"exec_ms", out.exec_ms},
          {"session_id", out.session_id}}},
    };
}

bool bearer_token_authorized(std::string_view authorization,
                             std::string_view api_key_a,
                             std::string_view api_key_b,
                             std::string_view api_key_c,
                             const std::string& expected_key)
{
    if (expected_key.empty())
        return true;
    const std::string auth = trim_ascii(std::string(authorization));
    if (ieq_prefix(auth, "Bearer ")) {
        const std::string token = trim_ascii(auth.substr(7));
        if (token == expected_key)
            return true;
    }
    for (std::string_view raw : {api_key_a, api_key_b, api_key_c}) {
        const std::string v = trim_ascii(std::string(raw));
        if (!v.empty() && v == expected_key)
            return true;
    }
    return false;
}

BrowserEnvGuard::BrowserEnvGuard(const nlohmann::json& body)
    : lock_(browser_env_mutex())
{
    prev_url_ = get_process_env("TANIT_CURRENT_URL");
    prev_title_ = get_process_env("TANIT_CURRENT_URL_TITLE");
    prev_last_ = get_process_env("TANIT_LAST_URL");
    prev_tabs_ = get_process_env("TANIT_BROWSER_TABS");

    const nlohmann::json* browser = nullptr;
    if (body.contains("tanit") && body["tanit"].is_object()) {
        const auto& tanit = body["tanit"];
        if (tanit.contains("browser") && tanit["browser"].is_object())
            browser = &tanit["browser"];
    }
    if (!browser) {
        set_process_env("TANIT_CURRENT_URL", {});
        set_process_env("TANIT_CURRENT_URL_TITLE", {});
        set_process_env("TANIT_LAST_URL", {});
        set_process_env("TANIT_BROWSER_TABS", {});
        lock_.unlock();
        return;
    }
    set_process_env("TANIT_CURRENT_URL", browser->value("currentUrl", std::string{}));
    set_process_env("TANIT_CURRENT_URL_TITLE", browser->value("currentTitle", std::string{}));
    set_process_env("TANIT_LAST_URL", browser->value("lastUrl", std::string{}));
    std::string packed;
    if (browser->contains("tabs") && (*browser)["tabs"].is_array()) {
        for (const auto& tab : (*browser)["tabs"]) {
            if (!tab.is_object())
                continue;
            const std::string url = tab.value("url", std::string{});
            if (url.empty())
                continue;
            if (!packed.empty())
                packed.push_back('\n');
            packed += url;
            packed.push_back('\t');
            packed += tab.value("title", std::string{});
        }
    }
    set_process_env("TANIT_BROWSER_TABS", packed);
}

BrowserEnvGuard::~BrowserEnvGuard()
{
    if (!lock_.owns_lock())
        lock_.lock();
    set_process_env("TANIT_CURRENT_URL", prev_url_);
    set_process_env("TANIT_CURRENT_URL_TITLE", prev_title_);
    set_process_env("TANIT_LAST_URL", prev_last_);
    set_process_env("TANIT_BROWSER_TABS", prev_tabs_);
}

void apply_provider_defaults(std::string& api_key,
                             std::string& base_url,
                             const std::string& server_key,
                             const std::string& server_base)
{
    if (api_key.empty()) {
        const bool foreign_base =
            !base_url.empty() && (server_base.empty() || base_url != server_base);
        if (!foreign_base)
            api_key = server_key;
    }
    if (base_url.empty())
        base_url = server_base;
}

VictoriaTarget victoria_target()
{
    VictoriaTarget target;
    const char* env = std::getenv("TANIT_LOGS_URL");
    if (!env || !env[0])
        return target;
    const std::string url = trim_ascii(env);
    std::string lower = url;
    lower = media::strutil::lower_ascii(std::move(lower));
    if (lower == "off" || lower == "0" || lower == "false" || lower == "none") {
        target.enabled = false;
        return target;
    }
    std::string rest = url;
    if (lower.rfind("http://", 0) == 0)
        rest = url.substr(7);
    else if (lower.rfind("https://", 0) == 0) {
        target.enabled = false;
        return target;
    }
    const auto slash = rest.find('/');
    if (slash != std::string::npos)
        rest.resize(slash);
    const auto colon = rest.rfind(':');
    if (colon != std::string::npos && colon + 1 < rest.size()) {
        target.host = rest.substr(0, colon);
        target.port = std::atoi(rest.c_str() + colon + 1);
    } else if (!rest.empty()) {
        target.host = rest;
    }
    if (target.host.empty() || target.port <= 0 || target.port > 65535) {
        target.host = "127.0.0.1";
        target.port = 9428;
    }
    return target;
}

std::string utc_now_iso()
{
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t secs = clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms.count()));
    return buf;
}

std::string clip_text(std::string s, std::size_t max)
{
    if (s.size() > max)
        s.resize(max);
    return s;
}

std::string log_level_name(std::string level)
{
    level = media::strutil::lower_ascii(std::move(level));
    if (level == "v" || level == "verbose" || level == "d" || level == "debug")
        return "debug";
    if (level == "w" || level == "warn" || level == "warning")
        return "warn";
    if (level == "e" || level == "error" || level == "f" || level == "fatal" || level == "a"
        || level == "assert")
        return "error";
    return "info";
}

bool parse_host_tool_provider_spec(const std::string& spec, std::string& id, std::string& url)
{
    const auto eq = spec.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 >= spec.size())
        return false;
    id = spec.substr(0, eq);
    url = spec.substr(eq + 1);
    while (!id.empty() && std::isspace(static_cast<unsigned char>(id.front())))
        id.erase(id.begin());
    while (!id.empty() && std::isspace(static_cast<unsigned char>(id.back())))
        id.pop_back();
    while (!url.empty() && std::isspace(static_cast<unsigned char>(url.front())))
        url.erase(url.begin());
    while (!url.empty() && std::isspace(static_cast<unsigned char>(url.back())))
        url.pop_back();
    return !id.empty() && url.rfind("http://", 0) == 0;
}

bool is_video_route(const std::string& path)
{
    const std::string prefix = "/v1/video";
    if (path == prefix)
        return true;
    return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0
           && path[prefix.size()] == '/';
}

} // namespace media::llm::agent::cliopt

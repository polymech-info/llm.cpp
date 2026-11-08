#include "mcp_stream_client.hpp"

#include "net/url_fetch.hpp"
#include "logging/logger.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

#if defined(MEDIA_ANDROID_NO_CURL) && (defined(__ANDROID__) || defined(ANDROID))

namespace media::llm::mcp {

StreamHttpClient::StreamHttpClient(std::string) {}
StreamHttpClient::StreamHttpClient(std::string, std::map<std::string, std::string>) {}
StreamHttpClient::~StreamHttpClient() = default;

bool StreamHttpClient::initialize(std::string& err)
{
    err = "MCP HTTP client unavailable (MEDIA_ANDROID_NO_CURL)";
    return false;
}

void StreamHttpClient::try_delete_session()
{}

void StreamHttpClient::set_request_options(int, std::function<bool()>)
{}

nlohmann::json StreamHttpClient::tools_list_all(std::string& err)
{
    err = "MCP HTTP client unavailable";
    return {};
}

nlohmann::json StreamHttpClient::resources_list_all(std::string& /*err*/)
{
    return nlohmann::json::array();
}

nlohmann::json StreamHttpClient::tools_call(const std::string&, const nlohmann::json&, std::string& err)
{
    err = "MCP HTTP client unavailable";
    return {};
}

} // namespace media::llm::mcp

#else

#include <curl/curl.h>
#include "string/string_utils.hpp"

namespace media::llm::mcp {
namespace {

void apply_android_ca(CURL* curl)
{
#if defined(__ANDROID__)
    const char* ca = std::getenv("CURL_CA_BUNDLE");
    if (!ca || !ca[0])
        ca = std::getenv("SSL_CERT_FILE");
    if (ca && ca[0])
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
#else
    (void)curl;
#endif
}

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t header_cb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

int progress_cancel_cb(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto* fn = static_cast<std::function<bool()>*>(userdata);
    if (!fn || !*fn)
        return 0;
    try {
        return (*fn)() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}


bool header_value_for(const std::string& blob, const char* name_lc, std::string& out)
{
    out.clear();
    std::istringstream in(blob);
    std::string        line;
    const std::string  needle = name_lc;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string key = line.substr(0, colon);
        key = media::strutil::lower_ascii(key);
        if (key != needle)
            continue;
        std::string val = line.substr(colon + 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
            val.erase(0, 1);
        out = std::move(val);
        return true;
    }
    return false;
}

bool content_type_is_sse(const std::string& ct)
{
    std::string x = ct;
    x = media::strutil::lower_ascii(x);
    return x.find("text/event-stream") != std::string::npos;
}

} // namespace

StreamHttpClient::StreamHttpClient(std::string mcp_endpoint_url)
    : mcp_endpoint_url_(std::move(mcp_endpoint_url))
{}

StreamHttpClient::StreamHttpClient(std::string mcp_endpoint_url, std::map<std::string, std::string> headers)
    : mcp_endpoint_url_(std::move(mcp_endpoint_url)), headers_(std::move(headers))
{}

StreamHttpClient::~StreamHttpClient()
{
    try_delete_session();
}

StreamHttpClient::HttpPostResult StreamHttpClient::http_post(const std::string& json_body, std::string& err)
{
    err.clear();
    HttpPostResult out;
    media::ensure_curl_global();

    CURL* curl = curl_easy_init();
    if (!curl) {
        err = "curl_easy_init failed";
        return out;
    }

    char curl_errbuf[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_errbuf);
    apply_android_ca(curl);

    std::string response_body;
    std::string response_headers;
    curl_easy_setopt(curl, CURLOPT_URL, mcp_endpoint_url_.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_body.size()));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    if (timeout_ms_ > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms_));
        const long connect_ms = timeout_ms_ < 15000 ? static_cast<long>(timeout_ms_) : 15000L;
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_ms);
    } else {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    }
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cancel_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancel_check_);

    struct curl_slist* hdr = nullptr;
    hdr = curl_slist_append(hdr, "Content-Type: application/json");
    hdr = curl_slist_append(hdr, "Accept: application/json, text/event-stream");
    for (const auto& [name, value] : headers_) {
        if (name.empty())
            continue;
        const std::string h = name + ": " + value;
        hdr                 = curl_slist_append(hdr, h.c_str());
    }
    if (!session_id_.empty()) {
        const std::string sh = "MCP-Session-Id: " + session_id_;
        hdr                    = curl_slist_append(hdr, sh.c_str());
    }
    if (!protocol_version_.empty()) {
        const std::string ph = "MCP-Protocol-Version: " + protocol_version_;
        hdr                    = curl_slist_append(hdr, ph.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);

    const CURLcode cr = curl_easy_perform(curl);
    curl_slist_free_all(hdr);
    if (cr != CURLE_OK) {
        err = std::string("curl: ") + curl_easy_strerror(cr);
        if (curl_errbuf[0])
            err += std::string(" (") + curl_errbuf + ")";
        curl_easy_cleanup(curl);
        return out;
    }
    long http_status = 0;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);

    out.http_status = http_status;
    out.body        = std::move(response_body);
    (void)header_value_for(response_headers, "content-type", out.content_type);
    (void)header_value_for(response_headers, "mcp-session-id", out.session_header_out);
    (void)header_value_for(response_headers, "mcp-protocol-version", out.protocol_header_out);
    return out;
}

nlohmann::json StreamHttpClient::parse_sse_data_json(const std::string& body, std::string& err)
{
    err.clear();
    std::istringstream stream(body);
    std::string        line;
    std::string        last_data;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("data:", 0) != 0)
            continue;
        std::string data = line.substr(5);
        const auto  p    = data.find_first_not_of(" \t");
        if (p != std::string::npos)
            data = data.substr(p);
        else
            data.clear();
        if (!data.empty())
            last_data = std::move(data);
    }
    if (last_data.empty()) {
        err = "SSE body contained no data: JSON";
        return {};
    }
    try {
        return nlohmann::json::parse(last_data);
    } catch (const std::exception& e) {
        err = std::string("SSE JSON parse: ") + e.what();
        return {};
    }
}

nlohmann::json StreamHttpClient::parse_jsonrpc_result_or_error(const nlohmann::json& envelope, std::string& err)
{
    err.clear();
    if (envelope.contains("error") && envelope["error"].is_object()) {
        const auto& e = envelope["error"];
        err           = e.value("message", std::string("jsonrpc error"));
        const int c   = e.value("code", 0);
        err += " (code " + std::to_string(c) + ")";
        return {};
    }
    if (!envelope.contains("result")) {
        err = "jsonrpc: missing result";
        return {};
    }
    return envelope["result"];
}

bool StreamHttpClient::initialize(std::string& err)
{
    std::lock_guard<std::mutex> lock(send_mu_);
    session_id_.clear();
    protocol_version_.clear();

    nlohmann::json params = nlohmann::json::object();
    params["protocolVersion"] = "2025-11-25";
    params["capabilities"]    = nlohmann::json::object();
    params["clientInfo"]        = nlohmann::json{{"name", "pm-image"}, {"version", "mcp-agent"}};

    const nlohmann::json req = {{"jsonrpc", "2.0"},
                                {"id", next_jsonrpc_id_++},
                                {"method", "initialize"},
                                {"params", std::move(params)}};

    HttpPostResult hp = http_post(req.dump(), err);
    if (!err.empty())
        return false;
    if (hp.http_status != 200) {
        err = "initialize HTTP " + std::to_string(hp.http_status) + " " + hp.body;
        return false;
    }

    nlohmann::json envelope;
    try {
        if (content_type_is_sse(hp.content_type))
            envelope = parse_sse_data_json(hp.body, err);
        else
            envelope = nlohmann::json::parse(hp.body);
    } catch (const std::exception& e) {
        err = std::string("initialize parse: ") + e.what();
        return false;
    }
    if (!err.empty())
        return false;

    if (envelope.contains("error")) {
        parse_jsonrpc_result_or_error(envelope, err);
        return false;
    }

    nlohmann::json result = parse_jsonrpc_result_or_error(envelope, err);
    if (!err.empty())
        return false;

    if (!hp.session_header_out.empty())
        session_id_ = hp.session_header_out;
    if (result.contains("protocolVersion") && result["protocolVersion"].is_string())
        protocol_version_ = result["protocolVersion"].get<std::string>();
    else if (!hp.protocol_header_out.empty())
        protocol_version_ = hp.protocol_header_out;
    else
        protocol_version_ = "2025-11-25";

    // notifications/initialized — expect 202 Accepted, empty body (ref/agent.cpp mcp_client.cpp)
    nlohmann::json notif = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}, {"params", nlohmann::json::object()}};
    HttpPostResult hp2   = http_post(notif.dump(), err);
    if (!err.empty())
        return false;
    if (hp2.http_status != 202 && hp2.http_status != 200) {
        err = "notifications/initialized HTTP " + std::to_string(hp2.http_status);
        return false;
    }
    return true;
}

void StreamHttpClient::set_request_options(int timeout_ms, std::function<bool()> cancel_check)
{
    std::lock_guard<std::mutex> lock(send_mu_);
    timeout_ms_ = timeout_ms > 0 ? timeout_ms : 60000;
    cancel_check_ = std::move(cancel_check);
}

void StreamHttpClient::try_delete_session()
{
    std::lock_guard<std::mutex> lock(send_mu_);
    if (session_id_.empty() || mcp_endpoint_url_.empty())
        return;

    media::ensure_curl_global();
    CURL* curl = curl_easy_init();
    if (!curl)
        return;
    apply_android_ca(curl);
    std::string         dummy_body;
    std::string         dummy_hdr;
    struct curl_slist* hdr = nullptr;
    hdr                    = curl_slist_append(hdr, "Accept: application/json, text/event-stream");
    for (const auto& [name, value] : headers_) {
        if (name.empty())
            continue;
        const std::string h = name + ": " + value;
        hdr                 = curl_slist_append(hdr, h.c_str());
    }
    const std::string sh = "MCP-Session-Id: " + session_id_;
    hdr                    = curl_slist_append(hdr, sh.c_str());
    if (!protocol_version_.empty()) {
        const std::string ph = "MCP-Protocol-Version: " + protocol_version_;
        hdr                    = curl_slist_append(hdr, ph.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_URL, mcp_endpoint_url_.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &dummy_body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &dummy_hdr);
    (void)curl_easy_perform(curl);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(curl);
    session_id_.clear();
}

nlohmann::json StreamHttpClient::tools_list_all(std::string& err)
{
    std::lock_guard<std::mutex> lock(send_mu_);
    nlohmann::json              all_tools = nlohmann::json::array();
    std::string                 cursor;
    for (;;) {
        nlohmann::json params = nlohmann::json::object();
        if (!cursor.empty())
            params["cursor"] = cursor;

        const nlohmann::json req = {{"jsonrpc", "2.0"},
                                    {"id", next_jsonrpc_id_++},
                                    {"method", "tools/list"},
                                    {"params", std::move(params)}};

        HttpPostResult hp = http_post(req.dump(), err);
        if (!err.empty())
            return {};
        if (hp.http_status != 200) {
            err = "tools/list HTTP " + std::to_string(hp.http_status) + " " + hp.body;
            return {};
        }

        nlohmann::json envelope;
        try {
            if (content_type_is_sse(hp.content_type))
                envelope = parse_sse_data_json(hp.body, err);
            else
                envelope = nlohmann::json::parse(hp.body);
        } catch (const std::exception& e) {
            err = std::string("tools/list parse: ") + e.what();
            return {};
        }
        if (!err.empty())
            return {};

        nlohmann::json result = parse_jsonrpc_result_or_error(envelope, err);
        if (!err.empty())
            return {};

        if (result.contains("tools") && result["tools"].is_array()) {
            for (const auto& t : result["tools"])
                all_tools.push_back(t);
        }
        if (result.contains("nextCursor") && result["nextCursor"].is_string()) {
            cursor = result["nextCursor"].get<std::string>();
            if (!cursor.empty())
                continue;
        }
        break;
    }
    return all_tools;
}

nlohmann::json StreamHttpClient::resources_list_all(std::string& err)
{
    std::lock_guard<std::mutex> lock(send_mu_);
    nlohmann::json all_resources = nlohmann::json::array();
    std::string    cursor;
    for (;;) {
        nlohmann::json params = nlohmann::json::object();
        if (!cursor.empty())
            params["cursor"] = cursor;

        const nlohmann::json req = {{"jsonrpc", "2.0"},
                                    {"id", next_jsonrpc_id_++},
                                    {"method", "resources/list"},
                                    {"params", std::move(params)}};

        HttpPostResult hp = http_post(req.dump(), err);
        if (!err.empty()) { err.clear(); return all_resources; }
        if (hp.http_status != 200) { return all_resources; }

        nlohmann::json envelope;
        try {
            if (content_type_is_sse(hp.content_type))
                envelope = parse_sse_data_json(hp.body, err);
            else
                envelope = nlohmann::json::parse(hp.body);
        } catch (...) { return all_resources; }
        if (!err.empty()) { err.clear(); return all_resources; }

        std::string rerr;
        nlohmann::json result = parse_jsonrpc_result_or_error(envelope, rerr);
        if (!rerr.empty()) return all_resources; // server doesn't support resources

        if (result.contains("resources") && result["resources"].is_array()) {
            for (const auto& r : result["resources"])
                all_resources.push_back(r);
        }
        if (result.contains("nextCursor") && result["nextCursor"].is_string()) {
            cursor = result["nextCursor"].get<std::string>();
            if (!cursor.empty()) continue;
        }
        break;
    }
    return all_resources;
}

nlohmann::json StreamHttpClient::tools_call(const std::string& remote_tool_name, const nlohmann::json& arguments,
                                            std::string& err)
{
    std::lock_guard<std::mutex> lock(send_mu_);
    nlohmann::json              params = {{"name", remote_tool_name}, {"arguments", arguments.is_object() ? arguments : nlohmann::json::object()}};
    const nlohmann::json        req    = {{"jsonrpc", "2.0"},
                                     {"id", next_jsonrpc_id_++},
                                     {"method", "tools/call"},
                                     {"params", std::move(params)}};

    HttpPostResult hp = http_post(req.dump(), err);
    if (!err.empty())
        return {};
    if (hp.http_status != 200) {
        err = "tools/call HTTP " + std::to_string(hp.http_status) + " " + hp.body;
        return {};
    }

    nlohmann::json envelope;
    try {
        if (content_type_is_sse(hp.content_type))
            envelope = parse_sse_data_json(hp.body, err);
        else
            envelope = nlohmann::json::parse(hp.body);
    } catch (const std::exception& e) {
        err = std::string("tools/call parse: ") + e.what();
        return {};
    }
    if (!err.empty())
        return {};

    nlohmann::json result = parse_jsonrpc_result_or_error(envelope, err);
    if (!err.empty())
        return {};
    return result;
}

} // namespace media::llm::mcp

#endif

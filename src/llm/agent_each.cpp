#include "agent_each.hpp"
#include "json/json_utils.hpp"

#include "agent_launch.hpp"
#include "agent_turn_options.hpp"
#include "fs/path_sanitizer.hpp"
#include "string/string_sanitizers.hpp"
#include "logging/logger.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#if defined(FEATURE_JQ) && FEATURE_JQ
#  include "process/async_iterator.hpp"
#  include "kbot.h"
#  include "llm_client.h"
#  include "llm/llama_agent_client.hpp"
#endif

namespace media::llm::agent::each {
namespace {

namespace fs = std::filesystem;

std::string json_string(const nlohmann::json& j, const char* a, const char* b = nullptr)
{
    if (j.contains(a) && j[a].is_string())
        return j[a].get<std::string>();
    if (b && j.contains(b) && j[b].is_string())
        return j[b].get<std::string>();
    return {};
}

bool json_bool(const nlohmann::json& j, const char* a, const char* b, bool fallback)
{
    auto take = [&](const char* k) -> bool {
        if (!j.contains(k) || j[k].is_null())
            return false;
        if (j[k].is_boolean()) {
            fallback = j[k].get<bool>();
            return true;
        }
        if (j[k].is_number_integer()) {
            fallback = j[k].get<int>() != 0;
            return true;
        }
        return false;
    };
    if (take(a))
        return fallback;
    if (b)
        (void)take(b);
    return fallback;
}

int json_int(const nlohmann::json& j, const char* a, const char* b, int fallback)
{
    auto take = [&](const char* k) -> bool {
        if (!j.contains(k) || !j[k].is_number_integer())
            return false;
        fallback = j[k].get<int>();
        return true;
    };
    if (take(a))
        return fallback;
    if (b)
        (void)take(b);
    return fallback;
}

void json_string_list(const nlohmann::json& j,
                      const char* a,
                      const char* b,
                      std::vector<std::string>& out)
{
    auto take = [&](const char* k) {
        if (!j.contains(k))
            return false;
        const auto& v = j[k];
        if (v.is_string()) {
            out.clear();
            const std::string raw = v.get<std::string>();
            std::string cur;
            for (char c : raw) {
                if (c == ',') {
                    while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.front())))
                        cur.erase(cur.begin());
                    while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.back())))
                        cur.pop_back();
                    if (!cur.empty())
                        out.push_back(std::move(cur));
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
            while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.front())))
                cur.erase(cur.begin());
            while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.back())))
                cur.pop_back();
            if (!cur.empty())
                out.push_back(std::move(cur));
            return true;
        }
        if (v.is_array()) {
            out.clear();
            for (const auto& it : v) {
                if (!it.is_string())
                    continue;
                std::string t = it.get<std::string>();
                while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front())))
                    t.erase(t.begin());
                while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back())))
                    t.pop_back();
                if (!t.empty())
                    out.push_back(std::move(t));
            }
            return true;
        }
        return false;
    };
    if (!take(a) && b)
        (void)take(b);
}

fs::path resolve_path(const std::string& path, const std::string& cwd)
{
    fs::path p = media::path::path_from_utf8(path);
    if (!cwd.empty() && p.is_relative())
        p = media::path::path_from_utf8(cwd) / p;
    return p;
}

bool load_json_file(const fs::path& path, nlohmann::json& out, std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "could not open: " + media::path::path_to_utf8(path);
        return false;
    }
    out = media::json::try_parse(in);
    if (out.is_discarded()) {
        err = "invalid JSON in: " + media::path::path_to_utf8(path);
        return false;
    }
    return true;
}

bool resolve_document(const EachOptions& opt, nlohmann::json& doc, std::string& err)
{
    if (!opt.document.is_null() && !opt.document.is_discarded()) {
        doc = opt.document;
        return true;
    }
    if (opt.source.empty()) {
        err = "document or source is required";
        return false;
    }
    return load_json_file(resolve_path(opt.source, opt.cwd), doc, err);
}

} // namespace

nlohmann::json EachResult::to_json() const
{
    return {
        {"ok", ok},
        {"function", "llm_agent_each"},
        {"error", error.empty() ? nlohmann::json(nullptr) : nlohmann::json(error)},
        {"source", source},
        {"selector", selector},
        {"target", target},
        {"mergeJson", merge_json},
        {"mergeKeys", merge_keys},
        {"dst", dst},
        {"dryRun", dry_run},
        {"router", router},
        {"model", model},
        {"concurrency", concurrency},
        {"matched", matched},
        {"transformed", transformed},
        {"skipped", skipped},
        {"errors", errors},
        {"cacheHits", cache_hits},
        {"aborted", aborted},
        {"abortedAfterErrors", aborted_after_errors},
        {"partialCommit", partial_commit},
        {"wroteDst", wrote_dst},
        {"timingsMs",
         {
             {"collect", collect_ms},
             {"transform", transform_ms},
             {"apply", apply_ms},
         }},
        {"output", document.is_null() ? nlohmann::json(nullptr) : document},
    };
}

bool parse_each_options(const nlohmann::json& j, EachOptions& out, std::string& err)
{
    err.clear();
    if (!j.is_object()) {
        err = "body must be a JSON object";
        return false;
    }
    if (j.contains("document") && !j["document"].is_null())
        out.document = j["document"];
    else if (j.contains("doc") && !j["doc"].is_null())
        out.document = j["doc"];

    out.source = json_string(j, "source", "sourcePath");
    if (out.source.empty())
        out.source = json_string(j, "input");
    out.selector = json_string(j, "selector", "path");
    out.target = json_string(j, "target", "as");
    out.dst = json_string(j, "dst", "output");
    if (out.dst.empty())
        out.dst = json_string(j, "outputPath");
    out.prompt = json_string(j, "prompt");
    out.system_prompt = json_string(j, "systemPrompt", "system_prompt");
    const std::string transform = json_string(j, "transform");
    if (!transform.empty())
        out.transform = transform;
    const std::string prefix = json_string(j, "prefix");
    if (!prefix.empty())
        out.prefix = prefix;
    out.concurrency = json_int(j, "concurrency", nullptr, out.concurrency);
    out.throttle_ms = json_int(j, "throttleMs", "throttle_ms", out.throttle_ms);
    out.max_retries = json_int(j, "maxRetries", "max_retries", out.max_retries);
    out.dry_run = json_bool(j, "dryRun", "dry_run", out.dry_run);
    out.no_cache = json_bool(j, "noCache", "no_cache", out.no_cache);
    out.merge_json = json_bool(j, "mergeJson", "merge_json", out.merge_json);
    json_string_list(j, "mergeKeys", "merge_keys", out.merge_keys);
    const std::string api_mode = json_string(j, "apiMode", "api_mode");
    if (!api_mode.empty())
        out.api_mode = api_mode;
    out.abort_consecutive =
        json_int(j, "abortAfterConsecutiveErrors", "abort_after_consecutive_errors",
                 out.abort_consecutive);
    out.partial_commit = json_bool(j, "partialCommit", "partial_commit", out.partial_commit);
    out.preset = json_string(j, "preset");
    out.router = json_string(j, "router");
    out.model = json_string(j, "model");
    out.api_key = json_string(j, "apiKey", "api_key");
    out.base_url = json_string(j, "baseUrl", "base_url");
    out.timeout_ms = json_int(j, "timeoutMs", "timeout_ms", out.timeout_ms);
    out.cwd = json_string(j, "cwd");
    return true;
}

bool validate_each_options(const EachOptions& opt, std::string& err)
{
    if (opt.selector.empty()) {
        err = "selector is required";
        return false;
    }
    if ((opt.document.is_null() || opt.document.is_discarded()) && opt.source.empty()) {
        err = "document or source is required";
        return false;
    }
    if (!opt.dry_run && opt.prompt.empty()) {
        err = "prompt is required unless dryRun";
        return false;
    }
    if (!opt.transform.empty()) {
        static const char* kOk[] = {"upper", "prefix", "identity", "llm-stub", "json-stub"};
        bool ok = false;
        for (const char* m : kOk) {
            if (opt.transform == m) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            err = "transform must be upper|prefix|identity|llm-stub|json-stub";
            return false;
        }
    }
    if (!opt.api_mode.empty()) {
        if (opt.api_mode != "completion" && opt.api_mode != "responses"
            && opt.api_mode != "realtime") {
            err = "apiMode must be completion|responses|realtime";
            return false;
        }
    }
    return true;
}

bool run_each(const EachOptions& opt,
              EachResult& out,
              ProgressCallback progress,
              ErrorCallback on_error)
{
    out = EachResult{};
    out.source = opt.source;
    out.selector = opt.selector;
    out.target = opt.target;
    out.dst = opt.dst;
    out.dry_run = opt.dry_run;
    out.merge_json = opt.merge_json;
    out.merge_keys = opt.merge_keys;
    out.partial_commit = opt.partial_commit;

    std::string err;
    if (!validate_each_options(opt, err)) {
        out.error = err;
        return false;
    }

#if !defined(FEATURE_JQ) || !FEATURE_JQ
    out.error = "llm agent each requires FEATURE_JQ in this build";
    return false;
#else
    namespace ai = media::async_iterator;
    using media::llm::agent::launch::AgentLaunchSpec;
    using media::llm::agent::launch::AgentLaunchResolved;
    using media::llm::agent::launch::resolve_agent_launch;

    if (opt.merge_json && !opt.target.empty()) {
        logger::warn("llm agent each: mergeJson ignores target "
                     "(object keys name the siblings; hard-overwrite merge)");
    }

    nlohmann::json doc;
    if (!resolve_document(opt, doc, err)) {
        out.error = err;
        return false;
    }

    const bool dry = opt.dry_run;
    const std::string& prompt = opt.prompt;

    ai::Options opts;
    opts.cache_identical_inputs = !opt.no_cache;
    opts.network.throttle_delay_ms = std::max(0, opt.throttle_ms);
    opts.network.concurrent_tasks =
        opt.concurrency > 0 ? opt.concurrency : (dry ? 1 : 4);
    opts.network.max_retries =
        opt.max_retries > 0 ? opt.max_retries
                            : (dry ? 1 : (opt.merge_json ? 5 : 3));
    opts.network.retry_delay_ms = dry ? 0 : 400;
    opts.network.abort_after_consecutive_errors = std::max(0, opt.abort_consecutive);
    out.concurrency = opts.network.concurrent_tasks;

    AgentLaunchResolved launch_resolved;
    std::string router_used;
    std::string model_used;

    if (dry) {
        const std::string& mode = opt.transform;
        if (mode == "prefix") {
            opts.transform = ai::prefix_transformer(opt.prefix);
        } else if (mode == "identity") {
            opts.transform = [](const std::string& v, const std::string&, std::string& e) {
                e.clear();
                return v;
            };
        } else if (mode == "llm-stub") {
            opts.transform = ai::make_prompt_transformer(
                prompt.empty() ? "transform" : prompt,
                [](const std::string& shaped, const std::string&, std::string& e) {
                    e.clear();
                    const std::string marker = "Text to transform: \"";
                    const auto pos = shaped.rfind(marker);
                    std::string text = shaped;
                    if (pos != std::string::npos) {
                        text = shaped.substr(pos + marker.size());
                        if (!text.empty() && text.back() == '"')
                            text.pop_back();
                    }
                    return std::string("[llm] ") + text;
                });
        } else if (mode == "json-stub") {
            opts.transform = [](const std::string& v, const std::string&, std::string& e) {
                e.clear();
                return nlohmann::json{
                    {"label_de", std::string("[de] ") + v},
                    {"label_fr", std::string("[fr] ") + v},
                }.dump();
            };
        } else {
            opts.transform = ai::uppercase_transformer();
        }
    } else {
        AgentLaunchSpec launch_spec;
        launch_spec.preset = opt.preset;
        launch_spec.router = opt.router;
        launch_spec.model = opt.model;
        launch_spec.api_key = opt.api_key;
        launch_spec.base_url = opt.base_url;
        launch_spec.timeout_ms = opt.timeout_ms;
        launch_spec.no_tools = true;
        launch_spec.api_mode =
            opt.api_mode.empty() ? std::string("completion") : opt.api_mode;
        launch_spec.streaming = "off";
        launch_spec.dry_run_credentials = false;

        std::string launch_err;
        if (!resolve_agent_launch(launch_spec, launch_resolved, launch_err)) {
            out.error = launch_err;
            return false;
        }
        const auto& prov = launch_resolved.provider;
        router_used = prov.router;
        model_used = prov.model;

        const bool local_llama = media::llm::agent::llama_client::is_llama_router(prov.router);
        if (!local_llama && prov.api_key.empty()) {
            out.error = "no API key configured for router '" + prov.router
                        + "' (set apiKey / settings / env)";
            return false;
        }

        auto kopts = std::make_shared<polymech::kbot::KBotOptions>();
        kopts->router = prov.router;
        kopts->model = prov.model;
        kopts->api_key = prov.api_key;
        kopts->base_url = prov.base_url;
        kopts->llm_timeout_ms = prov.timeout_ms > 0 ? prov.timeout_ms : 60'000;
        if (opt.merge_json)
            kopts->response_format_json = R"({"type":"json_object"})";
        auto client = std::make_shared<polymech::kbot::LLMClient>(*kopts);
        const int timeout_ms = kopts->llm_timeout_ms;
        const std::string model_for_llama = prov.model;

        const std::string prompt_utf8 = media::str_san::apply_text_sanitizer(
            prompt, media::str_san::TextSanitizer::RepairInvalidUtf8);

        std::string system_prompt;
        if (!opt.system_prompt.empty()) {
            media::llm::agent::Turn sys_turn;
            std::string sys_err;
            if (!media::llm::agent::options::apply_system_prompt_option(
                    opt.system_prompt, sys_turn, sys_err)) {
                out.error = sys_err;
                return false;
            }
            if (!sys_turn.system_prompt_is_null)
                system_prompt = std::move(sys_turn.system_prompt_override);
        }
        if (!system_prompt.empty())
            logger::info("llm agent each: system prompt chars="
                         + std::to_string(system_prompt.size()));

        opts.transform = ai::make_prompt_transformer(
            prompt_utf8,
            [client, local_llama, model_for_llama, timeout_ms, system_prompt](
                const std::string& shaped, const std::string& /*path*/,
                std::string& e) -> std::string {
                const std::string shaped_utf8 = media::str_san::apply_text_sanitizer(
                    shaped, media::str_san::TextSanitizer::RepairInvalidUtf8);
                std::vector<polymech::kbot::ChatMessage> messages;
                if (!system_prompt.empty()) {
                    polymech::kbot::ChatMessage sys;
                    sys.role = "system";
                    sys.content = system_prompt;
                    messages.push_back(std::move(sys));
                }
                polymech::kbot::ChatMessage user;
                user.role = "user";
                user.content = shaped_utf8;
                messages.push_back(std::move(user));
                logger::debug("llm agent each: leaf system_chars="
                              + std::to_string(system_prompt.size())
                              + " user_chars=" + std::to_string(shaped_utf8.size()));
                polymech::kbot::LLMResponse resp;
                if (local_llama) {
                    resp = media::llm::agent::llama_client::execute_chat_messages(
                        messages, model_for_llama, nlohmann::json{}, nlohmann::json{},
                        timeout_ms);
                } else {
                    resp = client->execute_chat_messages(messages);
                }
                if (!resp.success) {
                    e = resp.error.empty() ? "llm completion failed" : resp.error;
                    return {};
                }
                return resp.text;
            });
    }

    out.router = router_used;
    out.model = model_used;

    ai::FieldMapping mapping;
    mapping.path = opt.selector;
    mapping.merge_json = opt.merge_json;
    if (!mapping.merge_json && !opt.target.empty())
        mapping.target_path = opt.target;
    for (const auto& k : opt.merge_keys) {
        std::string t = k;
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front())))
            t.erase(t.begin());
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back())))
            t.pop_back();
        if (!t.empty())
            mapping.merge_keys.push_back(std::move(t));
    }
    out.merge_keys = mapping.merge_keys;
    mapping.options = {
        {"prompt", prompt},
        {"router", router_used},
        {"model", model_used},
        {"mergeJson", mapping.merge_json},
        {"mergeKeys", mapping.merge_keys},
    };

    if (on_error) {
        opts.error_callback = [on_error](const std::string& path, const std::string& value,
                                         const std::string& error) {
            on_error(path, value, error);
        };
    }
    if (progress) {
        opts.progress_callback = [progress](int done, int total, const std::string& path) {
            progress(done, total, path);
        };
    }

    ai::TransformStats stats;
    if (!ai::transform_mappings(doc, {mapping}, opts, &stats, &err)) {
        out.error = err.empty() ? "transform failed" : err;
        out.document = std::move(doc);
        out.matched = stats.matched;
        out.transformed = stats.transformed;
        out.skipped = stats.skipped;
        out.errors = stats.errors;
        out.cache_hits = stats.cache_hits;
        out.aborted = stats.aborted;
        out.aborted_after_errors = stats.aborted_after_errors;
        out.collect_ms = stats.collect_ms;
        out.transform_ms = stats.transform_ms;
        out.apply_ms = stats.apply_ms;
        return false;
    }

    out.matched = stats.matched;
    out.transformed = stats.transformed;
    out.skipped = stats.skipped;
    out.errors = stats.errors;
    out.cache_hits = stats.cache_hits;
    out.aborted = stats.aborted;
    out.aborted_after_errors = stats.aborted_after_errors;
    out.collect_ms = stats.collect_ms;
    out.transform_ms = stats.transform_ms;
    out.apply_ms = stats.apply_ms;
    out.document = doc;

    if (!opt.dst.empty()) {
        const bool partial_commit = opt.partial_commit && stats.errors > 0;
        out.partial_commit = partial_commit;
        if (stats.errors > 0 && !partial_commit) {
            out.error = "not writing dst (" + std::to_string(stats.errors)
                        + " leaf error(s); no partial commit)";
            out.ok = false;
            return false;
        }
        const fs::path out_path = resolve_path(opt.dst, opt.cwd);
        std::ofstream ofs(out_path, std::ios::binary);
        if (!ofs) {
            out.error = "could not write dst: " + media::path::path_to_utf8(out_path);
            out.ok = false;
            return false;
        }
        ofs << doc.dump(2) << "\n";
        out.wrote_dst = true;
    }

    out.ok = stats.errors == 0;
    if (!out.ok && out.error.empty())
        out.error = std::to_string(stats.errors) + " leaf error(s)";
    return out.ok;
#endif
}

} // namespace media::llm::agent::each

#include "agent_decide.hpp"
#include "json/json_utils.hpp"

#include "fs/path_sanitizer.hpp"
#include "core/settings_runtime.hpp"

#include <fstream>
#include <filesystem>

#if defined(FEATURE_JQ) && FEATURE_JQ
#  include "process/async_iterator.hpp"
#endif

namespace media::llm::agent::decide {
namespace {

namespace fs = std::filesystem;
namespace dec = media::llm::decisions;

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

bool decide_pair_from_node(const nlohmann::json& node,
                           const std::string& left_key,
                           const std::string& right_key,
                           nlohmann::json& left,
                           nlohmann::json& right,
                           std::string& id,
                           std::string& err)
{
    if (node.is_array()) {
        if (node.size() != 2) {
            err = "expected a 2-element array";
            return false;
        }
        left = node[0];
        right = node[1];
        id.clear();
        return true;
    }
    if (!node.is_object()) {
        err = "expected an object with '" + left_key + "' and '" + right_key
              + "', or a 2-element array";
        return false;
    }
    if (!node.contains(left_key) || !node.contains(right_key)) {
        err = "object is missing '" + left_key + "' or '" + right_key + "'";
        return false;
    }
    left = node[left_key];
    right = node[right_key];
    if (node.contains("id") && node["id"].is_string())
        id = node["id"].get<std::string>();
    else
        id.clear();
    return true;
}

nlohmann::json decide_answer_object(
    const std::vector<std::pair<std::string, nlohmann::json>>& answers)
{
    nlohmann::json obj = nlohmann::json::object();
    for (const auto& ans : answers)
        obj[ans.first] = ans.second;
    return obj;
}

} // namespace

nlohmann::json DecideResult::to_json() const
{
    return {
        {"ok", ok},
        {"function", "llm_agent_decide"},
        {"error", error.empty() ? nlohmann::json(nullptr) : nlohmann::json(error)},
        {"source", source},
        {"questionsFile", questions_file},
        {"provider", provider_id},
        {"model", model},
        {"resolvedModel", resolved_model},
        {"dryRun", dry_run},
        {"batched", batched},
        {"pairs", pairs},
        {"items", items},
        {"questions", questions},
        {"expandedQuestions", expanded_questions},
        {"requests", requests},
        {"chunk", chunk},
        {"maxTokens", max_tokens},
        {"target", target},
        {"dst", dst},
        {"wroteDst", wrote_dst},
        {"usage",
         {
             {"inputTokens", usage.input_tokens},
             {"outputTokens", usage.output_tokens},
             {"cost", usage.cost},
         }},
        {"output", document.is_null() ? nlohmann::json(nullptr) : document},
    };
}

bool parse_decide_options(const nlohmann::json& j, DecideOptions& out, std::string& err)
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

    if (j.contains("questions") && j["questions"].is_object())
        out.questions = j["questions"];
    out.questions_file = json_string(j, "questionsFile", "questions_file");
    if (out.questions_file.empty() && j.contains("questions") && j["questions"].is_string())
        out.questions_file = j["questions"].get<std::string>();

    out.selector = json_string(j, "selector");
    out.pairs = json_string(j, "pairs");
    out.left = json_string(j, "left");
    out.right = json_string(j, "right");
    const std::string left_key = json_string(j, "leftKey", "left_key");
    if (!left_key.empty())
        out.left_key = left_key;
    const std::string right_key = json_string(j, "rightKey", "right_key");
    if (!right_key.empty())
        out.right_key = right_key;
    const std::string target = json_string(j, "target");
    if (!target.empty())
        out.target = target;
    out.dst = json_string(j, "dst", "output");
    if (out.dst.empty())
        out.dst = json_string(j, "outputPath");
    const std::string provider = json_string(j, "provider");
    if (!provider.empty())
        out.provider = provider;
    out.path = json_string(j, "path");
    out.model = json_string(j, "model");
    out.api_key = json_string(j, "apiKey", "api_key");
    out.base_url = json_string(j, "baseUrl", "base_url");
    out.chunk = json_int(j, "chunk", nullptr, out.chunk);
    out.max_tokens = json_int(j, "maxTokens", "max_tokens", out.max_tokens);
    out.timeout_ms = json_int(j, "timeoutMs", "timeout_ms", out.timeout_ms);
    out.dry_run = json_bool(j, "dryRun", "dry_run", out.dry_run);
    out.cwd = json_string(j, "cwd");
    return true;
}

bool validate_decide_options(const DecideOptions& opt, std::string& err)
{
    if ((opt.document.is_null() || opt.document.is_discarded()) && opt.source.empty()) {
        err = "document or source is required";
        return false;
    }
    if ((opt.questions.is_null() || opt.questions.is_discarded() || !opt.questions.is_object())
        && opt.questions_file.empty()) {
        err = "questions object or questionsFile is required";
        return false;
    }
    if (opt.target.empty()) {
        err = "target must be a non-empty key";
        return false;
    }
    const bool have_selector = !opt.selector.empty();
    const bool have_pairs = !opt.pairs.empty();
    const bool have_left = !opt.left.empty();
    const bool have_right = !opt.right.empty();
    if (have_selector && (have_pairs || have_left || have_right)) {
        err = "selector cannot be combined with pairs or left/right";
        return false;
    }
    if (have_pairs && (have_left || have_right)) {
        err = "pairs cannot be combined with left/right";
        return false;
    }
    if (have_left != have_right) {
        err = "left and right are a pair";
        return false;
    }
    if (!opt.provider.empty()) {
        static const char* kOk[] = {"openrouter", "systemone", "jev", "openai", "stub"};
        bool ok = false;
        for (const char* p : kOk) {
            if (opt.provider == p) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            err = "provider must be openrouter|systemone|jev|openai|stub";
            return false;
        }
    }
    if (opt.chunk < 1 || opt.chunk > 4096) {
        err = "chunk must be in 1..4096";
        return false;
    }
    if (opt.max_tokens < 0 || opt.max_tokens > 32000) {
        err = "maxTokens must be in 0..32000";
        return false;
    }
    return true;
}

bool run_decide(const DecideOptions& opt, DecideResult& out)
{
    out = DecideResult{};
    out.source = opt.source;
    out.questions_file = opt.questions_file;
    out.dry_run = opt.dry_run;
    out.chunk = opt.chunk;
    out.max_tokens = opt.max_tokens;
    out.target = opt.target;
    out.dst = opt.dst;

    std::string err;
    if (!validate_decide_options(opt, err)) {
        out.error = err;
        return false;
    }

#if !defined(FEATURE_JQ) || !FEATURE_JQ
    out.error = "llm agent decide requires FEATURE_JQ in this build";
    return false;
#else
    namespace ai = media::async_iterator;

    nlohmann::json doc;
    if (!opt.document.is_null() && !opt.document.is_discarded()) {
        doc = opt.document;
    } else if (!load_json_file(resolve_path(opt.source, opt.cwd), doc, err)) {
        out.error = err;
        return false;
    }

    nlohmann::json qdoc;
    if (opt.questions.is_object()) {
        qdoc = opt.questions;
    } else if (!load_json_file(resolve_path(opt.questions_file, opt.cwd), qdoc, err)) {
        out.error = err;
        return false;
    }

    std::vector<dec::Question> questions;
    if (!dec::parse_questions_json(qdoc, questions, err)) {
        out.error = err;
        return false;
    }

    struct Slot {
        nlohmann::json path_keys;
        bool write_on_node = true;
        dec::Pair pair;
        dec::Item item;
    };
    std::vector<Slot> slots;
    const bool have_selector = !opt.selector.empty();
    const bool have_pairs = !opt.pairs.empty();
    const bool have_left = !opt.left.empty();
    const bool pair_mode = have_pairs || have_left;
    const bool item_mode = have_selector;

    if (have_selector) {
        std::vector<ai::JsonNode> nodes;
        if (!ai::collect_nodes(doc, opt.selector, nodes, &err)) {
            out.error = err.empty() ? "selector failed" : err;
            return false;
        }
        if (nodes.empty()) {
            out.error = "selector matched nothing";
            return false;
        }
        slots.reserve(nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            Slot slot;
            slot.path_keys = nodes[i].path_keys;
            slot.write_on_node = nodes[i].value.is_object();
            std::string id;
            if (slot.write_on_node && nodes[i].value.contains("id")
                && nodes[i].value["id"].is_string())
                id = nodes[i].value["id"].get<std::string>();
            slot.item.id = !id.empty() ? id : ("i" + std::to_string(i));
            slot.item.value = nodes[i].value;
            slots.push_back(std::move(slot));
        }
    } else if (have_pairs) {
        std::vector<ai::JsonNode> nodes;
        if (!ai::collect_nodes(doc, opt.pairs, nodes, &err)) {
            out.error = err.empty() ? "selector failed" : err;
            return false;
        }
        if (nodes.empty()) {
            out.error = "pairs matched nothing";
            return false;
        }
        slots.reserve(nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            Slot slot;
            slot.path_keys = nodes[i].path_keys;
            slot.write_on_node = true;
            std::string id;
            if (!decide_pair_from_node(nodes[i].value, opt.left_key, opt.right_key,
                                       slot.pair.left, slot.pair.right, id, err)) {
                out.error = nodes[i].pointer + ": " + err;
                return false;
            }
            slot.pair.id = !id.empty() ? id : ("p" + std::to_string(i));
            slots.push_back(std::move(slot));
        }
    } else if (have_left) {
        std::vector<ai::JsonNode> lefts;
        std::vector<ai::JsonNode> rights;
        if (!ai::collect_nodes(doc, opt.left, lefts, &err)) {
            out.error = std::string("left: ") + (err.empty() ? "selector failed" : err);
            return false;
        }
        if (!ai::collect_nodes(doc, opt.right, rights, &err)) {
            out.error = std::string("right: ") + (err.empty() ? "selector failed" : err);
            return false;
        }
        if (lefts.empty() || lefts.size() != rights.size()) {
            out.error = "left matched " + std::to_string(lefts.size()) + " and right matched "
                        + std::to_string(rights.size());
            return false;
        }
        slots.reserve(lefts.size());
        for (std::size_t i = 0; i < lefts.size(); ++i) {
            Slot slot;
            slot.path_keys = lefts[i].path_keys;
            slot.write_on_node = false;
            slot.pair.id = "p" + std::to_string(i);
            slot.pair.left = lefts[i].value;
            slot.pair.right = rights[i].value;
            slots.push_back(std::move(slot));
        }
    }

    const bool dry = opt.dry_run;
    dec::ProviderConfig cfg;
    cfg.kind = dry ? "stub" : opt.provider;
    cfg.model = opt.model;
    cfg.base_url = opt.base_url;
    cfg.path = opt.path;
    cfg.timeout_ms = opt.timeout_ms > 0 ? opt.timeout_ms : 60000;
    cfg.api_key = opt.api_key;
    if (!dry && (cfg.kind == "openrouter" || cfg.kind == "systemone") && cfg.api_key.empty()) {
        media::runtime_settings::merge_provider_credentials(
            "openrouter", false, cfg.api_key, cfg.base_url);
    }
    if (!dry && cfg.model.empty() && cfg.kind != "stub")
        cfg.model = "~typesafe/jev-latest";

    auto provider = dec::make_provider(cfg, err);
    if (!provider) {
        out.error = err;
        return false;
    }
    out.provider_id = provider->id();
    out.model = cfg.model;

    int requests = 0;
    int expanded = 0;
    int pair_count = 0;
    int item_count = 0;
    dec::Usage usage;
    std::string resolved_model;
    bool batched = false;

    if (pair_mode || item_mode) {
        dec::PairBatchOptions batch_opt;
        batch_opt.max_questions_per_request = opt.chunk > 0 ? opt.chunk : 32;
        batch_opt.max_request_tokens = opt.max_tokens;
        batch_opt.model = dry ? std::string{} : cfg.model;
        std::vector<dec::PairAnswers> answers;
        dec::BatchStats stats;
        if (pair_mode) {
            std::vector<dec::Pair> pairs;
            pairs.reserve(slots.size());
            for (const auto& slot : slots)
                pairs.push_back(slot.pair);
            if (!dec::decide_pairs(*provider, pairs, questions, batch_opt, answers, stats, err)) {
                out.error = err;
                return false;
            }
        } else {
            std::vector<dec::Item> items;
            items.reserve(slots.size());
            for (const auto& slot : slots)
                items.push_back(slot.item);
            if (!dec::decide_items(*provider, items, questions, batch_opt, answers, stats, err)) {
                out.error = err;
                return false;
            }
        }
        if (answers.size() != slots.size()) {
            out.error = "answer count does not match selected values";
            return false;
        }
        for (std::size_t i = 0; i < slots.size(); ++i) {
            const nlohmann::json body = decide_answer_object(answers[i].answers);
            const bool wrote = slots[i].write_on_node
                ? ai::assign_node_key(doc, slots[i].path_keys, opt.target, body, &err)
                : ai::assign_parent_key(doc, slots[i].path_keys, opt.target, body, &err);
            if (!wrote) {
                out.error = err.empty() ? "could not write answers" : err;
                return false;
            }
        }
        requests = stats.requests;
        expanded = stats.expanded_questions;
        pair_count = stats.pairs;
        item_count = stats.items;
        usage = stats.usage;
        resolved_model = stats.resolved_model;
        batched = true;
    } else {
        if (!doc.is_object()) {
            out.error =
                "document mode writes target onto a JSON object "
                "(pass selector, pairs, or left/right to judge selected values)";
            return false;
        }
        if (opt.max_tokens > 0) {
            const int estimated = dec::estimate_request_tokens(doc, questions);
            if (estimated > opt.max_tokens) {
                out.error = "document is about " + std::to_string(estimated)
                            + " tokens, over the " + std::to_string(opt.max_tokens)
                            + " request limit. Pass --selector to split values.";
                return false;
            }
        }
        dec::Request req;
        req.model = dry ? std::string{} : cfg.model;
        req.state = doc;
        req.questions = questions;
        dec::Response resp;
        if (!provider->decide(req, resp, err)) {
            out.error = err;
            return false;
        }
        nlohmann::json body = nlohmann::json::object();
        for (const auto& q : questions) {
            const auto it = resp.answers.find(q.id);
            if (it == resp.answers.end()) {
                out.error = "missing answer for '" + q.id + "'";
                return false;
            }
            body[q.id] = it->second;
        }
        doc[opt.target] = std::move(body);
        requests = 1;
        expanded = static_cast<int>(questions.size());
        usage = resp.usage;
        resolved_model = resp.model;
    }

    out.batched = batched;
    out.pairs = pair_count;
    out.items = item_count;
    out.questions = static_cast<int>(questions.size());
    out.expanded_questions = expanded;
    out.requests = requests;
    out.usage = usage;
    out.resolved_model = resolved_model;
    out.document = doc;

    if (!opt.dst.empty()) {
        const fs::path out_path = resolve_path(opt.dst, opt.cwd);
        std::ofstream ofs(out_path, std::ios::binary);
        if (!ofs) {
            out.error = "could not write dst: " + media::path::path_to_utf8(out_path);
            return false;
        }
        ofs << doc.dump(2) << "\n";
        out.wrote_dst = true;
    }

    out.ok = true;
    return true;
#endif
}

} // namespace media::llm::agent::decide

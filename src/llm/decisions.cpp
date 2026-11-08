#include "decisions.hpp"
#include "json/json_utils.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace media::llm::decisions {
namespace {

std::string trim_copy(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

std::string strip_trailing_slashes(std::string s)
{
    while (!s.empty() && (s.back() == '/' || s.back() == '\\'))
        s.pop_back();
    return s;
}

/// Keep scheme://host[:port]. An OpenAI-style base such as
/// https://openrouter.ai/api/v1 is reduced to the origin so the decisions
/// path is not appended twice.
std::string origin_of(std::string base)
{
    base = strip_trailing_slashes(std::move(base));
    const auto scheme = base.find("://");
    if (scheme == std::string::npos)
        return base;
    const auto path = base.find('/', scheme + 3);
    if (path != std::string::npos)
        base.resize(path);
    return base;
}

bool valid_question_id(const std::string& id)
{
    if (id.empty() || id.find('/') != std::string::npos)
        return false;
    for (unsigned char c : id) {
        if (std::isalnum(c) || c == '_' || c == '-' || c == '.')
            continue;
        return false;
    }
    return true;
}

int json_int(const nlohmann::json& v)
{
    if (v.is_number_integer())
        return v.get<int>();
    if (v.is_number())
        return static_cast<int>(v.get<double>());
    return 0;
}

std::string rewrite_token_string(const std::string& in,
                                 const std::string& token,
                                 const std::string& collection,
                                 int index)
{
    const std::string slot = collection + "[" + std::to_string(index) + "]";
    std::string out;
    out.reserve(in.size() + 8);
    for (std::size_t i = 0; i < in.size();) {
        const bool marked = i > 0 && in[i - 1] == '`';
        if (marked && in.compare(i, token.size(), token) == 0) {
            const std::size_t n = token.size();
            const char next = (i + n < in.size()) ? in[i + n] : '\0';
            if (next == '`' || next == '.') {
                out += slot;
                i += n;
                continue;
            }
        }
        out.push_back(in[i]);
        ++i;
    }
    return out;
}

nlohmann::json rewrite_token_refs(const nlohmann::json& node,
                                  const std::string& token,
                                  const std::string& collection,
                                  int index)
{
    if (node.is_string())
        return rewrite_token_string(node.get_ref<const std::string&>(), token, collection, index);
    if (node.is_array()) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& el : node)
            arr.push_back(rewrite_token_refs(el, token, collection, index));
        return arr;
    }
    if (node.is_object()) {
        nlohmann::json obj = nlohmann::json::object();
        for (auto it = node.begin(); it != node.end(); ++it)
            obj[it.key()] = rewrite_token_refs(it.value(), token, collection, index);
        return obj;
    }
    return node;
}

int slot_index_in(const nlohmann::json& instructions)
{
    const std::string blob = instructions.is_string()
        ? instructions.get<std::string>()
        : instructions.dump();
    auto pos = blob.find("pairs[");
    if (pos == std::string::npos)
        pos = blob.find("items[");
    if (pos == std::string::npos)
        return -1;
    std::size_t i = pos + 6;
    if (i >= blob.size() || !std::isdigit(static_cast<unsigned char>(blob[i])))
        return -1;
    int value = 0;
    while (i < blob.size() && std::isdigit(static_cast<unsigned char>(blob[i]))) {
        value = value * 10 + (blob[i] - '0');
        ++i;
    }
    return value;
}

const nlohmann::json* batch_element(const nlohmann::json& state, int index)
{
    if (index < 0 || !state.is_object())
        return nullptr;
    const char* keys[] = {"pairs", "items"};
    for (const char* key : keys) {
        if (!state.contains(key) || !state[key].is_array())
            continue;
        const auto& arr = state[key];
        if (index < static_cast<int>(arr.size()))
            return &arr[static_cast<std::size_t>(index)];
    }
    return nullptr;
}

std::vector<std::string> criteria_keys(const nlohmann::json& criteria)
{
    std::vector<std::string> keys;
    if (!criteria.is_object())
        return keys;
    keys.reserve(criteria.size());
    for (auto it = criteria.begin(); it != criteria.end(); ++it)
        keys.push_back(it.key());
    return keys;
}

std::string pick_choice(const nlohmann::json& criteria,
                        bool equal,
                        bool have_sides,
                        const nlohmann::json* element)
{
    const std::vector<std::string> keys = criteria_keys(criteria);
    if (keys.empty())
        return {};
    if (have_sides) {
        const char* prefer = equal ? "same" : "different";
        for (const auto& k : keys) {
            if (k == prefer)
                return k;
        }
        if (!equal && keys.size() >= 2)
            return keys[1];
        return keys[0];
    }
    auto match_string = [&](const std::string& s) -> std::string {
        for (const auto& k : keys) {
            if (k == s)
                return k;
        }
        return {};
    };
    if (element && element->is_string()) {
        const std::string hit = match_string(element->get<std::string>());
        if (!hit.empty())
            return hit;
    }
    if (element && element->is_object()) {
        for (auto it = element->begin(); it != element->end(); ++it) {
            if (!it.value().is_string())
                continue;
            const std::string hit = match_string(it.value().get<std::string>());
            if (!hit.empty())
                return hit;
        }
    }
    return keys[0];
}

nlohmann::json stub_answer(const Question& q, const nlohmann::json& state)
{
    const int index = slot_index_in(q.instructions);
    const nlohmann::json* element = batch_element(state, index);
    const bool have_slot = element != nullptr;
    const bool have_sides = have_slot && element->is_object()
        && element->contains("left") && element->contains("right");
    const bool equal = have_sides && (*element)["left"] == (*element)["right"];
    const double confidence = have_slot ? 1.0 : 0.5;

    if (q.type == "noul") {
        double noul = 0.5;
        if (have_sides)
            noul = equal ? 1.0 : 0.0;
        else if (have_slot && element->is_string())
            noul = element->get<std::string>().empty() ? 0.0 : 1.0;
        else if (have_slot && !element->is_null())
            noul = 1.0;
        else if (have_slot)
            noul = 0.0;
        return nlohmann::json{{"type", "noul"}, {"noul", noul}};
    }
    if (q.type == "score") {
        const int n = q.criteria.is_array() ? static_cast<int>(q.criteria.size()) : 0;
        const double score = (have_sides && !equal && n > 0) ? static_cast<double>(n - 1) : 0.0;
        nlohmann::json probs = nlohmann::json::object();
        nlohmann::json legend = nlohmann::json::object();
        for (int i = 0; i < n; ++i) {
            const std::string key = std::to_string(i);
            const bool on = static_cast<int>(score) == i;
            probs[key] = on ? 1.0 : 0.0;
            if (q.criteria[i].is_string() || q.criteria[i].is_object() || q.criteria[i].is_array())
                legend[key] = q.criteria[i];
        }
        return nlohmann::json{
            {"type", "score"},
            {"score", score},
            {"probabilities", std::move(probs)},
            {"legend", std::move(legend)},
            {"confidence", confidence},
        };
    }
    const std::string choice = pick_choice(q.criteria, equal, have_sides, element);
    nlohmann::json probs = nlohmann::json::object();
    if (q.criteria.is_object()) {
        for (auto it = q.criteria.begin(); it != q.criteria.end(); ++it)
            probs[it.key()] = it.key() == choice ? 1.0 : 0.0;
    }
    return nlohmann::json{
        {"type", "choice"},
        {"choice", choice},
        {"probabilities", std::move(probs)},
        {"confidence", confidence},
    };
}

nlohmann::json question_wire(const Question& q)
{
    nlohmann::json body = {
        {"type", q.type},
        {"instructions", q.instructions},
    };
    if (!q.criteria.is_null())
        body["criteria"] = q.criteria;
    return body;
}

bool parse_local_key(const std::string& key, int& local, std::string& qid)
{
    const auto slash = key.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= key.size())
        return false;
    try {
        std::size_t used = 0;
        const int value = std::stoi(key.substr(0, slash), &used);
        if (used != slash || value < 0)
            return false;
        local = value;
        qid = key.substr(slash + 1);
        return !qid.empty();
    } catch (...) {
        return false;
    }
}

class StubProvider final : public Provider {
public:
    const char* id() const override { return "stub"; }

    bool decide(const Request& req, Response& out, std::string& err) override
    {
        err.clear();
        if (req.questions.empty()) {
            err = "decisions: no questions";
            return false;
        }
        out = Response{};
        out.model = "stub";
        out.provider = "stub";
        for (const auto& q : req.questions) {
            if (out.answers.count(q.id)) {
                err = "decisions: duplicate question id '" + q.id + "'";
                return false;
            }
            out.answers.emplace(q.id, stub_answer(q, req.state));
        }
        return true;
    }
};

size_t curl_write(char* ptr, size_t size, size_t nmemb, void* ud)
{
    auto* s = static_cast<std::string*>(ud);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

class HttpProvider final : public Provider {
public:
    HttpProvider(std::string kind, std::string url, std::string api_key, int timeout_ms, bool openrouter_headers)
        : kind_(std::move(kind))
        , url_(std::move(url))
        , api_key_(std::move(api_key))
        , timeout_ms_(timeout_ms > 0 ? timeout_ms : 60000)
        , openrouter_headers_(openrouter_headers)
    {
    }

    const char* id() const override { return kind_.c_str(); }

    bool decide(const Request& req, Response& out, std::string& err) override
    {
        err.clear();
        if (req.model.empty()) {
            err = "decisions: model is required";
            return false;
        }
        if (req.questions.empty()) {
            err = "decisions: no questions";
            return false;
        }
        if (req.state.is_null()) {
            err = "decisions: state is required";
            return false;
        }

        nlohmann::json questions = nlohmann::json::object();
        for (const auto& q : req.questions) {
            if (questions.contains(q.id)) {
                err = "decisions: duplicate question id '" + q.id + "'";
                return false;
            }
            questions[q.id] = question_wire(q);
        }
        nlohmann::json body = {
            {"model", req.model},
            {"state", req.state},
            {"questions", std::move(questions)},
        };
        if (!req.session_id.empty())
            body["session_id"] = req.session_id;
        const std::string payload = body.dump();

        CURL* curl = curl_easy_init();
        if (!curl) {
            err = "decisions: curl init failed";
            return false;
        }
        std::string response;
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        hdrs = curl_slist_append(hdrs, "Accept: application/json");
        const std::string auth = "Authorization: Bearer " + api_key_;
        hdrs = curl_slist_append(hdrs, auth.c_str());
        if (openrouter_headers_) {
            hdrs = curl_slist_append(hdrs, "X-OpenRouter-Title: tanit");
        }

        curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "tanit-decisions");
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms_));
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);

        const CURLcode cc = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);

        if (cc != CURLE_OK) {
            err = std::string("decisions: HTTP error: ") + curl_easy_strerror(cc);
            return false;
        }
        if (http_code < 200 || http_code >= 300) {
            err = "decisions: HTTP " + std::to_string(http_code);
            if (!response.empty()) {
                err += ": ";
                err += response.substr(0, std::min<std::size_t>(response.size(), 400));
            }
            return false;
        }

        nlohmann::json parsed = media::json::try_parse(response);
        if (parsed.is_discarded() || !parsed.is_object()) {
            err = "decisions: response is not a JSON object";
            return false;
        }
        if (!parsed.contains("answers") || !parsed["answers"].is_object()) {
            err = "decisions: response has no answers object";
            return false;
        }

        out = Response{};
        out.model = parsed.value("model", req.model);
        out.provider = parsed.value("provider", std::string{});
        out.id = parsed.value("id", std::string{});
        if (parsed.contains("usage") && parsed["usage"].is_object()) {
            const auto& u = parsed["usage"];
            if (u.contains("input_tokens"))
                out.usage.input_tokens = json_int(u["input_tokens"]);
            if (u.contains("output_tokens"))
                out.usage.output_tokens = json_int(u["output_tokens"]);
            if (u.contains("cost") && u["cost"].is_number()) {
                out.usage.cost = u["cost"].get<double>();
                out.usage.has_cost = true;
            }
        }
        for (auto it = parsed["answers"].begin(); it != parsed["answers"].end(); ++it) {
            if (!it.value().is_object() || !it.value().contains("type")) {
                err = "decisions: answer '" + it.key() + "' is missing type";
                return false;
            }
            out.answers.emplace(it.key(), it.value());
        }
        return true;
    }

private:
    std::string kind_;
    std::string url_;
    std::string api_key_;
    int timeout_ms_ = 60000;
    bool openrouter_headers_ = false;
};

const char* default_path(const std::string& kind)
{
    if (kind == "systemone")
        return "/api/v1/systemone";
    if (kind == "jev")
        return "/v1/systemone";
    return "/api/alpha/decisions";
}

const char* default_origin(const std::string& kind)
{
    if (kind == "openrouter" || kind == "systemone")
        return "https://openrouter.ai";
    return "";
}

} // namespace

bool parse_questions_json(const nlohmann::json& doc,
                          std::vector<Question>& out,
                          std::string& err)
{
    err.clear();
    out.clear();
    if (!doc.is_object() || doc.empty()) {
        err = "decisions: questions must be a non-empty object keyed by question id";
        return false;
    }
    out.reserve(doc.size());
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        if (!valid_question_id(it.key())) {
            err = "decisions: question id '" + it.key()
                  + "' must be alphanumeric (plus . _ -) and must not contain '/'";
            return false;
        }
        if (!it.value().is_object()) {
            err = "decisions: question '" + it.key() + "' must be an object";
            return false;
        }
        const auto& body = it.value();
        Question q;
        q.id = it.key();
        q.type = body.value("type", std::string{});
        if (q.type != "choice" && q.type != "noul" && q.type != "score") {
            err = "decisions: question '" + q.id + "' type must be choice, noul, or score";
            return false;
        }
        if (!body.contains("instructions")
            || !(body["instructions"].is_string() || body["instructions"].is_object()
                 || body["instructions"].is_array())) {
            err = "decisions: question '" + q.id + "' needs instructions (string, object, or array)";
            return false;
        }
        if (body["instructions"].is_string() && trim_copy(body["instructions"].get<std::string>()).empty()) {
            err = "decisions: question '" + q.id + "' instructions are empty";
            return false;
        }
        q.instructions = body["instructions"];
        if (body.contains("criteria") && !body["criteria"].is_null())
            q.criteria = body["criteria"];
        if (q.type == "choice") {
            if (!q.criteria.is_object() || q.criteria.empty()) {
                err = "decisions: choice '" + q.id + "' needs a non-empty criteria object";
                return false;
            }
        } else if (q.type == "score") {
            if (!q.criteria.is_array() || q.criteria.empty()) {
                err = "decisions: score '" + q.id + "' needs a non-empty criteria array";
                return false;
            }
        } else if (!q.criteria.is_null() && !q.criteria.is_object()) {
            err = "decisions: noul '" + q.id + "' criteria must be an object when set";
            return false;
        }
        out.push_back(std::move(q));
    }
    return true;
}

std::unique_ptr<Provider> make_provider(const ProviderConfig& cfg, std::string& err)
{
    err.clear();
    const std::string kind = trim_copy(cfg.kind);
    if (kind == "stub")
        return std::make_unique<StubProvider>();
    if (kind == "openai") {
        err = "decisions: openai provider is not implemented "
              "(the request type is ready for a later adapter)";
        return nullptr;
    }
    if (kind != "openrouter" && kind != "systemone" && kind != "jev") {
        err = "decisions: unknown provider '" + kind
              + "' (expected openrouter, systemone, jev, openai, or stub)";
        return nullptr;
    }
    std::string origin = origin_of(cfg.base_url);
    if (origin.empty())
        origin = default_origin(kind);
    if (origin.empty()) {
        err = "decisions: provider '" + kind + "' requires --base-url "
              "(the body is the System One decisions document; --path overrides the default route)";
        return nullptr;
    }
    if (trim_copy(cfg.api_key).empty()) {
        err = "decisions: no API key configured for provider '" + kind + "'";
        return nullptr;
    }
    std::string path = trim_copy(cfg.path);
    if (path.empty())
        path = default_path(kind);
    if (path.empty() || path.front() != '/')
        path.insert(path.begin(), '/');
    const bool or_headers = kind == "openrouter" || kind == "systemone";
    return std::make_unique<HttpProvider>(
        kind, origin + path, trim_copy(cfg.api_key), cfg.timeout_ms, or_headers);
}

struct Collected {
    std::string id;
    nlohmann::json value;
};

int estimate_request_tokens(const nlohmann::json& state,
                            const std::vector<Question>& questions)
{
    nlohmann::json qobj = nlohmann::json::object();
    for (const auto& q : questions) {
        nlohmann::json body = {
            {"type", q.type},
            {"instructions", q.instructions},
        };
        if (!q.criteria.is_null())
            body["criteria"] = q.criteria;
        qobj[q.id] = std::move(body);
    }
    const nlohmann::json wire = {
        {"state", state},
        {"questions", std::move(qobj)},
    };
    const std::string dumped = wire.dump();
    const int chars = static_cast<int>(dumped.size());
    return (chars <= 0 ? 1 : (chars + 2) / 3) + 16;
}

Request build_collected_request(const std::vector<Collected>& rows,
                                std::size_t base,
                                std::size_t n,
                                const std::vector<Question>& questions,
                                const std::string& model,
                                const std::string& collection,
                                const std::string& token)
{
    Request req;
    req.model = model;
    req.state = nlohmann::json::object();
    req.state[collection] = nlohmann::json::array();
    req.questions.reserve(n * questions.size());
    for (std::size_t i = 0; i < n; ++i) {
        req.state[collection].push_back(rows[base + i].value);
        for (const auto& tmpl : questions) {
            Question q = tmpl;
            q.id = std::to_string(i) + "/" + tmpl.id;
            q.instructions = rewrite_token_refs(tmpl.instructions, token, collection, static_cast<int>(i));
            if (!tmpl.criteria.is_null())
                q.criteria = rewrite_token_refs(tmpl.criteria, token, collection, static_cast<int>(i));
            req.questions.push_back(std::move(q));
        }
    }
    return req;
}

bool decide_collected(Provider& provider,
                      const std::vector<Collected>& rows,
                      const std::vector<Question>& questions,
                      const PairBatchOptions& opt,
                      const std::string& collection,
                      const std::string& token,
                      const char* noun,
                      std::vector<PairAnswers>& out,
                      BatchStats& stats,
                      std::string& err)
{
    err.clear();
    out.clear();
    stats = BatchStats{};
    stats.questions = static_cast<int>(questions.size());
    stats.model = opt.model;
    stats.provider_id = provider.id();
    if (rows.empty()) {
        err = std::string("decisions: no ") + noun + "s";
        return false;
    }
    if (questions.empty()) {
        err = "decisions: no questions";
        return false;
    }
    {
        std::map<std::string, int> seen;
        for (const auto& row : rows) {
            if (row.id.empty()) {
                err = std::string("decisions: ") + noun + " id is empty";
                return false;
            }
            if (!seen.emplace(row.id, 1).second) {
                err = std::string("decisions: duplicate ") + noun + " id '" + row.id + "'";
                return false;
            }
        }
    }

    const int qcount = static_cast<int>(questions.size());
    const int cap = opt.max_questions_per_request < 1 ? 1 : opt.max_questions_per_request;
    const int per = std::max(1, cap / qcount);
    const int token_cap = opt.max_request_tokens;

    out.reserve(rows.size());
    for (std::size_t base = 0; base < rows.size();) {
        const std::size_t room = std::min<std::size_t>(
            static_cast<std::size_t>(per), rows.size() - base);
        std::size_t n = room;
        Request req;
        if (token_cap > 0) {
            n = 0;
            for (std::size_t take = 1; take <= room; ++take) {
                Request candidate = build_collected_request(
                    rows, base, take, questions, opt.model, collection, token);
                const int tokens = estimate_request_tokens(candidate.state, candidate.questions);
                if (tokens > token_cap) {
                    if (take == 1) {
                        err = "decisions: one " + std::string(noun) + " ('" + rows[base].id
                            + "') is about " + std::to_string(tokens)
                            + " tokens, over the " + std::to_string(token_cap)
                            + " request limit. A value is not split.";
                        return false;
                    }
                    break;
                }
                n = take;
                req = std::move(candidate);
            }
        } else {
            req = build_collected_request(rows, base, n, questions, opt.model, collection, token);
        }

        Response resp;
        if (!provider.decide(req, resp, err))
            return false;
        stats.requests += 1;
        stats.expanded_questions += static_cast<int>(req.questions.size());
        stats.usage.input_tokens += resp.usage.input_tokens;
        stats.usage.output_tokens += resp.usage.output_tokens;
        if (resp.usage.has_cost) {
            stats.usage.cost += resp.usage.cost;
            stats.usage.has_cost = true;
        }
        if (stats.resolved_model.empty())
            stats.resolved_model = resp.model;

        std::vector<PairAnswers> chunk(n);
        std::vector<std::map<std::string, int>> got(n);
        for (std::size_t i = 0; i < n; ++i)
            chunk[i].id = rows[base + i].id;
        for (const auto& [key, body] : resp.answers) {
            int local = 0;
            std::string qid;
            if (!parse_local_key(key, local, qid) || local >= static_cast<int>(n)) {
                err = "decisions: unexpected answer id '" + key + "'";
                return false;
            }
            if (!got[static_cast<std::size_t>(local)].emplace(qid, 1).second) {
                err = "decisions: duplicate answer '" + key + "'";
                return false;
            }
            chunk[static_cast<std::size_t>(local)].answers.emplace_back(qid, body);
        }
        for (std::size_t i = 0; i < n; ++i) {
            for (const auto& tmpl : questions) {
                if (!got[i].count(tmpl.id)) {
                    err = std::string("decisions: missing answer for ") + noun + " '" + chunk[i].id
                      + "' question '" + tmpl.id + "'";
                    return false;
                }
            }
            // Restore template order (std::map walk is sorted by wire key).
            std::vector<std::pair<std::string, nlohmann::json>> ordered;
            ordered.reserve(questions.size());
            for (const auto& tmpl : questions) {
                for (const auto& ans : chunk[i].answers) {
                    if (ans.first == tmpl.id) {
                        ordered.push_back(ans);
                        break;
                    }
                }
            }
            chunk[i].answers = std::move(ordered);
            out.push_back(std::move(chunk[i]));
        }
        base += n;
    }
    return true;
}

bool decide_pairs(Provider& provider,
                  const std::vector<Pair>& pairs,
                  const std::vector<Question>& questions,
                  const PairBatchOptions& opt,
                  std::vector<PairAnswers>& out,
                  BatchStats& stats,
                  std::string& err)
{
    std::vector<Collected> rows;
    rows.reserve(pairs.size());
    for (const auto& pair : pairs) {
        nlohmann::json value = nlohmann::json::object();
        value["id"] = pair.id;
        value["left"] = pair.left;
        value["right"] = pair.right;
        rows.push_back(Collected{pair.id, std::move(value)});
    }
    if (!decide_collected(provider, rows, questions, opt, "pairs", "pair", "pair", out, stats, err))
        return false;
    stats.pairs = static_cast<int>(pairs.size());
    return true;
}

bool decide_items(Provider& provider,
                  const std::vector<Item>& items,
                  const std::vector<Question>& questions,
                  const PairBatchOptions& opt,
                  std::vector<PairAnswers>& out,
                  BatchStats& stats,
                  std::string& err)
{
    std::vector<Collected> rows;
    rows.reserve(items.size());
    for (const auto& item : items)
        rows.push_back(Collected{item.id, item.value});
    if (!decide_collected(provider, rows, questions, opt, "items", "item", "item", out, stats, err))
        return false;
    stats.items = static_cast<int>(items.size());
    return true;
}

} // namespace media::llm::decisions

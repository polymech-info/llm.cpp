#pragma once

/// Provider-neutral System One decisions (TypeSafe primitives: choice, noul, score).
///
/// One request is one state plus many questions. The questions are the batch:
/// they share the state and are answered independently. OpenRouter's async
/// Batch API does not accept the decisions endpoint, so multi-item work is
/// packed into that one-state request (see decide_pairs) and split into
/// further requests when the expanded question count exceeds the chunk or
/// the estimated request would pass the token ceiling (Jev's context is 32k).
/// One value is never split. A value that does not fit alone is an error.
///
/// OpenRouter (`/api/alpha/decisions`, `/api/v1/systemone`) is one HTTP
/// adapter. `jev` is the same JSON body on a caller-supplied base URL.
/// `openai` is reserved for a later adapter; make_provider rejects it.
///
/// Provider::decide is one state and the questions as written (the document
/// call, and later agent_factory / agent_context routing). decide_items and
/// decide_pairs pack many selected values into that one state and expand
/// each question once per value. `item` / `item.field` and `pair` /
/// `pair.left` / `pair.right` are the template tokens.

#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace media::llm::decisions {

struct Question {
    std::string id;
    std::string type; ///< "choice" | "noul" | "score"
    nlohmann::json instructions = nullptr;
    nlohmann::json criteria = nullptr; ///< omitted on the wire when null
};

struct Request {
    std::string model;
    nlohmann::json state = nullptr;
    std::vector<Question> questions;
    std::string session_id;
};

struct Usage {
    int input_tokens = 0;
    int output_tokens = 0;
    double cost = 0;
    bool has_cost = false;
};

struct Response {
    std::string id;
    std::string model;
    std::string provider;
    Usage usage;
    /// Keyed by question id. Value is the answer object (`type` plus the
    /// primitive fields). Extra provider fields are kept.
    std::map<std::string, nlohmann::json> answers;
};

struct ProviderConfig {
    /// "openrouter" | "systemone" | "jev" | "openai" | "stub"
    std::string kind;
    std::string api_key;
    /// Origin, or an OpenAI-style base ending in /api/v1 (the path is stripped).
    std::string base_url;
    /// HTTP path. Empty selects the kind's default.
    std::string path;
    std::string model;
    int timeout_ms = 60000;
};

class Provider {
public:
    virtual ~Provider() = default;
    virtual const char* id() const = 0;
    virtual bool decide(const Request& req, Response& out, std::string& err) = 0;
};

std::unique_ptr<Provider> make_provider(const ProviderConfig& cfg, std::string& err);

/// Parse a questions object: `{ "<id>": { "type", "instructions", "criteria?" } }`.
bool parse_questions_json(const nlohmann::json& doc,
                          std::vector<Question>& out,
                          std::string& err);

struct Pair {
    std::string id;
    nlohmann::json left = nullptr;
    nlohmann::json right = nullptr;
};

struct PairBatchOptions {
    /// Soft cap on questions per HTTP call. A pair is never split, so one
    /// pair with more questions than this still goes out as one request.
    int max_questions_per_request = 32;
    /// Estimated token ceiling for one request. 0 disables it. Default sits
    /// under Jev's 32k context. The estimate is chars/3 of the JSON body, high
    /// on purpose.
    int max_request_tokens = 24000;
    std::string model;
};

/// Chars/3 of the decisions JSON body (state + questions), plus a small
/// allowance for the model field. High on purpose so a packed request stays
/// under a 32k context.
int estimate_request_tokens(const nlohmann::json& state,
                            const std::vector<Question>& questions);

struct PairAnswers {
    std::string id;
    /// Question id -> answer object, in template order.
    std::vector<std::pair<std::string, nlohmann::json>> answers;
};

struct Item {
    std::string id;
    nlohmann::json value = nullptr;
};

struct BatchStats {
    int pairs = 0;
    int items = 0;
    int questions = 0;
    int requests = 0;
    int expanded_questions = 0;
    Usage usage;
    std::string model;
    std::string resolved_model;
    std::string provider_id;
};

/// Pack pairs into `state.pairs` and expand each question template once per
/// pair. Instructions may name `` `pair.left` ``, `` `pair.right` ``, and
/// `` `pair` ``; those tokens are rewritten to `` `pairs[i]...` `` for the
/// pair's slot in that request.
bool decide_pairs(Provider& provider,
                  const std::vector<Pair>& pairs,
                  const std::vector<Question>& questions,
                  const PairBatchOptions& opt,
                  std::vector<PairAnswers>& out,
                  BatchStats& stats,
                  std::string& err);

/// Pack each value into `state.items` and expand questions. Instructions may
/// name `` `item` `` and `` `item.field` ``; those become `` `items[i]` ``
/// and `` `items[i].field` ``. The array element is the value itself.
bool decide_items(Provider& provider,
                  const std::vector<Item>& items,
                  const std::vector<Question>& questions,
                  const PairBatchOptions& opt,
                  std::vector<PairAnswers>& out,
                  BatchStats& stats,
                  std::string& err);

} // namespace media::llm::decisions

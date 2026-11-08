#pragma once

/// Shared options + runner for `llm agent decide` and `POST /v1/llm/decide`.
/// CLI flags and the HTTP JSON body map onto `DecideOptions` via
/// `parse_decide_options` (camelCase and snake_case keys both accepted).

#include "decisions.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace media::llm::agent::decide {

struct DecideOptions {
    /// Inline document. When null/absent, loaded from `source`.
    nlohmann::json document = nullptr;
    std::string source;
    /// Inline questions object. When null/absent, loaded from `questions_file`.
    nlohmann::json questions = nullptr;
    std::string questions_file;
    std::string selector;
    std::string pairs;
    std::string left;
    std::string right;
    std::string left_key = "left";
    std::string right_key = "right";
    std::string target = "decisions";
    std::string dst;
    std::string provider = "openrouter";
    std::string path;
    std::string model;
    std::string api_key;
    std::string base_url;
    int chunk = 32;
    int max_tokens = 24000;
    int timeout_ms = 0;
    bool dry_run = false;
    /// Base directory for relative `source` / `questions_file` / `dst` paths.
    std::string cwd;
};

struct DecideResult {
    bool ok = false;
    std::string error;
    nlohmann::json document = nullptr;

    std::string source;
    std::string questions_file;
    std::string provider_id;
    std::string model;
    std::string resolved_model;
    bool dry_run = false;
    bool batched = false;
    int pairs = 0;
    int items = 0;
    int questions = 0;
    int expanded_questions = 0;
    int requests = 0;
    int chunk = 32;
    int max_tokens = 24000;
    std::string target;
    std::string dst;
    media::llm::decisions::Usage usage;
    bool wrote_dst = false;

    nlohmann::json to_json() const;
};

/// Fill `out` from an HTTP / tool JSON object. Does not load files.
bool parse_decide_options(const nlohmann::json& j, DecideOptions& out, std::string& err);

/// Validate picker mutual exclusion and required fields.
bool validate_decide_options(const DecideOptions& opt, std::string& err);

/// Run System One decisions over the document / selected values.
bool run_decide(const DecideOptions& opt, DecideResult& out);

} // namespace media::llm::agent::decide

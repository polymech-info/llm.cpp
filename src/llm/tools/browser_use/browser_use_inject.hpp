#pragma once

#include "polymech_export.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace media::llm::browser_use {

struct PromptInjectionHit {
    /// Short machine-stable tag (e.g. "ssh_exfil", "pipe_to_shell").
    std::string kind;
    /// Human-readable reason for logs / abort message.
    std::string reason;
};

/// Heuristic scan for classic HTML/DOM prompt-injection payloads that try to
/// coerce the agent into shell/download/exfil. High-precision patterns only —
/// legitimate `run` / download tasks must not match on their own.
POLYMECH_API std::optional<PromptInjectionHit> detect_prompt_injection(
    std::string_view text);

/// Same scan over a browser tool envelope (pageText, item names, page-model
/// zone text, nested batch step data, tab titles, error strings).
POLYMECH_API std::optional<PromptInjectionHit> detect_prompt_injection_in_envelope(
    const nlohmann::json& envelope);

/// When @p hit is set, redact likely bait from pageText/item names and stamp
/// `prompt_injection_detected` / reason onto the envelope. Forces ok=false so
/// the agent loop can abort without feeding the payload forward.
POLYMECH_API void apply_prompt_injection_guard(
    nlohmann::json& envelope, const PromptInjectionHit& hit);

/// True when a prior browser observation was stamped with injection.
POLYMECH_API bool envelope_flags_prompt_injection(const nlohmann::json& envelope);

/// Shell / download follow-through that should be blocked after (or alongside)
/// browser content. Does not block ordinary local builds or package installs.
POLYMECH_API std::optional<PromptInjectionHit> detect_injection_followthrough_command(
    std::string_view command);

} // namespace media::llm::browser_use

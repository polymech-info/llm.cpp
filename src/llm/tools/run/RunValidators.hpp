#pragma once
//
// media::llm::run — command validation for the Run Tool.
//
// Four-tier model:
//   Allow      — clean, runs without comment.
//   Warn       — pervasive shell idioms (network access, $(…), method calls,
//                env-var writes, splatting). Logged but ALWAYS allowed, even
//                under FEATURE_LLM_RUN_STRICT — promoting these would break
//                legitimate agent commands.
//   WarnStrict — genuinely risky-but-not-certain (e.g. executing an external
//                script file). Logged when strict mode is off; promoted to a
//                hard failure when FEATURE_LLM_RUN_STRICT is on.
//   Deny       — always blocks execution, regardless of strict mode.
//
// Validators are regex-based pattern checkers applied in order; the first
// non-Allow result wins.  New validators should be added to k_validators[]
// in RunValidators.cpp.
//
#include "polymech_export.h"

#include <string>

namespace media::llm::run {

enum class ValidationTier { Allow, Warn, WarnStrict, Deny };

struct ValidationResult {
    ValidationTier tier = ValidationTier::Allow;
    std::string    reason;
};

/// Run the full validator chain against a command string.
/// Returns the first non-Allow result, or Allow if all pass.
POLYMECH_API ValidationResult validate_command(const std::string& command,
                                               const std::string& shell_hint);

} // namespace media::llm::run

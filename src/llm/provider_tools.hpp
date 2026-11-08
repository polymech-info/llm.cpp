#pragma once
//
// Single source of truth for the built-in AI provider registry.
//
// The registry intentionally keeps the full provider catalog, including rows
// disabled by FEATURE_PROVIDER_* for the current build. Runtime-facing callers
// should check build_enabled; GPO generation can still enumerate the full set.
//
#include "polymech_export.h"

#include <cstddef>
#include <string_view>

namespace pm::llm {

struct ProviderInfo {
    const char* name;              ///< canonical provider id used in settings / CLI
    const char* display_name;      ///< human-facing label
    const char* base_url;          ///< default API base URL
    const char* const* models;     ///< suggested/default model ids
    std::size_t model_count;
    const char* policy_key;        ///< e.g. Providers.EnableOpenai
    const char* policy_value_name; ///< e.g. EnableOpenai
    bool build_enabled;            ///< FEATURE_PROVIDER_* result for this build
};

/// Number of entries in the full provider registry.
POLYMECH_API std::size_t provider_registry_size() noexcept;

/// Pointer to the full provider registry. Stable for the process lifetime.
POLYMECH_API const ProviderInfo* provider_registry_data() noexcept;

/// Count of registry entries enabled by FEATURE_PROVIDER_* in this build.
POLYMECH_API std::size_t provider_enabled_registry_size() noexcept;

/// Find by canonical provider id — case-insensitive exact match.
POLYMECH_API const ProviderInfo* provider_find_by_name(std::string_view name) noexcept;

} // namespace pm::llm

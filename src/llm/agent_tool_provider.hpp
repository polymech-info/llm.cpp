#pragma once
//
// Host / app tool providers — first-class attachments on the Tanit agent loop.
//
// Builtins stay in the path catalog. MCP stays a later optional feature.
// OpenDesign (and the next editor) register an IAgentToolProvider; the agent
// owns the loop and calls invoke(). Clients do not execute assistant tool_calls.
//
// See docs/llm/llm-server.md.
//
#include "polymech_export.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace media::llm::agent {

struct ToolDefinition {
    std::string    name;
    std::string    description;
    nlohmann::json parameters = nlohmann::json::object();
};

struct ToolContext {
    std::string    session_id;
    std::string    cwd;
    nlohmann::json extra = nlohmann::json::object();
};

struct ToolResult {
    bool           ok = false;
    nlohmann::json envelope = nlohmann::json::object();

    static ToolResult error(std::string message)
    {
        ToolResult r;
        r.ok       = false;
        r.envelope = {{"ok", false}, {"error", std::move(message)}};
        return r;
    }

    static ToolResult from_envelope(nlohmann::json envelope)
    {
        ToolResult r;
        r.envelope = envelope.is_object() ? std::move(envelope) : nlohmann::json::object();
        try {
            r.ok = r.envelope.value("ok", false);
        } catch (...) {
            r.ok = false;
        }
        if (!r.envelope.contains("ok"))
            r.envelope["ok"] = r.ok;
        return r;
    }
};

class IAgentToolProvider {
public:
    virtual ~IAgentToolProvider() = default;

    virtual std::string id() const = 0;
    virtual std::string name() const = 0;
    virtual std::vector<ToolDefinition> tools() const = 0;

    virtual ToolResult invoke(std::string_view        tool_name,
                              const nlohmann::json&   arguments,
                              const ToolContext&      context) = 0;

    /// Optional block appended to the turn system prompt.
    virtual std::string context_prompt(const ToolContext&) const { return {}; }
};

/// OpenAI Chat Completions function wrapper for one host tool.
PM_CHAT_API nlohmann::json host_tool_openai_schema(const ToolDefinition& def);

/// Case-insensitive tool name match.
PM_CHAT_API bool host_tool_name_eq(std::string_view a, std::string_view b);

/// First registered provider that advertises `tool_name`, or null.
PM_CHAT_API IAgentToolProvider* find_host_tool(
    const std::vector<std::shared_ptr<IAgentToolProvider>>& providers,
    std::string_view                                        tool_name);

namespace tool_providers {

/// Process-wide trusted providers (daemon startup / in-process embed).
PM_CHAT_API void add(std::shared_ptr<IAgentToolProvider> provider);
PM_CHAT_API void remove(std::string_view id);
PM_CHAT_API void clear();
PM_CHAT_API std::vector<std::shared_ptr<IAgentToolProvider>> list();

/// `extras` first (same id wins), then the process registry.
/// Empty `ids` = all. Non-empty = whitelist (unknown ids ignored).
PM_CHAT_API std::vector<std::shared_ptr<IAgentToolProvider>> resolve(
    const std::vector<std::shared_ptr<IAgentToolProvider>>& extras,
    const std::vector<std::string>&                         ids);

} // namespace tool_providers

/// HTTP host provider. `base_url` is the catalog root
/// (e.g. http://127.0.0.1:3727/api/agent-tools). Loopback HTTP only.
std::shared_ptr<IAgentToolProvider> make_http_host_tool_provider(std::string id,
                                                                std::string base_url);

/// Create + register a loopback HTTP provider in the agent image (chat plugin).
PM_CHAT_API bool register_http_host_tool_provider(std::string  id,
                                                  std::string  url,
                                                  std::string* err = nullptr);

/// `{ "providers": [ { "id", "name" } ] }` from the agent-image registry.
PM_CHAT_API nlohmann::json list_registered_host_tool_providers();

} // namespace media::llm::agent

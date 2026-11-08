#include "llm/agent_tool_provider.hpp"

#include "logging/logger.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_set>
#include "string/string_utils.hpp"

namespace media::llm::agent {
namespace {


struct Registry {
    std::mutex                                           mu;
    std::vector<std::shared_ptr<IAgentToolProvider>>     items;
};

Registry& registry()
{
    static Registry r;
    return r;
}

} // namespace

nlohmann::json host_tool_openai_schema(const ToolDefinition& def)
{
    nlohmann::json parameters = def.parameters.is_object() ? def.parameters : nlohmann::json::object();
    if (!parameters.contains("type"))
        parameters["type"] = "object";
    return {
        {"type", "function"},
        {"function",
         {{"name", def.name},
          {"description", def.description},
          {"parameters", std::move(parameters)}}},
    };
}

bool host_tool_name_eq(std::string_view a, std::string_view b)
{
    return media::strutil::lower_ascii(std::string(a)) == media::strutil::lower_ascii(std::string(b));
}

IAgentToolProvider* find_host_tool(
    const std::vector<std::shared_ptr<IAgentToolProvider>>& providers,
    std::string_view                                        tool_name)
{
    for (const auto& p : providers) {
        if (!p)
            continue;
        for (const auto& def : p->tools()) {
            if (host_tool_name_eq(def.name, tool_name))
                return p.get();
        }
    }
    return nullptr;
}

namespace tool_providers {

void add(std::shared_ptr<IAgentToolProvider> provider)
{
    if (!provider)
        return;
    const std::string id = provider->id();
    if (id.empty()) {
        logger::warn("tool_providers.add: empty id — ignored");
        return;
    }
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    for (auto& existing : r.items) {
        if (existing && existing->id() == id) {
            existing = std::move(provider);
            return;
        }
    }
    r.items.push_back(std::move(provider));
}

void remove(std::string_view id)
{
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    r.items.erase(std::remove_if(r.items.begin(), r.items.end(),
                                 [&](const std::shared_ptr<IAgentToolProvider>& p) {
                                     return p && p->id() == id;
                                 }),
                  r.items.end());
}

void clear()
{
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    r.items.clear();
}

std::vector<std::shared_ptr<IAgentToolProvider>> list()
{
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    return r.items;
}

std::vector<std::shared_ptr<IAgentToolProvider>> resolve(
    const std::vector<std::shared_ptr<IAgentToolProvider>>& extras,
    const std::vector<std::string>&                         ids)
{
    std::vector<std::shared_ptr<IAgentToolProvider>> out;
    std::unordered_set<std::string>                  seen;

    auto push = [&](const std::shared_ptr<IAgentToolProvider>& p) {
        if (!p || p->id().empty() || !seen.insert(p->id()).second)
            return;
        out.push_back(p);
    };

    for (const auto& p : extras)
        push(p);
    for (const auto& p : list())
        push(p);

    if (ids.empty())
        return out;

    std::unordered_set<std::string> allow;
    allow.reserve(ids.size());
    for (const auto& id : ids) {
        if (!id.empty())
            allow.insert(id);
    }
    std::vector<std::shared_ptr<IAgentToolProvider>> filtered;
    for (const auto& p : out) {
        if (allow.count(p->id()))
            filtered.push_back(p);
    }
    return filtered;
}

} // namespace tool_providers

} // namespace media::llm::agent

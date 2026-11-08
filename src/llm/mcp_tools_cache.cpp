#include "mcp_config.hpp"
#include "fs/path_sanitizer.hpp"

#include "core/settings_store.hpp"

#include <cstdlib>
#include <filesystem>

namespace media::llm::mcp {

#if FEATURE_MCP_CLIENT

#if defined(PM_HAS_MCP_CONFIG) && PM_HAS_MCP_CONFIG
namespace detail {
void clear_mcp_tools_probe_memory();
void clear_mcp_config_memory();
} // namespace detail
#endif

std::filesystem::path mcp_tools_cache_path()
{
    if (const char* p = std::getenv("POLYMECH_MCP_TOOLS_CACHE")) {
        if (p[0])
            return media::path::path_from_utf8(p);
    }
    return media::settings::get_config_dir() / "mcp-tools-cache.json";
}

void clear_mcp_tools_memory_cache()
{
#if defined(PM_HAS_MCP_CONFIG) && PM_HAS_MCP_CONFIG
    detail::clear_mcp_tools_probe_memory();
    detail::clear_mcp_config_memory();
#endif
}

void invalidate_mcp_tools_cache()
{
    std::error_code ec;
    std::filesystem::remove(mcp_tools_cache_path(), ec);
    clear_mcp_tools_memory_cache();
}

#endif

} // namespace media::llm::mcp

/**
 * Android NDK: load `default_memory.doc` from the session store (plain JSON).
 * Same on-disk name/shape as desktop MemoryTools.cpp file backend.
 * Secure-storage path is not used (FEATURE_SECURE_STORAGE=0).
 */
#include "llm/tools/memory/MemoryTools.hpp"
#include "json/json_utils.hpp"

#include "llm/agent_memory.hpp"
#include "llm/chat_session_store.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace media::llm::path::detail {
namespace {

constexpr const char* k_default_memory_doc_name = "default_memory.doc";

fs::path default_memory_path()
{
  auto options = media::llm::chat_sessions::active_options();
  const fs::path root = options.root_override.empty()
                            ? media::llm::agent::session_store_dir()
                            : options.root_override;
  return root / k_default_memory_doc_name;
}

} // namespace

bool load_global_memory_document(nlohmann::json& out_document)
{
  out_document = nlohmann::json::object();
  const fs::path path = default_memory_path();
  std::error_code ec;
  if (!fs::exists(path, ec))
    return true;
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return true; // treat unreadable as empty (recall stays soft)
  nlohmann::json doc = media::json::try_parse(f);
  if (doc.is_object())
    out_document = std::move(doc);
  return true;
}

} // namespace media::llm::path::detail

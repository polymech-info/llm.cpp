// Copyright PolyMech / pm-image. Internal use only.
//
// Filesystem-oriented agent tools:
//   - list_images   — enumerate image files (exposed to LLM; image membership
//                     uses shared predicates from file_extensions.hpp)
//   - file_glob     — generic file glob (exposed; includes extension, size, and
//                     best-effort media::probe metadata)
//   - file_read     — UTF-8 text read, optional 1-based line window (offset/limit),
//                     optional options.markdown for PDF/Office/RTF/EPUB/CSV,
//                     plus whole-file version (sha256)
//   - file_search   — grep + filename search (exposed path tool). The path-tool
//                     executor calls do_file_search(), which builds
//                     media::SearchOptions and delegates to media::search_files().
//   - write_file    — UTF-8 text write with append (exposed)
//   - file_compose  — assemble one file from files, line ranges, and small literals
//   - file_str_replace — unique-snippet replace on an existing file
//   - file_delete   — delete one regular file
//
// Plus an internal-only helper:
//   - image_find    — kept for CLI / sidecar generation; the agent catalog
//                     no longer exposes it (see make_find_disabled stub).
#pragma once

#include "llm/tools/commons/PathToolCommons.hpp"

#include <cstdint>
#include <functional>

namespace media::llm::path {
struct ToolDef;
}

namespace media::llm::path::detail {

ExecuteResult do_list_images(const nlohmann::json& args);
ExecuteResult do_file_glob(const nlohmann::json& args);
ExecuteResult do_find(const nlohmann::json& args);          // image_find — internal-only
ExecuteResult do_file_search(const nlohmann::json& args);
ExecuteResult do_search_index(const nlohmann::json& args);
ExecuteResult do_search_query(const nlohmann::json& args);
ExecuteResult do_search_detail(const nlohmann::json& args);
/// Non-empty when path was just indexed and file_read must send the model to search_query.
std::string indexed_read_block(const std::string& abs_path);
/// Non-empty for a pdf-to-md part (`<stem>_N.md` inside a `*_md` folder).
std::string converted_part_read_block(const std::string& abs_path);
ExecuteResult do_file_read_path(const nlohmann::json& args);
ExecuteResult do_write_file(const nlohmann::json& args);
ExecuteResult do_file_compose(const nlohmann::json& args);
ExecuteResult do_file_str_replace(const nlohmann::json& args);
ExecuteResult do_file_delete(const nlohmann::json& args);
ExecuteResult do_download_url(const nlohmann::json& args);

void set_download_url_progress_hook(std::function<void(std::uint64_t received, std::uint64_t total)> f);
void clear_download_url_progress_hook();

ToolDef make_list_images();
ToolDef make_file_glob();
ToolDef make_file_read();
ToolDef make_file_search();
ToolDef make_search_index();
ToolDef make_search_query();
ToolDef make_search_detail();
ToolDef make_write_file();
ToolDef make_file_compose();
ToolDef make_file_str_replace();
ToolDef make_file_delete();
ToolDef make_download_url();

} // namespace media::llm::path::detail

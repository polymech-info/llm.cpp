#include "llm/huggingface/hf_manifest.hpp"

#include "core/batch_queue.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include "fs/path_sanitizer.hpp"

namespace fs = std::filesystem;

namespace pm::hf {
namespace {

std::string utc_now_iso()
{
    const auto now = std::chrono::system_clock::now();
    const auto t   = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

bool is_gguf_file(const fs::path& p)
{
    const std::string name = media::path::path_to_utf8(p.filename());
    return name.size() > 5 && name.rfind(".gguf") == name.size() - 5;
}

} // namespace

nlohmann::json build_manifest(const ModelEntry& entry, const fs::path& output_dir, const std::string& variant,
                              bool hash_files)
{
    nlohmann::json files = nlohmann::json::array();
    std::error_code ec;
    if (fs::is_directory(output_dir, ec)) {
        for (fs::recursive_directory_iterator it(output_dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec) || ec)
                continue;
            const fs::path path = it->path();
            if (path.filename() == "tanit-manifest.json")
                continue;
            if (!is_gguf_file(path))
                continue;
            nlohmann::json f;
            f["path"] = media::path::path_to_generic_utf8(path.lexically_relative(output_dir));
            f["size_bytes"] = static_cast<std::uint64_t>(fs::file_size(path, ec));
            if (hash_files)
                f["sha256"] = media::file_sha256_fast(media::path::path_to_utf8(path));
            files.push_back(std::move(f));
        }
    }

    nlohmann::json manifest = {
        {"generated_at", utc_now_iso()},
        {"store_name", entry.store_name},
        {"repo_id", entry.repo_id},
        {"format", entry.format},
        {"variant", variant},
        {"description", entry.description},
        {"file_count", files.size()},
        {"files", files},
    };
    if (!entry.alias.empty())
        manifest["alias"] = entry.alias;
    return manifest;
}

fs::path write_manifest(const ModelEntry& entry, const fs::path& output_dir, const std::string& variant,
                        bool hash_files)
{
    const nlohmann::json manifest = build_manifest(entry, output_dir, variant, hash_files);
    const fs::path manifest_path  = output_dir / "tanit-manifest.json";
    std::error_code ec;
    fs::create_directories(output_dir, ec);
    std::ofstream out(manifest_path, std::ios::binary | std::ios::trunc);
    out << manifest.dump(2) << '\n';
    return manifest_path;
}

} // namespace pm::hf

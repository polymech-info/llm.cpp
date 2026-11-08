#pragma once

#include "llm/huggingface/hf_stream_download.hpp"

#include <string>

namespace pm::hf {

struct XetFileDownloadRequest {
    std::string repo_id;
    std::string revision;   ///< HF commit sha; fetched when empty
    std::string xet_hash;   ///< Xet file id (tree xetHash)
    std::string hf_token;
    std::filesystem::path local_path;
    std::int64_t expected_size = -1;
    StreamDownloadOptions options;
};

/// Download one Xet-backed file via CAS reconstruction (HF Hub storage).
StreamDownloadResult xet_download_file(XetFileDownloadRequest request);

} // namespace pm::hf

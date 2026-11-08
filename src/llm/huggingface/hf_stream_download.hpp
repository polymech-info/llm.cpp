#pragma once

#include "llm/huggingface/hf_types.hpp"

#include <string>

namespace pm::hf {

StreamDownloadResult stream_download_url(const std::string& url,
                                         const std::filesystem::path& local_path,
                                         StreamDownloadOptions options = {});

} // namespace pm::hf

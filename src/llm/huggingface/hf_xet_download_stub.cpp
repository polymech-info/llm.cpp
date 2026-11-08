#include "llm/huggingface/hf_xet_download.hpp"

#if !defined(FEATURE_COMMAND_HG) || !FEATURE_COMMAND_HG

namespace pm::hf {

StreamDownloadResult xet_download_file(XetFileDownloadRequest /*request*/)
{
    StreamDownloadResult out;
    out.ok = false;
    out.err = "Xet CAS download requires FEATURE_COMMAND_HG in this build";
    return out;
}

} // namespace pm::hf

#endif

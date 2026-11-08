#include "universal_compressor.hpp"

#include <mutex>

namespace media::llm::compress {
namespace {

std::mutex& reducer_mutex()
{
    static std::mutex m;
    return m;
}

TextModelReducer& reducer_slot()
{
    static TextModelReducer r;
    return r;
}

} // namespace

void set_text_model_reducer(TextModelReducer reducer)
{
    std::lock_guard<std::mutex> lock(reducer_mutex());
    reducer_slot() = std::move(reducer);
}

bool has_text_model_reducer()
{
    std::lock_guard<std::mutex> lock(reducer_mutex());
    return static_cast<bool>(reducer_slot());
}

TextModelReducer text_model_reducer()
{
    std::lock_guard<std::mutex> lock(reducer_mutex());
    return reducer_slot();
}

} // namespace media::llm::compress

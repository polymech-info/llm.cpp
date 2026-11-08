#pragma once

#include <string>

namespace media::llm {

// DeepSeek-OCR / Unlimited-OCR grounding dialect:
//   label [x1, y1, x2, y2]text
// and the <|ref|> / <|det|> token form of the same family.
// Other model text is returned unchanged.
std::string clean_ocr_markup(const std::string& raw);

} // namespace media::llm

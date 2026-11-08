#include "universal_compressor.hpp"

#include <algorithm>
#include <string>

#include "structure_mask.hpp"

namespace media::llm::compress {

std::size_t estimate_tokens(std::string_view text) noexcept
{
    return (text.size() + 3) / 4;
}

namespace {

TextModelReducer current_reducer()
{
    return text_model_reducer();
}

// Prose-like types Kompress was trained on. JSON/Code/Diff keep the structural
// pipeline (the model would shred their syntax).
bool is_prose_like(ContentType t) noexcept
{
    return t == ContentType::Text || t == ContentType::Markdown || t == ContentType::Log;
}

// Deterministic, model-free reduction of a single non-structural span.
std::string compress_span(std::string_view span, const CompressOptions& opts)
{
    std::string reduced = reduce_text(span, opts.text);
    if (opts.enable_truncation) {
        reduced = simple_truncate(reduced, opts.compression_ratio_target);
    }
    // Never let "reduction" grow a span.
    if (reduced.size() >= span.size()) return std::string(span);
    return reduced;
}

} // namespace

CompressionResult compress(std::string_view content, const CompressOptions& opts)
{
    CompressionResult r;
    r.input_bytes    = content.size();
    r.output_bytes   = content.size();
    r.compressed.assign(content);
    r.tokens_before  = estimate_tokens(content);
    r.tokens_after   = r.tokens_before;
    r.content_type   = ContentType::Unknown;

    // 1. Too short to bother.
    if (content.size() < opts.min_content_length) {
        r.skipped = true;
        r.note    = "content shorter than min_content_length";
        return r;
    }

    // 2. Detect (or honor override).
    if (opts.type == ContentType::Auto) {
        DetectionResult d   = detect_content(content);
        r.content_type      = d.content_type;
        r.detection_confidence = d.confidence;
    } else {
        r.content_type      = opts.type;
        r.detection_confidence = 1.0;
    }

    // 2b. For prose-like content, prefer a registered model reducer (Kompress).
    //     Falls through to the deterministic pipeline if none is installed or
    //     the model declines / errors (returns nullopt).
    if (is_prose_like(r.content_type)) {
        if (TextModelReducer reducer = current_reducer()) {
            // target_ratio<=0 → let the model decide (headroom's threshold mode);
            // the proxy path never forces a ratio, matching headroom.
            if (std::optional<std::string> out = reducer(content, -1.0)) {
                r.compressed      = std::move(*out);
                r.handler_used    = "kompress";
                r.output_bytes    = r.compressed.size();
                r.tokens_after    = estimate_tokens(r.compressed);
                r.preservation_ratio =
                    content.empty() ? 1.0
                                    : static_cast<double>(r.compressed.size()) /
                                          static_cast<double>(content.size());
                r.compression_ratio =
                    r.input_bytes == 0 ? 1.0
                                       : static_cast<double>(r.output_bytes) /
                                             static_cast<double>(r.input_bytes);
                return r;
            }
            // model declined → deterministic fallback below
        }
    }

    // 3. Build the structural preservation mask.
    StructureMask mask;
    if (r.content_type == ContentType::Json && json_can_handle(content)) {
        JsonHandlerResult jr = json_structure_mask(content, opts.json);
        mask            = std::move(jr.mask);
        r.handler_used  = "json";
    } else if (opts.type == ContentType::Json) {
        // Forced JSON that does not validate: still tokenize best-effort.
        JsonHandlerResult jr = json_structure_mask(content, opts.json);
        mask            = std::move(jr.mask);
        r.handler_used  = "json";
    } else {
        // No structural handler for this type yet (text/code/log/…): everything
        // is compressible; entropy preservation (below) protects identifiers.
        mask           = StructureMask::empty_mask(content.size());
        r.handler_used = "noop";
    }

    // 4. Union the high-entropy-word preservation mask.
    if (opts.use_entropy_preservation) {
        StructureMask entropy = compute_entropy_mask(content, opts.entropy_threshold);
        mask = mask.union_with(entropy);
    }

    r.preservation_ratio = mask.preservation_ratio();

    // 5. Reduce only the non-structural spans.
    std::string out;
    out.reserve(content.size());
    for (const MaskSpan& span : mask_to_spans(mask)) {
        std::string_view text = content.substr(span.start, span.length());
        if (span.is_structural || span.length() <= opts.min_span_to_compress) {
            out.append(text);
        } else {
            out.append(compress_span(text, opts));
        }
    }

    r.compressed   = std::move(out);
    r.output_bytes = r.compressed.size();
    r.tokens_after = estimate_tokens(r.compressed);
    r.compression_ratio =
        r.input_bytes == 0 ? 1.0
                           : static_cast<double>(r.output_bytes) /
                                 static_cast<double>(r.input_bytes);
    return r;
}

} // namespace media::llm::compress

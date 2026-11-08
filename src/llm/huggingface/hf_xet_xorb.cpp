#include "llm/huggingface/hf_xet_xorb.hpp"

#include <lz4frame.h>

#include <cstring>

namespace pm::hf {
namespace {

std::uint32_t read_u24_le(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8)
           | (static_cast<std::uint32_t>(p[2]) << 16);
}

bool lz4f_decompress_buffer(const std::uint8_t* src, std::size_t src_len, std::size_t dst_hint,
                            std::vector<std::uint8_t>& out, std::string* err_out)
{
    LZ4F_dctx* dctx = nullptr;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION))) {
        if (err_out)
            *err_out = "LZ4F_createDecompressionContext failed";
        return false;
    }

    std::size_t dst_cap = dst_hint > 0 ? dst_hint : (src_len * 4 + 1024);
    out.resize(dst_cap);

    std::size_t src_pos = 0;
    std::size_t dst_pos = 0;
    while (src_pos < src_len) {
        std::size_t src_chunk = src_len - src_pos;
        std::size_t dst_chunk = dst_cap - dst_pos;
        const std::size_t ret =
            LZ4F_decompress(dctx, out.data() + dst_pos, &dst_chunk, src + src_pos, &src_chunk, nullptr);
        if (LZ4F_isError(ret)) {
            if (err_out)
                *err_out = LZ4F_getErrorName(ret);
            LZ4F_freeDecompressionContext(dctx);
            return false;
        }
        src_pos += src_chunk;
        dst_pos += dst_chunk;
        if (ret == 0)
            break;
        if (dst_pos >= dst_cap) {
            dst_cap *= 2;
            out.resize(dst_cap);
        }
    }

    out.resize(dst_pos);
    LZ4F_freeDecompressionContext(dctx);
    return true;
}

bool byte_ungroup_4(const std::uint8_t* grouped, std::size_t grouped_len, std::size_t original_len,
                    std::vector<std::uint8_t>& out, std::string* err_out)
{
    if (grouped_len < original_len) {
        if (err_out)
            *err_out = "byte_ungroup_4: grouped buffer too small";
        return false;
    }
    const std::size_t base  = original_len / 4;
    const std::size_t rem   = original_len % 4;
    std::size_t sizes[4]    = {base + (rem > 0 ? 1 : 0), base + (rem > 1 ? 1 : 0), base + (rem > 2 ? 1 : 0),
                               base};
    std::size_t offset      = 0;
    const std::uint8_t* groups[4];
    for (int i = 0; i < 4; ++i) {
        if (offset + sizes[i] > grouped_len) {
            if (err_out)
                *err_out = "byte_ungroup_4: invalid group sizes";
            return false;
        }
        groups[i] = grouped + offset;
        offset += sizes[i];
    }

    out.resize(original_len);
    for (std::size_t i = 0; i < original_len; ++i) {
        const std::size_t group_idx    = i % 4;
        const std::size_t pos_in_group = i / 4;
        out[i]                         = groups[group_idx][pos_in_group];
    }
    return true;
}

} // namespace

bool xet_decompress_chunk(std::uint8_t compression_type, const std::uint8_t* compressed,
                          std::size_t compressed_len, std::size_t uncompressed_len,
                          std::vector<std::uint8_t>& out, std::string* err_out)
{
    if (!compressed && compressed_len > 0) {
        if (err_out)
            *err_out = "xet_decompress_chunk: null input";
        return false;
    }

    switch (compression_type) {
    case 0: // None
        out.assign(compressed, compressed + compressed_len);
        return true;
    case 1: { // LZ4 frame
        return lz4f_decompress_buffer(compressed, compressed_len, uncompressed_len, out, err_out);
    }
    case 2: { // ByteGrouping4LZ4
        std::vector<std::uint8_t> grouped;
        if (!lz4f_decompress_buffer(compressed, compressed_len, 0, grouped, err_out))
            return false;
        return byte_ungroup_4(grouped.data(), grouped.size(), uncompressed_len, out, err_out);
    }
    default:
        if (err_out)
            *err_out = "xet_decompress_chunk: unknown compression type " + std::to_string(compression_type);
        return false;
    }
}

bool xet_parse_xorb_chunks(const std::uint8_t* data, std::size_t len,
                           std::vector<std::vector<std::uint8_t>>& chunks_out, std::string* err_out)
{
    chunks_out.clear();
    std::size_t pos = 0;
    while (pos + 8 <= len) {
        const std::uint8_t version          = data[pos];
        const std::uint32_t compressed_size = read_u24_le(data + pos + 1);
        const std::uint8_t compression_type = data[pos + 4];
        const std::uint32_t uncompressed_sz = read_u24_le(data + pos + 5);
        pos += 8;

        if (version != 0) {
            if (err_out)
                *err_out = "xet_parse_xorb_chunks: unsupported chunk version";
            return false;
        }
        if (compressed_size == 0 || pos + compressed_size > len) {
            if (err_out)
                *err_out = "xet_parse_xorb_chunks: truncated chunk payload";
            return false;
        }

        std::vector<std::uint8_t> chunk;
        std::string local_err;
        if (!xet_decompress_chunk(compression_type, data + pos, compressed_size, uncompressed_sz, chunk,
                                  &local_err)) {
            if (err_out)
                *err_out = local_err;
            return false;
        }
        chunks_out.push_back(std::move(chunk));
        pos += compressed_size;
    }

    if (pos != len) {
        if (err_out)
            *err_out = "xet_parse_xorb_chunks: trailing bytes after last chunk";
        return false;
    }
    return true;
}

} // namespace pm::hf

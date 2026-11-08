#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pm::hf {

/// Decompress one Xorb chunk payload (compression type from 8-byte chunk header).
bool xet_decompress_chunk(std::uint8_t compression_type, const std::uint8_t* compressed,
                          std::size_t compressed_len, std::size_t uncompressed_len,
                          std::vector<std::uint8_t>& out, std::string* err_out = nullptr);

/// Parse consecutive Xorb chunks from a byte buffer (one CAS/xorb download slice).
bool xet_parse_xorb_chunks(const std::uint8_t* data, std::size_t len,
                           std::vector<std::vector<std::uint8_t>>& chunks_out,
                           std::string* err_out = nullptr);

} // namespace pm::hf

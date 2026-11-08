#pragma once

#include <cstdint>
#include <string>

namespace pm::hf {

int quant_bits_from_label(const std::string& label);

double bytes_to_decimal_gb(std::uint64_t bytes);

std::string display_quant_label(const std::string& label);

bool is_spurious_variant_label(const std::string& label);

} // namespace pm::hf

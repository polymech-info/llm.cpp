#include "llm/huggingface/hf_quant.hpp"

#include <algorithm>
#include <cctype>
#include "string/string_utils.hpp"

namespace pm::hf {
namespace {

using media::strutil::lower_ascii;

} // namespace

int quant_bits_from_label(const std::string& label)
{
    const std::string lower = lower_ascii(label);
    if (lower == "fp16" || lower == "f16" || lower == "bf16")
        return 16;
    if (lower.rfind("iq", 0) == 0) {
        for (std::size_t i = 2; i < lower.size(); ++i) {
            if (std::isdigit(static_cast<unsigned char>(lower[i])))
                return lower[i] - '0';
        }
    }
    if (lower.empty() || lower[0] != 'q')
        return 0;
    int bits = 0;
    for (std::size_t i = 1; i < lower.size() && std::isdigit(static_cast<unsigned char>(lower[i])); ++i)
        bits = bits * 10 + (lower[i] - '0');
    return bits;
}

double bytes_to_decimal_gb(std::uint64_t bytes)
{
    return static_cast<double>(bytes) / 1'000'000'000.0;
}

std::string display_quant_label(const std::string& label)
{
    std::string out = label;
    for (char& c : out)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

bool is_spurious_variant_label(const std::string& label)
{
    if (label.empty())
        return true;
    return std::all_of(label.begin(), label.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

} // namespace pm::hf

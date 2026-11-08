#include "llm/huggingface/hf_gguf_header.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_map>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

namespace pm::hf {
namespace {

constexpr std::uint32_t k_gguf_magic = 0x46554747u; // "GGUF"

bool read_exact(std::ifstream& in, void* dst, std::size_t n)
{
    return static_cast<bool>(in.read(static_cast<char*>(dst), static_cast<std::streamsize>(n)));
}

template <typename T>
bool read_le(std::ifstream& in, T& out)
{
    return read_exact(in, &out, sizeof(T));
}

bool skip_gguf_value(std::ifstream& in, std::uint32_t vtype)
{
    switch (vtype) {
    case 0:
    case 1:
        return in.seekg(1, std::ios::cur).good();
    case 2:
    case 3:
        return in.seekg(2, std::ios::cur).good();
    case 4:
    case 5:
    case 6:
    case 7:
        return in.seekg(4, std::ios::cur).good();
    case 8: {
        std::uint64_t slen = 0;
        if (!read_le(in, slen) || slen > (1u << 20))
            return false;
        return in.seekg(static_cast<std::streamoff>(slen), std::ios::cur).good();
    }
    case 9: {
        std::uint32_t atype = 0;
        std::uint64_t alen  = 0;
        if (!read_le(in, atype) || !read_le(in, alen) || alen > (1u << 20))
            return false;
        for (std::uint64_t i = 0; i < alen; ++i) {
            if (!skip_gguf_value(in, atype))
                return false;
        }
        return true;
    }
    case 10:
    case 11:
    case 12:
        return in.seekg(8, std::ios::cur).good();
    default:
        return false;
    }
}

bool read_string_value(std::ifstream& in, std::string& out)
{
    std::uint64_t slen = 0;
    if (!read_le(in, slen) || slen > (1u << 20))
        return false;
    out.resize(static_cast<std::size_t>(slen));
    if (slen == 0)
        return true;
    return read_exact(in, out.data(), static_cast<std::size_t>(slen));
}

bool read_uint_value(std::ifstream& in, std::uint32_t vtype, std::uint64_t& out)
{
    if (vtype == 4) {
        std::uint32_t v = 0;
        if (!read_le(in, v))
            return false;
        out = v;
        return true;
    }
    if (vtype == 10) {
        return read_le(in, out);
    }
    return false;
}

using media::strutil::lower_ascii;

void set_arch_keys(const std::string& arch, std::unordered_map<std::string, std::string>& arch_keys)
{
    arch_keys.clear();
    const auto key = [&](const char* suffix, const char* attr) {
        arch_keys[arch + suffix] = attr;
    };
    key(".context_length", "context_length");
    key(".block_count", "block_count");
    key(".attention.head_count_kv", "n_kv_heads");
    key(".attention.head_count", "n_heads");
    key(".embedding_length", "embedding_length");
    key(".attention.key_length", "kv_key_length");
    key(".attention.value_length", "kv_value_length");
    key(".attention.kv_lora_rank", "kv_lora_rank");
    key(".attention.sliding_window", "sliding_window");
}

void apply_arch_attr(GgufHeaderMeta& meta, const std::string& attr, std::uint64_t value)
{
    if (attr == "context_length")
        meta.context_length = value;
    else if (attr == "block_count")
        meta.block_count = static_cast<std::uint32_t>(value);
    else if (attr == "n_kv_heads")
        meta.n_kv_heads = static_cast<std::uint32_t>(value);
    else if (attr == "n_heads")
        meta.n_heads = static_cast<std::uint32_t>(value);
    else if (attr == "embedding_length")
        meta.embedding_length = static_cast<std::uint32_t>(value);
    else if (attr == "kv_key_length")
        meta.kv_key_length = static_cast<std::uint32_t>(value);
    else if (attr == "kv_value_length")
        meta.kv_value_length = static_cast<std::uint32_t>(value);
    else if (attr == "kv_lora_rank")
        meta.kv_lora_rank = value;
    else if (attr == "sliding_window")
        meta.sliding_window = static_cast<std::uint32_t>(value);
}

} // namespace

GgufHeaderMeta read_gguf_header(const std::filesystem::path& path)
{
    GgufHeaderMeta meta;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return meta;

    std::uint32_t magic = 0;
    if (!read_le(in, magic) || magic != k_gguf_magic) {
        meta.error = "not a GGUF file";
        return meta;
    }

    std::uint32_t version = 0;
    std::uint64_t tensor_count = 0;
    std::uint64_t kv_count     = 0;
    if (!read_le(in, version) || !read_le(in, tensor_count) || !read_le(in, kv_count))
        return meta;

    static const char* k_wanted_general[] = {
        "general.architecture",
        "general.type",
        "general.basename",
        "general.organization",
        "general.base_model.0.repo_url",
        "general.source.huggingface.repository",
        "general.source.url",
        "tokenizer.chat_template",
    };

    std::unordered_map<std::string, std::string> arch_keys;
    std::string arch;

    for (std::uint64_t i = 0; i < kv_count; ++i) {
        std::uint64_t key_len = 0;
        if (!read_le(in, key_len) || key_len > (1u << 20))
            break;
        std::string key(static_cast<std::size_t>(key_len), '\0');
        if (!read_exact(in, key.data(), static_cast<std::size_t>(key_len)))
            break;

        std::uint32_t vtype = 0;
        if (!read_le(in, vtype))
            break;

        bool wanted = false;
        for (const char* w : k_wanted_general) {
            if (key == w) {
                wanted = true;
                break;
            }
        }
        if (!wanted) {
            const auto it = arch_keys.find(key);
            if (it != arch_keys.end())
                wanted = true;
        }

        if (!wanted) {
            if (!skip_gguf_value(in, vtype))
                break;
            continue;
        }

        if (key == "general.architecture" && vtype == 8) {
            if (!read_string_value(in, arch))
                break;
            meta.architecture = arch;
            set_arch_keys(arch, arch_keys);
            continue;
        }

        if (vtype == 8) {
            std::string val;
            if (!read_string_value(in, val))
                break;
            if (key == "general.type")
                meta.general_type = val;
            else if (key == "general.basename")
                meta.basename = val;
            else if (key == "general.organization")
                meta.organization = val;
            else if (key == "general.base_model.0.repo_url")
                meta.base_model_repo_url = val;
            else if (key == "tokenizer.chat_template") {
                meta.has_chat_template   = !val.empty();
                meta.chat_template_chars = val.size();
            }
            continue;
        }

        if (vtype == 4 || vtype == 10) {
            std::uint64_t val = 0;
            if (!read_uint_value(in, vtype, val))
                break;
            const auto it = arch_keys.find(key);
            if (it != arch_keys.end())
                apply_arch_attr(meta, it->second, val);
            continue;
        }

        if (!skip_gguf_value(in, vtype))
            break;
    }

    meta.is_mmproj = lower_ascii(meta.general_type) == "mmproj" ||
                     lower_ascii(media::path::path_to_utf8(path.filename())).find("mmproj") != std::string::npos;
    meta.valid     = !meta.architecture.empty() || meta.is_mmproj || meta.has_chat_template;
    return meta;
}

} // namespace pm::hf

#include "tool_executor.hpp"
#include "json/json_utils.hpp"
#include "llm/tools/run/RunTool.hpp"
#include "llm/path_tool_executor.hpp"

#include "llm/llm_constants.hpp"
#include "fs/fs_guard.hpp"
#include "fs/glob_filter.hpp"
#include "llm_image_tool_defaults.hpp"

#include "core/compress.hpp"
#include "core/find.hpp"
#include "core/meta.hpp"
#include "core/resize.hpp"
#include "core/transform.hpp"
#include "core/settings_portable.hpp"
#include "core/settings_runtime.hpp"
#include "logging/pm_log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <filesystem>
#include "fs/path_sanitizer.hpp"
#include "string/string_utils.hpp"

namespace media::llm {

namespace {

namespace fs = std::filesystem;

// ── Base64 helpers (RFC 4648 standard alphabet) ────────────────────────────

bool b64_decode(const std::string& in, std::string& out) {
    static int8_t T[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) T[i] = -1;
        const char *abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) T[(unsigned char)abc[i]] = (int8_t)i;
        init = true;
    }
    out.clear();
    out.reserve((in.size() / 4) * 3);
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        if (T[c] == -1) return false;
        val = (val << 6) | T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return true;
}

std::string b64_encode(const unsigned char* data, std::size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(tbl[(n >> 18) & 0x3F]);
        out.push_back(tbl[(n >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? tbl[(n >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? tbl[n & 0x3F]        : '=');
    }
    return out;
}

// ── Envelope builders ──────────────────────────────────────────────────────

ExecuteResult make_error(const std::string& msg) {
    ExecuteResult r;
    r.ok = false;
    r.error = msg;
    r.envelope = nlohmann::json{{"ok", false}, {"error", msg}};
    return r;
}

ExecuteResult make_binary_envelope(const std::string& mime, const std::string& bytes) {
    ExecuteResult r;
    r.ok = true;
    r.envelope = nlohmann::json{
        {"ok",    true},
        {"mime",  mime},
        {"bytes", bytes.size()},
        {"b64",   b64_encode(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size())},
    };
    return r;
}

ExecuteResult make_json_envelope(nlohmann::json result) {
    ExecuteResult r;
    r.ok = true;
    r.envelope = nlohmann::json{{"ok", true}, {"result", std::move(result)}};
    return r;
}

// Pull `arguments.image.{mime,b64}` → decoded bytes. Returns empty bytes on
// missing/invalid input and populates `err` when something is wrong.
bool extract_image(const nlohmann::json& args, std::string& bytes, std::string& mime, std::string& err) {
    if (!args.contains("image") || !args["image"].is_object()) {
        err = "missing 'image' object (expected { mime, b64 })";
        return false;
    }
    const auto& img = args["image"];
    if (!img.contains("b64") || !img["b64"].is_string()) {
        err = "missing 'image.b64' (base64-encoded bytes)";
        return false;
    }
    if (!b64_decode(img["b64"].get<std::string>(), bytes)) {
        err = "image.b64 is not valid base64";
        return false;
    }
    if (bytes.empty()) {
        err = "image.b64 decoded to zero bytes";
        return false;
    }
    mime = (img.contains("mime") && img["mime"].is_string()) ? img["mime"].get<std::string>() : "";
    return true;
}

const nlohmann::json& options_or_empty(const nlohmann::json& args) {
    static const nlohmann::json empty = nlohmann::json::object();
    if (args.contains("options") && args["options"].is_object()) return args["options"];
    return empty;
}

// ── Per-tool dispatchers ───────────────────────────────────────────────────

ExecuteResult do_resize(const nlohmann::json& args) {
    std::string in, mime, err;
    if (!extract_image(args, in, mime, err)) return make_error(err);
    media::ResizeOptions opt;
    media::apply_resize_options_from_json(options_or_empty(args), opt);
    opt.cache_enabled = false; // zero-fs path
    auto r = media::resize_buffer(in.data(), in.size(), opt);
    if (!r.ok) return make_error(r.error);
    return make_binary_envelope(r.mime, r.bytes);
}

ExecuteResult do_compress(const nlohmann::json& args) {
    std::string in, mime, err;
    if (!extract_image(args, in, mime, err)) return make_error(err);
    media::CompressOptions opt;
    media::apply_compress_options_from_json(options_or_empty(args), opt);
    auto r = media::compress_buffer(in.data(), in.size(), opt);
    if (!r.ok) return make_error(r.error);
    return make_binary_envelope(r.mime, r.bytes);
}

ExecuteResult do_transform(const nlohmann::json& args) {
    std::string in, mime, err;
    if (!extract_image(args, in, mime, err)) return make_error(err);
    const nlohmann::json& tool_opts = options_or_empty(args);
    media::TransformOptions opt;
    media::apply_transform_options_from_json(tool_opts, opt);
    opt.provider.clear();
    opt.model.clear();
    opt.api_key.clear();
    opt.base_url.clear();
    apply_chat_image_defaults_for_tool_options(opt.provider, opt.model, nlohmann::json::object());
    ensure_api_key_for_image_provider(opt.provider, opt.api_key);
    ensure_base_url_for_image_provider(opt.provider, opt.base_url);
    if (opt.provider.empty() || opt.model.empty())
        return make_error("image_transform: missing provider/model: set Chat -> Image provider/model in Settings");

    std::vector<media::ReferenceBuffer> refs;
    if (args.contains("references") && args["references"].is_array()) {
        for (const auto& ref : args["references"]) {
            if (!ref.is_object() || !ref.contains("b64") || !ref["b64"].is_string()) continue;
            std::string rb;
            if (!b64_decode(ref["b64"].get<std::string>(), rb) || rb.empty()) continue;
            media::ReferenceBuffer rbuf;
            rbuf.mime  = (ref.contains("mime") && ref["mime"].is_string())
                            ? ref["mime"].get<std::string>() : "image/png";
            rbuf.bytes = std::move(rb);
            refs.push_back(std::move(rbuf));
        }
    }

    auto r = media::transform_buffer(in.data(), in.size(),
                                     mime.empty() ? "image/png" : mime, opt, refs);
    if (!r.ok) return make_error(r.error);
    auto env = make_binary_envelope(r.mime, r.bytes);
    if (!r.ai_text.empty()) env.envelope["ai_text"] = r.ai_text;
    return env;
}

ExecuteResult do_image_create(const nlohmann::json& args) {
    const nlohmann::json& tool_opts = options_or_empty(args);
    media::TransformOptions opt;
    media::apply_transform_options_from_json(tool_opts, opt);
    /* Provider / model / keys from Chat → Image settings, not LLM tool JSON (matches path_tool_executor). */
    opt.provider.clear();
    opt.model.clear();
    opt.api_key.clear();
    opt.base_url.clear();
    apply_chat_image_defaults_for_tool_options(opt.provider, opt.model, nlohmann::json::object());
    media::runtime_settings::merge_provider_credentials(
        opt.provider, false, opt.api_key, opt.base_url);
    ensure_api_key_for_image_provider(opt.provider, opt.api_key);
    ensure_base_url_for_image_provider(opt.provider, opt.base_url);
    if (opt.prompt.empty()) return make_error("options.prompt is required");
    if (opt.provider.empty() || opt.model.empty())
        return make_error("image_create: missing provider/model: set Chat -> Image provider/model in Settings");

    std::vector<media::ReferenceBuffer> refs;
    if (args.contains("references") && args["references"].is_array()) {
        for (const auto& ref : args["references"]) {
            if (!ref.is_object() || !ref.contains("b64") || !ref["b64"].is_string()) continue;
            std::string rb;
            if (!b64_decode(ref["b64"].get<std::string>(), rb) || rb.empty()) continue;
            media::ReferenceBuffer rbuf;
            rbuf.mime  = (ref.contains("mime") && ref["mime"].is_string())
                            ? ref["mime"].get<std::string>() : "image/png";
            rbuf.bytes = std::move(rb);
            refs.push_back(std::move(rbuf));
        }
    }

    auto r = media::create_buffer(opt, refs);
    if (!r.ok) return make_error(r.error);
    auto env = make_binary_envelope(r.mime, r.bytes);
    if (!r.ai_text.empty()) env.envelope["ai_text"] = r.ai_text;
    return env;
}

ExecuteResult do_meta(const nlohmann::json& args) {
    std::string in, mime, err;
    if (!extract_image(args, in, mime, err)) return make_error(err);
    const nlohmann::json& tool_opts = options_or_empty(args);
    media::MetaOptions opt;
    media::apply_meta_options_from_json(tool_opts, opt);
    opt.provider.clear();
    opt.model.clear();
    opt.api_key.clear();
    opt.base_url.clear();
    apply_chat_image_recognition_defaults_for_tool_options(opt.provider, opt.model, nlohmann::json::object());
    repair_meta_google_model_if_replicate_slug(opt.provider, opt.model);
    if (!opt.dry_run) {
        ensure_api_key_for_image_provider(opt.provider, opt.api_key);
        ensure_base_url_for_image_provider(opt.provider, opt.base_url);
    }
    if (opt.provider.empty() || opt.model.empty())
        return make_error("image_meta: missing provider/model: set Chat -> Image recognition provider/model in Settings");
    // Buffer variant always produces JSON inline; suppress disk outputs.
    opt.out_md = false;
    opt.out_json = false;
    opt.update_exif = false;

    auto r = media::meta_extract_buffer(in.data(), in.size(),
                                        mime.empty() ? "image/jpeg" : mime, opt);
    if (!r.ok) return make_error(r.error);
    nlohmann::json parsed = media::json::try_parse(r.json_text);
    if (parsed.is_discarded()) return make_json_envelope({{"meta_text", r.json_text}});
    return make_json_envelope(std::move(parsed));
}

ExecuteResult do_find(const nlohmann::json& args) {
    if (!args.contains("inputs") || !args["inputs"].is_array() || args["inputs"].empty())
        return make_error("'inputs' array (paths/globs) is required");

    std::vector<std::string> inputs;
    for (const auto& v : args["inputs"]) if (v.is_string()) inputs.push_back(v.get<std::string>());
    if (inputs.empty()) return make_error("'inputs' contains no string entries");

    const nlohmann::json& o = options_or_empty(args);
    media::FindOptions     opt;
    media::apply_find_options_from_json(o, opt);
    if (!o.contains("mode") || o["mode"].is_null()
        || (o["mode"].is_string() && o["mode"].get<std::string>().empty()))
        opt.mode = media::FindMode::Llm;
    if (opt.mode == media::FindMode::Llm) {
        opt.find_semantic_judge = false;  // tool: local match + optional auto meta, no LLM judge
        opt.generate = true;  // same as path `image_find`: auto-gen missing sidecars when needed
    }
    opt.meta.provider.clear();
    opt.meta.model.clear();
    opt.meta.api_key.clear();
    opt.meta.base_url.clear();
    apply_chat_image_recognition_defaults_for_tool_options(
        opt.meta.provider, opt.meta.model, nlohmann::json::object());
    const bool prefill_find_provider = opt.mode == media::FindMode::Llm && !opt.dry_run
        && (opt.find_semantic_judge || !opt.reference_images.empty() || opt.generate);
    if (prefill_find_provider) {
        ensure_api_key_for_image_provider(opt.meta.provider, opt.meta.api_key);
        ensure_base_url_for_image_provider(opt.meta.provider, opt.meta.base_url);
    }

    auto r = media::find_images(inputs, opt);
    if (!r.ok) return make_error(r.error);

    nlohmann::json matches = nlohmann::json::array();
    for (const auto& m : r.matches) {
        matches.push_back({{"path", m.path}, {"score", m.score},
                           {"source", m.source}, {"reason", m.reason}});
    }
    return make_json_envelope({
        {"matches",    std::move(matches)},
        {"scanned",    r.scanned},
        {"considered", r.considered},
        {"generated",  r.generated},
        {"cache_hits", r.cache_hits},
    });
}

// ── file_read (small UTF-8 text only) ─────────────────────────────────────

constexpr std::uintmax_t kFileReadMaxBytes = pm::llm::k_file_read_window_bytes;


bool ext_blocked_for_file_read(const fs::path& p) {
    std::string ext = media::path::path_to_utf8(p.extension());
    ext = media::strutil::lower_ascii(ext);
    return glob_file_extension_blocked_like_file_read(ext);
}

bool buffer_looks_binary(const std::string& buf) {
    if (buf.find('\0') != std::string::npos) return true;
    const std::size_t scan = std::min(buf.size(), static_cast<std::size_t>(65536));
    std::size_t ctrl = 0;
    for (std::size_t i = 0; i < scan; ++i) {
        const auto c = static_cast<unsigned char>(buf[i]);
        if (c < 32 && c != '\t' && c != '\n' && c != '\r') ++ctrl;
    }
    return scan > 8000 && ctrl * 10 > scan;
}

ExecuteResult do_file_read(const nlohmann::json& args) {
    if (!args.contains("path") || !args["path"].is_string())
        return make_error("'path' (string) is required");
    std::string raw = args["path"].get<std::string>();
    if (raw.empty()) return make_error("path is empty");

    std::error_code ec;
    fs::path p = media::path::path_from_utf8(raw).lexically_normal();
    fs::path abs = fs::absolute(p, ec);
    if (ec) return make_error("could not resolve path: " + ec.message());

    if (!fs::exists(abs, ec))
        return make_error("path does not exist");
    if (!fs::is_regular_file(abs, ec))
        return make_error("not a regular file");

    fs::path resolved = fs::weakly_canonical(abs, ec);
    if (!ec && !resolved.empty()) abs = std::move(resolved);

    if (const std::string deny = llm_fs_guard_deny_reason(abs); !deny.empty())
        return make_error(deny);
    if (ext_blocked_for_file_read(abs))
        return make_error("refusing binary or image extension (use image_* for images)");

    const std::uintmax_t sz = fs::file_size(abs, ec);
    if (ec) return make_error("could not read file size: " + ec.message());
    if (sz > kFileReadMaxBytes)
        return make_error("file exceeds 96 KiB limit");

    std::ifstream in(abs, std::ios::binary);
    if (!in) return make_error("could not open file for reading");

    std::string buf;
    buf.assign(static_cast<std::size_t>(sz), '\0');
    if (sz > 0) {
        in.read(buf.data(), static_cast<std::streamsize>(sz));
        if (!in || static_cast<std::uintmax_t>(in.gcount()) != sz)
            return make_error("could not read full file");
    }

    if (buffer_looks_binary(buf))
        return make_error("refusing binary content");

    return make_json_envelope({
        {"path", media::path::path_to_generic_utf8(abs)},
        {"bytes", sz},
        {"text", std::move(buf)},
    });
}

} // namespace

ExecuteResult execute(const std::string& name, const nlohmann::json& arguments) {
    auto log_done = [&name](ExecuteResult r) -> ExecuteResult {
        if (r.ok)
            pm::log::debug_lazy("llm_tools", [&] { return std::string("tool=") + name + " ok"; });
        else
            pm::log::warn_lazy("llm_tools", [&] { return std::string("tool=") + name + " failed: " + r.error; });
        return r;
    };

    try {
        pm::log::debug_lazy("llm_tools", [&] { return std::string("invoke tool=") + name; });
        if (name == "image_resize")    return log_done(do_resize(arguments));
        if (name == "image_compress")  return log_done(do_compress(arguments));
        if (name == "image_transform") return log_done(do_transform(arguments));
        if (name == "image_create")    return log_done(do_image_create(arguments));
        if (name == "image_meta")      return log_done(do_meta(arguments));
        if (name == "image_find")      return log_done(do_find(arguments));
        if (name == "file_read")      return log_done(do_file_read(arguments));
        // Forward to self-contained RunTool so `llm tools-call --name run` works.
        if (name == "run") {
            auto pr = media::llm::run::execute(arguments, std::string{});
            ExecuteResult r;
            r.ok = pr.ok;
            r.error = pr.error;
            r.envelope = pr.envelope;
            return log_done(std::move(r));
        }
        pm::log::warn_lazy("llm_tools", [&] { return std::string("unknown tool name=") + name; });
        return make_error("unknown tool: " + name);
    } catch (const std::exception& e) {
        pm::log::error_lazy("llm_tools", [&] { return std::string("tool=") + name + " exception: " + e.what(); });
        return make_error(std::string("tool '") + name + "' exception: " + e.what());
    } catch (...) {
        pm::log::error_lazy("llm_tools", [&] { return std::string("tool=") + name + " exception: unknown"; });
        return make_error("tool '" + name + "' exception: unknown");
    }
}

} // namespace media::llm

// Copyright PolyMech / pm-image. Internal use only.
#include "llm/tools/pictures/PictureTools.hpp"
#include "json/json_utils.hpp"

#include "core/cli_cancel.hpp"
#include "core/compress.hpp"
#include "core/meta.hpp"
#include "fs/output_path.hpp"
#include "fs/path_sanitizer.hpp"
#include "core/resize.hpp"
#include "core/transform.hpp"
#include "core/settings_runtime.hpp"
#include "llm/llm_image_tool_defaults.hpp"
#include "llm/llama_vlm_client.hpp"

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO
#include "core/video.hpp"
#if defined(_WIN32)
#include <objbase.h>
#endif
#endif

#include "logging/pm_log.hpp"
#include "logging/logger.h"
#if defined(__ANDROID__)
#include "pm_android_host.hpp"
#include <android/log.h>
#include <fstream>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>

namespace media::llm::path::detail {

// ── image_transform per-file progress hook ────────────────────────────────
namespace {
thread_local std::function<void(const nlohmann::json&)> tls_image_transform_file_progress;
} // namespace

void set_image_transform_file_progress(std::function<void(const nlohmann::json&)> f) {
    tls_image_transform_file_progress = std::move(f);
}

void clear_image_transform_file_progress() {
    tls_image_transform_file_progress = {};
}

namespace {

bool resolve_picture_outputs(const nlohmann::json& args, const std::vector<std::string>& paths,
                             const char* tool, const std::string& format, const std::string& stem_suffix,
                             std::string& err, std::vector<std::string>& out_paths)
{
    const std::string out_dir = options_or_empty(args).value("out_dir", std::string{});
    const std::optional<std::string> explicit_output_path = get_output_path_argument(args);
    const std::vector<std::string> explicit_output_paths = get_string_array(args, "output_paths");
    if (explicit_output_path.has_value() && !explicit_output_paths.empty()) {
        err = std::string(tool) + ": pass either output_path or output_paths, not both";
        return false;
    }
    if (explicit_output_path.has_value() && paths.size() != 1) {
        err = std::string(tool) + ": output_path is only valid with a single input; use output_paths for multiple inputs";
        return false;
    }
    if (!explicit_output_paths.empty() && explicit_output_paths.size() != paths.size()) {
        err = std::string(tool) + ": output_paths length must match paths length";
        return false;
    }
    out_paths.resize(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        if (explicit_output_path.has_value()) {
            out_paths[i] = resolve_tool_path_string(*explicit_output_path);
            continue;
        }
        if (!explicit_output_paths.empty()) {
            out_paths[i] = resolve_tool_path_string(explicit_output_paths[i]);
            continue;
        }
        const std::string in_abs = resolve_tool_path_string(paths[i]);
        std::string infer_err;
        std::string inferred = media::default_output_path_for_one_input(in_abs, format, infer_err, stem_suffix);
        if (inferred.empty()) {
            err = infer_err.empty() ? (std::string(tool) + ": could not infer output path") : infer_err;
            return false;
        }
        if (!out_dir.empty())
            inferred = media::path::path_to_utf8(
                media::path::path_from_utf8(out_dir) /
                media::path::path_from_utf8(inferred).filename());
        out_paths[i] = inferred;
    }
    return true;
}

} // namespace

// ── do_resize ─────────────────────────────────────────────────────────────
ExecuteResult do_resize(const nlohmann::json& args) {
    auto paths = get_string_array(args, "paths");
    if (paths.empty()) return fatal("image_resize: 'paths' (string array) is required and non-empty");

    media::ResizeOptions opt;
    media::apply_resize_options_from_json(options_or_empty(args), opt);
    opt.cache_enabled = false;
    const bool has_size = opt.max_width > 0 || opt.max_height > 0 || !opt.aspect.empty();
    const bool has_geom = opt.rotate != 0 || opt.flip || opt.flop || opt.auto_rotate;
    if (!has_size && !has_geom && opt.format.empty())
        return fatal("image_resize: set options.max_width/max_height, options.format, or rotate/flip/flop");

    std::string out_err;
    std::vector<std::string> out_paths;
    if (!resolve_picture_outputs(args, paths, "image_resize", opt.format, opt.output_stem_suffix, out_err, out_paths))
        return fatal(out_err);

    std::vector<PerFileResult> out;
    out.reserve(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        media::cli::yield_to_interrupt();
        if (media::cli::cancel_requested()) {
            for (size_t j = i; j < paths.size(); ++j) {
                PerFileResult r2;
                r2.path  = resolve_tool_path_string(paths[j]);
                r2.ok    = false;
                r2.error = "interrupted (Ctrl+C)";
                out.push_back(std::move(r2));
            }
            return build_envelope("image_resize", std::move(out));
        }
        const std::string in_abs = resolve_tool_path_string(paths[i]);
        PerFileResult r; r.path = in_abs;
        if (const std::string wIn = llm_path_fs_deny(in_abs); !wIn.empty()) {
            r.ok = false;
            r.error = wIn;
            out.push_back(std::move(r));
            continue;
        }
        const std::string out_path = out_paths[i];
        if (const std::string wOut = llm_path_fs_deny(out_path); !wOut.empty()) {
            r.ok = false;
            r.error = wOut;
            out.push_back(std::move(r));
            continue;
        }
        if (const std::string wW = llm_path_write_deny(out_path); !wW.empty()) {
            r.ok = false;
            r.error = wW;
            out.push_back(std::move(r));
            continue;
        }
        std::string err;
        if (media::resize_file(in_abs, out_path, opt, err)) {
            r.ok = true;
            r.output_path = out_path;
        } else {
            r.error = err.empty() ? "resize_file failed" : err;
        }
        out.push_back(std::move(r));
    }
    return build_envelope("image_resize", std::move(out));
}

// ── do_crop ───────────────────────────────────────────────────────────────
ExecuteResult do_crop(const nlohmann::json& args) {
    auto paths = get_string_array(args, "paths");
    if (paths.empty()) return fatal("image_crop: 'paths' (string array) is required and non-empty");

    media::CropOptions opt;
    media::apply_crop_options_from_json(options_or_empty(args), opt);
    if (opt.width <= 0 || opt.height <= 0)
        return fatal("image_crop: options.width and options.height are required");

    std::string out_err;
    std::vector<std::string> out_paths;
    if (!resolve_picture_outputs(args, paths, "image_crop", opt.format, {}, out_err, out_paths))
        return fatal(out_err);

    std::vector<PerFileResult> out;
    out.reserve(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        media::cli::yield_to_interrupt();
        if (media::cli::cancel_requested()) {
            for (size_t j = i; j < paths.size(); ++j) {
                PerFileResult r2;
                r2.path  = resolve_tool_path_string(paths[j]);
                r2.ok    = false;
                r2.error = "interrupted (Ctrl+C)";
                out.push_back(std::move(r2));
            }
            return build_envelope("image_crop", std::move(out));
        }
        const std::string in_abs = resolve_tool_path_string(paths[i]);
        PerFileResult r; r.path = in_abs;
        if (const std::string wIn = llm_path_fs_deny(in_abs); !wIn.empty()) {
            r.ok = false;
            r.error = wIn;
            out.push_back(std::move(r));
            continue;
        }
        const std::string out_path = out_paths[i];
        if (const std::string wOut = llm_path_fs_deny(out_path); !wOut.empty()) {
            r.ok = false;
            r.error = wOut;
            out.push_back(std::move(r));
            continue;
        }
        if (const std::string wW = llm_path_write_deny(out_path); !wW.empty()) {
            r.ok = false;
            r.error = wW;
            out.push_back(std::move(r));
            continue;
        }
        std::string err;
        if (media::crop_file(in_abs, out_path, opt, err)) {
            r.ok = true;
            r.output_path = out_path;
        } else {
            r.error = err.empty() ? "crop_file failed" : err;
        }
        out.push_back(std::move(r));
    }
    return build_envelope("image_crop", std::move(out));
}

// ── do_compress ───────────────────────────────────────────────────────────
ExecuteResult do_compress(const nlohmann::json& args) {
    auto paths = get_string_array(args, "paths");
    if (paths.empty()) return fatal("image_compress: 'paths' (string array) is required and non-empty");

    media::CompressOptions opt;
    media::apply_compress_options_from_json(options_or_empty(args), opt);
    const std::string out_dir = options_or_empty(args).value("out_dir", std::string{});

    std::vector<PerFileResult> out;
    out.reserve(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        media::cli::yield_to_interrupt();
        if (media::cli::cancel_requested()) {
            for (size_t j = i; j < paths.size(); ++j) {
                PerFileResult r2;
                r2.path  = resolve_tool_path_string(paths[j]);
                r2.ok    = false;
                r2.error = "interrupted (Ctrl+C)";
                out.push_back(std::move(r2));
            }
            return build_envelope("image_compress", std::move(out));
        }
        const std::string& in  = paths[i];
        const std::string in_abs = resolve_tool_path_string(in);
        PerFileResult r; r.path = in_abs;
        if (const std::string wIn = llm_path_fs_deny(in_abs); !wIn.empty()) {
            r.ok = false;
            r.error = wIn;
            out.push_back(std::move(r));
            continue;
        }
        const std::string out_path = media::compress_default_output(in_abs, out_dir, opt);
        if (const std::string wOut = llm_path_fs_deny(out_path); !wOut.empty()) {
            r.ok = false;
            r.error = wOut;
            out.push_back(std::move(r));
            continue;
        }
        if (const std::string wW = llm_path_write_deny(out_path); !wW.empty()) {
            r.ok = false;
            r.error = wW;
            out.push_back(std::move(r));
            continue;
        }
        const std::string err = media::compress_file(in_abs, out_path, opt);
        if (err.empty()) {
            r.ok = true;
            r.output_path = out_path;
        } else {
            r.error = err;
        }
        out.push_back(std::move(r));
    }
    return build_envelope("image_compress", std::move(out));
}

// ── do_transform ──────────────────────────────────────────────────────────
ExecuteResult do_transform(const nlohmann::json& args) {
    auto paths = get_string_array(args, "paths");
    if (paths.empty()) return fatal("image_transform: 'paths' (string array) is required and non-empty");

    const nlohmann::json& tool_options = options_or_empty(args);
    media::TransformOptions opt;
    media::apply_transform_options_from_json(tool_options, opt);
    opt.provider.clear();
    opt.model.clear();
    opt.api_key.clear();
    opt.base_url.clear();
    apply_chat_image_defaults_for_tool_options(opt.provider, opt.model, nlohmann::json::object());
    ensure_api_key_for_image_provider(opt.provider, opt.api_key);
    ensure_base_url_for_image_provider(opt.provider, opt.base_url);
    logger::info(std::string("image_transform: resolved provider=") + opt.provider + " model=" + opt.model
                 + " (see `llm info` image_tools_resolved; settings.json reloads on file mtime change)");
    if (opt.prompt.empty()) return fatal("image_transform: options.prompt is required");
    if (opt.provider.empty() || opt.model.empty()) {
        return fatal("image_transform: missing provider/model: set Chat -> Image provider/model in Settings, "
                     "or pass explicit tool options.provider/options.model");
    }

    const std::string out_dir = options_or_empty(args).value("out_dir", std::string{});
    const std::optional<std::string> explicit_output_path = get_output_path_argument(args);
    const std::vector<std::string> explicit_output_paths = get_string_array(args, "output_paths");
    if (explicit_output_path.has_value() && !explicit_output_paths.empty()) {
        return fatal("image_transform: pass either output_path or output_paths, not both");
    }
    if (explicit_output_path.has_value() && paths.size() != 1) {
        return fatal("image_transform: output_path is only valid with a single input; use output_paths for multiple inputs");
    }
    if (!explicit_output_paths.empty()) {
        if (explicit_output_paths.size() != paths.size()) {
            return fatal("image_transform: output_paths length must match paths length");
        }
        for (const auto& p : explicit_output_paths) {
            if (p.empty())
                return fatal("image_transform: output_paths entries must be non-empty strings");
        }
    }

    std::vector<PerFileResult> out;
    out.reserve(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        media::cli::yield_to_interrupt();
        if (media::cli::cancel_requested()) {
            for (size_t j = i; j < paths.size(); ++j) {
                PerFileResult r2;
                r2.path  = resolve_tool_path_string(paths[j]);
                r2.ok    = false;
                r2.error = "interrupted (Ctrl+C)";
                out.push_back(std::move(r2));
            }
            return build_envelope("image_transform", std::move(out));
        }
        const std::string& in  = paths[i];
        const std::string in_abs = resolve_tool_path_string(in);
        PerFileResult r; r.path = in_abs;
        if (const std::string wIn = llm_path_fs_deny(in_abs); !wIn.empty()) {
            r.ok = false;
            r.error = wIn;
            out.push_back(std::move(r));
            continue;
        }
        std::string out_path;
        if (explicit_output_path.has_value()) {
            out_path = resolve_tool_path_string(*explicit_output_path);
        } else if (!explicit_output_paths.empty()) {
            out_path = resolve_tool_path_string(explicit_output_paths[i]);
        } else {
            out_path = media::default_transform_output(in_abs, opt.prompt);
        }
        if (!explicit_output_path.has_value() && explicit_output_paths.empty() && !out_dir.empty()) {
            fs::path dir = media::path::path_from_utf8(out_dir);
            std::error_code ec; fs::create_directories(dir, ec);
            out_path = media::path::path_to_utf8(
                dir / media::path::path_from_utf8(out_path).filename());
        }
        if (const std::string wOut = llm_path_fs_deny(out_path); !wOut.empty()) {
            r.ok = false;
            r.error = wOut;
            out.push_back(std::move(r));
            continue;
        }
        if (const std::string wW = llm_path_write_deny(out_path); !wW.empty()) {
            r.ok = false;
            r.error = wW;
            out.push_back(std::move(r));
            continue;
        }
        auto tr = media::transform_image(in_abs, out_path, opt);
        if (tr.ok) {
            r.ok = true;
            r.output_path = tr.output_path.empty() ? out_path : tr.output_path;
            if (!tr.ai_text.empty()) r.result = nlohmann::json{{"ai_text", tr.ai_text}};
        } else {
            r.error = tr.error;
        }
        if (tls_image_transform_file_progress) {
            std::vector<PerFileResult> one;
            one.push_back(r);
            tls_image_transform_file_progress(
                build_envelope("image_transform", std::move(one)).envelope);
        }
        out.push_back(std::move(r));
    }
    return build_envelope("image_transform", std::move(out));
}

// ── do_create_image ───────────────────────────────────────────────────────
ExecuteResult do_create_image(const nlohmann::json& args) {
    const nlohmann::json& tool_options = options_or_empty(args);
    media::TransformOptions opt;
    media::apply_transform_options_from_json(tool_options, opt);
    opt.provider.clear();
    opt.model.clear();
    opt.api_key.clear();
    opt.base_url.clear();
    nlohmann::json defaults_src = tool_options;
    defaults_src.erase("provider");
    defaults_src.erase("model");
    defaults_src.erase("api_key");
    apply_chat_image_defaults_for_tool_options(opt.provider, opt.model, defaults_src);
    media::runtime_settings::merge_provider_credentials(
        opt.provider, false, opt.api_key, opt.base_url);
    ensure_api_key_for_image_provider(opt.provider, opt.api_key);
    ensure_base_url_for_image_provider(opt.provider, opt.base_url);
    if (opt.prompt.empty()) return fatal("image_create: options.prompt is required");
    if (opt.provider.empty() || opt.model.empty()) {
        return fatal("image_create: no provider/model configured: set Chat -> Image provider/model in Settings");
    }

    media::cli::yield_to_interrupt();
    if (media::cli::cancel_requested()) return fatal("image_create: interrupted (Ctrl+C)");

    std::string out_path;
    const std::optional<std::string> explicit_output_path = get_output_path_argument(args);
    if (explicit_output_path.has_value()) {
        out_path = resolve_tool_path_string(*explicit_output_path);
    }
    if (out_path.empty()) out_path = media::default_create_image_output(opt.prompt);

    const std::string out_dir = tool_options.value("out_dir", std::string{});
    if (!explicit_output_path.has_value() && !out_dir.empty()) {
        fs::path dir = media::path::path_from_utf8(out_dir);
        std::error_code ec; fs::create_directories(dir, ec);
        out_path = media::path::path_to_utf8(
            dir / media::path::path_from_utf8(out_path).filename());
    }

    if (const std::string wOut = llm_path_fs_deny(out_path); !wOut.empty()) {
        PerFileResult r;
        r.path = out_path;
        r.ok = false;
        r.error = wOut;
        std::vector<PerFileResult> out;
        out.push_back(std::move(r));
        return build_envelope("image_create", std::move(out));
    }
    if (const std::string wW = llm_path_write_deny(out_path); !wW.empty()) {
        PerFileResult r;
        r.path = out_path;
        r.ok = false;
        r.error = wW;
        std::vector<PerFileResult> out;
        out.push_back(std::move(r));
        return build_envelope("image_create", std::move(out));
    }

    const std::string rpath = out_path;
    auto tr = media::create_image(out_path, opt);
    std::vector<PerFileResult> out;
    PerFileResult r;
    r.path = rpath;
    if (tr.ok) {
        r.ok = true;
        r.output_path = tr.output_path.empty() ? rpath : tr.output_path;
        if (!tr.ai_text.empty()) r.result = nlohmann::json{{"ai_text", tr.ai_text}};
    } else {
        r.error = tr.error;
    }
    out.push_back(std::move(r));
    return build_envelope("image_create", std::move(out));
}

// ── do_meta ───────────────────────────────────────────────────────────────
ExecuteResult do_meta(const nlohmann::json& args) {
    auto paths = get_string_array(args, "paths");
    if (paths.empty()) return fatal("image_meta: 'paths' (string array) is required and non-empty");

    const nlohmann::json& tool_options = options_or_empty(args);
    media::MetaOptions     opt;
    media::apply_meta_options_from_json(tool_options, opt);
    opt.provider.clear();
    opt.model.clear();
    opt.api_key.clear();
    opt.base_url.clear();
    apply_chat_image_recognition_defaults_for_tool_options(opt.provider, opt.model, nlohmann::json::object());
    repair_meta_google_model_if_replicate_slug(opt.provider, opt.model);
    if (opt.provider.empty() || opt.model.empty()) {
        return fatal("image_meta: missing provider/model: set Chat -> Image recognition provider/model in Settings, "
                     "or pass explicit tool options.provider/options.model");
    }

    std::vector<PerFileResult> out;
    out.reserve(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        media::cli::yield_to_interrupt();
        if (media::cli::cancel_requested()) {
            for (size_t j = i; j < paths.size(); ++j) {
                PerFileResult r2;
                r2.path  = resolve_tool_path_string(paths[j]);
                r2.ok    = false;
                r2.error = "interrupted (Ctrl+C)";
                out.push_back(std::move(r2));
            }
            return build_envelope("image_meta", std::move(out));
        }
        const std::string&  in     = paths[i];
        const std::string   in_abs = resolve_tool_path_string(in);
        PerFileResult       r;
        r.path = in_abs;
        if (const std::string wIn = llm_path_fs_deny(in_abs); !wIn.empty()) {
            r.ok = false;
            r.error = wIn;
            out.push_back(std::move(r));
            continue;
        }

        if (try_image_meta_from_sidecar(in_abs, opt, tool_options, r)) {
            out.push_back(std::move(r));
            continue;
        }

        if (!opt.dry_run) {
            ensure_api_key_for_image_provider(opt.provider, opt.api_key);
            ensure_base_url_for_image_provider(opt.provider, opt.base_url);
        }
        auto mr = media::meta_extract(in_abs, opt);
        if (mr.ok) {
            r.ok = true;
            if (!mr.json_path.empty())     r.output_path = mr.json_path;
            else if (!mr.md_path.empty())  r.output_path = mr.md_path;
            auto parsed = media::json::try_parse(mr.json_text);
            if (!parsed.is_discarded()) {
                r.result = std::move(parsed);
            } else if (!mr.json_text.empty()) {
                r.result = nlohmann::json{{"meta_text", mr.json_text}};
            }
        } else {
            r.error = mr.error;
        }
        out.push_back(std::move(r));
    }
    return build_envelope("image_meta", std::move(out));
}

// ── do_understand ─────────────────────────────────────────────────────────
ExecuteResult do_understand(const nlohmann::json& args) {
    auto paths = get_string_array(args, "paths");
    if (paths.empty())
        return fatal("image_understand: 'paths' (string array) is required and non-empty");

    const std::string prompt = args.contains("prompt") && args["prompt"].is_string()
                               ? args["prompt"].get<std::string>() : std::string{};
    if (prompt.empty())
        return fatal("image_understand: 'prompt' (string) is required");

#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "pm-chat", "image_understand paths=%zu first=%s",
                        paths.size(), paths.front().c_str());
#endif

    const nlohmann::json& tool_options = options_or_empty(args);

    media::MetaOptions opt;
    apply_chat_image_recognition_defaults_for_tool_options(opt.provider, opt.model,
                                                           nlohmann::json::object());
    repair_meta_google_model_if_replicate_slug(opt.provider, opt.model);
    if (opt.provider.empty() || opt.model.empty())
        return fatal("image_understand: no recognition provider configured — "
                     "set Chat \u2192 Image recognition provider/model in Settings");

    // ── Local VLM dispatch (provider = "llama") ───────────────────────────────
    if (opt.provider == "llama") {
        std::vector<std::string> abs_paths;
        abs_paths.reserve(paths.size());
        for (const auto& p : paths) {
            const std::string ap = resolve_tool_path_string(p);
            if (const std::string w = llm_path_fs_deny(ap); !w.empty())
                return fatal("image_understand: " + w);
            abs_paths.push_back(ap);
        }

        media::llm::vlm_client::VlmRequest req;
        req.image_paths  = std::move(abs_paths);
        req.prompt       = prompt;
        req.model_id     = opt.model;
        req.slot         = "vision";
        req.cancel_check = []{ return media::cli::cancel_requested(); };

        const auto res = media::llm::vlm_client::execute(req);

        // Multi-frame models return one answer for all images together.
        // Build one PerFileResult per path so the envelope shape is identical
        // to the cloud path (build_envelope → {ok, tool, summary, results[…]}).
        std::vector<PerFileResult> out;
        out.reserve(req.image_paths.size());
        for (const auto& img : req.image_paths) {
            PerFileResult r;
            r.path = img;
            r.ok   = res.ok;
            if (res.ok) {
                r.result["answer"]      = res.text;
                r.result["provider"]    = "llama";
                r.result["model"]       = res.model_id.empty() ? req.model_id : res.model_id;
                r.result["elapsed_ms"]  = res.elapsed_ms;
                r.result["n_gpu_layers"]= res.n_gpu_layers;
            } else {
                r.error = res.error;
            }
            out.push_back(std::move(r));
        }
        return build_envelope("image_understand", std::move(out));
    }

    // ── Cloud dispatch ────────────────────────────────────────────────────────
    opt.out_md      = false;
    opt.out_json    = false;
    opt.update_exif = false;
    opt.prompt      = prompt;

    opt.resize_first = tool_options.value("resize_first", true);
    opt.resize_width = tool_options.value("resize_width", 512);

    if (!opt.dry_run) {
        ensure_api_key_for_image_provider(opt.provider, opt.api_key);
        ensure_base_url_for_image_provider(opt.provider, opt.base_url);
    }

    std::vector<PerFileResult> out;
    out.reserve(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        media::cli::yield_to_interrupt();
        if (media::cli::cancel_requested()) {
            for (size_t j = i; j < paths.size(); ++j) {
                PerFileResult r2;
                r2.path  = resolve_tool_path_string(paths[j]);
                r2.ok    = false;
                r2.error = "interrupted (Ctrl+C)";
                out.push_back(std::move(r2));
            }
            return build_envelope("image_understand", std::move(out));
        }
        const std::string in_abs = resolve_tool_path_string(paths[i]);
        PerFileResult r;
        r.path = in_abs;
        if (const std::string w = llm_path_fs_deny(in_abs); !w.empty()) {
            r.ok    = false;
            r.error = w;
            out.push_back(std::move(r));
            continue;
        }
        // Same path as CLI `understand`: plain markdown/text, not catalog JSON.
        auto ur = media::image_understand(in_abs, opt);
        r.ok = ur.ok;
        if (ur.ok) {
            nlohmann::json res = nlohmann::json::object();
            res["answer"]   = ur.text;
            res["provider"] = opt.provider;
            res["model"]    = opt.model;
            r.result = std::move(res);
        } else {
            r.error = ur.error;
#if defined(__ANDROID__)
            __android_log_print(ANDROID_LOG_ERROR, "pm-chat", "image_understand fail: %s",
                                ur.error.c_str());
#endif
        }
        out.push_back(std::move(r));
    }
    return build_envelope("image_understand", std::move(out));
}

// ── do_ocr ────────────────────────────────────────────────────────────────
ExecuteResult do_ocr(const nlohmann::json& args) {
    auto paths = get_string_array(args, "paths");
    if (paths.empty())
        return fatal("ocr_text: 'paths' (string array) is required and non-empty");

    // Optional caller-supplied prompt override; default to verbatim extraction.
    const std::string prompt = (args.contains("prompt") && args["prompt"].is_string() && !args["prompt"].get<std::string>().empty())
        ? args["prompt"].get<std::string>()
        : "OCR: Extract all text verbatim, preserving line breaks and spatial layout.";

    // Resolve OCR provider/model: ocr_* slot → recognition fallback → "llama" fallback.
    std::string provider;
    std::string model;
    apply_chat_ocr_defaults_for_tool_options(provider, model, nlohmann::json::object());

    // When no explicit OCR provider is configured, prefer the local VLM if available.
    if (provider.empty() && media::llm::vlm_client::is_vlm_available("ocr"))
        provider = "llama";

    if (provider.empty())
        return fatal("ocr_text: no OCR provider configured — set Chat \u2192 OCR provider/model in Settings "
                     "or add a local VLM with 'ocr' slot");

    // ── Local VLM path ────────────────────────────────────────────────────────
    if (provider == "llama") {
        std::vector<std::string> abs_paths;
        abs_paths.reserve(paths.size());
        for (const auto& p : paths) {
            const std::string ap = resolve_tool_path_string(p);
            if (const std::string w = llm_path_fs_deny(ap); !w.empty())
                return fatal("ocr_text: " + w);
            abs_paths.push_back(ap);
        }

        media::llm::vlm_client::VlmRequest req;
        req.image_paths = std::move(abs_paths);
        req.prompt      = prompt;
        req.model_id    = model;
        req.slot        = "ocr";
        req.cancel_check = []{ return media::cli::cancel_requested(); };

        const auto res = media::llm::vlm_client::execute(req);

        std::vector<PerFileResult> out;
        out.reserve(req.image_paths.size());
        for (const auto& img : req.image_paths) {
            PerFileResult r;
            r.path = img;
            r.ok   = res.ok;
            if (res.ok) {
                r.result["text"]        = res.text;
                r.result["provider"]    = "llama";
                r.result["model"]       = res.model_id.empty() ? req.model_id : res.model_id;
                r.result["elapsed_ms"]  = res.elapsed_ms;
                r.result["n_gpu_layers"]= res.n_gpu_layers;
            } else {
                r.error = res.error;
            }
            out.push_back(std::move(r));
        }
        return build_envelope("ocr_text", std::move(out));
    }

    // ── Cloud/recognition fallback path ───────────────────────────────────────
    media::MetaOptions opt;
    opt.provider = provider;
    opt.model    = model;
    opt.prompt   = prompt;
    opt.out_md      = false;
    opt.out_json    = false;
    opt.update_exif = false;
    opt.resize_first = true;
    opt.resize_width = 1024;

    repair_meta_google_model_if_replicate_slug(opt.provider, opt.model);
    ensure_api_key_for_image_provider(opt.provider, opt.api_key);
    ensure_base_url_for_image_provider(opt.provider, opt.base_url);

    std::vector<PerFileResult> out;
    out.reserve(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        media::cli::yield_to_interrupt();
        if (media::cli::cancel_requested()) {
            for (size_t j = i; j < paths.size(); ++j) {
                PerFileResult r2;
                r2.path  = resolve_tool_path_string(paths[j]);
                r2.ok    = false;
                r2.error = "interrupted (Ctrl+C)";
                out.push_back(std::move(r2));
            }
            return build_envelope("ocr_text", std::move(out));
        }
        const std::string in_abs = resolve_tool_path_string(paths[i]);
        PerFileResult r;
        r.path = in_abs;
        if (const std::string w = llm_path_fs_deny(in_abs); !w.empty()) {
            r.ok    = false;
            r.error = w;
            out.push_back(std::move(r));
            continue;
        }
        auto ur = media::image_understand(in_abs, opt);
        r.ok = ur.ok;
        if (ur.ok) {
            nlohmann::json res = nlohmann::json::object();
            res["text"]     = ur.text;
            res["provider"] = opt.provider;
            res["model"]    = opt.model;
            r.result = std::move(res);
        } else {
            r.error = ur.error;
        }
        out.push_back(std::move(r));
    }
    return build_envelope("ocr_text", std::move(out));
}

// ── do_camera ─────────────────────────────────────────────────────────────
namespace {
std::string make_unique_camera_path(const std::string& base_path) {
    std::error_code ec;
    const fs::path p = media::path::path_from_utf8(base_path);
    if (!fs::exists(p, ec)) return base_path;
    const std::string stem = media::path::path_to_utf8(p.stem());
    const std::string ext  = media::path::path_to_utf8(p.extension());
    const fs::path    dir  = p.parent_path();
    for (int n = 1; n < 10000; ++n) {
        const fs::path candidate = dir / media::path::path_from_utf8(
            stem + "_" + std::to_string(n) + ext);
        if (!fs::exists(candidate, ec)) return media::path::path_to_utf8(candidate);
    }
    return base_path;
}
} // namespace

#if defined(__ANDROID__)
ExecuteResult do_camera_phone(const nlohmann::json& args) {
    nlohmann::json req = args.is_object() ? args : nlohmann::json::object();
    const bool explicit_path = req.contains("output_path") && req["output_path"].is_string()
                               && !req["output_path"].get<std::string>().empty();
    if (explicit_path) {
        std::string p = resolve_tool_path_string(req["output_path"].get<std::string>());
        if (const std::string w = llm_path_write_deny(p); !w.empty())
            return fatal("image_from_camera: " + w);
        req["output_path"] = p;
    }
    std::string reply;
    std::string err;
    if (!pm::android::device_capture(req.dump(), reply, err))
        return fatal("image_from_camera: " + (err.empty() ? "capture failed" : err));
    const auto body = media::json::try_parse(reply);
    if (body.is_discarded() || !body.is_object())
        return fatal("image_from_camera: bad phone reply");
    if (!body.value("ok", false)) {
        const std::string why = body.value("error", std::string("capture failed"));
        return fatal(why.rfind("image_from_camera", 0) == 0 ? why : "image_from_camera: " + why);
    }
    if (body.value("action", std::string{}) == "list") {
        PerFileResult r;
        r.ok = true;
        r.path = "(camera list)";
        r.result = {{"devices", body.value("devices", nlohmann::json::array())}};
        return build_envelope("image_from_camera", {std::move(r)});
    }
    const std::string path = body.value("path", std::string{});
    if (path.empty())
        return fatal("image_from_camera: phone did not return a path");
    {
        std::ifstream in(media::path::path_from_utf8(path), std::ios::binary);
        unsigned char mag[2] = {};
        in.read(reinterpret_cast<char*>(mag), 2);
        if (in.gcount() != 2 || mag[0] != 0xFF || mag[1] != 0xD8)
            return fatal("image_from_camera: saved file is not readable at " + path);
    }
    PerFileResult r;
    r.ok = true;
    r.path = path;
    r.output_path = path;
    r.result = {
        {"captured_at", body.value("captured_at", std::string{})},
        {"device", body.value("facing", std::string("back"))},
        {"facing", body.value("facing", std::string("back"))},
        {"width", body.value("width", 0)},
        {"height", body.value("height", 0)},
        {"bytes", body.value("bytes", 0)},
        {"note", explicit_path
            ? "Pass path to image_understand."
            : "Saved in the camera roll. Pass path to image_understand."},
    };
    return build_envelope("image_from_camera", {std::move(r)});
}
#endif

ExecuteResult do_camera(const nlohmann::json& args) {
#if defined(__ANDROID__)
    return do_camera_phone(args);
#elif !defined(FEATURE_VIDEO) || !FEATURE_VIDEO
    (void)args;
    return fatal("image_from_camera: webcam capture requires FEATURE_VIDEO "
                 "(not compiled into this build)");
#else
#if defined(_WIN32)
    const HRESULT hr_com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComGuard {
        bool owned;
        ~ComGuard() { if (owned) ::CoUninitialize(); }
    } com_guard{ hr_com == S_OK || hr_com == S_FALSE };
    if (FAILED(hr_com) && hr_com != RPC_E_CHANGED_MODE)
        return fatal("image_from_camera: CoInitializeEx failed");
#endif

    const std::string action = args.value("action", std::string("capture"));

    if (action == "list") {
        auto devices = pm::video::enumerate_capture_devices();
        nlohmann::json j_devs = nlohmann::json::array();
        for (const auto& d : devices) {
            auto raw_modes = pm::video::enumerate_device_modes(d.name);
            std::vector<pm::video::DeviceMode> unique_modes;
            for (const auto& m : raw_modes) {
                auto it = std::find_if(unique_modes.begin(), unique_modes.end(),
                    [&](const pm::video::DeviceMode& u) {
                        return u.width == m.width && u.height == m.height;
                    });
                if (it == unique_modes.end()) {
                    unique_modes.push_back(m);
                } else {
                    double cur_fps = it->fps_den > 0 ? (double)it->fps_num / it->fps_den : 0.0;
                    double new_fps = m.fps_den  > 0 ? (double)m.fps_num  / m.fps_den  : 0.0;
                    if (new_fps > cur_fps) *it = m;
                }
            }
            std::sort(unique_modes.begin(), unique_modes.end(),
                [](const pm::video::DeviceMode& a, const pm::video::DeviceMode& b) {
                    return (a.width * a.height) > (b.width * b.height);
                });
            nlohmann::json j_modes = nlohmann::json::array();
            for (const auto& m : unique_modes) {
                int fps_int = m.fps_den > 0 ? (int)((double)m.fps_num / m.fps_den + 0.5) : 0;
                j_modes.push_back({{"width", m.width}, {"height", m.height}, {"fps", fps_int}});
            }
            j_devs.push_back({
                {"name",       d.name},
                {"is_default", d.is_default},
                {"modes",      j_modes},
            });
        }
        PerFileResult r;
        r.ok     = true;
        r.path   = "(camera list)";
        r.result = {
            {"devices",      j_devs},
            {"context_folder", agent_path_base_empty() ? std::string("(none)") : agent_path_base()},
        };
        return build_envelope("image_from_camera", {std::move(r)});
    }

    const std::string device_hint = args.value("device", std::string{});
    const int req_w = args.value("width",  0);
    const int req_h = args.value("height", 0);

    std::string out_path;
    const bool explicit_path = args.contains("output_path")
                               && args["output_path"].is_string()
                               && !args["output_path"].get<std::string>().empty();
    if (explicit_path) {
        out_path = make_unique_camera_path(
                       resolve_tool_path_string(args["output_path"].get<std::string>()));
    } else {
        auto now = std::chrono::system_clock::now();
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch()) % 1000;
        std::time_t tt = std::chrono::system_clock::to_time_t(now);
        std::tm lt{};
#if defined(_WIN32)
        ::localtime_s(&lt, &tt);
#else
        ::localtime_r(&tt, &lt);
#endif
        char ts_buf[32]; char ms_buf[16];
        std::strftime(ts_buf, sizeof(ts_buf), "cam_%Y%m%d_%H%M%S", &lt);
        std::snprintf(ms_buf, sizeof(ms_buf), "_%03d.jpg", static_cast<int>(ms.count()));
        const std::string fname = std::string(ts_buf) + ms_buf;
        const fs::path base_dir = agent_path_base_empty()
            ? media::runtime_settings::effective_temp_dir()
            : media::path::path_from_utf8(agent_path_base());
        out_path = make_unique_camera_path(media::path::path_to_utf8(base_dir / fname));
    }
    if (const std::string w = llm_path_write_deny(out_path); !w.empty())
        return fatal("image_from_camera: " + w);

    auto frame = pm::video::capture_still(device_hint, req_w, req_h, /*timeout_ms=*/8000);

    auto cap_now = std::chrono::system_clock::now();
    auto cap_ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                       cap_now.time_since_epoch()) % 1000;
    std::time_t cap_tt = std::chrono::system_clock::to_time_t(cap_now);
    std::tm cap_lt{};
#if defined(_WIN32)
    ::localtime_s(&cap_lt, &cap_tt);
#else
    ::localtime_r(&cap_tt, &cap_lt);
#endif
    char cap_ts[32]; char cap_ms_buf[16];
    std::strftime(cap_ts, sizeof(cap_ts), "%Y-%m-%dT%H:%M:%S", &cap_lt);
    std::snprintf(cap_ms_buf, sizeof(cap_ms_buf), ".%03d", static_cast<int>(cap_ms.count()));
    const std::string captured_at = std::string(cap_ts) + cap_ms_buf;

    std::error_code ec_mk;
    fs::create_directories(media::path::path_from_utf8(out_path).parent_path(), ec_mk);

    std::string save_err;
    if (!pm::video::save_frame(frame, out_path, save_err))
        return fatal("image_from_camera: save failed: " + save_err);

    PerFileResult r;
    r.ok          = true;
    r.path        = out_path;
    r.output_path = out_path;
    r.result = {
        {"captured_at",    captured_at},
        {"device",         device_hint.empty() ? std::string("(default)") : device_hint},
        {"width",          frame.width},
        {"height",         frame.height},
        {"context_folder", agent_path_base_empty() ? std::string("(none)") : agent_path_base()},
    };
    return build_envelope("image_from_camera", {std::move(r)});
#endif // FEATURE_VIDEO
}

// ── Catalog schemas ───────────────────────────────────────────────────────

namespace {

nlohmann::json parse_schema(const char* schema) {
    return nlohmann::json::parse(schema);
}

} // namespace

ToolDef make_resize() {
    return ToolDef{
        "image_resize",
        "Resize, re-encode, or convert format for one or more images on disk (libvips, not AI). "
        "Use for scale / downsize / fit, format convert (png→webp, jpeg→avif, heic→jpeg, …), "
        "or rotate/flip. Combine freely: convert and resize in one call. "
        "Do NOT use this for content edits (remove object, restyle) — that is image_transform. "
        "Omit output_path: inferred next to the source like `pm-image resize` "
        "(same stem; extension follows options.format; in-place only when format is unchanged). "
        "Do not invent a filename.",
        parse_schema(R"json({
            "type": "object",
            "properties": {
                "paths": {
                    "type": "array",
                    "description": "Host file paths (absolute, or relative to the current folder). When the user does not name a path, use the selected files already listed in this turn.",
                    "items": {"type": "string"},
                    "minItems": 1
                },
                "output_path": {
                    "type": "string",
                    "description": "Optional. Omit to infer next to the source (CLI resize naming). Only set if the user asked for a specific path."
                },
                "output_paths": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional. One output per input; omit to infer each name."
                },
                "options": {
                    "type": "object",
                    "properties": {
                        "max_width": {"type": "integer", "minimum": 1, "description": "Target / max width in pixels. Omit with format-only convert."},
                        "max_height": {"type": "integer", "minimum": 1, "description": "Target / max height in pixels. Omit with format-only convert. Filled from aspect when only max_width is set."},
                        "aspect": {"type": "string", "description": "Target ratio W:H (4:5, 1:1, 16:9). Sets the unset side from max_width or max_height."},
                        "auto_rotate": {"type": "boolean", "description": "Rotate 90 when image orientation does not match the target box, after EXIF autorotate."},
                        "fit": {"type": "string", "enum": ["inside", "cover", "contain", "fill", "outside"], "description": "How the image fits the box. Default inside (no crop). cover crops to fill."},
                        "position": {"type": "string", "enum": ["centre", "attention", "entropy", "low", "high"], "description": "Crop anchor when fit=cover. Default centre."},
                        "format": {"type": "string", "enum": ["jpeg", "jpg", "png", "webp", "tiff", "tif", "avif", "heic", "heif", "bmp", "gif"], "description": "Output container (libvips). Set this to convert format. Omit to keep the source format."},
                        "quality": {"type": "integer", "minimum": 1, "maximum": 100, "description": "JPEG/WebP/AVIF/HEIC quality. Default 85."},
                        "png_compression": {"type": "integer", "minimum": 0, "maximum": 9, "description": "PNG DEFLATE level. Default 6."},
                        "kernel": {"type": "string", "enum": ["nearest", "cubic", "mitchell", "lanczos2", "lanczos3"], "description": "Resample kernel. Default lanczos3."},
                        "background": {"type": "string", "description": "Letterbox colour #rrggbb when fit=contain. Default #ffffff."},
                        "rotate": {"type": "integer", "enum": [0, 90, 180, 270], "description": "Rotate after EXIF autorotate."},
                        "flip": {"type": "boolean", "description": "Vertical flip."},
                        "flop": {"type": "boolean", "description": "Horizontal flop."},
                        "without_enlargement": {"type": "boolean", "description": "Do not upscale smaller images. Default true."}
                    }
                }
            },
            "required": ["paths", "options"]
        })json")};
}

ToolDef make_crop() {
    return ToolDef{
        "image_crop",
        "Crop one or more images on disk to a pixel rectangle (libvips, not AI). "
        "Use when the user gives a region (left/top/width/height) or asks to centre-crop to a size. "
        "May also convert format in the same call via options.format. "
        "Do NOT use this for content-aware 'remove the background' — that is image_transform. "
        "Omit output_path: inferred next to the source like `pm-image resize` "
        "(same stem; extension follows options.format; in-place only when format is unchanged). "
        "Do not invent a filename.",
        parse_schema(R"json({
            "type": "object",
            "properties": {
                "paths": {
                    "type": "array",
                    "description": "Host file paths (absolute, or relative to the current folder). When the user does not name a path, use the selected files already listed in this turn.",
                    "items": {"type": "string"},
                    "minItems": 1
                },
                "output_path": {
                    "type": "string",
                    "description": "Optional. Omit to infer next to the source (CLI resize naming). Only set if the user asked for a specific path."
                },
                "output_paths": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional. One output per input; omit to infer each name."
                },
                "options": {
                    "type": "object",
                    "properties": {
                        "left": {"type": "integer", "minimum": 0, "description": "Crop origin X in pixels. Default 0, or centred when anchor=centre."},
                        "top": {"type": "integer", "minimum": 0, "description": "Crop origin Y in pixels. Default 0, or centred when anchor=centre."},
                        "width": {"type": "integer", "minimum": 1, "description": "Crop width in pixels (required)."},
                        "height": {"type": "integer", "minimum": 1, "description": "Crop height in pixels (required)."},
                        "anchor": {"type": "string", "enum": ["top_left", "centre"], "description": "When left/top are omitted, top_left (default) or centre the WxH box."},
                        "format": {"type": "string", "enum": ["jpeg", "jpg", "png", "webp", "tiff", "tif", "avif", "heic", "heif", "bmp", "gif"], "description": "Output container (libvips). Set this to convert format after the crop. Omit to keep the source format."},
                        "quality": {"type": "integer", "minimum": 1, "maximum": 100, "description": "JPEG/WebP/AVIF/HEIC quality. Default 85."}
                    },
                    "required": ["width", "height"]
                }
            },
            "required": ["paths", "options"]
        })json")};
}

ToolDef make_transform() {
    return ToolDef{
        "image_transform",
        "AI image edit over one or more images on disk. Use "
        "this for ANY action verb that changes the image content: 'enhance', "
        "'edit', 'fix', 'improve', 'clean up', 'remove X', 'add Y', "
        "'restyle', 'colourise', 'beautify', 'make brighter', 'sharpen', "
        "'denoise', 'insert', 'composite'. The user's wording (or your light clarification) goes in "
        "options.prompt. To insert/composite image A into image B: `paths`=[B] (destination) and "
        "`options.reference_images`=[A] (content / logo / screen art). Naming a file in the prompt "
        "does NOT send its pixels. Do not put both files in `paths` — that edits each independently. "
        "Do NOT call image_meta first to 'see' the image \u2014 "
        "Gemini reads the bytes itself and a recon meta call is pure overhead. "
        "Writes the edited raster next to each source unless `output_path` (single input) or `output_paths` "
        "(one per input) is provided. "
        "**Do not** use this tool for Replicate **image-to-video** (MP4 from a selected photo): it always sends "
        "`image_input` + `output_format` in the shape of an **image** edit API, which fails validation on Wan, MiniMax, "
        "Kling-style i2v slugs (\u201cimage is required\u201d / wrong fields). For video-from-selection, call **create_video** "
        "with `frame_indices` [0] or [0,1]. Provider, model, and API keys come from app Settings "
        "(Chat \u2192 Image provider / model) — do not pass them as tool arguments. "
        "Large source rasters are downscaled to a 2048px long edge before the API "
        "(same cap as CLI `pm-image transform`); omit image_size unless the user asked "
        "for a specific output size.",
        parse_schema(R"json({
            "type": "object",
            "properties": {
                "paths": {
                    "type": "array",
                    "description": "Host file paths (absolute, or relative to the current folder). When the user does not name a path, use the selected files already listed in this turn.",
                    "items": {"type": "string"},
                    "minItems": 1
                },
                "output_path": {
                    "type": "string",
                    "description": "Optional exact output file path for a single input. Relative paths resolve to the current context folder."
                },
                "output_paths": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional exact output file paths for multiple inputs; length must match paths."
                },
                "options": {
                    "type": "object",
                    "properties": {
                        "prompt": {"type": "string", "description": "Editing prompt (required, e.g. 'remove the watermark')"},
                        "aspect_ratio": {"type": "string", "enum": ["1:1", "16:9", "4:3", "3:4", "9:16", "21:9"], "description": "Optional. Omit to keep the source aspect. Do not invent a ratio."},
                        "image_size": {"type": "string", "enum": ["512", "1K", "2K", "4K"], "description": "Optional. Omit to keep the source size. Do not invent a size."},
                        "reference_images": {
                            "type": "array",
                            "items": {"type": "string"},
                            "description": "Other stills whose pixels must reach the model: content to insert/composite into the destination in `paths`, plus logo / brand / style. Required when the prompt names another image. Aliases: `references` (array), `reference` (one string)."
                        },
                        "references": {
                            "type": "array",
                            "items": {"type": "string"},
                            "description": "Alias of reference_images."
                        },
                        "reference": {"type": "string", "description": "Single-path alias of reference_images."}
                    },
                    "required": ["prompt"]
                }
            },
            "required": ["paths", "options"]
        })json")};
}

ToolDef make_image_create() {
    return ToolDef{
        "image_create",
        "AI media generation on disk from a **text prompt** (and optional reference images). "
        "Use for 'generate', 'create an image', 'draw', 'imagine' when there is no full raster file "
        "to edit as the main target. **Provider, model, and API keys come from app Settings** (Chat \u2192 Image "
        "provider / model), not from tool arguments \u2014 same as **create_video**. "
        "Replicate can output video from prompt-only flows; for **image-to-video from selected files**, "
        "use **create_video** with `frame_indices`. "
        "There is no `paths` input file; output is written from the prompt alone, or pass "
        "**reference image paths** (options.reference_images, or `references` / `reference`) for style / layout / logo. "
        "`output_path` is optional; if omitted, a default filename is chosen from the prompt. "
        "To edit an existing file in place, use image_transform instead.",
        parse_schema(R"json({
            "type": "object",
            "properties": {
                "output_path": {
                    "type": "string",
                    "description": "Optional exact output file path. Relative paths resolve to the current context folder (.OpenDesign in OpenDesign). Use uploads/backgrounds/{slug}-v{n}.png for page backgrounds or uploads/{slug}-v{n}.png for canvas images. If omitted, a default name is chosen from the prompt."
                },
                "out_dir": {
                    "type": "string",
                    "description": "Optional output directory when output_path is omitted (e.g. uploads/backgrounds)."
                },
                "options": {
                    "type": "object",
                    "properties": {
                        "prompt": {"type": "string", "description": "Generation prompt (required)"},
                        "aspect_ratio": {"type": "string", "enum": ["1:1", "16:9", "4:3", "3:4", "9:16", "21:9"], "description": "Omit unless the user named a ratio. image-generation groups reject aspect_ratio. Set only for DALL-E or GPT Image."},
                        "image_size": {"type": "string", "enum": ["512", "1K", "2K", "4K"], "description": "Omit unless the user named a size. image-generation groups reject image_size. Set only for DALL-E or GPT Image."},
                        "resize_first": {"type": "boolean"},
                        "resize_width": {"type": "integer", "minimum": 64, "maximum": 4096},
                        "preresize_raw_only": {"type": "boolean"},
                        "reference_images": {
                            "type": "array",
                            "items": {"type": "string"},
                            "description": "Optional. Host paths to images used only as visual references (style, subjects, logo, color palette) - not a 'main' file to retouch. May mix user-named paths, list_images output, or the selected files already listed in this turn. Also accepts the keys `references` (array) or `reference` (one string) in options, same as image_transform."
                        },
                        "references": {
                            "type": "array",
                            "items": {"type": "string"},
                            "description": "Alias of reference_images."
                        },
                        "reference": {"type": "string", "description": "Single-path alias of reference_images."}
                    },
                    "required": ["prompt"],
                    "additionalProperties": false
                }
            },
            "required": ["options"]
        })json")};
}

ToolDef make_understand() {
    return ToolDef{
        "image_understand",
        "Ask the vision model a question about one or more images on disk. "
        "You MUST compose `prompt` as the exact question or instruction for the model — "
        "translate the user's intent into direct, specific language "
        "(e.g. 'Describe the scene and any people visible', "
        "'Compare these two images and list the key differences', "
        "'What text is readable in this photo?', "
        "'Is there a cat in this image?'). "
        "Use for ALL content questions: 'what is this', 'describe', 'caption', "
        "'compare A and B', 'is there a person', 'read the text'. "
        "Pass ALL relevant images in one call — the model sees them together, "
        "which is essential for comparisons. "
        "Provider and model are taken from Chat settings "
        "(image_recognition_provider / image_recognition_model); "
        "do NOT pass them as options. "
        "Does NOT write sidecars or update metadata — pure query, no side-effects.",
        parse_schema(R"json({
            "type": "object",
            "properties": {
                "paths": {
                    "type": "array",
                    "description": "Host file paths (absolute, or relative to the current folder). When the user does not name a path, use the selected files already listed in this turn.",
                    "items": {"type": "string"},
                    "minItems": 1
                },
                "prompt": {
                    "type": "string",
                    "description": "Required. The exact vision question or instruction for the model. Be specific and complete: write what you want the model to answer or do with the image(s). For comparisons, mention that multiple images are provided (e.g. 'Compare image 1 and image 2: ...')."
                },
                "options": {
                    "type": "object",
                    "properties": {
                        "resize_first": {
                            "type": "boolean",
                            "description": "Pre-resize images before upload (default true). Reduces cost and latency with no meaningful quality loss for most vision questions."
                        },
                        "resize_width": {
                            "type": "integer",
                            "minimum": 64,
                            "maximum": 2048,
                            "description": "Target width when resize_first is true (default 512)."
                        }
                    }
                }
            },
            "required": ["paths", "prompt"]
        })json")};
}

ToolDef make_ocr_text() {
    return ToolDef{
        "ocr_text",
        "Extract text from one or more images using the configured OCR model (local VLM or cloud). "
        "Use when the user asks to 'read', 'transcribe', 'extract text from', 'OCR', or 'digitize' "
        "a document, receipt, screenshot, sign, label, or any image containing text. "
        "Returns verbatim text preserving line breaks and layout. "
        "Provider and model are resolved automatically from Chat \u2192 OCR settings "
        "(falls back to recognition slot, then to the local VLM 'ocr' slot when available). "
        "Pass an optional 'prompt' to override the default OCR instruction "
        "(e.g. 'Extract only the line items and totals as JSON').",
        parse_schema(R"json({
            "type": "object",
            "properties": {
                "paths": {
                    "type": "array",
                    "description": "Host file paths (absolute, or relative to the current folder). When the user does not name a path, use the selected files already listed in this turn.",
                    "items": {"type": "string"},
                    "minItems": 1
                },
                "prompt": {
                    "type": "string",
                    "description": "Optional. Override the default OCR extraction instruction. Default: extract all text verbatim. Example: 'Extract only prices and product names as JSON'."
                }
            },
            "required": ["paths"]
        })json")};
}

ToolDef make_camera() {
    return ToolDef{
        "image_from_camera",
        "Capture a still image from a camera and save it as a JPEG.\n\n"
        "On a phone the default is the back camera. Pass facing=\"front\" for the "
        "selfie camera. Omit output_path to save into the camera roll (DCIM/Camera), "
        "the same folder the camera app uses. The result path is a normal image file: "
        "pass it to image_understand to describe the photo. action=\"list\" names the "
        "front and back cameras.\n\n"
        "On desktop:\n"
        "  1. Call with action=\"list\" to discover webcams and their resolutions.\n"
        "  2. Call with action=\"capture\" (the default) to take the photo. "
        "Without an explicit output_path the file lands in the current context folder "
        "named cam_YYYYMMDD_HHMMSS_mmm.jpg; if no folder is active it goes to the "
        "system temp directory. facing is ignored; use device.\n\n"
        "On Linux, if capture looks wrong, verify the camera with the native CLI: "
        "`pm-image video info` then `pm-image video image --dst /tmp/test.jpg` "
        "(same capture stack as this tool).\n\n"
        "Always report the saved path and captured_at timestamp to the user so they can "
        "track which photo was taken when.",
        parse_schema(R"json({
            "type": "object",
            "properties": {
                "action": {
                    "type": "string",
                    "enum": ["capture", "list"],
                    "default": "capture",
                    "description": "\"list\" - return all capture devices and their available resolutions. \"capture\" (default) - take a still image and save it to disk."
                },
                "facing": {
                    "type": "string",
                    "enum": ["back", "front"],
                    "description": "Phone only. Which camera to use. Default back. Desktop webcams ignore this; use device."
                },
                "device": {
                    "type": "string",
                    "description": "Desktop: case-insensitive substring of the webcam's friendly name (e.g. \"Logitech\", \"FaceTime\"). Omit to use the default camera. On a phone, \"front\" or \"back\" selects that camera when facing is omitted."
                },
                "width": {
                    "type": "integer",
                    "minimum": 0,
                    "description": "Preferred capture width in pixels. 0 or omit = highest available resolution (recommended for photos)."
                },
                "height": {
                    "type": "integer",
                    "minimum": 0,
                    "description": "Preferred capture height in pixels. 0 or omit = highest available resolution."
                },
                "output_path": {
                    "type": "string",
                    "description": "Destination file path (.jpg, .png, or .bmp). Relative paths resolve to the current context folder (same as other tools). Omit to auto-generate a timestamped filename in the context folder (or system temp when no folder is set)."
                }
            },
            "required": []
        })json")};
}

} // namespace media::llm::path::detail

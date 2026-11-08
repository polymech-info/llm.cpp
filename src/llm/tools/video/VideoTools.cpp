// Copyright PolyMech / pm-image. Internal use only.
#include "llm/tools/video/VideoTools.hpp"

#include "core/cli_cancel.hpp"
#include "core/settings_runtime.hpp"
#include "core/transform.hpp"
#include "llm/llm_image_tool_defaults.hpp"
#include "core/pixlwiz_provider_models_cli.hpp"
#include "core/replicate_provider_models_cli.hpp"

#include <cctype>
#include <string>
#include <vector>
#include "string/string_utils.hpp"

namespace media::llm::path::detail {

namespace {

int json_int_or(const nlohmann::json& args, const char* key, int fallback) {
    if (!args.contains(key) || args[key].is_null())
        return fallback;
    if (args[key].is_number_integer())
        return args[key].get<int>();
    if (args[key].is_string()) {
        try {
            return std::stoi(args[key].get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::string json_string_or(const nlohmann::json& args, const char* key, const std::string& fallback = {}) {
    if (args.contains(key) && args[key].is_string())
        return args[key].get<std::string>();
    return fallback;
}

bool json_bool_or(const nlohmann::json& args, const char* key, bool fallback) {
    if (!args.contains(key) || args[key].is_null())
        return fallback;
    if (args[key].is_boolean())
        return args[key].get<bool>();
    return fallback;
}

std::vector<std::string> json_path_list(const nlohmann::json& args, const char* key) {
    std::vector<std::string> out;
    if (!args.contains(key) || args[key].is_null())
        return out;
    const auto& v = args[key];
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        if (!s.empty())
            out.push_back(s);
        return out;
    }
    if (v.is_array()) {
        for (const auto& item : v) {
            if (item.is_string() && !item.get<std::string>().empty())
                out.push_back(item.get<std::string>());
        }
    }
    return out;
}

void resolve_path_list(std::vector<std::string>& paths) {
    for (std::string& p : paths)
        p = resolve_tool_path_string(p);
}

std::string keyframe_path_key(std::string p) {
    p = resolve_tool_path_string(std::move(p));
    for (char& c : p) {
        if (c == '\\')
            c = '/';
        else
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return p;
}

bool keyframes_agree(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty())
        return true;
    return keyframe_path_key(a) == keyframe_path_key(b);
}

} // namespace

ExecuteResult do_create_video(const nlohmann::json& args, const std::string& tool) {
    const std::string pfx = tool + ": ";

    media::TransformOptions opt;
    media::apply_transform_options_from_json(nlohmann::json::object(), opt);
    opt.provider.clear();
    opt.model.clear();
    opt.api_key.clear();
    opt.base_url.clear();
    nlohmann::json defaults_src = nlohmann::json::object();
    apply_chat_video_defaults_for_tool_options(opt.provider, opt.model, defaults_src);
    media::runtime_settings::merge_provider_credentials(
        opt.provider, false, opt.api_key, opt.base_url);
    ensure_api_key_for_image_provider(opt.provider, opt.api_key);
    ensure_base_url_for_image_provider(opt.provider, opt.base_url);
    if (opt.provider.empty() || opt.model.empty()) {
        return fatal(pfx + "no provider/model configured: set Chat -> Video provider/model in Settings");
    }

    const std::string provider_lc = opt.provider;
    const bool tanit_video = (provider_lc == "tanit")
        && media::pixlwiz_cli::is_tanit_video_model(opt.model);

    std::string prompt = json_string_or(args, "prompt");
    if (prompt.empty())
        return fatal(pfx + "`prompt` must be a non-empty string");
    opt.prompt = prompt;

    std::string out_path;
    if (args.contains("output") && args["output"].is_string()) {
        const std::string raw = args["output"].get<std::string>();
        if (!raw.empty())
            out_path = resolve_tool_path_string(raw);
    }
    if (const std::string wOut = llm_path_fs_deny(out_path); !out_path.empty() && !wOut.empty()) {
        PerFileResult r;
        r.path = out_path;
        r.ok = false;
        r.error = wOut;
        std::vector<PerFileResult> one;
        one.push_back(std::move(r));
        return build_envelope(tool, std::move(one));
    }
    if (const std::string wW = llm_path_write_deny(out_path); !out_path.empty() && !wW.empty()) {
        PerFileResult r;
        r.path = out_path;
        r.ok = false;
        r.error = wW;
        std::vector<PerFileResult> one;
        one.push_back(std::move(r));
        return build_envelope(tool, std::move(one));
    }

    media::cli::yield_to_interrupt();
    if (media::cli::cancel_requested())
        return fatal(pfx + "interrupted (Ctrl+C)");

    opt.video_first_frame = json_string_or(args, "start_frame");
    if (opt.video_first_frame.empty())
        opt.video_first_frame = json_string_or(args, "image");
    opt.video_last_frame = json_string_or(args, "last_frame");
    if (opt.video_last_frame.empty())
        opt.video_last_frame = json_string_or(args, "lastFrame");
    {
        std::vector<std::string> src = json_path_list(args, "src");
        if (src.empty())
            src = json_path_list(args, "paths");
        if (src.size() > 2)
            return fatal(pfx + "at most two `src` stills (first frame, last frame); extra style images use `references`");
        if (!src.empty()) {
            const std::string src0 = src[0];
            const std::string src1 = src.size() > 1 ? src[1] : std::string{};
            // Video workflow pins start/end and the host merges them; models often also pass src.
            if (!opt.video_first_frame.empty() && !keyframes_agree(opt.video_first_frame, src0))
                return fatal(pfx + "start_frame and src[0] disagree");
            if (!src1.empty() && !opt.video_last_frame.empty() && !keyframes_agree(opt.video_last_frame, src1))
                return fatal(pfx + "last_frame and src[1] disagree");
            if (opt.video_first_frame.empty())
                opt.video_first_frame = src0;
            if (opt.video_last_frame.empty() && !src1.empty())
                opt.video_last_frame = src1;
        }
    }
    if (!opt.video_first_frame.empty())
        opt.video_first_frame = resolve_tool_path_string(opt.video_first_frame);
    if (!opt.video_last_frame.empty())
        opt.video_last_frame = resolve_tool_path_string(opt.video_last_frame);
    {
        std::vector<std::string> refs = json_path_list(args, "reference_images");
        if (refs.empty())
            refs = json_path_list(args, "references");
        if (refs.empty())
            refs = json_path_list(args, "reference");
        resolve_path_list(refs);
        opt.reference_images = std::move(refs);
    }

    if (tanit_video) {
        opt.video_duration = json_int_or(args, "duration", json_int_or(args, "seconds", 0));
        opt.video_resolution = json_string_or(args, "resolution");
        opt.aspect_ratio = json_string_or(args, "aspect_ratio", json_string_or(args, "aspectRatio"));
        opt.video_generate_audio = json_bool_or(args, "generate_audio", false);

        nlohmann::json extra = nlohmann::json::object();
        static const char* kPassthrough[] = {
            "personGeneration", "negativePrompt", "enhancePrompt", "conditioningScale",
        };
        for (const char* k : kPassthrough) {
            if (args.contains(k) && !args[k].is_null())
                extra[k] = args[k];
        }
        if (!extra.empty())
            opt.provider_options = std::move(extra);

        const std::string intended_path = out_path.empty()
            ? media::default_replicate_video_output_path(opt.prompt)
            : out_path;
        nlohmann::json meta = nlohmann::json::object();
        meta["intended_output_path"] = intended_path;
        meta["note"] = "Tanit video via LiteLLM POST /v1/videos. On success `output_path` is authoritative.";

        const media::TransformResult tr = media::create_video(out_path, opt, nullptr, nullptr);
        std::vector<PerFileResult> out;
        PerFileResult r;
        r.path = intended_path;
        if (tr.ok) {
            r.ok = true;
            r.output_path = tr.output_path.empty() ? intended_path : tr.output_path;
            r.path = r.output_path;
            meta["output_path"] = r.output_path;
            if (!tr.ai_text.empty())
                meta["job_id"] = tr.ai_text;
            r.result = std::move(meta);
        } else {
            r.error = tr.error;
            r.result = std::move(meta);
        }
        out.push_back(std::move(r));
        return build_envelope(tool, std::move(out));
    }

    auto replicate_slug_ok = [](const std::string& m) -> bool {
        return !m.empty() && m.find('/') != std::string::npos;
    };
    if (!replicate_slug_ok(opt.model))
        return fatal(pfx + "model `" + opt.model + "` is not a Replicate slug and is not a Tanit video alias");

    nlohmann::json flat;
    nlohmann::json required_keys;
    std::string    openapi_err;
    if (!media::replicate_cli::lookup_replicate_openapi_input_flat(opt.model, flat, required_keys, openapi_err))
        return fatal(pfx + "OpenAPI Input not available for model `" + opt.model + "`: " + openapi_err);

    nlohmann::json input = nlohmann::json::object();
    for (auto it = flat.begin(); it != flat.end(); ++it) {
        const std::string& k = it.key();
        if (!args.contains(k))
            continue;
        if (args[k].is_null())
            continue;
        input[k] = args[k];
    }

    if (!opt.video_first_frame.empty() && !input.contains("image"))
        input["image"] = opt.video_first_frame;
    if (!opt.video_last_frame.empty() && !input.contains("last_frame"))
        input["last_frame"] = opt.video_last_frame;
    if (!opt.reference_images.empty() && !input.contains("reference_images")) {
        nlohmann::json refs = nlohmann::json::array();
        for (const auto& p : opt.reference_images)
            refs.push_back(p);
        input["reference_images"] = std::move(refs);
    }

    for (const auto& r : required_keys) {
        if (!r.is_string())
            continue;
        const std::string rk = r.get<std::string>();
        if (!input.contains(rk))
            return fatal(pfx + "missing required OpenAPI Input field `" + rk + "`");
    }

    if (input.contains("prompt") && input["prompt"].is_string())
        opt.prompt = input["prompt"].get<std::string>();
    if (opt.prompt.empty())
        return fatal(pfx + "`prompt` must be a non-empty string in the tool arguments (Replicate Input)");

    resolve_tool_paths_strings_in_json(input);

    const std::string intended_path = media::replicate_video_intended_output_path(out_path, input, opt.prompt);
    nlohmann::json      meta          = nlohmann::json::object();
    meta["intended_output_path"]     = intended_path;
    meta["note"] =
        "If Replicate returns a different video container, the written file may use another suffix than a `.png` "
        "placeholder; on success `output_path` is authoritative.";

    const std::string          rpath = out_path;
    const media::TransformResult tr =
        media::replicate_video_replicate_input(std::move(input), out_path, opt, nullptr);
    std::vector<PerFileResult> out;
    PerFileResult              r;
    r.path = intended_path;
    if (tr.ok) {
        r.ok          = true;
        r.output_path = tr.output_path.empty() ? rpath : tr.output_path;
        r.path        = r.output_path;
        meta["output_path"] = r.output_path;
        if (!tr.ai_text.empty()) meta["ai_text"] = tr.ai_text;
        if (!tr.replicate_prediction_web_url.empty())
            meta["replicate_web_url"] = tr.replicate_prediction_web_url;
        r.result = std::move(meta);
    } else {
        r.error  = tr.error;
        if (!tr.replicate_prediction_web_url.empty())
            meta["replicate_web_url"] = tr.replicate_prediction_web_url;
        r.result = std::move(meta);
    }
    out.push_back(std::move(r));
    return build_envelope(tool, std::move(out));
}

// ── Catalog schema ────────────────────────────────────────────────────────

namespace {

nlohmann::json create_video_fallback_input_schema() {
    nlohmann::json props = nlohmann::json::object();
    props["prompt"] = {
        {"type", "string"},
        {"description", "Motion / camera / lighting; combine with the user\u2019s wording and context."},
    };
    props["duration"] = {
        {"type", "integer"},
        {"enum", nlohmann::json::array({4, 6, 8})},
        {"description", "Clip length in seconds. Veo accepts 4 / 6 / 8 only."},
    };
    props["resolution"] = {
        {"type", "string"},
        {"enum", nlohmann::json::array({"720p", "1080p", "4K"})},
    };
    props["aspect_ratio"] = {
        {"type", "string"},
        {"enum", nlohmann::json::array({"16:9", "9:16"})},
    };
    props["generate_audio"] = {{"type", "boolean"}};
    props["src"] = {
        {"type", "array"},
        {"items", {{"type", "string"}}},
        {"minItems", 1},
        {"maxItems", 2},
        {"description", "Keyframe stills (host paths). [0] is the first frame, [1] is the last. "
                        "Same as start_frame/last_frame; if both are sent they must match. "
                        "Extra style images go in references."},
    };
    props["paths"] = {
        {"type", "array"},
        {"items", {{"type", "string"}}},
        {"minItems", 1},
        {"maxItems", 2},
        {"description", "Alias of src (Explorer selection / current folder stills)."},
    };
    props["start_frame"] = {
        {"type", "string"},
        {"description", "Optional start-frame still (host path). Alias of a single src[0]."},
    };
    props["image"] = {
        {"type", "string"},
        {"description", "Legacy alias of start_frame."},
    };
    props["last_frame"] = {
        {"type", "string"},
        {"description", "Optional last-frame still (host path). Alias of src[1]."},
    };
    props["reference_images"] = {
        {"type", "array"},
        {"items", {{"type", "string"}}},
        {"description", "Style / brand reference images (not keyframes). Sent as OpenRouter input_references."},
    };
    props["references"] = {
        {"type", "array"},
        {"items", {{"type", "string"}}},
        {"description", "Alias of reference_images."},
    };
    props["reference"] = {
        {"type", "string"},
        {"description", "Single-path alias of reference_images."},
    };
    props["output"] = {
        {"type", "string"},
        {"description", "Optional output path."},
    };
    return nlohmann::json{
        {"type", "object"},
        {"properties", std::move(props)},
        {"required", nlohmann::json::array({"prompt"})},
        {"additionalProperties", false},
    };
}

} // namespace

ToolDef make_create_video() {
    return ToolDef{
        "create_video",
        "Generate a short video clip. **Infer** arguments once from the user prompt, the runtime "
        "folder/selection block, and this tool\u2019s parameter schema. Keyframes use **src** "
        "(or **paths**): `[0]` first frame, `[1]` last frame, or **start_frame** / **image** / **last_frame** "
        "(aliases; both forms may be sent if they match). "
        "Style / brand stills use **references** (or **reference_images** / **reference**); they are "
        "not keyframes. "
        "**Single-call rule: at most one `create_video` per user request.** Never auto-retry after a failure. "
        "Tanit aliases (`video-fast`, `video-deep`) use LiteLLM `/v1/videos` (OpenRouter Veo). "
        "Replicate slugs keep the native predictions path. The reply carries "
        "`results[0].result.intended_output_path` (and `output_path` when `ok`).",
        create_video_fallback_input_schema(),
    };
}

void enrich_create_video_from_replicate_cache(std::vector<ToolDef>& tools) {
    for (ToolDef& t : tools) {
        if (t.name != "create_video")
            continue;
        std::string provider;
        std::string model;
        nlohmann::json empty_opts = nlohmann::json::object();
        media::llm::apply_chat_video_defaults_for_tool_options(provider, model, empty_opts);
        provider = media::strutil::lower_ascii(provider);
        if (provider.empty() || model.empty())
            return;

        if (provider == "tanit" && media::pixlwiz_cli::is_tanit_video_model(model)) {
            t.input_schema = media::pixlwiz_cli::tanit_video_tool_parameters(model);
            t.description += "\n\nCurrent Chat \u2192 Video model: `" + model
                + "` (Tanit / LiteLLM `/v1/videos`). Durations are 4 / 6 / 8 only.";
            return;
        }

        if ((provider != "replicate" && provider != "tanit")
            || model.find('/') == std::string::npos)
            return;

        nlohmann::json flat;
        nlohmann::json required_arr;
        std::string    err;
        if (!media::replicate_cli::lookup_replicate_openapi_input_flat(model, flat, required_arr, err)) {
            t.description += "\n\n(OpenAPI Input for `" + model + "` not in local model cache: " + err
                + " \u2014 refresh models in Settings \u2192 Video.)";
            t.input_schema = create_video_fallback_input_schema();
            return;
        }

        t.input_schema = media::replicate_cli::openapi_flat_to_create_tool_parameters(flat, required_arr);
        return;
    }
}

} // namespace media::llm::path::detail

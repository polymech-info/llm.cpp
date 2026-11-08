#include "kbot.h"
#include "source_files.h"
#include <fstream>
#include <filesystem>
#include <iostream>
#include "logging/logger.h"
#include "llm_client.h"
#include <nlohmann/json.hpp>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace polymech {
namespace kbot {

namespace {

namespace fs = std::filesystem;

static void replace_all(std::string &s, const std::string &from, const std::string &to) {
  std::size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.length(), to);
    pos += to.length();
  }
}

static std::string model_basename(const std::string &model) {
  if (model.empty())
    return "unknown_model";
  const auto slash = model.find_last_of("/\\");
  if (slash == std::string::npos)
    return model;
  return model.substr(slash + 1);
}

static std::string expand_dst_path(const KBotOptions &opts, std::string raw) {
  const std::string m = model_basename(opts.model);
  const std::string r = opts.router.empty() ? std::string("unknown_router") : opts.router;
  replace_all(raw, "${MODEL}", m);
  replace_all(raw, "${MODEL_NAME}", m);
  replace_all(raw, "${ROUTER}", r);
  return raw;
}

/** Same idea as TS `onCompletion`: write to --dst / --output; `dst` wins over legacy `output` if both set. */
static std::string effective_completion_dst(const KBotOptions &opts) {
  if (!opts.dst.empty())
    return opts.dst;
  return opts.output;
}

/** @returns true if wrote to file (caller should skip printing body to stdout). */
static bool try_write_completion_to_dst(const KBotOptions &opts, const std::string &text) {
  const std::string raw = effective_completion_dst(opts);
  if (raw.empty())
    return false;

  std::string expanded = expand_dst_path(opts, raw);
  fs::path p;
  try {
    p = fs::absolute(expanded);
  } catch (const std::exception &e) {
    logger::error(std::string("Invalid output path: ") + e.what());
    return false;
  }

  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  if (ec) {
    logger::error("Failed to create output directories: " + ec.message());
    return false;
  }

  const bool append_existing = (opts.append != "replace") && fs::exists(p);
  std::ofstream out(p, std::ios::binary | (append_existing ? std::ios::app : std::ios::trunc));
  if (!out) {
    logger::error("Failed to open output file: " + p.string());
    return false;
  }
  out << text;
  if (!text.empty() && text.back() != '\n')
    out.put('\n');
  logger::info(std::string(append_existing ? "Appended completion to " : "Wrote completion to ") + p.string());
  return true;
}

std::string json_job_result_ai(bool success, const std::string &text_or_error, bool is_text,
                               const std::string &provider_meta_json = {}) {
  nlohmann::json o;
  o["status"] = success ? "success" : "error";
  o["mode"] = "ai";
  if (success && is_text) o["text"] = text_or_error;
  else if (!success) o["error"] = text_or_error;
  if (!provider_meta_json.empty()) {
    try {
      o["llm"] = nlohmann::json::parse(provider_meta_json);
    } catch (...) {
      o["llm"] = nlohmann::json{{"_parse_error", true}, {"raw", provider_meta_json}};
    }
  }
  return o.dump();
}

} // namespace

int run_kbot_ai_pipeline(const KBotOptions &opts, const KBotCallbacks &cb) {
  logger::debug("Starting kbot ai pipeline");

  std::vector<std::string> source_rel_paths;
  const std::string full_prompt = build_prompt_with_sources(
      opts, opts.include_globs.empty() ? nullptr : &source_rel_paths);
  if (!opts.include_globs.empty()) {
    logger::info("kbot ai: attached " + std::to_string(source_rel_paths.size()) + " text source file(s)");
  }

  if (opts.dry_run) {
    logger::info("Dry run triggered for kbot ai");
    if (cb.onEvent) {
      if (!opts.include_globs.empty()) {
        cb.onEvent("job_result", make_dry_run_ai_result(opts, full_prompt, source_rel_paths).dump());
      } else {
        cb.onEvent("job_result", json_job_result_ai(true, "[dry-run] no LLM call", true));
      }
    }
    return 0;
  }

  LLMClient client(opts);
  std::string target_prompt = full_prompt;
  if (target_prompt.empty()) {
    target_prompt = "Respond with 'Hello from KBot C++ AI Pipeline!'";
  }

  logger::debug("Executing kbot ai completion via LLMClient...");
  LLMResponse res = client.execute_chat(target_prompt);

  if (res.success) {
    if (!try_write_completion_to_dst(opts, res.text))
      std::cout << res.text << "\n";
    if (cb.onEvent) {
      cb.onEvent("ai_progress",
                 "{\"message\":\"Task completion received\",\"has_text\":true}");
    }
  } else {
    logger::error("AI Task Failed: " + res.error);
    if (cb.onEvent) {
      rapidjson::StringBuffer ebuf;
      rapidjson::Writer<rapidjson::StringBuffer> ew(ebuf);
      ew.StartObject();
      ew.Key("error");
      ew.String(res.error.c_str(),
                static_cast<rapidjson::SizeType>(res.error.size()));
      ew.EndObject();
      cb.onEvent("ai_error",
                  std::string(ebuf.GetString(), ebuf.GetSize()));
    }
  }

  if (cb.onEvent) {
    if (res.success)
      cb.onEvent("job_result", json_job_result_ai(true, res.text, true, res.provider_meta_json));
    else
      cb.onEvent("job_result", json_job_result_ai(false, res.error, false));
  }

  return res.success ? 0 : 1;
}

int run_kbot_run_pipeline(const KBotRunOptions &opts, const KBotCallbacks &cb) {
  logger::info("Starting kbot run pipeline (stub) for config: " + opts.config);
  if (opts.dry) {
    logger::info("Dry run triggered for kbot run");
  }
  if (opts.list) {
    logger::info("List configs mode enabled");
  }

  if (!opts.dry && !opts.list) {
    logger::info("Simulating launching: .vscode/launch.json targeting " + opts.config);
  }

  if (cb.onEvent) {
    cb.onEvent("job_result", "{\"status\":\"success\",\"mode\":\"run\"}");
  }
  return 0;
}

} // namespace kbot
} // namespace polymech

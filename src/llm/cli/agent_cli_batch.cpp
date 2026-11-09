#include "pm_image_cmd_includes.hpp"
#include "json/json_utils.hpp"
#include "string/string_utils.hpp"
#include "pm_image_cmd_llm_agent.hpp"
#include "pm_image_cmd_llm_agent_prompt.hpp"
#include "pm_image_cmd_llm_agent_server.hpp"
#include "pm_image_cmd_llm_call.hpp"
#include "pm_image_cmd_llm_info.hpp"
#include "pm_image_cmd_llm_list.hpp"
#include "lan/tanit_lan.hpp"
#include "pm_image_cmd_llm_external_agent.hpp"
#include "pm_image_cli_activity.hpp"

#if defined(FEATURE_JQ) && FEATURE_JQ
#  include "process/async_iterator.hpp"
#  include "llm/agent_decide.hpp"
#  include "llm/agent_each.hpp"
#  include "llm/decisions.hpp"
#  include <cctype>
#endif

#include "cli/cli_tty.hpp"
#include "html/html.h"
#include "llm/agent_feature.hpp"
#include "llm/agent_internal.hpp"
#include "core/chat_settings_schema.hpp"
#include "llm/agent_cli_launch.hpp"
#include "llm/agent_launch.hpp"
#include "llm/agent_runner_log.hpp"
#include "llm/agent_runner_session.hpp"
#include "llm/agent_turn_options.hpp"
#include "llm/agent_memory.hpp"
#include "llm/agent_memory_artifacts.hpp"
#include "llm/agent_memory_recall.hpp"
#include "llm/agent_scheduler.hpp"
#include "fs/fs_guard.hpp"
#include "fs/glob_filter.hpp"
#include "llm/llm_image_tool_defaults.hpp"
#include "llm/path_tool_executor.hpp"  // abort_active_speak / abort_active_run
#ifndef FEATURE_HOME_LLM_TOOLS
#define FEATURE_HOME_LLM_TOOLS 1
#endif
#ifndef FEATURE_HOME_LLM_SKILLS
#define FEATURE_HOME_LLM_SKILLS 1
#endif
#if FEATURE_HOME_LLM_SKILLS
#include "llm/agent_skills.hpp"
#endif
#include "core/command_variables.hpp"
#include "core/settings_runtime.hpp"
#include "win/overlay_hud.hpp"
#include "win/taskbar_status.hpp"
#if defined(FEATURE_COMMAND_IPC_NEXT) && FEATURE_COMMAND_IPC_NEXT
#include "core/agent_session_ipc.hpp"
#include "core/cmd_ipc.hpp"
#endif

#include <filesystem>
namespace fs = std::filesystem;

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <memory>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include "fs/path_sanitizer.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <conio.h>
#include <windows.h>
#endif

#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
#include "security/consent_surface.hpp"
#include "security/ui/win/consent_ai_review.hpp" // ai_review_from_json
#include "llm/security_review.hpp"               // run_review (SecurityAgent)
#include <functional>
#endif

#if defined(FEATURE_STT) && FEATURE_STT
#include "core/audio.hpp"
#include "audio_constants.hpp"
#include "stt/elevenlabs.hpp"
#include "stt/proxy_stt.hpp"
#include "stt/whisper_local.hpp"
#include "tts/elevenlabs_tts.hpp"
#include "tts/proxy_tts.hpp"
#include "tts/windows_tts.hpp"
#include "stt/windows_ai_stt.hpp"
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#  if defined(FEATURE_REALTIME) && FEATURE_REALTIME
#include "core/realtime_voice_session_ipc.hpp"
#include "plugins/chat/host_chat.hpp"
#  endif
#endif
#include "llm/cli/agent_cli_batch.hpp"
#if defined(FEATURE_LLAMA) && FEATURE_LLAMA
#include "lib/llm/llama/EmbeddingModel.hpp"
#endif

namespace pm_llm_agent_cli {
#if defined(FEATURE_JQ) && FEATURE_JQ

/// `llm agent decide`: thin CLI over `media::llm::agent::decide::run_decide`.
int cmd_llm_agent_decide(PmImageCliState& st)
{
    if (st.ag_decide_source.empty()) {
        std::cerr << "llm agent decide: --source <json-file> is required\n";
        return 1;
    }
    if (st.ag_decide_questions.empty()) {
        std::cerr << "llm agent decide: --questions <json-file> is required\n";
        return 1;
    }
    if (st.ag_decide_dst.empty() && !st.ag_decide_json && !st.ag_json) {
        std::cerr << "llm agent decide: --dst <out.json> (or --json) is required\n";
        return 1;
    }

    media::llm::agent::decide::DecideOptions opt;
    opt.source = st.ag_decide_source;
    opt.questions_file = st.ag_decide_questions;
    opt.selector = st.ag_decide_selector;
    opt.pairs = st.ag_decide_pairs;
    opt.left = st.ag_decide_left;
    opt.right = st.ag_decide_right;
    opt.left_key = st.ag_decide_left_key;
    opt.right_key = st.ag_decide_right_key;
    opt.target = st.ag_decide_target;
    opt.dst = st.ag_decide_dst;
    opt.provider = st.ag_decide_provider;
    opt.path = st.ag_decide_path;
    opt.model = st.ag_model;
    opt.api_key = st.ag_api_key;
    opt.base_url = st.ag_base_url;
    opt.chunk = st.ag_decide_chunk;
    opt.max_tokens = st.ag_decide_max_tokens;
    opt.timeout_ms = st.ag_timeout_ms;
    opt.dry_run = st.ag_decide_dry_run || st.ag_dry;

    media::llm::agent::decide::DecideResult result;
    const bool ok = media::llm::agent::decide::run_decide(opt, result);
    if (!ok) {
        std::cerr << "llm agent decide: " << result.error << "\n";
        return 1;
    }
    if (result.wrote_dst && !st.ag_decide_dst.empty())
        media::commands::emit_run_artifact(st.ag_decide_dst, "llm");

    const bool as_json = st.ag_decide_json || st.ag_json;
    if (as_json) {
        std::cout << result.to_json().dump(2) << "\n";
    } else {
        std::cout << "llm agent decide: provider=" << result.provider_id
                  << " pairs=" << result.pairs
                  << " items=" << result.items
                  << " questions=" << result.questions
                  << " requests=" << result.requests
                  << " expanded=" << result.expanded_questions;
        if (!result.resolved_model.empty())
            std::cout << " model=" << result.resolved_model;
        if (!st.ag_decide_dst.empty())
            std::cout << " dst=" << st.ag_decide_dst;
        std::cout << "\n";
    }
    return 0;
}

/// `llm agent each`: thin CLI over `media::llm::agent::each::run_each`.
int cmd_llm_agent_each(PmImageCliState& st)
{
    if (st.ag_each_source.empty()) {
        std::cerr << "llm agent each: --source <json-file> is required\n";
        return 1;
    }
    if (st.ag_each_selector.empty()) {
        std::cerr << "llm agent each: --selector <jq-filter> is required\n";
        return 1;
    }
    if (st.ag_each_dst.empty() && !st.ag_each_json) {
        std::cerr << "llm agent each: --dst <out.json> (or --json) is required\n";
        return 1;
    }

    media::llm::agent::each::EachOptions opt;
    opt.source = st.ag_each_source;
    opt.selector = st.ag_each_selector;
    opt.target = st.ag_each_target;
    opt.dst = st.ag_each_dst;
    opt.prompt = !st.ag_each_prompt.empty() ? st.ag_each_prompt : st.ag_prompt;
    opt.system_prompt = st.ag_system_prompt;
    opt.transform = st.ag_each_transform;
    opt.prefix = st.ag_each_prefix;
    opt.concurrency = st.ag_each_concurrency;
    opt.throttle_ms = st.ag_each_throttle_ms;
    opt.max_retries = st.ag_each_max_retries;
    opt.dry_run = st.ag_each_dry_run || st.ag_dry;
    opt.no_cache = st.ag_each_no_cache;
    opt.merge_json = st.ag_each_merge_json;
    opt.merge_keys = st.ag_each_merge_keys;
    opt.api_mode = st.ag_each_api_mode;
    opt.abort_consecutive = st.ag_each_abort_consecutive;
    opt.partial_commit = st.ag_each_partial_commit;
    opt.preset = st.ag_preset;
    opt.router = st.ag_router;
    opt.model = st.ag_model;
    opt.api_key = st.ag_api_key;
    opt.base_url = st.ag_base_url;
    opt.timeout_ms = st.ag_timeout_ms;

    if (opt.merge_json && !opt.target.empty()) {
        std::cerr << "llm agent each: --merge-json ignores --target "
                     "(object keys name the siblings; hard-overwrite merge)\n";
    }

    media::llm::agent::each::EachResult result;
    const bool ok = media::llm::agent::each::run_each(
        opt, result,
        [](int done, int total, const std::string& path) {
            std::cerr << "llm agent each: " << done << "/" << total << " " << path << std::endl;
        },
        [](const std::string& path, const std::string& value, const std::string& error) {
            std::cerr << "llm agent each: error at " << path
                      << " value=" << value.substr(0, std::min<std::size_t>(value.size(), 48))
                      << " err=" << error << "\n";
        });

    if (!result.error.empty() && !ok) {
        std::cerr << "llm agent each: " << result.error << "\n";
        if (result.aborted) {
            std::cerr << "llm agent each: aborted after " << result.aborted_after_errors
                      << " consecutive leaf failure(s); model likely unsuited\n";
        }
    } else if (result.partial_commit && result.wrote_dst) {
        std::cerr << "llm agent each: partial commit to --dst ("
                  << result.transformed << " transformed, "
                  << result.errors << " leaf error(s)";
        if (result.aborted)
            std::cerr << "; aborted after " << result.aborted_after_errors
                      << " consecutive leaf failure(s)";
        std::cerr << "; re-run with --only-missing to resume)\n";
    }
    if (result.wrote_dst && !st.ag_each_dst.empty())
        media::commands::emit_run_artifact(st.ag_each_dst, "llm");

    if (st.ag_each_json || st.ag_json) {
        std::cout << result.to_json().dump(2) << "\n";
    } else if (ok || result.wrote_dst) {
        std::cout << "llm agent each: matched=" << result.matched
                  << " transformed=" << result.transformed
                  << " skipped=" << result.skipped
                  << " errors=" << result.errors
                  << " cacheHits=" << result.cache_hits
                  << " collectMs=" << result.collect_ms
                  << " transformMs=" << result.transform_ms
                  << " applyMs=" << result.apply_ms;
        if (result.aborted)
            std::cout << " abortedAfter=" << result.aborted_after_errors;
        if (!result.router.empty())
            std::cout << " router=" << result.router << " model=" << result.model;
        if (!st.ag_each_dst.empty())
            std::cout << " dst=" << st.ag_each_dst;
        std::cout << "\n";
    }
    return result.errors > 0 || !ok ? 1 : 0;
}

#if defined(FEATURE_LLAMA) && FEATURE_LLAMA
/// Pick cluster keeper: longest text; tie-break lowest index.
static int pick_cluster_keeper(const std::vector<int>& members,
                               const std::vector<std::string>& texts)
{
    int best = members.front();
    for (int idx : members) {
        if (texts[idx].size() > texts[best].size())
            best = idx;
        else if (texts[idx].size() == texts[best].size() && idx < best)
            best = idx;
    }
    return best;
}

int cmd_llm_agent_dedupe(PmImageCliState& st)
{
    namespace ai = media::async_iterator;

    if (st.ag_dedupe_source.empty()) {
        std::cerr << "llm agent dedupe: --source is required\n";
        return 1;
    }
    if (st.ag_dedupe_selector.empty()) {
        std::cerr << "llm agent dedupe: --selector is required\n";
        return 1;
    }
    if (st.ag_dedupe_embed_model.empty()) {
        std::cerr << "llm agent dedupe: --embed-model is required\n";
        return 1;
    }
    if (st.ag_dedupe_dst.empty() && !st.ag_dedupe_json) {
        std::cerr << "llm agent dedupe: --dst (or --json) is required\n";
        return 1;
    }

    std::ifstream in(st.ag_dedupe_source, std::ios::binary);
    if (!in) {
        std::cerr << "llm agent dedupe: could not open --source: " << st.ag_dedupe_source << "\n";
        return 1;
    }
    nlohmann::json obj = media::json::try_parse(in);
    if (obj.is_discarded()) {
        std::cerr << "llm agent dedupe: invalid JSON in --source\n";
        return 1;
    }

    std::vector<ai::StringLeaf> leaves;
    std::string err;
    if (!ai::collect_string_leaves(obj, st.ag_dedupe_selector, leaves, &err)) {
        std::cerr << "llm agent dedupe: " << (err.empty() ? "selector failed" : err) << "\n";
        return 1;
    }
    if (leaves.empty()) {
        std::cerr << "llm agent dedupe: selector matched no string leaves\n";
        return 1;
    }

    media::llama::EmbeddingModel model;
    logger::info("llm agent dedupe: loading embed model " + st.ag_dedupe_embed_model);
    if (!model.load(st.ag_dedupe_embed_model, st.ag_dedupe_threads, st.ag_dedupe_ctx)) {
        std::cerr << "llm agent dedupe: " << model.error() << "\n";
        return 1;
    }

    std::vector<std::string> texts;
    texts.reserve(leaves.size());
    for (const auto& leaf : leaves)
        texts.push_back(leaf.value);

    const auto embs = model.embed_batch(texts);
    const float threshold = st.ag_dedupe_threshold;
    const float margin = st.ag_dedupe_margin;

    struct Row {
        bool ok = false;
        std::vector<float> emb;
    };
    std::vector<Row> rows(leaves.size());
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        rows[i].ok = embs[i].ok;
        if (embs[i].ok)
            rows[i].emb = std::move(embs[i].embedding);
    }

    auto max_sim_to_cluster = [&](int idx, const std::vector<int>& cluster) -> float {
        float best = -1.f;
        for (int j : cluster) {
            if (!rows[idx].ok || !rows[j].ok)
                continue;
            const float s = media::llama::cosine_similarity(rows[idx].emb, rows[j].emb);
            if (s > best)
                best = s;
        }
        return best;
    };

    std::vector<std::vector<int>> clusters;
    std::vector<int> assign(leaves.size(), -1);
    for (int i = 0; i < static_cast<int>(leaves.size()); ++i) {
        if (!rows[i].ok) {
            clusters.push_back({i});
            assign[i] = static_cast<int>(clusters.size()) - 1;
            continue;
        }
        int best_c = -1;
        float best_s = -1.f;
        float second_s = -1.f;
        for (int c = 0; c < static_cast<int>(clusters.size()); ++c) {
            const float s = max_sim_to_cluster(i, clusters[c]);
            if (s > best_s) {
                second_s = best_s;
                best_s = s;
                best_c = c;
            } else if (s > second_s) {
                second_s = s;
            }
        }
        if (best_c >= 0 && best_s >= threshold && (best_s - second_s) >= margin) {
            clusters[best_c].push_back(i);
            assign[i] = best_c;
        } else {
            clusters.push_back({i});
            assign[i] = static_cast<int>(clusters.size()) - 1;
        }
    }

    std::vector<int> keepers(clusters.size(), -1);
    for (int c = 0; c < static_cast<int>(clusters.size()); ++c)
        keepers[c] = pick_cluster_keeper(clusters[c], texts);

    nlohmann::json items = nlohmann::json::array();
    nlohmann::json kept_texts = nlohmann::json::array();
    int kept_count = 0;
    for (int i = 0; i < static_cast<int>(leaves.size()); ++i) {
        const int c = assign[i];
        const int keeper = keepers[c];
        const bool keep = (i == keeper);
        if (keep)
            ++kept_count;

        float score = 1.f;
        if (!keep && rows[i].ok && rows[keeper].ok)
            score = media::llama::cosine_similarity(rows[i].emb, rows[keeper].emb);

        nlohmann::json row = {
            {"index", i},
            {"path", leaves[i].pointer},
            {"text", texts[i]},
            {"keep", keep},
            {"clusterId", c},
        };
        if (!keep)
            row["duplicateOf"] = keeper;
        row["score"] = score;
        items.push_back(std::move(row));
        if (keep)
            kept_texts.push_back(texts[i]);
    }

    nlohmann::json cluster_out = nlohmann::json::array();
    for (int c = 0; c < static_cast<int>(clusters.size()); ++c) {
        cluster_out.push_back({
            {"id", c},
            {"keepIndex", keepers[c]},
            {"members", clusters[c]},
            {"text", texts[keepers[c]]},
        });
    }

    nlohmann::json payload = {
        {"ok", true},
        {"function", "llm_agent_dedupe"},
        {"source", st.ag_dedupe_source},
        {"selector", st.ag_dedupe_selector},
        {"embedModel", st.ag_dedupe_embed_model},
        {"threshold", threshold},
        {"margin", margin},
        {"matched", leaves.size()},
        {"kept", kept_count},
        {"clusters", cluster_out},
        {"items", items},
        {"keptTexts", kept_texts},
    };
    if (st.ag_dedupe_drop) {
        payload["output"] = {{"keptTexts", kept_texts}};
    }

    if (!st.ag_dedupe_dst.empty()) {
        std::ofstream out(st.ag_dedupe_dst, std::ios::binary);
        if (!out) {
            std::cerr << "llm agent dedupe: could not write --dst: " << st.ag_dedupe_dst << "\n";
            return 1;
        }
        out << payload.dump(2) << "\n";
        media::commands::emit_run_artifact(st.ag_dedupe_dst, "llm");
    }

    if (st.ag_dedupe_json || st.ag_json) {
        std::cout << payload.dump(2) << "\n";
    } else {
        std::cout << "llm agent dedupe: matched=" << leaves.size()
                  << " kept=" << kept_count
                  << " clusters=" << clusters.size();
        if (!st.ag_dedupe_dst.empty())
            std::cout << " dst=" << st.ag_dedupe_dst;
        std::cout << "\n";
    }
    return 0;
}
#endif // FEATURE_LLAMA

#endif // FEATURE_JQ
} // namespace pm_llm_agent_cli

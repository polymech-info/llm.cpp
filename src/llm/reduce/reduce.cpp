//
// Attention reduction layer (POC) — implementation. See reduce.hpp.
//
#include "llm/reduce/reduce.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <mutex>

#include "llm/compressors/structure_mask.hpp"      // EntropyScore
#include "llm/compressors/universal_compressor.hpp" // compress(), estimate_tokens()

namespace media::llm::reduce {
namespace {

using clock_t_ = std::chrono::steady_clock;

// ── small text helpers ──────────────────────────────────────────────────────

std::string_view trim(std::string_view s) noexcept
{
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool is_blank(std::string_view s) noexcept { return trim(s).empty(); }

std::string to_upper(std::string_view s)
{
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// True if `word` appears in `hay` (already upper-cased) as a token (boundaries
// are non-alphabetic). Avoids "INFO" matching inside "INFORMATION".
bool contains_word(std::string_view hay, std::string_view word) noexcept
{
    std::size_t pos = 0;
    while ((pos = hay.find(word, pos)) != std::string_view::npos) {
        const bool left_ok  = pos == 0 || !std::isalpha(static_cast<unsigned char>(hay[pos - 1]));
        const std::size_t r = pos + word.size();
        const bool right_ok = r >= hay.size() || !std::isalpha(static_cast<unsigned char>(hay[r]));
        if (left_ok && right_ok) return true;
        pos = r;
    }
    return false;
}

bool is_hex_run(std::string_view t) noexcept
{
    if (t.empty()) return false;
    for (char c : t)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

bool is_uuid(std::string_view t) noexcept
{
    if (t.size() != 36) return false;
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (t[i] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(t[i]))) {
            return false;
        }
    }
    return true;
}

bool is_upper_code(std::string_view t) noexcept
{
    bool has_underscore = false, has_alpha = false;
    for (char c : t) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '_') has_underscore = true;
        else if (std::isupper(u)) has_alpha = true;
        else if (!std::isdigit(u)) return false;
    }
    return has_underscore && has_alpha;
}

// Classify a whitespace-delimited token as a "protected span" kind, or "" if
// it is ordinary text. Conservative: errs toward keeping identifiers intact.
std::string classify_token(std::string_view t)
{
    if (t.size() < 2) return {};
    if (t.find("://") != std::string_view::npos) return "url";
    if (t.rfind("--", 0) == 0 ||
        (t[0] == '-' && std::isalpha(static_cast<unsigned char>(t[1]))))
        return "flag";
    if (t.rfind("0x", 0) == 0 && t.size() > 2 && is_hex_run(t.substr(2))) return "code";
    if (is_uuid(t)) return "hash";
    if (t.size() >= 16 && is_hex_run(t)) return "hash";
    if ((t.find('/') != std::string_view::npos || t.find('\\') != std::string_view::npos) &&
        t.find('.') != std::string_view::npos)
        return "path";
    if (t.find("::") != std::string_view::npos) return "symbol";
    if (const auto p = t.find('('); p != std::string_view::npos && p > 0 &&
        (std::isalnum(static_cast<unsigned char>(t[p - 1])) || t[p - 1] == '_'))
        return "symbol";
    if (t.size() >= 4 && is_upper_code(t)) return "code";
    return {};
}

// Auto blend used both by the explicit "blend" stage default and by selection
// when no score_final has been written. When an objective is present the
// objective-conditioned channels win (embed upgrades the deterministic lexical
// floor) so relevance beats raw entropy — this is what stops a hash-dense but
// irrelevant unit from outranking an on-topic one.
float auto_blend(const std::unordered_map<std::string, float>& scores, bool has_objective) noexcept
{
    if (has_objective) {
        if (auto it = scores.find("embed");   it != scores.end()) return it->second;
        if (auto it = scores.find("lexical"); it != scores.end()) return it->second;
    }
    if (auto it = scores.find("kompress"); it != scores.end()) return it->second;
    if (auto it = scores.find("entropy"); it != scores.end()) return it->second;
    if (auto it = scores.find("embed"); it != scores.end()) return it->second;
    return 0.0f;
}

float ranking_score(const Unit& u, bool has_objective) noexcept
{
    if (u.score_final != 0.0f) return u.score_final;
    return auto_blend(u.scores, has_objective);
}

// ── deterministic stages ──────────────────────────────────────────────────

void stage_dedup(StageContext& ctx, const StageSpec&)
{
    std::string prev;
    bool        have = false;
    for (Unit& u : ctx.units) {
        if (!u.keep) continue;
        const std::string cur(trim(u.text(ctx.content)));
        if (have && cur == prev) {
            u.keep   = false;
            u.reason = "drop:dup";
        } else {
            prev = cur;
            have = true;
        }
    }
}

void stage_levels(StageContext& ctx, const StageSpec&)
{
    for (Unit& u : ctx.units) {
        const std::string up = to_upper(u.text(ctx.content));
        if (contains_word(up, "ERROR") || contains_word(up, "FATAL") ||
            contains_word(up, "PANIC") || contains_word(up, "WARN") ||
            contains_word(up, "WARNING")) {
            u.protected_ = true;
            if (u.reason.empty()) u.reason = "protect:level";
        } else if (contains_word(up, "INFO") || contains_word(up, "DEBUG") ||
                   contains_word(up, "TRACE")) {
            if (u.keep) {
                u.keep   = false;
                u.reason = "drop:level";
            }
        }
    }
}

void stage_entropy(StageContext& ctx, const StageSpec&)
{
    for (Unit& u : ctx.units) {
        const auto es        = compress::EntropyScore::compute(u.text(ctx.content));
        u.scores["entropy"]  = static_cast<float>(es.value);
    }
}

// Deterministic, objective-conditioned relevance: fraction of the objective's
// content words that appear in the unit. A cheap, model-free "embed" floor —
// useful on its own and as the always-available signal behind query-conditioned
// reduction. No-op (skipped) when there is no objective.
void stage_lexical(StageContext& ctx, const StageSpec&)
{
    if (!ctx.objective) return;
    auto content_words = [](std::string_view s, std::vector<std::string>& out) {
        std::string cur;
        auto flush = [&] {
            if (cur.size() >= 3) out.push_back(cur);
            cur.clear();
        };
        for (char c : s) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (std::isalnum(u)) cur.push_back(static_cast<char>(std::tolower(u)));
            else flush();
        }
        flush();
    };
    std::vector<std::string> obj;
    content_words(*ctx.objective, obj);
    if (obj.empty()) return;
    for (Unit& u : ctx.units) {
        std::vector<std::string> uw;
        content_words(u.text(ctx.content), uw);
        std::size_t hits = 0;
        for (const std::string& w : obj)
            if (std::find(uw.begin(), uw.end(), w) != uw.end()) ++hits;
        u.scores["lexical"] = static_cast<float>(hits) / static_cast<float>(obj.size());
    }
}

void stage_protect(StageContext& ctx, const StageSpec&)
{
    for (Unit& u : ctx.units) {
        if (u.protected_) continue;
        std::string_view txt = u.text(ctx.content);
        std::size_t      i   = 0;
        const std::size_t n  = txt.size();
        while (i < n) {
            while (i < n && std::isspace(static_cast<unsigned char>(txt[i]))) ++i;
            std::size_t j = i;
            while (j < n && !std::isspace(static_cast<unsigned char>(txt[j]))) ++j;
            if (j > i) {
                const std::string kind = classify_token(txt.substr(i, j - i));
                if (!kind.empty()) {
                    u.protected_ = true;
                    u.reason     = "protect:" + kind;
                    break;
                }
            }
            i = j;
        }
    }
}

void stage_blend(StageContext& ctx, const StageSpec& spec)
{
    const bool has_obj = ctx.objective.has_value();
    for (Unit& u : ctx.units) {
        if (spec.nums.empty()) {
            u.score_final = auto_blend(u.scores, has_obj);
            continue;
        }
        float sum = 0.0f;
        for (const auto& [chan, w] : spec.nums) {
            const auto it = u.scores.find(chan);
            if (it != u.scores.end()) sum += static_cast<float>(w) * it->second;
        }
        u.score_final = sum;
    }
}

// Selection: rank still-kept, non-protected units by score and keep within the
// budget. Protected units are always kept. Units are never reordered, so the
// original order is preserved on render (stable_order).
void do_select(StageContext& ctx, const StageSpec& spec, BudgetKind default_kind)
{
    const bool has_obj = ctx.objective.has_value();

    // Resolve budget: explicit stage params win, else fall back to ctx.budget.
    BudgetKind kind  = default_kind;
    double     value = 0.0;
    if (auto it = spec.nums.find("threshold"); it != spec.nums.end()) { kind = BudgetKind::Threshold; value = it->second; }
    else if (auto it2 = spec.nums.find("tokens"); it2 != spec.nums.end()) { kind = BudgetKind::MaxTokens; value = it2->second; }
    else if (auto it3 = spec.nums.find("ratio"); it3 != spec.nums.end()) { kind = BudgetKind::Ratio; value = it3->second; }
    else if (auto it4 = spec.nums.find("keep"); it4 != spec.nums.end()) { kind = BudgetKind::MaxTokens; value = -it4->second; /* sentinel: keep-N below */ }
    else if (ctx.budget.kind != BudgetKind::None) { kind = ctx.budget.kind; value = ctx.budget.value; }
    else { kind = (default_kind == BudgetKind::None ? BudgetKind::Ratio : default_kind); if (kind == BudgetKind::Ratio) value = 0.5; }

    const bool keep_n_mode = (spec.nums.count("keep") != 0);
    const std::size_t keep_n = keep_n_mode ? static_cast<std::size_t>(spec.nums.at("keep")) : 0;

    // Candidate indices: currently kept and not protected.
    std::vector<std::size_t> cand;
    for (std::size_t i = 0; i < ctx.units.size(); ++i)
        if (ctx.units[i].keep && !ctx.units[i].protected_) cand.push_back(i);

    std::stable_sort(cand.begin(), cand.end(), [&](std::size_t a, std::size_t b) {
        return ranking_score(ctx.units[a], has_obj) > ranking_score(ctx.units[b], has_obj);
    });

    auto mark_keep = [&](std::size_t idx) {
        Unit& u = ctx.units[idx];
        if (u.reason.empty()) u.reason = "keep:select";
    };
    auto mark_drop = [&](std::size_t idx, const char* why) {
        Unit& u  = ctx.units[idx];
        u.keep   = false;
        u.reason = why;
    };

    if (keep_n_mode) {
        for (std::size_t r = 0; r < cand.size(); ++r)
            (r < keep_n) ? mark_keep(cand[r]) : mark_drop(cand[r], "drop:budget");
        return;
    }

    switch (kind) {
        case BudgetKind::Threshold: {
            for (std::size_t idx : cand) {
                if (ranking_score(ctx.units[idx], has_obj) < static_cast<float>(value))
                    mark_drop(idx, "drop:below_threshold");
                else
                    mark_keep(idx);
            }
            break;
        }
        case BudgetKind::MaxTokens: {
            std::size_t used = 0;
            // protected + already-kept-noncandidate units consume budget too
            for (std::size_t i = 0; i < ctx.units.size(); ++i)
                if (ctx.units[i].keep && ctx.units[i].protected_)
                    used += compress::estimate_tokens(ctx.units[i].text(ctx.content));
            const std::size_t cap = static_cast<std::size_t>(std::max(0.0, value));
            for (std::size_t idx : cand) {
                const std::size_t t = compress::estimate_tokens(ctx.units[idx].text(ctx.content));
                if (used + t <= cap) { used += t; mark_keep(idx); }
                else mark_drop(idx, "drop:budget");
            }
            break;
        }
        case BudgetKind::Ratio:
        default: {
            const double r       = std::clamp(value, 0.0, 1.0);
            const std::size_t kn = static_cast<std::size_t>(std::llround(static_cast<double>(cand.size()) * r));
            for (std::size_t i = 0; i < cand.size(); ++i)
                (i < kn) ? mark_keep(cand[i]) : mark_drop(cand[i], "drop:budget");
            break;
        }
    }
}

// ── model-backed score stages (no-op without a callback) ────────────────────

// Score the still-kept units; assign into `channel`. Returns false (skipped) if
// the callback is missing or, for embed, no objective is present.
template <typename Fn>
bool score_kept_units(StageContext& ctx, const char* channel, Fn&& invoke)
{
    std::vector<std::size_t>      idx;
    std::vector<std::string_view> texts;
    for (std::size_t i = 0; i < ctx.units.size(); ++i) {
        if (!ctx.units[i].keep) continue;
        idx.push_back(i);
        texts.push_back(ctx.units[i].text(ctx.content));
    }
    if (texts.empty()) return true;
    const std::vector<float> scores = invoke(texts);
    if (scores.size() != texts.size()) return false;
    for (std::size_t k = 0; k < idx.size(); ++k)
        ctx.units[idx[k]].scores[channel] = scores[k];
    return true;
}

void stage_embed(StageContext& ctx, const StageSpec&)
{
    if (!ctx.embed_scores || !ctx.objective) return;  // skipped: no model / no objective
    score_kept_units(ctx, "embed", [&](const std::vector<std::string_view>& t) {
        return ctx.embed_scores(*ctx.objective, t);
    });
}

void stage_kompress(StageContext& ctx, const StageSpec&)
{
    if (!ctx.kompress_scores) return;  // skipped: no model
    score_kept_units(ctx, "kompress", [&](const std::vector<std::string_view>& t) {
        return ctx.kompress_scores(t);
    });
}

// ── registry ────────────────────────────────────────────────────────────────

const std::unordered_map<std::string, StageFn>& registry()
{
    static const std::unordered_map<std::string, StageFn> r = [] {
        std::unordered_map<std::string, StageFn> m;
        m["dedup"]     = stage_dedup;
        m["levels"]    = stage_levels;
        m["entropy"]   = stage_entropy;
        m["lexical"]   = stage_lexical;
        m["protect"]   = stage_protect;
        m["blend"]     = stage_blend;
        m["embed"]     = stage_embed;
        m["kompress"]  = stage_kompress;
        m["select"]    = [](StageContext& c, const StageSpec& s) { do_select(c, s, BudgetKind::Ratio); };
        m["topk"]      = [](StageContext& c, const StageSpec& s) { do_select(c, s, BudgetKind::Ratio); };
        m["ratio"]     = [](StageContext& c, const StageSpec& s) { do_select(c, s, BudgetKind::Ratio); };
        m["threshold"] = [](StageContext& c, const StageSpec& s) { do_select(c, s, BudgetKind::Threshold); };
        return m;
    }();
    return r;
}

Granularity resolve_granularity(Granularity g, compress::ContentType kind) noexcept
{
    if (g != Granularity::Auto) return g;
    return (kind == compress::ContentType::Log) ? Granularity::Lines : Granularity::Paragraphs;
}

const char* join_for(Granularity g) noexcept
{
    switch (g) {
        case Granularity::Lines:      return "\n";
        case Granularity::Paragraphs: return "\n\n";
        default:                      return "";
    }
}

} // namespace

// ── segmentation ──────────────────────────────────────────────────────────

std::vector<Unit> segment(std::string_view content, Granularity g)
{
    std::vector<Unit> units;
    const std::size_t n = content.size();
    if (n == 0) return units;

    if (g == Granularity::Whole || g == Granularity::Auto) {
        units.push_back(Unit{0, n});
        return units;
    }

    if (g == Granularity::Lines) {
        std::size_t i = 0;
        while (i < n) {
            const std::size_t j   = content.find('\n', i);
            const std::size_t end = (j == std::string_view::npos) ? n : j;
            units.push_back(Unit{i, end});
            if (j == std::string_view::npos) break;
            i = j + 1;
        }
        return units;
    }

    // Paragraphs: contiguous non-blank lines, split on blank-line boundaries.
    std::size_t i      = 0;
    std::size_t pstart = std::string_view::npos;
    std::size_t pend   = 0;
    auto        flush  = [&] {
        if (pstart != std::string_view::npos) {
            units.push_back(Unit{pstart, pend});
            pstart = std::string_view::npos;
        }
    };
    while (i < n) {
        const std::size_t j   = content.find('\n', i);
        const std::size_t end = (j == std::string_view::npos) ? n : j;
        if (is_blank(content.substr(i, end - i))) {
            flush();
        } else {
            if (pstart == std::string_view::npos) pstart = i;
            pend = end;
        }
        if (j == std::string_view::npos) break;
        i = j + 1;
    }
    flush();
    return units;
}

// ── presets ───────────────────────────────────────────────────────────────

std::vector<StageSpec> preset_stages(const std::string& preset)
{
    auto spec = [](const char* n) { return StageSpec{n, {}}; };
    std::vector<StageSpec> out{spec("protect")};  // always prepend protect
    if (preset == "logs") {
        out.push_back(spec("dedup")); out.push_back(spec("levels"));
        out.push_back(spec("entropy")); out.push_back(spec("topk"));
    } else if (preset == "docs") {
        out.push_back(spec("embed")); out.push_back(spec("entropy"));
        out.push_back(spec("blend")); out.push_back(spec("topk"));
    } else if (preset == "prose") {
        out.push_back(spec("kompress")); out.push_back(spec("topk"));
    } else if (preset == "tool") {
        out.push_back(spec("dedup")); out.push_back(spec("embed"));
        out.push_back(spec("blend")); out.push_back(spec("topk"));
    } else if (preset == "memory") {
        out.push_back(spec("embed")); out.push_back(spec("topk"));
    } else {
        return {};  // unknown preset
    }
    return out;
}

const StageFn* find_stage(const std::string& name)
{
    const auto& r  = registry();
    const auto  it = r.find(name);
    return it == r.end() ? nullptr : &it->second;
}

// ── entry ─────────────────────────────────────────────────────────────────

ReduceResult reduce(const ReduceRequest& req)
{
    ReduceResult out;
    out.input_bytes = req.content.size();

    const compress::ContentType kind =
        (req.kind == compress::ContentType::Auto) ? compress::detect_content(req.content).content_type
                                                  : req.kind;
    out.kind = kind;

    // Terminal structural path: JSON/code never go through attention stages.
    if (kind == compress::ContentType::Json || kind == compress::ContentType::Code) {
        compress::CompressOptions copts;
        copts.type        = kind;
        const auto cres   = compress::compress(req.content, copts);
        out.compressed    = cres.compressed;
        out.output_bytes  = cres.output_bytes;
        out.ratio         = cres.compression_ratio;
        out.units_in      = 1;
        out.units_out     = 1;
        out.structural    = true;
        out.note          = "structural terminal path (json/code)";
        return out;
    }

    const Granularity gran = resolve_granularity(req.unit, kind);
    std::vector<Unit> units = segment(req.content, gran);
    out.units_in = units.size();

    StageContext ctx{req.content, req.objective, req.budget, units,
                     req.embed_scores, req.kompress_scores};

    // Build the effective stage list; ensure "protect" runs first and a select
    // stage exists so a budget is actually applied.
    std::vector<StageSpec> stages = req.stages;
    if (stages.empty()) {
        // Objective-aware default: with an objective, score lexical relevance
        // (and embed, if a model was supplied) so on-topic units win; otherwise
        // fall back to entropy.
        if (req.objective)
            stages = {{"protect", {}}, {"lexical", {}}, {"embed", {}}, {"entropy", {}}, {"blend", {}}, {"select", {}}};
        else
            stages = {{"protect", {}}, {"entropy", {}}, {"blend", {}}, {"select", {}}};
    }
    const bool has_protect = std::any_of(stages.begin(), stages.end(),
                                         [](const StageSpec& s) { return s.name == "protect"; });
    if (!has_protect) stages.insert(stages.begin(), StageSpec{"protect", {}});

    for (const StageSpec& spec : stages) {
        const StageFn* fn = find_stage(spec.name);
        StageTiming    tm{spec.name, 0.0};
        if (fn) {
            const auto t0 = clock_t_::now();
            (*fn)(ctx, spec);
            tm.ms = std::chrono::duration<double, std::milli>(clock_t_::now() - t0).count();
        } else {
            tm.name = spec.name + " (unknown)";
        }
        out.timings.push_back(tm);
    }

    // ── render kept units in original order ───────────────────────────────
    const char*  joiner = join_for(gran);
    std::string  rendered;
    std::size_t  kept = 0;
    std::size_t  pending_drop = 0;
    bool         first = true;
    for (const Unit& u : units) {
        if (!u.keep) { ++pending_drop; continue; }
        if (!first) rendered.append(joiner);
        if (req.drop_markers && pending_drop > 0)
            rendered.append("…[dropped " + std::to_string(pending_drop) + "]…").append(joiner);
        pending_drop = 0;
        rendered.append(u.text(req.content));
        first = false;
        ++kept;
    }

    out.compressed   = std::move(rendered);
    out.output_bytes = out.compressed.size();
    out.units_out    = kept;
    out.ratio        = out.input_bytes ? static_cast<double>(out.output_bytes) /
                                             static_cast<double>(out.input_bytes)
                                       : 1.0;

    // ── per-unit reports ──────────────────────────────────────────────────
    const bool has_obj = req.objective.has_value();
    out.units.reserve(units.size());
    for (Unit& u : units) {
        if (u.reason.empty()) u.reason = u.keep ? "keep:default" : "drop:default";
        // ensure score_final is populated for reporting even if no blend ran
        if (u.score_final == 0.0f) u.score_final = auto_blend(u.scores, has_obj);
        out.units.push_back(UnitReport{std::string(u.text(req.content)), u.scores,
                                       u.score_final, u.keep, u.protected_, u.reason});
    }

    return out;
}

ReduceResult attention_reduce(std::string_view content,
                              std::optional<std::string_view> objective,
                              Budget budget,
                              EmbedScoreFn embed_scores)
{
    ReduceRequest req;
    req.content      = content;
    req.objective    = objective;
    req.budget       = budget;
    req.stages       = preset_stages("docs");
    req.embed_scores = std::move(embed_scores);
    return reduce(req);
}

} // namespace media::llm::reduce

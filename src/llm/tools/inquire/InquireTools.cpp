// Copyright PolyMech / pm-image. Internal use only.
//
// Backend implementation pasted verbatim from src/llm/tool_inquire.cpp (pre-
// split) — same FTXUI screens for Text / Choice / MultiChoice / Confirm,
// same reply-file cursor protocol, same atomic write fallback.
//
// New here: the do_ask_user / make_ask_user adapters that the path tool
// dispatcher + catalog hook into.
#include "llm/tools/inquire/InquireTools.hpp"
#include "json/json_utils.hpp"

#include "cli/cli_tty.hpp"
#include "logging/logger.h"

#include <nlohmann/json.hpp>

#if defined(PM_PLUGIN_FTXUI) && PM_PLUGIN_FTXUI
#include "plugins/ftxui/host_ftxui.hpp"
#else
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#endif

#include "core/cli_cancel.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>
#include "fs/path_sanitizer.hpp"

namespace pm::llm::inquire {

namespace {

namespace fs = std::filesystem;

constexpr const char k_log_ch[]      = "ask_user";
constexpr const char k_env_replies[] = "PM_ASK_USER_REPLY_FILE";

std::string env_or_empty(const char* name)
{
    if (!name) return {};
    if (const char* v = std::getenv(name); v && *v) return std::string(v);
    return {};
}

long long now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

Answer make_error(const std::string& msg, long long t0)
{
    Answer a;
    a.ok = false;
    a.stopped_by = StoppedBy::Error;
    a.error = msg;
    a.elapsed_ms = now_ms() - t0;
    return a;
}

bool atomic_write_json(const fs::path& target, const nlohmann::json& doc, std::string& err)
{
    std::error_code ec;
    if (target.has_parent_path())
        fs::create_directories(target.parent_path(), ec);

    static std::atomic<uint64_t> s_seq{0};
    const auto seq = s_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    fs::path tmp = target;
    tmp += ".tmp." + std::to_string(static_cast<unsigned long long>(
        std::hash<std::string>{}(std::to_string(seq) + "/" + std::to_string(now_ms())))) ;

    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) { err = "open tmp failed: " + media::path::path_to_utf8(tmp); return false; }
        ofs << doc.dump(2);
        if (!ofs) { err = "write tmp failed: " + media::path::path_to_utf8(tmp); return false; }
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code ec2;
        fs::copy_file(tmp, target, fs::copy_options::overwrite_existing, ec2);
        fs::remove(tmp, ec2);
        if (ec2) { err = "rename + copy fallback failed: " + ec.message(); return false; }
    }
    return true;
}

Answer ask_from_reply_file(const Question& q, long long t0)
{
    const std::string path_s = env_or_empty(k_env_replies);
    if (path_s.empty()) {
        return make_error(
            "ask_user: stdin is not a TTY and PM_ASK_USER_REPLY_FILE is unset — "
            "no way to collect an answer headless.", t0);
    }

    fs::path path(path_s);
    nlohmann::json doc;
    {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) {
            return make_error("ask_user: PM_ASK_USER_REPLY_FILE not found: " + path_s, t0);
        }
        try {
            ifs >> doc;
        } catch (const std::exception& e) {
            return make_error(std::string("ask_user: reply file parse error: ") + e.what(), t0);
        }
    }
    if (!doc.is_object() || !doc.contains("replies") || !doc["replies"].is_array())
        return make_error("ask_user: reply file must be an object with a 'replies' array", t0);

    int cursor = 0;
    if (doc.contains("cursor") && doc["cursor"].is_number_integer())
        cursor = doc["cursor"].get<int>();
    if (cursor < 0) cursor = 0;
    const auto& replies = doc["replies"];
    if (static_cast<int>(replies.size()) <= cursor) {
        return make_error(
            "ask_user: reply file exhausted at cursor " + std::to_string(cursor)
            + " (size=" + std::to_string(replies.size()) + ")", t0);
    }

    const nlohmann::json& r = replies[cursor];
    if (!r.is_object() || !r.contains("answer"))
        return make_error("ask_user: reply[" + std::to_string(cursor)
                          + "] missing 'answer' field", t0);

    Answer a;
    a.ok = true;
    a.stopped_by = StoppedBy::Scripted;
    if (r.contains("stopped_by") && r["stopped_by"].is_string()) {
        const std::string s = r["stopped_by"].get<std::string>();
        if      (s == "submitted") a.stopped_by = StoppedBy::Submitted;
        else if (s == "default")   a.stopped_by = StoppedBy::DefaultedOnEmpty;
        else if (s == "cancelled") a.stopped_by = StoppedBy::Cancelled;
        else if (s == "timeout")   a.stopped_by = StoppedBy::Timeout;
    }

    const nlohmann::json& ans = r["answer"];
    switch (q.kind) {
        case Kind::Text:
            if (!ans.is_string())
                return make_error("ask_user: reply[" + std::to_string(cursor)
                                  + "] expected string answer for kind=text", t0);
            a.text = ans.get<std::string>();
            break;
        case Kind::Choice: {
            int idx = -1;
            if      (ans.is_number_integer()) idx = ans.get<int>();
            else if (ans.is_string()) {
                const std::string v = ans.get<std::string>();
                for (int i = 0; i < static_cast<int>(q.options.size()); ++i)
                    if (q.options[i] == v) { idx = i; break; }
            }
            if (idx < 0 || idx >= static_cast<int>(q.options.size()))
                return make_error("ask_user: reply[" + std::to_string(cursor)
                                  + "] answer does not match any option for kind=choice", t0);
            a.index = idx;
            break;
        }
        case Kind::MultiChoice: {
            if (!ans.is_array())
                return make_error("ask_user: reply[" + std::to_string(cursor)
                                  + "] expected array answer for kind=multi_choice", t0);
            for (const auto& v : ans) {
                int idx = -1;
                if      (v.is_number_integer()) idx = v.get<int>();
                else if (v.is_string()) {
                    const std::string label = v.get<std::string>();
                    for (int i = 0; i < static_cast<int>(q.options.size()); ++i)
                        if (q.options[i] == label) { idx = i; break; }
                }
                if (idx < 0 || idx >= static_cast<int>(q.options.size()))
                    return make_error("ask_user: reply[" + std::to_string(cursor)
                                      + "] item does not match any option for kind=multi_choice", t0);
                a.indices.push_back(idx);
            }
            std::sort(a.indices.begin(), a.indices.end());
            a.indices.erase(std::unique(a.indices.begin(), a.indices.end()), a.indices.end());
            break;
        }
        case Kind::Confirm:
            if (!ans.is_boolean())
                return make_error("ask_user: reply[" + std::to_string(cursor)
                                  + "] expected boolean answer for kind=confirm", t0);
            a.flag = ans.get<bool>();
            break;
    }

    doc["cursor"] = cursor + 1;
    std::string werr;
    if (!atomic_write_json(path, doc, werr)) {
        logger::warn(std::string("[ask_user] could not persist reply cursor: ") + werr);
    }

    a.elapsed_ms = now_ms() - t0;
    return a;
}

#if !defined(PM_PLUGIN_FTXUI) || !PM_PLUGIN_FTXUI
using namespace ftxui;

Element title_bar(const std::string& q, Kind k)
{
    return vbox({
        text(std::string("ask_user · ") + to_string(k)) | dim,
        separator(),
        paragraph(q) | bold,
        separator(),
    });
}

Element hint_bar(const std::string& hint)
{
    return vbox({ separator(), text(hint) | dim });
}

Answer run_text(const Question& q, long long t0)
{
    std::string content     = q.default_text.value_or(std::string{});
    std::string placeholder = q.placeholder.empty()
        ? std::string("type your answer · Enter to submit · Esc to cancel")
        : q.placeholder;
    auto screen = ScreenInteractive::TerminalOutput();

    bool submitted = false;
    bool cancelled = false;

    InputOption opt;
    opt.multiline = q.multiline;
    opt.transform = [](InputState s) {
        auto e = s.element;
        if (s.is_placeholder) e |= dim;
        if (s.focused)        e |= bold;
        return e;
    };
    opt.on_enter = [&] {
        if (!q.multiline) { submitted = true; screen.Exit(); }
    };

    auto input = Input(&content, &placeholder, opt);

    auto submit_btn = Button("Submit", [&]{ submitted = true; screen.Exit(); }, ButtonOption::Ascii());
    auto cancel_btn = Button("Cancel", [&]{ cancelled = true; screen.Exit(); }, ButtonOption::Ascii());

    auto layout = Container::Vertical({
        input,
        Container::Horizontal({ submit_btn, cancel_btn }),
    });

    auto layout_with_keys = CatchEvent(layout, [&](Event e){
        if (e == Event::Escape) { cancelled = true; screen.Exit(); return true; }
        return false;
    });

    auto renderer = Renderer(layout_with_keys, [&] {
        return vbox({
            title_bar(q.question, q.kind),
            input->Render() | border,
            hbox({ submit_btn->Render(), filler(), cancel_btn->Render() }),
            hint_bar(q.multiline
                ? std::string("Enter inserts newline · Tab to Submit · Esc cancels")
                : std::string("Enter submits · Esc cancels")),
        }) | border;
    });

    screen.Loop(renderer);

    Answer a;
    a.elapsed_ms = now_ms() - t0;
    if (cancelled && !submitted) {
        a.ok = false;
        a.stopped_by = StoppedBy::Cancelled;
        a.error = "user cancelled";
        return a;
    }
    a.ok = true;
    a.text = content;
    if (content.empty() && q.default_text) {
        a.text = *q.default_text;
        a.stopped_by = StoppedBy::DefaultedOnEmpty;
    } else {
        a.stopped_by = StoppedBy::Submitted;
    }
    return a;
}

Answer run_choice(const Question& q, long long t0)
{
    int selected = 0;
    if (q.default_index && *q.default_index >= 0
        && *q.default_index < static_cast<int>(q.options.size()))
        selected = *q.default_index;

    auto screen = ScreenInteractive::TerminalOutput();
    auto menu = Menu(&q.options, &selected);

    bool submitted = false;
    bool cancelled = false;
    auto submit_btn = Button("Submit", [&]{ submitted = true; screen.Exit(); }, ButtonOption::Ascii());
    auto cancel_btn = Button("Cancel", [&]{ cancelled = true; screen.Exit(); }, ButtonOption::Ascii());

    auto layout = Container::Vertical({
        menu,
        Container::Horizontal({ submit_btn, cancel_btn }),
    });

    auto layout_with_keys = CatchEvent(layout, [&](Event e){
        if (e == Event::Escape) { cancelled = true; screen.Exit(); return true; }
        if (e == Event::Return) { submitted = true; screen.Exit(); return true; }
        return false;
    });

    auto renderer = Renderer(layout_with_keys, [&]{
        return vbox({
            title_bar(q.question, q.kind),
            menu->Render() | border,
            hbox({ submit_btn->Render(), filler(), cancel_btn->Render() }),
            hint_bar("↑/↓ to move · Enter selects · Esc cancels"),
        }) | border;
    });

    screen.Loop(renderer);

    Answer a;
    a.elapsed_ms = now_ms() - t0;
    if (cancelled && !submitted) {
        a.ok = false;
        a.stopped_by = StoppedBy::Cancelled;
        a.error = "user cancelled";
        return a;
    }
    a.ok = true;
    a.index = selected;
    a.stopped_by = StoppedBy::Submitted;
    return a;
}

Answer run_multi(const Question& q, long long t0)
{
    const size_t n = q.options.size();
    auto checked = std::make_unique<bool[]>(n);
    for (size_t i = 0; i < n; ++i) checked[i] = false;
    for (int i : q.default_indices)
        if (i >= 0 && static_cast<size_t>(i) < n) checked[i] = true;

    auto screen = ScreenInteractive::TerminalOutput();
    Components items;
    items.reserve(n);
    for (size_t i = 0; i < n; ++i)
        items.push_back(Checkbox(&q.options[i], &checked[i]));
    auto choices = Container::Vertical(items);

    bool submitted = false;
    bool cancelled = false;
    auto submit_btn = Button("Submit", [&]{ submitted = true; screen.Exit(); }, ButtonOption::Ascii());
    auto cancel_btn = Button("Cancel", [&]{ cancelled = true; screen.Exit(); }, ButtonOption::Ascii());

    auto layout = Container::Vertical({
        choices,
        Container::Horizontal({ submit_btn, cancel_btn }),
    });

    auto layout_with_keys = CatchEvent(layout, [&](Event e){
        if (e == Event::Escape) { cancelled = true; screen.Exit(); return true; }
        if (e == Event::Return) { submitted = true; screen.Exit(); return true; }
        return false;
    });

    auto renderer = Renderer(layout_with_keys, [&]{
        return vbox({
            title_bar(q.question, q.kind),
            choices->Render() | border,
            hbox({ submit_btn->Render(), filler(), cancel_btn->Render() }),
            hint_bar("Space toggles · Enter submits · Esc cancels"),
        }) | border;
    });

    screen.Loop(renderer);

    Answer a;
    a.elapsed_ms = now_ms() - t0;
    if (cancelled && !submitted) {
        a.ok = false;
        a.stopped_by = StoppedBy::Cancelled;
        a.error = "user cancelled";
        return a;
    }
    a.ok = true;
    for (size_t i = 0; i < n; ++i)
        if (checked[i]) a.indices.push_back(static_cast<int>(i));
    a.stopped_by = StoppedBy::Submitted;
    return a;
}

Answer run_confirm(const Question& q, long long t0)
{
    const bool def = q.default_bool.value_or(true);
    int selected = def ? 0 : 1;
    std::vector<std::string> entries = { "Yes", "No" };

    auto screen = ScreenInteractive::TerminalOutput();
    auto menu = Menu(&entries, &selected, MenuOption::HorizontalAnimated());

    bool submitted = false;
    bool cancelled = false;
    auto layout = CatchEvent(menu, [&](Event e){
        if (e == Event::Escape)       { cancelled = true; screen.Exit(); return true; }
        if (e == Event::Return)       { submitted = true; screen.Exit(); return true; }
        if (e == Event::Character('y') || e == Event::Character('Y'))
            { selected = 0; submitted = true; screen.Exit(); return true; }
        if (e == Event::Character('n') || e == Event::Character('N'))
            { selected = 1; submitted = true; screen.Exit(); return true; }
        return false;
    });

    auto renderer = Renderer(layout, [&]{
        return vbox({
            title_bar(q.question, q.kind),
            menu->Render() | border,
            hint_bar("y/n or ←/→ + Enter · Esc cancels"),
        }) | border;
    });

    screen.Loop(renderer);

    Answer a;
    a.elapsed_ms = now_ms() - t0;
    if (cancelled && !submitted) {
        a.ok = false;
        a.stopped_by = StoppedBy::Cancelled;
        a.error = "user cancelled";
        return a;
    }
    a.ok = true;
    a.flag = (selected == 0);
    a.stopped_by = StoppedBy::Submitted;
    return a;
}

Answer ask_via_ftxui(const Question& q, long long t0)
{
    try {
        switch (q.kind) {
            case Kind::Text:        return run_text(q, t0);
            case Kind::Choice:      return run_choice(q, t0);
            case Kind::MultiChoice: return run_multi(q, t0);
            case Kind::Confirm:     return run_confirm(q, t0);
        }
    } catch (const std::exception& e) {
        return make_error(std::string("ask_user: FTXUI failure: ") + e.what(), t0);
    }
    return make_error("ask_user: unhandled kind", t0);
}
#else
Answer ask_via_ftxui(const Question& q, long long t0)
{
    Answer a = pm::plugins::ftxui_host::ask(q, t0);
    if (a.elapsed_ms == 0)
        a.elapsed_ms = now_ms() - t0;
    return a;
}
#endif

} // namespace

const char* to_string(Kind k) noexcept
{
    switch (k) {
        case Kind::Text:        return "text";
        case Kind::Choice:      return "choice";
        case Kind::MultiChoice: return "multi_choice";
        case Kind::Confirm:     return "confirm";
    }
    return "text";
}

Kind parse_kind(std::string_view s, Kind fallback) noexcept
{
    if (s == "text")         return Kind::Text;
    if (s == "choice")       return Kind::Choice;
    if (s == "multi_choice") return Kind::MultiChoice;
    if (s == "confirm")      return Kind::Confirm;
    return fallback;
}

const char* to_string(StoppedBy s) noexcept
{
    switch (s) {
        case StoppedBy::Submitted:        return "submitted";
        case StoppedBy::DefaultedOnEmpty: return "default";
        case StoppedBy::Cancelled:        return "cancelled";
        case StoppedBy::Timeout:          return "timeout";
        case StoppedBy::Scripted:         return "scripted";
        case StoppedBy::Error:            return "error";
    }
    return "error";
}

std::string stopped_by_label(StoppedBy s) { return to_string(s); }

Answer ask_cli(const Question& q)
{
    const long long t0 = now_ms();

    if (q.question.empty())
        return make_error("ask_user: question is required", t0);
    if ((q.kind == Kind::Choice || q.kind == Kind::MultiChoice) && q.options.empty())
        return make_error("ask_user: options is required for kind=choice / multi_choice", t0);

    if (!env_or_empty(k_env_replies).empty()) {
        return ask_from_reply_file(q, t0);
    }

    if (!cli_tty::stdin_is_tty() || !cli_tty::stdout_is_tty()) {
        return ask_from_reply_file(q, t0);
    }

    return ask_via_ftxui(q, t0);
}

Answer ask_ui_stub(const Question& q)
{
    (void)q;
    Answer a;
    a.ok = false;
    a.stopped_by = StoppedBy::Error;
    a.error = "ui_not_implemented: ask_user UI backend pending (see ChatWebPanel.cpp)";
    return a;
}

// ── UI backend slot ──────────────────────────────────────────────────────
//
// `g_ui_backend` is the active callable set by `set_ui_backend()`. The atomic
// flag is a cheap probe used by `ui_backend_active()`/`ask()` so the agent
// loop doesn't grab the mutex on every tool call when no UI is around.
namespace {
std::mutex          g_ui_backend_mu;
UiAskFn             g_ui_backend;
std::atomic<bool>   g_ui_backend_set{false};
} // namespace

void set_ui_backend(UiAskFn fn)
{
    std::lock_guard<std::mutex> lk(g_ui_backend_mu);
    g_ui_backend = std::move(fn);
    g_ui_backend_set.store(static_cast<bool>(g_ui_backend), std::memory_order_release);    
}

void clear_ui_backend()
{
    std::lock_guard<std::mutex> lk(g_ui_backend_mu);
    g_ui_backend = nullptr;
    g_ui_backend_set.store(false, std::memory_order_release);
}

bool ui_backend_active() noexcept
{
    return g_ui_backend_set.load(std::memory_order_acquire);
}

Answer ask(const Question& q)
{
    if (ui_backend_active()) {
        UiAskFn fn;
        {
            std::lock_guard<std::mutex> lk(g_ui_backend_mu);
            fn = g_ui_backend;
        }
        if (fn) {
            return fn(q);
        }
        // Slot raced clear() — fall through to CLI rather than returning Error.
        logger::warn("[ask_user] UI backend slot raced clear(); falling back to ask_cli()");
    }
    return ask_cli(q);
}

// ── Stream ask (HTTP serve `--consent-ui stream`) ─────────────────────────

namespace {

struct StreamAskSlot {
    std::promise<nlohmann::json> promise;
    std::atomic<bool> done{false};

    void finish(nlohmann::json reply)
    {
        bool expected = false;
        if (done.compare_exchange_strong(expected, true))
            promise.set_value(std::move(reply));
    }
};

struct StreamAskEmitFrame {
    std::uint64_t ticket = 0;
    StreamAskEmit fn;
};

struct StreamAskState {
    std::mutex mu;
    std::uint64_t next_ticket = 0;
    std::vector<StreamAskEmitFrame> stack;
    StreamAskEmit emit;
    std::unordered_map<std::string, std::shared_ptr<StreamAskSlot>> pending;
    std::atomic<bool> backend_installed{false};
};

StreamAskState& stream_ask_state()
{
    static StreamAskState state;
    return state;
}

void publish_top_ask_emit(StreamAskState& state)
{
    state.emit = state.stack.empty() ? StreamAskEmit{} : state.stack.back().fn;
}

std::string make_stream_ask_req_id()
{
    using namespace std::chrono;
    static std::atomic<std::uint64_t> counter{0};
    const auto now = static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    const auto n = counter.fetch_add(1, std::memory_order_relaxed);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "ask-%llu-%llu",
                  static_cast<unsigned long long>(now),
                  static_cast<unsigned long long>(n));
    return std::string(buf);
}

nlohmann::json question_to_stream_json(const std::string& req_id, const Question& q)
{
    nlohmann::json j;
    j["reqId"] = req_id;
    j["kind"] = to_string(q.kind);
    j["question"] = q.question;
    j["options"] = q.options;
    j["placeholder"] = q.placeholder;
    j["multiline"] = q.multiline;
    j["timeoutMs"] = q.timeout_ms;
    if (q.default_text)
        j["defaultText"] = *q.default_text;
    if (q.default_index)
        j["defaultIndex"] = *q.default_index;
    if (!q.default_indices.empty())
        j["defaultIndices"] = q.default_indices;
    if (q.default_bool)
        j["defaultBool"] = *q.default_bool;
    return j;
}

Answer answer_from_stream_reply(const Question& q, const nlohmann::json& j, long long elapsed_ms)
{
    Answer a;
    a.elapsed_ms = elapsed_ms;

    const std::string stopped = j.value("stoppedBy", std::string{"submitted"});
    if (stopped == "submitted") {
        a.ok = true;
        a.stopped_by = StoppedBy::Submitted;
    } else if (stopped == "default") {
        a.ok = true;
        a.stopped_by = StoppedBy::DefaultedOnEmpty;
    } else if (stopped == "cancelled") {
        a.ok = false;
        a.stopped_by = StoppedBy::Cancelled;
    } else if (stopped == "timeout") {
        a.ok = false;
        a.stopped_by = StoppedBy::Timeout;
    } else if (stopped == "error") {
        a.ok = false;
        a.stopped_by = StoppedBy::Error;
        a.error = j.value("error", std::string{"ask_user: stream ui error"});
    } else {
        a.ok = true;
        a.stopped_by = StoppedBy::Submitted;
    }

    if (!a.ok && a.stopped_by != StoppedBy::Error)
        a.error = j.value("error", std::string{});

    switch (q.kind) {
        case Kind::Text:
            if (j.contains("text") && j["text"].is_string())
                a.text = j["text"].get<std::string>();
            else if (a.stopped_by == StoppedBy::DefaultedOnEmpty && q.default_text)
                a.text = *q.default_text;
            break;
        case Kind::Choice:
            if (j.contains("index") && j["index"].is_number_integer())
                a.index = j["index"].get<int>();
            break;
        case Kind::MultiChoice:
            if (j.contains("indices") && j["indices"].is_array()) {
                for (const auto& v : j["indices"])
                    if (v.is_number_integer())
                        a.indices.push_back(v.get<int>());
            }
            break;
        case Kind::Confirm:
            if (j.contains("bool") && j["bool"].is_boolean())
                a.flag = j["bool"].get<bool>();
            break;
    }
    return a;
}

Answer ask_via_stream(const Question& q)
{
    const auto t0 = std::chrono::steady_clock::now();
    StreamAskEmit emit;
    {
        std::lock_guard<std::mutex> lk(stream_ask_state().mu);
        emit = stream_ask_state().emit;
    }
    if (!emit) {
        // No HTTP turn bound — fall back to CLI / reply-file (headless error).
        return ask_cli(q);
    }

    const std::string req_id = make_stream_ask_req_id();
    auto slot = std::make_shared<StreamAskSlot>();
    auto future = slot->promise.get_future();
    {
        std::lock_guard<std::mutex> lk(stream_ask_state().mu);
        stream_ask_state().pending[req_id] = slot;
    }

    const std::string payload = question_to_stream_json(req_id, q).dump();
    if (!emit("ask", payload)) {
        slot->finish(nlohmann::json{
            {"reqId", req_id},
            {"stoppedBy", "error"},
            {"error", "ask_user: stream emit failed"},
        });
    }

    nlohmann::json reply{
        {"reqId", req_id},
        {"stoppedBy", "cancelled"},
        {"error", "ask_user: stream ask cancelled"},
    };
    for (;;) {
        if (future.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready)
            break;
        if (media::cli::cancel_requested()) {
            slot->finish(nlohmann::json{
                {"reqId", req_id},
                {"stoppedBy", "cancelled"},
                {"error", "ask_user: cancelled"},
            });
            break;
        }
    }
    try {
        reply = future.get();
    } catch (...) {
        /* keep cancelled default */
    }
    {
        std::lock_guard<std::mutex> lk(stream_ask_state().mu);
        stream_ask_state().pending.erase(req_id);
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    return answer_from_stream_reply(q, reply, elapsed);
}

} // namespace

std::uint64_t bind_stream_ask_emit(StreamAskEmit emit)
{
    std::lock_guard<std::mutex> lk(stream_ask_state().mu);
    auto& state = stream_ask_state();
    const std::uint64_t ticket = ++state.next_ticket;
    state.stack.push_back(StreamAskEmitFrame{ticket, std::move(emit)});
    publish_top_ask_emit(state);
    return ticket;
}

void restore_stream_ask_emit(std::uint64_t ticket)
{
    if (ticket == 0)
        return;
    std::lock_guard<std::mutex> lk(stream_ask_state().mu);
    auto& state = stream_ask_state();
    for (auto it = state.stack.begin(); it != state.stack.end(); ++it) {
        if (it->ticket != ticket)
            continue;
        state.stack.erase(it);
        publish_top_ask_emit(state);
        return;
    }
}

bool resolve_stream_ask(const std::string& reply_json)
{
    nlohmann::json reply = media::json::try_parse(reply_json);
    if (!reply.is_object())
        return false;
    const std::string req_id = reply.value("reqId", std::string{});
    if (req_id.empty())
        return false;
    std::shared_ptr<StreamAskSlot> slot;
    {
        std::lock_guard<std::mutex> lk(stream_ask_state().mu);
        const auto it = stream_ask_state().pending.find(req_id);
        if (it == stream_ask_state().pending.end())
            return false;
        slot = it->second;
    }
    slot->finish(std::move(reply));
    return true;
}

void cancel_stream_ask()
{
    std::unordered_map<std::string, std::shared_ptr<StreamAskSlot>> pending;
    {
        std::lock_guard<std::mutex> lk(stream_ask_state().mu);
        pending.swap(stream_ask_state().pending);
    }
    for (auto& [req_id, slot] : pending) {
        if (!slot)
            continue;
        slot->finish(nlohmann::json{
            {"reqId", req_id},
            {"stoppedBy", "cancelled"},
            {"error", "ask_user: stream ask cancelled"},
        });
    }
}

void ensure_stream_ask_ui_backend()
{
    auto& state = stream_ask_state();
    bool expected = false;
    if (!state.backend_installed.compare_exchange_strong(expected, true))
        return;
    set_ui_backend([](const Question& q) -> Answer { return ask_via_stream(q); });
    logger::info("[ask_user] stream UI backend installed (HTTP --consent-ui stream)");
}

} // namespace pm::llm::inquire

// ── Path-tool adapters ─────────────────────────────────────────────────────

namespace media::llm::path::detail {

ExecuteResult do_ask_user(const nlohmann::json& args) {
    using pm::llm::inquire::Kind;
    using pm::llm::inquire::Question;
    using pm::llm::inquire::Answer;

    auto read_str = [&](const char* key) -> std::string {
        if (auto s = get_nonempty_string(args, key)) return *s;
        if (auto s = get_nonempty_string(options_or_empty(args), key)) return *s;
        return {};
    };

    Question q;
    q.kind        = pm::llm::inquire::parse_kind(read_str("kind"), Kind::Text);
    q.question    = read_str("question");
    q.placeholder = read_str("placeholder");

    if (q.question.empty())
        return fatal("ask_user: 'question' (string) is required");

    if (args.contains("multiline") && args["multiline"].is_boolean())
        q.multiline = args["multiline"].get<bool>();
    else if (const auto& o = options_or_empty(args);
             o.contains("multiline") && o["multiline"].is_boolean())
        q.multiline = o["multiline"].get<bool>();

    if (args.contains("timeout_ms") && args["timeout_ms"].is_number_integer())
        q.timeout_ms = args["timeout_ms"].get<int>();

    if (args.contains("options") && args["options"].is_array()) {
        for (const auto& v : args["options"])
            if (v.is_string()) q.options.push_back(v.get<std::string>());
    }
    if ((q.kind == Kind::Choice || q.kind == Kind::MultiChoice) && q.options.empty())
        return fatal("ask_user: 'options' (string array, non-empty) is required for kind=choice / multi_choice");
    if (q.options.size() > 64)
        return fatal("ask_user: too many options (max 64)");

    if (args.contains("default")) {
        const auto& d = args["default"];
        switch (q.kind) {
            case Kind::Text:
                if (d.is_string()) q.default_text = d.get<std::string>();
                break;
            case Kind::Choice:
                if (d.is_number_integer()) q.default_index = d.get<int>();
                else if (d.is_string()) {
                    for (int i = 0; i < static_cast<int>(q.options.size()); ++i)
                        if (q.options[i] == d.get<std::string>()) { q.default_index = i; break; }
                }
                break;
            case Kind::MultiChoice:
                if (d.is_array()) {
                    for (const auto& v : d) {
                        if (v.is_number_integer()) q.default_indices.push_back(v.get<int>());
                        else if (v.is_string()) {
                            for (int i = 0; i < static_cast<int>(q.options.size()); ++i)
                                if (q.options[i] == v.get<std::string>()) { q.default_indices.push_back(i); break; }
                        }
                    }
                }
                break;
            case Kind::Confirm:
                if (d.is_boolean()) q.default_bool = d.get<bool>();
                break;
        }
    }

    // Routes to UI backend if registered (chat WebView), else FTXUI / reply-file.
    Answer ans = pm::llm::inquire::ask(q);

    if (!ans.ok) {
        ExecuteResult r;
        r.ok    = false;
        r.error = ans.error.empty() ? std::string("ask_user: ") + pm::llm::inquire::to_string(ans.stopped_by)
                                    : ans.error;
        r.envelope = nlohmann::json{
            {"ok", false},
            {"tool", "ask_user"},
            {"kind", pm::llm::inquire::to_string(q.kind)},
            {"stopped_by", pm::llm::inquire::stopped_by_label(ans.stopped_by)},
            {"elapsed_ms", ans.elapsed_ms},
            {"error", r.error},
        };
        return r;
    }

    nlohmann::json envelope = {
        {"ok", true},
        {"tool", "ask_user"},
        {"kind", pm::llm::inquire::to_string(q.kind)},
        {"stopped_by", pm::llm::inquire::stopped_by_label(ans.stopped_by)},
        {"elapsed_ms", ans.elapsed_ms},
    };
    switch (q.kind) {
        case Kind::Text:
            envelope["answer"] = ans.text.value_or(std::string{});
            break;
        case Kind::Choice:
            if (ans.index) {
                envelope["answer_index"] = *ans.index;
                if (*ans.index >= 0 && *ans.index < static_cast<int>(q.options.size()))
                    envelope["answer"] = q.options[*ans.index];
            }
            break;
        case Kind::MultiChoice: {
            nlohmann::json idx = nlohmann::json::array();
            nlohmann::json lbl = nlohmann::json::array();
            for (int i : ans.indices) {
                idx.push_back(i);
                if (i >= 0 && i < static_cast<int>(q.options.size()))
                    lbl.push_back(q.options[i]);
            }
            envelope["answer_indices"] = std::move(idx);
            envelope["answer"]         = std::move(lbl);
            break;
        }
        case Kind::Confirm:
            envelope["answer"] = ans.flag.value_or(false);
            break;
    }

    ExecuteResult r;
    r.ok        = true;
    r.envelope  = std::move(envelope);
    return r;
}

ToolDef make_ask_user() {
    return ToolDef{
        "ask_user",
        "Interactively ask the human user a question and wait for their reply. "
        "ONE tool, four `kind`s: "
        "`text` — free-form line (use `placeholder` to hint format, `default` to pre-fill); "
        "`choice` — exactly one of `options` (use `default` as label or 0-based index); "
        "`multi_choice` — zero or more of `options` (use `default` as array of labels or indices); "
        "`confirm` — yes/no (use `default`: true=Yes, false=No). "
        "Use when a fact you need to proceed is missing from memory and the user is the only source — do NOT use it to chit-chat, request approval for safe actions, or paraphrase what the user just said. "
        "The CLI surface renders this via FTXUI (modal box, Esc to cancel). "
        "Headless / scripted runs read pre-seeded replies from `PM_ASK_USER_REPLY_FILE` (test harness). "
        "The UI surface is wired to the chat window. "
        "Returns `{ok, kind, stopped_by, answer, ...}` where `stopped_by` is one of "
        "`submitted` | `default` | `cancelled` | `timeout` | `scripted` | `error`. "
        "For `choice` the envelope adds `answer_index`; for `multi_choice` it adds `answer_indices` alongside the label array.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {
                {"kind", {
                    {"type", "string"},
                    {"enum", {"text", "choice", "multi_choice", "confirm"}},
                    {"description",
                     "Interaction modality. Pick the smallest one that fits: `confirm` for binary, "
                     "`choice` when the answer set is closed and small, `multi_choice` when the user "
                     "may pick several, `text` only when no fixed option set exists."},
                    {"default", "text"},
                }},
                {"question", {
                    {"type", "string"},
                    {"description",
                     "The prompt rendered to the user. Phrase it in plain language; "
                     "do NOT include the option labels here for `choice`/`multi_choice` — "
                     "they're shown as a menu underneath."},
                }},
                {"options", {
                    {"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"description",
                     "Selectable labels for `choice` / `multi_choice`. "
                     "Required and non-empty for those kinds; ignored otherwise. Max 64 items."},
                }},
                {"default", {
                    {"description",
                     "Optional pre-selected answer matching `kind`: "
                     "`text` → string; `choice` → option label or 0-based index; "
                     "`multi_choice` → array of labels or indices; `confirm` → boolean. "
                     "Applied when the user submits an empty `text` answer, or as initial cursor "
                     "position / selection in the menu/confirm screens."},
                }},
                {"placeholder", {
                    {"type", "string"},
                    {"description",
                     "Hint text shown inside the text input when empty (e.g. `e.g. blue`). "
                     "Only used when `kind=text`."},
                }},
                {"multiline", {
                    {"type", "boolean"},
                    {"description",
                     "Allow multi-line input (`kind=text` only). Default false — Enter submits. "
                     "When true, Enter inserts a newline and the user submits via the Submit button."},
                    {"default", false},
                }},
                {"timeout_ms", {
                    {"type", "integer"},
                    {"minimum", 0},
                    {"description",
                     "Reserved. Currently unused; FTXUI / UI backends submit only on explicit "
                     "user action. Pass 0 or omit."},
                    {"default", 0},
                }},
            }},
            {"required", nlohmann::json::array({"question"})},
        }};
}

} // namespace media::llm::path::detail

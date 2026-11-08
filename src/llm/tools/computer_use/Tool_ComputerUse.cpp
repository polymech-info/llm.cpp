#include "Tool_ComputerUse.hpp"

#include "constants.hpp"
#include "fs/path_sanitizer.hpp"
#include "fs/fs_guard.hpp"
#include "ui_defaults.hpp"

#if defined(_WIN32) && defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT && defined(FEATURE_AGENT_COMPUTER_USE) && FEATURE_AGENT_COMPUTER_USE
#include "core/host_backends.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "string/string_utils.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace media::llm::computer_use {
namespace {

namespace fs = std::filesystem;
using media::llm::path::ExecuteResult;
using media::llm::path::ToolDef;

ExecuteResult fatal(const std::string& msg) {
    ExecuteResult r;
    r.ok = false;
    r.error = msg;
    r.envelope = nlohmann::json{{"ok", false}, {"error", msg}};
    return r;
}

ExecuteResult tool_ok(const std::string& tool, nlohmann::json payload) {
    payload["ok"] = true;
    payload["tool"] = tool;
    ExecuteResult r;
    r.ok = true;
    r.envelope = std::move(payload);
    return r;
}

std::string json_string(const nlohmann::json& j, const char* key, const std::string& def = {}) {
    if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    for (const char* section : {"target", "query", "options"}) {
        if (j.contains(section) && j[section].is_object() && j[section].contains(key) && j[section][key].is_string())
            return j[section][key].get<std::string>();
    }
    return def;
}

bool json_bool(const nlohmann::json& j, const char* key, bool def = false) {
    if (j.contains(key) && j[key].is_boolean()) return j[key].get<bool>();
    for (const char* section : {"target", "query", "options"}) {
        if (j.contains(section) && j[section].is_object() && j[section].contains(key) && j[section][key].is_boolean())
            return j[section][key].get<bool>();
    }
    return def;
}

int json_int(const nlohmann::json& j, const char* key, int def = 0) {
    if (j.contains(key) && j[key].is_number_integer()) return j[key].get<int>();
    for (const char* section : {"target", "query", "options"}) {
        if (j.contains(section) && j[section].is_object() && j[section].contains(key) && j[section][key].is_number_integer())
            return j[section][key].get<int>();
    }
    return def;
}

std::uint64_t json_uint64(const nlohmann::json& j, const char* key, std::uint64_t def = 0) {
    auto pick = [](const nlohmann::json& v) -> std::uint64_t {
        if (v.is_number_unsigned()) return v.get<std::uint64_t>();
        if (v.is_number_integer())  return static_cast<std::uint64_t>(v.get<std::int64_t>());
        if (v.is_number_float())    return static_cast<std::uint64_t>(v.get<double>());
        return 0;
    };
    if (j.contains(key) && j[key].is_number()) return pick(j[key]);
    for (const char* section : {"target", "query", "options"}) {
        if (j.contains(section) && j[section].is_object() && j[section].contains(key) && j[section][key].is_number())
            return pick(j[section][key]);
    }
    return def;
}

std::string resolve_tool_path_string(const std::string& raw, const std::string& agent_path_base) {
    if (raw.empty()) return raw;
    std::error_code ec;
    fs::path p(raw);
    if (p.is_absolute()) return media::path::path_to_utf8(fs::weakly_canonical(p, ec));
    fs::path base = agent_path_base.empty()
        ? media::ui_defaults::fallback_work_folder()
        : media::path::path_from_utf8(agent_path_base);
    if (base.empty()) {
        std::error_code ec_fallback;
        base = fs::current_path(ec_fallback);
        if (ec_fallback || media::ui_defaults::is_under_install_dir(base))
            base = fs::path{};
    }
    return media::path::path_to_utf8((base / p).lexically_normal());
}

std::string write_deny_reason(const std::string& abs) {
    if (auto s = media::llm::llm_fs_guard_deny_reason(media::path::path_from_utf8(abs)); !s.empty()) return s;
    return media::llm::llm_fs_guard_write_deny_reason(media::path::path_from_utf8(abs));
}

#if defined(_WIN32) && defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT && defined(FEATURE_AGENT_COMPUTER_USE) && FEATURE_AGENT_COMPUTER_USE
namespace ai = pm::host::app_inspect;
namespace au = pm::host::app_use;
namespace ab = pm::host::app_batch;

struct AppSession {
    std::vector<ai::WindowDump> windows;
    HWND current_hwnd = nullptr;
    int revision = 0;
};

static thread_local std::unordered_map<std::string, AppSession> g_sessions;

std::string app_session_id_from_args(const nlohmann::json& args) {
    return json_string(args, "session_id", "default");
}

void remember_app_hwnd(const nlohmann::json& args, HWND hwnd) {
    if (!hwnd || !::IsWindow(hwnd)) return;
    g_sessions[app_session_id_from_args(args)].current_hwnd = hwnd;
}

HWND current_app_hwnd(const nlohmann::json& args) {
    auto it = g_sessions.find(app_session_id_from_args(args));
    if (it == g_sessions.end()) return nullptr;
    HWND hwnd = it->second.current_hwnd;
    return hwnd && ::IsWindow(hwnd) ? hwnd : nullptr;
}

std::wstring u2w(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n);
    return out;
}

std::string w2u(const std::wstring& ws) {
    if (ws.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), &out[0], n, nullptr, nullptr);
    return out;
}

bool contains_ci(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    std::wstring h = haystack, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    std::transform(n.begin(), n.end(), n.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return h.find(n) != std::wstring::npos;
}

nlohmann::json rect_json(const ai::Rect& r) {
    return {{"x", r.x}, {"y", r.y}, {"w", r.w}, {"h", r.h},
            {"left", r.x}, {"top", r.y}, {"right", r.x + r.w}, {"bottom", r.y + r.h},
            {"center", {{"x", r.x + r.w / 2}, {"y", r.y + r.h / 2}}}};
}

nlohmann::json element_json(const ai::ElementInfo& e) {
    return {
        {"index", e.index},
        {"pid", e.pid},
        {"hwnd", reinterpret_cast<std::uintptr_t>(e.hwnd)},
        {"process", w2u(e.process)},
        {"windowTitle", w2u(e.window_title)},
        {"controlType", w2u(e.control_type)},
        {"name", w2u(e.name)},
        {"value", w2u(e.value)},
        {"automationId", w2u(e.automation_id)},
        {"className", w2u(e.class_name)},
        {"frameworkId", w2u(e.framework_id)},
        {"enabled", e.enabled},
        {"offscreen", e.offscreen},
        {"focused", e.focused},
        {"keyboardFocusable", e.keyboard_focusable},
        {"nativeHwnd", e.native_hwnd},
        {"patterns", {
            {"invoke", e.has_invoke},
            {"value", e.has_value},
            {"text", e.has_text},
            {"selectionItem", e.has_selection_item},
            {"expandCollapse", e.has_expand_collapse},
            {"scroll", e.has_scroll},
        }},
        {"rect", rect_json(e.rect)},
        {"center", {{"x", e.rect.x + e.rect.w / 2}, {"y", e.rect.y + e.rect.h / 2}}},
    };
}

bool controls_mentions_cells(const std::string& controls) {
    const std::string c = media::strutil::lower_ascii(controls);
    return c.find("cell") != std::string::npos || c.find("dataitem") != std::string::npos;
}

std::string controls_from_view(const nlohmann::json& args, const char* fallback = "buttons,menus,editable") {
    const std::string explicit_controls = json_string(args, "controls");
    if (!explicit_controls.empty()) return explicit_controls;
    const std::string view = media::strutil::lower_ascii(json_string(args, "view"));
    if (view == "all") return "all";
    if (view == "cells" || view == "grid") return "cells,editable";
    if (view == "editable" || view == "inputs" || view == "text") return "editable";
    if (view == "buttons" || view == "actions") return "buttons,menus,editable";
    return fallback ? std::string(fallback) : std::string{};
}

bool likely_spreadsheet_args(const nlohmann::json& args) {
    const std::string hay = media::strutil::lower_ascii(json_string(args, "process") + " " + json_string(args, "title"));
    return hay.find("soffice") != std::string::npos || hay.find("libreoffice") != std::string::npos ||
           hay.find("calc") != std::string::npos || hay.find("excel") != std::string::npos;
}

ai::Query query_from_args(const nlohmann::json& args) {
    ai::Query q;
    q.foreground = json_bool(args, "foreground");
    q.process_contains = u2w(json_string(args, "process"));
    q.title_contains = u2w(json_string(args, "title"));
    const bool has_explicit_target = q.foreground || !q.process_contains.empty() || !q.title_contains.empty();
    if (!has_explicit_target) {
        if (HWND current = current_app_hwnd(args))
            q.hwnd = current;
    }
    q.limit = json_int(args, "limit", 160);
    q.probe_cells = json_bool(args, "probe_cells") ||
                    controls_mentions_cells(controls_from_view(args, "")) ||
                    likely_spreadsheet_args(args);
    return q;
}

bool parse_rect(const std::string& s, ai::Rect& r) {
    std::vector<int> nums;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == 'x' || c == 'X' || std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) { nums.push_back(std::stoi(cur)); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) nums.push_back(std::stoi(cur));
    if (nums.size() != 4) return false;
    r = {nums[0], nums[1], nums[2], nums[3]};
    return true;
}

bool control_match(const ai::ElementInfo& e, const std::string& controls_raw) {
    if (controls_raw.empty()) return true;
    std::string ct = media::strutil::lower_ascii(w2u(e.control_type));
    ct.erase(std::remove_if(ct.begin(), ct.end(), [](char c) { return c == '-' || c == '_'; }), ct.end());
    std::string token;
    auto one = [&](std::string t) {
        t.erase(std::remove_if(t.begin(), t.end(), [](char c) { return c == '-' || c == '_'; }), t.end());
        if (t.size() > 1 && t.back() == 's') t.pop_back();
        const bool is_menu = ct == "menu" || ct == "menubar" || ct == "menuitem";
        const bool is_input = ct == "edit" || ct == "document" || ct == "combobox" ||
                              ((e.has_value || e.has_text) && e.keyboard_focusable);
        if (t == "all" || t == ct) return true;
        if (t == "button" && ct == "splitbutton") return true;
        if ((t == "cell" || t == "dataitem") && ct == "dataitem") return true;
        if ((t == "input" || t == "textfield" || t == "textbox" || t == "editable") && is_input) return true;
        if (t == "text" && (ct == "text" || ct == "document" || e.has_text)) return true;
        if (t == "menu" && is_menu) return true;
        return false;
    };
    for (char c : media::strutil::lower_ascii(controls_raw) + ",") {
        if (c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c))) {
            if (!token.empty() && one(token)) return true;
            token.clear();
        } else {
            token.push_back(c);
        }
    }
    return false;
}

std::vector<ai::ElementInfo> find_elements(const nlohmann::json& args, std::string& err) {
    std::vector<ai::WindowDump> windows;
    const std::string session_id = json_string(args, "session_id");
    const bool has_target = json_bool(args, "foreground") || !json_string(args, "process").empty() || !json_string(args, "title").empty();
    if (!has_target && !session_id.empty()) {
        auto it = g_sessions.find(session_id);
        if (it == g_sessions.end()) {
            err = "session_id not found; call app_inspect_dump first or pass target";
            return {};
        }
        windows = it->second.windows;
    } else if (!has_target && session_id.empty()) {
        auto it = g_sessions.find("default");
        if (it != g_sessions.end()) {
            windows = it->second.windows;
        } else if (!ai::dump_windows(query_from_args(args), windows, err)) {
            return {};
        }
    } else if (!ai::dump_windows(query_from_args(args), windows, err)) {
        return {};
    }
    const std::wstring name = u2w(json_string(args, "name"));
    const std::wstring value = u2w(json_string(args, "value"));
    const std::wstring automation_id = u2w(json_string(args, "automation_id"));
    const std::wstring class_name = u2w(json_string(args, "class_name"));
    const std::string controls = controls_from_view(args, "buttons,menus,editable");
    std::vector<ai::ElementInfo> out;
    for (const auto& w : windows) {
        for (const auto& e : w.elements) {
            if (!control_match(e, controls)) continue;
            if (!contains_ci(e.name, name)) continue;
            if (!contains_ci(e.value, value)) continue;
            if (!contains_ci(e.automation_id, automation_id)) continue;
            if (!contains_ci(e.class_name, class_name)) continue;
            out.push_back(e);
        }
    }
    return out;
}

ExecuteResult do_app_inspect_dump(const nlohmann::json& args) {
    std::vector<ai::WindowDump> windows;
    std::string err;
    if (!ai::dump_windows(query_from_args(args), windows, err))
        return fatal("app_inspect_dump: " + err);
    const std::string fmt = media::strutil::lower_ascii(json_string(args, "format", "md"));
    nlohmann::json payload;
    const std::string session_id = json_string(args, "session_id", "default");
    auto& session = g_sessions[session_id];
    session.windows = windows;
    if (!windows.empty() && windows.front().hwnd)
        session.current_hwnd = windows.front().hwnd;
    ++session.revision;
    payload["session_id"] = session_id;
    payload["revision"] = session.revision;
    payload["windowCount"] = windows.size();
    if (fmt == "json") {
        payload["windows"] = nlohmann::json::array();
        for (const auto& w : windows) {
            nlohmann::json jw = {{"pid", w.pid}, {"hwnd", reinterpret_cast<std::uintptr_t>(w.hwnd)},
                                 {"process", w2u(w.process)}, {"title", w2u(w.title)},
                                 {"rect", rect_json(w.rect)}, {"elements", nlohmann::json::array()}};
            for (const auto& e : w.elements) jw["elements"].push_back(element_json(e));
            payload["windows"].push_back(std::move(jw));
        }
    } else {
        ai::MarkdownOptions opts;
        opts.control_types_csv = u2w(controls_from_view(args, "buttons,menus,editable"));
        opts.text_max_chars = json_int(args, "text_max_chars", 1200);
        payload["markdown"] = ai::dump_windows_markdown(windows, opts);
    }
    return tool_ok("app_inspect_dump", std::move(payload));
}

ExecuteResult do_app_inspect_find(const nlohmann::json& args) {
    std::string err;
    auto matches = find_elements(args, err);
    if (!err.empty()) return fatal("app_inspect_find: " + err);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : matches) arr.push_back(element_json(e));
    nlohmann::json payload{{"count", matches.size()}, {"matches", std::move(arr)}};
    if (const std::string session_id = json_string(args, "session_id"); !session_id.empty()) {
        payload["session_id"] = session_id;
        if (auto it = g_sessions.find(session_id); it != g_sessions.end()) payload["revision"] = it->second.revision;
    }
    const int nth = json_int(args, "nth", -1);
    if (nth >= 0 && nth < static_cast<int>(matches.size()))
        payload["selected"] = element_json(matches[static_cast<size_t>(nth)]);
    return tool_ok("app_inspect_find", std::move(payload));
}

// Forward declarations -- definitions live further down the file. Needed
// because do_app_screenshot / do_app_click now activate the target window
// before capturing / clicking (foreground-correctness fix).
HWND           resolve_target_window         (const nlohmann::json& args, std::string& err);
bool           activate_target_if_specified  (const nlohmann::json& args, HWND& used, std::string& err);
nlohmann::json window_meta_json              (HWND hwnd);

ExecuteResult do_app_screenshot(const nlohmann::json& args, const std::string& agent_path_base) {
    const std::string raw_out = json_string(args, "output_path");
    if (raw_out.empty()) return fatal("app_screenshot: 'output_path' is required");
    const std::string out_abs = resolve_tool_path_string(raw_out, agent_path_base);
    if (const std::string w = write_deny_reason(out_abs); !w.empty()) return fatal("app_screenshot: " + w);
    std::error_code ec;
    fs::create_directories(media::path::path_from_utf8(out_abs).parent_path(), ec);

    ai::Rect rect;
    HWND window_capture = nullptr;
    std::string err;
    const std::string rect_s = json_string(args, "rect");
    if (!rect_s.empty()) {
        try {
            if (!parse_rect(rect_s, rect)) return fatal("app_screenshot: rect must be x,y,w,h");
        } catch (const std::exception& ex) {
            return fatal(std::string("app_screenshot: invalid rect: ") + ex.what());
        }
    } else if (args.contains("element_index")) {
        if (!ai::find_element_rect(query_from_args(args), json_int(args, "element_index"), rect, err))
            return fatal("app_screenshot: " + err);
    } else {
        std::vector<ai::WindowDump> windows;
        if (!ai::dump_windows(query_from_args(args), windows, err)) return fatal("app_screenshot: " + err);
        if (windows.empty()) return fatal("app_screenshot: no matching window");
        window_capture = windows.front().hwnd;
        rect = windows.front().rect;
    }
    // When we resolved a window-by-query and the caller didn't ask to skip
    // activation, bring the window to foreground first. Goals:
    //   - the captured image reflects what the user (and the agent's next click)
    //     would actually see; obscured pixels are no longer guessed.
    //   - subsequent app_click / app_drag land on the same surface we just saw.
    //   - any popups / dropdowns that depend on focus render correctly.
    // Opt-out: pass {"activate": false}. Skipped for full-screen / explicit-rect
    // captures because there is no target window to activate.
    HWND activated = nullptr;
    if (window_capture && json_bool(args, "activate", true)) {
        std::string act_err;
        if (!au::activate_window(window_capture, act_err))
            return fatal("app_screenshot: " + act_err);
        activated = window_capture;
        remember_app_hwnd(args, window_capture);
        // Re-resolve rect: the window may have moved (restore from minimized)
        // or been re-Z-ordered which can shift DPI-virtualized bounds.
        RECT r{};
        if (::GetWindowRect(window_capture, &r) && r.right > r.left && r.bottom > r.top)
            rect = ai::Rect{r.left, r.top, r.right - r.left, r.bottom - r.top};
    }
    const int quality = json_int(args, "quality", 85);
    if (window_capture) {
        if (!ai::save_window_jpeg(window_capture, rect, media::path::path_from_utf8(out_abs).wstring(), quality, err))
            return fatal("app_screenshot: " + err);
    } else if (!ai::save_screen_rect_jpeg(rect, media::path::path_from_utf8(out_abs).wstring(), quality, err)) {
        return fatal("app_screenshot: " + err);
    }
    nlohmann::json payload{{"output_path", out_abs}, {"capture", window_capture ? "window" : "screen"},
                           {"rect", rect_json(rect)}};
    if (activated) payload["activated"] = window_meta_json(activated);
    return tool_ok("app_screenshot", std::move(payload));
}

ExecuteResult do_app_click(const nlohmann::json& args) {
    int x = json_int(args, "x", std::numeric_limits<int>::min());
    int y = json_int(args, "y", std::numeric_limits<int>::min());
    nlohmann::json selected;
    if (x == std::numeric_limits<int>::min() || y == std::numeric_limits<int>::min()) {
        std::string err;
        auto matches = find_elements(args, err);
        if (!err.empty()) return fatal("app_click: " + err);
        if (matches.empty()) return fatal("app_click: selector found no matching elements");
        const int nth = json_int(args, "nth", 0);
        if (nth < 0 || nth >= static_cast<int>(matches.size())) return fatal("app_click: nth is outside matches");
        const auto& e = matches[static_cast<size_t>(nth)];
        x = e.rect.x + e.rect.w / 2;
        y = e.rect.y + e.rect.h / 2;
        selected = element_json(e);
    }
    // Bring target window to foreground BEFORE clicking. Defensive against the
    // focus-stealing class of bug: if the user clicked away to another app
    // between the agent's last app_inspect_dump and now, a raw click lands on
    // whatever's on top at those screen coords (which is NOT the agent's
    // intended target). Skipped when no target hint is provided -- caller
    // accepts the current foreground.
    HWND activated = nullptr;
    std::string fg_err;
    if (!activate_target_if_specified(args, activated, fg_err))
        return fatal("app_click: " + fg_err);
    std::string err;
    const std::string button = json_string(args, "button", "left");
    const int count = json_int(args, "count", 1);
    const bool virtual_click = json_bool(args, "virtual");
    if (!au::click_point(x, y, u2w(button), count, virtual_click, err))
        return fatal("app_click: " + err);
    nlohmann::json payload{{"x", x}, {"y", y}, {"button", button}, {"count", count}, {"virtual", virtual_click}};
    if (!selected.is_null()) payload["selected"] = std::move(selected);
    if (activated) {
        remember_app_hwnd(args, activated);
        payload["activated"] = window_meta_json(activated);
    }
    return tool_ok("app_click", std::move(payload));
}

nlohmann::json window_meta_json(HWND hwnd) {
    nlohmann::json out;
    if (!hwnd || !::IsWindow(hwnd)) return out;
    wchar_t title[1024] = {};
    ::GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    out["hwnd"] = reinterpret_cast<std::uintptr_t>(hwnd);
    out["title"] = w2u(title);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid) out["pid"] = pid;
    RECT r{};
    if (::GetWindowRect(hwnd, &r) && r.right > r.left && r.bottom > r.top) {
        ai::Rect rr{r.left, r.top, r.right - r.left, r.bottom - r.top};
        out["rect"] = rect_json(rr);
    }
    return out;
}

/// Resolve a window from arg fields: hwnd / pid / title / process / foreground.
/// Returns nullptr without setting err when no target hints are present
/// (caller decides whether to fall back to the current foreground window).
HWND resolve_target_window(const nlohmann::json& args, std::string& err) {
    const std::uint64_t hwnd_raw = json_uint64(args, "hwnd");
    if (hwnd_raw) {
        HWND h = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(hwnd_raw));
        if (::IsWindow(h)) return h;
        err = "hwnd is not a valid window";
        return nullptr;
    }
    const DWORD pid = static_cast<DWORD>(json_uint64(args, "pid"));
    const std::wstring title = u2w(json_string(args, "title"));
    const std::wstring process = u2w(json_string(args, "process"));
    const bool foreground = json_bool(args, "foreground");

    if (foreground) return ::GetForegroundWindow();
    if (!pid && title.empty() && process.empty()) return nullptr;

    if (pid || !title.empty()) {
        HWND out = nullptr;
        if (au::find_window(pid, nullptr, title, out, err)) return out;
        if (process.empty()) return nullptr;
        err.clear();
    }
    ai::Query q;
    q.process_contains = process;
    q.title_contains = title;
    std::vector<ai::WindowDump> windows;
    if (!ai::dump_windows(q, windows, err)) return nullptr;
    if (windows.empty()) { err = "no matching window for selector"; return nullptr; }
    return windows.front().hwnd;
}

bool activate_target_if_specified(const nlohmann::json& args, HWND& used, std::string& err) {
    const bool process_only = current_app_hwnd(args) && !json_string(args, "process").empty() &&
                              json_uint64(args, "pid") == 0 && json_uint64(args, "hwnd") == 0 &&
                              json_string(args, "title").empty() && !json_bool(args, "foreground");
    HWND target = process_only ? current_app_hwnd(args) : resolve_target_window(args, err);
    if (!target && err.empty())
        target = current_app_hwnd(args);
    if (!target) {
        if (!err.empty()) return false;
        used = nullptr;
        return true;
    }
    if (!au::activate_window(target, err)) return false;
    used = target;
    remember_app_hwnd(args, target);
    return true;
}

ExecuteResult do_app_open(const nlohmann::json& args) {
    au::LaunchOptions opts;
    opts.exe = u2w(json_string(args, "exe"));
    if (opts.exe.empty())
        opts.exe = u2w(json_string(args, "process"));
    if (opts.exe.empty()) return fatal("app_open: 'exe' is required");
    opts.args = u2w(json_string(args, "args"));
    opts.cwd = u2w(json_string(args, "cwd"));
    opts.x = json_int(args, "x", 0);
    opts.y = json_int(args, "y", 0);
    opts.w = json_int(args, "width", 0);
    opts.h = json_int(args, "height", 0);
    opts.wait_ms = json_int(args, "wait_ms", 3000);

    au::LaunchResult res;
    std::string err;
    if (!au::open_app(opts, res, err)) return fatal("app_open: " + err);

    nlohmann::json payload{{"pid", res.pid}};
    if (res.hwnd) {
        remember_app_hwnd(args, res.hwnd);
        auto meta = window_meta_json(res.hwnd);
        if (meta.contains("hwnd")) payload["hwnd"] = meta["hwnd"];
        if (meta.contains("title")) payload["title"] = meta["title"];
        if (meta.contains("rect")) payload["rect"] = meta["rect"];
    } else {
        payload["note"] = "no main window resolved within wait_ms; UWP / launcher app — locate by title with app_inspect_dump";
    }
    return tool_ok("app_open", std::move(payload));
}

ExecuteResult do_app_type(const nlohmann::json& args) {
    const std::string text = json_string(args, "text");
    if (text.empty()) return fatal("app_type: 'text' is required");
    HWND used = nullptr;
    std::string err;
    if (!activate_target_if_specified(args, used, err)) return fatal("app_type: " + err);
    if (!au::type_text(u2w(text), err)) return fatal("app_type: " + err);
    nlohmann::json payload{{"chars", text.size()}};
    if (used) {
        remember_app_hwnd(args, used);
        payload["activated"] = window_meta_json(used);
    }
    return tool_ok("app_type", std::move(payload));
}

ExecuteResult do_app_hotkey(const nlohmann::json& args) {
    const std::string keys = json_string(args, "keys");
    if (keys.empty()) return fatal("app_hotkey: 'keys' is required");
    HWND used = nullptr;
    std::string err;
    if (!activate_target_if_specified(args, used, err)) return fatal("app_hotkey: " + err);
    const auto parts = au::split_keys(u2w(keys));
    if (parts.empty()) return fatal("app_hotkey: keys did not parse");
    if (!au::send_hotkey(parts, err)) return fatal("app_hotkey: " + err);
    nlohmann::json payload{{"keys", keys}};
    if (used) {
        remember_app_hwnd(args, used);
        payload["activated"] = window_meta_json(used);
    }
    return tool_ok("app_hotkey", std::move(payload));
}

ExecuteResult do_app_drag(const nlohmann::json& args) {
    HWND used = nullptr;
    std::string err;
    if (!activate_target_if_specified(args, used, err)) return fatal("app_drag: " + err);

    // Resolve a (x|xw, y|yw)-style coord against the just-activated window's rect.
    RECT used_rect{};
    const bool have_rect = used && ::GetWindowRect(used, &used_rect);
    auto resolve = [&](const nlohmann::json& obj,
                       const char* x_abs, const char* y_abs,
                       const char* x_win, const char* y_win,
                       int& out_x, int& out_y) -> bool {
        const bool has_abs = obj.contains(x_abs) || obj.contains(y_abs);
        const bool has_win = obj.contains(x_win) || obj.contains(y_win);
        if (!has_abs && !has_win) return false;
        out_x = json_int(obj, x_abs);
        out_y = json_int(obj, y_abs);
        if (has_win) {
            if (!have_rect) {
                err = "window-relative coords need an activated window (pass title/pid/hwnd to activate first)";
                return false;
            }
            if (obj.contains(x_win)) out_x = used_rect.left + json_int(obj, x_win);
            if (obj.contains(y_win)) out_y = used_rect.top  + json_int(obj, y_win);
        }
        return true;
    };

    // Build the path. Priority: arc > to/dx,dy.
    std::vector<au::Point> path;
    const char*            mode = nullptr;

    if (args.contains("arc") && args["arc"].is_object()) {
        const auto& a = args["arc"];
        int cx = 0, cy = 0;
        if (!resolve(a, "cx", "cy", "cxw", "cyw", cx, cy))
            return fatal("app_drag: arc requires center (cx/cy or cxw/cyw)");
        const double radius = static_cast<double>(json_int(a, "radius"));
        if (radius < 1.0) return fatal("app_drag: arc 'radius' must be > 0");
        const double start_deg = static_cast<double>(json_int(a, "start_deg", json_int(a, "startDeg", 0)));
        const double end_deg   = static_cast<double>(json_int(a, "end_deg",   json_int(a, "endDeg", 360)));
        const double sweep     = end_deg - start_deg;
        if (std::abs(sweep) < 0.5) return fatal("app_drag: arc sweep is zero (start_deg == end_deg)");
        int segments = json_int(a, "segments");
        if (segments <= 0) segments = std::max(8, std::min(120, static_cast<int>(std::abs(sweep) / 8.0)));
        if (segments > 360) segments = 360;
        constexpr double kPi = 3.14159265358979323846;
        path.reserve(static_cast<std::size_t>(segments + 1));
        for (int i = 0; i <= segments; ++i) {
            const double t   = start_deg + (sweep * i) / segments;
            const double rad = (t * kPi) / 180.0;
            path.push_back({cx + static_cast<int>(std::cos(rad) * radius),
                            cy + static_cast<int>(std::sin(rad) * radius)});
        }
        mode = "arc";
    } else {
        int x1 = 0, y1 = 0;
        if (!resolve(args, "x", "y", "xw", "yw", x1, y1))
            return fatal("app_drag: start point required (x,y or xw,yw)");
        int x2 = std::numeric_limits<int>::min();
        int y2 = std::numeric_limits<int>::min();
        if (args.contains("to_x") || args.contains("to_y") || args.contains("toXw") || args.contains("toYw")) {
            nlohmann::json end_obj;
            if (args.contains("to_x"))  end_obj["x"]  = args["to_x"];
            if (args.contains("to_y"))  end_obj["y"]  = args["to_y"];
            if (args.contains("toXw")) end_obj["xw"] = args["toXw"];
            if (args.contains("toYw")) end_obj["yw"] = args["toYw"];
            if (!resolve(end_obj, "x", "y", "xw", "yw", x2, y2))
                return fatal("app_drag: end point invalid");
        } else if (args.contains("dx") && args["dx"].is_number_integer()) {
            x2 = x1 + args["dx"].get<int>();
            y2 = y1 + (args.value("dy", 0));
        } else if (args.contains("dy") && args["dy"].is_number_integer()) {
            x2 = x1 + (args.value("dx", 0));
            y2 = y1 + args["dy"].get<int>();
        } else {
            return fatal("app_drag: end point required (to_x/to_y, toXw/toYw, dx/dy, or arc)");
        }
        if (x2 == x1 && y2 == y1)
            return fatal("app_drag: end point equals start");
        path = {{x1, y1}, {x2, y2}};
        mode = "linear";
    }

    const std::string button      = json_string(args, "button", "left");
    const int         duration_ms = json_int(args, "duration_ms", json_int(args, "durationMs", 250));
    if (duration_ms < 0 || duration_ms > 30000)
        return fatal("app_drag: 'duration_ms' must be in [0,30000]");

    if (!au::drag_path(path, u2w(button), duration_ms, err))
        return fatal("app_drag: " + err);

    nlohmann::json payload{
        {"mode",        mode},
        {"from",        {{"x", path.front().x}, {"y", path.front().y}}},
        {"to",          {{"x", path.back().x},  {"y", path.back().y}}},
        {"points",      static_cast<int>(path.size())},
        {"button",      button},
        {"duration_ms", duration_ms},
    };
    if (used) {
        remember_app_hwnd(args, used);
        payload["activated"] = window_meta_json(used);
    }
    return tool_ok("app_drag", std::move(payload));
}

ExecuteResult do_app_batch(const nlohmann::json& args) {
    // Accept either {steps: [...]}, {actions: [...]}, stringified JSON arrays,
    // or a bare array. Models often recover from a schema miss by passing the
    // right JSON as a string; make that executable instead of causing a loop.
    auto normalize_steps_value = [](const nlohmann::json& v, std::string& err) -> nlohmann::json {
        if (v.is_array()) return v;
        if (!v.is_string()) return {};
        const std::string raw = v.get<std::string>();
        try {
            nlohmann::json parsed = nlohmann::json::parse(raw);
            if (parsed.is_array()) return parsed;
            if (parsed.is_object() && parsed.contains("steps") && parsed["steps"].is_array())
                return parsed["steps"];
            if (parsed.is_object() && parsed.contains("actions") && parsed["actions"].is_array())
                return parsed["actions"];
            err = "string value did not parse to an array, {steps:[...]}, or {actions:[...]}";
            return {};
        } catch (const std::exception& ex) {
            const auto first = raw.find_first_not_of(" \t\r\n");
            const bool looks_array = first != std::string::npos && raw[first] == '[';
            const bool looks_object = first != std::string::npos && raw[first] == '{';
            const auto end = looks_array ? raw.find_last_of(']') : (looks_object ? raw.find_last_of('}') : std::string::npos);
            if (end != std::string::npos && end + 1 < raw.size()) {
                try {
                    nlohmann::json parsed = nlohmann::json::parse(raw.substr(0, end + 1));
                    if (parsed.is_array()) return parsed;
                    if (parsed.is_object() && parsed.contains("steps") && parsed["steps"].is_array())
                        return parsed["steps"];
                    if (parsed.is_object() && parsed.contains("actions") && parsed["actions"].is_array())
                        return parsed["actions"];
                } catch (...) {
                }
            }
            err = std::string("invalid JSON string: ") + ex.what();
            return {};
        }
    };

    nlohmann::json doc;
    std::string normalize_err;
    if (args.is_array()) {
        doc = nlohmann::json{{"steps", args}};
    } else if (args.contains("steps")) {
        nlohmann::json steps = normalize_steps_value(args["steps"], normalize_err);
        if (steps.is_array()) doc = nlohmann::json{{"steps", std::move(steps)}};
    } else if (args.contains("actions")) {
        nlohmann::json steps = normalize_steps_value(args["actions"], normalize_err);
        if (steps.is_array()) doc = nlohmann::json{{"steps", std::move(steps)}};
    }
    if (doc.empty()) {
        if (!normalize_err.empty())
            return fatal("app_batch: " + normalize_err);
    }
    if (doc.empty()) {
        if (args.is_string()) {
            nlohmann::json steps = normalize_steps_value(args, normalize_err);
            if (steps.is_array()) doc = nlohmann::json{{"steps", std::move(steps)}};
            else if (!normalize_err.empty()) return fatal("app_batch: " + normalize_err);
        }
    }
    if (!doc.empty()) {
        // Continue below.
    } else {
        // Distinguish "empty object" (almost always output truncation) from "wrong keys" so the agent
        // gets actionable feedback instead of looping with the same plan.
        const bool empty_obj = args.is_object() && args.empty();
        if (empty_obj) {
            return fatal(
                "app_batch: arguments were empty ({}). This almost always means the assistant response "
                "was truncated mid-call (max_output_tokens hit). Retry with a SHORTER plan: split into "
                "two app_batch calls, drop optional fields, or shorten any large 'text' values.");
        }
        return fatal(
            "app_batch: pass {steps:[...]} (or actions:[...]) with batch actions. "
            "Got keys: " + [&]{ std::string k; for (auto it=args.begin(); it!=args.end(); ++it) { if(!k.empty()) k+=","; k+=it.key(); } return k.empty()?std::string("<none>"):k; }());
    }

    {
        auto it = g_sessions.find(app_session_id_from_args(args));
        const bool has_inspect_context = it != g_sessions.end() && it->second.revision > 0;
        if (!has_inspect_context && doc.contains("steps") && doc["steps"].is_array()) {
            bool needs_inspect = false;
            for (const auto& step : doc["steps"]) {
                if (!step.is_object()) continue;
                const std::string action = media::strutil::lower_ascii(step.value("action", step.value("type", std::string{})));
                if (action == "click" || action == "drag" || action == "type" || action == "hotkey" ||
                    action == "key-press" || action == "key-down" || action == "key-up" ||
                    action == "mouse-move" || action == "mouse-down" || action == "mouse-up" ||
                    action == "scroll" || action == "click-element" || action == "wait-element") {
                    needs_inspect = true;
                    break;
                }
            }
            if (needs_inspect) {
                return fatal(
                    "app_batch: call app_inspect_dump first for this app session before batching UI "
                    "targeting actions. This provides the private target hwnd and auditable control "
                    "metadata; then retry app_batch without guessing toolbar/window coordinates.");
            }
        }
    }

    // Allow per-step "type" alias for "action" (lenient batch schema TODO #5).
    for (auto& step : doc["steps"]) {
        if (step.is_object() && !step.contains("action") && step.contains("type") && step["type"].is_string()) {
            step["action"] = step["type"];
        }
    }

    ab::RunOptions opts;
    opts.default_delay_ms = json_int(args, "default_delay_ms", json_int(args, "defaultDelayMs", 50));
    opts.default_wait_timeout_ms = json_int(args, "default_wait_timeout_ms",
                                            json_int(args, "defaultWaitTimeoutMs", 5000));
    opts.default_wait_interval_ms = json_int(args, "default_wait_interval_ms",
                                             json_int(args, "defaultWaitIntervalMs", 100));
    opts.stop_on_error = !json_bool(args, "continue_on_error", json_bool(args, "continueOnError", false));
    {
        std::string target_err;
        HWND target = resolve_target_window(args, target_err);
        if (!target && target_err.empty())
            target = current_app_hwnd(args);
        if (target)
            opts.initial_hwnd = reinterpret_cast<std::uintptr_t>(target);
    }

    nlohmann::json report;
    std::string err;
    const bool ok = ab::run_json_batch(doc, opts, report, err);
    nlohmann::json payload{
        {"steps",   report.value("steps", nlohmann::json::array())},
        {"all_ok",  ok},
    };
    if (!err.empty()) payload["error"] = err;
    if (!ok) {
        // Surface the failing step so the agent can self-correct without another tool call.
        for (const auto& s : payload["steps"]) {
            if (!s.value("ok", true)) { payload["first_failure"] = s; break; }
        }
    }
    // Don't bail the agent loop on a partial-failure batch — return a structured report.
    return tool_ok("app_batch", std::move(payload));
}

ExecuteResult do_app_close(const nlohmann::json& args) {
    const bool force = json_bool(args, "force");

    std::string err;
    HWND target = resolve_target_window(args, err);
    DWORD pid = static_cast<DWORD>(json_uint64(args, "pid"));
    if (target && pid == 0) ::GetWindowThreadProcessId(target, &pid);

    if (!target && !pid)
        return fatal("app_close: requires hwnd, pid, process, title, or foreground");
    if (!target && pid && !force && !err.empty())
        return fatal("app_close: " + err);

    if (force) {
        if (!pid) return fatal("app_close: force requires a resolvable pid");
        HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (!h) return fatal("app_close: OpenProcess failed");
        const BOOL ok = ::TerminateProcess(h, 0);
        ::CloseHandle(h);
        if (!ok) return fatal("app_close: TerminateProcess failed");
        return tool_ok("app_close", {{"pid", pid}, {"method", "terminate"}});
    }

    if (!target) return fatal("app_close: no window resolved for graceful close (try force: true with pid)");
    if (!::PostMessageW(target, WM_CLOSE, 0, 0))
        return fatal("app_close: PostMessage(WM_CLOSE) failed");
    nlohmann::json payload{{"hwnd", reinterpret_cast<std::uintptr_t>(target)}, {"method", "wm_close"}};
    if (pid) payload["pid"] = pid;
    return tool_ok("app_close", std::move(payload));
}
#endif

nlohmann::json parse_schema(const char* schema) {
    return nlohmann::json::parse(schema);
}

ToolDef make_app_inspect_dump() {
    return ToolDef{
        "app_inspect_dump",
        "Observe a desktop app and cache a session tree for later app_inspect_find/app_click calls. "
        "Default output is compact Markdown of actionable/editable controls, not a full dump. "
        "Use view='cells' for spreadsheet cells, view='all' only when the user explicitly needs every visible control.",
        parse_schema(R"json({
            "type": "object",
            "properties": {
                "target": {
                    "type": "object",
                    "description": "Preferred target selector.",
                    "properties": {
                        "process": {"type": "string", "description": "Process-name substring, e.g. wordpad.exe, soffice."},
                        "title": {"type": "string", "description": "Window-title substring."},
                        "foreground": {"type": "boolean", "description": "Observe the foreground window."}
                    }
                },
                "session_id": {"type": "string", "description": "Cache key for this app tree. Default 'default'."},
                "view": {"type": "string", "enum": ["actions", "editable", "cells", "all"], "description": "What to show. Default actions = buttons, menus, editable fields. Avoid all unless necessary."},
                "format": {"type": "string", "enum": ["md", "json"], "description": "Default md. json can be large."},
                "limit": {"type": "integer", "minimum": 1, "maximum": 10000, "description": "Max elements to inspect/cache. Default 160."},
                "text_max_chars": {"type": "integer", "minimum": 20, "maximum": 200000, "description": "Max chars per text/value field. Default 1200."},
                "probe_cells": {"type": "boolean", "description": "Force slower visible-cell probing. Auto-enabled for view=cells/spreadsheet targets."},
                "process": {"type": "string", "description": "Backward-compatible alias for target.process."},
                "title": {"type": "string", "description": "Backward-compatible alias for target.title."},
                "foreground": {"type": "boolean", "description": "Backward-compatible alias for target.foreground."},
                "controls": {"type": "string", "description": "Advanced raw control filter: buttons,menus,editable,cells,all."}
            }
        })json")};
}

ToolDef make_app_inspect_find() {
    return ToolDef{
        "app_inspect_find",
        "Find targetable elements. Prefer querying the cached session from app_inspect_dump by session_id; "
        "pass target only when no session exists or the app changed. Returns rect and center for clicking. "
        "SELECTORS: Use 'ref' for stable element targeting across sessions (returned from wait-element/click-element). "
        "Use 'automation_id' for programmatic IDs (exact match, highest priority). "
        "Use 'name' with 'nth' for UI labels. "
        "MATCHING: Scores exact automation_id (500) > exact name (400) > word-boundary name (300) > prefix (200) > contains (100). "
        "Multiple matches are returned sorted by score; use 'nth' to select a specific match.",
        parse_schema(R"json({
            "type": "object",
            "properties": {
                "session_id": {"type": "string", "description": "Use cached tree from app_inspect_dump. If omitted and no target is given, uses 'default' if present."},
                "query": {
                    "type": "object",
                    "description": "Preferred element selector.",
                    "properties": {
                        "view": {"type": "string", "enum": ["actions", "editable", "cells", "all"]},
                        "name": {"type": "string", "description": "Case-insensitive name substring, e.g. File, B4, Rich Text Window."},
                        "value": {"type": "string", "description": "Case-insensitive value substring."},
                        "automation_id": {"type": "string", "description": "AutomationId property - exact or substring match, highest priority."},
                        "class_name": {"type": "string", "description": "ClassName property for native controls."},
                        "controlType": {"type": "string", "description": "Control type filter e.g. Button, Edit, MenuItem."},
                        "ref": {"type": "string", "description": "Stable reference ID from prior wait-element/click-element. Format: 'ControlType:name_hash:automation_id_hash:nth'."},
                        "nth": {"type": "integer", "minimum": 0, "description": "Select the n-th best match by score (0=first/best)."},
                        "minScore": {"type": "integer", "minimum": 0, "maximum": 1000, "description": "Minimum match score threshold. Default 1 (any match)."}
                    }
                },
                "target": {
                    "type": "object",
                    "description": "Optional fresh target if not using session.",
                    "properties": {
                        "process": {"type": "string"},
                        "title": {"type": "string"},
                        "foreground": {"type": "boolean"}
                    }
                },
                "nth": {"type": "integer", "minimum": 0, "description": "Optional zero-based match index to return as selected."},
                "limit": {"type": "integer", "minimum": 1, "maximum": 10000, "description": "Only used for fresh inspect. Default 160."},
                "probe_cells": {"type": "boolean"},
                "view": {"type": "string", "enum": ["actions", "editable", "cells", "all"], "description": "Backward-compatible alias for query.view."},
                "controls": {"type": "string", "description": "Advanced raw control filter."},
                "name": {"type": "string", "description": "Backward-compatible alias for query.name."},
                "value": {"type": "string", "description": "Backward-compatible alias for query.value."},
                "automation_id": {"type": "string", "description": "Backward-compatible alias for query.automation_id."},
                "class_name": {"type": "string", "description": "Backward-compatible alias for query.class_name."},
                "ref": {"type": "string", "description": "Backward-compatible alias for query.ref."},
                "process": {"type": "string", "description": "Backward-compatible alias for target.process."},
                "title": {"type": "string", "description": "Backward-compatible alias for target.title."}
            }
        })json")};
}

ToolDef make_app_screenshot() {
    return ToolDef{
        "app_screenshot",
        "Capture a desktop app window, element, or explicit screen rectangle as JPEG. "
        "When the target is resolved by window query (title / process / pid / foreground), the window is "
        "brought to the foreground first by default so the capture matches what the user actually sees and "
        "so a subsequent app_click lands on the same surface. Pass activate=false to skip foregrounding "
        "(non-intrusive capture; obscured pixels are best-effort).",
        parse_schema(R"json({
            "type": "object",
            "properties": {
                "output_path": {"type": "string", "description": "Destination .jpg path. Relative paths resolve to the agent folder."},
                "process": {"type": "string"},
                "title": {"type": "string"},
                "foreground": {"type": "boolean"},
                "activate": {"type": "boolean", "description": "Bring target window to foreground before capture. Default true for window queries; ignored for explicit rect / element_index."},
                "element_index": {"type": "integer", "minimum": 0, "description": "Element index from a prior dump/find."},
                "rect": {"type": "string", "description": "Screen rectangle x,y,w,h."},
                "quality": {"type": "integer", "minimum": 1, "maximum": 100}
            },
            "required": ["output_path"]
        })json")};
}

ToolDef make_app_click() {
    return ToolDef{
        "app_click",
        "Click a screen coordinate, a window-relative coordinate, or find an app element and click its center. "
        "Prefer session_id + query over raw coordinates. If app_inspect_dump was called first, omit target and use the cached tree. "
        "Pass title / pid / hwnd / process / foreground to first bring that window to the foreground before the click "
        "(strongly recommended -- defends against the user clicking away to another app between dump and click). "
        "PREFER xw,yw (window-relative, origin = top-left of the activated window) over x,y — those survive the window being moved. "
        "SELECTOR MODE: Pass ref (from wait-element/click-element) for the most stable targeting. Or use "
        "automation_id (highest match priority) > exact name > name word-boundary > name prefix > name contains.",
        parse_schema(R"json({
            "type": "object",
            "properties": {
                "x": {"type": "integer", "description": "Screen x (absolute). Use xw instead when possible."},
                "y": {"type": "integer", "description": "Screen y (absolute). Use yw instead when possible."},
                "xw": {"type": "integer", "description": "Window-relative x. Origin = top-left of the target window. Preferred."},
                "yw": {"type": "integer", "description": "Window-relative y. Origin = top-left of the target window. Preferred."},
                "session_id": {"type": "string", "description": "Cached tree id from app_inspect_dump."},
                "query": {
                    "type": "object",
                    "description": "Element selector for click-by-element.",
                    "properties": {
                        "view": {"type": "string", "enum": ["actions", "editable", "cells", "all"]},
                        "ref": {"type": "string", "description": "Stable reference ID from prior element interaction. Format: 'ControlType:name_hash:automation_id_hash:nth'. Highest priority."},
                        "name": {"type": "string", "description": "Element name substring. Case-insensitive."},
                        "value": {"type": "string", "description": "Element value substring."},
                        "automation_id": {"type": "string", "description": "AutomationId property - exact match bonus."},
                        "class_name": {"type": "string", "description": "ClassName property."},
                        "controlType": {"type": "string", "description": "Control type e.g. Button, Edit, MenuItem."},
                        "nth": {"type": "integer", "minimum": 0, "description": "Select n-th best match by score (0=first)."}
                    }
                },
                "process": {"type": "string"},
                "title": {"type": "string"},
                "foreground": {"type": "boolean"},
                "controls": {"type": "string", "description": "Control filter for selector mode."},
                "ref": {"type": "string", "description": "Backward-compatible alias for query.ref."},
                "name": {"type": "string", "description": "Backward-compatible alias for query.name."},
                "value": {"type": "string", "description": "Backward-compatible alias for query.value."},
                "automation_id": {"type": "string", "description": "Backward-compatible alias for query.automation_id."},
                "class_name": {"type": "string", "description": "Backward-compatible alias for query.class_name."},
                "nth": {"type": "integer", "minimum": 0, "description": "Zero-based match index for selector mode. Default 0."},
                "button": {"type": "string", "enum": ["left", "right", "middle"]},
                "count": {"type": "integer", "minimum": 1, "maximum": 10},
                "virtual": {"type": "boolean", "description": "Post mouse messages instead of moving the physical cursor."},
                "probe_cells": {"type": "boolean"}
            }
        })json")};
}

ToolDef make_app_open() {
    return ToolDef{
        "app_open",
        "Launch a desktop application and optionally place its window. "
        "Returns pid and (when resolved) hwnd, title, and rect for the new window. "
        "Prefer this over running 'notepad.exe' / 'calc.exe' through the run tool: "
        "shell quoting issues and UWP launchers are handled here.",
        parse_schema(R"json({
            "type": "object",
            "properties": {
                "exe": {"type": "string", "description": "Executable name on PATH (notepad.exe, calc.exe, soffice.exe) or absolute path."},
                "args": {"type": "string", "description": "Optional command-line arguments as a single string."},
                "cwd": {"type": "string", "description": "Optional working directory."},
                "x": {"type": "integer", "description": "Optional window x position (screen px)."},
                "y": {"type": "integer", "description": "Optional window y position (screen px)."},
                "width": {"type": "integer", "minimum": 0, "description": "Optional window width."},
                "height": {"type": "integer", "minimum": 0, "description": "Optional window height."},
                "wait_ms": {"type": "integer", "minimum": 0, "maximum": 60000, "description": "How long to wait for the main window. Default 3000."}
            },
            "required": ["exe"]
        })json")};
}

ToolDef make_app_type() {
    return ToolDef{
        "app_type",
        "Type Unicode text into the currently focused control. "
        "Pass pid / hwnd / title / process / foreground to first activate that window. "
        "If no target is given, types into whatever has focus right now.",
        parse_schema(R"json({
            "type": "object",
            "properties": {
                "text": {"type": "string", "description": "The text to type. Use \\n for newlines."},
                "pid": {"type": "integer", "minimum": 0, "description": "Optional pid to activate first."},
                "hwnd": {"type": "integer", "description": "Optional HWND (as integer) to activate first."},
                "title": {"type": "string", "description": "Optional window-title substring to activate first."},
                "process": {"type": "string", "description": "Optional process-name substring (used when no pid/title)."},
                "foreground": {"type": "boolean", "description": "If true, activate the current foreground window first."}
            },
            "required": ["text"]
        })json")};
}

ToolDef make_app_hotkey() {
    return ToolDef{
        "app_hotkey",
        "Send a hotkey combination to the focused (or activated) window. "
        "Keys are joined with '+', e.g. 'ctrl+s', 'alt+f4', 'ctrl+shift+t', 'enter', 'win+r'. "
        "Same target selectors as app_type.",
        parse_schema(R"json({
            "type": "object",
            "properties": {
                "keys": {"type": "string", "description": "Hotkey spec like 'ctrl+s' or 'alt+f4'. Single keys like 'enter' / 'esc' are accepted."},
                "pid": {"type": "integer", "minimum": 0},
                "hwnd": {"type": "integer"},
                "title": {"type": "string"},
                "process": {"type": "string"},
                "foreground": {"type": "boolean"}
            },
            "required": ["keys"]
        })json")};
}

ToolDef make_app_drag() {
    return ToolDef{
        "app_drag",
        "Press the mouse button, walk along a path, release. Use app_batch with action:\"drag\" for richer paths. "
        "This single-action variant supports straight-line drags (to/dx,dy) plus arc:{...} for circles (O's, loops). "
        "For polylines / signatures / handwriting / multi-segment paths use app_batch's drag step with path:[{...},...]. "
        "Coordinates accept absolute (x,y) or window-relative (xw,yw). Prefer xw,yw — survives window moves. "
        "Drawing apps like Paint need duration_ms >= 250 or strokes can be silently lost.",
        parse_schema(R"json({
            "type": "object",
            "properties": {
                "x": {"type": "integer", "description": "Start screen x (absolute). Use xw instead when possible."},
                "y": {"type": "integer", "description": "Start screen y (absolute). Use yw instead when possible."},
                "xw": {"type": "integer", "description": "Start window-relative x. Origin = top-left of target window. Preferred."},
                "yw": {"type": "integer", "description": "Start window-relative y. Origin = top-left of target window. Preferred."},
                "to_x": {"type": "integer", "description": "End screen x. Use this OR dx OR arc."},
                "to_y": {"type": "integer", "description": "End screen y. Use this OR dy OR arc."},
                "dx": {"type": "integer", "description": "Horizontal delta from start. Use this OR to_x OR arc."},
                "dy": {"type": "integer", "description": "Vertical delta from start. Use this OR to_y OR arc."},
                "arc": {"type": "object", "description": "Draw a circular arc as the drag path. Fields: cx,cy (or cxw,cyw window-relative), radius, start_deg, end_deg (0=+x, 90=+y/down - for a full O: start_deg:0, end_deg:360), segments (optional, default ~one sample per 8 deg)."},
                "button": {"type": "string", "enum": ["left", "right", "middle"], "description": "Default 'left'."},
                "steps": {"type": "integer", "minimum": 1, "maximum": 200, "description": "Legacy hint - paths are now auto-sampled at one event per ~16 ms frame."},
                "duration_ms": {"type": "integer", "minimum": 0, "maximum": 30000, "description": "Total drag wall-time in ms. Default 250."},
                "title": {"type": "string", "description": "Optional window-title substring to activate first."},
                "pid": {"type": "integer", "minimum": 0, "description": "Optional pid to activate first."},
                "hwnd": {"type": "integer", "description": "Optional HWND (as integer) to activate first."},
                "process": {"type": "string", "description": "Optional process-name substring."},
                "foreground": {"type": "boolean", "description": "Activate the foreground window first."}
            },
            "required": ["x", "y"]
        })json")};
}

ToolDef make_app_batch() {
    return ToolDef{
        "app_batch",
        "Execute a sequence of UI actions in ONE tool call \u2014 use for any multi-step gesture "
        "(drawing strokes, menu-open + item-click, activate+select-all+type, playing notes). "
        "Far fewer round-trips/tokens than N separate calls. "
        "STEP ACTIONS: open-app, activate, click, drag, mouse-move, mouse-down, mouse-up, scroll, type, "
        "hotkey, key-down, key-up, key-press, wait, wait-element, click-element, clipboard-set, "
        "clipboard-get, screenshot. Each step takes the same fields as the matching single-action tool "
        "plus optional delayMs (sleep after the step). Per-step 'type' aliases 'action'. "

        "COORDINATES: any x,y step also accepts xw,yw (window-relative to the last-activated window). "
        "PREFER xw,yw \u2014 they survive the window moving/resizing; xw/yw win when both are given. In "
        "inspect Markdown, center=x,y is absolute (for x/y) and win_center=xw,yw is window-relative (for xw/yw). "

        "DISCOVERY: for unfamiliar UI, call app_inspect_dump / app_inspect_find first (cheaper, returns "
        "targetable controls; use view:'all' for palettes/colors). Do not guess control names/coordinates "
        "in the first batch; if a swatch has no accessible name and you must use raw coords, add "
        "note:'red swatch' for an auditable log. For inspect-first tasks call app_open, then "
        "app_inspect_dump, then app_batch (do not hide open-app in the first batch). Screenshots are for "
        "visual verification, drawing, and final artifacts \u2014 write image files directly; do NOT open "
        "the File/Save UI unless the user wants the app document saved. "

        "INPUT MODALITY (critical): before batching many steps in an unfamiliar canvas/custom app, PROBE "
        "with one click or key-press, then verify via screenshot/inspect; if nothing changed you picked the "
        "wrong modality. Many multimedia apps (drum kits, virtual pianos, synths, rhythm games, emulators) "
        "are KEYBOARD-driven \u2014 tells: bracketed hotkeys like \"Snare [B]\", \"Press A to play\", "
        "QWERTY-to-notes. Use key-press / key-down / key-up, NOT click. "

        "virtual:true CAVEAT: uses PostMessage and works ONLY for standard Win32 controls (buttons, menus, "
        "lists, edits, dialogs). It is SILENTLY DROPPED by canvases, games, pianos, browsers, and "
        "Electron/Qt/WPF custom controls (returns ok:true with no effect). If a click reported ok but "
        "nothing changed, retry with virtual:false (or keyboard). "

        "SCHEDULING (rhythm / melody / sequenced onsets, any action): each step accepts at_ms (absolute "
        "from batch start) or offset_ms (relative to the previous step's onset); at_ms wins. Use at_ms for "
        "transcribed melodies / beat grids, offset_ms for tempo-by-ear (120 BPM 8th = offset_ms:250). When "
        "the next step is scheduled, this step's default_delay_ms is suppressed. The report gives "
        "scheduled_at_ms / started_at_ms / late_ms; late_ms > ~30 means the step body overran (shorten "
        "hold_ms, set move_duration_ms:0, or virtual:true). "

        "MOUSE TIMING: click/mouse-move/drag/scroll animate smoothly (~4000 px/s) so hover/preview/brush "
        "state fires; set move_duration_ms:0 to teleport. "

        "DRAWING: each drag is ONE continuous stroke (down at first point \u2192 last point up). For a "
        "cross/X/plus use TWO drag steps (one per stroke) \u2014 a single multi-point path connects points "
        "into odd geometry. Select the requested color explicitly before drawing. "

        "DRAG flavours (priority): (1) path:[{x|xw,y|yw},...] \u2014 explicit stroke; >2 points rejected "
        "unless continuous:true. (2) arc:{cx|cxw,cy|cyw,radius,start_deg,end_deg,segments?} for O's/loops "
        "(0\u00b0=+x, 90\u00b0=down). (3) to/toX,toY or dx,dy point-to-point. duration_ms is total motion. "

        "SCROLL: wheel at the cursor by default; pass x,y or xw,yw to position first, axis:\"h\" for "
        "horizontal; clicks>0 scrolls up/right (Win32 convention). "

        "KEYBOARD: key-press {key, hold_ms, modifiers[]} \u2014 hold_ms is key-down time (note sustain), "
        "delayMs is the rest after release. key-down/key-up are the low-level pair for chords or "
        "overlapping notes. "

        "SELECTOR STEPS (wait-element, click-element): prefer 'ref' (from a prior element step) for "
        "stability; then automation_id exact > name exact > name word-boundary > name prefix > name "
        "contains. Use 'nth' to pick among matches.",
        parse_schema(R"json({
            "type": "object",
            "properties": {
                "steps": {"type": "array", "description": "Array of step objects, executed in order. Each has an 'action' (or 'type') field."},
                "actions": {"type": "array", "description": "Alias for steps."},
                "default_delay_ms": {"type": "integer", "minimum": 0, "maximum": 60000, "description": "Sleep added after each step that does not set delayMs. Default 50."},
                "continue_on_error": {"type": "boolean", "description": "If true, run remaining steps even after one fails. Default false."}
            }
        })json")};
}

ToolDef make_app_close() {
    return ToolDef{
        "app_close",
        "Close a desktop window safely. Default sends WM_CLOSE (the app may show a save prompt). "
        "Pass force: true (with pid) to TerminateProcess instead. "
        "Resolve the target by hwnd, pid, or process/title selector — never close the foreground "
        "blindly, as that can hit the wrong window.",
        parse_schema(R"json({
            "type": "object",
            "properties": {
                "pid": {"type": "integer", "minimum": 0, "description": "Process id (e.g. from app_open)."},
                "hwnd": {"type": "integer", "description": "Window handle (as integer) from a prior dump/find."},
                "title": {"type": "string", "description": "Window-title substring (used with pid or alone)."},
                "process": {"type": "string", "description": "Process-name substring (used when no pid/hwnd)."},
                "foreground": {"type": "boolean", "description": "Close the current foreground window. Use sparingly."},
                "force": {"type": "boolean", "description": "TerminateProcess instead of WM_CLOSE (requires pid). Default false."}
            }
        })json")};
}

} // namespace

std::vector<ToolDef> tool_defs() {
#if defined(_WIN32) && defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT && defined(FEATURE_AGENT_COMPUTER_USE) && FEATURE_AGENT_COMPUTER_USE
    return {make_app_inspect_dump(), make_app_inspect_find(), make_app_screenshot(), make_app_click(),
            make_app_drag(),  make_app_open(),          make_app_type(),         make_app_hotkey(),
            make_app_close(), make_app_batch()};
#else
    return {};
#endif
}

bool is_computer_use_tool(const std::string& name) {
    return name == "app_inspect_dump" || name == "app_inspect_find" ||
           name == "app_screenshot" || name == "app_click" ||
           name == "app_drag" || name == "app_open" ||
           name == "app_type" || name == "app_hotkey" ||
           name == "app_close" || name == "app_batch";
}

ExecuteResult execute(const std::string& name, const nlohmann::json& args, const std::string& agent_path_base) {
#if defined(_WIN32) && defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT && defined(FEATURE_AGENT_COMPUTER_USE) && FEATURE_AGENT_COMPUTER_USE
    if (name == "app_inspect_dump") return do_app_inspect_dump(args);
    if (name == "app_inspect_find") return do_app_inspect_find(args);
    if (name == "app_screenshot") return do_app_screenshot(args, agent_path_base);
    if (name == "app_click") return do_app_click(args);
    if (name == "app_drag") return do_app_drag(args);
    if (name == "app_open") return do_app_open(args);
    if (name == "app_type") return do_app_type(args);
    if (name == "app_hotkey") return do_app_hotkey(args);
    if (name == "app_close") return do_app_close(args);
    if (name == "app_batch") return do_app_batch(args);
    return fatal("unknown computer-use tool: " + name);
#else
    (void)args;
    (void)agent_path_base;
    return fatal(name + " unavailable in this build");
#endif
}

} // namespace media::llm::computer_use

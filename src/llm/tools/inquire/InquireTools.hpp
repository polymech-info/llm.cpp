// Copyright PolyMech / pm-image. Internal use only.
//
// `ask_user` agent tool surface — split into two namespaces:
//
//   pm::llm::inquire  — surface-agnostic Q/A backend (FTXUI for TTY,
//                       reply-file for headless; UI host stub for chat-next).
//   media::llm::path::detail — the `do_ask_user` dispatcher + `make_ask_user`
//                              JSON-Schema definition consumed by the path-mode
//                              tool catalog.
//
// Moved here from src/llm/tool_inquire.{hpp,cpp} so the inquire tool lives
// alongside the other path-mode categories (fs / pictures / audio / …).
//
// The header is stdlib-only. FTXUI screens live in tanit-ftxui-plugin when
// PM_PLUGIN_FTXUI is on; otherwise InquireTools.cpp links FTXUI statically.
#pragma once

#include "llm/tools/commons/PathToolCommons.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pm::llm::inquire {

/// Which interaction modality the agent is asking for. The tool advertises
/// these values to the LLM via the JSON schema enum.
enum class Kind {
    Text,        // free-form line(s) of text
    Choice,      // pick exactly one option (Menu)
    MultiChoice, // pick zero or more options (Checkbox grid)
    Confirm,     // yes / no
};

const char* to_string(Kind k) noexcept;
Kind        parse_kind(std::string_view s, Kind fallback = Kind::Text) noexcept;

/// All the state the LLM can pass through the schema. Validation happens in
/// the executor — this struct is "already valid" by construction.
struct Question {
    Kind                       kind = Kind::Text;
    std::string                question;     // required, non-empty
    std::vector<std::string>   options;      // required for Choice / MultiChoice
    std::string                placeholder;  // text mode hint
    bool                       multiline = false;
    int                        timeout_ms = 0; // <= 0 = no timeout

    // Polymorphic default — only the field matching `kind` is consulted.
    std::optional<std::string> default_text;
    std::optional<int>         default_index;     // Choice
    std::vector<int>           default_indices;   // MultiChoice
    std::optional<bool>        default_bool;      // Confirm
};

/// Why the inquirer returned. Round-tripped to the LLM so it can tell apart
/// "the user actually picked X" from "we timed out and fell back to default".
enum class StoppedBy {
    Submitted,
    DefaultedOnEmpty,
    Cancelled,
    Timeout,
    Scripted,
    Error,
};

const char* to_string(StoppedBy s) noexcept;

/// Result envelope. Exactly one of {text, index, indices, flag} is populated,
/// matching the requested `Kind`. `stopped_by` is always set.
struct Answer {
    bool                       ok = false;
    StoppedBy                  stopped_by = StoppedBy::Error;
    std::string                error;
    long long                  elapsed_ms = 0;

    std::optional<std::string> text;
    std::optional<int>         index;
    std::vector<int>           indices;
    std::optional<bool>        flag;
};

std::string stopped_by_label(StoppedBy s);

/// CLI entry point. Picks the TTY inquirer (FTXUI plugin, or statically linked
/// FTXUI when `PM_PLUGIN_FTXUI` is off) when `stdin` is a real TTY, otherwise
/// consults `PM_ASK_USER_REPLY_FILE`. If that env var is unset and we're not
/// on a TTY the call returns `StoppedBy::Error` with an explanatory message —
/// agents running headless without pre-seeded replies should not block.
Answer ask_cli(const Question& q);

/// UI host placeholder. Returns `ok=false / stopped_by=Error` with
/// `error="ui_not_implemented"`. Used by `ask_ui` when no UI backend has been
/// registered via `set_ui_backend()` — i.e. headless / pre-chat-window code
/// paths.
Answer ask_ui_stub(const Question& q);

// ── UI backend registrar ─────────────────────────────────────────────────
//
// The chat WebView host (ChatWebPanel_AskUser.cpp) registers a callable here
// at startup. When set, `do_ask_user` routes inline questions through this
// backend (which posts to JS via `window.pmChat.askUser`, then blocks on a
// promise the JS reply fulfils). When **not** set, the dispatcher falls back
// to `ask_cli` — the FTXUI / reply-file path used by tests and headless CLI.
//
// All three functions are safe to call from any thread; the registered
// backend itself MUST be safe to invoke from a non-UI worker (the agent loop).
using UiAskFn = std::function<Answer(const Question&)>;

void set_ui_backend(UiAskFn fn);
void clear_ui_backend();
bool ui_backend_active() noexcept;

/// Convenience for callers that want "UI if available, else CLI" without
/// caring about the surface. Used by `do_ask_user`.
Answer ask(const Question& q);

// ── HTTP `--consent-ui stream` ask surface ────────────────────────────────
//
// When `llm agent --serve` runs with `--consent-ui stream`, the turn binds an
// emit callback (SSE `tanit.ask`) and installs a UI backend that blocks until
// `POST /v1/chat/ask` resolves the reqId — same shape as stream consent.
using StreamAskEmit = std::function<bool(const std::string& type, const std::string& payload_json)>;

std::uint64_t bind_stream_ask_emit(StreamAskEmit emit);
void restore_stream_ask_emit(std::uint64_t ticket);
bool resolve_stream_ask(const std::string& reply_json);
void cancel_stream_ask();

/// Install (idempotent) the stream UI backend used by serve. Safe when no emit
/// is bound — falls through to `ask_cli` (headless error / reply-file).
void ensure_stream_ask_ui_backend();

} // namespace pm::llm::inquire

namespace media::llm::path {
struct ToolDef;
}

namespace media::llm::path::detail {

ExecuteResult do_ask_user(const nlohmann::json& args);
ToolDef       make_ask_user();

} // namespace media::llm::path::detail

#pragma once

// CLI HUD, console title, and consent-surface helpers shared by the agent
// handler and the voice loops.

#include "pm_image_cli_state.hpp"
#include "llm/agent.hpp"
#include "llm/agent_launch.hpp"
#include "string/string_utils.hpp"
#include "core/cli_cancel.hpp"
#include "win/overlay_hud.hpp"
#include "win/taskbar_status.hpp"
#include "logging/logger.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <memory>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
#include "security/consent_surface.hpp"
#include "security/ui/win/consent_ai_review.hpp"
#include "llm/security_review.hpp"
#include <functional>
#endif

namespace pm_llm_agent_cli {

struct AgentCliHud {
    std::unique_ptr<media::win::OverlayJob> overlay;
    std::unique_ptr<media::win::TaskbarJob> taskbar;
    int max_iter = 0;

    static AgentCliHud make(bool hud, const std::string& mode, int max_iter)
    {
        AgentCliHud h;
        h.max_iter = max_iter;
        std::string m = mode;
        m = media::strutil::lower_ascii(std::move(m));
        if (m.empty())
            m = hud ? "overlay" : "off";
        if (m == "overlay" || m == "both") {
            h.overlay = std::make_unique<media::win::OverlayJob>("Tanit AI");
            h.overlay->set_on_stop([] { media::cli::test_request_cancel(); });
            h.overlay->busy("working");
        }
        if (m == "taskbar" || m == "both") {
            h.taskbar = std::make_unique<media::win::TaskbarJob>("Agent working");
            h.taskbar->set_on_stop([] { media::cli::test_request_cancel(); });
            h.taskbar->busy("working");
        }
        return h;
    }

    bool active() const { return overlay || taskbar; }

    void busy(const std::string& d)
    {
        if (overlay) overlay->busy(d);
        if (taskbar) taskbar->busy(d);
    }
    void progress(uint64_t a, uint64_t b, const std::string& d)
    {
        if (overlay) overlay->progress(a, b, d);
        if (taskbar) taskbar->progress(a, b, d);
    }
    void error(const std::string& d)
    {
        if (overlay) overlay->error(d);
        if (taskbar) taskbar->error(d);
    }
    void completed(const std::string& d = {})
    {
        if (overlay) overlay->completed(d);
        if (taskbar) taskbar->completed(d);
    }
    void on_event(const media::llm::agent::Event& e)
    {
        if (!active())
            return;
        using K = media::llm::agent::Event::Kind;
        switch (e.kind) {
        case K::ToolCall:
            busy(e.tool_name.empty() ? "tool" : e.tool_name);
            break;
        case K::LlmRound: {
            const int round = e.payload.is_object() ? e.payload.value("round", 0) : 0;
            const std::string model = e.payload.is_object()
                ? e.payload.value("model", std::string{}) : std::string{};
            const std::string detail = model.empty() ? "thinking" : model;
            if (max_iter > 0 && round > 0)
                progress(static_cast<uint64_t>(round), static_cast<uint64_t>(max_iter), detail);
            else
                busy(detail);
            break;
        }
        case K::Error:
            error(e.text.empty() ? "failed" : e.text);
            break;
        default:
            break;
        }
    }
};

#if defined(PM_CLI_CONSENT_UI) && PM_CLI_CONSENT_UI
using ConsentSurfaceScope = pm::security::ConsentSurfaceScope;

inline std::string resolve_consent_ui_for_launch(
    const std::string& cli_consent_ui,
    const media::llm::agent::launch::AgentLaunchResolved& resolved)
{
    // Preset yolo/auto-deny still wins. Otherwise the native dialog: the CLI
    // stdio surface is opt-in via --consent-ui cli.
    return pm::security::resolve_consent_ui_mode(
        cli_consent_ui,
        resolved.has_preset ? resolved.selected_preset.consent_ui : std::string{},
        "win32");
}

// Build the SecurityAgent "Review with AI" callback for the win32 consent
// surface. Reuses the run's provider, overridden by --ai-consent-router /
// --ai-consent-model when set (mirrors agent_factory's CliAiReviewConsentDelegate).
// Shared by the normal and --realtime run paths so both surfaces behave 1:1.
inline ConsentSurfaceScope::ReviewFn make_consent_review_fn(
        const PmImageCliState& st,
        const media::llm::agent::ProviderSettings& base_provider) {
    media::llm::agent::ProviderSettings review_provider = base_provider;
    if (!st.ag_ai_consent_router.empty()) review_provider.router = st.ag_ai_consent_router;
    if (!st.ag_ai_consent_model.empty())  review_provider.model  = st.ag_ai_consent_model;
    return [review_provider](const pm::security::ConsentRequest& cr)
               -> pm::security::ui::AiConsentReview {
        try {
            auto res = media::llm::agent::security_review::run_review(cr, review_provider);
            return pm::security::ui::ai_review_from_json(res.review, res.ok, res.error);
        } catch (const std::exception& e) {
            return pm::security::ui::ai_review_from_json(nlohmann::json::object(), false, e.what());
        }
    };
}
#endif // PM_CLI_CONSENT_UI

#if defined(_WIN32)
class ScopedConsoleTitle {
public:
    explicit ScopedConsoleTitle(const std::wstring& title)
    {
        wchar_t buf[512]{};
        const DWORD n = ::GetConsoleTitleW(buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
        if (n > 0) {
            old_title_.assign(buf, buf + n);
            had_title_ = true;
        }
        ::SetConsoleTitleW(title.c_str());
    }

    ~ScopedConsoleTitle()
    {
        if (had_title_)
            ::SetConsoleTitleW(old_title_.c_str());
    }

    ScopedConsoleTitle(const ScopedConsoleTitle&) = delete;
    ScopedConsoleTitle& operator=(const ScopedConsoleTitle&) = delete;

private:
    std::wstring old_title_;
    bool had_title_ = false;
};
#endif
} // namespace pm_llm_agent_cli

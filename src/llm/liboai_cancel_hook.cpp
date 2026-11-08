// liboai_cancel_hook.cpp
//
// Strong symbol for `liboai_pm_cancel_check`, called by the
// CURLOPT_XFERINFOFUNCTION wired in packages/liboai/.../netimpl.cpp.
//
// liboai performs blocking `curl_easy_perform` calls for every LLM round.
// Without this hook a `cancel` request (UI stop button, CLI Ctrl+C) cannot
// abort an in-flight HTTP request, so the worker thread (and any thread
// joining it — e.g. the UI panel destructor) stays blocked for up to
// `timeout_ms` (180 s by default).
//
// With this hook, curl's progress callback fires roughly once per second and
// returns 1 when cancel is requested, which aborts curl with
// CURLE_ABORTED_BY_CALLBACK. The worker then exits within ~1 s.

#include "core/cli_cancel.hpp"
#include "logging/logger.h"

extern "C" int polymech_kbot_llm_cancel_check();

extern "C" int liboai_pm_cancel_check()
{
    // libcurl polls this ~1 Hz; trace only the false→true edge per thread to
    // avoid spam during the abort window. Re-arms when cancel is cleared so
    // subsequent cancels in the same worker thread still get logged.
    static thread_local bool last_seen = false;
    const bool req = media::cli::cancel_requested()
        || (polymech_kbot_llm_cancel_check() != 0);
    if (req && !last_seen)
        logger::trace("[cancel] liboai_pm_cancel_check → 1 (aborting curl)");
    last_seen = req;
    return req ? 1 : 0;
}

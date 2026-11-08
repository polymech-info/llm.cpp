#include "agent_tools.hpp"

#include <cctype>
#include <cstring>

namespace pm::llm {

// ── Canonical tool registry ───────────────────────────────────────────────────
//
// ORDER must match media::llm::path::tool_catalog() so that flag-based
// filtering (agent_tools_disabled_names) produces a deterministic catalog.
// When adding a new AgentTool enumerator:
//   1. Add a bit in constants.hpp  (AgentTool enum)
//   2. Add a row here              (agent_tools.cpp)
//   3. Add a make_*() in          path_tool_catalog.cpp
//   4. Append to k_agent_tools_all in constants.hpp
//
static constexpr AgentToolInfo k_registry[] = {
    // ── File / folder ─────────────────────────────────────────────────────
    { AgentTool::ListImages,        "list_images",
      "List image files in a folder or matching a glob pattern.",
      AgentToolGroup::File },

    { AgentTool::FileGlob,          "file_glob",
      "Discover files matching a glob / recursive pattern.",
      AgentToolGroup::File },

    { AgentTool::FileRead,          "file_read",
      "Read a text file. Large files need offset and limit. A converted document is search_index then search_query, not a read of every part.",
      AgentToolGroup::File },

    { AgentTool::FileSearch,        "file_search",
      "Grep or filename search (use indexer=rg + regex for headings).",
      AgentToolGroup::File },

    { AgentTool::SearchIndex,       "search_index",
      "Index a file or folder for later search_query. Returns a heading outline, not file bodies.",
      AgentToolGroup::File },

    { AgentTool::SearchQuery,       "search_query",
      "Search an indexed document. Returns a few hits with doc_id and a short snippet.",
      AgentToolGroup::File },

    { AgentTool::SearchDetail,      "search_detail",
      "Open one indexed passage by doc_id from search_query.",
      AgentToolGroup::File },

    // ── Image processing ──────────────────────────────────────────────────
    { AgentTool::ImageResize,       "image_resize",
      "Resize, convert format, or re-encode images on disk (libvips).",
      AgentToolGroup::Image },

    { AgentTool::ImageCrop,         "image_crop",
      "Crop images on disk to a pixel rectangle (libvips).",
      AgentToolGroup::Image },

    { AgentTool::ImageTransform,    "image_transform",
      "AI image edit on disk — enhance, restyle, retouch (Gemini / Replicate).",
      AgentToolGroup::Image },

    { AgentTool::ImageCreate,       "image_create",
      "AI text-to-image generation on disk (Gemini / Replicate).",
      AgentToolGroup::Image },

    { AgentTool::CreateVideo,       "create_video",
      "Generate or convert video on disk (Replicate).",
      AgentToolGroup::Image },

    { AgentTool::ImageUnderstand,   "image_understand",
      "Visual question-answering on image files (Gemini / local VLM).",
      AgentToolGroup::Image },

    { AgentTool::ImageFromCamera,   "image_from_camera",
      "Capture a still from the webcam, or from the phone camera (back unless facing is front).",
      AgentToolGroup::Image },

    { AgentTool::OcrText,           "ocr_text",
      "Extract text from images — uses local VLM (ocr slot) or cloud recognition provider.",
      AgentToolGroup::Image },

    // ── Utility ───────────────────────────────────────────────────────────
    { AgentTool::WriteFile,         "write_file",
      "Write or overwrite a UTF-8 text file on disk.",
      AgentToolGroup::Utility },

    { AgentTool::FileCompose,       "file_compose",
      "Assemble a file from existing files, line ranges, and small literal fragments.",
      AgentToolGroup::Utility },

    { AgentTool::DownloadUrl,       "download_url",
      "Download one http(s) URL into the current folder, or an explicit path.",
      AgentToolGroup::Utility },

    { AgentTool::FileStrReplace,    "file_str_replace",
      "Surgical unique-string replace in an existing UTF-8 file.",
      AgentToolGroup::Utility },

    { AgentTool::FileDelete,        "file_delete",
      "Delete one regular file on disk.",
      AgentToolGroup::Utility },

    { AgentTool::Speak,             "speak",
      "Synthesize text-to-speech: play on speaker, save audio to `dst`, write transcript to `text_out`.",
      AgentToolGroup::Utility },

    // Shares AgentTool::Speak: the tool bitmask is full.
    { AgentTool::Speak,             "play_file",
      "Play an audio file on the default speaker. Returns when playback finishes.",
      AgentToolGroup::Utility },

    // Shares AgentTool::Speak: the tool bitmask is full.
    { AgentTool::Speak,             "suspend_voice",
      "End the live voice session and return to standby.",
      AgentToolGroup::Utility },

    { AgentTool::AudioTranscribe,   "audio_transcribe",
      "Record from microphone and return the transcript (auto-VAD stop). Pair with `speak` to ask the user a question and listen for the spoken answer.",
      AgentToolGroup::Utility },

    { AgentTool::AskUser,           "ask_user",
      "Show an interactive prompt and wait for the user's reply: free-form text, single choice, multi-select, or yes/no. Use when a fact required to proceed is missing and not in memory.",
      AgentToolGroup::Utility },

#if defined(FEATURE_SCHEDULER_TOOL) && FEATURE_SCHEDULER_TOOL
    // ── Scheduler ─────────────────────────────────────────────────────────
    { AgentTool::ScheduleAt,        "schedule_at",
      "Schedule a task to run once at a specific date / time.",
      AgentToolGroup::Scheduler },

    { AgentTool::ScheduleIn,        "schedule_in",
      "Schedule a task to run once after a delay.",
      AgentToolGroup::Scheduler },

    { AgentTool::ScheduleEvery,     "schedule_every",
      "Schedule a task to run on a repeating interval.",
      AgentToolGroup::Scheduler },

    { AgentTool::ScheduleCancel,    "schedule_cancel",
      "Cancel a scheduled task by ID.",
      AgentToolGroup::Scheduler },

    { AgentTool::ScheduleList,      "schedule_list",
      "List all current scheduled tasks.",
      AgentToolGroup::Scheduler },
#endif

    // ── Memory ────────────────────────────────────────────────────────────
    { AgentTool::MemoryRead,        "memory_read",
      "Read persistent session memory (facts, notes, preferences).",
      AgentToolGroup::Memory },

    { AgentTool::MemoryWrite,       "memory_write",
      "Write or update persistent session memory.",
      AgentToolGroup::Memory },

    { AgentTool::MemoryAppendEvent, "memory_append_event",
      "Append a structured event record to the session log.",
      AgentToolGroup::Memory },

    { AgentTool::MemoryFind,        "memory_find",
      "Search session memory and materialize exact turn or tool evidence.",
      AgentToolGroup::Memory },

    // ── Shell execution ───────────────────────────────────────────────────
    { AgentTool::Run,               "run",
      "Execute a shell command and return stdout/stderr. Large streams are clipped; the full text is saved for file_read.",
      AgentToolGroup::Utility },

    { AgentTool::RunSequence,       "run_sequence",
      "Execute a deterministic sequence of shell commands and timed waits without extra LLM round-trips.",
      AgentToolGroup::Utility },

    { AgentTool::InfoLookup,        "info_lookup",
      "Look up exact PM-Image command, tool, skill, MCP, or capability details.",
      AgentToolGroup::Utility },

    // ── Network ───────────────────────────────────────────────────────────
#if defined(FEATURE_SSH) && FEATURE_SSH
    { AgentTool::Network,           "ssh_exec",
      "Run one command on a remote host over SSH (key auth).",
      AgentToolGroup::Network },
#endif
#if defined(FEATURE_MQTT) && FEATURE_MQTT
    { AgentTool::Network,           "mqtt",
      "Publish, subscribe, or round-trip one MQTT message.",
      AgentToolGroup::Network },
#endif
#if defined(FEATURE_MODBUS) && FEATURE_MODBUS
    { AgentTool::Network,           "modbus_read",
      "Read Modbus registers or bits from a device.",
      AgentToolGroup::Network },
    { AgentTool::Network,           "modbus_write",
      "Write Modbus holding registers or coils on a device.",
      AgentToolGroup::Network },
#endif

#if defined(FEATURE_INSTALLER_VFS) && FEATURE_INSTALLER_VFS
    // ── Tanit service API ─────────────────────────────────────────────────
    { AgentTool::ServicePageCreate,  "service_page_create",
      "Create a Tanit CMS page from markdown (inline or local .md file).",
      AgentToolGroup::Service },
    { AgentTool::ServicePageUpdate,  "service_page_update",
      "Update an existing Tanit CMS page from markdown.",
      AgentToolGroup::Service },
    { AgentTool::ServicePageList,    "service_page_list",
      "List Tanit CMS pages for the logged-in user (find id/slug before update).",
      AgentToolGroup::Service },
    { AgentTool::ServicePageGet,     "service_page_get",
      "Fetch a Tanit CMS page by id or owner/slug (markdown body when present).",
      AgentToolGroup::Service },
    { AgentTool::ServiceFilesList,   "service_files_list",
      "List files in Tanit VFS remote storage.",
      AgentToolGroup::Service },
    { AgentTool::ServiceFilesGet,    "service_files_get",
      "Read or download a file from Tanit VFS.",
      AgentToolGroup::Service },
    { AgentTool::ServiceFilesUpload, "service_files_upload",
      "Upload a local file, folder, or inline text to Tanit VFS.",
      AgentToolGroup::Service },
    // No free bits: delete shares the upload (VFS write) bit, link the get bit.
    { AgentTool::ServiceFilesUpload, "service_files_delete",
      "Delete files or folders from Tanit VFS.",
      AgentToolGroup::Service },
    { AgentTool::ServiceFilesGet,    "service_files_link",
      "Share link for a Tanit VFS file or folder.",
      AgentToolGroup::Service },
    { AgentTool::ServiceSearch,      "service_search",
      "Full-text search Tanit CMS content (pages, posts, pictures, files, places).",
      AgentToolGroup::Service },
#endif

    // ── Command authoring ─────────────────────────────────────────────────
#if defined(FEATURE_TOOLS_COMMAND_CREATION) && FEATURE_TOOLS_COMMAND_CREATION
    { AgentTool::CreateCommand,     "create_command",
      "Create a new custom command entry in commands.json (ribbon / context menu).",
      AgentToolGroup::Utility },
#endif

    // ── Computer use / app inspection ─────────────────────────────────────
#if FEATURE_TOOLS_APP_USE
    { AgentTool::AppInspectDump,    "app_inspect_dump",
      "Inspect a desktop app and return a compact element tree.",
      AgentToolGroup::Computer },

    { AgentTool::AppInspectFind,    "app_inspect_find",
      "Find targetable controls/elements in a desktop app.",
      AgentToolGroup::Computer },

    { AgentTool::AppScreenshot,     "app_screenshot",
      "Capture a desktop app window, element, or rectangle to an image file.",
      AgentToolGroup::Computer },

    { AgentTool::AppClick,          "app_click",
      "Click a desktop coordinate or found app element.",
      AgentToolGroup::Computer },

    { AgentTool::AppDrag,           "app_drag",
      "Press, move along a straight line, release (drawing, selection, drag-and-drop).",
      AgentToolGroup::Computer },

    { AgentTool::AppOpen,           "app_open",
      "Launch a desktop application and optionally place its window.",
      AgentToolGroup::Computer },

    { AgentTool::AppType,           "app_type",
      "Type text into the focused (or activated) desktop window.",
      AgentToolGroup::Computer },

    { AgentTool::AppHotkey,         "app_hotkey",
      "Send a hotkey combination (e.g. ctrl+s, alt+f4) to the focused window.",
      AgentToolGroup::Computer },

    { AgentTool::AppClose,          "app_close",
      "Close a desktop window or terminate its process (force).",
      AgentToolGroup::Computer },

    { AgentTool::AppBatch,          "app_batch",
      "Execute a sequence of UI actions in ONE tool call (saves round-trips).",
      AgentToolGroup::Computer },

#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
    { AgentTool::BrowserRead,       "browser_read",
      "Read a compact actionable index or focused DOM snapshot from the active WebView.",
      AgentToolGroup::Browser },
    { AgentTool::BrowserFind,       "browser_find",
      "Find active-WebView elements by selector, role, or accessible name.",
      AgentToolGroup::Browser },
    { AgentTool::BrowserScope,      "browser_scope",
      "Inspect or clear the session zone for in-app WebView reads.",
      AgentToolGroup::Browser },
    { AgentTool::BrowserClick,      "browser_click",
      "Click an active-WebView element by stable ref or selector.",
      AgentToolGroup::Browser },
    { AgentTool::BrowserType,       "browser_type",
      "Enter text into an active-WebView form control.",
      AgentToolGroup::Browser },
    { AgentTool::BrowserSelect,     "browser_select",
      "Select an option in an active-WebView select control.",
      AgentToolGroup::Browser },
    { AgentTool::BrowserBatch,      "browser_batch",
      "Execute a sequence of WebView actions in one tool call.",
      AgentToolGroup::Browser },
    { AgentTool::BrowserNavigate,   "browser_navigate",
      "Navigate the centre in-app browser (goto URL, back, reload).",
      AgentToolGroup::Browser },
#endif
#endif

#if defined(_WIN32) && defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT
    { AgentTool::AppCommand,        "app_command",
      "Run a Tanit app/UI command on the running instance.",
      AgentToolGroup::App },
#endif

};

static constexpr std::size_t k_registry_size =
    sizeof(k_registry) / sizeof(k_registry[0]);

// ── Public API ────────────────────────────────────────────────────────────────

std::size_t agent_tool_registry_size() noexcept { return k_registry_size; }

const AgentToolInfo* agent_tool_registry_data() noexcept { return k_registry; }

const AgentToolInfo* agent_tool_find(AgentTool flag) noexcept
{
    if (flag == AgentTool::None) return nullptr;
    for (const auto& e : k_registry)
        if (e.flag == flag) return &e;
    return nullptr;
}

const AgentToolInfo* agent_tool_find_by_name(std::string_view name) noexcept
{
    for (const auto& e : k_registry) {
        const char* n = e.name;
        std::size_t i = 0;
        for (; i < name.size() && n[i]; ++i) {
            if (std::tolower(static_cast<unsigned char>(name[i])) !=
                std::tolower(static_cast<unsigned char>(n[i])))
                break;
        }
        if (i == name.size() && n[i] == '\0')
            return &e;
    }
    return nullptr;
}

} // namespace pm::llm

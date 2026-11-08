# Adding a new operation — playbook for LLM coding agents

This document is a **mechanical checklist** for adding a new pipeline operation (think `resize`, `compress`, `transform`, `meta`) to **`tanit`**. Follow the layers from the bottom up — **lib → CLI → REST → IPC → UI → docs** — and use one of the existing operations as a template instead of inventing a new layout.

> Reading order if you are unfamiliar with the codebase:
> [docs/api-dev.md](api-dev.md) (high-level), [docs/api-rest.md](api-rest.md) (REST contract), [docs/api-uds.md](api-uds.md) (IPC envelope), [docs/win32xx-ui.md](win32xx-ui.md) (UI), [README.md](../README.md) (CLI flag reference).

---

## 1. Architecture summary

```
┌────────────────────────────────────────────────────────────────────┐
│  src/win/ui_next/   (Windows ribbon + dock UI, optional)           │
│    Settings panel buttons → m_homeTabLastMode → Run dispatch       │
│                                                                    │
├────────────────────────────────────────────────────────────────────┤
│  src/main.cpp       (CLI entry — CLI11 subcommands)                │
│  src/http/serve.cpp (HTTP REST endpoints — cpp-httplib)            │
│  src/ipc/ipc_serve.cpp (line-JSON over TCP / Unix socket)          │
│                                                                    │
├────────────────────────────────────────────────────────────────────┤
│  src/core/<op>.hpp / <op>.cpp   (pure C++ workers, no I/O glue)    │
│    - <Op>Options struct                                            │
│    - do_op()           : single-file worker                        │
│    - apply_<op>_options_from_json(j, opts)                         │
│                                                                    │
└────────────────────────────────────────────────────────────────────┘
                  ↓ statically linked into both
              pm-media (.lib / .so)        tanit (.exe)
```

**Hard rules:**

1. **Lib first.** Everything new starts in `src/core/<op>.{hpp,cpp}`. The CLI, REST, IPC, and UI all consume it via the same struct + function signature. **No I/O glue (HTTP, paths-on-disk, GUI) belongs in `src/core/`.**
2. **One JSON option mapper.** A free function `apply_<op>_options_from_json(j, opts)` converts a `nlohmann::json` blob into the strongly-typed options struct. REST, IPC, and the UI all reuse it — option names and aliases are guaranteed to match across surfaces.
3. **Pass-through contract (REST / IPC) — no user-visible paths; prefer true in-memory.** For **new** operations, REST and IPC must not read/write **user** paths from the request. Implement a **buffer variant** in the lib (`<op>_buffer(in_data, in_size, …)`) and return bytes inline; the file-based `<op>_file()` is for the **CLI** only.
   - **Multipart-only** handlers (`compress` / `transform` / `meta`, and any new `/v1/<op>`): reject `application/json` bodies with **HTTP 415**. Call `<op>_buffer()` on `upload.content` — **no** temp input/output files (see `/v1/compress` in `serve.cpp`).
   - **Legacy `POST /v1/resize`** is special: it still supports **JSON** with `input` / `output` **host paths** (batch-friendly), **and** a **multipart** branch whose implementation may use **private** scratch files under the system temp dir before deleting them — not ideal, and `media::resize_buffer()` exists if that endpoint is refactored. **Do not copy the temp-file pattern for new ops** — follow the compress/transform model instead.
   - **IPC** mirrors the buffer contract: inline / base64 bytes in the envelope, no path I/O for pass-through ops.
4. **Error returns, not exceptions.** Workers return `{ok=false, error="..."}` instead of throwing. The UI wraps each worker call in a try/catch as a belt-and-braces guard so a single bad item never aborts the worker thread.
5. **Worker thread cleanup.** Any worker thread that calls libvips must call `vips_thread_shutdown()` **once at thread exit** — never between iterations.
6. **Batch queue control.** Every file-batch worker must capture `std::shared_ptr<media::BatchControl> ctrl` from `CMainFrame::m_batchCtrl` and call `ctrl->check_pause()` between items. Post `UWM_BATCH_PAUSED` (with a heap `BatchState*`) before blocking, and `UWM_BATCH_CANCELLED` / `UWM_BATCH_STATE_UPDATE` at thread exit. See [docs/queues.md](queues.md) for the full pattern and heap-ownership rules.

---

## 2. Data flow (using `resize` as the canonical example)

```
              ┌──────────────────┐
              │ tanit resize  │
              │ photo.jpg out.jpg│
              └────────┬─────────┘
                       │ CLI11 → ResizeOptions
                       ▼
   ┌───────────────────────────────────────────────┐
   │ media::resize_file(in, out, opt, err) (core)  │
   │   ↓ libvips: vips_thumbnail / vips_jpegsave   │
   └───────────────────────────────────────────────┘

           HTTP REST                IPC (TCP/Unix)             UI worker
           ─────────                 ─────────────              ─────────
   POST /v1/resize JSON       {"op":"resize", ...}\n     OnResize() std::thread
   apply_resize_options_from_json    apply_resize_options_from_json    apply_resize_options_from_json
                       ▼
                   resize_file()
                       ▼
        ┌──────────────────────────┬─────────────────────────────┐
        │ CLI: writes to disk      │ REST/IPC: bytes in body /   │
        │                          │ JSON inline                 │
        │ UI worker: per-file      │                             │
        │ progress via PostMessage │                             │
        └──────────────────────────┴─────────────────────────────┘
```

---

## 3. The 12-step checklist

For a new operation `myop`:

### Step 1 — Lib header `src/core/myop.hpp`

```cpp
#pragma once
#include "polymech_export.h"
#include <nlohmann/json.hpp>
#include <string>

namespace media {

struct MyopOptions {
    std::string foo = "default";
    int         bar = 0;
    bool        baz = true;
    // ... only POD-ish state; no HWND, no httplib, no asio.
};

struct MyopResult {
    bool        ok    = false;
    std::string error;            // populated on failure
    std::string output_path;      // populated on disk-write success
    std::string text_payload;     // populated on inline JSON return
};

/// Single-file worker. Returns errors via MyopResult.error (never throws).
POLYMECH_API MyopResult myop_run(const std::string& input_path,
                                  const std::string& output_path,
                                  const MyopOptions& opts);

/// Merge JSON keys → opts (REST + IPC + UI). Unknown keys ignored.
POLYMECH_API void apply_myop_options_from_json(const nlohmann::json& j,
                                                MyopOptions& opts);

} // namespace media
```

**Patterns to copy:** `src/core/compress.hpp`, `src/core/meta.hpp`.

### Step 2 — Lib implementation `src/core/myop.cpp`

- Use `nlohmann::json` for option parsing only.
- For libvips workers, use the `ensure_vips()` / `vips_err()` helpers already present in `resize.cpp`.
- For HTTP workers, use `ensure_curl_global()` + libcurl as in `transform.cpp`.
- **Wrap the public entry in a try/catch** that converts any exception to a `{ok=false, error="myop exception: ..."}` (template: `meta_extract` in `meta.cpp`).

```cpp
MyopResult myop_run_impl(const std::string& in, const std::string& out, const MyopOptions& o);

MyopResult myop_run(const std::string& in, const std::string& out, const MyopOptions& o) {
    try { return myop_run_impl(in, out, o); }
    catch (const std::exception& e) { return {false, std::string("myop exception: ") + e.what(), {}, {}}; }
    catch (...)                     { return {false, "myop exception: unknown",                  {}, {}}; }
}
```

### Step 3 — Add to `pm-media` library (`CMakeLists.txt`)

Append `src/core/myop.cpp` to `_pm_media_sources` and `src/core/myop.hpp` to the install list:

```cmake
set(_pm_media_sources
    src/core/cache.cpp
    src/core/compress.cpp
    src/core/meta.cpp
    src/core/myop.cpp        # ← add
    ...
)
install(FILES
    src/core/polymech_export.h
    src/core/myop.hpp        # ← add
    ...
    DESTINATION include/polymech
)
```

### Step 4 — CLI subcommand `src/main.cpp`

Mirror the `compress` or `meta` blocks: declare flag-bound variables, register a CLI11 subcommand, then dispatch. **Always** register `--<long>` and a single-letter `-<short>` for the most-used flags. Dry-run support is recommended for any operation that mutates source files or makes a paid API call.

```cpp
std::string mo_input, mo_output, mo_foo;
int         mo_bar = 0;
bool        mo_baz_off = false;
bool        mo_dry = false;

auto *myop_cmd = app.add_subcommand("myop", "<one-line description>");
myop_cmd->add_option("input",  mo_input,  "Input path")->required(true);
myop_cmd->add_option("output", mo_output, "Output path (omit = auto)");
myop_cmd->add_option("--foo",  mo_foo,    "Foo (string)");
myop_cmd->add_option("--bar",  mo_bar,    "Bar (int)")->default_val(0);
myop_cmd->add_flag("--no-baz", mo_baz_off, "Disable baz");
myop_cmd->add_flag("--dry-run", mo_dry,   "Validate args; no API / no writes");
// ... in the dispatch block:
if (myop_cmd->parsed()) {
    media::MyopOptions opts;
    opts.foo = mo_foo;
    opts.bar = mo_bar;
    opts.baz = !mo_baz_off;
    auto r = media::myop_run(mo_input, mo_output, opts);
    if (!r.ok) { std::cerr << "myop: " << r.error << "\n"; return 1; }
    std::cout << r.output_path << "\n";
    return 0;
}
```

### Step 5 — REST endpoint `src/http/serve.cpp`

Add a `svr.Post("/v1/myop", [](const httplib::Request& req, httplib::Response& res) { ... })` block. Follow the **microservice contract — multipart-only, in-memory**:

1. **Reject non-multipart bodies** with `res.status = 415` and a JSON error explaining the contract (copy the wording from `/v1/compress`). Do **not** add a JSON path mode for new ops.
2. Pull the image bytes via `pick_multipart_image(req, upload)` and the option fields via `multipart_fields_to_json(req)` → `apply_<op>_options_from_json(jopt, opts)`.
3. Collect any extra parts (reference images, masks, …) **as in-memory buffers** from `req.files`. Never copy to a temp file.
4. Call the **buffer variant** of the worker — `media::<op>_buffer(upload.content.data(), upload.content.size(), …)`. The lib must already support this; if it doesn't, go back to Step 1 and add it.
5. Stream the result inline — `res.set_content(std::move(result.bytes), result.mime)` for binary, or `res.set_content(result.json_text, "application/json")` for JSON. Set `Content-Disposition: inline; filename="…"` for binaries.

Templates: `/v1/compress` (binary out), `/v1/meta` (JSON out). Do **not** follow the older **multipart `/v1/resize`** path that shells out via `unique_temp_path` + `resize_file` — use a buffer worker end-to-end like compress.

> **R-string tip:** any JSON literal that contains `)"` (e.g. `R"({"error":"bad (file/upload)"})"`) **closes the raw literal early** and breaks compilation. Use a custom delimiter: `R"json({"error":"bad (file/upload)"})json"`.

### Step 6 — IPC dispatch `src/ipc/ipc_serve.cpp`

1. Add `make_myop_response(json)` next to `make_resize_response`. Same options parser.
2. For binary results, base64-encode bytes into `{ok, mime, bytes, b64}` (helpers already present at the top of the file).
3. Add `if (op == "myop") return make_myop_response(j);` to `dispatch()`.

### Step 7 — UI ribbon command `src/win/ui_next/`

1. **`Resource.h`** — `#define IDC_CMD_MYOP 31N` (next free ID).
2. **`Ribbon.xml`** — add `cmdMyop` Command entry; add it to `cmdGroupActions` on the Home tab. **Bump the group's `SizeDefinition`** (e.g. `FiveButtons` → `SixButtons`) when you add a new action. Re-running `cmake --preset release` triggers `uicc.exe` to regenerate `RibbonUI.h/.rc`.
3. **`SettingsPanel.h`** — add `MODE_MYOP` to the `Mode` enum, `MyopSettings` struct (UI mirror of `media::MyopOptions`), `ReadMyopSettings()` declaration, control handles, and a `m_myopControls` vector.
4. **`SettingsPanel.cpp`** — create the controls in `CreateControls()` (push into `m_myopControls`); update `SetMode()` to show/hide that vector; implement `ReadMyopSettings()`.
   - **Use the centralised UI font.** Call `pmui::ui_font()` from [`helpers/ui_font.hpp`](../src/win/ui_next/helpers/ui_font.hpp) instead of `GetStockObject(DEFAULT_GUI_FONT)`. The user's "App Settings → Font size" preference is applied automatically when `pmui::apply_font_to_tree(panel)` runs from `CMainFrame::ApplyAppearance()`.
   - **Use themed colours in `WM_CTLCOLOR*`** — read `pmui::theme_palette()` (from [`helpers/theme.hpp`](../src/win/ui_next/helpers/theme.hpp)) so the panel paints correctly under both Light and Dark themes. Pattern: `palette.window_bg` for the panel background, `palette.window_fg` for label text, `palette.control_bg` / `palette.control_fg` for EDIT / LISTBOX text fields. Cached `HBRUSH`es should be re-created when the cached colour changes.
5. **`Mainfrm.h` / `Mainfrm.cpp`**:
   - Declare `void OnMyop();` in the private section.
   - In `Execute()`, route `IDC_CMD_MYOP` to set `m_homeTabLastMode = MODE_MYOP` and call `SwitchSettingsMode(MODE_MYOP)` (mode-only — actual run happens via the unified **Run** button).
   - In the `IDC_CMD_RUN` case, dispatch `MODE_MYOP → OnMyop()`.
   - In `SwitchSettingsMode`, set the dock caption / tab text for the new mode.
6. **`Mainfrm_commands.cpp`** — implement `OnMyop()`:
   - Build the `items` vector from Explorer selection (priority) **or** queue (use the same pattern as `OnResize` / `OnCompress` / `OnFind`).
   - Spin a `std::thread`, post `UWM_QUEUE_PROGRESS` with new state codes for "running / done / error" (next free triple in the existing 1-9 range).
   - **Batch control**: capture `m_batchCtrl` and the pre-built `m_currentSession.items` copy; call `ctrl->check_pause()` + check `ctrl->cancel.load()` between items; post `UWM_BATCH_PAUSED` / `UWM_BATCH_CANCELLED` / `UWM_BATCH_STATE_UPDATE` with a heap `BatchState*` at exit. See [docs/queues.md](queues.md) for the exact pattern.
   - Wrap the worker call in a try/catch so a bad item never aborts the thread.
   - Call `vips_thread_shutdown()` **once after the loop**.
   - Post `UWM_<MYOP>_DONE` (`#define UWM_MYOP_DONE (WM_USER + N)` in `Resource.h`).
   - **Verbose logging** — log every input path and every important option to the Log panel before the loop (`OnFind` is the reference). Users debug failures from the Log; brief "Started → Done" messages are not enough.
7. **`Mainfrm.cpp`** — add `OnQueueProgress` cases for the new state codes (`Doing…` / `✓ Done` / `✗ Err`) and a `case UWM_MYOP_DONE:` handler that resets `m_processing` and shows a status message.

#### Step 7b — Optional: dedicated dock panel for results

If your op produces a structured result set the user wants to keep around (matches, comparisons, anything more interesting than "queued/done"), add a **dockable results panel** in the same style as **Find Results**:

- **Panel files** — `XxxPanel.{h,cpp}` defining `CXxxView` (the `CListView` / `CWnd` content), `CXxxContainer : public CDockContainerBase`, `CDockXxx : public CDockPanelBase`. Pattern reference: [`FindPanel.h/.cpp`](../src/win/ui_next/FindPanel.h).
- **Theme awareness** — expose a `RefreshThemeColors()` on the view (calls `ListView_SetBkColor` / `SetTextBkColor` / `SetTextColor` from the active palette) and call it from `CMainFrame::ApplyAppearance()`.
- **Dock ID + persistence** — add `static constexpr int DOCK_ID_XXX = N` on `CMainFrame`, register in `NewDockerFromID`, add `pv_xxx` to `WindowLayout` in [`win/settings_store.hpp`](../src/win/settings_store.hpp) so visibility survives across launches.
- **View tab toggle** — `cmdViewXxx` ToggleButton in `Ribbon.xml`; route `IDC_CMD_VIEW_XXX` in `Execute()` via `TogglePanelView`. Bump `cmdGroupPanels`' `SizeDefinition`.
- **Worker → panel pumping** — post a heap-allocated `std::vector<XxxRow>*` via a custom `UWM_XXX_PROGRESS`; the frame's handler dequeues, populates the listview, and `delete`s the batch.

### Step 8 — Tests `orchestrator/test-media.mjs`

Add focused suites:

- **CLI suite** `suiteMyopCli(rep)` — runs `dist/tanit.exe myop ...` against `tests/assets/` fixtures. Always include a **`--dry-run`** test if the lib supports it.
- **API suite** — extend `suiteApiRest` with: a multipart success case (status code, content type, body length), and a `Content-Type: application/json` case asserting **HTTP 415** (proves the multipart-only contract is enforced). Extend `suiteApiIpc` with `op:"myop"` cases (assert `res.ok === true`, the right mime, and that `bytes ≈ b64.length * 3/4`).
- Add a **`--myop-only`** flag (mirror `--meta-only` / `--api-only`).

In `package.json`:

```json
"test:media:myop": "node orchestrator/test-media.mjs --myop-only"
```

…and append it to `test:all`.

### Step 9 — Documentation updates

| File | What to add |
|------|--------------|
| **[README.md](../README.md)** | New `### myop` subsection in the CLI examples block: short prose intro, **flag table** (mirror `compress` / `meta`), one or two `bash` invocations. Add `tanit myop --help` to the Help block. Add `npm run test:media:myop` to the Tests block. |
| **[docs/api-rest.md](api-rest.md)** | New `## POST /v1/myop` section: dual-intake table (JSON + multipart), recognized JSON keys (literal block), response shape (binary or JSON), `curl` examples for both intakes. |
| **[docs/api-uds.md](api-uds.md)** | New `## op: "myop"` section: example envelopes, response shape (`{ok, mime, bytes, b64}` for binary, `{ok, ...}` for JSON). |
| **[docs/api-dev.md](api-dev.md)** | The "Adding a new operation" checklist already lives there at a higher level — add a single line if your operation introduces a new core concept (e.g. multimodal inputs, dry-run mode). |
| **[docs/integration.md](integration.md)** | If the operation needs new env vars, list them in the systemd `EnvironmentFile` example, the NSSM `AppEnvironmentExtra`, and the wrapper-script env table. |
| **[docs/product.md](product.md)** | End-user description (no jargon). Add an entry in the "What you can do" section. |
| **[docs/win32xx-ui.md](win32xx-ui.md)** | One-line entry in the Settings-panel modes table. |

After editing the markdown, regenerate offline HTML for the installer:

```bash
npm run docs:html
```

### Step 10 — Installer (NSIS) — `installer.nsi`

Only needed if your operation ships **new files** (e.g. a `.ps1` for an Explorer context menu, a Start-Menu shortcut, a sample asset). Add to **both** the `Section "Install"` (`File "..."` / `CreateShortcut`) **and** the `Section "Uninstall"` (`Delete "$INSTDIR\..."` / `Delete "$SMPROGRAMS\..."`).

### Step 11 — Explorer integration `src/win/register_explorer.cpp` (optional)

If the operation is a useful right-click action on Windows:

1. Add a new option field to `RegisterExplorerOptions` for the helper script path (`packages/media/scripts/explorer-myop.ps1`).
2. Add a `build_myop_command_line(...)` helper.
3. Register a verb under each association (mirror the `cmpmoz` / `metagen` entries in `register_one_target`).
4. Update the `--dry` listing.
5. Add the `.ps1` file to the installer payload (Step 10).

### Step 12 — Build, run, verify

```powershell
# Stop the running UI so the linker can replace tanit.exe.
Get-Process tanit -ErrorAction SilentlyContinue | Stop-Process -Force

cmake --build --preset release --target pm-image

# Quick CLI smoke
.\dist\tanit.exe myop tests/assets/square-64.png /tmp/out.png

# Targeted suite
npm run test:media:myop

# Full regression
npm run test:all
```

---

## 4. Files cheat-sheet — touch every row

| Layer | File | Operation needed |
|-------|------|------------------|
| Lib (declaration)   | `src/core/<op>.hpp`             | Add struct + function declarations |
| Lib (implementation)| `src/core/<op>.cpp`             | Implement worker + JSON option mapper |
| Build wiring        | `CMakeLists.txt`                | `_pm_media_sources` += new `.cpp`; install header |
| CLI                 | `src/main.cpp`                  | New subcommand block + dispatch |
| REST                | `src/http/serve.cpp`            | New `svr.Post("/v1/<op>", ...)` |
| IPC                 | `src/ipc/ipc_serve.cpp`         | New `make_<op>_response()` + `dispatch()` arm |
| UI commands         | `src/win/ui_next/Resource.h`    | `IDC_CMD_<OP>` + `UWM_<OP>_DONE` |
| UI ribbon           | `src/win/ui_next/Ribbon.xml`    | `cmd<Op>` Command + Home Actions group |
| UI settings panel   | `src/win/ui_next/SettingsPanel.{h,cpp}` | `MODE_<OP>`, controls, `Read<Op>Settings` |
| UI frame            | `src/win/ui_next/Mainfrm.{h,cpp}`       | `On<Op>()` decl + `Execute()` + `OnQueueProgress` cases |
| UI worker           | `src/win/ui_next/Mainfrm_commands.cpp`  | `On<Op>()` worker thread |
| Tests (orchestrator)| `orchestrator/test-media.mjs`   | `suite<Op>Cli` + `suiteApiRest` + `suiteApiIpc` extensions, `--<op>-only` flag |
| Tests (npm)         | `package.json`                  | `test:media:<op>` script + `test:all` chain |
| Docs (CLI)          | `README.md`                     | Subsection in `## CLI examples` |
| Docs (REST)         | `docs/api-rest.md`              | `## POST /v1/<op>` |
| Docs (IPC)          | `docs/api-uds.md`               | `## op: "<op>"` |
| Docs (user)         | `docs/product.md`               | "What you can do" entry |
| Docs (operator)     | `docs/integration.md`           | Env vars + service definitions if changed |
| Docs (UI)           | `docs/win32xx-ui.md`            | Settings-panel modes table row |
| Installer           | `installer.nsi`                 | If new files / shortcuts ship |
| Explorer            | `src/win/register_explorer.cpp` | Optional right-click verb |
| Explorer            | `packages/media/scripts/explorer-<op>.ps1` | Optional helper script |
| Offline help        | `dist/docs-html/`               | Regenerate via `npm run docs:html` |

---

## 5. Common pitfalls (learned the hard way)

- **`vips_thread_shutdown()` between iterations** — corrupts thread-local libvips state and aborts the next call. Call it **once at thread exit**, not inside the loop.
- **Raw-string literals containing `)"`** — close the literal early. Use `R"json(...)json"` (or any custom delimiter) when the JSON contains parentheses.
- **`.get<std::string>()` on model JSON** — a model can return the same key as an array, number, or null on different calls. Always check `is_string()` (or use a `js_str(j, key)` helper that coerces) before extracting. Otherwise a single bad response throws and aborts the worker thread.
- **Win32 EDIT control limit** — defaults to 32 KB on legacy Win32. Lift with `EM_SETLIMITTEXT` for any text preview > 32 K.
- **NSIS shortcut paths** — `$SMPROGRAMS\<group>\` must be added to **both** Install and Uninstall sections. Old groups must be cleaned in Uninstall when you rename.
- **Building while UI runs** — `tanit.exe` is locked. Always `Get-Process tanit | Stop-Process -Force` before `cmake --build`.
- **AI options carry an API key** — only the CLI accepts `--api-key`. REST / IPC / UI rely on `IMAGE_TRANSFORM_GOOGLE_API_KEY` from the service environment. Document this on every new AI-using endpoint.
- **New REST multipart endpoints must not use temp files for the op** — no `unique_temp_path()` / `ofstream` for input or output; add `<op>_buffer()` in the lib first (legacy **`/v1/resize` multipart** is the exception — uses scratch files today). The contract test in `suiteApiRest` posts `application/json` and asserts **HTTP 415** — if that fails for your op, you forgot to reject non-multipart bodies.
- **`vips_thumbnail_buffer` takes `void*`, not `const void*`** — the libvips C API isn't const-correct. Cast with `const_cast<void*>(in_data)`; libvips does not write through it.
- **Recursive uxtheme themers must skip `Win32++` framework windows** — `EnumChildWindows` visits every descendant, including Win32++'s own custom CWnds (`Win32++ Bar` for the splitter, `Win32++ DockClient`, dock containers). Pushing `SetWindowTheme` / `AllowDarkModeForWindow` / `WM_THEMECHANGED` to those resets internal state and breaks splitter drag. The themer in [`helpers/theme.cpp`](../src/win/ui_next/helpers/theme.cpp) early-returns when `wcsncmp(cls, L"Win32++", 7) == 0`. Mimic that pattern in any future visitor that walks dock-panel descendants.
- **Don't theme `IExplorerBrowser`-hosting panels** — the Shell COM creates its own child windows (file list `SysListView32`, header `SysHeader32`, breadcrumb edits) and runs its own internal layout. Pushing extra theme messages there silently breaks splitter hit-testing on that one dock. `CMainFrame::ApplyAppearance` excludes `m_pDockFileTree` from the recursive themer for this reason; do the same for any new panel that hosts a Shell COM.
- **`BS_AUTOCHECKBOX` text colour ignores `WM_CTLCOLORBTN`** — uxtheme draws the label text via `DrawThemeText` which uses the visual-style's own colour, not the colour we set in `WM_CTLCOLORBTN`. Result: dark-on-dark labels in dark mode. Fix: handle `WM_NOTIFY` for `NM_CUSTOMDRAW` from BUTTON children at `CDDS_PREPAINT` stage, set the desired colour on `nmcd->hdc`, and **return `CDRF_NEWFONT`**. Reference: `CSettingsView::WndProc` in [`SettingsPanel.cpp`](../src/win/ui_next/SettingsPanel.cpp).
- **`WS_EX_CLIENTEDGE` 3D bevel looks wrong in dark mode** — strip it on `EDIT` / `COMBOBOX` / `LISTBOX` / `SysListView32` when dark, and restore in light. The themer does this with `SetWindowLongPtr` + `SetWindowPos(SWP_FRAMECHANGED)`.
- **Embedded `IWebBrowser2` (Markdown preview) doesn't follow the OS dark mode** — pass an explicit theme to `html::wrap_markdown_html(body, title, HtmlTheme::Dark)` from `pmui::theme_palette().dark`. The light/dark CSS variants live in [`packages/html/src/md.cpp`](../packages/html/src/md.cpp).
- **Don't call `CFrame::RecalcLayout()` after pure colour / font changes** — frame layout recalculation is for size/position changes (e.g. ribbon resize). Calling it from `ApplyAppearance` cascades into the dock layout and can leave splitters in an inconsistent state. `InvalidateRect(GetHwnd(), nullptr, TRUE)` is enough to force a repaint.
- **The Windows Ribbon Framework cannot go truly dark.** `UIRibbon.dll` is closed-source and exposes only an HSB tint hook (`UI_PKEY_GlobalBackgroundColor` / `UI_PKEY_GlobalHighlightColor` / `UI_PKEY_GlobalTextColor` on `cmdQAT`). Best-case dark mode is a desaturated grey-ish ribbon (`TintRibbonForTheme` in `Mainfrm.cpp`); the fully dark look would require replacing the ribbon entirely.

---

## 6. Reference implementations to copy from

| Pattern | Best template |
|---------|---------------|
| **Pure libvips** worker (no AI, no network) | `src/core/resize.cpp`, `src/core/compress.cpp` |
| **AI** worker (curl + Gemini, multimodal parts, references) | `src/core/transform.cpp` |
| **Hybrid** (libvips + AI, dry-run, structured output) | `src/core/meta.cpp` |
| **Aggregator** (enumerates files, reuses another core lib, optional cache) | `src/core/find.cpp` |
| **REST binary-out** endpoint | `/v1/compress` in `src/http/serve.cpp` |
| **REST JSON-out** endpoint | `/v1/meta` in `src/http/serve.cpp` |
| **IPC dispatcher** | `make_*_response` + `dispatch()` in `src/ipc/ipc_serve.cpp` |
| **UI Settings panel mode + controls** | `MODE_META` in `src/win/ui_next/SettingsPanel.cpp` |
| **UI worker thread** | `OnMeta()` in `src/win/ui_next/Mainfrm_commands.cpp` |
| **UI: dedicated results dock + listview + context menu + reveal-in-Explorer** | `FindPanel.h/.cpp` + `OnFind()` in `Mainfrm_commands.cpp` |
| **UI: themed `CDockContainer` (custom `DrawTabs` / `DrawTabBorders`)** | `CDockContainerBase` in `helpers/dock_helpers.h` |
| **UI: global App Settings dialog (theme + font)** | `AppSettingsDlg.cpp` + `CMainFrame::ApplyAppearance` |
| **UI: `NM_CUSTOMDRAW` + `CDRF_NEWFONT` for dark BUTTON text** | `CSettingsView::WndProc` `WM_NOTIFY` arm |
| **HTML doc with light + dark CSS variants** | `wrap_markdown_html(body, title, HtmlTheme)` in `packages/html/src/md.cpp` |
| **Orchestrator suite** | `suiteMetaCli` + `suiteApiRest` / `suiteApiIpc` in `orchestrator/test-media.mjs` |
| **Optional dry-run + reference inputs** | `meta` (dry-run), `transform` (reference_images), `find` (reference_images for visual similarity) |

When in doubt: **copy the closest sibling, then change names**.


## Testing 

1. Using screenshot CLI probe

`tanit test screenshot --output ./tests/screenshot-last.png` (optional: --wait-ms 2000 )

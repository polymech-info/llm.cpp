# Browser-use — centre WebView agent driver

Tanit’s in-app **browser-use** drives centre **browser tabs** (WebView2) via injected JS
(`domUse.js`), not the external Python [browser-use](https://github.com/browser-use/browser-use)
library. Path tools (`browser_read`, `browser_click`, …) RPC into `window.__tanitBrowserUse`
on the **current tab** (or `tab=<id>`).

Manual smoke: [`tests/browser-use/manual-checklist.md`](../../tests/browser-use/manual-checklist.md) ·
local harness: [`tests/browser-use/test.html`](../../tests/browser-use/test.html).

## Stack

```
AgentFactory (Plan phase)
  → BrowserUseFeature::on_prepare          status → Turn.system_extra
  → path tools → Tool_BrowserUse::execute
  → media::win::browser_use::rpc_active
  → ExecuteScript → __tanitBrowserUse.rpc({cmd,args,epoch})
  → LiveMirror + MutationObserver + pageModel
  ← {ok, epoch, revision, data|error}
  → inject guard → history compact → agent messages
```

| Layer | Owns |
|-------|------|
| **Injected JS** | Live `eN` refs, epoch/revision, index/actions/batch, zoneHints, page-model slice, interaction flashes (default on) |
| **WebView host** | Load `domUse.js`, host registry, active-host pick, RPC marshal |
| **C++ tools** | 8 `browser_*` path tools, batch/navigate helpers, validation |
| **Agent feature** | Context inject, tool allowlist retain, disable parallel dispatch, injection abort |
| **Security** | Consent (`builtin.browser_*` family), prompt-injection scan before history keep |

**Requires (all ON):** `FEATURE_BROWSER` · `FEATURE_ASSISTANT` · `FEATURE_TOOLS_APP_USE` · `FEATURE_AGENT_COMPUTER_USE` · usually `FEATURE_HOME_LLM_TOOLS` for agent registration.

**Page model trial:** `FEATURE_BROWSER_PAGE_MODEL` (base preset default **ON**) — C++ gates agent inject text + keeping `model` in compacted history. Injected TS currently always attaches `model{}` on status/read.

## Tools

| Tool | Role |
|------|------|
| `browser_read` | Scout / pin / scoped index (`compact`, `within_ref`, `scope=set\|full`) |
| `browser_find` | Role/name search inside scope |
| `browser_scope` | status / set / clear session pin |
| `browser_click` / `type` / `select` | Act on `eN` ref (epoch optional) |
| `browser_batch` | Multi-step in one round; host may auto-read after mutations |
| `browser_navigate` | goto / back / reload / **tabs** / **switch** / **new** / **close** (epoch resets on nav) |

One browser-use tool per LLM round (`browser_batch` counts as one). Parallel tool dispatch is forced off while browser tools are live.

All tools accept optional **`tab`** (dock id string, e.g. `"12"`). Omit it to use the **current** centre browser tab.

## Tabs

Each `openurl` / `target=_blank` / `window.open` centre page is its own viewer tab with its own WebView, address bar, and history. Chat / file-viewer WebViews are **not** browser tabs.

| Piece | Behavior |
|-------|----------|
| **Open tabs** | Plan-phase `system_extra` lists `tab=<id> url=… title=…` (`*` = current) |
| **Current tab** | Selected centre browser tab if it is a real page; else last non-`about:blank` tab |
| **`tab=`** | RPC / click / type / batch / navigate that tab (switches to it on navigate) |
| **`browser_navigate action=tabs`** | Return `{tabs:[{id,url,title,current,pinned}], count, tab}` |
| **`action=switch`** | Focus `tab=` |
| **`action=new`** | New tab (same as `target=_blank`) then goto `url` |
| **`action=goto`** | Reuse an open tab on the same URL / origin+path / origin (unpinned), else navigate current or open |
| **`action=close`** | Close `tab=` or the current unpinned tab (last centre tab resets to empty) |

`eN` refs and epoch are **per tab**. After `switch` / `new` / `goto`, call `browser_read` before click/type.

## Techniques

| Technique | What it does |
|-----------|--------------|
| **LiveMirror `eN`** | Stable element ids for the current epoch; detached nodes pruned |
| **epoch / revision** | Epoch = document lifetime (nav/reset); revision++ on relevant DOM mutations |
| **zoneHints** | Soft partitions for scout: dense interactive subtrees → pin candidates |
| **session scope** | Pin a zone root across reads until clear / epoch or URL mismatch |
| **compact read** | Lite `{ref,role,name}` index; pageText omitted when unchanged |
| **browser_batch** | click/type/select/read/wait in one call; auto-read after mutating steps |
| **history compact** | Older browser envelopes slimmed in agent message history |
| **prompt injection guard** | Scan tool envelopes for ssh/exfil/pipe-shell/override bait; abort turn + block follow-through `run` |
| **consent family** | One Always grant for `builtin.browser_*` covers the tool set per origin |
| **page model `model{}`** | Zones + events + dismissCandidates on status/read (first slice; see Todos) |
| **interaction flash** | Orange corner brackets on click/type/select (default on; `highlight:false` to disable) — browser-use style |
| **active host** | Current centre **browser tab**; `tab=` selects another. If no browser tab, largest visible registered WebView |

## Involved files

### Injected runtime

| Path | Role |
|------|------|
| [`apps/shared/inspect/domUse.ts`](../../apps/shared/inspect/domUse.ts) | Core RPC runtime (mirror, observer, cmds, zoneHints, scope) |
| [`apps/shared/inspect/interactionHighlight.ts`](../../apps/shared/inspect/interactionHighlight.ts) | On-page click/type/select flashes (closed shadow) |
| [`apps/shared/inspect/pageModel/`](../../apps/shared/inspect/pageModel/) | Trial page model (`createPageModel`, types) |
| [`apps/shared/inspect/domTraverse.ts`](../../apps/shared/inspect/domTraverse.ts) | Element serialize for ref reads |
| [`apps/tanit-chrome/src/domUse.ts`](../../apps/tanit-chrome/src/domUse.ts) | Bundle entry → `dist/shared/inspect/domUse.js` |
| `dist/shared/inspect/domUse.js` | What C++ injects (rebuild via `npm run build:tanit-chrome`) |

### C++ tools / agent

| Path | Role |
|------|------|
| [`src/llm/tools/browser_use/Tool_BrowserUse.cpp`](../../src/llm/tools/browser_use/Tool_BrowserUse.cpp) | Tool defs + execute + RPC bridge |
| [`src/llm/tools/browser_use/browser_use_compact.cpp`](../../src/llm/tools/browser_use/browser_use_compact.cpp) | History slim; keeps `model` when FEATURE on |
| [`src/llm/tools/browser_use/browser_use_inject.cpp`](../../src/llm/tools/browser_use/browser_use_inject.cpp) | Prompt-injection detect |
| [`src/llm/agent_feature_BrowserUse.cpp`](../../src/llm/agent_feature_BrowserUse.cpp) | Plan-phase context + page-model line + abort wiring |
| [`src/llm/agent_factory.cpp`](../../src/llm/agent_factory.cpp) | Feature register, history compact, injection abort |
| [`src/llm/path_tool_catalog.cpp`](../../src/llm/path_tool_catalog.cpp) / [`path_tool_executor.cpp`](../../src/llm/path_tool_executor.cpp) | Catalog + dispatch |
| [`src/xblox/blocks/browser_blocks.cpp`](../../src/xblox/blocks/browser_blocks.cpp) | Same execute backend for XBlox |

### WebView host

| Path | Role |
|------|------|
| [`src/win/web/webview_BrowserUse.cpp`](../../src/win/web/webview_BrowserUse.cpp) | Inject, host registry, tab hooks, `rpc` / `rpc_for_tab` |
| [`src/win/web/webview_dom_inspect.cpp`](../../src/win/web/webview_dom_inspect.cpp) | UI-thread `ExecuteScript` JSON transport |
| [`src/win/web/CWebView.cpp`](../../src/win/web/CWebView.cpp) | Host lifecycle; calls inject/register |
| [`src/win/web/BrowserWebPanel*.cpp`](../../src/win/web/) | Per-tab centre browser (toolbar + history) |
| [`src/win/web/browser_batch.cpp`](../../src/win/web/browser_batch.cpp) | Host batch runner + auto-read |
| [`src/win/web/browser_navigate.cpp`](../../src/win/web/browser_navigate.cpp) | Navigation + post-nav epoch |

### Security

| Path | Role |
|------|------|
| [`src/security/CliConsentDelegate.hpp`](../../src/security/CliConsentDelegate.hpp) | `builtin.browser_*` family grant |
| [`src/security/SecurityGateway.cpp`](../../src/security/SecurityGateway.cpp) | Authorize; suggested origin scope |

### Build / harness

| Path | Role |
|------|------|
| [`scripts/build-browser-use.sh`](../../scripts/build-browser-use.sh) | `build:tanit-chrome` + `build:cpp` + session reports |
| [`tests/browser-use/test.html`](../../tests/browser-use/test.html) | Local board / settings / form / page-model promo smoke |
| [`tests/browser-use/manual-checklist.md`](../../tests/browser-use/manual-checklist.md) | DevTools RPC + agent checklist |
| [`tests/xblox/browser-use-manual.xblox`](../../tests/xblox/browser-use-manual.xblox) | XBlox loop fixture |

> `scripts/test-browser-use.mjs` exercises the **external** Python browser-use repo — not this WebView stack.

## Rebuild & smoke

```bash
# inject + C++ (+ session reports)
sh ./scripts/build-browser-use.sh

# or separately
npm run build:tanit-chrome   # → dist/shared/inspect/domUse.js
npm run build:cpp
```

1. Centre WebView → open [`tests/browser-use/test.html`](../../tests/browser-use/test.html) (`file://` or navigate).
2. Console: `__tanitBrowserUse.rpc({ v:1, id:"t1", cmd:"read", args:{ compact:true, limit:80 } })` — expect `data.model.zones[]` with `doc`, `durable`, `semanticKey`; board/settings may include `collectionItems`.
3. Click **Show promo overlay** via `__tanitBrowserUse.rpc({… cmd:"click", args:{ref}})` → click `data.model` has dialog + `modal-opened` + `dismissCandidates` (no extra read).
4. Agent: `system_extra` lists `rankedZones` (BM25 vs user prompt). Or `browser_read` with `query: "settings"` → `model.rankedZones`; General settings `collectionItems` include Display name / Theme, not only Save.
5. Reload page / epoch reset → durable re-ground should emit `zone-rerendered` rather than only remove+add when locators match.

Token baselines: `node scripts/session-reports.mjs --format both --out stemp/browser-use`.

## Page model modules (`apps/shared/inspect/pageModel/`)

| File | Role |
|------|------|
| `zoneGraph.ts` | Live + semantic identity, dirty ownership, reconcile, events |
| `fingerprints.ts` | structure / text / actions / geometry + locationBucket |
| `overlayClassify.ts` | kind / purpose / blocking / dismiss / obscured |
| `collections.ts` | V2 lite: collection item keys |
| `durable.ts` | V3: durable locators + `reGround` after epoch |
| `index.ts` | Debounced mutate → statusSlice |
| `browser_use_page_model.cpp` | V4: BM25 `rankedZones` via [`agent_retrieval`](../../src/llm/agent_retrieval.hpp) |

## Todos

### Page model trial (`FEATURE_BROWSER_PAGE_MODEL`)

| Status | Item |
|--------|------|
| Done | CMake/presets default ON; stub; agent inject; promo harness |
| Done | **V1** zone graph + semantic match + fingerprints + dirty routing + overlay/blocking/obscured |
| Done | **V2** collection item keys + `collection-items-changed` |
| Done | **V3** durable locators + re-ground after epoch reset |
| Done | **V4** host BM25: `model.rankedZones` on prepare (user prompt) + `browser_read query=` |
| Done | Yield: compact `read` keeps lite `items[]`; history keeps `collectionItems[{liveRef}]`; strategy = pin+act without second scout |
| Done | Yield: click/type/select flush mutations + attach `model{}`; history slim keeps it; batch auto-read is compact |
| Done | Yield: settings/panels with inputs prefer form controls in `collectionItems` (not Save-button-only) |
| Next | FEATURE-off: strip `model` from JS observations (today TS always emits) |
| Later | Optional `browser_model_diff` if status payload gets fat |
| Later | Compact zone cap (8) → ranked/blocking first + lite stubs for the rest |
| **V5** | Dense zone ranking + event log — [`EmbedInference`](../../src/lib/llm/llama/EmbedInference.hpp) / VectorStore + RRF |

### Parked / out of scope

- Prompt planner/system seed rewrites for browser cheat-sheets
- RunPlan / playbook feature
- CDP, Playwright, rrweb replay, mutation journals as the primary path

## Strip page model

1. Set `FEATURE_BROWSER_PAGE_MODEL=OFF` (CMake / preset).
2. Delete [`apps/shared/inspect/pageModel/`](../../apps/shared/inspect/pageModel/) and the `domUse.ts` hooks.
3. Drop `#if FEATURE_BROWSER_PAGE_MODEL` blocks in `agent_feature_BrowserUse.cpp` / `browser_use_compact.cpp`.

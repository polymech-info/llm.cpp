# Path-mode edit tools

Status: **shipped** for `file_read` windowing, `file_str_replace`, and `file_delete`.
`file_outline`, tree-sitter, and ast-grep are **deferred** — locate structure
with `file_search` (`indexer=rg`, `regex=true`).

This is the file-edit surface for the in-app / `tanit llm agent` path tools.
The agent can change a large file without loading or rewriting the whole body.

### Harness

`tests/orchestrator/test-agent-edit.mjs`. Fixtures live in `tests/editor/`
(`cli.md`, `audio.hpp`, `audio.cpp`). The harness copies them into
`stemp/agent-edit-harness/work` and **wipes that work dir each run** — do
not leave fixtures only in `stemp/`.

Suites assert **locate + unique patch** (rg → windowed read → unique
`old_string`; ambiguous snippets fail). `edges` covers FS refuse
(missing path, directory, image-ext, binary, secret-like basename).
`mass` covers sequential version-chained edits; same-round batch on one
file is treated as unsafe, not as an API.

| Script | What |
|---|---|
| `npm run test:agent:edit` | Loopback: tools, headings, contract, edges, mass, cpp, history |
| `npm run test:agent:edit:history` | ChangeStore: version guard, objects/files/sessions under `config/history` |
| `npm run test:agent:edit:headings` | `##### Examples` in `cli.md` |
| `npm run test:agent:edit:contract` | `replace_all`, line window, CRLF, empty/identical `old_string`, 400-line cap |
| `npm run test:agent:edit:edges` | Missing/dir/binary/window/unicode/expansion; `*token*` basename deny |
| `npm run test:agent:edit:mass` | 24 sequential versioned flips; `replace_all`; same-round batch is unsafe |
| `npm run test:agent:edit:cpp` | `audio.{hpp,cpp}`: same cheap cases as `cli.md` — locate, ambiguous `void stop();` / `is_running()`, unique neighbor + line-window, unique `set_aec_stream_delay_ms` + `AudioRingBuffer::push` |
| `npm run test:agent:edit:live` | One live turn on `cli.md` |
| `npm run test:agent:edit:live:cpp` | One live turn on `audio.cpp` |

Live uses the installed default preset + config-dir (no `--preset` /
`--config-dir` unless the caller passed `--preset`).

Related:

- Catalog assembly: [`src/llm/path_tool_catalog.cpp`](../../src/llm/path_tool_catalog.cpp)
- FS bodies + schemas: [`src/llm/tools/fs/FsTools.cpp`](../../src/llm/tools/fs/FsTools.cpp)
- Dispatcher: [`src/llm/path_tool_executor.cpp`](../../src/llm/path_tool_executor.cpp)
- ChangeStore: [`src/llm/change_store.cpp`](../../src/llm/change_store.cpp)
- Flags: `AgentTool` in [`src/constants.hpp`](../../src/constants.hpp), rows in
  [`src/llm/agent_tools.cpp`](../../src/llm/agent_tools.cpp)
- Tree-sitter symbols: [`src/search/source_adapter_code.cpp`](../../src/search/source_adapter_code.cpp)
  (`FEATURE_CODE_TREE_SITTER`, C/C++ today; heuristic fallback for other code exts)

---

## 1. Design rules

1. **Keep `file_*` names.** Do not add a mega `text_editor` / `apply_patch` tool.
   Models already know Cursor/Claude `Read` + `Grep` + `StrReplace` + `Write`.
2. **No format-specific parse options.** No `parse=markdown`, heading extractors,
   ATX/setext modes, or front-matter flags on any tool. Structure comes from
   `file_search` (rg). Tree-sitter / `file_outline` are deferred.
3. **Markdown is not a special case.** Find headings with `file_search`
   (`query: "^### grant"`, `regex: true`, `indexer: rg`). Do not add a heading walker.
4. **Window, then patch.** `file_read` with `offset`/`limit` for a slice;
   `file_str_replace` for a unique snippet; `write_file` only for create or a
   true full rewrite.
5. **Same path + deny policy as today.** `resolve_tool_path_string`,
   `llm_path_fs_deny`, `llm_path_write_deny`. `file_str_replace` and `file_delete`
   are writes.

### Out of catalog (do not add)

| Idea | Why not |
|---|---|
| `parse` / `format` / `md` options on read or edit | Tree-sitter + `file_search` cover it |
| `apply_patch` / LLM unified diff | Models emit broken hunks; unique `old_string` is enough. Generated diffs are ChangeStore/UI only |
| `file_insert` (line + text) | Same job as `file_str_replace` with neighboring lines |
| `undo_edit` | Review/revert is ChangeStore + viewer Accept/Reject, not a file-edit parameter |
| `notebook_edit` | No first-class notebook pipeline |
| `file_move` / `file_copy` | Already documented on `write_file`: use `run` |
| Directory delete / recursive rm | `file_delete` is files only |

---

## 2. Catalog at a glance

| Tool | Status | Role |
|---|---|---|
| `file_glob` | exists | Discover paths |
| `file_search` | exists | Grep / name search (rg or built-in) |
| `file_read` | **shipped** | UTF-8 read, windowed by line (`offset`/`limit`) |
| `file_outline` | **deferred** | Tree-sitter / ast-grep later |
| `file_str_replace` | **shipped** | Surgical unique-string replace |
| `write_file` | exists | Create, overwrite, or append the full body |
| `file_delete` | **shipped** | Delete one regular file |

`list_images` stays an image enumerator. It is not an edit tool.

Intended loop on a large file (`tests/editor/cli.md`, `tests/editor/audio.cpp`, …):

```text
file_search (indexer=rg, regex=true)  →  line of the heading / symbol
file_read    →  that window only (offset + limit)
file_str_replace →  unique old_string from the window
```

Never: `file_read` whole file → `write_file` whole file.

---

## 3. Shared conventions

**Paths.** Absolute, or relative to the current Explorer folder (same as
`file_read` / `write_file` / `file_glob`).

**Lines.** 1-based, inclusive ranges. `file_search.offset` stays “skip N hits”
(0-based pagination) — do not confuse it with `file_read.offset`.

**Line breaks.** Tools match and write the file’s existing newlines (`\n` or
`\r\n`). Callers pass `\n` in JSON strings; the executor normalizes on the
way in so Windows files do not fail `old_string` matches.

**Envelope.** Same `build_envelope` / `PerFileResult` shape as other path tools.
Do not return the new full file body on writes — path, bytes, match counts only.

**Security.** `agent_factory.cpp` already maps `delete_file` / `remove_file` to
`Action::Delete`. New names:

| Tool | `pm::security::Action` |
|---|---|
| `file_read`, `file_search`, `file_glob` | Read |
| `file_str_replace`, `write_file` | Write |
| `file_delete` | Delete |

**Flags.** Shipped bits (do not reuse). After `BrowserScope` (`1ULL << 54`):

| Enumerator | Bit | Name | Group |
|---|---|---|---|
| `FileStrReplace` | `1ULL << 55` | `file_str_replace` | Utility (with `write_file`) |
| `FileDelete` | `1ULL << 56` | `file_delete` | Utility |
| `FileOutline` | deferred | `file_outline` | File (later) |

Present in `k_agent_tools_all`, `agent_tools.cpp` (catalog order),
`tool_catalog()`, and `path_tool_executor.cpp`.

Claude/Codex runners: vendor `Edit` / `str_replace` map onto `file_str_replace`.

---

## 4. `file_read` (windowed)

Read a UTF-8 text file as a **line window**. Path-mode does not hard-fail
on file size; it pages. Whole-file cap is 16 MiB (same as `write_file`).

Keep the existing denylist (binary / image extensions, hidden / sensitive
paths, binary-looking buffers). Windowed reads still scan only the requested
slice for the binary heuristic.

### Parameters

| Name | Type | Required | Default | Description |
|---|---|---|---|---|
| `path` | string | yes | — | File path. |
| `offset` | integer ≥ 1 | no | `1` | First **line** to return (1-based). |
| `limit` | integer ≥ 1 | no | omitted = remaining lines, capped | Max lines to return. Cap 400. |

No `parse`, `language`, `headings`, or `max_bytes` on the schema. Byte safety
is an implementation cap on the returned `text` (**per window**, not per file).
If the window exceeds the byte cap, return fewer lines and set `truncated: true`.

Omit `offset` and `limit` on a small file → full text, still subject to the
per-response line cap (400). If the file is larger than 400 lines or 128 KiB
and the caller did not pass a window, **fail** with a recoverable error
telling the agent to `file_search` then `file_read(offset, limit)`. Do not
silently return lines 1–400.

### Result (`PerFileResult.result`)

```json
{
  "path": "C:/repo/security/cli.md",
  "text": "### grant\n\n...",
  "line_start": 242,
  "line_end": 390,
  "total_lines": 2157,
  "bytes": 6120,
  "truncated": false,
  "version": "sha256:…"
}
```

| Field | Type | Description |
|---|---|---|
| `path` | string | Resolved absolute path. |
| `text` | string | Window body. May omit a trailing newline on the last line if the file does. |
| `line_start` | integer | First line included (1-based). |
| `line_end` | integer | Last line included (1-based, inclusive). |
| `total_lines` | integer | Line count of the whole file (always, so the model can page). |
| `bytes` | integer | UTF-8 byte length of `text`. |
| `truncated` | boolean | More lines exist after `line_end`, or the byte cap cut the window short. |
| `version` | string | `sha256:<hex>` of the **whole file**, not the window. Optional `expected_version` on mutations. |

`offset` past `total_lines` → `{ok:false, error:"file_read: offset 4000 past end (2157 lines)"}`.

### Schema

```json
{
  "type": "object",
  "properties": {
    "path": {
      "type": "string",
      "description": "File path: absolute, or relative to the Explorer folder / selection base in-app."
    },
    "offset": {
      "type": "integer",
      "minimum": 1,
      "description": "First line to return (1-based). Default 1. Not the same as file_search.offset (hit pagination)."
    },
    "limit": {
      "type": "integer",
      "minimum": 1,
      "maximum": 400,
      "description": "Max lines to return. Default: remaining lines, capped at 400."
    }
  },
  "required": ["path"]
}
```

Advertised description:

> Read a UTF-8 text file. For large files you MUST pass `offset` (1-based line)
> and `limit` (max 400 lines) — an unwindowed read of a large file fails.
> Returns `line_start` / `line_end` / `total_lines` / `truncated` so you can page.
> Use `file_search` to find the window first (`file_outline` is deferred).
> Binary and image extensions are refused (use `image_*`).

---

## 5. `file_search` (exists — no schema change)

Already the grep/name tool. Use it instead of a markdown parser.

Typical locate (heading or C++ symbol):

```json
{
  "query": "^##### Examples",
  "path": "cli.md",
  "regex": true,
  "indexer": "rg",
  "head_limit": 20
}
```

```json
{
  "query": "^void AudioInput::set_aec_stream_delay_ms",
  "path": "audio.cpp",
  "regex": true,
  "indexer": "rg",
  "head_limit": 8
}
```

Existing parameters (do not add more for “structure”):

| Name | Type | Required | Default | Description |
|---|---|---|---|---|
| `query` | string | yes* | — | Pattern. Alias: `pattern`. |
| `pattern` | string | — | — | Alias for `query`. |
| `inputs` | string[] | no | Explorer folder | Files or directories. |
| `path` | string | no | — | Alias for `inputs[0]`. |
| `mode` | `"grep"` \| `"name"` | no | `grep` | Content vs filename. |
| `indexer` | `"own"` \| `"rg"` | no | `own` | Built-in walker vs bundled ripgrep. |
| `glob` | string | no | — | `rg -g`. |
| `type` | string | no | — | `rg -t` (cpp, md, py, …). |
| `output_mode` | `"content"` \| `"files_with_matches"` | no | `content` | Hits vs paths. |
| `regex` | boolean | no | `false` | `true` = regex; default literal (`rg -F`). |
| `whole_word` | boolean | no | `false` | `rg -w`. |
| `-i` | boolean | no | `true` | Case-insensitive. |
| `multiline` | boolean | no | `false` | `rg -U`. |
| `-B` / `-A` / `-C` | integer ≥ 0 | no | 0 | Context lines. |
| `head_limit` | integer ≥ 0 | no | 250 | Cap hits; 0 = unlimited. |
| `offset` | integer ≥ 0 | no | 0 | Skip first N **hits**. |

---

## 6. `file_glob` (exists — no schema change)

Path discovery. Not an editor.

| Name | Type | Required | Default | Description |
|---|---|---|---|---|
| `pattern` | string | yes | — | Glob or directory. |
| `options.max_results` | integer 1…5000 | no | 500 | Cap. |
| `options.recursive` | boolean | no | `true` | Recurse when `pattern` is a directory. |
| `options.skip_dev_folders` | boolean | no | `true` | Skip `node_modules`, `.git`, … |

---

## 7. `file_outline` (deferred)

Not implemented. Use `file_search` with `indexer=rg` and `regex=true` until
tree-sitter / ast-grep land. Schema below is kept as the later contract.

---

Language-agnostic symbol map for **one file**. Backed by
`CodeSourceAdapter` / `tree_sitter_cpp_symbols` when
`FEATURE_CODE_TREE_SITTER` is on and the language is C/C++; otherwise the
existing heuristic (class/struct/enum/function + brace range).

Returns names and line ranges so the model can `file_read` a function or
section without grepping. **Not a parser API** — no CST, no query language,
no `format: markdown`.

Language is inferred from the extension (same map as
`code_language_from_ext`). Unknown / non-code extensions: return
`{ok:true, symbols:[], language:"text", note:"no grammar"}` — the agent
falls back to `file_search`. Do not invent markdown heading extraction here.

When a markdown (or other) grammar is vendored later, it plugs into the same
adapter and this tool’s schema does not change.

### Parameters

| Name | Type | Required | Default | Description |
|---|---|---|---|---|
| `path` | string | yes | — | File path. |
| `query` | string | no | — | Case-insensitive substring filter on `symbol` / `qualified_symbol`. |
| `kinds` | string[] | no | all | Restrict to these `symbol_kind` values. |
| `max_results` | integer 1…500 | no | 200 | Cap. |

Allowed `kinds` (advertise; extra values are ignored, not fatal):

`file`, `namespace`, `class`, `struct`, `enum`, `function`, `method`,
`heading` (only once a grammar emits it).

### Result

```json
{
  "path": "C:/repo/src/llm/tools/fs/FsTools.cpp",
  "language": "cpp",
  "backend": "tree-sitter",
  "total_lines": 893,
  "symbols": [
    {
      "symbol": "do_file_read_path",
      "kind": "function",
      "qualified": "media::llm::path::detail::do_file_read_path",
      "parent": "",
      "namespace": "media::llm::path::detail",
      "signature": "ExecuteResult do_file_read_path(const nlohmann::json& args)",
      "line_start": 591,
      "line_end": 618
    }
  ],
  "truncated": false
}
```

| Field | Type | Description |
|---|---|---|
| `language` | string | From extension (`cpp`, `c`, `javascript`, …) or `"text"`. |
| `backend` | `"tree-sitter"` \| `"heuristic"` \| `"none"` | What produced `symbols`. |
| `total_lines` | integer | File line count. |
| `symbols` | array | Ordered by `line_start`. |
| `symbols[].symbol` | string | Unqualified name. |
| `symbols[].kind` | string | See `kinds`. |
| `symbols[].qualified` | string | May be empty on heuristic backend. |
| `symbols[].parent` | string | Enclosing class / symbol; may be empty. |
| `symbols[].namespace` | string | May be empty. |
| `symbols[].signature` | string | May be empty on heuristic backend. |
| `symbols[].line_start` / `line_end` | integer | 1-based inclusive. |
| `truncated` | boolean | Hit `max_results`. |

Do not include source text or doc comments in the default payload (those
belong in a follow-up `file_read` of `line_start`…`line_end`). Keeps outlines
of large TUs cheap.

### Schema

```json
{
  "type": "object",
  "properties": {
    "path": {
      "type": "string",
      "description": "File to outline. Language from extension; C/C++ uses tree-sitter when built."
    },
    "query": {
      "type": "string",
      "description": "Optional case-insensitive substring on symbol or qualified name."
    },
    "kinds": {
      "type": "array",
      "items": { "type": "string" },
      "description": "Optional kind filter: namespace, class, struct, enum, function, method, heading."
    },
    "max_results": {
      "type": "integer",
      "minimum": 1,
      "maximum": 500,
      "description": "Max symbols returned. Default 200."
    }
  },
  "required": ["path"]
}
```

Description:

> Symbol map for one source file (name + 1-based line range). C/C++ uses
> tree-sitter; other code extensions use a brace heuristic. No grammar →
> empty `symbols` (then use `file_search`). Not a markdown parser. Follow
> with `file_read` `offset=line_start` `limit=line_end-line_start+1`.

---

## 8. `file_str_replace`

Replace one unique snippet. This is the default way to change an existing file.

`old_string` must match exactly once unless `replace_all` is true. On 0 or
(when not `replace_all`) N>1 matches, fail and report count + first few line
numbers so the model can widen context and retry. Do not pick the “first”
match silently.

Empty `old_string` is fatal. `old_string == new_string` is fatal.

Same 16 MiB file-size ceiling as `write_file`. Same write denylist.

### Parameters

| Name | Type | Required | Default | Description |
|---|---|---|---|---|
| `path` | string | yes | — | Existing file (must exist; this tool does not create). |
| `old_string` | string | yes | — | Exact snippet to find. Include enough neighbors to be unique. |
| `new_string` | string | yes | — | Replacement. Empty string = delete that snippet. |
| `replace_all` | boolean | no | `false` | Replace every match. Still fails if count is 0. |
| `start_line` | integer ≥ 1 | no | omitted | If set with `end_line`, search only that inclusive window. |
| `end_line` | integer ≥ 1 | no | omitted | Window end. Requires `start_line`. |
| `expected_version` | string | no | omitted | Optional `sha256:…` from `file_read`. Stale → fatal; re-read. |

`start_line` / `end_line` are a **search constraint**, not a line-rewrite API.
They exist so two identical `**Example**` blocks in `cli.md` can be
disambiguated after a windowed `file_read`. If only one of the pair is set →
fatal.

Create-new-file is `write_file`, not this tool.

### Result

```json
{
  "path": "C:/repo/security/cli.md",
  "replacements": 1,
  "lines": [76],
  "bytes_written": 88421
}
```

| Field | Type | Description |
|---|---|---|
| `replacements` | integer | How many substitutions landed. |
| `lines` | integer[] | 1-based start line of each match **before** the edit (cap 20). |
| `bytes_written` | integer | Size of the file after the write. |

Errors (stable prefixes for tests):

| Condition | Error |
|---|---|
| Empty `old_string` | `file_str_replace: old_string is empty` |
| Identical strings | `file_str_replace: old_string and new_string are identical` |
| Only one of `start_line` / `end_line` | `file_str_replace: start_line and end_line must be passed together` |
| Missing file | `file_str_replace: path does not exist` |
| 0 matches | `file_str_replace: old_string not found` |
| >1 match, `replace_all=false` | `file_str_replace: old_string matched N times (lines 12, 88, 210); widen old_string or set replace_all` |
| Binary / image ext | `file_str_replace: refusing binary or image extension` |
| Write deny | `file_str_replace: ` + deny reason |

### Schema

```json
{
  "type": "object",
  "properties": {
    "path": {
      "type": "string",
      "description": "Existing UTF-8 file to edit. Will not create a missing file (use write_file)."
    },
    "old_string": {
      "type": "string",
      "description": "Exact text to find. Must be unique unless replace_all is true. Include neighboring lines when the snippet repeats (e.g. **Example**)."
    },
    "new_string": {
      "type": "string",
      "description": "Replacement text. Empty string deletes the snippet."
    },
    "replace_all": {
      "type": "boolean",
      "default": false,
      "description": "Replace every match. Default false (require exactly one match)."
    },
    "start_line": {
      "type": "integer",
      "minimum": 1,
      "description": "Optional 1-based window start; must pair with end_line."
    },
    "end_line": {
      "type": "integer",
      "minimum": 1,
      "description": "Optional 1-based window end; must pair with start_line."
    }
  },
  "required": ["path", "old_string", "new_string"]
}
```

Description:

> Surgical edit of an existing UTF-8 file. `old_string` must match once
> unless `replace_all` is true; on ambiguity the tool fails with line
> numbers — widen the snippet or pass `start_line`/`end_line`. Do not use
> this to create files or rewrite a whole large file (`write_file`).

Atomic write: temp file in the same directory + rename, same as a careful
`write_file`. Do not leave a truncated target on failure.

---

## 9. `write_file` (exists — keep)

Create, overwrite, or append. **Full body only.**

Use when:

- the file does not exist yet;
- the file is small and the whole content is changing;
- the user asked to export / save a new note, report, CSV.

Do **not** use it to change one section of a large file.

### Parameters

| Name | Type | Required | Default | Description |
|---|---|---|---|---|
| `path` | string | yes | — | Output path; parents created. |
| `content` | string | yes | — | Full UTF-8 body. |
| `options.append` | boolean | no | `false` | Append instead of truncate. |

Result today: `{bytes_written, append}`. No change.

Cap stays 16 MiB. Description should add one sentence pointing at
`file_str_replace` for in-place edits.

---

## 10. `file_delete`

Delete one regular file. Refuse directories, refuse missing paths
(`ok:false`, not success). Same write/delete denylist as `write_file`
plus `Action::Delete`.

Security factory already lists `delete_file` / `remove_file` — implement
**one** public name: `file_delete`. Treat the other two as unrecognized
unless you add them as undocumented aliases.

### Parameters

| Name | Type | Required | Default | Description |
|---|---|---|---|---|
| `path` | string | yes | — | Regular file to delete. |

### Result

```json
{
  "path": "C:/repo/tmp/scratch.md",
  "deleted": true
}
```

### Schema

```json
{
  "type": "object",
  "properties": {
    "path": {
      "type": "string",
      "description": "Regular file to delete. Directories are refused; use run only if the user asked to remove a folder."
    }
  },
  "required": ["path"]
}
```

Description:

> Delete one regular file. Refuses directories, missing paths, and
> policy-denied locations. Not a recursive rm.

---

## 11. Wiring (shipped)

Touched with the tools (same list as `agent_tools.cpp`):

1. `AgentTool` bits + `k_agent_tools_all` in `src/constants.hpp`
2. Registry rows in `src/llm/agent_tools.cpp` (order = `tool_catalog()`)
3. `AgentToolGroup` comment in `src/llm/agent_tools.hpp`
4. `make_*` + `do_*` in `src/llm/tools/fs/FsTools.{hpp,cpp}`
5. `tool_catalog()` push in `src/llm/path_tool_catalog.cpp`
6. Name dispatch in `src/llm/path_tool_executor.cpp`
7. Slim arg logging in `PathToolCommons.cpp` (never log `old_string` / `new_string` / `content` bodies — sizes only)
8. `security_action_for_tool` (`file_str_replace` → Write; `file_delete` already Delete)
9. Artifact / memory hooks: treat `file_str_replace` like `write_file` (path + fingerprint, no full body); `file_delete` should drop / stale the session item
10. Claude/Cursor runner `Edit` → `file_str_replace`
11. Planner / `agent_context.cpp` prompt: windowed read + str_replace; do not tell the model to `write_file` a whole large file
12. `info_lookup` picks these up from the catalog — no extra table

`file_read` windowing is a behavior change of an existing tool (no new bit).

---

## 12. Prompt contract (short)

Add to the path-agent system prompt, not as tool options:

- Large file: `file_search` (`indexer=rg`, `regex=true` for `^### …`) → `file_read(offset, limit)` → `file_str_replace`.
- `old_string` must include neighbors when the file repeats boilerplate (`**Example**`, fence markers).
- `write_file` is create / full rewrite only.
- Do not call `run` with `sed` / `python -c` to edit a file when `file_str_replace` exists.
- Structure = `file_search`. No markdown mode; `file_outline` is deferred.

---

## 13. ChangeStore (backend + review UI shipped)

Path-mode mutations write history from **actual before/after bytes**. The
agent still edits with `file_str_replace` / `write_file` / `file_delete`.
The user reviews pending records in the **centre viewer toolbar** (not a
file-edit tool). Gated by `FEATURE_COMMAND_LLM` (on in the full app; off
on the viewer SKU). When the flag is off, host `vw_changes_*` and the
web review chrome are no-ops.

| Root | When |
|---|---|
| `%APPDATA%\<vendor>\<app>\history\` | Production (same root as sessions / logs) |
| `<config_dir>/history\` | `--config-dir` / `PM_CHANGE_HISTORY_DIR` |

```text
history/
  objects/sha256/<aa>/<digest>
  files/<uri-sha>.json
  sessions/<session>/<change-id>.json
  sessions/<session>/changeset.json
```

Each record: **`session_id`** (first-class; agent turn, editor, or VFS
session), `ts` / `ts_ms`, tool / tool_call, resource URI,
before/after SHA-256 + object refs, generated hunks, `state=pending`,
and an open `meta` object (reason, context, source, … — callers own it).
Objects are content-addressed (dedup). When a path has no pending records
left (last accept/reject, or accept/reject all), that path's resolved
records, empty file manifest, empty session leftovers, and unreferenced
objects are deleted. Pending records are never GC'd.

### Review UI

Monaco **and** markdown preview share the same review controls. Markdown
registers them on the **main viewer toolbar** (same slot as video zoom /
subtitles), not a sticky bar in the page.

| Control | Role |
|---|---|
| **Review** (`GitCompare`) | Toggle review. Amber **count bubble** (`pending_count`) shows before you enter review. Violet chrome while review is on. |
| Prev / next | Hunk (or next pending record when the file has several). |
| Accept / Reject | One change. Green / red **icon color only** (no filled button). |
| Accept all / Reject all | All pending records on this path. |

Markdown also highlights hunks in the preview (`data-pm-hunk`) and shows a
hover overlay (prev/next + accept/reject) at the hunk. Accepting a change
keeps scroll (`pm-md-scroll:<path>`). After Accept all / Reject all the
stale `pendingChange` hint is ignored so the bubble clears; a **new**
pending id brings it back.

LLM edits on the already-open file arrive as `vw_changes_updated` (editor
stays put; markdown may refresh the body). Host `setStatus.features.pendingChange`
is the one-shot hint (`id`, `session_id`, `plus`, `minus`, `tool`, `state`,
`pending_count`).

| Message | Direction | Role |
|---|---|---|
| `pendingChange` on `setStatus` | host → web | Hint; turns review on without polling. |
| `vw_changes_get` / `_result` | web ↔ host | `changes[]`, `before_text` / `after_text`. |
| `vw_changes_resolve` | web → host | `action` accept/reject, `scope` `one` \| `file`. |
| `vw_changes_updated` | host → web | Push after an in-place LLM edit. |
| `vw_reload` | web → host | Re-read the current centre file from disk (all viewers). |

Web: `apps/viewer-next/src/viewers/editor/changeReview/`.
Host: `CViewerWebPanel` + `CFileViewer::ReloadCurrentPreview` for `vw_reload`.

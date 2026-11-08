---
title: Office Document Indexing — Draft Plan
summary: Index .docx/.xlsx/.pptx into the local vector store via officecli text extraction.
tags: [llm, vector-store, office, docx, xlsx, pptx, officecli, draft]
---

# Office Document Indexing — Draft Plan

> **Status:** draft. No code written yet.

Index Word, Excel, and PowerPoint files into the local vector store using
`officecli view <file> text` for text extraction. Requires `officecli` installed.

---

## Text extraction strategy per format

### Word (.docx)

`officecli view <file> text` — whole document plain-text.  
Structured split: feed result to `MarkdownChunker` or `PlainTextChunker`.

For finer granularity per heading section:

```bash
officecli view report.docx outline --json   # section structure + heading text
officecli view report.docx text             # full plain text
```

Metadata available:
```bash
officecli view report.docx stats            # pages, words, title (doc properties)
officecli get  report.docx /body/p[1] --json # first paragraph → infer title
```

Each **document** = one `has_path` key. Chunk by `PlainTextChunker` (800 chars).  
Title: first `Heading1` paragraph or doc-property title from `stats`.

---

### Excel (.xlsx)

Per-sheet extraction — each sheet is a separate chunk group:

```bash
officecli view data.xlsx text               # all sheets concatenated
officecli get  data.xlsx /Sheet1 --json     # individual sheet → table data
```

Better approach: per-sheet via `view text` scoped to sheet:
```bash
officecli query data.xlsx 'sheet' --json    # list sheet names
officecli get   data.xlsx '/Sheet1' --depth 2 --json  # rows + cells
```

Path key: `<file>#<SheetName>` so each sheet deduplicates independently.  
Title: `<filename> — <SheetName>`.  
Type: `"table"` chunk type.

Pivot tables, named ranges, and charts are skipped (text-only mode).

---

### PowerPoint (.pptx)

Per-slide extraction:

```bash
officecli view slides.pptx text             # all slides concatenated
officecli view slides.pptx text --start 1 --end 1   # slide 1 only
officecli get  slides.pptx '/slide[1]' --depth 2 --json  # structured
```

Path key: `<file>#slide<N>` so each slide deduplicates independently.  
Title: title shape text from `/slide[N]/shape[@name=Title 1]` or `shape[1]`.  
Type: `"slide"` chunk type.

Speaker notes available via:
```bash
officecli get slides.pptx '/slide[1]/notes' --json
```

---

## Architecture

### Option A — Extend `vectorLocalAddFile` (recommended)

Add office format detection to the existing C++ block:

```
vectorLocalAddFile:
  .md / .txt / .text → existing MarkdownChunker / PlainTextChunker (no change)
  .docx / .xlsx / .pptx → new OfficeExtractor (see below)
```

New `OfficeExtractor` in `src/lib/llm/`:

```cpp
// office_extractor.hpp
struct OfficeChunk {
    std::string path;    // "file.docx" or "file.xlsx#Sheet1" or "file.pptx#slide2"
    std::string title;
    std::string type;    // "document" | "sheet" | "slide"
    std::string text;    // plain text from officecli
};

// Shells out to `officecli` and returns per-unit text chunks
std::vector<OfficeChunk> extract_office_text(const std::filesystem::path& file);
```

`extract_office_text` calls:
1. `officecli view <file> text` (whole-document fallback)
2. For xlsx: `officecli query <file> 'sheet' --json` → per-sheet `view text`
3. For pptx: `officecli view <file> stats --json` → slide count → per-slide `view text --start N --end N`

Then in `vectorLocalAddFile`, if extension is `.docx/.xlsx/.pptx`:

```cpp
auto office_chunks = extract_office_text(file_path);
for (auto& oc : office_chunks) {
    if (vs->has_path(oc.path)) continue;       // dedup per-sheet / per-slide
    PlainTextChunker chunker;
    auto chunks = chunker.chunk(oc.path, oc.text, copts);
    for (auto& ch : chunks) {
        ch.title = ch.title.empty() ? oc.title : ch.title;
        VStoreDoc doc;
        doc.source = "office";
        doc.path   = oc.path;
        doc.title  = ch.title;
        doc.chunk  = ch.content;
        vs->add(doc, ch.embedding_text);
    }
}
```

### Option B — Node.js orchestrator only (quickest to ship)

No C++ changes. A script shells out `officecli`, feeds extracted text to
`vectorLocalAddText` blocks.

```
scripts/index-office-docs.mjs
  args: <dir> <storePath> [--model X] [--ext docx,xlsx,pptx]

  for each file found:
    text = spawnSync('officecli', ['view', file, 'text']).stdout
    run xblox vectorLocalAddText { text, path: file, source: 'office', ... }
```

Pros: works today, no build needed, easy to iterate.  
Cons: no sheet/slide granularity, no dedup beyond path-level.

---

## New files (Option A)

| File | Purpose |
| --- | --- |
| `src/lib/llm/office_extractor.hpp` | `OfficeChunk` struct + `extract_office_text()` declaration |
| `src/lib/llm/office_extractor.cpp` | `officecli` subprocess call, JSON parsing via nlohmann |
| *(modify)* `src/xblox/blocks/vector_blocks_local.cpp` | Detect office extensions in `vectorLocalAddFile` |
| `tests/orchestrator/test-xblox-vector-store-office.mjs` | Orchestrator: create sample docs, add, search, assert |
| `scripts/index-office-docs.mjs` | Standalone indexer script |

---

## Deduplication

Office files use sub-document path keys so changed sheets/slides can be
re-indexed without clearing the whole store:

| Format | `has_path` key | Granularity |
| --- | --- | --- |
| `.docx` | `abs/path/to/file.docx` | whole document |
| `.xlsx` | `abs/path/to/file.xlsx#Sheet1` | per sheet |
| `.pptx` | `abs/path/to/file.pptx#slide3` | per slide |

---

## officecli subprocess contract

| Call | Returns |
| --- | --- |
| `officecli view <file> text` | Plain text, stdout, newline-delimited |
| `officecli view <file> stats --json` | `{pages, words, slides, sheets, title, …}` |
| `officecli query <file> 'sheet' --json` | `[{name, index, visible}]` |
| `officecli view <file> text --start N --end N` | Slide N plain text (pptx only) |
| `officecli get <file> '/slide[N]/shape[1]' --json` | Shape JSON with `.text` field |

The subprocess must be found on `PATH`. The C++ implementation uses
`std::filesystem` + platform `popen` / `CreateProcess`, similar to the
existing `llm_fs_guard.hpp` pattern.

---

## CMake

No new dependencies (uses `officecli` as an external tool, not a linked library).  
Add `office_extractor.cpp` to `_pm_media_sources` under a new feature flag:

```cmake
option(FEATURE_OFFICE_EXTRACTOR "Index .docx/.xlsx/.pptx via officecli" OFF)

if(FEATURE_OFFICE_EXTRACTOR AND FEATURE_VECTOR_LOCAL_XBLOX)
    list(APPEND _pm_media_sources src/lib/llm/office_extractor.cpp)
    target_compile_definitions(pm-media PRIVATE FEATURE_OFFICE_EXTRACTOR=1)
endif()
```

---

## Test plan

```bash
# Option B only (no build needed):
node scripts/index-office-docs.mjs ./test-docs out/office-store

# Full test suite (after Option A is built):
npm run test:xblox:vector-store-office

# Bench: add office docs to existing bench store, re-run queries
node tests/orchestrator/test-xblox-vector-store-local-bench.mjs \
  --skip-ingest --only semantic
```

Test cases:
- Empty `.docx` → no chunks, no error
- `.xlsx` with 3 sheets → 3 path keys, re-index skips unchanged sheets
- `.pptx` 10 slides → 10 path keys, slide titles appear in search results
- Query `"quarterly revenue"` → hits xlsx sheet chunk with that text
- Query `"product roadmap"` → hits pptx slide with matching title
- Re-add same file → skipped (all paths already have `has_path` = true)

---

## Open questions

- **`officecli` availability check**: block should emit a clear error when `officecli` is not on PATH, not a cryptic subprocess failure
- **Large xlsx**: a 50-sheet workbook with 10 k rows per sheet — text output could be huge; consider row-count cap or top-N rows per sheet
- **Password-protected files**: `officecli` returns non-zero exit; block should handle gracefully
- **Binary content**: embedded images/charts produce no text — silently skipped, which is correct
- **Incremental update**: when a file changes, current dedup only checks path existence; a content hash or mtime check would enable partial re-index without clearing the store

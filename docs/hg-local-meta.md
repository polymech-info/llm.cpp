# HG local metadata (`model.local.json`)

summary: New **local LLM branch** — sidecar metadata for HG-downloaded GGUF models. Does not replace existing `model.json` / `local_model_catalog` discovery; migrate later.

See also: [hg-models.md](hg-models.md), [hg-cpp.md](hg-cpp.md), [local-llms.md](local-llms.md).

---

## Layout (unchanged download tree)

```
${MODELS_DIR}/
  Qwen2.5-7B-Instruct/
    Q4_K_M/
      Qwen2.5-7B-Instruct-Q4_K_M.gguf
      tanit-manifest.json      # store/CDN ingest (SHA-256)
      model.local.json         # NEW — runtime sidecar (this doc)
```

Vision repos may also list `mmproj-*.gguf` in the sidecar `vision` block; weights download remains one-variant-per-invocation. Fetch mmproj separately (future: `hg download --with-mmproj`).

---

## `model.local.json` schema (`pixlwiz.hg.local/1`)

Written on `hg download` and rebuildable via `hg sidecar` / `hg download --remanifest-only`.

| Section | Purpose |
| --- | --- |
| `schema` | `pixlwiz.hg.local/1` |
| `quant` | Label, bits (from filename), decimal GB, shard flag |
| `files.weights` | Basenames of weight shards |
| `files.mmproj` | Recommended mmproj basename (repo-level, may not be local yet) |
| `vision` | `hasVision`, `mmprojFiles[]`, `recommendedMmproj` |
| `gguf` | Header KV read from local `.gguf` (architecture, context_length, chat template size) |
| `vram` | Rough weights + KV estimate for quant picker (8k ctx default) |
| `inference` | `runtime: llama.cpp`, `backend: llama-server`, `slots`, `task` |
| `llama_server` | Suggested spawn args + denylist (Studio-aligned) |
| `hf` | Pipeline tag + repo id from HF API |

### Example (text chat)

```json
{
  "schema": "pixlwiz.hg.local/1",
  "repo_id": "bartowski/Qwen2.5-7B-Instruct-GGUF",
  "variant": "Q4_K_M",
  "quant": { "bits": 4, "size_gb": 4.68, "sharded": false },
  "inference": {
    "runtime": "llama.cpp",
    "backend": "llama-server",
    "slots": ["text", "planner"],
    "task": "chat"
  },
  "gguf": { "architecture": "qwen2", "contextLength": 32768, "hasChatTemplate": true },
  "vram": { "totalGbEst": 5.4, "contextAssumed": 8192, "heuristic": false },
  "llama_server": {
    "suggestedArgs": { "-c": 0, "-ngl": -1, "--flash-attn": "on", "--jinja": true }
  }
}
```

---

## CLI / RPC enrichment

Remote probe (`hg variants`, `hg probe`, `hg meta`) now adds:

- `bits`, `sizeGb`, `sharded` per variant
- `vram.totalGbEst` (heuristic until local GGUF header exists)
- `repo.hasVision`, `repo.recommendedMmproj`
- `summary` object (HF-style quant table without scraping the website)

```powershell
tanit-cli.exe hg variants qwen2.5-7b-instruct
tanit-cli.exe hg probe "https://huggingface.co/Qwen/Qwen2.5-Coder-14B-Instruct-GGUF" -v q4_k_m
tanit-cli.exe hg download qwen2.5-7b-instruct -v Q4_K_M
tanit-cli.exe hg sidecar "%APPDATA%\PolyMech\tanit\models\Qwen2.5-7B-Instruct\Q4_K_M"
```

WebView2 RPC (`hgVariantsList`, `hgMetaGet`, `hgProbe`, `hgDownloadStart`) returns the same enriched fields.

---

## C++ modules (new branch)

| Module | Role |
| --- | --- |
| `hf_gguf_header.cpp` | Fast GGUF KV parser (architecture, ctx, template) |
| `hf_quant.cpp` | Q4_K_M → 4-bit, decimal GB |
| `hf_mmproj.cpp` | Vision / mmproj detection in repo file list |
| `hf_vram.cpp` | KV + weights VRAM projection |
| `hf_meta.cpp` | Enrich probe results, filter shard noise labels |
| `hf_sidecar.cpp` | Build/write `model.local.json`, llama-server hints |

**Not wired yet:** `local_model_catalog` still uses `model.json` + `.gguf` heuristics. Sidecar is consumed by the future local-LLM router only.

---

## llama-server target (planned, likely route)

Tanit today uses **in-process** `TextChatModel` (`llama_agent_client`). The sidecar's `llama_server` block specs the **alternate** path aligned with Unsloth Studio.

### Binary

- Ship `tanit-llama-server.exe` next to the app (`FEATURE_LLAMA_SERVER`; `packages/llama/build-cuda.ps1` → `prebuilt/bin/llama-server.exe` → `dist/win-x64/`). Override via env `LLAMA_SERVER_PATH`
- Vendored source: `packages/llama/tools/server/`

### Spawn contract (from sidecar `llama_server.suggestedArgs`)

```
llama-server
  -m <files.weights[0]>          # managed — not user-overridable
  --port <ephemeral>             # managed
  -c <0 = native ctx from gguf>  # from sidecar gguf.contextLength
  -ngl -1                        # or --fit on when vram.totalGbEst > free_vram
  --flash-attn on
  --no-context-shift
  --threads -1
  --jinja
  [--mmproj <files.mmproj>]      # when vision.required
```

### Process ownership (proposed)

| Piece | Owner |
| --- | --- |
| Download + sidecar | `hg` / settings UI (existing) |
| Server spawn/kill | New `LlamaServerBackend` in `src/llm/` |
| Chat API | HTTP client to `127.0.0.1:<port>/v1/chat/completions` |
| Router switch | `settings.json` → `chat.router = "llama-server"` (future) |

### Managed vs pass-through args

**Managed (denylist):** `-m`, `--mmproj`, `--port`, `--host`, `--parallel` (Studio pattern)

**User pass-through:** sampling, `-c`, `-ngl`, `--flash-attn`, cache types

### VRAM policy at load

1. Read sidecar `vram` + live `gpu_memory_stats`
2. If `total_bytes_est > 0.95 * free_vram` → add `--fit on`, cap `-c`
3. If vision and mmproj missing locally → fail with “download mmproj” (list in sidecar `vision.mmprojFiles`)

### Open decisions

- Single global server vs per-model instance
- Reuse Studio log tee under `%APPDATA%/…/logs/llama-server/`
- Whether C++ router replaces or complements in-process `llama` router during migration

---

## Migration path (later)

1. **Now:** `hg` writes `model.local.json`; old catalog unchanged
2. **Next:** Settings quant picker reads `summary` / sidecar VRAM hints
3. **Then:** New `discover_hg_local_models()` reads only `model.local.json`
4. **Finally:** Optional `llama-server` router; deprecate flat `.gguf` heuristics for chat

---

## Changelog

| Date | Change |
| --- | --- |
| 2026-07-13 | Initial sidecar schema, GGUF header probe, VRAM projection, llama-server spec |

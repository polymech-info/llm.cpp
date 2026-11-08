# Hugging Face model downloads (GGUF)

summary: Download GGUF chat models from Hugging Face with **upstream filenames** and **one quant per folder** — ready for llama.cpp, Tanit store upload, and (planned) native C++ download.

---

## Problem

Hugging Face’s default cache layout is content-addressed:

```
~/.cache/huggingface/hub/models--org--repo/blobs/sha256-…
  snapshots/<rev>/  → symlinks into blobs/
```

That works for Python tooling but is a poor fit when you need:

- **Clear paths** for `llama-server -m …` or `tanit` settings
- **Store/CDN upload** with stable object names (`Qwen2.5-7B-Instruct-Q4_K_M.gguf`)
- **One variant at a time** (not whole repos or safetensors shards)

Unsloth Studio also resolves GGUF from this cache when you pick a remote repo — see [Runtime: Unsloth Studio](#runtime-unsloth-studio).

---

## Current solution: `tanit` CLI (Python, Unsloth fork)

Lives under the **Unsloth** repo so upstream can still `git pull`. Tanit docs this here; implementation is in `../unsloth/tanit/`.

| Doc | Path |
| --- | --- |
| Operator guide | [`unsloth/tanit-cli.md`](../../unsloth/tanit-cli.md) |
| Catalog | [`unsloth/tanit/catalog/models.yaml`](../../unsloth/tanit/catalog/models.yaml) |

### Layout

```
unsloth/tanit/
  __init__.py
  __main__.py              # python -m tanit
  cli.py                   # doctor | list | variants | download
  catalog.py               # load models.yaml
  download.py              # hf_hub_download → local_dir, manifest
  catalog/models.yaml      # alias → HF GGUF repo
  requirements.txt         # huggingface_hub, pyyaml, typer
  scripts/setup.ps1        # local .venv
  scripts/setup.sh
  examples/
    download-qwen25-7b-instruct.ps1
  models/                  # gitignored download root
```

### Output convention

One **quantization variant** per directory; files keep **upstream names**:

```
tanit/models/<StoreName>/<Variant>/
  Qwen2.5-7B-Instruct-Q4_K_M.gguf
  tanit-manifest.json
```

Example on disk (first model shipped):

```
unsloth/tanit/models/Qwen2.5-7B-Instruct/Q4_K_M/
  Qwen2.5-7B-Instruct-Q4_K_M.gguf   (~4.36 GB)
  tanit-manifest.json
```

Vision models may need a separate `mmproj-*.gguf` in the same folder when downloaded manually; Studio currently pulls mmproj from HF cache when loading remote GGUF repos.

### Catalog (initial)

| Alias | HF repo | Default variant |
| --- | --- | --- |
| `qwen2.5-7b-instruct` | `bartowski/Qwen2.5-7B-Instruct-GGUF` | `Q4_K_M` |
| `qwen2.5-7b` | `bartowski/Qwen2.5-7B-GGUF` | `Q4_K_M` |

`bartowski/*` repos are used because they publish **single-file** quants with readable names. Official `Qwen/*-GGUF` repos often shard large variants (`*-00001-of-00004.gguf`).

---

## Commands

### Setup (once)

```powershell
cd C:\Users\zx\Desktop\pixlwiz\unsloth
.\tanit\scripts\setup.ps1
$env:HF_TOKEN = "hf_..."   # recommended; never commit
.\tanit\.venv\Scripts\python.exe -m tanit doctor
```

### List & download (one variant at a time)

```powershell
cd C:\Users\zx\Desktop\pixlwiz\unsloth

# catalog aliases
.\tanit\.venv\Scripts\python.exe -m tanit list

# quants available for a repo
.\tanit\.venv\Scripts\python.exe -m tanit variants qwen2.5-7b-instruct

# download exactly one quant
.\tanit\.venv\Scripts\python.exe -m tanit download qwen2.5-7b-instruct --variant Q4_K_M

# custom output root (e.g. mirror into pixlwiz dist)
.\tanit\.venv\Scripts\python.exe -m tanit download qwen2.5-7b-instruct `
  --variant Q4_K_M `
  -o C:\Users\zx\Desktop\pixlwiz\pixlwiz\dist\models
# → dist\models\Qwen2.5-7B-Instruct\Q4_K_M\Qwen2.5-7B-Instruct-Q4_K_M.gguf
```

Shortcut:

```powershell
.\unsloth\tanit\examples\download-qwen25-7b-instruct.ps1
```

Any HF GGUF repo id works without a catalog entry:

```powershell
python -m tanit variants bartowski/Qwen2.5-7B-Instruct-GGUF
python -m tanit download bartowski/Qwen2.5-7B-Instruct-GGUF --variant Q5_K_M
```

### Manifest (store upload)

`tanit-manifest.json` is written beside the `.gguf` with per-file SHA-256:

```json
{
  "store_name": "Qwen2.5-7B-Instruct",
  "repo_id": "bartowski/Qwen2.5-7B-Instruct-GGUF",
  "format": "gguf",
  "variant": "Q4_K_M",
  "files": [
    {
      "path": "Qwen2.5-7B-Instruct-Q4_K_M.gguf",
      "size_bytes": 4683074240,
      "sha256": "65b8fcd92af6b4fefa935c625d1ac27ea29dcb6ee14589c55a8f115ceaaa1423"
    }
  ]
}
```

Rebuild after manual edits:

```powershell
python -m tanit download qwen2.5-7b-instruct --variant Q4_K_M --remanifest-only
```

---

## Runtime: llama.cpp

### Direct

```powershell
C:\Users\zx\.unsloth\llama.cpp\build\bin\Release\llama-server.exe `
  -m "C:\Users\zx\Desktop\pixlwiz\unsloth\tanit\models\Qwen2.5-7B-Instruct\Q4_K_M\Qwen2.5-7B-Instruct-Q4_K_M.gguf" `
  --port 8080 -c 32768 --parallel 1 --flash-attn on --no-context-shift -ngl -1 --threads -1 --jinja
```

### Unsloth Studio (GGUF)

Load a **local** `.gguf` path in Chat, or use CLI:

```powershell
unsloth studio run --model "tanit/models/Qwen2.5-7B-Instruct/Q4_K_M/Qwen2.5-7B-Instruct-Q4_K_M.gguf"
```

When Studio loads a **remote** GGUF repo it spawns `llama-server` itself. Example command line (from studio logs, Gemma 4 vision):

```
C:\Users\zx\.unsloth\llama.cpp\build\bin\Release\llama-server.exe
  -m C:\Users\zx\.cache\huggingface\hub\models--unsloth--gemma-4-E2B-it-GGUF\snapshots\…\gemma-4-E2B-it-UD-Q4_K_XL.gguf
  --port 49419 -c 131072 --parallel 1 --flash-attn on --no-context-shift -ngl -1 --threads -1 --jinja
  --spec-default --chat-template-kwargs "{\"enable_thinking\": true}"
  --mmproj C:\Users\zx\.cache\huggingface\hub\…\mmproj-F16.gguf
```

Process tree when GGUF is active:

```
unsloth.exe studio …
  └─ python.exe (studio backend)
       └─ llama-server.exe -m …gguf …
```

Inspect on Windows:

```powershell
Get-CimInstance Win32_Process |
  Where-Object { $_.Name -match 'llama-server' } |
  Select-Object ProcessId, ParentProcessId, CommandLine | Format-List
```

Studio log tee: `%USERPROFILE%\.unsloth\studio\logs\llama-server\llama-*-port-*.log`

**Note:** Loading `unsloth/Qwen2.5-*-bnb-4bit` uses **safetensors in-process** (no `llama-server`). Only **GGUF** paths/repos use llama.cpp.

---

## Relation to Tanit / Tanit

| Concern | Today | Target |
| --- | --- | --- |
| Embedding GGUF (BGE) | `npm run download:bge-models` → `dist/models/` | [vector-store-local.md](vector-store-local.md) |
| Chat GGUF | Python `tanit` in `unsloth/` | C++ `tanit models download` (below) |
| Model root | `dist/models/` | Same layout: `{name}/{variant}/{file}.gguf` |
| Manifest | `tanit-manifest.json` | Same schema for CDN ingest |

Tanit launcher stub: [`scripts/unsloth.sh`](../scripts/unsloth.sh) (Studio on `:8888`).

Broader llama/CUDA loading plan: [local-llms.md](local-llms.md).

---

## C++ port (planned)

Goal: **`tanit models …`** in `tanit` / `tanit-cli` without Python, same directory layout and manifest.

### Proposed surface

```bash
tanit models list                              # read models.yaml (or embedded catalog)
tanit models variants qwen2.5-7b-instruct
tanit models download qwen2.5-7b-instruct --variant Q4_K_M [--output dist/models]
tanit models remanifest --path dist/models/Qwen2.5-7B-Instruct/Q4_K_M
```

### Suggested modules (pixlwiz)

| Piece | Location | Notes |
| --- | --- | --- |
| HF HTTP client | `src/net/hf_hub_client.cpp` | `GET /api/models/{repo}`, ranged `GET` file download, resume |
| Catalog | `src/llm/model_catalog.cpp` | Parse YAML (reuse `yaml-cpp` from vector store) or ship JSON |
| Variant picker | `src/llm/gguf_variant.cpp` | Port regex from `unsloth/tanit/download.py` (`_pick_gguf_files`) |
| Manifest | `src/llm/model_manifest.cpp` | SHA-256 per file; JSON write |
| CLI | `src/cli/pm_image_cmd_models.cpp` | Subcommands above |
| Progress | IPC / log | Reuse download progress patterns from other asset fetches |

src\llm\huggingface\hf_catalog.cpp

### Behaviour to preserve

1. **One variant per invocation** — never bulk-download all quants.
2. **`local_dir` semantics** — real files, not HF blob symlinks (`hf_hub_download(..., local_dir=…)` today).
3. **`HF_TOKEN`** — env var for rate limits / gated repos.
4. **Resume** — partial `.gguf` can continue (HF hub + `Range` or temp file rename).
5. **Vision** — optional second step for `mmproj-*.gguf` when catalog marks `vision: true`.

### Open decisions

- Ship catalog in-repo (`resources/models/catalog.yaml`) vs download from Tanit store.
- Whether C++ uses **libcurl** (already in tree?) or WinHTTP only on Windows first.
- Align npm script: `npm run download:chat-models` mirroring `download:bge-models`.

### Reference implementation

Python source of truth until port lands:

- [`unsloth/tanit/download.py`](../../unsloth/tanit/download.py) — `download_gguf_variant`, `list_gguf_variants`
- [`unsloth/tanit/cli.py`](../../unsloth/tanit/cli.py) — CLI wiring
- Unsloth Studio spawn args: [`unsloth/studio/backend/core/inference/llama_cpp.py`](../../unsloth/studio/backend/core/inference/llama_cpp.py) (`cmd = [binary, "-m", model_path, …]`)

---

## Adding a new model

1. Find a HF repo with **single-file** GGUF quants (or accept multi-shard download in `download.py`).
2. Add entry to `unsloth/tanit/catalog/models.yaml`.
3. `python -m tanit variants <alias>` — confirm quants.
4. Download one variant; verify `tanit-manifest.json` and llama load.
5. Document alias in this file’s catalog table.
6. When C++ lands: add the same entry to the embedded catalog.

---

## Changelog

| Date | Change |
| --- | --- |
| 2026-07-13 | Initial pixlwiz doc; Python `tanit` GGUF workflow, Studio llama-server notes, C++ port sketch |
| 2026-07-10 | First model: `Qwen2.5-7B-Instruct` / `Q4_K_M` via `bartowski` repo |

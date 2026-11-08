# Context Compression

Shrinks oversized text/JSON before it is handed to an LLM, so prompts cost
fewer tokens without losing the parts that carry information. Ported from the
Python `headroom` library into the C++ `pm-media` core.

There are **two pipelines**, chosen by content type:

| Content                         | Pipeline                          | Lossy? | Reversible structure |
| ------------------------------- | --------------------------------- | ------ | -------------------- |
| JSON                            | Structural mask + value truncation | partial | keys/braces kept     |
| Code                            | Structural / heuristic             | partial | yes                  |
| Prose (Text / Markdown / Log)   | Deterministic heuristic **or** Kompress model | yes | no |

Short inputs are passed through untouched (JSON below the size gate; prose
below `min_words = 10`).

---

## What each pipeline does

### 1. Deterministic structural pipeline (always on)
Pure C++, no model. Lives behind `FEATURE_LLM_COMPRESSION`.

- **JSON**: preserves the structure (keys, braces, arrays) and collapses
  whitespace; bulky string values are truncated. High-entropy substrings
  (UUIDs, hashes, IDs) are detected and preserved through truncation — the
  entropy signal is what keeps `a1b2c3d4-…` alive while dropping filler.
- **Prose**: whitespace/boilerplate collapse + entropy-aware truncation.

This path is deterministic, dependency-free, and is the **fallback whenever the
model is not registered or declines**.

### 2. Kompress text model (optional, opt-in)
A ModernBERT token-importance scorer (`chopratejas/kompress-v2-base`) that rates
each word and keeps the important ones. Compiled only when
`FEATURE_LLAMA && FEATURE_ONNX`. It mirrors `headroom`'s
`kompress_compressor.compress()` exactly:

```
words = whitespace split
if < 10 words: passthrough
for each chunk of 350 words:
  tokenize (is_split_into_words) -> input_ids / attention_mask / word_ids
  ONNX run -> final_scores (one float per subtoken)
  per-word score = MAX over that word's subtokens
  threshold mode (default): keep words with score > 0.5
  ratio mode:               keep top ceil(count * ratio) words per chunk
kept words (original order) joined by single spaces
```

It plugs into `universal_compressor` through a process-wide seam
(`set_text_model_reducer`). Call `register_kompress_text_reducer(cfg, err)` once
at startup; prose-like content then routes through the model, and everything
else (and any model failure) falls back to the deterministic path.

---

## Tokenizer dependencies — explained

This is the part that looks heavier than it is. The key split:

> **Heavy Python tooling is a one-time, build-machine step to *prepare artifacts*.
> The shipped runtime adds *zero* new libraries.**

**Why a tokenizer at all?** Kompress is a ModernBERT model. To get correct
`input_ids` you must tokenize *exactly* the way ModernBERT was trained — which is
**GPT-2 byte-level BPE** (a vocabulary + ~50 k merge rules + a byte mapping). Get
this wrong and the scores are garbage.

**Why we don't ship a new tokenizer library.** We already vendor **llama.cpp**,
whose BPE tokenizer natively understands the `"modern-bert"` pre-tokenizer (it
maps to GPT-2 byte-level). So instead of adding HuggingFace's Rust `tokenizers`
or hand-rolling a BPE, we reuse llama's. llama just needs the **vocabulary**, in
GGUF form — not the model weights.

So the chain is:

```
ModernBERT tokenizer.json  ──(one-time, Python)──►  modernbert-vocab.gguf  (1.8 MB, vocab+merges, NO weights)
                                                            │
                                              runtime: llama.cpp loads it with vocab_only=true
                                                            │
                          ModernBertTokenizer: per-word encode → frame [CLS]…[SEP] → input_ids + word_ids
```

| Phase            | Needs                                                            | When                    |
| ---------------- | --------------------------------------------------------------- | ----------------------- |
| **Artifact prep** | Python via `uv`: `huggingface_hub`, `transformers`, `torch`, vendored `convert_hf_to_gguf.py` | once, on a build machine |
| **Runtime**       | `llama.dll` + the 1.8 MB vocab GGUF + ONNX model + `onnxruntime.dll` (all already vendored/shipped) | every run |

No Python, no `torch`, no HuggingFace at runtime. The tokenizer "dependency" at
runtime is a **1.8 MB file** and a DLL we already ship.

Parity with the real HuggingFace tokenizer is **proven**, not assumed — see
*Tests* below. `word_ids` (the word↔subtoken map needed to aggregate scores) is
reconstructed by tokenizing each whitespace word standalone with no prefix space,
which reproduces HF's `is_split_into_words=True` output byte-for-byte.

---

## Footprint

Measured on AMD Ryzen 7 3700X (16 threads), CPU execution provider, `n_threads = 4`.

### Disk (`dist/models/kompress/`)
| File                      | Size    | Purpose                              |
| ------------------------- | ------- | ------------------------------------ |
| `kompress-int8-wo.onnx`   | ~274 MB | int8 weight-only ModernBERT scorer   |
| `modernbert-vocab.gguf`   | ~1.8 MB | vocab + merges (no weights)          |
| `kompress-fixture.json`   | tiny    | HF reference for the parity test     |
| `model-meta.json`         | tiny    | I/O names + special-token ids        |

### CPU vs GPU
- **CPU** (default, everywhere): provider `Auto` resolves to the CPU EP if no
  GPU is present. This is the common case and what the numbers below reflect.
- **GPU / accelerators**: the ONNX session supports `CUDA` (ships
  `onnxruntime_providers_cuda.dll`, plus TensorRT) and `CoreML` on Apple.
  `provider = Auto` tries CUDA first and **silently falls back to CPU**; set it
  explicitly (`cuda` / `coreml`) to require acceleration. The Kompress model is
  small; the inference itself is not the bottleneck on either path (see below).

### Memory / latency shape
- The vocab tokenizer is loaded `vocab_only=true` — **no weights, negligible RAM**.
- The 274 MB ONNX model is loaded by ORT into memory. **First load is the cost**
  (session init + reading the model); inference per ≤350-word chunk is small.
- **In-process** (registered reducer): the model loads **once** and is reused, so
  per-compression cost is just tokenize + one ORT run per chunk.
- **One-shot CLI**: each `test core kompress` invocation reloads the model, so
  CLI timings are *cold-load dominated*, not steady-state inference.

---

## Side effects & caveats

1. **Opt-in.** Nothing changes unless `register_kompress_text_reducer()` succeeds
   at startup. Otherwise the deterministic structural pipeline runs — default
   behaviour is unchanged.
2. **Lossy and not reversible.** The model *drops whole words*. Great for LLM
   context, wrong if you need verbatim text.
3. **Prose only.** Only `Text` / `Markdown` / `Log` route to the model. JSON and
   code always use the structural path.
4. **EP can shift borderline selections.** int8 inference is deterministic on a
   given execution provider (CPU output is byte-identical run-to-run — verified).
   But CPU vs CUDA float scores can differ slightly, which *could* flip a word
   sitting right at the `0.5` threshold. Pin the provider if you need identical
   selections across machines.
5. **Per-chunk selection.** Ratio mode keeps the top fraction *within each
   350-word chunk*, not globally — long documents are scored chunk-by-chunk.
6. **Passthrough gate.** Inputs under 10 words are returned untouched.

---

## Fetching the artifacts (one time)

```bash
uv run --with "huggingface_hub,transformers,torch,numpy,sentencepiece,protobuf" \
    scripts/fetch-kompress.py
```

Downloads the ONNX model, fetches the ModernBERT tokenizer, converts a
vocab-only GGUF via the vendored `convert_hf_to_gguf.py --vocab-only`, and emits
the parity fixture + I/O meta into `dist/models/kompress/`. Steps degrade
gracefully (e.g. `--skip-gguf`, `--skip-fixture`).

---

## Trying it (CLI)

```bash
# Tokenizer parity probe — dump input_ids / word_ids for a string
tanit-cli test core tokenize --vocab dist/models/kompress/modernbert-vocab.gguf  --input" ModernBERT byte-level BPE handles subwords."

# Score + compress prose (ratio mode keeps the top half per chunk)
tanit-cli test core kompress \
  --onnx  dist/models/kompress/kompress-int8-wo.onnx \
  --vocab dist/models/kompress/modernbert-vocab.gguf \
  --input "Compression keeps high-entropy ids like 7f3a9c2b intact. ..." \
  --ratio 0.5 --scores

# Route through universal_compressor's registered text seam
tanit-cli test core kompress --onnx ... --vocab ... --input "..." --via-compressor
```

Reference implementation we match: `../../../headroom/headroom/transforms/kompress_compressor.py`.

---

## Tests, accuracy & speed

Everything is exercised by the `kompress` suite in
`tests/orchestrator/test-core.mjs` (artifact- and feature-gated — it records a
skip instead of failing when the model or `FEATURE_LLAMA`/`FEATURE_ONNX` is
absent):

```bash
npm run test:core -- --only kompress     # just this suite
npm run test:core                        # all core suites (kompress included)
npm run test:onnx                        # OnnxSession (incl. the int64 path)
```

**Accuracy**
- **Tokenizer parity: 5/5 exact.** Every `input_id` *and* `word_id` matches the
  HuggingFace reference fixture, across adversarial cases (hyphenation,
  `7f3a9c2b`, `10.0.0.42:8080`, glued punctuation, single char).
- **Scoring sanity**: stop-words score low (`"the"`≈0.32, `"a"`≈0.44) and drop;
  rare/high-information tokens (e.g. `7f3a9c2b`≈0.99) survive.
- **Determinism**: identical input → byte-identical output (asserted).

**Speed** (one-shot CLI = cold model load each call; in-process reuse is far cheaper)

| Operation                          | Cold per call | Dominated by                         |
| ---------------------------------- | ------------- | ------------------------------------ |
| `tokenize` (parity probe)          | ~0.26 s       | process start + 1.8 MB vocab load    |
| `kompress` score / compress / seam | ~2.0 s        | process start + 274 MB model load    |

The model-load cost is paid **once** when the reducer is registered in-process;
steady-state compression is tokenize + one ORT run per ≤350-word chunk.

### Reports — yes, this is covered

`test:core` writes a Markdown report to **`tests/test-report-last.md`** (and, in
accumulate mode, appends a chapter to `tests/test-report-all.md`). The
**`Core kompress`** suite shows up there with:

- per-step **timings** (the speed numbers above come straight from it), and
- pass/fail **assertion** counts (accuracy),
- plus host context (CPU model, RAM, OS) and process RSS.

So a single `npm run test:core -- --only kompress` produces both the green/red
result *and* a timing/accuracy report you can diff between runs.


### Examples

$cli = ".\dist\win-x64\tanit-cli.exe"

# JSON in → metrics + compressed JSON out
& $cli test core compress --type json --input-file in.json |
  ConvertFrom-Json | Tee-Object -Variable r | Out-Null
"ratio=$($r.compressionRatio) handler=$($r.handler)"
$r.output | Set-Content out.json

# Markdown in → out (structural/deterministic path)
& $cli test core compress --type markdown --input-file in.md |
  ConvertFrom-Json | % output | Set-Content out.md

# Markdown/prose via the Kompress model (needs artifacts fetched)
& $cli test core kompress --onnx dist/models/kompress/kompress-int8-wo.onnx `
  --vocab dist/models/kompress/modernbert-vocab.gguf --input-file in.md
  
#!/usr/bin/env node
import { llmExe, prepareCliArgs } from './exe.mjs';
import { REPO as PACKAGE_ROOT } from './repo.mjs';
/**
 * Local llama.cpp agent smoke/precision harness.
 *
 * First-slice intent:
 *   - router/model only (`--router llama --model <id-or-path>`)
 *   - no tools, no MCP, no skills, no planner
 *   - measure wall time and exactness on small deterministic prompts
 *
 * Usage:
 *   npm run test:agent:local
 *   npm run test:agent:local -- --model gemma3-12b-it-q4-k-m
 *   npm run test:agent:local -- --require-model
 *   npm run test:agent:local -- --ngl 0          # CPU-only
 *   npm run test:agent:local -- --ngl -1         # all GPU layers (default)
 *   npm run test:agent:local -- --no-warmup      # skip model-load warmup pass
 *
 * Recommended: Gemma 4 12B IT (Q4_K_M), GPU-all — native tool calling.
 *   Unlike Gemma 3 (whose chat template carries no tool definitions, so the
 *   tool catalog never reaches the model), Gemma 4 has a tool-capable template
 *   and llama.cpp parses its <|tool_call>call: syntax natively. The tool-file-read
 *   case fires a real file_read tool call, executes it, and synthesises the result.
 *
 *     # fetch the model first (writes dist/models/gemma4-12b-it-q4-k-m.model.json)
 *     powershell -ExecutionPolicy Bypass -File scripts/fetch-gemma-local-windows.ps1 -Preset gemma4-12b
 *
 *     # run the suite on GPU (all layers) — the full 34-tool catalog is ~16K
 *     # prompt tokens, so prompt-eval per round is slow; raise the timeout.
 *     npm run test:agent:local -- --model gemma4-12b-it-q4-k-m --ngl -1 --require-model --timeout-ms 600000
 *
 *     # --- fast+agentic baseline: gemma-4 E4B (~4.7 GB, native tools + vision) ---
 *     # fetch (writes dist/models/gemma4-e4b-it-q4-k-m.model.json):
 *     powershell -ExecutionPolicy Bypass -File scripts/fetch-gemma-local-windows.ps1 -Preset gemma4-e4b
 *     # E4B fits fully on an 8 GB GPU; tool cases use --preset local --planner so the
 *     # planner pre-selects a tiny tool subset. file_read round-trips in ~9 s cold.
 *     npm run test:agent:local -- --model gemma4-e4b-it-q4-k-m --ngl -1 --require-model --timeout-ms 240000


$env:PM_AGENT_LOCAL_NGL="-1"; $disable="image_transform,image_create,create_video,image_understand,image_from_camera,speak,audio_transcribe,ask_user,schedule_at,schedule_in,schedule_every,schedule_cancel,schedule_list,memory_read,memory_write,memory_append_event,run,run_sequence,info_lookup,create_command,app_inspect_dump,app_inspect_find,app_screenshot,app_click,app_drag,app_open,app_type,app_hotkey,app_close,app_batch"; dist\win-x64\tanit-cli.exe --log-level warn llm agent --router llama --model gemma4-12b-it-q4-k-m --api-key local --prompt "Read this file with file_read and return only its exact contents: C:/Users/zx/Desktop/pixlwiz/pixlwiz/tests/local/tool-fixture.txt" --single-turn --json --max-iter 3 --no-mcp --no-skills --planner --disable-tools $disable --log stemp\g4-fsplanner.json 1> stemp\g4-fsplanner.out 2>&1; Write-Output "exit=$LASTEXITCODE"

$env:PM_AGENT_LOCAL_NGL="-1"; $disable="image_transform,image_create,create_video,image_understand,image_from_camera,speak,audio_transcribe,ask_user,schedule_at,schedule_in,schedule_every,schedule_cancel,schedule_list,memory_read,memory_write,memory_append_event,run,run_sequence,info_lookup,create_command,app_inspect_dump,app_inspect_find,app_screenshot,app_click,app_drag,app_open,app_type,app_hotkey,app_close,app_batch"; dist\win-x64\tanit-cli.exe --log-level warn llm agent --router llama --model gemma4-12b-it-q4-k-m --api-key local --prompt "Read this file with file_read and return only its exact contents: C:/Users/zx/Desktop/pixlwiz/pixlwiz/tests/local/tool-fixture.txt" --single-turn --json --max-iter 3 --no-mcp --no-skills --planner --disable-tools $disable --log stemp\g4-fsplanner2.json 1> stemp\g4-fsplanner2.out 2>&1; Write-Output "exit=$LASTEXITCODE"

 * A dedicated warmup run (model load + one short generation) runs before scored
 * cases unless --no-warmup. Timings in reports are post-warmup; see warmup report.
 *
 * Reports: tests/local/report-warmup-model-<slug>.md
 *          tests/local/report-<case>-model-<slug>.md
 *          tests/local/report-summary-model-<slug>.md|.json
 */

import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { deflateSync } from 'node:zlib';

const __dirname = dirname(fileURLToPath(import.meta.url));
const STEMP_DIR = join(PACKAGE_ROOT, 'stemp');
const RUN_DIR = join(STEMP_DIR, 'agent-local');
const REPORT_DIR = join(PACKAGE_ROOT, 'tests', 'local');
const DEFAULT_CWD_ROOT = './stemp';
const DEFAULT_TIMEOUT_MS = 180_000;
const DEFAULT_DUMMY_API_KEY = 'iloveyou';
const WARMUP_PROMPT = 'Reply with exactly one word: ok';
const WARMUP_ID = 'warmup';
const TOOL_CASE_ID = 'tool-file-read';
const IMAGE_CASE_ID = 'image-red-square';
const TOOL_FIXTURE_TEXT = 'LOCAL_LLAMA_TOOL_FIXTURE_OK_42';
// Unique, easy-to-match tokens for the agentic tool cases (file_glob / file_search).
// Distinct strings keep the synthesised answer deterministic and unambiguous.
const GLOB_FIXTURE_NAME = 'GLOB_FIXTURE_UNIQUE_8842';
const SEARCH_FIXTURE_TOKEN = 'SEARCH_HIT_LINE_5521';
// Chat preset used for tool cases: trims the path-tool catalog so the planner
// has a small, relevant set to pre-select from. Override with --preset.
const LOCAL_PRESET = argValue('--preset') || process.env.PM_AGENT_LOCAL_PRESET || 'local';
// Tool turns need >1 iteration (tool call round + synthesis round).
const TOOL_MAX_ITER = Number(argValue('--tool-max-iter') || process.env.PM_AGENT_LOCAL_TOOL_MAX_ITER || 3);

const BASELINE_MODEL_HINTS = [
  // gemma-4 E4B: native tool-calling + vision in a ~4.7 GB / 8 GB-VRAM footprint.
  // Fast+agentic sweet spot — file_read tool round-trips end-to-end in ~9 s cold
  // (planner select + tool exec + synthesis), vs ~34 s for the 12b at full ctx.
  'gemma4-e4b-it-q4-k-m',
  'gemma-4-e4b',
  'gemma4-e4b',
  'gemma4-12b-it-q4-k-m',
  'gemma-4-12b',
  'gemma4-12b',
  'gemma3-12b-it-q4-k-m',
  'gemma-3-12b',
  'gemma3-12b',
  'gemma3-4b',
  'gemma-3-4b',
  'gemma-4b',
  'gemma3',
  'gemma',
];

const CASES = [
  {
    id: 'arithmetic-exact',
    prompt:
      'Return only the answer token. No punctuation. No explanation.\n' +
      'What is 17 + 25?\n' +
      'Answer token:',
    expect: /^42$/,
  },
  {
    id: 'json-exact',
    prompt:
      'Return only compact JSON with keys "animal" and "count". No markdown. No explanation.\n' +
      'animal = otter, count = 3',
    expect: /^\{"animal":"otter","count":3\}$/,
    normalize: (s) => {
      let t = s.trim();
      const fence = t.match(/```(?:json)?\s*([\s\S]*?)```/i);
      if (fence) t = fence[1].trim();
      return t.replace(/\s+/g, '');
    },
  },
  {
    id: 'instruction-following',
    prompt:
      'Return exactly three lowercase words separated by commas, no spaces: red, green, blue',
    expect: /^red,green,blue$/,
    normalize: (s) => s.trim().replace(/\s*,\s*/g, ','),
  },
  {
    id: 'translation-es',
    prompt:
      'Translate the English word "cat" to Spanish. Return only the lowercase translated word.',
    expect: /^gato$/,
    normalize: (s) => s.trim().toLowerCase().replace(/[."']/g, ''),
  },
  {
    id: 'spelling-correction',
    prompt:
      'Correct the spelling. Return only the corrected phrase, lowercase: teh quick brwon fox',
    expect: /^the quick brown fox$/,
    normalize: (s) => s.trim().toLowerCase().replace(/[."']/g, ''),
  },
  {
    id: 'grammar-correction',
    prompt:
      'Correct the grammar. Return only the corrected sentence: She don\'t like apples.',
    expect: /^she doesn't like apples\.$/,
    normalize: (s) => s.trim().toLowerCase().replace(/[“”]/g, '"').replace(/[‘’]/g, "'"),
  },
  {
    id: 'translation-de',
    prompt:
      'Translate the English word "dog" to German. Return only the lowercase translated word.',
    expect: /^hund$/,
    normalize: (s) => s.trim().toLowerCase().replace(/[."']/g, ''),
  },
  {
    id: 'uppercase-transform',
    prompt: 'Convert to uppercase. Return only the result, nothing else: hello world',
    expect: /^HELLO WORLD$/,
    normalize: (s) => s.trim().replace(/[."']/g, ''),
  },
  {
    id: 'antonym-exact',
    prompt: 'Return only the single lowercase antonym of the word "hot". No punctuation.',
    expect: /^cold$/,
    normalize: (s) => s.trim().toLowerCase().replace(/[."']/g, ''),
  },
  {
    id: 'extract-number',
    prompt:
      'Return only the integer, no words: The order shipped 7 boxes containing 12 items each.\n' +
      'How many items in total?',
    expect: /^84$/,
    normalize: (s) => s.trim().replace(/[^0-9]/g, ''),
  },
];

function cliExePath() {
  const llm = process.env.PM_LLM_EXE;
  if (llm && llm.trim() && existsSync(llm.trim())) return resolve(llm.trim());
  const env = process.env.PM_IMAGE_CLI || process.env.MEDIA_IMG_CLI_EXE;
  if (env && env.trim()) return resolve(env.trim());
  const name = process.platform === 'win32' ? 'tanit-cli.exe' : 'tanit-cli';
  const winX64 = resolve(PACKAGE_ROOT, 'dist', 'win-x64', name);
  if (existsSync(winX64)) return winX64;
  return resolve(PACKAGE_ROOT, 'dist', name);
}

function argValue(name) {
  const prefix = `${name}=`;
  const direct = process.argv.find((arg) => arg.startsWith(prefix));
  if (direct) return direct.slice(prefix.length);
  const i = process.argv.indexOf(name);
  if (i >= 0 && i + 1 < process.argv.length) return process.argv[i + 1];
  return '';
}

function hasArg(name) {
  return process.argv.includes(name);
}

function runCli(exe, args, { timeout = DEFAULT_TIMEOUT_MS, env = process.env } = {}) {
  const started = performance.now();
  const result = spawnSync(exe, prepareCliArgs(exe, args), {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout,
    env,
  });
  const elapsedMs = Math.round((performance.now() - started) * 100) / 100;
  return {
    status: result.status ?? -1,
    signal: result.signal || '',
    stdout: result.stdout || '',
    stderr: result.stderr || '',
    elapsedMs,
    timedOut: Boolean(result.error && result.error.code === 'ETIMEDOUT'),
  };
}

function parseJsonObject(text) {
  const raw = String(text || '').trim();
  const start = raw.indexOf('{');
  if (start < 0) return null;
  try {
    return JSON.parse(raw.slice(start));
  } catch {
    return null;
  }
}

function readJsonFileIfExists(path) {
  if (!path || !existsSync(path)) return null;
  try {
    return JSON.parse(readFileSync(path, 'utf8').replace(/^\uFEFF/, ''));
  } catch {
    return null;
  }
}

function mmprojPathForModel(modelEntry) {
  const explicit = argValue('--mmproj') || process.env.PM_AGENT_LOCAL_MMPROJ || '';
  if (explicit) return resolve(explicit);
  const manifestPath = modelEntry?.manifest_path || '';
  const manifest = readJsonFileIfExists(manifestPath);
  const mmproj = manifest?.files?.mmproj || manifest?.mmproj;
  if (!mmproj) return '';
  return resolve(dirname(manifestPath), mmproj);
}

function extractFinalText(json, stdout) {
  const candidates = [
    json?.final_text,
    json?.result?.final_text,
    json?.transcript?.summary,
    json?.answer,
    json?.text,
  ];
  for (const value of candidates) {
    if (typeof value === 'string' && value.trim()) return value.trim();
  }
  return String(stdout || '').trim();
}

function crc32(buf) {
  const table = crc32.table || (crc32.table = Array.from({ length: 256 }, (_, n) => {
    let c = n;
    for (let k = 0; k < 8; k += 1) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    return c >>> 0;
  }));
  let crc = 0xffffffff;
  for (const byte of buf) crc = table[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

function pngChunk(type, data) {
  const typeBuf = Buffer.from(type, 'ascii');
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length, 0);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(Buffer.concat([typeBuf, data])), 0);
  return Buffer.concat([len, typeBuf, data, crc]);
}

function writeSolidPng(path, { width = 64, height = 64, rgb = [255, 0, 0] } = {}) {
  const raw = Buffer.alloc((width * 3 + 1) * height);
  for (let y = 0; y < height; y += 1) {
    const row = y * (width * 3 + 1);
    raw[row] = 0;
    for (let x = 0; x < width; x += 1) {
      const i = row + 1 + x * 3;
      raw[i] = rgb[0];
      raw[i + 1] = rgb[1];
      raw[i + 2] = rgb[2];
    }
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 2; // truecolor
  writeFileSync(path, Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    pngChunk('IHDR', ihdr),
    pngChunk('IDAT', deflateSync(raw)),
    pngChunk('IEND', Buffer.alloc(0)),
  ]));
}

function discoverLocalModels(exe) {
  const result = runCli(exe, ['--log-level', 'warn', 'llm', 'info', 'models', '--provider', 'llama', '--json'], {
    timeout: 30_000,
  });
  if (result.status !== 0) {
    return { ok: false, error: result.stderr || result.stdout || `exit ${result.status}`, models: [] };
  }
  const doc = parseJsonObject(result.stdout);
  const models = Array.isArray(doc?.models) ? doc.models : [];
  return { ok: true, error: '', models, roots: doc?.roots || [] };
}

function chooseModel(models, explicit) {
  if (explicit) return explicit;
  const valid = models.filter((m) => m && m.valid === true && typeof m.id === 'string' && m.id);
  const gemma = valid.find((m) => {
    const hay = `${m.id} ${m.label || ''} ${m.display_name || ''}`.toLowerCase();
    return BASELINE_MODEL_HINTS.some((hint) => hay.includes(hint));
  });
  if (gemma) return gemma.id;
  return valid[0]?.id || '';
}

function assertCondition(condition, message) {
  if (!condition) throw new Error(message);
}

function modelSlug(modelId) {
  return String(modelId || 'unknown')
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '') || 'unknown';
}

function readCMakeCacheFlag(name) {
  const cachePath = join(PACKAGE_ROOT, 'build', 'release', 'CMakeCache.txt');
  if (!existsSync(cachePath)) return null;
  const cache = readFileSync(cachePath, 'utf8');
  const m = cache.match(new RegExp(`^${name}:[^=]*=(ON|OFF|TRUE|FALSE|1|0)$`, 'm'));
  if (!m) return null;
  return /^(ON|TRUE|1)$/i.test(m[1]);
}

function detectGpuBackend(ngl) {
  if (ngl === '0' || ngl === 0) {
    return { name: 'CPU', detail: 'CPU-only (n_gpu_layers=0)', ngl: 0, ngl_label: '0 (CPU)' };
  }
  if (readCMakeCacheFlag('GGML_CUDA') || readCMakeCacheFlag('FEATURE_LLAMA_CUDA')) {
    return {
      name: 'CUDA',
      detail: 'GGML_CUDA=ON',
      ngl: ngl === '-1' || ngl === -1 ? -1 : Number(ngl),
      ngl_label: ngl === '-1' || ngl === -1 ? 'all layers (GPU)' : `${ngl} layers`,
    };
  }
  if (readCMakeCacheFlag('GGML_VULKAN') || readCMakeCacheFlag('FEATURE_LLAMA_VULKAN')) {
    return {
      name: 'Vulkan',
      detail: 'GGML_VULKAN=ON',
      ngl: ngl === '-1' || ngl === -1 ? -1 : Number(ngl),
      ngl_label: ngl === '-1' || ngl === -1 ? 'all layers (GPU)' : `${ngl} layers`,
    };
  }
  return {
    name: 'unknown',
    detail: 'GPU backend not detected in CMakeCache.txt',
    ngl: ngl === '-1' || ngl === -1 ? -1 : Number(ngl),
    ngl_label: String(ngl),
  };
}

function queryGpuMemory() {
  const res = spawnSync('nvidia-smi', [
    '--query-gpu=index,name,memory.used,memory.total',
    '--format=csv,noheader,nounits',
  ], { encoding: 'utf8', timeout: 5_000 });
  if (res.status !== 0 || !String(res.stdout || '').trim()) return null;
  const gpus = String(res.stdout).trim().split(/\r?\n/).map((line) => {
    const [index, name, used, total] = line.split(',').map((s) => s.trim());
    return {
      index: parseInt(index, 10),
      name,
      used_mib: parseInt(used, 10),
      total_mib: parseInt(total, 10),
    };
  }).filter((g) => Number.isFinite(g.used_mib) && Number.isFinite(g.total_mib));
  if (gpus.length === 0) return null;
  return {
    gpus,
    total_used_mib: gpus.reduce((sum, g) => sum + g.used_mib, 0),
    total_mib: gpus.reduce((sum, g) => sum + g.total_mib, 0),
  };
}

function startGpuMemorySampler(intervalMs = 500) {
  const baseline = queryGpuMemory();
  let peak = baseline;
  const record = () => {
    const sample = queryGpuMemory();
    if (!sample) return;
    if (!peak || sample.total_used_mib > peak.total_used_mib) peak = sample;
  };
  record();
  const timer = setInterval(record, intervalMs);
  return {
    stop() {
      clearInterval(timer);
      record();
      return {
        available: Boolean(peak),
        baseline_used_mib: baseline?.total_used_mib ?? null,
        peak_used_mib: peak?.total_used_mib ?? null,
        peak_delta_mib: peak && baseline ? peak.total_used_mib - baseline.total_used_mib : null,
        total_mib: peak?.total_mib ?? baseline?.total_mib ?? null,
        devices: peak?.gpus ?? baseline?.gpus ?? [],
      };
    },
  };
}

function extractProviderMeta(json, logPath) {
  const rounds = json?.llm_usage_aggregate?.llm_rounds
    || json?.result?.llm_usage?.llm_rounds;
  if (Array.isArray(rounds) && rounds.length) {
    const last = rounds[rounds.length - 1];
    if (last?.provider_meta) return last.provider_meta;
  }
  try {
    const raw = json?.provider_meta_json || json?.result?.provider_meta_json;
    if (typeof raw === 'string' && raw.trim()) return JSON.parse(raw);
    if (raw && typeof raw === 'object') return raw;
  } catch {
    /* ignore */
  }
  if (logPath && existsSync(logPath)) {
    try {
      const log = JSON.parse(readFileSync(logPath, 'utf8'));
      const llmRound = log?.events?.find((e) => e.kind === 'llm_round')?.detail;
      if (llmRound) {
        return {
          ...(llmRound.provider_meta || {}),
          duration_ms: llmRound.duration_ms,
          model: llmRound.model,
          provider: llmRound.provider || log?.provider?.router,
          n_gpu_layers: Number(process.env.PM_AGENT_LOCAL_NGL ?? -1),
          gpu_mode: Number(process.env.PM_AGENT_LOCAL_NGL ?? -1) === 0 ? 'cpu' : 'gpu-all',
        };
      }
    } catch {
      /* ignore */
    }
  }
  return null;
}

function reportPathFor(caseId, model) {
  return join(REPORT_DIR, `report-${caseId}-model-${modelSlug(model)}.md`);
}

function formatNumber(value, digits = 2) {
  return Number.isFinite(Number(value)) ? Number(value).toFixed(digits) : '—';
}

function writeCaseReport(row, { backend, modelEntry, timestamp }) {
  const isWarmup = row.phase === 'warmup';
  const isTool = row.phase === 'tool';
  const isImage = row.phase === 'image';
  const icon = isWarmup
    ? (row.status === 0 ? 'WARMUP OK' : 'WARMUP FAIL')
    : isTool
      ? (row.ok ? 'TOOL PASS' : 'TOOL FAIL')
      : isImage
        ? (row.ok ? 'IMAGE PASS' : 'IMAGE FAIL')
      : (row.ok ? 'PASS' : 'FAIL');
  const meta = row.providerMeta || {};
  const lines = [
    `# Local Agent — ${row.id}`,
    '',
    '| Field | Value |',
    '|:---|:---|',
    `| **Result** | ${icon} |`,
    `| **Phase** | ${isWarmup ? 'warmup (not scored)' : isTool ? 'tool smoke' : isImage ? 'image smoke' : 'measured'} |`,
    `| **Case** | \`${row.id}\` |`,
    `| **Model** | \`${row.model}\` |`,
    `| **Router** | \`llama\` |`,
    `| **Backend** | ${backend.name} (${backend.ngl_label}) |`,
    `| **Build** | ${backend.detail} |`,
    `| **Wall time** | ${row.elapsedMs} ms |`,
    `| **Inference** | ${meta.duration_ms != null ? `${meta.duration_ms} ms` : '—'} |`,
    `| **Prompt tokens** | ${meta.prompt_tokens ?? '—'} |`,
    `| **Completion tokens** | ${meta.completion_tokens ?? '—'} |`,
    `| **Decode speed** | ${meta.tokens_per_second != null ? `${formatNumber(meta.tokens_per_second)} tok/s` : '—'} |`,
    `| **Total speed** | ${meta.total_tokens_per_second != null ? `${formatNumber(meta.total_tokens_per_second)} tok/s` : '—'} |`,
    `| **GPU mode (meta)** | ${meta.gpu_mode || '—'} |`,
    `| **n_gpu_layers** | ${meta.n_gpu_layers != null ? meta.n_gpu_layers : backend.ngl} |`,
    `| **Timestamp** | ${timestamp} |`,
    `| **Exit** | ${row.status} |`,
    `| **Routing** | ${row.routingOk == null ? '—' : row.routingOk ? 'llama.cpp (ok)' : 'kbot/openai (unexpected)'} |`,
  ];
  if (!isWarmup) {
    lines.push(`| **Precision** | ${row.precisionOk ? 'ok' : 'fail'} |`);
  }
  if (isTool) {
    lines.push(`| **Tools enabled** | yes |`);
    lines.push(`| **Tool expected** | ${row.toolExpected || 'file_read'} |`);
  }
  if (isImage) {
    lines.push(`| **Image** | \`${row.imagePath || ''}\` |`);
    lines.push(`| **mmproj** | \`${row.mmprojPath || ''}\` |`);
  }
  if (modelEntry?.model_path) {
    lines.push(`| **Model path** | \`${modelEntry.model_path}\` |`);
  }
  if (row.gpuMemory?.available) {
    lines.push(`| **VRAM baseline** | ${row.gpuMemory.baseline_used_mib} / ${row.gpuMemory.total_mib} MiB |`);
    lines.push(`| **VRAM peak** | ${row.gpuMemory.peak_used_mib} / ${row.gpuMemory.total_mib} MiB |`);
    lines.push(`| **VRAM delta** | +${row.gpuMemory.peak_delta_mib} MiB |`);
    if (row.gpuMemory.devices?.length) {
      lines.push(`| **GPU devices** | ${row.gpuMemory.devices.map((g) => g.name).join('; ')} |`);
    }
  } else {
    lines.push('| **VRAM** | nvidia-smi unavailable (CPU-only run or no NVIDIA GPU) |');
  }
  lines.push(
    '',
    '## Prompt',
    '',
    '```text',
    row.prompt,
    '```',
    '',
    '## Output',
    '',
    '```text',
    row.finalText || '(empty)',
    '```',
    '',
    '## Normalized (scored)',
    '',
    '```text',
    row.normalized || '(empty)',
    '```',
  );
  if (row.error) {
    lines.push('', '## Error', '', '```text', row.error, '```');
  }
  if (row.stderr) {
    lines.push('', '## Stderr', '', '```text', row.stderr, '```');
  }
  if (row.logPath && existsSync(row.logPath)) {
    lines.push('', '## Agent log', '', `\`${row.logPath}\``);
  }
  lines.push('');
  writeFileSync(reportPathFor(row.id, row.model), lines.join('\n'), 'utf8');
}

function writeSummaryReport(summary, { backend, timestamp, modelEntry }) {
  const slug = modelSlug(summary.model);
  const measured = summary.rounds.filter((r) => r.phase === 'measured');
  const steadyMs = measured.map((r) => r.elapsedMs);
  const steadyAvg = steadyMs.length
    ? Math.round((steadyMs.reduce((a, b) => a + b, 0) / steadyMs.length) * 100) / 100
    : 0;
  const md = [
    '# Local Agent Test Summary',
    '',
    '| Field | Value |',
    '|:---|:---|',
    `| **Model** | \`${summary.model}\` |`,
    `| **Router** | \`llama\` |`,
    `| **Backend** | ${backend.name} (${backend.ngl_label}) |`,
    `| **Build** | ${backend.detail} |`,
    `| **Timestamp** | ${timestamp} |`,
    `| **Warmup** | ${summary.warmupRan ? `yes (${summary.warmup?.elapsedMs ?? '—'} ms)` : 'skipped (--no-warmup)'} |`,
    `| **Passed** | ${summary.passed}/${summary.total} (measured only) |`,
    `| **Tool suite** | ${(summary.toolCases || []).filter((t) => t.ok).length}/${(summary.toolCases || []).length} passed |`,
    `| **Image smoke** | ${summary.imageCase ? (summary.imageCase.ok ? 'passed' : 'failed') : 'skipped'} |`,
    `| **Measured wall (total)** | ${summary.measuredElapsedMs} ms |`,
    `| **Measured wall (avg)** | ${steadyAvg} ms |`,
    `| **GPU tested** | ${summary.gpuTested ? 'yes (VRAM sampled)' : 'no (nvidia-smi missing or --ngl 0)'} |`,
    '',
    '_Each case is a fresh `tanit-cli` process; warmup primes GGUF page cache and GPU init before scored runs._',
    '',
    '## Cases (post-warmup)',
    '',
    '| Case | Phase | Result | Wall ms | Infer ms | Tok/s | VRAM Δ MiB | Report |',
    '|:---|:---|:---:|---:|---:|---:|---:|:---|',
    ...(summary.warmup ? (() => {
      const w = summary.warmup;
      const vram = w.gpuMemory?.available ? `+${w.gpuMemory.peak_delta_mib}` : '—';
      const infer = w.providerMeta?.duration_ms ?? '—';
      const tps = w.providerMeta?.tokens_per_second != null ? formatNumber(w.providerMeta.tokens_per_second) : '—';
      const rel = `report-${WARMUP_ID}-model-${slug}.md`;
      return [`| \`${WARMUP_ID}\` | warmup | ${w.status === 0 ? 'ok' : 'fail'} | ${w.elapsedMs} | ${infer} | ${tps} | ${vram} | [${rel}](./${rel}) |`];
    })() : []),
    ...(summary.toolCases || []).map((t) => {
      const vram = t.gpuMemory?.available ? `+${t.gpuMemory.peak_delta_mib}` : '—';
      const infer = t.providerMeta?.duration_ms ?? '—';
      const tps = t.providerMeta?.tokens_per_second != null ? formatNumber(t.providerMeta.tokens_per_second) : '—';
      const rel = `report-${t.id}-model-${slug}.md`;
      return `| \`${t.id}\` | tool (${t.toolExpected || '—'}) | ${t.ok ? 'PASS' : 'FAIL'} | ${t.elapsedMs} | ${infer} | ${tps} | ${vram} | [${rel}](./${rel}) |`;
    }),
    ...(summary.imageCase ? (() => {
      const img = summary.imageCase;
      const vram = img.gpuMemory?.available ? `+${img.gpuMemory.peak_delta_mib}` : '—';
      const infer = img.providerMeta?.duration_ms ?? '—';
      const tps = img.providerMeta?.tokens_per_second != null ? formatNumber(img.providerMeta.tokens_per_second) : '—';
      const rel = `report-${IMAGE_CASE_ID}-model-${slug}.md`;
      return [`| \`${IMAGE_CASE_ID}\` | image smoke | ${img.ok ? 'PASS' : 'FAIL'} | ${img.elapsedMs} | ${infer} | ${tps} | ${vram} | [${rel}](./${rel}) |`];
    })() : []),
    ...measured.map((r) => {
      const vram = r.gpuMemory?.available ? `+${r.gpuMemory.peak_delta_mib}` : '—';
      const infer = r.providerMeta?.duration_ms ?? '—';
      const tps = r.providerMeta?.tokens_per_second != null ? formatNumber(r.providerMeta.tokens_per_second) : '—';
      const rel = `report-${r.id}-model-${slug}.md`;
      return `| \`${r.id}\` | measured | ${r.ok ? 'PASS' : 'FAIL'} | ${r.elapsedMs} | ${infer} | ${tps} | ${vram} | [${rel}](./${rel}) |`;
    }),
    '',
  ];
  writeFileSync(join(REPORT_DIR, `report-summary-model-${slug}.md`), md.join('\n'), 'utf8');
  writeFileSync(join(REPORT_DIR, `report-summary-model-${slug}.json`), JSON.stringify(summary, null, 2), 'utf8');
}

function extractErrorText(json, stdout, stderr) {
  if (json?.ok === true || json?.result?.ok === true) return '';
  const candidates = [
    json?.error,
    json?.result?.error,
    json?.transcript?.error,
    stderr,
    stdout,
  ];
  for (const value of candidates) {
    if (typeof value === 'string' && value.trim()) return value.trim();
  }
  return '';
}

/** Fail if the run accidentally used kbot's OpenAI-compatible HTTP client. */
function assertLocalLlamaRouting({ stdout, stderr, json }) {
  const blob = `${stdout}\n${stderr}\n${JSON.stringify(json || {})}`;
  const forbidden = [
    /api\.openai\.com/i,
    /openrouter\.ai/i,
    /liboai/i,
    /LLMClient::execute_chat/i,
    /API Key is empty/i,
    /router=llama must use built-in/i,
  ];
  for (const pattern of forbidden) {
    assertCondition(!pattern.test(blob), `expected local llama routing; matched ${pattern}: ${blob.slice(0, 400)}`);
  }
  const provider = json?.provider?.router
    || json?.result?.provider?.router
    || json?.transcript?.provider?.router;
  if (provider) {
    assertCondition(provider === 'llama', `expected provider router llama, got ${provider}`);
  }
}

function main() {
  const exe = cliExePath();
  const explicitModel = argValue('--model') || process.env.PM_AGENT_LOCAL_MODEL || '';
  const apiKey = argValue('--api-key') || process.env.PM_AGENT_LOCAL_API_KEY || DEFAULT_DUMMY_API_KEY;
  const required = hasArg('--require-model') || process.env.PM_AGENT_LOCAL_REQUIRE_MODEL === '1';
  const timeout = Number(argValue('--timeout-ms') || process.env.PM_AGENT_LOCAL_TIMEOUT_MS || DEFAULT_TIMEOUT_MS);
  const ngl = argValue('--ngl') || process.env.PM_AGENT_LOCAL_NGL || '-1';
  const backend = detectGpuBackend(ngl);
  const timestamp = new Date().toISOString();

  rmSync(RUN_DIR, { recursive: true, force: true, maxRetries: 3, retryDelay: 200 });
  mkdirSync(RUN_DIR, { recursive: true });
  mkdirSync(REPORT_DIR, { recursive: true });
  const toolFixturePath = join(REPORT_DIR, 'tool-fixture.txt');
  writeFileSync(toolFixturePath, `${TOOL_FIXTURE_TEXT}\n`, 'utf8');
  const toolFixturePromptPath = toolFixturePath.replace(/\\/g, '/');

  // file_glob fixture: a folder with one uniquely-named .md so the synthesised
  // answer is deterministic (the model must surface the exact filename).
  const globFixtureDir = join(REPORT_DIR, 'tool-glob');
  mkdirSync(globFixtureDir, { recursive: true });
  writeFileSync(join(globFixtureDir, `${GLOB_FIXTURE_NAME}.md`), '# glob fixture\n', 'utf8');
  writeFileSync(join(globFixtureDir, 'ignore-me.txt'), 'not markdown\n', 'utf8');
  const globFixtureDirPrompt = globFixtureDir.replace(/\\/g, '/');

  // file_search fixture: one unique token on its own line for grep-style search.
  const searchFixturePath = join(REPORT_DIR, 'tool-search.txt');
  writeFileSync(
    searchFixturePath,
    `alpha line\n${SEARCH_FIXTURE_TOKEN}\nomega line\n`,
    'utf8',
  );
  const searchFixturePromptPath = searchFixturePath.replace(/\\/g, '/');

  const imageFixturePath = join(REPORT_DIR, 'image-red-square.png');
  writeSolidPng(imageFixturePath);

  const catalog = discoverLocalModels(exe);
  const model = chooseModel(catalog.models, explicitModel);

  const modelEntry = catalog.models.find((m) => m.id === model) || null;
  const mmprojPath = mmprojPathForModel(modelEntry);

  console.log('local agent harness');
  console.log(`  exe      : ${exe}`);
  console.log(`  run dir  : ${RUN_DIR}`);
  console.log(`  reports  : ${REPORT_DIR}`);
  console.log(`  model    : ${model || '(none)'}`);
  console.log(`  backend  : ${backend.name} (${backend.ngl_label})`);
  console.log(`  ngl      : ${ngl} (PM_AGENT_LOCAL_NGL)`);
  console.log(`  api key  : ${apiKey ? '(set)' : '(empty)'}`);
  console.log(`  catalog  : ${catalog.models.length} entries`);

  if (!model) {
    const msg = 'No valid local llama text/planner model found. Add a text GGUF/model.json under config_dir/models or dist/models, or pass --model <id-or-path>.';
    if (required) throw new Error(msg);
    console.log(`SKIP: ${msg}`);
    writeFileSync(join(RUN_DIR, 'summary.json'), JSON.stringify({
      skipped: true,
      reason: msg,
      catalog,
    }, null, 2), 'utf8');
    return;
  }

  const env = {
    ...process.env,
    PM_IMAGE_DEBUG_DUMPS: '1',
    PM_AGENT_LOCAL_NGL: String(ngl),
  };
  const skipWarmup = hasArg('--no-warmup') || process.env.PM_AGENT_LOCAL_NO_WARMUP === '1';

  function runAgentPass({ id, prompt, phase, testCase, toolsEnabled = false, toolExpected = '' }) {
    const logPath = join(RUN_DIR, `${id}.json`);
    const gpuSampler = startGpuMemorySampler();
    // Tool cases run through the `local` chat preset (trimmed tool catalog) with
    // the planner enabled, so the model only ever sees the handful of tools the
    // planner pre-selects instead of the full ~16K-token catalog. The explicit
    // --router/--model override the preset's cloud router, keeping local llama.
    // Tool turns also need >1 iteration: round 1 emits the tool call, round 2
    // synthesises the result into the final answer. Text cases stay lean
    // (--no-tools / --no-planner) since they have nothing to dispatch.
    const args = [
      '--log-level', 'warn',
      '--cwd', DEFAULT_CWD_ROOT,
      'llm', 'agent',
      '--router', 'llama',
      '--model', model,
      '--api-key', apiKey,
      '--prompt', prompt,
      '--single-turn',
      '--json',
      '--max-iter', toolsEnabled ? String(TOOL_MAX_ITER) : '1',
      '--no-mcp',
      '--no-skills',
      '--log', logPath,
    ];
    if (toolsEnabled) {
      args.splice(args.indexOf('--no-mcp'), 0, '--preset', LOCAL_PRESET, '--planner');
    } else {
      args.splice(args.indexOf('--no-mcp'), 0, '--no-tools', '--no-planner');
    }
    const result = runCli(exe, args, { timeout, env });
    const gpuMemory = gpuSampler.stop();
    const json = parseJsonObject(result.stdout);
    const providerMeta = extractProviderMeta(json, logPath);
    const finalText = extractFinalText(json, result.stdout);
    const errorText = result.status === 0 ? '' : extractErrorText(json, result.stdout, result.stderr);
    const normalized = testCase
      ? (testCase.normalize || ((s) => s.trim()))(finalText)
      : finalText.trim();
    const precisionOk = testCase
      ? result.status === 0 && testCase.expect.test(normalized)
      : result.status === 0;
    let routingOk = null;
    if ((phase === 'measured' || phase === 'tool') && result.status === 0) {
      routingOk = true;
      try {
        assertLocalLlamaRouting({ stdout: result.stdout, stderr: result.stderr, json });
      } catch (routeErr) {
        routingOk = false;
        console.log(`  routing: FAIL — ${routeErr.message}`);
      }
    } else if (phase === 'measured') {
      routingOk = false;
    } else if (phase === 'tool') {
      routingOk = !/api\.openai\.com|openrouter\.ai|liboai|API Key is empty/i.test(
        `${result.stdout}\n${result.stderr}\n${JSON.stringify(json || {})}`,
      );
    }
    const ok = phase === 'warmup'
      ? result.status === 0
      : phase === 'tool'
        ? precisionOk && routingOk
        : precisionOk && routingOk;
    const row = {
      id,
      phase,
      status: result.status,
      signal: result.signal,
      timedOut: result.timedOut,
      elapsedMs: result.elapsedMs,
      model,
      prompt,
      finalText,
      normalized,
      error: errorText,
      ok,
      precisionOk: phase === 'measured' || phase === 'tool' ? precisionOk : null,
      routingOk,
      toolExpected,
      logPath,
      stderr: result.stderr.trim(),
      providerMeta,
      gpuMemory,
      backend: backend.name,
      ngl,
    };
    writeCaseReport(row, { backend, modelEntry, timestamp });
    console.log(`  report   : ${reportPathFor(row.id, row.model)}`);
    return row;
  }

  function runVlmPass({ imagePath, mmprojPath }) {
    const prompt = 'What is the dominant color in this image? Return exactly one lowercase word.';
    const gpuSampler = startGpuMemorySampler();
    const args = [
      '--log-level', 'warn',
      'llama', 'vlm',
      '--model', modelEntry.model_path,
      '--mmproj', mmprojPath,
      '--image', imagePath,
      '--prompt', prompt,
      '--n-predict', '16',
      '--ngl', String(ngl),
    ];
    const result = runCli(exe, args, { timeout, env });
    const gpuMemory = gpuSampler.stop();
    const json = parseJsonObject(result.stdout);
    const finalText = extractFinalText(json, result.stdout);
    const normalized = finalText.trim().toLowerCase();
    const redAnswer = /\bred\b/.test(normalized)
      || json?.dominant_color === 'red'
      || json?.color === 'red';
    const precisionOk = result.status === 0 && redAnswer;
    const row = {
      id: IMAGE_CASE_ID,
      phase: 'image',
      status: result.status,
      signal: result.signal,
      timedOut: result.timedOut,
      elapsedMs: result.elapsedMs,
      model,
      prompt,
      finalText,
      normalized,
      error: result.status === 0 ? '' : extractErrorText(json, result.stdout, result.stderr),
      ok: precisionOk,
      precisionOk,
      routingOk: null,
      logPath: '',
      stderr: result.stderr.trim(),
      providerMeta: json?.elapsed_ms != null ? { duration_ms: json.elapsed_ms, n_gpu_layers: Number(ngl), gpu_mode: ngl === '0' ? 'cpu' : 'gpu-all' } : null,
      gpuMemory,
      backend: backend.name,
      ngl,
      imagePath,
      mmprojPath,
    };
    writeCaseReport(row, { backend, modelEntry, timestamp });
    console.log(`  report   : ${reportPathFor(row.id, row.model)}`);
    return row;
  }

  let warmup = null;
  if (!skipWarmup) {
    console.log('\n── Warmup (model load + one generation, not scored) ──');
    warmup = runAgentPass({ id: WARMUP_ID, prompt: WARMUP_PROMPT, phase: 'warmup' });
    console.log(`${WARMUP_ID}: ${warmup.status === 0 ? 'ok' : 'fail'} ${warmup.elapsedMs} ms`);
    if (warmup.finalText) console.log(`  final: ${warmup.finalText}`);
    if (warmup.gpuMemory?.available) {
      console.log(`  vram     : peak ${warmup.gpuMemory.peak_used_mib} MiB (+${warmup.gpuMemory.peak_delta_mib} MiB)`);
    }
    assertCondition(warmup.status === 0, `warmup failed (exit ${warmup.status}): ${warmup.stderr || 'see report'}`);
  } else {
    console.log('\n── Warmup skipped (--no-warmup) ──');
  }

  // Agentic tool suite: each case drives the planner → native tool call →
  // executor → synthesis loop end-to-end. The fixtures are crafted so the
  // correct answer is a unique token, so a passing run proves real execution
  // (not a hallucinated reply).
  const toolPlans = [
    {
      id: TOOL_CASE_ID,
      tool: 'file_read',
      prompt: `Read this file with file_read and return only its exact contents: ${toolFixturePromptPath}`,
      expect: new RegExp(`^${TOOL_FIXTURE_TEXT}$`),
      normalize: (s) => s.trim(),
    },
    {
      id: 'tool-file-glob',
      tool: 'file_glob',
      // Embed the full pattern (dir + *.md) so the model passes it verbatim as the
      // `pattern` arg. Phrasing it as "the .md files in <folder>" lets small models
      // drop the directory and glob the cwd instead — a prompt issue, not a tool one.
      prompt:
        'Use file_glob with the pattern set to exactly this string ' +
        `(pass it verbatim, do not change it): ${globFixtureDirPrompt}/*.md\n` +
        'Return only the matching file name(s), nothing else.',
      expect: new RegExp(GLOB_FIXTURE_NAME),
      normalize: (s) => s.trim(),
    },
    {
      id: 'tool-file-search',
      tool: 'file_search',
      prompt:
        `Use file_search to find the line containing the token ${SEARCH_FIXTURE_TOKEN} ` +
        `in this file: ${searchFixturePromptPath}\n` +
        'Return only the matching line text.',
      expect: new RegExp(SEARCH_FIXTURE_TOKEN),
      normalize: (s) => s.trim(),
    },
  ];

  console.log('\n── Agentic tool suite (planner → native tool call → executor → synthesis) ──');
  const toolCases = [];
  for (const plan of toolPlans) {
    const row = runAgentPass({
      id: plan.id,
      prompt: plan.prompt,
      phase: 'tool',
      toolExpected: plan.tool,
      testCase: { expect: plan.expect, normalize: plan.normalize },
      toolsEnabled: true,
    });
    toolCases.push(row);
    console.log(`\n${plan.id} (${plan.tool}): ${row.ok ? 'PASS' : 'FAIL'} ${row.elapsedMs} ms`);
    console.log(`  final: ${row.finalText || '(empty)'}`);
    if (row.error && !row.ok) console.log(`  error: ${row.error}`);
    if (row.gpuMemory?.available) {
      console.log(`  vram     : peak ${row.gpuMemory.peak_used_mib} MiB (+${row.gpuMemory.peak_delta_mib} MiB)`);
    }
  }
  // Primary smoke (file_read) kept as `toolCase` for back-compat in the summary.
  const toolCase = toolCases[0];

  let imageCase = null;
  if (modelEntry?.model_path && mmprojPath && existsSync(mmprojPath)) {
    console.log('\n── Image smoke (llama vlm with local mmproj) ──');
    imageCase = runVlmPass({ imagePath: imageFixturePath, mmprojPath });
    console.log(`${IMAGE_CASE_ID}: ${imageCase.ok ? 'PASS' : 'FAIL'} ${imageCase.elapsedMs} ms`);
    console.log(`  final: ${imageCase.finalText || '(empty)'}`);
    if (imageCase.error && !imageCase.ok) console.log(`  error: ${imageCase.error}`);
    if (imageCase.gpuMemory?.available) {
      console.log(`  vram     : peak ${imageCase.gpuMemory.peak_used_mib} MiB (+${imageCase.gpuMemory.peak_delta_mib} MiB)`);
    }
  } else {
    console.log('\n── Image smoke skipped (no mmproj in selected model manifest) ──');
  }

  const rounds = [];
  console.log('\n── Measured cases (post-warmup) ──');
  for (const testCase of CASES) {
    const row = runAgentPass({
      id: testCase.id,
      prompt: testCase.prompt,
      phase: 'measured',
      testCase,
    });
    rounds.push(row);
    const ok = row.ok && row.routingOk !== false;
    console.log(`\n${testCase.id}: ${ok ? 'PASS' : 'FAIL'} ${row.elapsedMs} ms`);
    if (row.routingOk === false) console.log('  routing: used kbot/openai path (expected llama.cpp)');
    console.log(`  final: ${row.finalText}`);
    if (row.stderr) console.log(`  stderr: ${row.stderr}`);
    if (row.gpuMemory?.available) {
      console.log(`  vram     : peak ${row.gpuMemory.peak_used_mib} MiB (+${row.gpuMemory.peak_delta_mib} MiB)`);
    }
  }

  const passed = rounds.filter((r) => r.ok && r.routingOk !== false).length;
  const gpuTested = [...rounds, warmup].filter(Boolean).some((r) => r.gpuMemory?.available) && ngl !== '0';
  const measuredElapsedMs = Math.round(rounds.reduce((sum, r) => sum + r.elapsedMs, 0) * 100) / 100;
  const summary = {
    skipped: false,
    timestamp,
    model,
    router: 'llama',
    backend,
    ngl,
    warmupRan: !skipWarmup,
    warmup,
    toolCase,
    toolCases,
    imageCase,
    gpuTested,
    passed,
    total: rounds.length,
    passRate: rounds.length ? passed / rounds.length : 0,
    measuredElapsedMs,
    elapsedMs: Math.round(((warmup?.elapsedMs || 0) + measuredElapsedMs) * 100) / 100,
    modelEntry,
    catalog,
    rounds,
  };
  writeSummaryReport(summary, { backend, timestamp, modelEntry });
  writeFileSync(join(RUN_DIR, 'summary.json'), JSON.stringify(summary, null, 2), 'utf8');
  writeFileSync(join(RUN_DIR, 'summary.md'), [
    '# Local Agent Summary',
    '',
    `- Model: \`${model}\``,
    `- Warmup: ${summary.warmupRan ? `${summary.warmup?.elapsedMs} ms` : 'skipped'}`,
    `- Passed: ${passed}/${rounds.length} (measured)`,
    `- Tool suite: ${toolCases.filter((t) => t.ok).length}/${toolCases.length} passed`,
    `- Image smoke: ${imageCase ? (imageCase.ok ? 'passed' : 'failed') : 'skipped'}`,
    `- Measured wall time: ${summary.measuredElapsedMs} ms`,
    `- Total (warmup + measured): ${summary.elapsedMs} ms`,
    '',
    '| Case | Result | Wall ms | Final |',
    '|:---|:---:|---:|:---|',
    ...toolCases.map((t) => `| \`${t.id}\` | ${t.ok ? 'PASS' : 'FAIL'} | ${t.elapsedMs} | ${String(t.finalText).replace(/\|/g, '\\|')} |`),
    ...(imageCase ? [`| \`${imageCase.id}\` | ${imageCase.ok ? 'PASS' : 'FAIL'} | ${imageCase.elapsedMs} | ${String(imageCase.finalText).replace(/\|/g, '\\|')} |`] : []),
    ...rounds.map((r) => `| \`${r.id}\` | ${r.ok ? 'PASS' : 'FAIL'} | ${r.elapsedMs} | ${String(r.finalText).replace(/\|/g, '\\|')} |`),
    '',
  ].join('\n'), 'utf8');

  console.log(`\n[report] ${join(REPORT_DIR, `report-summary-model-${modelSlug(model)}.md`)}`);
  console.log(`[report] ${join(REPORT_DIR, `report-summary-model-${modelSlug(model)}.json`)}`);

  assertCondition(passed === rounds.length, `local agent precision failed: ${passed}/${rounds.length} passed`);
  const failedTools = toolCases.filter((t) => !t.ok).map((t) => t.id);
  assertCondition(failedTools.length === 0, `local llama tool suite failed: ${failedTools.join(', ') || 'none'}`);
  if (imageCase) assertCondition(imageCase.ok, 'local llama image smoke failed');
}

main();

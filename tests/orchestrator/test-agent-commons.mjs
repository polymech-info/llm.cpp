import { llmExe, prepareCliArgs } from './exe.mjs';
import { REPO as PACKAGE_ROOT } from './repo.mjs';
export { PACKAGE_ROOT };
/**
 * Shared bootstrap + helpers for agent orchestrator harnesses.
 *
 * Used by test-agent-memory.mjs and future agent strategy harnesses.
 */

import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { buildMetricsBundle, renderMarkdownReport } from '@polymech/tanit/tests/orchestrator/reports.js';
import {
  appendMarkdownChapter,
  getChapterTitle,
  getNpmScriptLabel,
  isAccumulateMode,
  TEST_REPORT_ALL_PATH,
} from '@polymech/tanit/tests/orchestrator/test-report-accumulate.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
export const STEMP_DIR = join(PACKAGE_ROOT, 'stemp');
export const TEST_REPORT_PATH = join(PACKAGE_ROOT, 'tests', 'test-report-last.md');

export const DEFAULT_CHAT_PRESET = 'quick';
export const DEFAULT_LOG_ARGS = ['--log-level', 'warn'];

/** @typedef {'loopback' | 'live'} AgentHarnessMode */

/** @typedef {{
 *   packageRoot: string,
 *   stempDir: string,
 *   harnessDir: string,
 *   configDir: string,
 *   sessionDir: string,
 *   workDir: string,
 *   runsDir: string,
 * }} AgentHarnessPaths */

/** @typedef {{
 *   paths: AgentHarnessPaths,
 *   mode: AgentHarnessMode,
 *   preset: string,
 *   loopbackPresetId: string,
 *   displayName: string,
 *   onlyScopes: Set<string> | null,
 *   requireLive: boolean,
 *   memoryFeaturesMask: string,
 *   wantSuite: (name: string) => boolean,
 * }} AgentHarnessOptions */

export function parseArgValue(name, argv = process.argv.slice(2)) {
  const idx = argv.indexOf(name);
  if (idx >= 0 && argv[idx + 1]) return argv[idx + 1];
  const eq = argv.find((a) => a.startsWith(`${name}=`));
  return eq ? eq.slice(name.length + 1) : '';
}

/** @returns {AgentHarnessMode} */
export function parseHarnessMode(argv = process.argv.slice(2)) {
  const raw = (parseArgValue('--mode', argv) || process.env.PM_AGENT_MEMORY_MODE || 'loopback').trim().toLowerCase();
  return raw === 'live' ? 'live' : 'loopback';
}

export function parsePreset(argv = process.argv.slice(2)) {
  return (parseArgValue('--preset', argv) || process.env.PM_AGENT_MEMORY_PRESET || DEFAULT_CHAT_PRESET).trim()
    || DEFAULT_CHAT_PRESET;
}

export function parseOnlyScopes(argv = process.argv.slice(2)) {
  const raw = parseArgValue('--only', argv);
  if (!raw.trim()) return null;
  const set = new Set(raw.split(',').map((s) => s.trim().toLowerCase()).filter(Boolean));
  return set.size ? set : null;
}

export function parseMemoryFeaturesMask(argv = process.argv.slice(2)) {
  const fromEnv = process.env.PM_LLM_STRUCTURED_MEMORY_FEATURES?.trim();
  const fromArg = argv.find((a) => a.startsWith('--memory-features='));
  if (fromArg) return fromArg.slice('--memory-features='.length);
  return fromEnv || '';
}

export function createWantSuite(onlyScopes) {
  return (name) => !onlyScopes || onlyScopes.has(name);
}

export function parseRequireLive(argv = process.argv.slice(2)) {
  return argv.includes('--require-live');
}

/** @param {string} harnessSubdir */
export function resolveHarnessPaths(harnessSubdir) {
  const harnessDir = join(STEMP_DIR, harnessSubdir);
  return {
    packageRoot: PACKAGE_ROOT,
    stempDir: STEMP_DIR,
    harnessDir,
    configDir: join(harnessDir, 'config'),
    sessionDir: join(harnessDir, 'sessions'),
    workDir: join(harnessDir, 'work'),
    runsDir: join(harnessDir, 'runs'),
  };
}

export function parseJson(text) {
  const raw = String(text || '').trim();
  if (!raw) return null;
  try {
    return JSON.parse(raw);
  } catch {
    const start = Math.min(...[raw.indexOf('{'), raw.indexOf('[')].filter((n) => n >= 0));
    if (!Number.isFinite(start)) return null;
    try { return JSON.parse(raw.slice(start)); } catch { return null; }
  }
}

export function readJson(path) {
  return JSON.parse(readFileSync(path, 'utf8'));
}

export function truncateText(value, max = 120) {
  const text = String(value ?? '');
  return text.length <= max ? text : `${text.slice(0, max)}...`;
}

/** @param {AgentHarnessPaths} paths */
export function resetHarnessTree(paths, { keepAll = false } = {}) {
  mkdirSync(paths.stempDir, { recursive: true });
  if (keepAll) {
    mkdirSync(paths.configDir, { recursive: true });
    mkdirSync(paths.sessionDir, { recursive: true });
    mkdirSync(paths.workDir, { recursive: true });
    return;
  }
  // Session documents are analysis artifacts. Keep prior runs; bootstrap puts
  // each run in its own child directory so retained sessions cannot pollute
  // cross-session retrieval assertions in later runs.
  mkdirSync(paths.sessionDir, { recursive: true });
  rmSync(paths.workDir, { recursive: true, force: true, maxRetries: 3, retryDelay: 200 });
  mkdirSync(paths.workDir, { recursive: true });
  mkdirSync(paths.configDir, { recursive: true });
}

export function writeLoopbackHarnessConfig(configDir, loopbackPresetId) {
  writeFileSync(join(configDir, 'settings.json'), JSON.stringify({
    custom: {
      global_tools: {
        disabled_path_tools: [],
        mcp_tools_enabled: false,
        disabled_mcp_servers: [],
        disabled_mcp_tools: [],
      },
      settings_chat_presets: [
        {
          id: loopbackPresetId,
          name: loopbackPresetId,
          is_default: true,
          isDefault: true,
          chat: {
            router: 'loopback',
            model: 'loopback',
            timeout_ms: 30000,
            max_iterations: 8,
            planner_enabled: false,
          },
          tools: {
            disabled_path_tools: [],
            mcp_tools_enabled: false,
            disabled_mcp_servers: [],
            disabled_mcp_tools: [],
          },
        },
      ],
    },
    permissions: [],
  }, null, 2), 'utf8');
}

export function llmAgentHasConfiguredKey(exe, {
  paths,
  mode,
  preset,
  configDir = '',
  cwd,
  env = process.env,
} = {}) {
  const logPath = join(paths.harnessDir, '_dry-run-key-probe.json');
  const args = [
    ...DEFAULT_LOG_ARGS,
    ...(configDir ? ['--config-dir', configDir] : []),
    '--cwd', cwd,
    'llm', 'agent',
    '--prompt', 'ping',
    '--single-turn',
    '--dry-run',
    '--log', logPath,
  ];
  if (mode === 'live' && preset) {
    args.splice(args.indexOf('agent') + 1, 0, '--preset', preset);
  }
  const result = spawnSync(exe, prepareCliArgs(exe, args), {
    cwd: paths.packageRoot,
    encoding: 'utf8',
    timeout: 30_000,
    env,
  });
  if (existsSync(logPath)) {
    try {
      const provider = readJson(logPath)?.provider || {};
      if (provider.api_key_configured === true) return true;
      const router = String(provider.router || '').toLowerCase();
      const model = String(provider.model || '').toLowerCase();
      // Local llama.cpp / in-process GGUF has no cloud key.
      if (router === 'llama' || router === 'local' || model.startsWith('llama:'))
        return true;
    } catch { /* fall through */ }
  }
  const combined = `${result.stdout}\n${result.stderr}`;
  if (/\|\s*api_key\s*\|\s*set\s*\|/i.test(combined)) return true;
  if (/\|\s*api_key\s*\|\s*\*\*missing\*\*\s*\|/i.test(combined)) return false;
  return result.status === 0 && /llama/i.test(combined);
}

export function extractCommunication(logDoc, stdoutJson) {
  const result = logDoc?.result || stdoutJson || {};
  const transcript = result?.transcript || {};
  const planner = result?.planner || transcript?.planner || {};
  const events = Array.isArray(logDoc?.events) ? logDoc.events : [];
  return {
    planner: {
      meta: planner,
      messages: Array.isArray(planner?.messages) ? planner.messages : [],
    },
    main_agent: {
      messages: Array.isArray(transcript?.messages) ? transcript.messages : [],
      summary: transcript?.summary || null,
      final_text: result?.final_text ?? '',
      ok: result?.ok ?? null,
      error: result?.error ?? null,
      iterations: result?.iterations ?? null,
      llm_usage: result?.llm_usage || null,
    },
    tools: events
      .filter((event) => event.kind === 'tool_call' || event.kind === 'tool_result')
      .map((event) => ({
        kind: event.kind,
        tool: event.tool,
        detail: event.detail,
      })),
    memory_updates: events
      .filter((event) => event.kind === 'memory_update')
      .map((event) => event.detail),
  };
}

export function writeCommunicationBundle(runDir, roundKey, roundMeta, logPath, stdoutJson) {
  const logDoc = existsSync(logPath) ? readJson(logPath) : {};
  const communication = extractCommunication(logDoc, stdoutJson);
  const bundlePath = join(runDir, `${roundKey}-communication.json`);
  writeFileSync(bundlePath, JSON.stringify({
    round: roundMeta,
    log_path: logPath,
    communication,
  }, null, 2), 'utf8');
  return { logDoc, communication, bundlePath };
}

/**
 * Adapt per-round harness bundles to the UI session shape consumed by
 * scripts/session-reports.mjs, then invoke that generator.
 */
export function generateHarnessSessionReports({
  runDir,
  runId,
  displayName = 'agent-harness',
}) {
  if (!runDir || !existsSync(runDir)) {
    throw new Error('harness report generation requires an existing run directory');
  }
  const bundles = readdirSync(runDir)
    .filter((name) => name.endsWith('-communication.json'))
    .map((name) => ({ name, doc: readJson(join(runDir, name)) }))
    .sort((a, b) => {
      const ai = Number(a.doc?.round?.index);
      const bi = Number(b.doc?.round?.index);
      if (Number.isFinite(ai) && Number.isFinite(bi) && ai !== bi) return ai - bi;
      return a.name.localeCompare(b.name);
    });
  if (bundles.length === 0) {
    throw new Error('no communication bundles found for session report');
  }

  const now = Date.now();
  const entries = [];
  let entryId = 1;
  for (let i = 0; i < bundles.length; i += 1) {
    const bundle = bundles[i].doc;
    const round = bundle?.round || {};
    const logPath = String(bundle?.log_path || '');
    const agentJsonText = logPath && existsSync(logPath)
      ? readFileSync(logPath, 'utf8')
      : undefined;
    entries.push({
      id: entryId++,
      role: 'user',
      text: String(round.prompt || round.name || `Harness round ${i + 1}`),
      ts: now + i * 2,
      ...(agentJsonText ? { agentJsonText } : {}),
    });
    entries.push({
      id: entryId++,
      role: 'assistant',
      text: String(bundle?.communication?.main_agent?.final_text || ''),
      ts: now + i * 2 + 1,
      assistantKind: 'agent',
    });
  }

  const inputDir = join(runDir, 'session-report-input');
  const outputDir = join(runDir, 'session-reports');
  mkdirSync(inputDir, { recursive: true });
  const sessionId = `harness-${runId || 'run'}`;
  const sessionPath = join(inputDir, `session__${sessionId}.doc`);
  writeFileSync(sessionPath, JSON.stringify({
    id: sessionId,
    title: `${displayName} test run ${runId || ''}`.trim(),
    createdAt: now,
    updatedAt: Date.now(),
    entries,
  }, null, 2), 'utf8');

  const scriptPath = join(PACKAGE_ROOT, 'scripts', 'session-reports.mjs');
  const generated = spawnSync(process.execPath, [
    scriptPath,
    '--dir', inputDir,
    '--session', sessionId,
    '--format', 'all',
    '--out', outputDir,
  ], {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout: 120_000,
    maxBuffer: 16 << 20,
  });
  if ((generated.status ?? -1) !== 0) {
    throw new Error(
      `session report generator failed (${generated.status}): ` +
      `${generated.stderr || generated.stdout || 'no output'}`,
    );
  }
  return {
    inputDir,
    outputDir,
    sessionPath,
    stdout: generated.stdout || '',
  };
}

export function toolResultEnvelope(bundlePath, toolName) {
  const bundle = bundlePath ? readJson(bundlePath) : null;
  const events = bundle?.communication?.tools || [];
  const ev = events.find((e) => e.kind === 'tool_result' && e.tool === toolName);
  if (!ev) return null;
  let d = ev.detail;
  if (typeof d === 'string') { try { d = JSON.parse(d); } catch { d = null; } }
  if (!d) return null;
  return d.envelope ?? d;
}

export function toolCalls(bundlePath) {
  const bundle = bundlePath ? readJson(bundlePath) : null;
  const events = bundle?.communication?.tools || [];
  return events.filter((e) => e.kind === 'tool_call').map((e) => e.tool);
}

export function finalText(bundlePath) {
  const bundle = bundlePath ? readJson(bundlePath) : null;
  return bundle?.communication?.main_agent?.final_text || '';
}

export function systemPromptFromBundle(bundlePath) {
  const bundle = bundlePath ? readJson(bundlePath) : null;
  const messages = bundle?.communication?.main_agent?.messages;
  if (!Array.isArray(messages)) return '';
  const sys = messages.find((m) => m?.role === 'system');
  return typeof sys?.content === 'string' ? sys.content : '';
}

export function memoryUpdatesFromBundle(bundlePath) {
  const bundle = bundlePath ? readJson(bundlePath) : null;
  return Array.isArray(bundle?.communication?.memory_updates)
    ? bundle.communication.memory_updates
    : [];
}

/** Latest MemoryUpdate.recall telemetry payload from a round bundle, or null. */
export function recallTelemetryFromBundle(bundlePath) {
  const updates = memoryUpdatesFromBundle(bundlePath);
  for (let i = updates.length - 1; i >= 0; --i) {
    const recall = updates[i]?.recall;
    if (recall && typeof recall === 'object') return recall;
  }
  return null;
}

export function runAgentRound({
  paths,
  loopbackPresetId,
  exe,
  mode,
  preset,
  configDir,
  cwd,
  sessionId,
  prompt,
  script,
  runDir,
  roundKey,
  roundMeta = {},
  extraArgs = [],
  mcpEnabled,
  plannerEnabled = false,
  env = process.env,
  timeout,
}) {
  const logPath = join(runDir, `${roundKey}-agent.json`);
  const t0 = performance.now();
  const isLive = mode === 'live';
  const args = [
    ...DEFAULT_LOG_ARGS,
    ...(configDir ? ['--config-dir', configDir] : []),
    '--cwd', cwd,
    'llm', 'agent',
    '--json',
    plannerEnabled ? '--planner' : '--no-planner',
    // Live uses the installed MCP config. office-cli is an MCP server there.
    ...((mcpEnabled ?? isLive) ? [] : ['--no-mcp']),
    '--session-id', sessionId,
    '--prompt', prompt,
    '--log', logPath,
    ...extraArgs,
  ];

  const hasMaxIter = extraArgs.some((a, i) => a === '--max-iter' || String(a).startsWith('--max-iter='));
  const hasConsentUi = extraArgs.some((a, i) => a === '--consent-ui' || String(a).startsWith('--consent-ui='));
  if (isLive) {
    if (preset) args.splice(args.indexOf('agent') + 1, 0, '--preset', preset);
    args.push('--single-turn');
    // Headless live runs must not open the native consent dialog or write a
    // permanent grant. yolo allows each Ask with a session grant.
    if (!hasConsentUi) args.push('--consent-ui', 'yolo');
    if (!hasMaxIter) args.push('--max-iter', '6');
  } else {
    args.push('--router', 'loopback', '--single-turn');
    if (!hasMaxIter) args.push('--max-iter', '8');
  }

  const roundEnv = isLive
    ? env
    : { ...env, PM_LOOPBACK_SCRIPT: JSON.stringify(script ?? []) };

  const result = spawnSync(exe, prepareCliArgs(exe, args), {
    cwd: paths.packageRoot,
    encoding: 'utf8',
    timeout: timeout ?? (isLive
      ? (/^local/i.test(String(preset || '')) ? 300_000 : 180_000)
      : 60_000),
    maxBuffer: 16 << 20,
    env: roundEnv,
  });
  const ms = performance.now() - t0;
  const stdoutJson = parseJson(result.stdout);
  const logDoc = existsSync(logPath) ? readJson(logPath) : null;
  const json = (stdoutJson && Object.prototype.hasOwnProperty.call(stdoutJson, 'ok'))
    ? stdoutJson
    : (logDoc?.result || stdoutJson);
  const { communication, bundlePath } = writeCommunicationBundle(
    runDir,
    roundKey,
    {
      ...roundMeta,
      mode,
      prompt,
      sessionId,
      roundKey,
      preset: isLive ? preset : loopbackPresetId,
    },
    logPath,
    json,
  );
  return {
    status: result.status ?? -1,
    error: result.error ? String(result.error.message || result.error) : '',
    stdout: result.stdout || '',
    stderr: result.stderr || '',
    json,
    communication,
    bundlePath,
    logPath,
    args,
    ms,
  };
}

/** @param {AgentHarnessPaths} paths */
export function createAgentTestReport(paths) {
  return {
    meta: {},
    notes: [],
    suites: [],
    _cur: null,
    beginSuite(name) {
      if (this._cur) this.endSuite();
      this._cur = { name, steps: [], t0: performance.now() };
    },
    endSuite() {
      if (!this._cur) return;
      this._cur.totalMs = performance.now() - this._cur.t0;
      this.suites.push(this._cur);
      this._cur = null;
    },
    step(label, ms, detail = '') {
      if (!this._cur) return;
      this._cur.steps.push({ label, ms: Math.round(ms * 100) / 100, detail });
    },
    note(text) {
      this.notes.push(String(text));
    },
    finalize(stats, wallMs, exe, extra = {}) {
      if (this._cur) this.endSuite();
      this.meta = {
        generatedAt: new Date().toISOString(),
        node: process.version,
        argv: process.argv.slice(2),
        exe,
        harnessDir: paths.harnessDir,
        configDir: paths.configDir,
        sessionDir: paths.sessionDir,
        workDir: paths.workDir,
        passed: stats.passed,
        failed: stats.failed,
        wallClockMs: Math.round(wallMs * 100) / 100,
        ...this.meta,
        ...extra,
      };
    },
  };
}

function finiteNumber(value) {
  const n = Number(value);
  return Number.isFinite(n) ? n : null;
}

function usageCounts(usage) {
  const src = usage && typeof usage === 'object' ? usage : {};
  const prompt = finiteNumber(src.prompt_tokens) ?? finiteNumber(src.input_tokens) ?? 0;
  const completion = finiteNumber(src.completion_tokens) ?? finiteNumber(src.output_tokens) ?? 0;
  const total = finiteNumber(src.total_tokens) ?? (prompt + completion);
  return {
    prompt,
    completion,
    total,
    cached: finiteNumber(src.prompt_tokens_details?.cached_tokens),
    reasoning: finiteNumber(src.completion_tokens_details?.reasoning_tokens),
    cost: finiteNumber(src.cost),
  };
}

/** Per-agent-run token and cost totals, plus each llm_round the agent emitted. */
export function collectRunLlmUsage(runDir) {
  if (!runDir || !existsSync(runDir)) return [];
  const turns = [];
  for (const file of readdirSync(runDir).filter((name) => name.endsWith('-agent.json')).sort()) {
    let doc;
    try {
      doc = readJson(join(runDir, file));
    } catch {
      continue;
    }
    const usage = doc?.result?.llm_usage;
    if (!usage || typeof usage !== 'object') continue;
    const totals = usageCounts(usage);
    if (totals.total <= 0 && totals.cost == null) continue;
    const rounds = Array.isArray(usage.llm_rounds)
      ? usage.llm_rounds.map((round, index) => ({
        index: index + 1,
        model: round?.model || '',
        provider: round?.provider || '',
        duration_ms: finiteNumber(round?.duration_ms),
        ...usageCounts(round?.usage),
      }))
      : [];
    turns.push({
      label: file.replace(/-agent\.json$/, ''),
      ...totals,
      rounds,
    });
  }
  return turns;
}

export function writeAgentTestReportFile(rep, metricsCollector, startedAtIso, {
  runDir,
  displayName,
  testName,
  extraMeta = {},
} = {}) {
  const finishedAtIso = new Date().toISOString();
  const metrics = buildMetricsBundle(metricsCollector, startedAtIso, finishedAtIso);
  const llmUsage = collectRunLlmUsage(runDir);
  if (llmUsage.length) {
    const prompt = llmUsage.reduce((sum, turn) => sum + turn.prompt, 0);
    const completion = llmUsage.reduce((sum, turn) => sum + turn.completion, 0);
    const total = llmUsage.reduce((sum, turn) => sum + turn.total, 0);
    const cost = llmUsage.reduce((sum, turn) => sum + (turn.cost ?? 0), 0);
    const hasCost = llmUsage.some((turn) => turn.cost != null);
    console.log(`  tokens           : ${total} total (${prompt} in, ${completion} out)`);
    console.log(`  cost             : ${hasCost ? `$${cost.toFixed(6)}` : 'n/a'}`);
  }
  const failed = rep.meta.failed ?? 0;
  const ok = failed === 0 && !rep.meta.abortReason && !rep.meta.uncaughtError;

  let md = renderMarkdownReport({
    ok,
    passed: rep.meta.passed,
    failed,
    abortReason: rep.meta.abortReason,
    uncaughtError: rep.meta.uncaughtError,
    error: rep.meta.uncaughtError,
    meta: {
      cwd: process.cwd(),
      displayName,
      testName,
      writtenAt: finishedAtIso,
      generatedAt: rep.meta.generatedAt,
      exe: rep.meta.exe,
      argv: Array.isArray(rep.meta.argv) ? rep.meta.argv.join(' ') : String(rep.meta.argv ?? ''),
      wallClockMs: rep.meta.wallClockMs,
      harnessDir: rep.meta.harnessDir,
      configDir: rep.meta.configDir,
      sessionDir: rep.meta.sessionDir,
      runDir,
      ...extraMeta,
    },
    metrics,
    images: [],
    mediaSuites: rep.suites,
    integrationNotes: rep.notes,
    llmUsage,
  });

  const acc = isAccumulateMode() && getChapterTitle();
  md += acc
    ? '\n---\n\n*Per-run: `tests/test-report-last.md` - this run is also a chapter in `tests/test-report-all.md`.*\n'
    : '\n---\n\n*Artifact: `tests/test-report-last.md` - overwritten on each test run.*\n';
  writeFileSync(TEST_REPORT_PATH, md, 'utf8');
  if (acc) {
    appendMarkdownChapter(getChapterTitle(), md, { npmScript: getNpmScriptLabel() || undefined });
    console.log(`  (aggregated report chapter: ${TEST_REPORT_ALL_PATH})`);
  }
}

/**
 * Parse argv/env and resolve stemp paths for an agent harness.
 *
 * @param {{
 *   harnessSubdir?: string,
 *   loopbackPresetId?: string,
 *   displayName?: string,
 *   argv?: string[],
 * }} [options]
 * @returns {AgentHarnessOptions}
 */
export function createAgentHarnessOptions({
  harnessSubdir = 'agent-harness',
  loopbackPresetId = 'agent-harness-loopback',
  displayName = 'agent',
  argv = process.argv.slice(2),
} = {}) {
  const paths = resolveHarnessPaths(harnessSubdir);
  const onlyScopes = parseOnlyScopes(argv);
  return {
    paths,
    mode: parseHarnessMode(argv),
    preset: parsePreset(argv),
    loopbackPresetId,
    displayName,
    onlyScopes,
    requireLive: parseRequireLive(argv),
    memoryFeaturesMask: parseMemoryFeaturesMask(argv),
    wantSuite: createWantSuite(onlyScopes),
  };
}

/**
 * Bootstrap a harness run directory + agent env/context.
 *
 * @param {AgentHarnessOptions} harness
 * @param {{ exe: string, runId?: string, perRunWorkDir?: boolean }} params
 */
export function bootstrapAgentHarness(harness, {
  exe,
  runId = Date.now().toString(36),
  perRunWorkDir = false,
}) {
  const { paths, mode, memoryFeaturesMask } = harness;
  const runDir = join(paths.runsDir, runId);
  const sessionDir = join(paths.sessionDir, runId);
  const workDir = perRunWorkDir ? join(paths.workDir, runId) : paths.workDir;

  resetHarnessTree(paths, {
    keepAll: perRunWorkDir || process.env.PM_KEEP_AGENT_MEMORY_TMP === '1',
  });
  mkdirSync(runDir, { recursive: true });
  mkdirSync(sessionDir, { recursive: true });
  mkdirSync(workDir, { recursive: true });
  if (mode === 'loopback') {
    writeLoopbackHarnessConfig(paths.configDir, harness.loopbackPresetId);
  }

  const historyDir = join(runDir, 'history');
  const env = {
    ...process.env,
    PM_IMAGE_SESSION_DIR: sessionDir,
    PM_IMAGE_DEBUG_DUMPS: '1',
    ...(mode === 'loopback' ? { PM_CHANGE_HISTORY_DIR: historyDir } : {}),
    ...(memoryFeaturesMask ? { PM_LLM_STRUCTURED_MEMORY_FEATURES: memoryFeaturesMask } : {}),
  };

  const ctx = {
    paths: { ...paths, sessionDir },
    historyDir: mode === 'loopback' ? historyDir : '',
    loopbackPresetId: harness.loopbackPresetId,
    exe,
    mode: harness.mode,
    preset: harness.preset,
    configDir: mode === 'loopback' ? paths.configDir : '',
    cwd: workDir,
    runDir,
    runId,
    env,
  };

  return { runDir, runId, sessionDir, workDir, env, ctx };
}

export function printAgentHarnessBanner(harness, { exe, runDir, sessionDir = harness.paths.sessionDir }) {
  const { paths, mode, preset, memoryFeaturesMask, onlyScopes } = harness;
  console.log(`\nAgent harness (${harness.displayName}, ${mode})\n`);
  console.log(`   exe        : ${exe}`);
  console.log(`   mode       : ${mode}`);
  if (mode === 'live') console.log(`   preset     : ${preset || '(user default)'}`);
  if (mode === 'loopback') console.log(`   config-dir : ${paths.configDir}`);
  else console.log(`   config-dir : (user install — presets/credentials)`);
  console.log(`   sessions   : ${sessionDir}`);
  console.log(`   work       : ${paths.workDir}`);
  console.log(`   run dir    : ${runDir}`);
  if (memoryFeaturesMask) console.log(`   features   : PM_LLM_STRUCTURED_MEMORY_FEATURES=${memoryFeaturesMask}`);
  if (onlyScopes) console.log(`   only       : ${[...onlyScopes].join(', ')}`);
}

/**
 * Run live suites when API key is present; skip or throw otherwise.
 *
 * @param {AgentHarnessOptions} harness
 * @param {{ exe: string, ctx: object, env: object, rep: object, runSuites: () => void }} params
 */
export function runHarnessSuites(harness, { exe, ctx, env, rep, runSuites }) {
  if (harness.mode === 'live') {
    const hasKey = llmAgentHasConfiguredKey(exe, {
      paths: harness.paths,
      mode: harness.mode,
      preset: harness.preset,
      cwd: harness.paths.workDir,
      env,
    });
    if (!hasKey) {
      const msg = `live mode requires a configured provider for preset "${harness.preset}"`;
      if (harness.requireLive) throw new Error(msg);
      console.log(`\n  [skip] ${msg}\n`);
      rep.note(`skipped: ${msg}`);
      return;
    }
  }
  runSuites(ctx);
}

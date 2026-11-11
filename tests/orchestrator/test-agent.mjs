#!/usr/bin/env node
import { llmExe, prepareCliArgs } from './exe.mjs';
import { REPO as PACKAGE_ROOT } from './repo.mjs';
/**
 * Live agent memory harness.
 *
 * Invoked via:
 *   npm run test:agent:memory
 *   npm run test:agent:smoke          # cherry-picked live cases + --streaming on
 *   npm run test:agent:smoke:followup # multi-tool file-write flow (heavier)
 *
 * `--streaming-smoke` forces `--streaming on` and asserts no duplicate tool_call /
 * assistant_text replay in the agent --log JSON (catches recent SSE dedupe bugs).
 *
 * This is intentionally diagnostic rather than a strict product test. It runs a
 * few hot agent turns with one session id and writes the full planner/main-agent
 * communication surfaces to ./stemp/agent-memory.
 */

import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { cliExePath, brand, brandK } from '@polymech/tanit/tests/orchestrator/test-commons.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const STEMP_DIR = join(PACKAGE_ROOT, 'stemp');
const RUN_DIR = join(STEMP_DIR, 'agent-memory');
const SESSION_DIR = join(RUN_DIR, 'sessions');
// 'warn' keeps stderr clean; full trace still lands in the --log JSON file.
const DEFAULT_LOG_ARGS = ['--log-level', 'warn'];
const DEFAULT_CWD_ROOT = './stemp';
const DEFAULT_CHAT_PRESET = 'quick';
const CHILD_AGENT_PRESET = 'superfast';
const STREAMING_SMOKE = process.argv.includes('--streaming-smoke');
// Live planner prompt used case-wide. Iterating on this file does NOT require
// a rebuild: tanit-cli reads it through `--planner-prompt @<path>` and runs
// it through `${...}` template substitution (e.g. `${builtin}` to extend
// rather than fully replace the baked-in default). A case can override via
// `options.plannerPrompt` (literal text, `@path`, `file:path`, or
// `null`/`none` to fall back to the binary default).
//
// Resolution order (first existing file wins):
//   1. stemp/prompts/planner.md   — local dev override (iterate without rebuild)
//   2. %APPDATA%/PolyMech/pm-image/prompts/planner-prompt.md  — user's installed prompt
//   3. (nothing)                  — binary default baked into tanit-cli.exe
const DEFAULT_PLANNER_PROMPT_FILE = join(STEMP_DIR, 'prompts', 'planner.md');
const ROAMING_PLANNER_PROMPT_FILE = process.env.APPDATA
  ? join(process.env.APPDATA, brandK.k_config_subpath_u8, 'prompts', 'planner-prompt.md')
  : null;

// ── Pre-API context reduction (attention reduction layer) test knobs ─────────
// The case-chat verifier exercises the `--context-reduction` CLI surface with a
// deterministic dry-run (resolves provider/tools/selection only — no LLM call,
// no API key, no spend). Default ON. The live chat recall rounds always run
// with reduction Off so their wire payload stays byte-stable; these knobs only
// drive the opt-in wiring probe.
//   CONTEXT_REDUCTION_CHECK     run the wiring assertions at all (false = skip)
//   CONTEXT_REDUCTION_MODE      off | none | tool-results | tools | all
//   CONTEXT_REDUCTION_RATIO     target output/input byte ratio (0..1)
//   CONTEXT_REDUCTION_MIN_BYTES min message size (bytes) before reduction
//   CONTEXT_REDUCTION_OBJECTIVE relevance objective for text reduction
const CONTEXT_REDUCTION_CHECK = true;
const CONTEXT_REDUCTION_MODE = 'tool-results';
const CONTEXT_REDUCTION_RATIO = 0.4;
const CONTEXT_REDUCTION_MIN_BYTES = 4096;
const CONTEXT_REDUCTION_OBJECTIVE = 'keep the dog facts';

function truncateText(value, max = 900) {
  const text = String(value ?? '');
  return text.length <= max ? text : `${text.slice(0, max)}...`;
}

function runCli(exe, args, { timeout = 120_000, env = process.env } = {}) {
  const started = performance.now();
  console.log(`\n$ ${JSON.stringify(exe)} ${args.map((arg) => JSON.stringify(arg)).join(' ')}`);
  const result = spawnSync(exe, prepareCliArgs(exe, args), {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout,
    env,
  });
  const elapsedMs = Math.round((performance.now() - started) * 100) / 100;
  if (result.stdout) {
    console.log('--- stdout ---');
    console.log(result.stdout.trimEnd());
  }
  if (result.stderr) {
    console.log('--- stderr ---');
    console.error(result.stderr.trimEnd());
  }
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

function readJson(path) {
  return JSON.parse(readFileSync(path, 'utf8'));
}

function llmAgentDryRunHasConfiguredKey(exe, env) {
  const logPath = join(RUN_DIR, 'dry-run.json');
  const result = runCli(
    exe,
    [
      ...DEFAULT_LOG_ARGS,
      '--cwd', DEFAULT_CWD_ROOT,
      'llm', 'agent',
      '--prompt', 'ping',
      '--single-turn',
      '--dry-run',
      '--log', logPath,
    ],
    { timeout: 30_000, env },
  );
  if (existsSync(logPath)) {
    try {
      return readJson(logPath)?.provider?.api_key_configured === true;
    } catch {
      // Fall through to stdout/stderr heuristic.
    }
  }
  const combined = `${result.stdout}\n${result.stderr}`;
  if (/\|\s*api_key\s*\|\s*set\s*\|/i.test(combined)) return true;
  if (/\|\s*api_key\s*\|\s*\*\*missing\*\*\s*\|/i.test(combined)) return false;
  return false;
}

// Does the built CLI expose the `--context-reduction` surface? It is guarded by
// FEATURE_LLM_COMPRESSION, so a build without the attention reduction layer
// simply won't register the flag. Probed via --help so the wiring test can
// degrade gracefully (off-mode default is still asserted unconditionally).
function llmAgentHasContextReductionFlag(exe, env = process.env) {
  const help = runCli(exe, [...DEFAULT_LOG_ARGS, 'llm', 'agent', '--help'],
    { timeout: 30_000, env });
  return /--context-reduction\b/.test(`${help.stdout}\n${help.stderr}`);
}

// Run `llm agent --dry-run` and return the parsed `turn.context_reduction` block
// from the --log JSON. Deterministic: resolves provider/tools/selection only —
// no LLM round-trip, no API key required. Used to assert the pre-API context
// reduction CLI surface lands on the Turn.
function dryRunContextReduction(exe, cwd, extraArgs, env = process.env) {
  const logPath = join(RUN_DIR, `dry-context-reduction-${Date.now().toString(36)}.json`);
  const result = runCli(
    exe,
    [
      ...DEFAULT_LOG_ARGS,
      '--cwd', cwd,
      'llm', 'agent',
      '--prompt', 'ping',
      '--single-turn',
      '--dry-run',
      ...extraArgs,
      '--log', logPath,
    ],
    { timeout: 30_000, env },
  );
  let doc = null;
  if (existsSync(logPath)) {
    try { doc = readJson(logPath); } catch { doc = null; }
  }
  return {
    status: result.status,
    stderr: result.stderr,
    contextReduction: doc?.turn?.context_reduction || null,
  };
}

function caseCwdArg(testCase) {
  return testCase.cwdOffset ? caseAbsDir(testCase) : DEFAULT_CWD_ROOT;
}

function caseAbsDir(testCase) {
  return testCase.cwdOffset ? join(STEMP_DIR, testCase.cwdOffset) : STEMP_DIR;
}

function extractCommunication(logDoc, stdoutJson) {
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
    clarification_events: events
      .filter((event) => event.kind === 'clarification_needed')
      .map((event) => event.detail || {}),
    error_reflexion_events: events
      .filter((event) => event.kind === 'error_reflexion')
      .map((event) => event.detail || {}),
    self_check_events: events
      .filter((event) => event.kind === 'self_check')
      .map((event) => event.detail || {}),
    memory_updates: events
      .filter((event) => event.kind === 'memory_update')
      .map((event) => event.detail),
    llm_rounds: events
      .filter((event) => event.kind === 'llm_round')
      .map((event) => event.detail),
  };
}

function numberValue(value) {
  return typeof value === 'number' && Number.isFinite(value) ? value : 0;
}

function usageMetrics(usage) {
  const outer = usage && typeof usage === 'object' ? usage : {};
  const safe = outer.usage && typeof outer.usage === 'object' ? outer.usage : outer;
  const rounds = Array.isArray(safe.llm_rounds) ? safe.llm_rounds : [];
  const promptTokens = numberValue(safe.prompt_tokens) || numberValue(safe.input_tokens);
  const completionTokens = numberValue(safe.completion_tokens) || numberValue(safe.output_tokens);
  return {
    prompt_tokens: promptTokens,
    completion_tokens: completionTokens,
    total_tokens: numberValue(safe.total_tokens) || promptTokens + completionTokens,
    cost_usd: numberValue(safe.cost),
    llm_rounds: rounds.length,
  };
}

function addUsage(a, b) {
  return {
    prompt_tokens: a.prompt_tokens + b.prompt_tokens,
    completion_tokens: a.completion_tokens + b.completion_tokens,
    total_tokens: a.total_tokens + b.total_tokens,
    cost_usd: Math.round((a.cost_usd + b.cost_usd) * 1_000_000_000) / 1_000_000_000,
    llm_rounds: a.llm_rounds + b.llm_rounds,
  };
}

function roundMetrics(result, communication) {
  const plannerDurationMs = numberValue(communication.planner.meta?.duration_ms);
  const wallMs = numberValue(result.elapsedMs);
  const plannerUsage = usageMetrics(communication.planner.meta?.usage);
  const mainUsage = usageMetrics(communication.main_agent.llm_usage);
  const totalUsage = addUsage(plannerUsage, mainUsage);
  return {
    duration_ms: {
      wall: wallMs,
      planner: plannerDurationMs,
      main_estimate: Math.max(0, Math.round((wallMs - plannerDurationMs) * 100) / 100),
    },
    usage: {
      planner: plannerUsage,
      main_agent: mainUsage,
      total: totalUsage,
    },
  };
}

function totalMetrics(rounds) {
  const emptyUsage = {
    prompt_tokens: 0,
    completion_tokens: 0,
    total_tokens: 0,
    cost_usd: 0,
    llm_rounds: 0,
  };
  const totals = {
    duration_ms: {
      wall: 0,
      planner: 0,
      main_estimate: 0,
    },
    usage: {
      planner: { ...emptyUsage },
      main_agent: { ...emptyUsage },
      total: { ...emptyUsage },
    },
  };
  for (const round of rounds) {
    const metrics = round.metrics || {};
    totals.duration_ms.wall += numberValue(metrics.duration_ms?.wall);
    totals.duration_ms.planner += numberValue(metrics.duration_ms?.planner);
    totals.duration_ms.main_estimate += numberValue(metrics.duration_ms?.main_estimate);
    totals.usage.planner = addUsage(totals.usage.planner, metrics.usage?.planner || emptyUsage);
    totals.usage.main_agent = addUsage(totals.usage.main_agent, metrics.usage?.main_agent || emptyUsage);
    totals.usage.total = addUsage(totals.usage.total, metrics.usage?.total || emptyUsage);
  }
  for (const key of Object.keys(totals.duration_ms)) {
    totals.duration_ms[key] = Math.round(totals.duration_ms[key] * 100) / 100;
  }
  return totals;
}

function writeCommunicationBundle(round, logPath, stdoutJson) {
  const logDoc = existsSync(logPath) ? readJson(logPath) : {};
  const communication = extractCommunication(logDoc, stdoutJson);
  const prefix = round.caseId ? `${round.caseId}-` : '';
  const bundlePath = join(RUN_DIR, `${prefix}round-${String(round.index).padStart(2, '0')}-communication.json`);
  writeFileSync(bundlePath, JSON.stringify({
    round,
    log_path: logPath,
    communication,
  }, null, 2), 'utf8');
  return { logDoc, communication, bundlePath };
}

function assertHarness(condition, message) {
  if (!condition) throw new Error(message);
}

function printRoundSummary(round, result, communication, bundlePath) {
  const metrics = roundMetrics(result, communication);
  console.log(`\nround ${round.index}: exit=${result.status} ms=${result.elapsedMs}`);
  console.log(`  planner messages : ${communication.planner.messages.length}`);
  console.log(`  main messages    : ${communication.main_agent.messages.length}`);
  console.log(`  tool events      : ${communication.tools.length}`);
  console.log(`  tokens           : ${metrics.usage.total.total_tokens} total (${metrics.usage.total.prompt_tokens} in, ${metrics.usage.total.completion_tokens} out)`);
  console.log(`  cost             : $${metrics.usage.total.cost_usd.toFixed(6)}`);
  console.log(`  final            : ${truncateText(communication.main_agent.final_text, 220)}`);
  console.log(`  bundle           : ${bundlePath}`);
}

const CASES = {
  'case-mixed': {
    id: 'case-mixed',
    cwdOffset: 'case-mixed',
    goal:
      'Interleave a chat fact (project codename) with two tool-driven turns (JSON listing, ' +
      'Markdown summary) and a recall turn. Validates that structured memory carries both ' +
      'kinds of context across an entire mixed session.',
    options: {
      chatPreset: DEFAULT_CHAT_PRESET,
      agent: ['--multi-turn'],
      plannerBudget: '6',
      maxIter: '8',
    },
    fixtures: {
      files: [
        { name: 'falcon-01.jpg', content: '' },
        { name: 'falcon-02.jpg', content: '' },
        { name: 'falcon-03.jpg', content: '' },
      ],
    },
    expected: {
      jsonFile: 'falcon-listing.json',
      markdownFile: 'falcon-summary.md',
    },
    rounds: [
      {
        index: 1,
        name: 'introduce codename',
        prompt:
          "Hi! My project codename for this session is Falcon. Please acknowledge in one short sentence " +
          "and remember the name.",
      },
      {
        index: 2,
        name: 'tool: jpg listing json',
        prompt:
          'In the current folder, find all files matching *.jpg. Create a JSON directory listing file named ' +
          'falcon-listing.json in this same folder. The JSON must be an object with keys cwd, pattern, count, ' +
          'and files. files must be an array of objects with at least name and path. After writing the file, ' +
          'reply exactly: JSON_LISTING_WRITTEN falcon-listing.json',
      },
      {
        index: 3,
        name: 'chat: recall codename',
        prompt:
          "What is my project codename for this session? Answer in one short sentence; do not ask me to repeat it.",
      },
      {
        index: 4,
        name: 'tool: markdown summary with codename',
        prompt:
          'Using the JSON file you created in the previous tool turn, read that JSON file and write a Markdown ' +
          'summary file named falcon-summary.md in the same folder. The summary must start with a heading that ' +
          'contains the project codename, then list the total file count, then one bullet per JPG (filename only). ' +
          'Do not ask me for the JSON path or the codename; use the items remembered from prior turns. ' +
          'After writing the file, reply exactly: MARKDOWN_SUMMARY_WRITTEN falcon-summary.md',
      },
    ],
  },
  'case-chat': {
    id: 'case-chat',
    cwdOffset: 'case-chat',
    goal:
      'Carry conversational facts across three pure-chat turns. Validates that structured ' +
      'memory (kind=conversation/role=turn_pair items) preserves user-supplied facts so the ' +
      'agent can answer follow-ups about them in later rounds.',
    options: {
      chatPreset: DEFAULT_CHAT_PRESET,
      agent: ['--multi-turn'],
      plannerBudget: '4',
      maxIter: '4',
    },
    rounds: [
      {
        index: 1,
        name: 'introduce facts',
        prompt:
          "My dog's name is Bruno. He's a 3-year-old chocolate Labrador and he absolutely loves swimming. " +
          'Please acknowledge in one short sentence.',
      },
      {
        index: 2,
        name: 'add fact',
        prompt:
          "One more thing about Bruno: his favourite toy is a yellow rubber duck named Quackers. " +
          'Acknowledge briefly.',
      },
      {
        index: 3,
        name: 'recall facts',
        prompt:
          "What is my dog's name, breed, and the name of his favourite toy? Answer in one short sentence; " +
          "do not ask me to repeat the facts.",
      },
    ],
  },
  'case-exact-turn-export': {
    id: 'case-exact-turn-export',
    cwdOffset: 'case-exact-turn-export',
    goal:
      'Without transcript replay or automatic artifact promotion, recover a long chat-only ' +
      'assistant response from its canonical agent log and export it on a terse follow-up.',
    options: {
      chatPreset: DEFAULT_CHAT_PRESET,
      agent: ['--multi-turn'],
      plannerBudget: '5',
      maxIter: '8',
    },
    expected: {
      markdownFile: 'prior-report.md',
      markdownContains: ['EXACT_EXPORT_HEAD_91C4', 'EXACT_EXPORT_TAIL_91C4'],
    },
    rounds: [
      {
        index: 1,
        name: 'assemble long chat-only report',
        prompt:
          'Write a detailed Markdown report in chat only. Do not call tools and do not create a file. ' +
          'The response must begin with EXACT_EXPORT_HEAD_91C4, end with EXACT_EXPORT_TAIL_91C4, ' +
          'and contain at least 6,000 characters between those markers.',
      },
      {
        index: 2,
        name: 'terse exact export',
        prompt: 'write as md file named prior-report.md',
      },
    ],
  },
  'case-followup': {
    id: 'case-followup',
    cwdOffset: 'case-followup',
    goal:
      'Replay the chat-UI smoke flow that surfaced session-id-per-turn bugs in ChatWebPanel: ' +
      'a terse multi-turn conversation (`list jpeg files` → `as json file` → `now as markdown report`) ' +
      'where every prompt after the first is an implicit reference to the previous output. ' +
      'Validates that structured memory carries the directory listing forward and that the agent ' +
      'resolves "as json file" / "now as markdown report" against session items rather than re-globbing ' +
      'or asking for clarifications.',
    options: {
      chatPreset: DEFAULT_CHAT_PRESET,
      agent: ['--multi-turn'],
      plannerBudget: '6',
      maxIter: '8',
    },
    fixtures: {
      files: [
        { name: 'DSC05575.jpg',       content: '' },
        { name: 'flux-5_resized.jpg', content: '' },
      ],
    },
    rounds: [
      {
        index: 1,
        name: 'chat: list jpeg files',
        prompt:
          'List the JPEG files in the current folder. Reply with a short Markdown table containing ' +
          'the filename and an approximate size; do not write any files yet.',
      },
      {
        index: 2,
        name: 'terse followup: now as dir.json',
        prompt:
          'now as dir.json',
      },
      {
        index: 3,
        name: 'terse followup: now as markdown report.md',
        prompt:
          'now as markdown report.md',
      },
      {
        index: 4,
        name: 'same-stem collision: also as report.txt',
        prompt:
          'also as report.txt',
      },
    ],
  },
  'case-commands': {
    id: 'case-commands',
    cwdOffset: 'case-commands',
    goal:
      'Planner tuning: a broad, human-like prompt that maps to a registered local custom command ' +
      `(seen via the compact ${brand.BRAND_APP_DISPLAY} index — e.g. custom.mic-status, custom.video-status) must be ` +
      `routed through the \`run\` tool with a \`${brand.APP_ID_SLUG}-cli custom.<id>\` invocation. Validates that ` +
      'the planner picks `run`, that the agent invokes the exact `custom.<id>` from the catalog ' +
      '(no hallucinated leaf names), that a terse follow-up resolves a parallel custom command ' +
      'from session memory, and that a multi-step "do X for N seconds then stop" prompt chains ' +
      '`custom.<id>-start` → wait → `custom.<id>-stop` correctly via whatever sequencing strategy ' +
      'the agent picks (in-shell delay, scheduler+memory, etc.).',
    options: {
      chatPreset: DEFAULT_CHAT_PRESET,
      agent: ['--multi-turn'],
      plannerBudget: '4',
      maxIter: '8',
    },
    rounds: [
      {
        index: 1,
        name: 'broad ask: mic status',
        prompt: 'is the mic recording?',
      },
      {
        index: 2,
        name: 'terse follow-up: and the video?',
        prompt: 'and the video?',
      },
      {
        index: 3,
        name: 'mic record 5s then stop',
        prompt: 'record me 5 sec of audio then stop',
      },
      {
        index: 4,
        name: 'mic + video parallel for 5s',
        prompt: 'do the same but with video on top, both at the same time for 5 sec',
      },
    ],
  },
  'case-agent-cli': {
    id: 'case-agent-cli',
    cwdOffset: 'case-agent-cli',
    goal:
      'Planner + CLI routing for sub-agent dispatch: a root agent must spawn a ' +
      'child PM-Image agent via `' + brandK.k_exe_basename_cli_u8 + ' --log-level error llm agent --preset superfast --single-turn --prompt …` ' +
      'and scrape stdout. R1 covers a single trivial arithmetic child. ' +
      'R2 covers parallel spawn of two arithmetic children via a single backgrounded run command (prefer ' +
      'Start-Process -PassThru with redirected stdout — anything that actually runs the two ' +
      'child processes concurrently).',
    options: {
      chatPreset: DEFAULT_CHAT_PRESET,
      agent: ['--type', 'completion', '--multi-turn', '--disable-tools=file_read,file_glob,file_search'],
      plannerBudget: '3',
      maxIter: '5',
    },
    rounds: [
      {
        index: 1,
        name: 'spawn one sub-agent: arithmetic',
        prompt:
          `use ${brandK.k_exe_basename_cli_u8} as a sub-agent with this exact command shape: ` +
          `${brandK.k_exe_basename_cli_u8} --log-level error llm agent --preset ${CHILD_AGENT_PRESET} --single-turn --prompt "<child prompt>". ` +
          'Ask the child: "What is 1+1? Reply with only the number." ' +
          'Do not use info_lookup, do not use llm.agent, do not use --input, and do not wrap the command in powershell -Command. ' +
          'Return the child answer in your final reply.',
      },
      {
        index: 2,
        name: 'spawn two sub-agents in parallel',
        prompt:
          `now spawn two ${brandK.k_exe_basename_cli_u8} llm agent children with this exact command shape: ` +
          `${brandK.k_exe_basename_cli_u8} --log-level error llm agent --preset ${CHILD_AGENT_PRESET} --single-turn --prompt "<child prompt>". ` +
          'Run them concurrently ' +
          '(not one after the other) inside a single run call. ' +
          'child A answers: "What is 1+1? Reply with only the number." ' +
          'child B answers: "What is 2+2? Reply with only the number." ' +
          'Do not use info_lookup, do not use llm.agent, do not use --input, and do not prefix the run command with powershell -Command because the run tool already executes in a shell. ' +
          'Inside ONE run command, use PowerShell Start-Process -PassThru for each child with -RedirectStandardOutput to separate files, then Wait-Process for both pids, then Get-Content the two output files. ' +
          'Return both child answers in your final reply.',
      },
    ],
  },
  'case-app-use': {
    id: 'case-app-use',
    cwdOffset: 'case-app-use',
    goal:
      'Agent desktop-app routing: a natural app-use request should be handled with first-class ' +
      'app_* tools, not shell/file shortcuts. The agent opens MS Paint, selects red, draws a ' +
      'visible cross on the canvas, and captures before/after screenshots.',
    options: {
      chatPreset: DEFAULT_CHAT_PRESET,
      agent: ['--type', 'completion', '--single-turn', '--disable-tools=run,write_file,file_read,file_glob,file_search'],
      plannerBudget: '5',
      maxIter: '100',
    },
    cleanFiles: [
      'paint-cross-before.jpg',
      'paint-cross-after.jpg',
    ],
    rounds: [
      {
        index: 1,
        name: 'open Paint and draw red cross',
        prompt:
          'Open MS Paint, choose a red brush or pencil, draw a large red cross on the canvas, ' +
          'save a before screenshot as paint-cross-before.jpg and an after screenshot as paint-cross-after.jpg, ' +
          'then reply exactly: DONE. Use desktop app tools; do not use shell or file tools.',
      },
    ],
  },
  // case-ask-user: end-to-end exercise of the `ask_user` agent tool across
  // every supported `kind` (text / multi_choice / confirm). The CLI surface
  // normally renders an FTXUI modal; here the harness pre-seeds answers via
  // PM_ASK_USER_REPLY_FILE so the run is headless and deterministic. Cursor
  // persists across rounds (one reply per round, in declaration order).
  'case-ask-user': {
    id: 'case-ask-user',
    cwdOffset: 'case-ask-user',
    goal:
      'Round-trip the `ask_user` tool across all three primary kinds (text, ' +
      'multi_choice, confirm) using the PM_ASK_USER_REPLY_FILE backend so ' +
      'the run is fully headless. Validates the tool is callable from the ' +
      'planner, that arguments are forwarded intact, that the envelope ' +
      'shapes per kind match the schema (answer + answer_index / answer_indices), ' +
      'and that the agent can reflect the answer back to the user.',
    options: {
      chatPreset: 'superfast', // gpt-4o-mini: cheap + deterministic enough
      // No planner work needed — superfast already disables it. We force the
      // tool catalog open and ask in plain English; the agent must pick the
      // right kind from the prompt and the FAQ context.
      agent: ['--single-turn'],
      plannerBudget: '4',
      maxIter: '5',
    },
    // Materialised to <caseDir>/replies.json before the case runs. Cursor
    // advances on each consumed reply so rounds map 1-to-1 with entries.
    replyFile: {
      cursor: 0,
      replies: [
        { answer: 'blueberry' },                          // R1: text
        { answer: ['apple', 'orange'] },                  // R2: multi_choice
        { answer: true },                                 // R3: confirm
      ],
    },
    rounds: [
      {
        index: 1,
        name: 'text input',
        prompt:
          "Call the ask_user tool with kind=\"text\" and question=\"Which fruit do you like best?\". " +
          "When you receive the answer, reply with exactly one word: the answer (no punctuation, no extra words).",
      },
      {
        index: 2,
        name: 'multi_choice',
        prompt:
          "Call the ask_user tool with kind=\"multi_choice\", question=\"Pick the fruits you want in the salad\", " +
          "and options=[\"apple\",\"banana\",\"orange\"]. " +
          "When you receive the answer, reply with exactly the chosen labels joined by commas in their original " +
          "options-array order. No spaces around commas, no extra words. Example: apple,orange",
      },
      {
        index: 3,
        name: 'confirm',
        prompt:
          "Call the ask_user tool with kind=\"confirm\" and question=\"Ship the build?\". " +
          "When you receive the answer, reply with exactly one word: 'yes' if the answer is true, 'no' otherwise.",
      },
    ],
  },
  // ── P1 smoke test ──────────────────────────────────────────────────────────
  // Validates that the planner emits intent/action/subject fields and that
  // build_system_prompt injects ## Turn Context into the main agent's system
  // prompt. Three rounds exercise query → execute → followup classification.
  'case-planner-context': {
    id: 'case-planner-context',
    cwdOffset: 'case-planner-context',
    goal:
      'Verify P1 (planner context packet): planner must emit intent/action/subject fields ' +
      'in its JSON output (no fallback), and the main-agent system prompt must contain a ' +
      '## Turn Context block with those fields. ' +
      'R1=query intent (info only), R2=execute intent (mic status), ' +
      'R3=followup intent (terse subject swap to video).',
    options: {
      chatPreset: DEFAULT_CHAT_PRESET,
      agent: ['--multi-turn'],
      plannerBudget: '5',
      maxIter: '6',
    },
    rounds: [
      {
        index: 1,
        name: 'query: what file tools are available',
        prompt:
          `What ${brand.APP_ID_SLUG}-cli tools are available for reading and writing files? ` +
          'List them briefly; do not call any tools.',
      },
      {
        index: 2,
        name: 'execute: mic status check',
        prompt: 'is the mic recording?',
      },
      {
        index: 3,
        name: 'followup: subject swap to video',
        prompt: 'and the video?',
      },
    ],
  },
  // ── P6 smoke test ──────────────────────────────────────────────────────────
  // Asks the agent to read two independent files in one shot. Both reads must
  // succeed and content must appear in the final reply. If the model batches
  // both tool calls into one response (parallel_tool_calls), the parallel burst
  // pattern (all tool_call events before any tool_result within that round) is
  // verified. Test passes regardless of whether the model actually batches —
  // parallel dispatch is transparent when it falls back to N=1 per round.
  'case-parallel-tools': {
    id: 'case-parallel-tools',
    cwdOffset: 'case-parallel-tools',
    goal:
      'P6 parallel tool dispatch: agent reads two independent fixture files. ' +
      'Both reads must succeed; content must appear in the final reply. ' +
      'If the model batches both tool calls into a single response the event ' +
      'stream must show all tool_calls before any tool_result (burst pattern).',
    options: {
      chatPreset: 'superfast',  // gpt-4o-mini: cheap + often batches parallel calls
      agent: ['--single-turn'],
      plannerBudget: '4',
      maxIter: '5',
    },
    fixtures: {
      files: [
        { name: 'alpha.txt', content: 'ALPHA_CONTENT_XJ7', overwrite: true },
        { name: 'beta.txt',  content: 'BETA_CONTENT_QZ3',  overwrite: true },
      ],
    },
    rounds: [
      {
        index: 1,
        name: 'parallel reads: alpha.txt + beta.txt simultaneously',
        prompt:
          'Read BOTH files in the current folder at the same time - issue both file_read calls ' +
          'in a single response, NOT one after the other: alpha.txt and beta.txt. ' +
          'After reading both, reply with exactly: ' +
          '"A: <content of alpha.txt> | B: <content of beta.txt>".',
      },
    ],
  },
  'case-dir': {
    id: 'case-dir',
    cwdOffset: 'case-dir',
    goal: 'Carry a generated JSON listing path across turns, then use it to create Markdown.',
    options: {
      chatPreset: DEFAULT_CHAT_PRESET,
      agent: ['--multi-turn'],
      plannerBudget: '6',
      maxIter: '8',
    },
    expected: {
      jsonFile: 'jpg-listing.json',
      markdownFile: 'jpg-listing.md',
    },
    rounds: [
      {
        index: 1,
        name: 'jpg listing json',
        prompt:
          'In the current folder, find all files matching *.jpg. Create a JSON directory listing file named ' +
          'jpg-listing.json in this same folder. The JSON must be an object with keys cwd, pattern, count, ' +
          'and files. files must be an array of objects with at least name and path. After writing the file, ' +
          'reply exactly: JSON_LISTING_WRITTEN jpg-listing.json',
      },
      {
        index: 2,
        name: 'jpg listing markdown from remembered json path',
        prompt:
          'Using the JSON file you created in the previous round, read that JSON file and create a Markdown ' +
          'directory listing file named jpg-listing.md in the same folder. Include a heading, total count, ' +
          'and one bullet per JPG. Do not ask me for the JSON path; use the path from the prior turn. ' +
          'After writing the file, reply exactly: MARKDOWN_LISTING_WRITTEN jpg-listing.md',
      },
    ],
  },

  // ── P9: SelfCheckFeature ────────────────────────────────────────────────────
  // A single-turn run with a simple file-write. After the turn the agent must
  // emit a self_check event (SelfCheckResult). The check should pass (passed=true)
  // because the tool call succeeded and there is no deflection in the reply.
  'case-self-check': {
    id: 'case-self-check',
    cwdOffset: 'case-self-check',
    goal:
      'P9 SelfCheckFeature: the agent writes a small file and finishes normally. ' +
      'The event log must contain a self_check event with passed=true and ' +
      'deflection_detected=false, confirming the quality gate fired.',
    options: {
      chatPreset: 'superfast',
      agent: ['--single-turn'],
      plannerBudget: '4',
      maxIter: '5',
    },
    rounds: [
      {
        index: 1,
        name: 'write fixture; expect self_check passed',
        prompt:
          'Create a file called check-result.txt in the current folder with content "SELF_CHECK_PASS". ' +
          'After writing the file reply: DONE_CHECK',
      },
    ],
  },

  // ── P12: Named session snapshots ───────────────────────────────────────────
  // Round 1 builds session memory by writing a file and saving a snapshot.
  // Round 2 loads that snapshot in a fresh single-turn invocation; it must be
  // able to recall the snapshot metadata (saved_at field) injected into context.
  // ── run_sequence: timed camera burst ──────────────────────────────────────
  // Validates that the planner selects `run_sequence` (not bare `run` with
  // sleep loops, not child agents) for a timed multi-shot camera task, and
  // that the agent follows up with `image_transform` calls.
  //
  // Keep the interval intentionally short (2 s) so the test completes quickly.
  // Camera failure is tolerated — what matters is that the agent *planned* the
  // right tool, not that hardware was present.
  'case-timed-capture': {
    id: 'case-timed-capture',
    cwdOffset: 'case-timed-capture',
    goal:
      'run_sequence sequencer: agent must plan a timed camera burst PLUS pencil-sketch ' +
      'transform as one end-to-end sequence (shell + wait steps). ' +
      'Must NOT spawn child agents via run or split transform into a separate round. ' +
      'Validates the sequencer path introduced to replace multi-round LLM loops.',
    // Remove prior outputs so we never falsely pass on a stale sketch.
    cleanFiles: [
      'cam_shot_001.jpg', 'cam_shot_002.jpg',
      'cam_shot_001_sketch.jpg', 'cam_shot_002_sketch.jpg',
    ],
    options: {
      chatPreset: 'quick',
      agent: ['--single-turn'],
      plannerBudget: '5',
      maxIter: '8',
    },
    rounds: [
      {
        index: 1,
        name: 'timed burst via run_sequence: 2 shots 2 s apart -> pencil sketch',
        prompt:
          'Use the run_sequence tool to do this whole job in ONE sequence (no separate rounds): ' +
          'take 2 photos from the camera exactly 2 seconds apart, saved as cam_shot_001.jpg and ' +
          'cam_shot_002.jpg, then convert each to a pencil-sketch style image saved as ' +
          'cam_shot_001_sketch.jpg and cam_shot_002_sketch.jpg, all in the current folder. ' +
          `Camera capture command: ${brand.APP_ID_SLUG} video image --dst <path>. ` +
          `Pencil sketch command: ${brand.APP_ID_SLUG} transform <in> <out> -p "pencil sketch drawing" --json. ` +
          'Build the steps as: shell (capture 1), wait 2000 ms, shell (capture 2), ' +
          'shell (transform 1), shell (transform 2). ' +
          'Reply with BURST_DONE once the sequence is complete (camera errors are OK, just report them).',
      },
    ],
  },

  'case-snapshot': {
    id: 'case-snapshot',
    cwdOffset: 'case-snapshot',
    goal:
      'P12 Named snapshot: first turn writes a file and saves a snapshot. ' +
      'Second turn loads the snapshot and must confirm the loaded context.',
    options: {
      chatPreset: 'superfast',
      agent: ['--multi-turn'],
      plannerBudget: '4',
      maxIter: '5',
    },
    rounds: [
      {
        index: 1,
        name: 'write file + save snapshot',
        prompt:
          'Create a file called snap-data.txt in the current folder with content "SNAP_VALUE_42". ' +
          'After writing reply: SNAPSHOT_ROUND_1_DONE',
        agentFlags: ['--save-snapshot', 'test-snap-p12'],
      },
      {
        index: 2,
        name: 'load snapshot + read file',
        prompt:
          'Read the file snap-data.txt in the current folder and reply exactly: ' +
          'SNAP_CONTENT <the file contents>',
        agentFlags: ['--load-snapshot', 'test-snap-p12'],
      },
    ],
  },

  // ── Explicit memory tools (loopback / offline) ────────────────────────────
  // Drives memory_write → memory_read → memory_find across three rounds on the
  // same session ID. Uses --router loopback with scripted responses so the test
  // is deterministic and offline. No planner, no preset (no API key needed).
  'case-memory-tools': {
    id: 'case-memory-tools',
    cwdOffset: 'case-memory-tools',
    goal:
      'Verify memory_write / memory_read / memory_find end-to-end via global ' +
      'default_memory.doc persistence.',
    options: {
      noPreset:  true,
      noPlanner: true,
      router:    'loopback',
      agent:     ['--single-turn'],
      maxIter:   '5',
    },
    rounds: [
      {
        index: 1,
        name: 'memory_write — store fact',
        prompt: 'memory-write-test',
        envOverrides: {
          PM_LOOPBACK_SCRIPT: JSON.stringify([
            {
              tool_calls: [{
                id: 'tc_mw1',
                name: 'memory_write',
                arguments: { state: { codename: 'Nighthawk', phase: 2 } },
              }],
            },
            { text: 'MEMORY_WRITTEN codename=Nighthawk phase=2' },
          ]),
        },
      },
      {
        index: 2,
        name: 'memory_read — recall fact',
        prompt: 'memory-read-test',
        envOverrides: {
          PM_LOOPBACK_SCRIPT: JSON.stringify([
            {
              tool_calls: [{
                id: 'tc_mr1',
                name: 'memory_read',
                arguments: {},
              }],
            },
            { text: 'MEMORY_READ_DONE' },
          ]),
        },
      },
      {
        index: 3,
        name: 'memory_find — search fact',
        prompt: 'memory-find-test',
        envOverrides: {
          PM_LOOPBACK_SCRIPT: JSON.stringify([
            {
              tool_calls: [{
                id: 'tc_mf1',
                name: 'memory_find',
                arguments: { query: 'Nighthawk' },
              }],
            },
            { text: 'MEMORY_FIND_DONE' },
          ]),
        },
      },
    ],
  },

  // ── Explicit memory tools (live) ──────────────────────────────────────────
  // Two turns with a real LLM on the default quick preset. Checks that the
  // agent actually calls memory_write / memory_read when instructed, and that
  // the stored fact survives across turns.
  'case-memory-tools-live': {
    id: 'case-memory-tools-live',
    cwdOffset: 'case-memory-tools-live',
    goal:
      'Agent stores a fact with memory_write in round 1, then recalls it with ' +
      'memory_read in round 2. Validates default_memory.doc round-trip.',
    options: {
      noPreset:    true,
      agent:       ['--multi-turn'],
      plannerBudget: '4',
      maxIter:     '4',
    },
    rounds: [
      {
        index: 1,
        name: 'write fact via memory_write',
        prompt:
          'Use the memory_write tool to store exactly: {"codename":"Nighthawk","phase":2}. ' +
          'After the tool succeeds reply: MEMORY_WRITTEN',
      },
      {
        index: 2,
        name: 'read back via memory_read',
        prompt:
          'Use the memory_read tool to read your stored memory. ' +
          'Then reply: RECALL codename=<value> phase=<value>',
      },
    ],
  },

  // ── Cross-session memory (loopback) ───────────────────────────────────────
  // Write a secret, then verify another turn can find it via default memory.
  'case-memory-cross-session': {
    id: 'case-memory-cross-session',
    cwdOffset: 'case-memory-cross-session',
    goal:
      'Turn 1 stores a secret with memory_write; turn 2 finds it via ' +
      'memory_find(searchPastChats=true) from global default memory.',
    options: {
      noPreset:  true,
      noPlanner: true,
      router:    'loopback',
      agent:     ['--single-turn'],
      maxIter:   '5',
    },
    rounds: [
      {
        index: 1,
        sessionSuffix: '-a',
        name: 'session A — write secret',
        prompt: 'cross-session-write',
        envOverrides: {
          PM_LOOPBACK_SCRIPT: JSON.stringify([
            {
              tool_calls: [{
                id: 'tc_xw1',
                name: 'memory_write',
                arguments: { state: { secret_token: 'CrossSession42' } },
              }],
            },
            { text: 'CROSS_WRITTEN secret=CrossSession42' },
          ]),
        },
      },
      {
        index: 2,
        sessionSuffix: '-b',
        name: 'session B — find across sessions',
        prompt: 'cross-session-find',
        envOverrides: {
          PM_LOOPBACK_SCRIPT: JSON.stringify([
            {
              tool_calls: [{
                id: 'tc_xf1',
                name: 'memory_find',
                arguments: { query: 'CrossSession42', searchPastChats: true },
              }],
            },
            { text: 'CROSS_FOUND' },
          ]),
        },
      },
    ],
  },

  // ── create_command against an isolated commands.json (real tool, no LLM) ─
  // Writes only under stemp/case-create-command/config via --config-dir.
  'case-create-command': {
    id: 'case-create-command',
    cwdOffset: 'case-create-command',
    goal:
      'Exercise create_command on the built CLI: internal dry-parse, shell ' +
      'one-liner, external exe, and the fail-closed paths (ENV vars, missing ' +
      'group, duplicate label). Persistence is an isolated commands.json.',
    options: {
      noPreset:  true,
      noPlanner: true,
      router:    'loopback',
      agent:     ['--single-turn', '--consent-ui', 'auto-allow'],
      maxIter:   '4',
      configDir: 'config',
    },
    seedConfig: {
      version: 1,
      ribbon: { groups: [{ label: 'Existing', items: [] }] },
    },
    rounds: [
      {
        index: 1,
        name: 'internal CLI command',
        prompt: 'create a ribbon button: create-internal',
        envOverrides: {
          PM_LOOPBACK_SCRIPT: JSON.stringify([
            {
              tool_calls: [{
                id: 'tc_cc1',
                name: 'create_command',
                arguments: {
                  label: 'Harness Commands',
                  command: 'commands',
                  args: ['--json'],
                  group: 'Existing',
                },
              }],
            },
            { text: 'CREATED_INTERNAL' },
          ]),
        },
      },
      {
        index: 2,
        name: 'shell one-liner',
        prompt: 'create a ribbon button: create-shell',
        envOverrides: {
          PM_LOOPBACK_SCRIPT: JSON.stringify([
            {
              tool_calls: [{
                id: 'tc_cc2',
                name: 'create_command',
                arguments: {
                  label: 'Harness Shell',
                  shell: 'pwsh',
                  shell_line: 'Write-Output harness',
                },
              }],
            },
            { text: 'CREATED_SHELL' },
          ]),
        },
      },
      {
        index: 3,
        name: 'external exe',
        prompt: 'create a ribbon button: create-external',
        envOverrides: {
          PM_LOOPBACK_SCRIPT: JSON.stringify([
            {
              tool_calls: [{
                id: 'tc_cc3',
                name: 'create_command',
                arguments: {
                  label: 'Harness Where',
                  command: 'C:\\Windows\\System32\\where.exe',
                  args: ['cmd'],
                  group: 'Existing',
                },
              }],
            },
            { text: 'CREATED_EXTERNAL' },
          ]),
        },
      },
      {
        index: 4,
        name: 'reject ${ENV:}',
        prompt: 'create a ribbon button: reject-env',
        envOverrides: {
          PM_LOOPBACK_SCRIPT: JSON.stringify([
            {
              tool_calls: [{
                id: 'tc_cc4',
                name: 'create_command',
                arguments: {
                  label: 'Harness Env',
                  command: 'commands',
                  args: ['${ENV:PATH}'],
                },
              }],
            },
            { text: 'REJECTED_ENV' },
          ]),
        },
      },
      {
        index: 5,
        name: 'reject missing group',
        prompt: 'create a ribbon button: reject-group',
        envOverrides: {
          PM_LOOPBACK_SCRIPT: JSON.stringify([
            {
              tool_calls: [{
                id: 'tc_cc5',
                name: 'create_command',
                arguments: {
                  label: 'Harness Missing Group',
                  command: 'commands',
                  args: ['--json'],
                  group: 'No Such Group',
                },
              }],
            },
            { text: 'REJECTED_GROUP' },
          ]),
        },
      },
      {
        index: 6,
        name: 'create_group and duplicate label',
        prompt: 'create a ribbon button: create-group-then-dup',
        envOverrides: {
          PM_LOOPBACK_SCRIPT: JSON.stringify([
            {
              tool_calls: [{
                id: 'tc_cc6',
                name: 'create_command',
                arguments: {
                  label: 'Harness New Group',
                  command: 'commands',
                  args: ['--json'],
                  group: 'Harness Group',
                  create_group: true,
                },
              }],
            },
            {
              tool_calls: [{
                id: 'tc_cc7',
                name: 'create_command',
                arguments: {
                  label: 'Harness Commands',
                  command: 'commands',
                  args: ['--json'],
                },
              }],
            },
            { text: 'CREATED_GROUP_REJECTED_DUP' },
          ]),
        },
      },
    ],
  },

  // ── Cross-turn memory (live) ─────────────────────────────────────────────
  'case-memory-cross-session-live': {
    id: 'case-memory-cross-session-live',
    cwdOffset: 'case-memory-cross-session-live',
    goal:
      'Live smoke: turn 1 writes a secret; turn 2 finds it with memory_find ' +
      'from global default memory.',
    options: {
      noPreset: true,
      agent:    ['--single-turn'],
      maxIter:  '4',
    },
    rounds: [
      {
        index: 1,
        sessionSuffix: '-a',
        name: 'session A — write secret',
        prompt:
          'Use memory_write to store exactly: {"secret_token":"CrossSession42"}. ' +
          'After the tool succeeds reply: CROSS_WRITTEN',
      },
      {
        index: 2,
        sessionSuffix: '-b',
        name: 'session B — find across sessions',
        prompt:
          'Use memory_find with searchPastChats=true and query "CrossSession42" to search ' +
          'past chat sessions. Reply: CROSS_FOUND count=<number of matches>',
      },
    ],
  },
};

function selectedCases() {
  const flag = process.argv.find((arg) => arg.startsWith('--case='));
  if (!flag) return Object.values(CASES);
  const ids = flag.slice('--case='.length).split(',').map((id) => id.trim()).filter(Boolean);
  return ids.map((id) => {
    if (!CASES[id]) throw new Error(`unknown case id: ${id}`);
    return CASES[id];
  });
}

function verifyCaseOutputs(testCase) {
  const dir = caseAbsDir(testCase);
  const checks = [];
  if (testCase.expected?.jsonFile) {
    const p = join(dir, testCase.expected.jsonFile);
    assertHarness(existsSync(p), `${testCase.id}: JSON output exists: ${p}`);
    const doc = readJson(p);
    assertHarness(Array.isArray(doc.files), `${testCase.id}: JSON output has files array`);
    assertHarness(doc.files.length > 0, `${testCase.id}: JSON output lists at least one JPG`);
    checks.push({ kind: 'json', path: p, count: doc.files.length });
  }
  if (testCase.expected?.markdownFile) {
    const p = join(dir, testCase.expected.markdownFile);
    assertHarness(existsSync(p), `${testCase.id}: Markdown output exists: ${p}`);
    const md = readFileSync(p, 'utf8');
    if (Array.isArray(testCase.expected.markdownContains)) {
      for (const marker of testCase.expected.markdownContains) {
        assertHarness(md.includes(marker),
          `${testCase.id}: Markdown output contains ${marker}`);
      }
    } else {
      assertHarness(/\.jpe?g/i.test(md), `${testCase.id}: Markdown output mentions JPG files`);
    }
    checks.push({ kind: 'markdown', path: p, bytes: md.length });
  }
  return checks;
}

// ── P1: planner context packet verification ─────────────────────────────────
// Checks three contracts introduced by P1:
//   1. After a successful planner run (fallback=false), intent/action/subject
//      fields appear at the top level of planner_meta (promoted out of raw).
//   2. When at least one context field is present, the main agent's system
//      prompt contains a "## Turn Context" block.
//   3. R2 (execute) carries a non-empty subject; R3 (followup) carries a
//      subject that references "video".
function verifyPlannerContextBehavior(testCase, caseSummary) {
  const checks = [];

  for (const round of caseSummary.rounds || []) {
    assertHarness(round.bundlePath, `${testCase.id}: round ${round.round} bundle path exists`);
    const communication = readJson(round.bundlePath)?.communication || {};
    const plannerMeta = communication.planner?.meta || {};
    const mainSystem  = roleContent(communication.main_agent?.messages, 'system');

    // Planner must not have encountered an error.  Two valid fallback reasons:
    //   fallback=false             → planner selected tools (normal)
    //   reason="planner selected no tools" → pure-query turn, intentional
    // Invalid reasons (planner did not attempt): "tools disabled", "realtime
    // mode", "planner LLM error", "planner JSON parse failed".
    const didFallback = plannerMeta.fallback === true;
    const reason = plannerMeta.reason || '';
    const intentionalZeroTools = didFallback && reason === 'planner selected no tools';
    const plannerErrored = didFallback && !intentionalZeroTools;
    assertHarness(!plannerErrored,
      `${testCase.id}: round ${round.round} planner did not error ` +
      `(meta: ${JSON.stringify({ fallback: plannerMeta.fallback, reason })})`);

    // When the planner ran (either selected tools or intentional 0-tool), it
    // must have emitted at least `intent`.
    const intent  = plannerMeta.intent  || '';
    const subject = plannerMeta.subject || '';
    assertHarness(intent.length > 0,
      `${testCase.id}: round ${round.round} planner_meta.intent is populated ` +
      `(got: ${JSON.stringify(intent)}, reason: ${reason})`);

    // The main agent's system prompt must carry the ## Turn Context block
    // whenever intent was classified (including 0-tool turns now that the
    // condition checks for intent fields rather than fallback=false).
    assertHarness(/## Turn Context/i.test(mainSystem),
      `${testCase.id}: round ${round.round} main system prompt contains ## Turn Context ` +
      `(intent=${intent}, first 120 chars of system: "${mainSystem.slice(0, 120).replace(/\n/g, '↵')}")`);
    assertHarness(new RegExp(`intent:\\s*${intent}`, 'i').test(mainSystem),
      `${testCase.id}: round ${round.round} ## Turn Context contains intent value "${intent}"`);

    if (round.round === 2) {
      // Execute round: planner must include `run` and subject must be non-empty.
      const selected = Array.isArray(plannerMeta.selected_tools) ? plannerMeta.selected_tools : [];
      assertHarness(selected.includes('run'),
        `${testCase.id}: round 2 planner selected run (got: ${selected.join(',') || 'none'})`);
      assertHarness(subject.length > 0,
        `${testCase.id}: round 2 planner_meta.subject is populated (got: "${subject}")`);
    }

    if (round.round === 3) {
      // Followup round: intent should be "followup" and subject should mention video.
      assertHarness(/followup/i.test(intent),
        `${testCase.id}: round 3 intent is "followup" (got: "${intent}")`);
      assertHarness(/video/i.test(subject),
        `${testCase.id}: round 3 subject mentions video (got: "${subject}")`);
    }

    // P3: when the planner emits a known intent, a ## Behavioral Mode block must
    // appear in the system prompt (mode overlay file is present in roaming prompts).
    const INTENT_HAS_OVERLAY = new Set(['query', 'execute', 'followup', 'write', 'create']);
    if (INTENT_HAS_OVERLAY.has(intent)) {
      assertHarness(/## Behavioral Mode:/i.test(mainSystem),
        `${testCase.id}: round ${round.round} system prompt has ## Behavioral Mode block for intent="${intent}" ` +
        `(snippet: "${mainSystem.slice(mainSystem.search(/Behavioral Mode/i) >= 0 ? mainSystem.search(/Behavioral Mode/i) : 0, 80).replace(/\n/g, '↵')}")`);
    }

    // P2: confident planner (confidence >= 0.4) must NOT emit clarification_needed.
    const confidence = typeof plannerMeta.confidence === 'number' ? plannerMeta.confidence : 1.0;
    const clarEvents = Array.isArray(communication.clarification_events)
      ? communication.clarification_events : [];
    if (confidence >= 0.4) {
      assertHarness(clarEvents.length === 0,
        `${testCase.id}: round ${round.round} no ClarificationNeeded for confidence=${confidence.toFixed(2)} ` +
        `(got ${clarEvents.length} event(s))`);
    }

    // P5: successful turn must NOT emit error_reflexion events.
    const reflexionEvents = Array.isArray(communication.error_reflexion_events)
      ? communication.error_reflexion_events : [];
    assertHarness(reflexionEvents.length === 0,
      `${testCase.id}: round ${round.round} no ErrorReflexion event on successful turn ` +
      `(got ${reflexionEvents.length} event(s))`);

    checks.push({
      kind: 'planner-context',
      round: round.round,
      intent,
      action:  plannerMeta.action  || '',
      subject,
      confidence,
    });
  }

  return checks;
}

// ── P6: parallel tool dispatch verification ──────────────────────────────────
// Checks that both fixture files were read and their sentinel content tokens
// appear in the final reply. If the model batched both file_read calls into a
// single LLM response (the "parallel burst" pattern), also verifies that all
// tool_call events precede any tool_result events within that burst — the
// ordering guarantee of the P6 parallel collection loop.
function verifyParallelToolsBehavior(testCase, caseSummary) {
  const checks = [];

  const round1 = (caseSummary.rounds || []).find((r) => r.round === 1);
  assertHarness(round1?.bundlePath, `${testCase.id}: round 1 bundle exists`);
  const communication = readJson(round1.bundlePath)?.communication || {};
  const tools = Array.isArray(communication.tools) ? communication.tools : [];

  // Both file reads must have been attempted.
  const readCalls = tools.filter((e) => e.kind === 'tool_call' && e.tool === 'file_read');
  assertHarness(
    readCalls.length >= 2,
    `${testCase.id}: both fixture files read via file_read (got ${readCalls.length} call(s); ` +
    `all tool events: ${tools.map((e) => `${e.kind}:${e.tool}`).join(', ')})`,
  );

  // Both sentinel strings must appear in the final reply.
  const finalText = String(round1.finalText || communication.main_agent?.final_text || '');
  assertHarness(
    /ALPHA_CONTENT_XJ7/i.test(finalText),
    `${testCase.id}: final reply contains alpha.txt sentinel (got: "${truncateText(finalText, 200)}")`,
  );
  assertHarness(
    /BETA_CONTENT_QZ3/i.test(finalText),
    `${testCase.id}: final reply contains beta.txt sentinel (got: "${truncateText(finalText, 200)}")`,
  );

  // Opportunistic parallel-burst check.  Not a hard failure — some providers
  // only return one tool_call per response (e.g. certain OpenRouter models).
  // When a burst IS detected, the P6 collection loop must have preserved order:
  //   [tool_call, tool_call, …, tool_result, tool_result, …]
  let burstDetected = false;
  for (let i = 1; i < tools.length; i++) {
    if (tools[i].kind === 'tool_call' && tools[i - 1].kind === 'tool_call') {
      burstDetected = true;
      break;
    }
  }
  if (burstDetected) {
    // Find the longest leading run of consecutive tool_calls.
    const burstStart = tools.findIndex((e) => e.kind === 'tool_call');
    let burstEnd = burstStart;
    while (burstEnd + 1 < tools.length && tools[burstEnd + 1].kind === 'tool_call') burstEnd++;
    const burstSize = burstEnd - burstStart + 1;
    const postBurst = tools.slice(burstEnd + 1, burstEnd + 1 + burstSize);
    assertHarness(
      postBurst.length === burstSize && postBurst.every((e) => e.kind === 'tool_result'),
      `${testCase.id}: P6 burst of ${burstSize} tool_calls followed by ${burstSize} tool_results ` +
      `(got post-burst kinds: ${postBurst.map((e) => e.kind).join(',')})`,
    );
    console.log(`  P6 burst    : ✓ ${burstSize} tool calls dispatched in parallel`);
  } else {
    console.log(`  P6 burst    : – model returned tool calls one-at-a-time (serial fallback; no failure)`);
  }

  checks.push({ kind: 'parallel-tools', burstDetected, readCallCount: readCalls.length });
  return checks;
}

// P9: verify SelfCheckFeature emits a passed self_check event
function verifySelfCheckBehavior(testCase, caseSummary) {
  const checks = [];
  const round1 = (caseSummary.rounds || []).find((r) => r.round === 1);
  assertHarness(round1?.bundlePath, `${testCase.id}: round 1 communication bundle exists`);
  const bundle = readJson(round1.bundlePath);
  const communication = bundle?.communication || {};

  const selfCheckEvents = Array.isArray(communication.self_check_events) ? communication.self_check_events : [];
  assertHarness(
    selfCheckEvents.length > 0,
    `${testCase.id}: self_check event must be present in event log`,
  );
  const selfCheckEvent = selfCheckEvents[0] ? { detail: selfCheckEvents[0] } : undefined;
  if (selfCheckEvents.length > 0) {
    const detail = selfCheckEvents[0];
    assertHarness(
      detail.passed === true,
      `${testCase.id}: self_check.passed must be true (got ${JSON.stringify(detail.passed)})`,
    );
    assertHarness(
      detail.deflection_detected === false,
      `${testCase.id}: self_check.deflection_detected must be false`,
    );
    console.log(
      `  P9 self_check: passed=${detail.passed} tool_ok_rate=${detail.tool_ok_rate?.toFixed(2)} ` +
      `confidence=${detail.completion_confidence?.toFixed(2)}`,
    );
    checks.push({ kind: 'self-check', passed: detail.passed, detail });
  }
  return checks;
}

// run_sequence: verify the agent used the sequencer instead of a bare run-sleep loop
// or child agent dispatch for a timed camera burst task.
function verifyTimedCaptureBehavior(testCase, caseSummary) {
  const checks = [];
  const round1 = (caseSummary.rounds || []).find((r) => r.round === 1);
  assertHarness(round1?.bundlePath, `${testCase.id}: round 1 bundle exists`);
  const communication = readJson(round1.bundlePath)?.communication || {};
  const tools = Array.isArray(communication.tools) ? communication.tools : [];
  const toolCalls = tools.filter((e) => e.kind === 'tool_call').map((e) => e.tool);
  const plannerSelected = communication.planner?.meta?.selected_tools || [];

  // 1. Planner must have selected run_sequence (and ideally info_lookup).
  assertHarness(
    plannerSelected.includes('run_sequence'),
    `${testCase.id}: planner selected run_sequence (got: ${plannerSelected.join(', ')})`,
  );

  // 2. Agent must have called run_sequence at least once.
  const seqCalls = tools.filter((e) => e.kind === 'tool_call' && e.tool === 'run_sequence');
  assertHarness(
    seqCalls.length >= 1,
    `${testCase.id}: agent called run_sequence (got: ${toolCalls.join(', ')})`,
  );

  // 3. Agent should have used info_lookup to discover the CLI command before
  //    building the sequence (not mandatory — the command may be known).
  const usedInfoLookup = toolCalls.includes('info_lookup');
  // Soft check — log but don't fail; some models know the syntax already.
  if (!usedInfoLookup)
    console.log(`  ⚠  ${testCase.id}: info_lookup not called (agent may have known CLI syntax already)`);

  // 3. No child agent spawning via run.
  const runCalls = tools.filter((e) => e.kind === 'tool_call' && e.tool === 'run');
  const childAgentRuns = runCalls.filter((e) => {
    const cmd = e.detail?.arguments?.command || '';
    return new RegExp(brand.APP_ID_SLUG.replace(/-/g, '-?') + '(-cli)?.*(llm\\s+agent|agent\\s+--)', 'i').test(cmd);
  });
  assertHarness(
    childAgentRuns.length === 0,
    `${testCase.id}: no child agent spawned via run (found ${childAgentRuns.length})`,
  );

  // 4. run_sequence steps must contain a camera/capture shell command AND a wait block.
  const seqHasCamera = seqCalls.some((e) => {
    const steps = e.detail?.arguments?.steps;
    if (!Array.isArray(steps)) return false;
    return steps.some((s) =>
      /video\s+image|custom\.capture|image_from_camera/i
        .test(JSON.stringify(s)) ||
      new RegExp(brand.APP_ID_SLUG + '.*(video|capture)', 'i').test(JSON.stringify(s))
    );
  });
  assertHarness(
    seqHasCamera,
    `${testCase.id}: run_sequence steps contain a camera capture command`,
  );

  const seqHasWait = seqCalls.some((e) => {
    const steps = e.detail?.arguments?.steps;
    if (!Array.isArray(steps)) return false;
    return steps.some((s) => s?.kind === 'wait' && typeof s?.ms === 'number' && s.ms >= 1000);
  });
  assertHarness(
    seqHasWait,
    `${testCase.id}: run_sequence contains a wait block (≥ 1 s) for precise timing`,
  );

  // 5. Verify the .xblox artefact was written into the case dir.
  const seqResult = tools.find((e) => e.kind === 'tool_result' && e.tool === 'run_sequence');
  const xbloxFile = seqResult?.detail?.envelope?.result?.xblox_file
    || seqResult?.detail?.envelope?.xblox_file;
  if (xbloxFile) {
    assertHarness(
      existsSync(xbloxFile),
      `${testCase.id}: .xblox artefact written to disk at ${xbloxFile}`,
    );
    checks.push({ kind: 'xblox_artifact', path: xbloxFile });
  } else {
    console.log(`  ⚠  ${testCase.id}: xblox_file path not found in run_sequence result envelope`);
  }

  // 6. The whole job must run inside the sequence: a transform shell step is
  //    present (not split out into a separate image_transform LLM round).
  const seqHasTransform = seqCalls.some((e) => {
    const steps = e.detail?.arguments?.steps;
    if (!Array.isArray(steps)) return false;
    return steps.some((s) => /pm-image[^"]*transform/i.test(JSON.stringify(s)));
  });
  assertHarness(
    seqHasTransform,
    `${testCase.id}: run_sequence includes a pm-image transform step (sketch in-sequence)`,
  );

  // 7. The sketch outputs must actually land on disk — proves the transform
  //    step received valid args, ran to completion, and was not killed by the
  //    old 30s shell timeout.
  const caseDir = caseSummary.cwd;
  const sketchFiles = ['cam_shot_001_sketch.jpg', 'cam_shot_002_sketch.jpg'];
  const sketchesOnDisk = sketchFiles.filter((f) => existsSync(join(caseDir, f)));
  assertHarness(
    sketchesOnDisk.length >= 1,
    `${testCase.id}: at least one pencil-sketch output written to disk ` +
      `(found ${sketchesOnDisk.length}/${sketchFiles.length} in ${caseDir})`,
  );

  checks.push({
    kind: 'timed_capture',
    seqCalls: seqCalls.length,
    seqHasTransform,
    sketchesOnDisk,
    toolCalls,
    plannerSelected,
  });
  return checks;
}

// P12: verify snapshot save (round 1) wrote a file; verify snapshot load (round 2) surfaced context
function verifySnapshotBehavior(testCase, caseSummary) {
  const checks = [];

  // Round 1: file was created, reply contains sentinel
  const round1 = (caseSummary.rounds || []).find((r) => r.round === 1);
  assertHarness(round1?.bundlePath, `${testCase.id}: round 1 communication bundle exists`);
  const bundle1 = readJson(round1.bundlePath);
  const finalText1 = bundle1?.communication?.main_agent?.final_text || '';
  assertHarness(
    finalText1.includes('SNAPSHOT_ROUND_1_DONE'),
    `${testCase.id}: round 1 reply must contain SNAPSHOT_ROUND_1_DONE`,
  );

  // Snapshot file lives adjacent to the session dir (parent / snapshots).
  // In the test harness PM_IMAGE_SESSION_DIR overrides the session dir to
  // stemp/agent-memory/sessions, so snapshots land in stemp/agent-memory/snapshots.
  const snapFile = join(RUN_DIR, 'snapshots', 'test-snap-p12.json');
  assertHarness(
    existsSync(snapFile),
    `${testCase.id}: snapshot file must exist at ${snapFile}`,
  );
  console.log(`  P12 snapshot : ✓ file exists at ${snapFile}`);

  // Round 2: agent reads the file and replies with content sentinel
  const round2 = (caseSummary.rounds || []).find((r) => r.round === 2);
  assertHarness(round2?.bundlePath, `${testCase.id}: round 2 communication bundle exists`);
  const bundle2 = readJson(round2.bundlePath);
  const finalText2 = bundle2?.communication?.main_agent?.final_text || '';
  assertHarness(
    finalText2.includes('SNAP_CONTENT'),
    `${testCase.id}: round 2 reply must contain SNAP_CONTENT`,
  );

  checks.push({ kind: 'snapshot', snapFileExists: existsSync(snapFile) });
  return checks;
}

function toolResultEnvelope(bundlePath, toolName) {
  const bundle = bundlePath ? readJson(bundlePath) : null;
  const events = bundle?.communication?.tools || [];
  const ev = events.find((e) => e.kind === 'tool_result' && e.tool === toolName);
  if (!ev) return null;
  let d = ev.detail;
  if (typeof d === 'string') { try { d = JSON.parse(d); } catch { d = null; } }
  if (!d) return null;
  return d.envelope ?? d;
}

function toolCalls(bundlePath) {
  const bundle = bundlePath ? readJson(bundlePath) : null;
  const events = bundle?.communication?.tools || [];
  return events.filter((e) => e.kind === 'tool_call').map((e) => e.tool);
}

function verifyMemoryToolsBehavior(testCase, caseSummary) {
  const checks = [];
  const round = (n) => (caseSummary.rounds || []).find((r) => r.round === n);

  const r1 = round(1);
  assertHarness(r1?.bundlePath, `${testCase.id}: round 1 bundle exists`);
  assertHarness(
    toolCalls(r1.bundlePath).includes('memory_write'),
    `${testCase.id}: round 1 called memory_write`,
  );
  const writeResult = toolResultEnvelope(r1.bundlePath, 'memory_write');
  assertHarness(
    writeResult?.ok === true,
    `${testCase.id}: round 1 memory_write returned ok=true (got ${JSON.stringify(writeResult?.ok)})`,
  );
  assertHarness(
    writeResult?.saved?.codename === 'Nighthawk',
    `${testCase.id}: round 1 memory_write saved codename=Nighthawk (got ${JSON.stringify(writeResult?.saved?.codename)})`,
  );
  const r1Final = readJson(r1.bundlePath)?.communication?.main_agent?.final_text || '';
  assertHarness(
    r1Final.includes('MEMORY_WRITTEN'),
    `${testCase.id}: round 1 final text contains sentinel MEMORY_WRITTEN`,
  );

  const r2 = round(2);
  assertHarness(r2?.bundlePath, `${testCase.id}: round 2 bundle exists`);
  assertHarness(
    toolCalls(r2.bundlePath).includes('memory_read'),
    `${testCase.id}: round 2 called memory_read`,
  );
  const readResult = toolResultEnvelope(r2.bundlePath, 'memory_read');
  assertHarness(
    readResult?.ok === true,
    `${testCase.id}: round 2 memory_read returned ok=true (got ${JSON.stringify(readResult?.ok)})`,
  );
  assertHarness(
    JSON.stringify(readResult?.state ?? {}).includes('Nighthawk'),
    `${testCase.id}: round 2 memory_read state references Nighthawk`,
  );

  const r3 = round(3);
  assertHarness(r3?.bundlePath, `${testCase.id}: round 3 bundle exists`);
  assertHarness(
    toolCalls(r3.bundlePath).includes('memory_find'),
    `${testCase.id}: round 3 called memory_find`,
  );
  const findResult = toolResultEnvelope(r3.bundlePath, 'memory_find');
  assertHarness(
    findResult?.ok === true,
    `${testCase.id}: round 3 memory_find returned ok=true (got ${JSON.stringify(findResult?.ok)})`,
  );
  assertHarness(
    typeof findResult?.count === 'number' && findResult.count > 0,
    `${testCase.id}: round 3 memory_find returned at least one item (count=${findResult?.count})`,
  );

  checks.push({ kind: 'memory-tools-loopback', writeOk: writeResult?.ok, readState: readResult?.state, findCount: findResult?.count });
  return checks;
}

function verifyMemoryToolsLiveBehavior(testCase, caseSummary) {
  const checks = [];
  const round = (n) => (caseSummary.rounds || []).find((r) => r.round === n);

  const r1 = round(1);
  assertHarness(r1?.bundlePath, `${testCase.id}: round 1 bundle exists`);
  assertHarness(
    toolCalls(r1.bundlePath).includes('memory_write'),
    `${testCase.id}: round 1 called memory_write`,
  );
  const writeResult = toolResultEnvelope(r1.bundlePath, 'memory_write');
  assertHarness(
    writeResult?.ok === true,
    `${testCase.id}: round 1 memory_write returned ok=true (got ${JSON.stringify(writeResult?.ok)})`,
  );
  assertHarness(
    writeResult?.saved?.codename === 'Nighthawk',
    `${testCase.id}: round 1 memory_write saved codename=Nighthawk (got ${JSON.stringify(writeResult?.saved?.codename)})`,
  );
  const r1Final = readJson(r1.bundlePath)?.communication?.main_agent?.final_text || '';
  assertHarness(
    /MEMORY_WRITTEN/i.test(r1Final),
    `${testCase.id}: round 1 final text contains MEMORY_WRITTEN sentinel`,
  );

  const r2 = round(2);
  assertHarness(r2?.bundlePath, `${testCase.id}: round 2 bundle exists`);
  assertHarness(
    toolCalls(r2.bundlePath).includes('memory_read'),
    `${testCase.id}: round 2 called memory_read`,
  );
  const readResult = toolResultEnvelope(r2.bundlePath, 'memory_read');
  assertHarness(
    readResult?.ok === true,
    `${testCase.id}: round 2 memory_read returned ok=true (got ${JSON.stringify(readResult?.ok)})`,
  );
  const r2Final = readJson(r2.bundlePath)?.communication?.main_agent?.final_text || '';
  assertHarness(
    /Nighthawk/i.test(r2Final),
    `${testCase.id}: round 2 reply mentions Nighthawk (got: ${truncateText(r2Final, 120)})`,
  );

  checks.push({ kind: 'memory-tools-live', writeOk: writeResult?.ok, r2Final: truncateText(r2Final, 120) });
  return checks;
}

function verifyMemoryCrossSessionBehavior(testCase, caseSummary) {
  const checks = [];
  const round = (n) => (caseSummary.rounds || []).find((r) => r.round === n);

  const r1 = round(1);
  assertHarness(r1?.bundlePath, `${testCase.id}: round 1 bundle exists`);
  assertHarness(
    toolCalls(r1.bundlePath).includes('memory_write'),
    `${testCase.id}: session A called memory_write`,
  );
  const writeResult = toolResultEnvelope(r1.bundlePath, 'memory_write');
  assertHarness(writeResult?.ok === true, `${testCase.id}: session A memory_write ok=true`);
  assertHarness(
    writeResult?.saved?.secret_token === 'CrossSession42',
    `${testCase.id}: session A saved secret_token=CrossSession42`,
  );

  const r2 = round(2);
  assertHarness(r2?.bundlePath, `${testCase.id}: round 2 bundle exists`);
  assertHarness(
    toolCalls(r2.bundlePath).includes('memory_find'),
    `${testCase.id}: session B called memory_find`,
  );
  const findResult = toolResultEnvelope(r2.bundlePath, 'memory_find');
  assertHarness(findResult?.ok === true, `${testCase.id}: session B memory_find ok=true`);
  assertHarness(
    typeof findResult?.count === 'number' && findResult.count > 0,
    `${testCase.id}: session B found prior-session matches (count=${findResult?.count})`,
  );
  const items = Array.isArray(findResult?.items) ? findResult.items : [];
  assertHarness(
    items.some((it) => JSON.stringify(it).includes('CrossSession42')),
    `${testCase.id}: memory_find results include CrossSession42`,
  );

  checks.push({ kind: 'memory-cross-session-loopback', findCount: findResult?.count });
  return checks;
}

function verifyMemoryCrossSessionLiveBehavior(testCase, caseSummary) {
  const checks = [];
  const round = (n) => (caseSummary.rounds || []).find((r) => r.round === n);

  const r1 = round(1);
  assertHarness(r1?.bundlePath, `${testCase.id}: round 1 bundle exists`);
  assertHarness(
    toolCalls(r1.bundlePath).includes('memory_write'),
    `${testCase.id}: session A called memory_write`,
  );
  const writeResult = toolResultEnvelope(r1.bundlePath, 'memory_write');
  assertHarness(writeResult?.ok === true, `${testCase.id}: session A memory_write ok=true`);
  const r1Final = readJson(r1.bundlePath)?.communication?.main_agent?.final_text || '';
  assertHarness(/CROSS_WRITTEN/i.test(r1Final), `${testCase.id}: session A replied CROSS_WRITTEN`);

  const r2 = round(2);
  assertHarness(r2?.bundlePath, `${testCase.id}: round 2 bundle exists`);
  assertHarness(
    toolCalls(r2.bundlePath).includes('memory_find'),
    `${testCase.id}: session B called memory_find`,
  );
  const findResult = toolResultEnvelope(r2.bundlePath, 'memory_find');
  assertHarness(findResult?.ok === true, `${testCase.id}: session B memory_find ok=true`);
  assertHarness(
    typeof findResult?.count === 'number' && findResult.count > 0,
    `${testCase.id}: session B found prior-session matches (count=${findResult?.count})`,
  );
  const items = Array.isArray(findResult?.items) ? findResult.items : [];
  assertHarness(
    items.some((it) => JSON.stringify(it).includes('CrossSession42')),
    `${testCase.id}: memory_find results include CrossSession42`,
  );
  const r2Final = readJson(r2.bundlePath)?.communication?.main_agent?.final_text || '';
  assertHarness(/CROSS_FOUND/i.test(r2Final), `${testCase.id}: session B replied CROSS_FOUND`);

  checks.push({ kind: 'memory-cross-session-live', findCount: findResult?.count });
  return checks;
}

function toolCallFingerprint(event) {
  const detail = event.detail || {};
  const args = detail.args ?? detail.arguments ?? detail;
  return `${event.tool}::${JSON.stringify(args)}`;
}

// When `--streaming-smoke` is set: every round gets `--streaming on` and we scan
// the agent log for duplicate tool_call / assistant_text replay (the class of
// bugs that showed up as triple assistant text or 2× tool rows in session docs).
function verifyStreamingSmokeBehavior(testCase, caseSummary) {
  if (!STREAMING_SMOKE) return [];
  const checks = [];

  for (const round of caseSummary.rounds || []) {
    assertHarness(round.logPath && existsSync(round.logPath),
      `${testCase.id}: round ${round.round} log exists for streaming smoke`);
    const logDoc = readJson(round.logPath);
    const events = Array.isArray(logDoc?.events) ? logDoc.events : [];

    const toolCalls = events.filter((event) => event.kind === 'tool_call');
    const toolFpCounts = new Map();
    for (const tc of toolCalls) {
      const fp = toolCallFingerprint(tc);
      toolFpCounts.set(fp, (toolFpCounts.get(fp) || 0) + 1);
    }
    const dupToolCalls = [...toolFpCounts.entries()].filter(([, count]) => count > 1);
    assertHarness(
      dupToolCalls.length === 0,
      `${testCase.id}: round ${round.round} no duplicate tool_call events ` +
      `(dupes: ${dupToolCalls.map(([fp, count]) => `${fp} x${count}`).join('; ')})`,
    );

    const assistantTexts = events
      .filter((event) => event.kind === 'assistant_text')
      .map((event) => String(event.text || ''))
      .filter(Boolean);
    const dupAssistantCounts = new Map();
    for (const text of assistantTexts) {
      dupAssistantCounts.set(text, (dupAssistantCounts.get(text) || 0) + 1);
    }
    const dupAssistant = [...dupAssistantCounts.entries()].filter(([, count]) => count > 1);
    assertHarness(
      dupAssistant.length === 0,
      `${testCase.id}: round ${round.round} no duplicate assistant_text log events ` +
      `(count=${dupAssistant[0]?.[1]}, len=${dupAssistant[0]?.[0]?.length ?? 0})`,
    );

    const finalText = String(round.finalText || '').trim();
    if (finalText.length >= 24) {
      const third = Math.floor(finalText.length / 3);
      const chunk = finalText.slice(0, third);
      if (chunk.length >= 8 && finalText === chunk.repeat(3)) {
        assertHarness(false,
          `${testCase.id}: round ${round.round} final text looks like a 3× replay ` +
          `(snippet: "${truncateText(chunk, 80)}")`);
      }
    }

    checks.push({
      kind: 'streaming-smoke',
      round: round.round,
      toolCalls: toolCalls.length,
      assistantTextEvents: assistantTexts.length,
      thinkingEvents: events.filter((event) => event.kind === 'thinking').length,
    });
  }

  return checks;
}

function verifyCaseBehavior(testCase, caseSummary) {
  const streamingSmokeChecks = verifyStreamingSmokeBehavior(testCase, caseSummary);
  let specificChecks = [];
  if (testCase.id === 'case-memory-tools') specificChecks = verifyMemoryToolsBehavior(testCase, caseSummary);
  else if (testCase.id === 'case-memory-tools-live') specificChecks = verifyMemoryToolsLiveBehavior(testCase, caseSummary);
  else if (testCase.id === 'case-memory-cross-session') specificChecks = verifyMemoryCrossSessionBehavior(testCase, caseSummary);
  else if (testCase.id === 'case-memory-cross-session-live') specificChecks = verifyMemoryCrossSessionLiveBehavior(testCase, caseSummary);
  else if (testCase.id === 'case-parallel-tools') specificChecks = verifyParallelToolsBehavior(testCase, caseSummary);
  else if (testCase.id === 'case-planner-context') specificChecks = verifyPlannerContextBehavior(testCase, caseSummary);
  else if (testCase.id === 'case-chat') specificChecks = verifyChatBehavior(testCase, caseSummary);
  else if (testCase.id === 'case-mixed') specificChecks = verifyMixedBehavior(testCase, caseSummary);
  else if (testCase.id === 'case-exact-turn-export') specificChecks = verifyExactTurnExportBehavior(testCase, caseSummary);
  else if (testCase.id === 'case-followup') specificChecks = verifyFollowupBehavior(testCase, caseSummary);
  else if (testCase.id === 'case-commands') specificChecks = verifyCommandsBehavior(testCase, caseSummary);
  else if (testCase.id === 'case-create-command') specificChecks = verifyCreateCommandBehavior(testCase, caseSummary);
  else if (testCase.id === 'case-agent-cli') specificChecks = verifyAgentCliBehavior(testCase, caseSummary);
  else if (testCase.id === 'case-app-use') specificChecks = verifyAppUseBehavior(testCase, caseSummary);
  else if (testCase.id === 'case-ask-user') specificChecks = verifyAskUserBehavior(testCase, caseSummary);
  else if (testCase.id === 'case-self-check') specificChecks = verifySelfCheckBehavior(testCase, caseSummary);
  else if (testCase.id === 'case-snapshot') specificChecks = verifySnapshotBehavior(testCase, caseSummary);
  else if (testCase.id === 'case-timed-capture') specificChecks = verifyTimedCaptureBehavior(testCase, caseSummary);
  else if (testCase.id !== 'case-dir') return streamingSmokeChecks;

  if (testCase.id !== 'case-dir') {
    return [...streamingSmokeChecks, ...specificChecks];
  }

  const checks = [];
  const round2 = (caseSummary.rounds || []).find((round) => round.round === 2);
  assertHarness(round2?.bundlePath, `${testCase.id}: round 2 communication bundle exists`);
  const bundle = readJson(round2.bundlePath);
  const communication = bundle?.communication || {};
  const tools = Array.isArray(communication.tools) ? communication.tools : [];
  const toolCalls = tools.filter((event) => event.kind === 'tool_call').map((event) => event.tool);
  assertHarness(toolCalls.includes('file_read'), `${testCase.id}: round 2 reads remembered JSON session item`);
  assertHarness(toolCalls.includes('write_file'), `${testCase.id}: round 2 writes Markdown session item`);
  assertHarness(!toolCalls.includes('file_glob'), `${testCase.id}: round 2 does not re-glob after session-item memory`);

  const plannerUser = (communication.planner?.messages || []).find((message) => message.role === 'user')?.content || '';
  // Planner now shares the same heading as the main-agent system prompt
  // (## Session Items) so a single rendering covers both. We accept either
  // the unified `## Session Items` or the legacy `Session items:` heading
  // so the test survives both code paths.
  assertHarness(/(?:## Session Items|Session items:)/i.test(plannerUser), `${testCase.id}: round 2 planner receives session items`);
  assertHarness(/jpg-listing\.json/i.test(plannerUser), `${testCase.id}: round 2 planner sees JSON item path`);
  assertHarness(/role=directory_listing/i.test(plannerUser), `${testCase.id}: round 2 planner sees directory listing role`);
  assertHarness(/(?:## Relevant Session Rules|Session rules:)/i.test(plannerUser), `${testCase.id}: round 2 planner receives scoped rules`);

  const mainSystem = roleContent(communication.main_agent?.messages, 'system');
  assertHarness(/## Session Items/i.test(mainSystem), `${testCase.id}: round 2 system prompt includes items`);
  assertHarness(/role=directory_listing/i.test(mainSystem), `${testCase.id}: round 2 system prompt includes item role`);
  assertHarness(/## Relevant Session Rules/i.test(mainSystem), `${testCase.id}: round 2 system prompt includes rules`);

  const memoryUpdates = Array.isArray(communication.memory_updates) ? communication.memory_updates : [];
  assertHarness(memoryUpdates.length > 0, `${testCase.id}: round 2 emits memory_update event`);
  assertHarness(
    memoryUpdates.some((update) => {
      const items = update?.patch?.items || {};
      // Item id derives from stem + sanitized extension under the new scheme
      // (jpg_listing_md). Fall back to scanning by path / role so the test
      // doesn't pin a specific id string format.
      if (items.jpg_listing_md) return true;
      return Object.values(items).some((it) =>
        it?.kind === 'file'
        && it?.role === 'markdown_document'
        && /jpg-listing\.md$/i.test(String(it?.locator?.path || '')));
    }),
    `${testCase.id}: round 2 memory_update includes Markdown session item`,
  );

  checks.push({ kind: 'behavior', path: round2.bundlePath, checks: toolCalls });
  return [...streamingSmokeChecks, ...checks];
}

function verifyMixedBehavior(testCase, caseSummary) {
  const checks = [];

  // Round 3 = pure chat recall of the codename across an intervening tool turn.
  const round3 = (caseSummary.rounds || []).find((round) => round.round === 3);
  assertHarness(round3?.bundlePath, `${testCase.id}: round 3 communication bundle exists`);
  const r3 = readJson(round3.bundlePath)?.communication || {};
  const r3Tools = (Array.isArray(r3.tools) ? r3.tools : [])
    .filter((event) => event.kind === 'tool_call').map((event) => event.tool);
  const r3External = r3Tools.filter((t) =>
    /^(file_|image_|create_video|web_|info_lookup|run|app_|speak|mcp_)/i.test(t));
  assertHarness(r3External.length === 0,
    `${testCase.id}: round 3 codename recall does not use external tools (got ${r3External.join(',') || 'none'})`);
  const r3Final = String(round3.finalText || r3.main_agent?.final_text || '');
  assertHarness(/Falcon/i.test(r3Final), `${testCase.id}: round 3 reply names the codename Falcon`);

  // Round 4 = tool turn that depends on BOTH the prior JSON artifact and the
  // chat codename. Path comes from session items, codename from turn_pair.
  const round4 = (caseSummary.rounds || []).find((round) => round.round === 4);
  assertHarness(round4?.bundlePath, `${testCase.id}: round 4 communication bundle exists`);
  const r4 = readJson(round4.bundlePath)?.communication || {};
  const r4Tools = (Array.isArray(r4.tools) ? r4.tools : [])
    .filter((event) => event.kind === 'tool_call').map((event) => event.tool);
  assertHarness(r4Tools.includes('file_read'),
    `${testCase.id}: round 4 reads remembered JSON listing via file_read`);
  assertHarness(r4Tools.includes('write_file'),
    `${testCase.id}: round 4 writes Markdown summary via write_file`);
  assertHarness(!r4Tools.includes('file_glob'),
    `${testCase.id}: round 4 does not re-glob after JSON item is in session memory`);

  const mainSystem4 = roleContent(r4.main_agent?.messages, 'system');
  assertHarness(/## Session Items/i.test(mainSystem4),
    `${testCase.id}: round 4 system prompt includes session items`);
  assertHarness(/falcon-listing\.json/i.test(mainSystem4),
    `${testCase.id}: round 4 system prompt carries JSON item path`);
  assertHarness(/Falcon/i.test(mainSystem4),
    `${testCase.id}: round 4 system prompt carries codename via turn_pair item`);

  // Final markdown must mention the codename.
  const mdPath = join(caseAbsDir(testCase), testCase.expected.markdownFile);
  assertHarness(existsSync(mdPath), `${testCase.id}: ${testCase.expected.markdownFile} exists on disk`);
  const md = readFileSync(mdPath, 'utf8');
  assertHarness(/Falcon/i.test(md), `${testCase.id}: Markdown summary mentions the codename`);

  checks.push({ kind: 'behavior', path: round4.bundlePath, checks: r4Tools });
  return checks;
}

function verifyExactTurnExportBehavior(testCase, caseSummary) {
  const checks = [];
  const round2 = (caseSummary.rounds || []).find((round) => round.round === 2);
  assertHarness(round2?.bundlePath, `${testCase.id}: round 2 communication bundle exists`);
  const communication = readJson(round2.bundlePath)?.communication || {};
  const calls = (Array.isArray(communication.tools) ? communication.tools : [])
    .filter((event) => event.kind === 'tool_call');
  const tools = calls.map((event) => event.tool);
  assertHarness(tools.includes('memory_find'),
    `${testCase.id}: terse follow-up materializes the prior turn (tools: ${tools.join(',') || 'none'})`);
  assertHarness(tools.includes('write_file'),
    `${testCase.id}: terse follow-up writes the requested Markdown file`);

  const exactCall = calls.find((event) => event.tool === 'memory_find');
  const exactArgs = exactCall?.detail?.arguments || {};
  assertHarness(/^turn_/i.test(String(exactArgs.itemId || '')) && exactArgs.includeBody === true,
    `${testCase.id}: memory_find uses exact itemId + includeBody`);

  const system = roleContent(communication.main_agent?.messages, 'system');
  assertHarness(/body_available=true/.test(system),
    `${testCase.id}: round 2 prompt exposes a compact retrievable-body hint`);
  assertHarness(/memory_find\(itemId=<turn id>, includeBody=true\)/.test(system),
    `${testCase.id}: round 2 prompt explains exact body materialization`);
  assertHarness(!system.includes('EXACT_EXPORT_TAIL_91C4'),
    `${testCase.id}: no broad transcript replay leaks the long tail into the prompt`);

  const expectedPath = join(caseAbsDir(testCase), testCase.expected.markdownFile);
  assertHarness(existsSync(expectedPath),
    `${testCase.id}: ${testCase.expected.markdownFile} exists`);
  if (existsSync(expectedPath)) {
    const body = readFileSync(expectedPath, 'utf8');
    assertHarness(body.includes('EXACT_EXPORT_HEAD_91C4'),
      `${testCase.id}: exported file preserves the early sentinel`);
    assertHarness(body.includes('EXACT_EXPORT_TAIL_91C4'),
      `${testCase.id}: exported file preserves the tail sentinel`);
  }

  const plannerTools = communication.planner?.valid_selected_tools
    || communication.planner?.selected_tools || [];
  const invalidPlannerTools = communication.planner?.invalid_selected_tools || [];
  assertHarness(!invalidPlannerTools.includes('memory_find'),
    `${testCase.id}: planner recognizes memory_find as a registered tool`);
  assertHarness(tools.includes('memory_find') || plannerTools.includes('memory_find'),
    `${testCase.id}: exact retrieval is available despite the planner schema budget`);
  checks.push({ kind: 'behavior', path: round2.bundlePath, checks: { tools, exactArgs } });
  return checks;
}

function verifyFollowupBehavior(testCase, caseSummary) {
  const checks = [];
  const dir = caseAbsDir(testCase);

  // Round 2: terse "as json file" must resolve against the round-1 listing.
  // We accept any *.json under the case dir as evidence the agent produced
  // a file; the hard rule is "do not re-glob *.json to look for existing
  // ones" — that's the failure mode we saw in the chat-UI run.
  const round2 = (caseSummary.rounds || []).find((round) => round.round === 2);
  assertHarness(round2?.bundlePath, `${testCase.id}: round 2 communication bundle exists`);
  const r2 = readJson(round2.bundlePath)?.communication || {};
  const r2Calls = (Array.isArray(r2.tools) ? r2.tools : [])
    .filter((event) => event.kind === 'tool_call');
  const r2Tools = r2Calls.map((event) => event.tool);
  assertHarness(r2Tools.includes('write_file'),
    `${testCase.id}: round 2 writes a JSON file (got tools: ${r2Tools.join(',') || 'none'})`);
  const r2GlobJson = r2Calls.some((event) =>
    event.tool === 'file_glob' && /\.json\b/i.test(JSON.stringify(event.detail?.arguments || {})));
  assertHarness(!r2GlobJson,
    `${testCase.id}: round 2 does NOT glob *.json (would mean memory miss)`);

  const writeArgs2 = r2Calls.filter((event) => event.tool === 'write_file')
    .map((event) => String(event.detail?.arguments?.path || event.detail?.arguments?.filename || ''));
  const jsonWritten = writeArgs2.find((p) => /\.json$/i.test(p));
  assertHarness(!!jsonWritten,
    `${testCase.id}: round 2 wrote a *.json file (got write_file args: ${writeArgs2.join(' | ') || 'none'})`);

  // The round-1 listing facts (filenames) must end up inside the JSON we wrote.
  // We don't pin the schema — just that the file mentions at least one JPG.
  if (jsonWritten) {
    const onDisk = jsonWritten.includes(':') ? jsonWritten : join(dir, jsonWritten);
    if (existsSync(onDisk)) {
      const body = readFileSync(onDisk, 'utf8');
      assertHarness(/DSC05575|flux-5_resized/i.test(body),
        `${testCase.id}: round 2 JSON file mentions a fixture JPG (carried via memory)`);
    }
  }

  // Round 3: "now as markdown report" — must consult the JSON from round 2,
  // either via file_read of the session item OR via memory_read; must not
  // re-glob.
  const round3 = (caseSummary.rounds || []).find((round) => round.round === 3);
  assertHarness(round3?.bundlePath, `${testCase.id}: round 3 communication bundle exists`);
  const r3 = readJson(round3.bundlePath)?.communication || {};
  const r3Calls = (Array.isArray(r3.tools) ? r3.tools : [])
    .filter((event) => event.kind === 'tool_call');
  const r3Tools = r3Calls.map((event) => event.tool);
  assertHarness(r3Tools.includes('write_file'),
    `${testCase.id}: round 3 writes a Markdown report (tools: ${r3Tools.join(',') || 'none'})`);
  const writeArgs3 = r3Calls.filter((event) => event.tool === 'write_file')
    .map((event) => String(event.detail?.arguments?.path || event.detail?.arguments?.filename || ''));
  const mdWritten = writeArgs3.find((p) => /\.(md|markdown)$/i.test(p));
  assertHarness(!!mdWritten,
    `${testCase.id}: round 3 wrote a *.md file (got: ${writeArgs3.join(' | ') || 'none'})`);

  const r3System = roleContent(r3.main_agent?.messages, 'system');
  assertHarness(/## Session Items/i.test(r3System),
    `${testCase.id}: round 3 system prompt includes session items`);
  // Either the JSON file or its role/content_type must appear so we know the
  // round-2 artifact was actually carried forward.
  assertHarness(/\.json\b/i.test(r3System) || /json_data|directory_listing/i.test(r3System),
    `${testCase.id}: round 3 system prompt references the round-2 JSON item`);

  checks.push({ kind: 'behavior', path: round3.bundlePath, checks: { r2Tools, r3Tools } });

  // Round 4: "also as report.txt" — same stem as round-3's report.md.
  // This is the regression test for the artifact-id collision bug:
  // pre-fix, both files mapped to id `report_markdown_document` and the
  // .md item was silently overwritten by the .txt item, losing history.
  const round4 = (caseSummary.rounds || []).find((round) => round.round === 4);
  if (round4) {
    assertHarness(round4.bundlePath, `${testCase.id}: round 4 communication bundle exists`);
    const r4 = readJson(round4.bundlePath)?.communication || {};
    const r4Calls = (Array.isArray(r4.tools) ? r4.tools : [])
      .filter((event) => event.kind === 'tool_call');
    const r4Tools = r4Calls.map((event) => event.tool);
    assertHarness(r4Tools.includes('write_file'),
      `${testCase.id}: round 4 writes report.txt (got tools: ${r4Tools.join(',') || 'none'})`);

    const r4Writes = r4Calls.filter((event) => event.tool === 'write_file')
      .map((event) => String(event.detail?.arguments?.path || ''));
    assertHarness(r4Writes.some((p) => /report\.txt$/i.test(p)),
      `${testCase.id}: round 4 wrote report.txt (got: ${r4Writes.join(' | ') || 'none'})`);

    // The session memory after round 4 must contain BOTH the .md (from round 3)
    // and the .txt (from round 4) as distinct items.
    //
    // We collect *file* entries across every round's memory_update patch.
    // Patches may also carry partial LRU-bump entries (just an id + new
    // last_used_at) which would clobber the real kind=file record if we
    // used a naive Object.assign — so we keep entries with kind === 'file'
    // and we keep the *first* full record per id (chronologically the
    // earliest, when the artifact was created).
    const allItems = {};
    for (const r of caseSummary.rounds || []) {
      if (!r?.bundlePath) continue;
      const bundle = readJson(r.bundlePath)?.communication || {};
      for (const u of bundle.memory_updates || []) {
        const items = u?.patch?.items;
        if (!items || typeof items !== 'object') continue;
        for (const [id, it] of Object.entries(items)) {
          if (it?.kind === 'file' && !allItems[id]) {
            allItems[id] = it;
          }
        }
      }
    }
    const fileItems = Object.values(allItems);
    const reportItems = fileItems.filter((it) => {
      const p = String(it?.locator?.path || '');
      return /[\/\\]report\.(md|txt)$/i.test(p);
    });
    const reportExts = new Set(reportItems.map((it) =>
      String(it?.locator?.path || '').toLowerCase().replace(/.*\./, '.')));
    assertHarness(reportExts.has('.md') && reportExts.has('.txt'),
      `${testCase.id}: same-stem collision: both report.md and report.txt are distinct session items (got ${[...reportExts].join(',') || 'none'})`);

    // IDs must be unique. Pre-fix both items collapsed onto a single id.
    const reportIds = new Set(reportItems.map((it) => String(it?.id || '')));
    assertHarness(reportIds.size === reportItems.length && reportItems.length >= 2,
      `${testCase.id}: same-stem collision: report items have distinct IDs (got ${[...reportIds].join(',') || 'none'})`);

    checks.push({ kind: 'behavior', path: round4.bundlePath, checks: { r4Tools, reportIds: [...reportIds] } });
  }
  return checks;
}

function commandItems(doc) {
  const groups = doc?.ribbon?.groups;
  if (!Array.isArray(groups)) return [];
  const out = [];
  for (const group of groups) {
    for (const item of group.items || []) {
      out.push({ group: group.label, item });
    }
  }
  return out;
}

function verifyCreateCommandBehavior(testCase, caseSummary) {
  const checks = [];
  const round = (n) => (caseSummary.rounds || []).find((r) => r.round === n);
  const docPath = join(caseAbsDir(testCase), testCase.options.configDir, 'commands.json');
  assertHarness(existsSync(docPath), `${testCase.id}: isolated commands.json exists`);
  const doc = readJson(docPath);
  const items = commandItems(doc);
  const byLabel = (label) => items.find((row) => row.item?.label === label);

  const r1 = round(1);
  const internalEnv = toolResultEnvelope(r1?.bundlePath, 'create_command');
  assertHarness(internalEnv?.ok === true && internalEnv?.type === 'internal',
    `${testCase.id}: round 1 create_command internal ok (got ${JSON.stringify(internalEnv)?.slice(0, 240)})`);
  const internal = byLabel('Harness Commands');
  assertHarness(internal?.group === 'Existing',
    `${testCase.id}: internal command landed in Existing`);
  assertHarness(internal?.item?.cliCommand === 'commands' && internal?.item?.userData?.createdBy === 'create_command',
    `${testCase.id}: internal entry keeps cliCommand and createdBy`);
  assertHarness(JSON.stringify(internal?.item?.args) === JSON.stringify(['--json']),
    `${testCase.id}: internal args are ["--json"]`);

  const r2 = round(2);
  const shellEnv = toolResultEnvelope(r2?.bundlePath, 'create_command');
  assertHarness(shellEnv?.ok === true && shellEnv?.type === 'shell' && shellEnv?.interpreter === 'pwsh',
    `${testCase.id}: round 2 shell ok with pwsh (got ${JSON.stringify(shellEnv)?.slice(0, 240)})`);
  const shell = byLabel('Harness Shell');
  assertHarness(shell?.group === 'AI Commands' && shellEnv?.group_created === true,
    `${testCase.id}: omitted group created AI Commands`);
  const ext = shell?.item?.externalCommand || {};
  assertHarness(ext.mode === 'shell' && ext.interpreter === 'pwsh' && ext.shellLine === 'Write-Output harness',
    `${testCase.id}: shell entry stores interpreter and shellLine verbatim`);
  assertHarness(ext.command === '' && Array.isArray(ext.args) && ext.args.length === 0,
    `${testCase.id}: shell entry does not invent an argv command`);

  const r3 = round(3);
  const exeEnv = toolResultEnvelope(r3?.bundlePath, 'create_command');
  assertHarness(exeEnv?.ok === true && exeEnv?.type === 'external',
    `${testCase.id}: round 3 external ok (got ${JSON.stringify(exeEnv)?.slice(0, 240)})`);
  const where = byLabel('Harness Where');
  assertHarness(/where\.exe$/i.test(String(where?.item?.externalCommand?.command || '')),
    `${testCase.id}: external command persisted the where.exe path`);
  assertHarness(where?.item?.externalCommand?.mode === 'argv',
    `${testCase.id}: external entry is argv mode`);

  const r4 = round(4);
  const envReject = toolResultEnvelope(r4?.bundlePath, 'create_command');
  assertHarness(envReject?.ok === false && /ENV/i.test(String(envReject?.error || '')),
    `${testCase.id}: round 4 rejects \${ENV:} (got ${JSON.stringify(envReject)?.slice(0, 240)})`);
  assertHarness(!byLabel('Harness Env'), `${testCase.id}: rejected ENV command was not saved`);

  const r5 = round(5);
  const groupReject = toolResultEnvelope(r5?.bundlePath, 'create_command');
  assertHarness(groupReject?.ok === false && /does not exist/i.test(String(groupReject?.error || '')),
    `${testCase.id}: round 5 rejects a missing group (got ${JSON.stringify(groupReject)?.slice(0, 240)})`);
  assertHarness(!byLabel('Harness Missing Group'), `${testCase.id}: missing-group command was not saved`);

  const r6 = round(6);
  const events = readJson(r6?.bundlePath)?.communication?.tools || [];
  const results = events.filter((e) => e.kind === 'tool_result' && e.tool === 'create_command');
  assertHarness(results.length === 2, `${testCase.id}: round 6 has two create_command results`);
  const created = results[0]?.detail?.envelope ?? results[0]?.detail;
  const dup = results[1]?.detail?.envelope ?? results[1]?.detail;
  assertHarness(created?.ok === true && created?.group === 'Harness Group' && created?.group_created === true,
    `${testCase.id}: create_group added Harness Group (got ${JSON.stringify(created)?.slice(0, 240)})`);
  assertHarness(dup?.ok === false && /already exists/i.test(String(dup?.error || '')),
    `${testCase.id}: duplicate label is rejected (got ${JSON.stringify(dup)?.slice(0, 240)})`);
  const createdCount = items.filter((row) => row.item?.label === 'Harness Commands').length;
  assertHarness(createdCount === 1, `${testCase.id}: duplicate label did not add a second item`);

  checks.push({ kind: 'create-command', items: items.map((row) => row.item?.label) });
  return checks;
}

// case-commands: planner-tuning probe for natural-language asks that map to
// registered local custom commands (custom.mic-status, custom.video-status).
// The contract we want to hold:
//   - planner picks the `run` tool (custom-command ids must NOT appear in
//     selected_tools; the planner-system prompt already forbids this).
//   - main agent invokes `run` with a `tanit-cli custom.<id>` command.
//   - status calls only — no start/stop side effects.
// We deliberately keep this lightweight: we don't pin info_lookup either way
// (the agent may legitimately look up syntax first) and we accept any shell
// quoting for the command string. Failure modes we want to catch:
//   - planner picks info_lookup *alone* and the agent never gets to run.
//   - agent globs/reads files trying to satisfy a shell-command request.
//   - agent ignores "don't start/stop" and triggers the recorder.
function verifyCommandsBehavior(testCase, caseSummary) {
  const checks = [];

  const round1 = (caseSummary.rounds || []).find((round) => round.round === 1);
  assertHarness(round1?.bundlePath, `${testCase.id}: round 1 communication bundle exists`);
  const r1 = readJson(round1.bundlePath)?.communication || {};

  // Planner: `run` should be in selected_tools. We don't insist `info_lookup`
  // is absent — the agent may look up exact syntax — but we do insist that
  // `run` made it through so the schema is expanded for the main call.
  const r1Selected = Array.isArray(r1.planner?.meta?.selected_tools)
    ? r1.planner.meta.selected_tools
    : (Array.isArray(r1.planner?.meta?.valid_selected_tools)
      ? r1.planner.meta.valid_selected_tools
      : []);
  assertHarness(r1Selected.includes('run'),
    `${testCase.id}: round 1 planner selects run (got: ${r1Selected.join(',') || 'none'})`);
  // Custom command ids must not leak into selected_tools.
  const leaked = r1Selected.filter((t) => /^custom\./i.test(String(t)));
  assertHarness(leaked.length === 0,
    `${testCase.id}: round 1 planner did not leak custom.* into selected_tools (got: ${leaked.join(',') || 'none'})`);

  const r1Calls = (Array.isArray(r1.tools) ? r1.tools : [])
    .filter((event) => event.kind === 'tool_call');
  const r1RunCmds = r1Calls
    .filter((event) => event.tool === 'run')
    .map((event) => String(event.detail?.arguments?.command || ''));
  assertHarness(r1RunCmds.length > 0,
    `${testCase.id}: round 1 invokes the run tool at least once ` +
    `(saw tools: ${r1Calls.map((e) => e.tool).join(',') || 'none'})`);
  // R1 is a mic-status query. Accept either the canonical custom.mic-status,
  // or a built-in audio-side status invocation. We just demand the agent
  // went through {slug}-cli and the command line clearly references mic
  // or audio recording state (no Get-Process / WASAPI fishing).
  const r1MicHit = r1RunCmds.some((cmd) =>
    new RegExp(brand.APP_ID_SLUG + '-cli', 'i').test(cmd)
    && (/\bcustom\.mic-status\b/i.test(cmd) || /\b(mic|audio)\b.*\b(status|state|info|record)\b/i.test(cmd)));
  assertHarness(r1MicHit,
    `${testCase.id}: round 1 run command queries mic state via ${brand.APP_ID_SLUG}-cli ` +
    `(got: ${r1RunCmds.join(' | ') || 'none'})`);
  // Prompt forbids start/stop; flag the bad invocation by name.
  const r1Bad = r1RunCmds.find((cmd) => /\bmic-(start|stop)\b/i.test(cmd));
  assertHarness(!r1Bad,
    `${testCase.id}: round 1 must not start or stop the microphone (saw: ${r1Bad || ''})`);

  // Round 2: terse follow-up "and the video?" — same shape as R1, video side.
  const round2 = (caseSummary.rounds || []).find((round) => round.round === 2);
  assertHarness(round2?.bundlePath, `${testCase.id}: round 2 communication bundle exists`);
  const r2 = readJson(round2.bundlePath)?.communication || {};
  const r2Calls = (Array.isArray(r2.tools) ? r2.tools : [])
    .filter((event) => event.kind === 'tool_call');
  const r2RunCmds = r2Calls
    .filter((event) => event.tool === 'run')
    .map((event) => String(event.detail?.arguments?.command || ''));
  assertHarness(r2RunCmds.length > 0,
    `${testCase.id}: round 2 invokes the run tool ` +
    `(saw tools: ${r2Calls.map((e) => e.tool).join(',') || 'none'})`);
  const r2VideoHit = r2RunCmds.some((cmd) =>
    new RegExp(brand.APP_ID_SLUG + '-cli', 'i').test(cmd)
    && (/\bcustom\.video-status\b/i.test(cmd) || /\bvideo\b.*\b(status|state|info|record)\b/i.test(cmd)));
  assertHarness(r2VideoHit,
    `${testCase.id}: round 2 run command queries video state via ${brand.APP_ID_SLUG}-cli ` +
    `(got: ${r2RunCmds.join(' | ') || 'none'})`);
  const r2Bad = r2RunCmds.find((cmd) => /\bvideo-(start|stop)\b/i.test(cmd));
  assertHarness(!r2Bad,
    `${testCase.id}: round 2 must not start or stop video (saw: ${r2Bad || ''})`);

  checks.push({
    kind: 'behavior',
    path: round2.bundlePath,
    checks: { r1Selected, r1RunCmds, r2RunCmds },
  });

  // Round 3: "record me 5 sec of audio then stop".
  // The agent has two valid paths:
  //   (a) custom.mic-start → wait → custom.mic-stop  (registered surface, multi-step)
  //   (b) built-in `audio record --duration-ms 5000` (one-shot, duration-aware)
  // Both honour the request; we accept either. The real multi-runtool stress
  // test lives in the parallel mic+video round, where (b) is unavailable.
  // What we DO refuse:
  //   - hallucinated custom leaf ids (custom.mic.is-recording, custom.mic-halt, …)
  //   - inventing PowerShell `Get-Process` style probes instead of the host CLI
  // Wall time is intentionally not asserted.
  const round3 = (caseSummary.rounds || []).find((round) => round.round === 3);
  if (round3?.bundlePath) {
    const r3 = readJson(round3.bundlePath)?.communication || {};
    const r3Calls = (Array.isArray(r3.tools) ? r3.tools : [])
      .filter((event) => event.kind === 'tool_call');
    const r3RunCmds = r3Calls
      .filter((event) => event.tool === 'run')
      .map((event) => String(event.detail?.arguments?.command || ''));
    assertHarness(r3RunCmds.length > 0,
      `${testCase.id}: round 3 invokes the run tool ` +
      `(saw tools: ${r3Calls.map((e) => e.tool).join(',') || 'none'})`);

    const cliPat = new RegExp(brand.APP_ID_SLUG + '-cli', 'i');
    const usedStartStop =
      r3RunCmds.some((cmd) => cliPat.test(cmd) && /\bcustom\.mic-start\b/i.test(cmd))
      && r3RunCmds.some((cmd) => cliPat.test(cmd) && /\bcustom\.mic-stop\b/i.test(cmd));
    const usedAudioRecord = r3RunCmds.some((cmd) =>
      cliPat.test(cmd) && /\baudio\s+record\b/i.test(cmd));
    assertHarness(usedStartStop || usedAudioRecord,
      `${testCase.id}: round 3 records 5s of audio via either ` +
      `custom.mic-start + custom.mic-stop or built-in 'audio record' ` +
      `(got: ${r3RunCmds.join(' | ') || 'none'})`);

    // Flag hallucinated leaf names that aren't in the shipped catalog.
    const hallucinated = r3RunCmds.find((cmd) =>
      /custom\.mic\.[a-z-]+/i.test(cmd)
      || /custom\.mic-(begin|halt|cease|terminate|kill|abort|is-recording)/i.test(cmd));
    assertHarness(!hallucinated,
      `${testCase.id}: round 3 must use shipped command ids (custom.mic-start/-stop ` +
      `or the audio builtin), not hallucinated variants (saw: ${hallucinated || ''})`);

    checks.push({
      kind: 'behavior',
      path: round3.bundlePath,
      checks: {
        r3RunCmds,
        r3Tools: r3Calls.map((e) => e.tool),
        path: usedStartStop ? 'custom.mic-start/stop' : (usedAudioRecord ? 'audio record builtin' : 'unknown'),
      },
    });
  }

  // Round 4: "do the same but with video on top — both at the same time for 5 sec".
  // There is intentionally NO single PM-Image builtin for "record audio AND
  // video together for N seconds", so the agent must compose multiple ops.
  // We accept any of:
  //   (a) custom.mic-start  + custom.video-start + … + custom.video-stop + custom.mic-stop
  //   (b) audio  record … + video  record …      (two duration-aware builtins)
  //   (c) any mix of the above (e.g. background jobs in PowerShell)
  // What we require:
  //   - some tanit-cli invocation targeting the audio side
  //   - some tanit-cli invocation targeting the video side
  //   - at least two run-tool calls OR one chained shell line containing both
  //   - no Get-Process / WASAPI / sound-device fishing
  //   - no hallucinated leaf ids
  const round4 = (caseSummary.rounds || []).find((round) => round.round === 4);
  if (round4?.bundlePath) {
    const r4 = readJson(round4.bundlePath)?.communication || {};
    const r4Calls = (Array.isArray(r4.tools) ? r4.tools : [])
      .filter((event) => event.kind === 'tool_call');
    const r4RunCmds = r4Calls
      .filter((event) => event.tool === 'run')
      .map((event) => String(event.detail?.arguments?.command || ''));
    assertHarness(r4RunCmds.length > 0,
      `${testCase.id}: round 4 invokes the run tool ` +
      `(saw tools: ${r4Calls.map((e) => e.tool).join(',') || 'none'})`);

    const isPmImage = (cmd) => new RegExp(brand.APP_ID_SLUG + '-cli', 'i').test(cmd);
    const isAudio = (cmd) => /\bcustom\.mic-(start|stop)\b/i.test(cmd) || /\baudio\s+record\b/i.test(cmd);
    const isVideo = (cmd) => /\bcustom\.video-(start|stop)\b/i.test(cmd) || /\bvideo\s+record\b/i.test(cmd);

    const audioHit = r4RunCmds.some((cmd) => isPmImage(cmd) && isAudio(cmd));
    const videoHit = r4RunCmds.some((cmd) => isPmImage(cmd) && isVideo(cmd));
    assertHarness(audioHit,
      `${testCase.id}: round 4 invokes the audio side (custom.mic-start/-stop or audio record) ` +
      `(got: ${r4RunCmds.join(' | ') || 'none'})`);
    assertHarness(videoHit,
      `${testCase.id}: round 4 invokes the video side (custom.video-start/-stop or video record) ` +
      `(got: ${r4RunCmds.join(' | ') || 'none'})`);

    // Multi-runtool evidence: either >= 2 separate run calls, or a single
    // run call whose command line composes both audio and video.
    const composed = r4RunCmds.some((cmd) => isAudio(cmd) && isVideo(cmd));
    assertHarness(r4RunCmds.length >= 2 || composed,
      `${testCase.id}: round 4 must compose multiple ops — either >=2 run calls ` +
      `or a single chained command covering both sides ` +
      `(got ${r4RunCmds.length} run call(s): ${r4RunCmds.join(' | ') || 'none'})`);

    // Hard nope on Get-Process / WASAPI / sound-device fishing.
    const adHoc = r4RunCmds.find((cmd) =>
      /\bGet-Process\b/i.test(cmd)
      || /WASAPI|MMDevice|AudioSession|SoundRecorder|MediaCapture/i.test(cmd));
    assertHarness(!adHoc,
      `${testCase.id}: round 4 must not fish via PowerShell / Windows APIs when ` +
      `registered commands cover the intent (saw: ${adHoc || ''})`);

    // Hallucinated leaf names on either side.
    const hallucinated = r4RunCmds.find((cmd) =>
      /custom\.(mic|video)\.[a-z-]+/i.test(cmd)
      || /custom\.(mic|video)-(begin|halt|cease|terminate|kill|abort|is-recording)/i.test(cmd));
    assertHarness(!hallucinated,
      `${testCase.id}: round 4 must use shipped command ids, not hallucinated variants ` +
      `(saw: ${hallucinated || ''})`);

    // Diagnose sequential vs. parallel from command shape (event bundles do
    // not carry per-event start timestamps). Sequential is acceptable. What
    // we will NOT accept is the agent claiming "simultaneously" / "in parallel"
    // / "at the same time" in its reply when the actual commands were issued
    // sequentially with no backgrounding mechanic.
    const isBackgroundShape = (cmd) =>
      /\bStart-Job\b|\bStart-ThreadJob\b|\bStart-Process\b|\bWait-Job\b|\bGet-Job\b/i.test(cmd)
      || /&\s*\{/.test(cmd)              // & { ... } background script-block
      || /\|\s*Out-Null\s*$/i.test(cmd)  // ... | Out-Null on its own
      || /\s&\s*$/.test(cmd)             // trailing & (unix-ish, sometimes used)
      ;
    const anyBackground = r4RunCmds.some(isBackgroundShape);
    const composition = composed
      ? (anyBackground ? 'composed-parallel' : 'composed-sequential')
      : (r4RunCmds.length >= 2
        ? (anyBackground ? 'parallel-runs' : 'sequential-runs')
        : 'single');

    const finalText = String(r4.final_text || '');
    const claimsParallel = /\b(simultaneous(ly)?|in\s+parallel|at\s+the\s+same\s+time|at\s+once|concurrently)\b/i.test(finalText);
    if (claimsParallel && (composition === 'sequential-runs' || composition === 'composed-sequential')) {
      assertHarness(false,
        `${testCase.id}: round 4 reply claims parallel/simultaneous execution but ` +
        `the actual run commands had no backgrounding mechanic (Start-Job / Start-Process / etc.) — ` +
        `agent must report what it actually did. ` +
        `Commands: ${r4RunCmds.join(' | ')} | reply: "${finalText.slice(0, 200).replace(/\s+/g, ' ')}"`);
    }

    checks.push({
      kind: 'behavior',
      path: round4.bundlePath,
      checks: {
        r4RunCmds,
        r4Tools: r4Calls.map((e) => e.tool),
        runCount: r4RunCmds.length,
        composition,
        claimsParallel,
      },
    });
  }

  return checks;
}

// case-agent-cli: root agent must dispatch a child `{slug}-cli llm agent`
// process, instruct it to reply silently (raw content only), then act on the
// scraped stdout locally. R2 additionally requires parallel dispatch of two
// children with a real backgrounding mechanic.
function verifyAgentCliBehavior(testCase, caseSummary) {
  const checks = [];
  const cliSlug = brand.APP_ID_SLUG + '-cli';

  // Helpers shared across rounds.
  const isLlmAgentInvocation = (cmd) =>
    new RegExp(
      cliSlug.replace(/-/g, '-') + '(?:\\.exe)?(?:\\s+[^|;]*\\bllm\\s+agent\\b|[^|;]*-ArgumentList[^|;]*["\']llm["\'][^|;]*["\']agent["\'])',
      'i',
    ).test(String(cmd || ''));
  const countLlmAgentInvocations = (cmd) => {
    const s = String(cmd || '');
    const directPat = new RegExp(cliSlug + '(?:\\.exe)?\\s+[^|;]*\\bllm\\s+agent\\b', 'gi');
    const direct = (s.match(directPat) || []).length;
    const splitPat = new RegExp(
      'Start-Process\\s+["\']' + cliSlug + '(?:\\.exe)?["\'][\\s\\S]*?-ArgumentList[\\s\\S]*?["\']llm["\'][\\s\\S]*?["\']agent["\']',
      'gi',
    );
    const splitArgumentList = (s.match(splitPat) || []).length;
    return direct + splitArgumentList;
  };
  const commandHasArg = (cmd, arg, value) => {
    const escapedArg = arg.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    const escapedValue = value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    const joined = new RegExp(`${escapedArg}\\s+${escapedValue}\\b`, 'i');
    const split = new RegExp(`${escapedArg}["']?\\s*,\\s*["']${escapedValue}["']`, 'i');
    return joined.test(String(cmd || '')) || split.test(String(cmd || ''));
  };
  const isBackgroundShape = (cmd) =>
    /\bStart-Job\b|\bStart-ThreadJob\b|\bStart-Process\b|\bWait-Job\b|\bGet-Job\b|\bReceive-Job\b/i.test(cmd)
    || /&\s*\{/.test(cmd)
    || /\s&\s*$/.test(cmd);
  const toolResultText = (events) => (Array.isArray(events) ? events : [])
    .filter((event) => event.kind === 'tool_result')
    .map((event) => [
      event.detail?.envelope?.stdout,
      event.detail?.envelope?.stderr,
      event.detail?.envelope?.error,
    ].filter(Boolean).join('\n'))
    .join('\n');

  // ── Round 1 ────────────────────────────────────────────────────────────
  // run with tanit-cli llm agent → arithmetic answer.
  const round1 = (caseSummary.rounds || []).find((round) => round.round === 1);
  assertHarness(round1?.bundlePath, `${testCase.id}: round 1 communication bundle exists`);
  const r1 = readJson(round1.bundlePath)?.communication || {};

  // We don't pin the planner's `selected_tools` here: both `run` and
  // `info_lookup` are always-available base path tools, so the planner
  // doesn't strictly need to expand their schemas for the agent to call
  // them. What we actually care about is the agent's real tool usage
  // downstream — that's checked against `r1Tools` below.
  const r1Selected = Array.isArray(r1.planner?.meta?.selected_tools)
    ? r1.planner.meta.selected_tools
    : (Array.isArray(r1.planner?.meta?.valid_selected_tools)
      ? r1.planner.meta.valid_selected_tools
      : []);

  const r1Calls = (Array.isArray(r1.tools) ? r1.tools : [])
    .filter((event) => event.kind === 'tool_call');
  const r1Tools = r1Calls.map((event) => event.tool);
  assertHarness(!r1Tools.includes('info_lookup'),
    `${testCase.id}: round 1 should not spend meta lookup calls; command shape is known ` +
    `(saw tools: ${r1Tools.join(',') || 'none'})`);
  assertHarness(r1Tools.includes('run'),
    `${testCase.id}: round 1 spawns the child via the run tool ` +
    `(saw tools: ${r1Tools.join(',') || 'none'})`);

  const r1RunCmds = r1Calls
    .filter((event) => event.tool === 'run')
    .map((event) => String(event.detail?.arguments?.command || ''));
  const r1ChildCmd = r1RunCmds.find(isLlmAgentInvocation);
  assertHarness(r1ChildCmd,
    `${testCase.id}: round 1 run command targets \`${cliSlug} llm agent\` ` +
    `(got: ${r1RunCmds.join(' | ') || 'none'})`);
  assertHarness(/1\s*\+\s*1/i.test(r1ChildCmd),
    `${testCase.id}: round 1 child --prompt asks 1+1 ` +
    `(got: ${r1ChildCmd})`);
  assertHarness(new RegExp(`--preset\\s+${CHILD_AGENT_PRESET}`, 'i').test(r1ChildCmd),
    `${testCase.id}: round 1 child uses --preset ${CHILD_AGENT_PRESET} (got: ${r1ChildCmd})`);
  assertHarness(/--single-turn\b/i.test(r1ChildCmd),
    `${testCase.id}: round 1 child is single-turn (got: ${r1ChildCmd})`);
  assertHarness(/--log-level\s+error\b/i.test(r1ChildCmd),
    `${testCase.id}: round 1 child uses quiet logging (got: ${r1ChildCmd})`);
  const r1Evidence = `${toolResultText(r1.tools)}\n${r1.final_text || r1.main_agent?.final_text || ''}`;
  assertHarness(/\b2\b/.test(r1Evidence),
    `${testCase.id}: round 1 child answer contains 2 (evidence: "${r1Evidence.slice(0, 240).replace(/\s+/g, ' ')}")`);

  checks.push({
    kind: 'behavior',
    path: round1.bundlePath,
    checks: {
      r1Selected,
      r1Tools,
      r1RunCmds,
      childCmd: r1ChildCmd,
    },
  });

  // ── Round 2 ────────────────────────────────────────────────────────────
  // Two tanit-cli llm agent calls in parallel via a real backgrounding
  // mechanic.
  const round2 = (caseSummary.rounds || []).find((round) => round.round === 2);
  assertHarness(round2?.bundlePath, `${testCase.id}: round 2 communication bundle exists`);
  const r2 = readJson(round2.bundlePath)?.communication || {};

  // Same reasoning as R1: don't pin the planner's exact selected_tools.
  // The agent's actual `run` invocations are checked below.
  const r2Selected = Array.isArray(r2.planner?.meta?.selected_tools)
    ? r2.planner.meta.selected_tools
    : (Array.isArray(r2.planner?.meta?.valid_selected_tools)
      ? r2.planner.meta.valid_selected_tools
      : []);

  const r2Calls = (Array.isArray(r2.tools) ? r2.tools : [])
    .filter((event) => event.kind === 'tool_call');
  const r2Tools = r2Calls.map((event) => event.tool);
  assertHarness(!r2Tools.includes('info_lookup'),
    `${testCase.id}: round 2 should not spend meta lookup calls; command shape is known ` +
    `(saw tools: ${r2Tools.join(',') || 'none'})`);
  const r2RunCmds = r2Calls
    .filter((event) => event.tool === 'run')
    .map((event) => String(event.detail?.arguments?.command || ''));
  assertHarness(r2RunCmds.length > 0,
    `${testCase.id}: round 2 invokes the run tool ` +
    `(saw tools: ${r2Calls.map((e) => e.tool).join(',') || 'none'})`);

  // Count llm-agent invocations across all run commands. Two are required —
  // either as two run calls or as one chained run call containing two
  // invocations.
  const totalChildInvocations = r2RunCmds.reduce(
    (sum, cmd) => sum + countLlmAgentInvocations(cmd), 0);
  assertHarness(totalChildInvocations >= 2,
    `${testCase.id}: round 2 must spawn two \`${cliSlug} llm agent\` children ` +
    `(saw ${totalChildInvocations} invocation(s): ${r2RunCmds.join(' | ') || 'none'})`);
  for (const cmd of r2RunCmds.filter(isLlmAgentInvocation)) {
    assertHarness(commandHasArg(cmd, '--preset', CHILD_AGENT_PRESET),
      `${testCase.id}: round 2 child uses --preset ${CHILD_AGENT_PRESET} (got: ${cmd})`);
    assertHarness(/--single-turn\b/i.test(cmd),
      `${testCase.id}: round 2 child is single-turn (got: ${cmd})`);
    assertHarness(commandHasArg(cmd, '--log-level', 'error'),
      `${testCase.id}: round 2 child uses quiet logging (got: ${cmd})`);
  }

  // Parallelism: at least one run command (likely the dispatch one) must use
  // a real backgrounding mechanic so the two children actually run together.
  const composedTwoChildren = r2RunCmds.some((cmd) => countLlmAgentInvocations(cmd) >= 2);
  const anyBackground = r2RunCmds.some(isBackgroundShape);
  const parallelEvidence = anyBackground || (composedTwoChildren && /\bStart-Job\b|\bStart-Process\b|\bStart-ThreadJob\b|&\s*\{/i.test(r2RunCmds.join(' ')));
  assertHarness(parallelEvidence,
    `${testCase.id}: round 2 must use a real backgrounding mechanic ` +
    `(Start-Job / Start-ThreadJob / Start-Process / \`& {…}\` + Wait-Job) so the ` +
    `two children run concurrently — sequential spawn defeats the test. ` +
    `Commands: ${r2RunCmds.join(' | ')}`);

  const r2Evidence = `${toolResultText(r2.tools)}\n${r2.final_text || r2.main_agent?.final_text || ''}`;
  assertHarness(/\b2\b/.test(r2Evidence) && /\b4\b/.test(r2Evidence),
    `${testCase.id}: round 2 child answers contain 2 and 4 ` +
    `(evidence: "${r2Evidence.slice(0, 240).replace(/\s+/g, ' ')}")`);

  // Honesty check: reply must not claim "in parallel" if commands were
  // sequential with no backgrounding mechanic.
  const finalText = String(r2.final_text || r2.main_agent?.final_text || '');
  const claimsParallel = /\b(simultaneous(ly)?|in\s+parallel|at\s+the\s+same\s+time|at\s+once|concurrently)\b/i.test(finalText);
  if (claimsParallel && !parallelEvidence) {
    assertHarness(false,
      `${testCase.id}: round 2 reply claims parallel execution but no ` +
      `backgrounding mechanic was used in the run commands ` +
      `(reply: "${finalText.slice(0, 200).replace(/\s+/g, ' ')}")`);
  }

  checks.push({
    kind: 'behavior',
    path: round2.bundlePath,
    checks: {
      r2Selected,
      r2RunCmds,
      childInvocations: totalChildInvocations,
      composedTwoChildren,
      anyBackground,
    },
  });

  return checks;
}

function verifyAppUseBehavior(testCase, caseSummary) {
  const checks = [];
  const round1 = (caseSummary.rounds || []).find((round) => round.round === 1);
  assertHarness(round1?.bundlePath, `${testCase.id}: round 1 communication bundle exists`);
  const r1 = readJson(round1.bundlePath)?.communication || {};
  const calls = (Array.isArray(r1.tools) ? r1.tools : [])
    .filter((event) => event.kind === 'tool_call');
  const tools = calls.map((event) => event.tool);
  const normalizeBatchSteps = (args) => {
    const raw = Array.isArray(args?.steps) ? args.steps : args?.steps
      ?? (Array.isArray(args?.actions) ? args.actions : args?.actions);
    if (Array.isArray(raw)) return raw;
    if (typeof raw === 'string') {
      try {
        const parsed = JSON.parse(raw);
        if (Array.isArray(parsed)) return parsed;
        if (Array.isArray(parsed?.steps)) return parsed.steps;
        if (Array.isArray(parsed?.actions)) return parsed.actions;
      } catch {
        const trimmed = raw.trimStart();
        const end = trimmed.startsWith('[') ? raw.lastIndexOf(']')
          : (trimmed.startsWith('{') ? raw.lastIndexOf('}') : -1);
        if (end >= 0 && end + 1 < raw.length) {
          try {
            const parsed = JSON.parse(raw.slice(0, end + 1));
            if (Array.isArray(parsed)) return parsed;
            if (Array.isArray(parsed?.steps)) return parsed.steps;
            if (Array.isArray(parsed?.actions)) return parsed.actions;
          } catch {}
        }
      }
    }
    return [];
  };
  const batchArgs = calls
    .filter((event) => event.tool === 'app_batch')
    .map((event) => event.detail?.arguments || {});
  const batchSteps = batchArgs.flatMap(normalizeBatchSteps);
  const batchOpenSteps = batchSteps.filter((step) =>
    /^(open-app|open_app)$/i.test(String(step?.action || step?.type || '')));
  const firstBatchIndex = calls.findIndex((event) => event.tool === 'app_batch');
  const firstInspectIndex = calls.findIndex((event) => event.tool === 'app_inspect_dump');
  assertHarness(firstInspectIndex >= 0 && (firstBatchIndex < 0 || firstInspectIndex < firstBatchIndex),
    `${testCase.id}: app_inspect_dump runs before app_batch so UI targets use hwnd/Markdown metadata (got tools: ${tools.join(',') || 'none'})`);

  assertHarness(tools.includes('app_open') || batchOpenSteps.length > 0,
    `${testCase.id}: opens Paint via app_open or app_batch open-app (got tools: ${tools.join(',') || 'none'})`);
  assertHarness(tools.includes('app_batch') || tools.filter((tool) => tool === 'app_drag').length >= 2,
    `${testCase.id}: draws the cross via app_batch or at least two app_drag calls (got tools: ${tools.join(',') || 'none'})`);
  for (const forbidden of ['run', 'write_file', 'file_read', 'file_glob', 'file_search']) {
    assertHarness(!tools.includes(forbidden),
      `${testCase.id}: must not use ${forbidden} for desktop app control (got tools: ${tools.join(',') || 'none'})`);
  }

  const openArgs = calls
    .filter((event) => event.tool === 'app_open')
    .map((event) => event.detail?.arguments || {})
    .concat(batchOpenSteps);
  assertHarness(openArgs.some((args) => /mspaint(?:\.exe)?$/i.test(String(args.exe || args.process || ''))),
    `${testCase.id}: open step targets mspaint.exe (got: ${JSON.stringify(openArgs)})`);

  const screenshotArgs = [
    ...calls
      .filter((event) => event.tool === 'app_screenshot')
      .map((event) => String(event.detail?.arguments?.output_path || event.detail?.arguments?.path || '')),
    ...batchSteps
      .filter((step) => /^(screenshot)$/i.test(String(step?.action || step?.type || '')))
      .map((step) => String(step.output_path || step.output || step.path || step.filename || '')),
  ];
  assertHarness(screenshotArgs.some((p) => /paint-cross-before\.jpe?g$/i.test(p)),
    `${testCase.id}: before screenshot path is paint-cross-before.jpg (got: ${screenshotArgs.join(' | ') || 'none'})`);
  assertHarness(screenshotArgs.some((p) => /paint-cross-after\.jpe?g$/i.test(p)),
    `${testCase.id}: after screenshot path is paint-cross-after.jpg (got: ${screenshotArgs.join(' | ') || 'none'})`);

  const dragArgs = calls
    .filter((event) => event.tool === 'app_drag')
    .map((event) => event.detail?.arguments || {});
  const dragSteps = [
    ...dragArgs,
    ...batchSteps.filter((step) => /^drag$/i.test(String(step?.action || step?.type || ''))),
  ];
  assertHarness(dragSteps.length >= 2,
    `${testCase.id}: at least two separate drag gestures for the cross/X (got ${dragSteps.length}; one polyline is one continuous stroke, not a cross)`);
  const colorEvidence = JSON.stringify(batchSteps);
  const redEvidence = /\bred\b|#f00|#ff0000|rgb\s*\(\s*255\s*,\s*0\s*,\s*0\s*\)|color|brush|pencil|palette/i.test(colorEvidence);
  assertHarness(redEvidence,
    `${testCase.id}: batch includes an attempt to choose red brush/pencil/color (steps=${truncateText(colorEvidence, 600)})`);

  const events = Array.isArray(r1.tools) ? r1.tools : [];
  const batchResults = events.filter((event) => event.kind === 'tool_result' && event.tool === 'app_batch');
  assertHarness(batchResults.some((event) => event.detail?.envelope?.all_ok === true),
    `${testCase.id}: at least one app_batch result reports all_ok=true`);
  const successfulBatch = batchResults.find((event) => event.detail?.envelope?.all_ok === true)?.detail?.envelope || {};
  const resultSteps = Array.isArray(successfulBatch.steps) ? successfulBatch.steps : [];
  const resultScreenshots = resultSteps.filter((step) => step.action === 'screenshot');
  const resultDrags = resultSteps.filter((step) => step.action === 'drag');
  assertHarness(resultScreenshots.length >= 2 && resultScreenshots.every((step) => step.data?.capture === 'window' && step.data?.window?.hwnd),
    `${testCase.id}: batch screenshot results capture a target window`);
  assertHarness(resultDrags.length >= 2 && resultDrags.every((step) => step.data?.target?.hwnd),
    `${testCase.id}: batch drag results include target window metadata`);

  const finalText = String(round1.finalText || r1.main_agent?.final_text || r1.final_text || '');
  assertHarness(/\bDONE\b/i.test(finalText),
    `${testCase.id}: final reply contains DONE (got: ${truncateText(finalText, 200)})`);

  checks.push({
    kind: 'behavior',
    path: round1.bundlePath,
    checks: {
      tools,
      screenshotArgs,
      dragCount: dragSteps.length,
    },
  });
  return checks;
}

// case-ask-user: end-to-end inquirer round-trip via the PM_ASK_USER_REPLY_FILE
// backend. For each round we assert (a) ask_user was the tool the agent called,
// (b) the envelope shape matches the requested `kind`, (c) the reply file
// cursor advanced, and (d) the agent's final reply faithfully reflects the
// pre-seeded answer using the format the prompt asked for.
function verifyAskUserBehavior(testCase, caseSummary) {
  const checks = [];
  const dir = caseAbsDir(testCase);
  const replyFile = join(dir, 'replies.json');
  assertHarness(existsSync(replyFile),
    `${testCase.id}: reply file exists after the run: ${replyFile}`);

  const replyDoc = readJson(replyFile);
  assertHarness(typeof replyDoc?.cursor === 'number' && replyDoc.cursor >= testCase.rounds.length,
    `${testCase.id}: reply cursor advanced past every round (cursor=${replyDoc?.cursor}, rounds=${testCase.rounds.length})`);

  const findCall = (events) => (Array.isArray(events) ? events : [])
    .find((e) => e.kind === 'tool_call' && e.tool === 'ask_user');
  const findResult = (events) => (Array.isArray(events) ? events : [])
    .find((e) => e.kind === 'tool_result' && e.tool === 'ask_user');

  for (const round of caseSummary.rounds || []) {
    assertHarness(round.bundlePath, `${testCase.id}: round ${round.round} bundle path exists`);
    const bundle = readJson(round.bundlePath);
    const communication = bundle?.communication || {};
    const tools = Array.isArray(communication.tools) ? communication.tools : [];

    const call   = findCall(tools);
    const result = findResult(tools);
    assertHarness(call,   `${testCase.id}: round ${round.round} invokes ask_user (got tools: ${tools.map((e) => `${e.kind}:${e.tool}`).join(',') || 'none'})`);
    assertHarness(result, `${testCase.id}: round ${round.round} ask_user returned a tool_result`);

    const env = result?.detail?.envelope || {};
    assertHarness(env.ok === true,
      `${testCase.id}: round ${round.round} envelope.ok===true (got ${JSON.stringify(env).slice(0, 240)})`);
    assertHarness(env.stopped_by === 'scripted',
      `${testCase.id}: round ${round.round} envelope.stopped_by='scripted' (got '${env.stopped_by}')`);
    assertHarness(typeof env.elapsed_ms === 'number',
      `${testCase.id}: round ${round.round} envelope.elapsed_ms is numeric`);

    const finalText = String(round.finalText || communication.main_agent?.final_text || '').trim();

    if (round.round === 1) {
      assertHarness(env.kind === 'text',
        `${testCase.id}: round 1 envelope.kind='text' (got '${env.kind}')`);
      assertHarness(env.answer === 'blueberry',
        `${testCase.id}: round 1 envelope.answer='blueberry' (got '${env.answer}')`);
      assertHarness(/^blueberry\.?$/i.test(finalText),
        `${testCase.id}: round 1 reply is just the answer word (got: "${finalText.slice(0, 120)}")`);
    } else if (round.round === 2) {
      assertHarness(env.kind === 'multi_choice',
        `${testCase.id}: round 2 envelope.kind='multi_choice' (got '${env.kind}')`);
      assertHarness(Array.isArray(env.answer) && Array.isArray(env.answer_indices),
        `${testCase.id}: round 2 envelope has answer[] + answer_indices[]`);
      assertHarness(JSON.stringify(env.answer) === JSON.stringify(['apple', 'orange']),
        `${testCase.id}: round 2 envelope.answer === ['apple','orange'] (got ${JSON.stringify(env.answer)})`);
      assertHarness(JSON.stringify(env.answer_indices) === JSON.stringify([0, 2]),
        `${testCase.id}: round 2 envelope.answer_indices === [0,2] (got ${JSON.stringify(env.answer_indices)})`);
      // Reflect-back: must mention both labels, in input order. Accept any
      // separator the LLM picks rather than pinning a literal "apple,orange"
      // — the round-trip itself was checked above.
      const idxApple  = finalText.toLowerCase().indexOf('apple');
      const idxOrange = finalText.toLowerCase().indexOf('orange');
      assertHarness(idxApple >= 0 && idxOrange >= 0 && idxApple < idxOrange,
        `${testCase.id}: round 2 reply mentions apple then orange (got: "${finalText.slice(0, 120)}")`);
      assertHarness(!/banana/i.test(finalText),
        `${testCase.id}: round 2 reply must not mention banana (got: "${finalText.slice(0, 120)}")`);
    } else if (round.round === 3) {
      assertHarness(env.kind === 'confirm',
        `${testCase.id}: round 3 envelope.kind='confirm' (got '${env.kind}')`);
      assertHarness(env.answer === true,
        `${testCase.id}: round 3 envelope.answer===true (got ${env.answer})`);
      assertHarness(/^yes\.?$/i.test(finalText),
        `${testCase.id}: round 3 reply is 'yes' (got: "${finalText.slice(0, 120)}")`);
    }

    checks.push({
      kind: 'behavior',
      path: round.bundlePath,
      checks: { askUserKind: env.kind, askUserAnswer: env.answer, askUserIndices: env.answer_indices },
    });
  }

  return checks;
}

function verifyChatBehavior(testCase, caseSummary) {
  const checks = [];
  const round3 = (caseSummary.rounds || []).find((round) => round.round === 3);
  assertHarness(round3?.bundlePath, `${testCase.id}: round 3 communication bundle exists`);
  const bundle = readJson(round3.bundlePath);
  const communication = bundle?.communication || {};

  // The recall turn must not reach for any external lookup; `memory_read` is
  // a legitimate way to pull the structured memory back out, so we allow it.
  const tools = Array.isArray(communication.tools) ? communication.tools : [];
  const toolCalls = tools.filter((event) => event.kind === 'tool_call').map((event) => event.tool);
  const externalTools = toolCalls.filter((t) =>
    /^(file_|image_|create_video|web_|info_lookup|run|app_|speak|mcp_)/i.test(t));
  assertHarness(externalTools.length === 0,
    `${testCase.id}: round 3 answers from session memory without external tools (got ${externalTools.join(',') || 'none'})`);

  // Round 3 main-agent system prompt must carry the conversation items, with
  // the user-supplied facts visible to the model.
  const mainSystem = roleContent(communication.main_agent?.messages, 'system');
  assertHarness(/## Session Items/i.test(mainSystem), `${testCase.id}: round 3 system prompt includes items`);
  assertHarness(/kind=conversation/i.test(mainSystem) && /role=turn_pair/i.test(mainSystem),
    `${testCase.id}: round 3 system prompt includes turn_pair items`);
  assertHarness(/Bruno/i.test(mainSystem),
    `${testCase.id}: round 3 system prompt carries the dog-name fact`);
  assertHarness(/Labrador/i.test(mainSystem),
    `${testCase.id}: round 3 system prompt carries the breed fact`);

  // The final answer should mention the facts. We do not assert exact wording,
  // only that the LLM correctly resolved them from structured memory.
  const finalText = String(round3.finalText || communication.main_agent?.final_text || '');
  assertHarness(/Bruno/i.test(finalText), `${testCase.id}: round 3 reply names the dog`);
  assertHarness(/Labrador/i.test(finalText), `${testCase.id}: round 3 reply names the breed`);
  assertHarness(/Quackers/i.test(finalText), `${testCase.id}: round 3 reply names the toy`);

  // Three turn_pair items should land across rounds 1..3.
  const memoryUpdates = Array.isArray(communication.memory_updates) ? communication.memory_updates : [];
  assertHarness(memoryUpdates.some((u) => u?.patch?.items
      && Object.values(u.patch.items).some((it) => it?.role === 'turn_pair')),
    `${testCase.id}: round 3 memory_update includes a turn_pair item`);

  checks.push({ kind: 'behavior', path: round3.bundlePath, checks: { toolCalls, finalText: truncateText(finalText, 200) } });

  // ── Pre-API context reduction wiring (deterministic dry-run; no LLM call) ───
  // Confirms the `--context-reduction` CLI surface flows onto the Turn and shows
  // up in the resolved dry-run plan. The live chat rounds above run with
  // reduction Off, so the recall assertions stay byte-stable on the wire; this
  // sub-check exercises the opt-in path separately and cheaply (--dry-run only
  // resolves provider/tools/selection — no model round-trip, no API key).
  // Gated by the CONTEXT_REDUCTION_* header knobs (default ON).
  if (CONTEXT_REDUCTION_CHECK) {
    const exe = llmExe();
    const caseDir = caseAbsDir(testCase);

    // Default (no flag): the attention reduction layer must stay Off so the wire
    // payload is byte-identical to today unless a caller opts in.
    const offRun = dryRunContextReduction(exe, caseDir, []);
    assertHarness(offRun.status === 0,
      `${testCase.id}: dry-run (no reduction) exits 0 (got ${offRun.status})`);
    assertHarness(offRun.contextReduction?.mode === 'off',
      `${testCase.id}: default context_reduction mode is off (got ${offRun.contextReduction?.mode ?? 'null'})`);

    // Opt-in: only meaningful when the build carries the attention reduction
    // layer (FEATURE_LLM_COMPRESSION). When absent the flag isn't registered, so
    // we keep the off-mode default check above and skip the override assertions.
    if (llmAgentHasContextReductionFlag(exe)) {
      const onRun = dryRunContextReduction(exe, caseDir, [
        '--context-reduction', CONTEXT_REDUCTION_MODE,
        '--context-reduction-ratio', String(CONTEXT_REDUCTION_RATIO),
        '--context-reduction-min-bytes', String(CONTEXT_REDUCTION_MIN_BYTES),
        '--context-reduction-objective', CONTEXT_REDUCTION_OBJECTIVE,
      ]);
      assertHarness(onRun.status === 0,
        `${testCase.id}: dry-run (--context-reduction ${CONTEXT_REDUCTION_MODE}) exits 0 (got ${onRun.status})`);
      const cr = onRun.contextReduction || {};
      assertHarness(cr.mode === CONTEXT_REDUCTION_MODE,
        `${testCase.id}: --context-reduction ${CONTEXT_REDUCTION_MODE} lands on Turn (got ${cr.mode ?? 'null'})`);
      assertHarness(Math.abs(Number(cr.ratio) - CONTEXT_REDUCTION_RATIO) < 1e-9,
        `${testCase.id}: --context-reduction-ratio lands on Turn (got ${cr.ratio})`);
      assertHarness(Number(cr.min_bytes) === CONTEXT_REDUCTION_MIN_BYTES,
        `${testCase.id}: --context-reduction-min-bytes lands on Turn (got ${cr.min_bytes})`);
      assertHarness(cr.objective === CONTEXT_REDUCTION_OBJECTIVE,
        `${testCase.id}: --context-reduction-objective lands on Turn (got ${cr.objective ?? 'null'})`);
      checks.push({ kind: 'context_reduction_wiring', off: offRun.contextReduction, on: cr });
    } else {
      console.log(`  context-reduction: flag not present in this build (FEATURE_LLM_COMPRESSION off) — skipping opt-in checks`);
      checks.push({ kind: 'context_reduction_wiring', off: offRun.contextReduction, on: null, skipped: true });
    }
  }

  return checks;
}

function mdEscape(text) {
  return String(text ?? '').replace(/\|/g, '\\|');
}

function fencedJson(value) {
  return `\`\`\`json\n${JSON.stringify(value ?? null, null, 2)}\n\`\`\``;
}

function fencedText(value) {
  return `\`\`\`text\n${String(value ?? '')}\n\`\`\``;
}

function roleContent(messages, role) {
  const found = Array.isArray(messages) ? messages.find((message) => message?.role === role) : null;
  return found?.content ?? '';
}

function writeCaseSummaryMarkdown(testCase, caseSummary) {
  const totals = caseSummary.totals || totalMetrics(caseSummary.rounds || []);
  const lines = [];
  lines.push(`# ${testCase.id} Summary`);
  lines.push('');
  lines.push(`Goal: ${testCase.goal}`);
  lines.push('');
  lines.push(`CWD: \`${caseSummary.cwd}\``);
  lines.push(`Session: \`${caseSummary.sessionId}\``);
  lines.push('');
  lines.push('| Round | Prompt | Response | Tokens | Cost | Duration |');
  lines.push('|:---:|:---|:---|---:|---:|---:|');
  for (const round of caseSummary.rounds || []) {
    const metrics = round.metrics || {};
    const totalUsage = metrics.usage?.total || {};
    const duration = metrics.duration_ms?.wall ?? 0;
    lines.push([
      round.round,
      mdEscape(round.prompt),
      mdEscape(round.finalText),
      totalUsage.total_tokens ?? 0,
      `$${Number(totalUsage.cost_usd || 0).toFixed(6)}`,
      `${Number(duration || 0).toFixed(2)} ms`,
    ].join(' | '));
  }
  lines.push('');
  lines.push('## Totals');
  lines.push('');
  lines.push(`- Wall time: ${Number(totals.duration_ms?.wall || 0).toFixed(2)} ms`);
  lines.push(`- Planner time: ${Number(totals.duration_ms?.planner || 0).toFixed(2)} ms`);
  lines.push(`- Main estimate: ${Number(totals.duration_ms?.main_estimate || 0).toFixed(2)} ms`);
  lines.push(`- Input tokens: ${totals.usage?.total?.prompt_tokens ?? 0}`);
  lines.push(`- Output tokens: ${totals.usage?.total?.completion_tokens ?? 0}`);
  lines.push(`- Total tokens: ${totals.usage?.total?.total_tokens ?? 0}`);
  lines.push(`- Cost: $${Number(totals.usage?.total?.cost_usd || 0).toFixed(6)}`);
  lines.push('');
  if (Array.isArray(caseSummary.outputs) && caseSummary.outputs.length) {
    lines.push('## Outputs');
    lines.push('');
    for (const output of caseSummary.outputs) {
      lines.push(`- ${output.kind}: \`${output.path}\``);
    }
    lines.push('');
  }
  lines.push('## Round Details');
  lines.push('');
  for (const round of caseSummary.rounds || []) {
    let bundle = null;
    try {
      bundle = readJson(round.bundlePath);
    } catch {
      bundle = null;
    }
    const communication = bundle?.communication || {};
    const planner = communication.planner || {};
    const main = communication.main_agent || {};
    const tools = Array.isArray(communication.tools) ? communication.tools : [];
    const mainSystem = roleContent(main.messages, 'system');

    lines.push(`### Round ${round.round}: ${round.name}`);
    lines.push('');
    lines.push('#### User Prompt');
    lines.push('');
    lines.push(fencedText(round.prompt));
    lines.push('');
    lines.push('#### Planner Context / Prompt');
    lines.push('');
    const plannerMessages = Array.isArray(planner.messages) ? planner.messages : [];
    if (plannerMessages.length) {
      for (const message of plannerMessages) {
        lines.push(`##### Planner ${message.role || 'message'}`);
        lines.push('');
        lines.push(message.role === 'assistant' ? fencedJson(message) : fencedText(message.content));
        lines.push('');
      }
    } else {
      lines.push('_No planner messages captured._');
      lines.push('');
    }
    lines.push('#### Curated System Prompt');
    lines.push('');
    lines.push(mainSystem ? fencedText(mainSystem) : '_No system prompt captured._');
    lines.push('');
    lines.push('#### Tool Calls');
    lines.push('');
    if (tools.length) {
      for (let i = 0; i < tools.length; ++i) {
        const tool = tools[i];
        lines.push(`##### ${i + 1}. ${tool.kind || 'tool'}: ${tool.tool || '(unknown)'}`);
        lines.push('');
        lines.push(fencedJson(tool.detail));
        lines.push('');
      }
    } else {
      lines.push('_No tool calls/results captured._');
      lines.push('');
    }
    lines.push('#### Assistant Response');
    lines.push('');
    lines.push(fencedText(round.finalText));
    lines.push('');
  }
  const summaryPath = join(caseAbsDir(testCase), 'summary.md');
  writeFileSync(summaryPath, `${lines.join('\n')}\n`, 'utf8');
  return summaryPath;
}

async function main() {
  mkdirSync(STEMP_DIR, { recursive: true });
  // Keep the literal default `--cwd ./stemp` and prepare fixture directories
  // inside it for any relative paths the agent/tool layer may resolve.
  mkdirSync(join(STEMP_DIR, 'stemp'), { recursive: true });
  try {
    rmSync(RUN_DIR, { recursive: true, force: true, maxRetries: 3, retryDelay: 200 });
  } catch {
    // On Windows a previous agent process may still hold handles. Continue;
    // individual case files will overwrite their own paths.
  }
  mkdirSync(SESSION_DIR, { recursive: true });

  const exe = llmExe();
  const runId = Date.now().toString(36);

  // `--no-replay` clears the ReplayTranscript bit (=1<<11) from the default
  // structured-memory mask, so the agent runs with structured items + rules
  // only (no raw transcript injection). Pass `--memory-features=0xFFF` to set
  // an explicit mask instead.
  const noReplay = process.argv.includes('--no-replay');
  const maskOverride = process.argv.find((a) => a.startsWith('--memory-features='));
  let envMask;
  if (maskOverride) envMask = maskOverride.slice('--memory-features='.length);
  else if (noReplay) envMask = '0x7FF';

  const env = {
    ...process.env,
    PM_IMAGE_SESSION_DIR: SESSION_DIR,
    PM_IMAGE_DEBUG_DUMPS: '1',
    ...(envMask ? { PM_LLM_STRUCTURED_MEMORY_FEATURES: envMask } : {}),
  };

  const resolvedPlannerPrompt = existsSync(DEFAULT_PLANNER_PROMPT_FILE)
    ? DEFAULT_PLANNER_PROMPT_FILE
    : (ROAMING_PLANNER_PROMPT_FILE && existsSync(ROAMING_PLANNER_PROMPT_FILE) ? ROAMING_PLANNER_PROMPT_FILE : null);

  console.log('agent memory harness');
  console.log(`  exe        : ${exe}`);
  console.log(`  default    : ${DEFAULT_LOG_ARGS.join(' ')} --cwd ${DEFAULT_CWD_ROOT}`);
  console.log(`  run dir    : ${RUN_DIR}`);
  console.log(`  planner    : ${resolvedPlannerPrompt ?? '(binary default)'}`);
  if (envMask) console.log(`  features   : PM_LLM_STRUCTURED_MEMORY_FEATURES=${envMask}`);
  if (STREAMING_SMOKE) console.log('  streaming  : on (--streaming-smoke)');

  const summary = [];
  const caseSummaries = [];
  for (const testCase of selectedCases()) {
    const caseDir = caseAbsDir(testCase);
    mkdirSync(caseDir, { recursive: true });
    // Optional fixture files (e.g. dummy jpgs the agent should discover).
    // Existing files are not overwritten unless `overwrite: true` is set.
    for (const fx of testCase.fixtures?.files || []) {
      const fxPath = join(caseDir, fx.name);
      if (!existsSync(fxPath) || fx.overwrite) {
        writeFileSync(fxPath, fx.content ?? '', 'utf8');
      }
    }
    for (const name of testCase.cleanFiles || []) {
      rmSync(join(caseDir, name), { force: true });
    }
    if (testCase.seedConfig && testCase.options?.configDir) {
      const cfgDir = join(caseDir, testCase.options.configDir);
      mkdirSync(cfgDir, { recursive: true });
      writeFileSync(join(cfgDir, 'commands.json'), JSON.stringify(testCase.seedConfig, null, 2), 'utf8');
    }
    // Optional ask_user reply file. The C++ side reads
    // `PM_ASK_USER_REPLY_FILE` per process; the cursor field in the JSON
    // advances across rounds inside the same case so each round consumes
    // exactly one pre-seeded reply. We always reset to cursor=0 on case
    // setup so re-running the harness is idempotent.
    let caseEnv = env;
    if (testCase.replyFile) {
      const replyPath = join(caseDir, 'replies.json');
      const doc = { cursor: 0, replies: testCase.replyFile.replies || [] };
      writeFileSync(replyPath, JSON.stringify(doc, null, 2), 'utf8');
      caseEnv = { ...env, PM_ASK_USER_REPLY_FILE: replyPath };
      console.log(`  replyFile  : ${replyPath} (${doc.replies.length} replies pre-seeded)`);
    }
    const sessionId = `agent-${testCase.id}-${runId}`;
    const caseSummary = {
      id: testCase.id,
      cwdOffset: testCase.cwdOffset,
      cwd: caseDir,
      goal: testCase.goal,
      sessionId,
      options: testCase.options,
      rounds: [],
    };
    console.log(`\ncase ${testCase.id}`);
    console.log(`  cwd        : ${caseDir}`);
    console.log(`  goal       : ${testCase.goal}`);

    for (const round of testCase.rounds) {
      const logPath = join(RUN_DIR, `${testCase.id}-round-${String(round.index).padStart(2, '0')}-agent.json`);
      const roundMeta = { ...round, caseId: testCase.id };

      // Resolve the planner prompt arg. Case-level override wins; otherwise
      // the live file under stemp/prompts/planner.md is passed via @path so
      // we can iterate on it without recompiling the C++ default. When the
      // file is missing, fall back to the baked-in planner default by
      // skipping the flag entirely (don't pass an empty / bogus path).
      const plannerPromptArg = (() => {
        const o = testCase.options?.plannerPrompt;
        if (typeof o === 'string' && o.trim()) return o;
        if (existsSync(DEFAULT_PLANNER_PROMPT_FILE)) return `@${DEFAULT_PLANNER_PROMPT_FILE}`;
        if (ROAMING_PLANNER_PROMPT_FILE && existsSync(ROAMING_PLANNER_PROMPT_FILE)) return `@${ROAMING_PLANNER_PROMPT_FILE}`;
        return null;
      })();

      const args = [
        ...DEFAULT_LOG_ARGS,
        ...(testCase.options?.configDir
          ? ['--config-dir', join(caseDir, testCase.options.configDir)]
          : []),
        '--cwd', caseCwdArg(testCase),
        'llm', 'agent',
        ...(testCase.options?.noPreset ? [] : ['--preset', testCase.options?.chatPreset || DEFAULT_CHAT_PRESET]),
        '--prompt', round.prompt,
        '--json',
        ...(testCase.options?.noPlanner
          ? ['--no-planner']
          : ['--planner', '--planner-budget', testCase.options?.plannerBudget || '6']),
        ...(plannerPromptArg && !testCase.options?.noPlanner ? ['--planner-prompt', plannerPromptArg] : []),
        '--session-id', round.sessionSuffix ? `${sessionId}${round.sessionSuffix}` : sessionId,
        '--max-iter', testCase.options?.maxIter || '8',
        ...(testCase.options?.router ? ['--router', testCase.options.router] : []),
        ...(testCase.options?.agent || ['--multi-turn']),
        ...(testCase.options?.streaming
          ? ['--streaming', testCase.options.streaming]
          : (STREAMING_SMOKE ? ['--streaming', 'on'] : [])),
        ...(round.agentFlags || []),
        '--log', logPath,
      ];
      const roundEnv = round.envOverrides
        ? { ...caseEnv, ...round.envOverrides }
        : caseEnv;
      const result = runCli(exe, args, { timeout: 180_000, env: roundEnv });
      const stdoutJson = parseJsonObject(result.stdout);
      const { communication, bundlePath } = writeCommunicationBundle(roundMeta, logPath, stdoutJson);
      const metrics = roundMetrics(result, communication);
      printRoundSummary(roundMeta, result, communication, bundlePath);

      // P8: the feature pipeline must be phase-ordered. AgentFactory::default_agent()
      // emits a logger::warn (→ stderr) if any feature is out of order.
      assertHarness(
        !/feature\[.*\].*out of (phase )?order/i.test(result.stderr),
        `${testCase.id} round ${round.index}: P8 phase-order violation detected in stderr ` +
        `(stderr snippet: "${result.stderr.slice(0, 200).replace(/\n/g, '↵')}")`);

      assertHarness(result.status === 0, `${testCase.id} round ${round.index} exited ${result.status}`);
      assertHarness(existsSync(logPath), `${testCase.id} round ${round.index} wrote log ${logPath}`);
      assertHarness(communication.main_agent.messages.length > 0, `${testCase.id} round ${round.index} has main-agent messages`);
      if (!testCase.options?.noPlanner) {
        assertHarness(communication.planner.messages.length >= 2, `${testCase.id} round ${round.index} has planner request messages`);
        assertHarness(
          communication.planner.messages.some((message) => message.role === 'assistant'),
          `${testCase.id} round ${round.index} has planner assistant response`,
        );
      }

      const row = {
        caseId: testCase.id,
        round: round.index,
        name: round.name,
        prompt: round.prompt,
        logPath,
        bundlePath,
        ok: communication.main_agent.ok,
        finalText: communication.main_agent.final_text,
        plannerSelectedTools: communication.planner.meta?.selected_tools || [],
        mainMessageCount: communication.main_agent.messages.length,
        toolEventCount: communication.tools.length,
        metrics,
      };
      summary.push(row);
      caseSummary.rounds.push(row);
    }
    caseSummary.outputs = [
      ...verifyCaseOutputs(testCase),
      ...verifyCaseBehavior(testCase, caseSummary),
    ];
    caseSummary.totals = totalMetrics(caseSummary.rounds);
    caseSummary.summaryMarkdownPath = writeCaseSummaryMarkdown(testCase, caseSummary);
    caseSummaries.push(caseSummary);
  }

  const summaryPath = join(RUN_DIR, 'summary.json');
  writeFileSync(summaryPath, JSON.stringify({
    run_id: runId,
    default_args: [...DEFAULT_LOG_ARGS, '--cwd', DEFAULT_CWD_ROOT],
    run_dir: RUN_DIR,
    cases: caseSummaries,
    totals: totalMetrics(summary),
    rounds: summary,
  }, null, 2), 'utf8');

  const totals = totalMetrics(summary);
  console.log('\ntotal summary');
  console.log(`  wall ms          : ${totals.duration_ms.wall}`);
  console.log(`  planner ms       : ${totals.duration_ms.planner}`);
  console.log(`  main estimate ms : ${totals.duration_ms.main_estimate}`);
  console.log(`  total tokens     : ${totals.usage.total.total_tokens}`);
  console.log(`  input tokens     : ${totals.usage.total.prompt_tokens}`);
  console.log(`  output tokens    : ${totals.usage.total.completion_tokens}`);
  console.log(`  cost             : $${totals.usage.total.cost_usd.toFixed(6)}`);
  console.log(`\nsummary: ${summaryPath}`);
}

main().catch((error) => {
  console.error(error?.stack || String(error));
  process.exitCode = 1;
});


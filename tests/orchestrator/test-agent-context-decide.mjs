#!/usr/bin/env node
import { llmExe, prepareCliArgs } from './exe.mjs';
import { REPO as PACKAGE_ROOT } from './repo.mjs';
/**
 * Isolated context-stuffing decisions.
 *
 * One real Tanit turn (resize 9 screenshots to 1980) is scored the way a
 * Context-phase feature would score it before the next user turn:
 * keep the call, keep the result verbatim, stuff a short line into the
 * next system prompt. Labels stay off the wire.
 *
 *   npm run test:llm:context
 *   npm run test:llm:context:live
 */

import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { createAssert, cliExePath, brand } from '../../../../tests/orchestrator/test-commons.js';
import {
  buildMetricsBundle,
  createMetricsCollector,
  renderMarkdownReport,
  writeTestReports,
} from '../../../../tests/orchestrator/reports.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const RUN_DIR = join(PACKAGE_ROOT, 'stemp', 'agent-context-decide');
const FIXTURE = join(__dirname, 'fixtures', 'session-resize-1980-context.json');
const TEST_REPORT_PATH = join(PACKAGE_ROOT, 'tests', 'test-report-last.md');
const KEEP = 0.5;

const MODE = (() => {
  const idx = process.argv.indexOf('--mode');
  if (idx >= 0 && process.argv[idx + 1]) return process.argv[idx + 1].toLowerCase();
  const eq = process.argv.find((a) => a.startsWith('--mode='));
  if (eq) return eq.slice('--mode='.length).toLowerCase();
  return 'loopback';
})();
const LIVE = MODE === 'live';
const REQUIRE_LIVE = process.argv.includes('--require-live');

const QUESTIONS = {
  keepCall: {
    type: 'noul',
    instructions: 'Next user: `item.nextUser`. This earlier call was `item.tool` `item.command` (`item.status`). The rest of the turn: `item.transcript`. Does knowing this call was made, with that input, still matter for the next turn?',
  },
  keepResult: {
    type: 'noul',
    instructions: 'Result note: `item.result` (`item.payloadChars` characters stored). Must that result stay verbatim for the next turn, because re-running the tool would repeat a side effect or would not recover what matters?',
  },
  stuff: {
    type: 'noul',
    instructions: 'The next system prompt can carry one short line for this call, not the payload. Rules: `item.rules`. Should this call be in that line for `item.nextUser`?',
  },
};

const perfRows = [];
const accuracyReports = [];

function runCli(exe, args, { timeout = 180_000 } = {}) {
  const started = performance.now();
  const result = spawnSync(exe, prepareCliArgs(exe, args), { cwd: PACKAGE_ROOT, encoding: 'utf8', timeout });
  const elapsedMs = Math.round((performance.now() - started) * 100) / 100;
  if (result.stdout) console.log(result.stdout.trimEnd());
  if (result.stderr) console.error(result.stderr.trimEnd());
  return { status: result.status ?? -1, stdout: result.stdout || '', elapsedMs };
}

function parseJson(text) {
  const raw = String(text || '').trim();
  const start = raw.indexOf('{');
  if (start < 0) return null;
  try {
    return JSON.parse(raw.slice(start));
  } catch {
    return null;
  }
}

function roundMs(ms) {
  return Math.round(Number(ms) * 100) / 100;
}

function noulOf(row, key) {
  const value = row?.decisions?.[key]?.noul;
  return typeof value === 'number' ? value : null;
}

function hit(noul, expected) {
  if (noul == null) return false;
  return (noul >= KEEP) === expected;
}

function scoreContext(fixture, out) {
  const byId = new Map((out.items || []).map((row) => [row.id, row]));
  const rows = [];
  const totals = { keepCall: 0, keepResult: 0, stuff: 0 };
  for (const call of fixture.calls) {
    const row = byId.get(call.id) || {};
    const keepCall = noulOf(row, 'keepCall');
    const keepResult = noulOf(row, 'keepResult');
    const stuff = noulOf(row, 'stuff');
    const keepCallHit = hit(keepCall, call.expectedKeepCall);
    const keepResultHit = hit(keepResult, call.expectedKeepResult);
    const stuffHit = hit(stuff, call.expectedStuff);
    if (keepCallHit) totals.keepCall += 1;
    if (keepResultHit) totals.keepResult += 1;
    if (stuffHit) totals.stuff += 1;
    const miss = [
      keepCallHit ? '' : 'keepCall',
      keepResultHit ? '' : 'keepResult',
      stuffHit ? '' : 'stuff',
    ].filter(Boolean).join(', ');
    rows.push({
      id: call.id,
      command: call.command,
      keepCall,
      keepResult,
      stuff,
      expect: [
        call.expectedKeepCall ? 'call' : '',
        call.expectedKeepResult ? 'result' : '',
        call.expectedStuff ? 'line' : '',
      ].filter(Boolean).join('+') || 'drop',
      miss,
    });
  }
  const n = fixture.calls.length;
  const ratio = (correct) => (n ? correct / n : 0);
  return { items: n, rows, totals, ratio };
}

function payloadFor(fixture) {
  const shared = {
    nextUser: fixture.nextUser,
    rules: fixture.rules,
    priorUser: fixture.priorUser,
    assistant: fixture.assistant,
  };
  return {
    calls: fixture.calls.map((call) => ({
      id: call.id,
      tool: call.tool,
      command: call.command,
      status: call.status,
      payloadChars: call.payloadChars,
      result: call.result,
      transcript: call.transcript,
      ...shared,
    })),
  };
}

function notePerf(label, ms, json, scored) {
  const wall = roundMs(ms);
  const expanded = Number(json?.expandedQuestions);
  const row = {
    label,
    ms: wall,
    requests: json?.requests ?? null,
    expanded: Number.isFinite(expanded) ? expanded : null,
    pairs: 0,
    items: json?.items || 0,
    msPerQuestion: Number.isFinite(expanded) && expanded > 0 ? roundMs(wall / expanded) : null,
    inputTokens: json?.usage?.inputTokens ?? null,
    outputTokens: json?.usage?.outputTokens ?? null,
    cost: json?.usage && Object.prototype.hasOwnProperty.call(json.usage, 'cost') ? json.usage.cost : null,
    model: json?.resolvedModel || '',
  };
  if (json?.ok === true) perfRows.push(row);
  const parts = [];
  if (json?.provider) parts.push(String(json.provider));
  if (row.requests != null) parts.push(`${row.requests} req`);
  if (row.expanded != null) parts.push(`${row.expanded} expanded`);
  if (row.items) parts.push(`${row.items} items`);
  if (row.msPerQuestion != null) parts.push(`${row.msPerQuestion} ms/q`);
  if (row.inputTokens) parts.push(`${row.inputTokens} in / ${row.outputTokens ?? 0} out`);
  if (row.cost) parts.push(`cost ${row.cost}`);
  if (row.model && row.model !== 'stub') parts.push(row.model);
  if (scored) {
    parts.push(`call ${(scored.ratio(scored.totals.keepCall) * 100).toFixed(1)}%`);
    parts.push(`result ${(scored.ratio(scored.totals.keepResult) * 100).toFixed(1)}%`);
    parts.push(`stuff ${(scored.ratio(scored.totals.stuff) * 100).toFixed(1)}%`);
  }
  return { label, ms: wall, detail: parts.join(' · ') };
}

function pushAccuracy(label, mode, fixture, scored) {
  const n = scored.items;
  const score = (key) => ({
    label: key,
    correct: `${scored.totals[key]}/${n}`,
    accuracy: scored.ratio(scored.totals[key]),
  });
  accuracyReports.push({
    label,
    mode,
    items: n,
    note: `Next turn is "${fixture.nextUser}" after "${fixture.priorUser}". A keep is noul ≥ ${KEEP}. The two artifact calls (the resize that wrote resized_1980, and the listing of that folder) should stay; the catalog lookup, source listing, --help, and the failed PIL probe should drop. Expected labels are not on the wire. This is the decision a Context-phase feature would make in on_finish, then inject on the next on_prepare.`,
    scores: [score('keepCall'), score('keepResult'), score('stuff')],
    table: {
      headers: ['Call', 'Expect', 'keepCall', 'keepResult', 'stuff', 'Miss'],
      rows: scored.rows.map((row) => [
        row.command,
        row.expect,
        row.keepCall,
        row.keepResult,
        row.stuff,
        row.miss || '—',
      ]),
    },
  });
}

function suiteContext(assert, exe, { live }) {
  const fixture = JSON.parse(readFileSync(FIXTURE, 'utf8'));
  const tag = live ? 'live' : 'stub';
  const source = join(RUN_DIR, `context-${tag}.src.json`);
  const questions = join(RUN_DIR, 'context-questions.json');
  const dst = join(RUN_DIR, `context-${tag}.out.json`);
  writeFileSync(source, `${JSON.stringify(payloadFor(fixture), null, 2)}\n`, 'utf8');
  writeFileSync(questions, `${JSON.stringify(QUESTIONS, null, 2)}\n`, 'utf8');
  const args = [
    '--log-level', 'warn',
    'llm', 'agent', 'decide',
    '-i', source,
    '--questions', questions,
    '--selector', '.calls[]',
    '--target', 'decisions',
    '-o', dst,
    '--json',
  ];
  if (live) args.push('--provider', 'openrouter');
  else args.push('--dry-run');
  const r = runCli(exe, args, { timeout: live ? 180_000 : 60_000 });
  const json = parseJson(r.stdout);
  if (live && json && json.ok !== true && /api key|credential|OPENROUTER/i.test(`${json.error || ''} ${r.stdout}`) && !REQUIRE_LIVE) {
    console.log('   skip live context: no OpenRouter key');
    return { steps: [{ label: 'context live skipped', ms: r.elapsedMs, detail: 'no api key' }] };
  }
  assert(r.status === 0 && json?.ok === true, `context ${tag} ok (status=${r.status})`);
  assert(json.items === fixture.calls.length, `6 calls (got ${json.items})`);
  assert(json.requests === 1, `one request for 6 calls x 3 questions (got ${json.requests})`);
  const out = JSON.parse(readFileSync(dst, 'utf8'));
  const scored = scoreContext(fixture, { items: out.calls });
  console.log(
    `   context ${tag}: keepCall ${scored.totals.keepCall}/${scored.items} `
    + `keepResult ${scored.totals.keepResult}/${scored.items} `
    + `stuff ${scored.totals.stuff}/${scored.items}`,
  );
  for (const row of scored.rows) {
    if (row.miss) console.log(`   miss ${row.id}: ${row.miss} call=${row.keepCall} result=${row.keepResult} stuff=${row.stuff}`);
  }
  if (!live) {
    const flat = scored.rows.flatMap((row) => [row.keepCall, row.keepResult, row.stuff]);
    assert(flat.every((n) => n === 1), `stub noul is 1 on every object (got ${flat.join(',')})`);
  } else {
    assert(scored.rows.every((row) => [row.keepCall, row.keepResult, row.stuff].every((n) => typeof n === 'number')),
      'live noul present on every question');
  }
  pushAccuracy(
    `context stuffing vs resize follow-up (${tag})`,
    tag,
    fixture,
    scored,
  );
  const step = notePerf(live ? 'context live' : 'context stub', r.elapsedMs, json, scored);
  return {
    steps: [step],
    llmLive: live ? {
      title: 'resize turn, next ask is webp',
      prompt: fixture.nextUser,
      expected: 'keep resize + output listing; drop lookup, help, source list, PIL',
      actual: `call ${scored.totals.keepCall}/${scored.items} result ${scored.totals.keepResult}/${scored.items} stuff ${scored.totals.stuff}/${scored.items} model=${json.resolvedModel}`,
      roundTripMs: r.elapsedMs,
    } : undefined,
  };
}

async function writeReport(stats, wallMs, exe, suites, metricsCollector, startedAtIso) {
  const finishedAtIso = new Date().toISOString();
  const mode = LIVE ? 'live' : 'dry-run';
  const payload = {
    ok: (stats.failed ?? 0) === 0,
    passed: stats.passed,
    failed: stats.failed ?? 0,
    meta: {
      cwd: process.cwd(),
      displayName: `llm-context ${mode}`,
      testName: LIVE ? 'llm-context-live' : 'llm-context-dry',
      writtenAt: finishedAtIso,
      exe,
      brand: brand?.name || 'tanit',
      mode,
      wallClockMs: roundMs(wallMs),
    },
    metrics: buildMetricsBundle(metricsCollector, startedAtIso, finishedAtIso),
    images: [],
    mediaSuites: suites,
    decisions: { mode, rows: perfRows, accuracy: accuracyReports },
    integrationNotes: [
      'Isolated from the decide suite. One request, six tool calls from session-10-01-11-24-11, three noul questions.',
      'keepCall / keepResult match fast-jev compaction. stuff is the short line append_recent_turns would inject.',
      'A Context-phase feature (agent_feature.cpp, beside MemoryFeature) can score on_finish and inject on the next on_prepare. This run does not wire that feature.',
      'The stub answers 1 for every object, so its accuracy is the share of calls we expect to keep (2/6).',
    ],
  };
  const md = `${renderMarkdownReport(payload)}\n---\n\n*Artifact: \`tests/test-report-last.md\`.*\n`;
  writeFileSync(TEST_REPORT_PATH, md, 'utf8');
  const stamped = await writeTestReports(payload.meta.testName, payload, { cwd: PACKAGE_ROOT });
  console.log(`report: ${stamped.mdPath}`);
}

const startedAtIso = new Date().toISOString();
const metricsCollector = createMetricsCollector();
const stats = createAssert();
const suites = [];
const t0 = performance.now();
const exe = llmExe();

try {
  if (!existsSync(exe)) throw new Error(`tanit-cli not found at ${exe}`);
  mkdirSync(RUN_DIR, { recursive: true });
  console.log(`\nLLM context decide\n   exe: ${exe}\n   mode: ${LIVE ? 'live' : 'dry-run'}`);
  const dry = suiteContext(stats.assert, exe, { live: false });
  suites.push({ name: 'context stub', totalMs: dry.steps[0].ms, steps: dry.steps });
  if (LIVE) {
    const live = suiteContext(stats.assert, exe, { live: true });
    suites.push({ name: 'context live', totalMs: live.steps[0].ms, steps: live.steps, llmLive: live.llmLive });
  }
} catch (e) {
  console.error(e);
  stats.assert(false, e?.stack || String(e));
} finally {
  await writeReport(stats, performance.now() - t0, exe, suites, metricsCollector, startedAtIso);
}

console.log(`\n${stats.failed === 0 ? 'PASS' : 'FAIL'} llm:context: ${stats.passed} passed, ${stats.failed} failed`);
if (stats.failed > 0) process.exitCode = 1;

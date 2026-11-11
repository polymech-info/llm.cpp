#!/usr/bin/env node
import { llmExe, prepareCliArgs } from './exe.mjs';
import { REPO as PACKAGE_ROOT } from './repo.mjs';
/**
 * Isolated learning harvest.
 *
 * Observations from one finished resize turn are classified into the two
 * stores the agent already has: a short-term session artifact, or a
 * long-term memory_write fact. The label is not sent.
 *
 *   npm run test:llm:learn
 *   npm run test:llm:learn:live
 */

import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { createAssert, cliExePath, brand } from '@polymech/tanit/tests/orchestrator/test-commons.js';
import {
  buildMetricsBundle,
  createMetricsCollector,
  renderMarkdownReport,
  writeTestReports,
} from '@polymech/tanit/tests/orchestrator/reports.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const RUN_DIR = join(PACKAGE_ROOT, 'stemp', 'agent-learn-decide');
const FIXTURE = join(__dirname, 'fixtures', 'session-resize-1980-learn.json');
const TEST_REPORT_PATH = join(PACKAGE_ROOT, 'tests', 'test-report-last.md');

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
  horizon: {
    type: 'choice',
    instructions: 'Episode: `item.episode`. Observation: `item.text`. Where should this be harvested?',
    criteria: {
      drop: 'Do not store it. A help dump, a catalog body, or a detail with nothing to learn.',
      session: 'Short-term artifact for this chat only: the output path, the files touched, the command that worked, or the ask.',
      durable: 'Long-term memory_write fact for later chats: an environment fact, a standing project locator, or a preference. Keep it small.',
      both: 'Needed as a session artifact now, and still true as a memory_write fact in later chats.',
    },
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

function coversSession(horizon) {
  return horizon === 'session' || horizon === 'both';
}

function coversDurable(horizon) {
  return horizon === 'durable' || horizon === 'both';
}

function scoreLearn(fixture, rows) {
  const byId = new Map(rows.map((row) => [row.id, row]));
  const totals = { exact: 0, session: 0, durable: 0 };
  const table = [];
  for (const item of fixture.observations) {
    const choice = byId.get(item.id)?.decisions?.horizon?.choice || '';
    const exact = choice === item.expectedHorizon;
    const session = coversSession(choice) === coversSession(item.expectedHorizon);
    const durable = coversDurable(choice) === coversDurable(item.expectedHorizon);
    if (exact) totals.exact += 1;
    if (session) totals.session += 1;
    if (durable) totals.durable += 1;
    const miss = [
      exact ? '' : 'exact',
      session ? '' : 'session',
      durable ? '' : 'durable',
    ].filter(Boolean).join(', ');
    table.push([item.text, item.expectedHorizon, choice || '—', miss || '—']);
  }
  const n = fixture.observations.length;
  return { items: n, totals, table, ratio: (k) => (n ? totals[k] / n : 0) };
}

function notePerf(label, ms, json, scored) {
  const wall = roundMs(ms);
  const expanded = Number(json?.expandedQuestions);
  if (json?.ok === true) {
    perfRows.push({
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
    });
  }
  const parts = [];
  if (json?.provider) parts.push(String(json.provider));
  if (json?.requests != null) parts.push(`${json.requests} req`);
  if (Number.isFinite(expanded)) parts.push(`${expanded} expanded`);
  if (json?.items) parts.push(`${json.items} items`);
  if (json?.usage?.inputTokens) parts.push(`${json.usage.inputTokens} in / ${json.usage.outputTokens ?? 0} out`);
  if (json?.usage?.cost) parts.push(`cost ${json.usage.cost}`);
  if (json?.resolvedModel && json.resolvedModel !== 'stub') parts.push(json.resolvedModel);
  if (scored) {
    parts.push(`exact ${(scored.ratio('exact') * 100).toFixed(1)}%`);
    parts.push(`session ${(scored.ratio('session') * 100).toFixed(1)}%`);
    parts.push(`durable ${(scored.ratio('durable') * 100).toFixed(1)}%`);
  }
  return { label, ms: wall, detail: parts.join(' · ') };
}

function suiteLearn(assert, exe, { live }) {
  const fixture = JSON.parse(readFileSync(FIXTURE, 'utf8'));
  const tag = live ? 'live' : 'stub';
  const source = join(RUN_DIR, `learn-${tag}.src.json`);
  const questions = join(RUN_DIR, 'learn-questions.json');
  const dst = join(RUN_DIR, `learn-${tag}.out.json`);
  const doc = {
    observations: fixture.observations.map((item) => ({
      id: item.id,
      text: item.text,
      episode: fixture.episode,
    })),
  };
  writeFileSync(source, `${JSON.stringify(doc, null, 2)}\n`, 'utf8');
  writeFileSync(questions, `${JSON.stringify(QUESTIONS, null, 2)}\n`, 'utf8');
  const args = [
    '--log-level', 'warn',
    'llm', 'agent', 'decide',
    '-i', source,
    '--questions', questions,
    '--selector', '.observations[]',
    '--target', 'decisions',
    '-o', dst,
    '--json',
    live ? '--provider' : '--dry-run',
  ];
  if (live) args.push('openrouter');
  const r = runCli(exe, args, { timeout: live ? 180_000 : 60_000 });
  const json = parseJson(r.stdout);
  if (live && json && json.ok !== true && /api key|credential|OPENROUTER/i.test(`${json.error || ''} ${r.stdout}`) && !REQUIRE_LIVE) {
    console.log('   skip live learn: no OpenRouter key');
    return { steps: [{ label: 'learn live skipped', ms: r.elapsedMs, detail: 'no api key' }] };
  }
  assert(r.status === 0 && json?.ok === true, `learn ${tag} ok (status=${r.status})`);
  assert(json.items === fixture.observations.length, `8 observations (got ${json.items})`);
  assert(json.requests === 1, `one request (got ${json.requests})`);
  const out = JSON.parse(readFileSync(dst, 'utf8'));
  const scored = scoreLearn(fixture, out.observations || []);
  console.log(
    `   learn ${tag}: exact ${scored.totals.exact}/${scored.items} `
    + `session ${scored.totals.session}/${scored.items} `
    + `durable ${scored.totals.durable}/${scored.items}`,
  );
  for (const row of scored.table) {
    if (row[3] !== '—') console.log(`   miss: expected=${row[1]} choice=${row[2]} ${row[0]}`);
  }
  if (!live) {
    const choices = new Set((out.observations || []).map((row) => row.decisions?.horizon?.choice));
    assert(choices.size === 1, `stub uses one horizon (got ${[...choices].join(',')})`);
  } else {
    assert((out.observations || []).every((row) => typeof row.decisions?.horizon?.choice === 'string'),
      'live horizon present on every observation');
  }
  const n = scored.items;
  accuracyReports.push({
    label: `learning harvest (${tag})`,
    note: 'Short-term is a session artifact for this chat. Long-term is a memory_write fact for later chats. exact is the four-way label. session/durable count a "both" as covering that store. The label is not on the wire.',
    items: n,
    scores: [
      { label: 'exact horizon', correct: `${scored.totals.exact}/${n}`, accuracy: scored.ratio('exact') },
      { label: 'session store', correct: `${scored.totals.session}/${n}`, accuracy: scored.ratio('session') },
      { label: 'durable store', correct: `${scored.totals.durable}/${n}`, accuracy: scored.ratio('durable') },
    ],
    table: {
      headers: ['Observation', 'Expect', 'Choice', 'Miss'],
      rows: scored.table,
    },
  });
  return {
    steps: [notePerf(live ? 'learn live' : 'learn stub', r.elapsedMs, json, scored)],
    llmLive: live ? {
      title: 'harvest resize-turn observations',
      prompt: fixture.episode,
      expected: 'session artifacts for this chat; memory_write for PIL and the screenshots folder; drop help and the catalog body',
      actual: `exact ${scored.totals.exact}/${n} session ${scored.totals.session}/${n} durable ${scored.totals.durable}/${n} model=${json.resolvedModel}`,
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
      displayName: `llm-learn ${mode}`,
      testName: LIVE ? 'llm-learn-live' : 'llm-learn-dry',
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
      'Learning harvest only. Context stuffing is not scored here.',
      'session matches a session item written for this chat. durable matches memory_write into the long-term document.',
      'The stub picks one criteria key for every row, so its exact score is how many labels equal that key.',
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
  console.log(`\nLLM learn decide\n   exe: ${exe}\n   mode: ${LIVE ? 'live' : 'dry-run'}`);
  const dry = suiteLearn(stats.assert, exe, { live: false });
  suites.push({ name: 'learn stub', totalMs: dry.steps[0].ms, steps: dry.steps });
  if (LIVE) {
    const live = suiteLearn(stats.assert, exe, { live: true });
    suites.push({ name: 'learn live', totalMs: live.steps[0].ms, steps: live.steps, llmLive: live.llmLive });
  }
} catch (e) {
  console.error(e);
  stats.assert(false, e?.stack || String(e));
} finally {
  await writeReport(stats, performance.now() - t0, exe, suites, metricsCollector, startedAtIso);
}

console.log(`\n${stats.failed === 0 ? 'PASS' : 'FAIL'} llm:learn: ${stats.passed} passed, ${stats.failed} failed`);
if (stats.failed > 0) process.exitCode = 1;

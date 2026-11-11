#!/usr/bin/env node
/**
 * Tanit agent harness for the C++ office_* tools.
 *
 * Loopback scripts the tool calls through `tanit-cli llm agent --router loopback`
 * and checks the tool envelopes plus the files on disk. Live asks the configured
 * preset to create a docx with those tools, then reads it back through loopback.
 *
 *   npm run test:agent:office
 *   npm run test:agent:office:fast-build
 *   npm run test:agent:office:live
 *
 * Flags:
 *   --mode loopback|live
 *   --preset <chat-preset>   (live; default quick)
 *   --only docx,xlsx,pptx,reject,live
 *   --require-live
 */
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

import { createAssert, brand } from '@polymech/tanit/tests/orchestrator/test-commons.js';
import { createMetricsCollector } from '@polymech/tanit/tests/orchestrator/reports.js';
import { llmExe } from './exe.mjs';
import {
  TEST_REPORT_PATH,
  bootstrapAgentHarness,
  createAgentHarnessOptions,
  createAgentTestReport,
  finalText,
  printAgentHarnessBanner,
  readJson,
  runAgentRound,
  runHarnessSuites,
  toolCalls,
  toolResultEnvelope,
  truncateText,
  writeAgentTestReportFile,
} from './test-agent-commons.mjs';

const LOOPBACK_PRESET = 'office-tools-loopback';
const OFFICE_TOOLS = ['--enable-tools',
  'office_create,office_view,office_get,office_query,office_add,office_set,office_remove,office_move,office_swap,office_batch,office_validate'];
const SUITE_NAMES = ['docx', 'xlsx', 'pptx', 'reject', 'live'];

const harness = createAgentHarnessOptions({
  harnessSubdir: 'agent-office',
  loopbackPresetId: LOOPBACK_PRESET,
  displayName: 'agent-office',
});

function toolEnvelopes(bundlePath, name) {
  const bundle = bundlePath ? readJson(bundlePath) : null;
  const out = [];
  for (const ev of bundle?.communication?.tools || []) {
    if (ev.kind !== 'tool_result' || ev.tool !== name) continue;
    let d = ev.detail;
    if (typeof d === 'string') {
      try { d = JSON.parse(d); } catch { d = null; }
    }
    if (d && typeof d === 'object') out.push(d.envelope ?? d);
  }
  return out;
}

function scripted(ctx, { suite, index, name, calls, timeout }) {
  return runAgentRound({
    ...ctx,
    mode: 'loopback',
    sessionId: `${ctx.runId}-${suite}-${index}`,
    prompt: name,
    extraArgs: [...OFFICE_TOOLS, '--consent-ui', 'auto-allow', '--no-parallel-tools'],
    script: [
      { tool_calls: calls.map((c, i) => ({ id: `tc_${suite}_${index}_${i}`, ...c })) },
      { text: `done: ${name}` },
    ],
    timeout: timeout ?? 120_000,
    roundKey: `${suite}-r${String(index).padStart(2, '0')}`,
    roundMeta: { suite, index, name },
  });
}

function isZip(path) {
  if (!existsSync(path)) return false;
  const bytes = readFileSync(path);
  return bytes.length > 22 && bytes[0] === 0x50 && bytes[1] === 0x4b;
}

function suiteDocx(rep, assert, ctx) {
  console.log('\n-- docx --\n');
  rep.beginSuite('docx');
  const file = join(ctx.cwd, 'note.docx');
  const r = scripted(ctx, {
    suite: 'docx',
    index: 1,
    name: 'create, edit, and read a docx',
    calls: [
      { name: 'office_create', arguments: { path: file } },
      { name: 'office_add', arguments: { path: file, parent: '/', type: 'paragraph', props: { text: 'Hello', style: 'Heading1' } } },
      { name: 'office_add', arguments: { path: file, parent: '/', type: 'paragraph', props: { text: 'Second' } } },
      { name: 'office_view', arguments: { path: file, mode: 'outline' } },
      { name: 'office_set', arguments: { path: file, node: '/', find: 'Hello', replace: 'Hi' } },
      { name: 'office_swap', arguments: { path: file, node: '/body/p[1]', node2: '/body/p[2]' } },
      { name: 'office_view', arguments: { path: file, mode: 'text' } },
      { name: 'office_get', arguments: { path: file, node: '/body/p[1]' } },
      { name: 'office_validate', arguments: { path: file } },
    ],
  });
  const created = toolResultEnvelope(r.bundlePath, 'office_create');
  const added = toolEnvelopes(r.bundlePath, 'office_add');
  const views = toolEnvelopes(r.bundlePath, 'office_view');
  const swapped = toolResultEnvelope(r.bundlePath, 'office_swap');
  const got = toolResultEnvelope(r.bundlePath, 'office_get');
  const valid = toolResultEnvelope(r.bundlePath, 'office_validate');
  rep.step('docx edit', r.ms, truncateText(views[1]?.text, 80));
  assert(r.status === 0, `docx: agent exits 0 (status=${r.status} stderr=${truncateText(r.stderr, 160)})`);
  assert(created?.ok === true, `docx: office_create ok (got ${truncateText(JSON.stringify(created), 160)})`);
  assert(added.length === 2 && added.every((e) => e.ok === true), `docx: two adds (got ${added.length})`);
  assert(String(views[0]?.text || '').includes('Heading1'), 'docx: outline names Heading1');
  assert(swapped?.ok === true, `docx: swap ok (got ${truncateText(JSON.stringify(swapped), 160)})`);
  const text = String(views[1]?.text || '');
  assert(text.includes('Hi') && !text.includes('Hello'), `docx: find/replace (got ${truncateText(text, 80)})`);
  assert(text.indexOf('Second') < text.indexOf('Hi'), `docx: swap order (got ${truncateText(text, 80)})`);
  assert(JSON.stringify(got || {}).includes('Second'), 'docx: get reads the swapped first paragraph');
  assert(valid?.ok === true, 'docx: validate ok');
  assert(isZip(file), 'docx: file is a zip package');
  rep.endSuite();
}

function suiteXlsx(rep, assert, ctx) {
  console.log('\n-- xlsx --\n');
  rep.beginSuite('xlsx');
  const file = join(ctx.cwd, 'book.xlsx');
  const r = scripted(ctx, {
    suite: 'xlsx',
    index: 1,
    name: 'create and edit a workbook',
    calls: [
      { name: 'office_create', arguments: { path: file } },
      { name: 'office_set', arguments: { path: file, node: 'Sheet1!A1', props: { value: 'Alice' } } },
      { name: 'office_set', arguments: { path: file, node: 'Sheet1!A2', props: { value: '10' } } },
      { name: 'office_get', arguments: { path: file, node: 'Sheet1!A1' } },
      { name: 'office_query', arguments: { path: file, selector: 'sheet[name=Sheet1] > row[1] > cell' } },
      { name: 'office_add', arguments: { path: file, parent: '/', type: 'sheet', props: { name: 'Data' } } },
      { name: 'office_view', arguments: { path: file, mode: 'outline' } },
    ],
  });
  const got = toolResultEnvelope(r.bundlePath, 'office_get');
  const queried = toolResultEnvelope(r.bundlePath, 'office_query');
  const outline = toolResultEnvelope(r.bundlePath, 'office_view');
  rep.step('xlsx edit', r.ms, truncateText(outline?.text, 80));
  assert(r.status === 0, `xlsx: agent exits 0 (status=${r.status})`);
  assert(JSON.stringify(got || {}).includes('Alice'), `xlsx: get A1 (got ${truncateText(JSON.stringify(got), 160)})`);
  assert(JSON.stringify(queried || {}).includes('Alice'), 'xlsx: query finds Alice');
  assert(String(outline?.text || '').includes('Data'), `xlsx: outline lists Data (got ${truncateText(outline?.text, 80)})`);
  assert(isZip(file), 'xlsx: file is a zip package');
  rep.endSuite();
}

function suitePptx(rep, assert, ctx) {
  console.log('\n-- pptx --\n');
  rep.beginSuite('pptx');
  const file = join(ctx.cwd, 'deck.pptx');
  const png = join(ctx.cwd, 'pixel.png');
  writeFileSync(png, Buffer.from(
    'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==',
    'base64'));
  const r = scripted(ctx, {
    suite: 'pptx',
    index: 1,
    name: 'create a slide',
    calls: [
      { name: 'office_create', arguments: { path: file } },
      { name: 'office_add', arguments: { path: file, parent: '/', type: 'slide', props: { title: 'Deck' } } },
      { name: 'office_add', arguments: { path: file, parent: '/slide[1]', type: 'shape', props: { text: 'Body' } } },
      { name: 'office_set', arguments: { path: file, node: '/slide[1]', props: { background: '1B2838', transition: 'fade' } } },
      { name: 'office_set', arguments: { path: file, node: '/slide[1]/shape[2]', props: { animation: 'fade-entrance-500' } } },
      { name: 'office_add', arguments: { path: file, parent: '/slide[1]', type: 'picture', props: { src: png, name: 'Shot' } } },
      { name: 'office_view', arguments: { path: file, mode: 'text' } },
      { name: 'office_validate', arguments: { path: file } },
    ],
  });
  const viewed = toolResultEnvelope(r.bundlePath, 'office_view');
  const styled = toolEnvelopes(r.bundlePath, 'office_set');
  const added = toolEnvelopes(r.bundlePath, 'office_add');
  const pictured = added[added.length - 1];
  const valid = toolResultEnvelope(r.bundlePath, 'office_validate');
  const text = String(viewed?.text || '');
  rep.step('pptx edit', r.ms, truncateText(text, 80));
  assert(r.status === 0, `pptx: agent exits 0 (status=${r.status})`);
  assert(text.includes('Deck') && text.includes('Body'), `pptx: view text (got ${truncateText(text, 80)})`);
  assert(styled.length === 2 && styled.every((e) => e.ok === true), `pptx: transition and animation (got ${truncateText(JSON.stringify(styled), 160)})`);
  assert(pictured?.ok === true && String(pictured?.path || '').includes('picture'), `pptx: picture placed (got ${truncateText(JSON.stringify(pictured), 160)})`);
  assert(valid?.ok === true, 'pptx: validate ok');
  assert(isZip(file), 'pptx: file is a zip package');
  rep.endSuite();
}

function suiteReject(rep, assert, ctx) {
  console.log('\n-- reject --\n');
  rep.beginSuite('reject');
  const file = join(ctx.cwd, 'reject.docx');
  const created = scripted(ctx, {
    suite: 'reject',
    index: 0,
    name: 'seed docx',
    calls: [{ name: 'office_create', arguments: { path: file } }],
  });
  assert(toolResultEnvelope(created.bundlePath, 'office_create')?.ok === true, 'reject: seed docx');
  const chart = scripted(ctx, {
    suite: 'reject',
    index: 1,
    name: 'unsupported chart',
    calls: [{ name: 'office_add', arguments: { path: file, parent: '/', type: 'chart' } }],
  });
  const chartEnv = toolResultEnvelope(chart.bundlePath, 'office_add');
  rep.step('chart', chart.ms, truncateText(chartEnv?.error, 80));
  assert(chart.status === 0, 'reject: agent still exits 0 when the tool returns an error');
  assert(chartEnv?.ok === false && /unsupported/i.test(String(chartEnv?.error || '')),
    `reject: chart is unsupported (got ${truncateText(JSON.stringify(chartEnv), 160)})`);

  const again = scripted(ctx, {
    suite: 'reject',
    index: 2,
    name: 'create existing',
    calls: [{ name: 'office_create', arguments: { path: file } }],
  });
  const againEnv = toolResultEnvelope(again.bundlePath, 'office_create');
  rep.step('exists', again.ms, truncateText(againEnv?.error, 80));
  assert(againEnv?.ok === false, `reject: create fails when the file exists (got ${truncateText(JSON.stringify(againEnv), 160)})`);
  rep.endSuite();
}

function suiteLive(rep, assert, ctx) {
  console.log('\n-- live --\n');
  rep.beginSuite('live');
  if (ctx.mode !== 'live') {
    rep.note('live skipped in loopback mode');
    rep.endSuite();
    return;
  }
  const file = join(ctx.cwd, 'live-note.docx');
  const r = runAgentRound({
    ...ctx,
    sessionId: `${ctx.runId}-live-office`,
    prompt: [
      `Create "${file}".`,
      'The document must contain one paragraph whose text is exactly OFFICE_LIVE_MARKER.',
      'Use office_create and office_add. Do not use write_file.',
      'When the file is saved, reply with exactly OFFICE_OK.',
    ].join(' '),
    extraArgs: [...OFFICE_TOOLS, '--max-iter', '8', '--no-parallel-tools'],
    timeout: 180_000,
    roundKey: 'live-r01',
    roundMeta: { suite: 'live', index: 1, name: 'create a marked docx' },
  });
  const calls = toolCalls(r.bundlePath);
  rep.step('live create', r.ms, calls.join(','));
  rep.note(`live stdout: ${truncateText(r.stdout, 240)}`);
  assert(r.status === 0, `live: exits 0 (status=${r.status} stderr=${truncateText(r.stderr, 160)})`);
  assert(calls.includes('office_create'), `live: called office_create (got ${calls.join(',')})`);
  assert(calls.some((name) => name === 'office_add' || name === 'office_set' || name === 'office_batch'),
    `live: edited the document with an office tool (got ${calls.join(',')})`);
  assert(!calls.includes('write_file'), 'live: did not use write_file');
  assert(/OFFICE_OK/.test(finalText(r.bundlePath)), `live: reply OFFICE_OK (got ${truncateText(finalText(r.bundlePath), 120)})`);

  const viewed = scripted(ctx, {
    suite: 'live',
    index: 2,
    name: 'read the live docx back',
    calls: [{ name: 'office_view', arguments: { path: file, mode: 'text' } }],
  });
  const text = String(toolResultEnvelope(viewed.bundlePath, 'office_view')?.text || '');
  rep.step('live readback', viewed.ms, truncateText(text, 80));
  assert(text.includes('OFFICE_LIVE_MARKER'), `live: saved text (got ${truncateText(text, 80)})`);
  assert(isZip(file), 'live: file is a zip package');
  rep.endSuite();
}

function runSuites(ctx) {
  const { wantSuite } = harness;
  if (wantSuite('docx')) suiteDocx(rep, stats.assert, ctx);
  if (wantSuite('xlsx')) suiteXlsx(rep, stats.assert, ctx);
  if (wantSuite('pptx')) suitePptx(rep, stats.assert, ctx);
  if (wantSuite('reject')) suiteReject(rep, stats.assert, ctx);
  if (wantSuite('live')) suiteLive(rep, stats.assert, ctx);
  if (harness.onlyScopes && !SUITE_NAMES.some(wantSuite)) {
    throw new Error(`No matching suites for --only ${[...harness.onlyScopes].join(', ')}`);
  }
}

const startedAtIso = new Date().toISOString();
const metricsCollector = createMetricsCollector();
const stats = createAssert();
const rep = createAgentTestReport(harness.paths);
const t0 = performance.now();
let runDir = '';
let sessionDir = '';

try {
  const exe = llmExe();
  if (!existsSync(exe)) {
    throw new Error(`${brand.APP_ID_SLUG}-cli not found at ${exe} (build with: npm run build:cpp-fast)`);
  }
  const boot = bootstrapAgentHarness(harness, { exe, perRunWorkDir: true });
  runDir = boot.runDir;
  sessionDir = boot.sessionDir;
  printAgentHarnessBanner(harness, { exe, runDir: boot.runDir, sessionDir: boot.sessionDir });
  rep.note(`Run artifacts: ${boot.runDir}`);
  rep.note(`Mode: ${harness.mode}${harness.mode === 'live' ? `, preset=${harness.preset}` : ''}`);
  runHarnessSuites(harness, {
    exe,
    ctx: boot.ctx,
    env: boot.env,
    rep,
    runSuites,
  });
} catch (e) {
  console.error(e);
  rep.meta.abortReason = 'uncaught exception';
  rep.meta.uncaughtError = e?.stack || String(e);
} finally {
  rep.finalize(stats, performance.now() - t0, llmExe(), {
    runDir,
    sessionDir,
    mode: harness.mode,
    preset: harness.mode === 'live' ? harness.preset : LOOPBACK_PRESET,
  });
  rep.meta.failed = stats.failed;
  rep.meta.passed = stats.passed;
  writeAgentTestReportFile(rep, metricsCollector, startedAtIso, {
    runDir,
    displayName: harness.displayName,
    testName: harness.displayName,
    extraMeta: {
      mode: harness.mode,
      preset: harness.mode === 'live' ? harness.preset : LOOPBACK_PRESET,
      sessionDir,
    },
  });
}

console.log(`\n${stats.failed === 0 && !rep.meta.abortReason ? 'PASS' : 'FAIL'} agent-office: ${stats.passed} passed, ${stats.failed} failed`);
console.log(`  report     : ${TEST_REPORT_PATH}`);
console.log(`  run dir    : ${runDir}`);
if (stats.failed > 0 || rep.meta.abortReason) process.exitCode = 1;

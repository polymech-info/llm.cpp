#!/usr/bin/env node
import { llmExe, prepareCliArgs } from './exe.mjs';
import { REPO as PACKAGE_ROOT } from './repo.mjs';
/**
 * Async JSON iterator harness (`llm agent each` CLI + `test core iterator`).
 *
 * Invoked via:
 *   npm run test:llm:iterator              # dry-run (default)
 *   npm run test:llm:iterator:live         # live LLM leaf transforms
 *   npm run test:llm:iterator:build
 *
 * HTTP `POST /v1/llm/each` lives in `test:llm-server` /
 * `test:llm-server:each:live` (test-llm-server.mjs), not here.
 *
 * Flag roles (see `llm agent each --help`):
 *   --source     input JSON file
 *   --selector   jq filter selecting string leaves
 *   --target     optional sibling key on the parent (omit = in-place)
 *   --merge-json checked JSON-object reply → hard-overwrite merge onto parent
 *   --dst        full mutated JSON document output path
 */

import { spawnSync } from 'node:child_process';
import { copyFileSync, existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { createAssert, cliExePath, brand } from '../../../../tests/orchestrator/test-commons.js';
import { buildMetricsBundle, createMetricsCollector, renderMarkdownReport } from '../../../../tests/orchestrator/reports.js';
import {
  appendMarkdownChapter,
  getChapterTitle,
  getNpmScriptLabel,
  isAccumulateMode,
  TEST_REPORT_ALL_PATH,
} from '../../../../tests/orchestrator/test-report-accumulate.mjs';
const __dirname = dirname(fileURLToPath(import.meta.url));
const FIXTURES = join(__dirname, 'fixtures');
const STEMP_DIR = join(PACKAGE_ROOT, 'stemp');
const RUN_DIR = join(STEMP_DIR, 'agent-iterator');
const TEST_REPORT_PATH = join(PACKAGE_ROOT, 'tests', 'test-report-last.md');
const DEFAULT_LOG_ARGS = ['--log-level', 'warn'];

const MODE = (() => {
  const idx = process.argv.indexOf('--mode');
  if (idx >= 0 && process.argv[idx + 1]) return process.argv[idx + 1].toLowerCase();
  const eq = process.argv.find((a) => a.startsWith('--mode='));
  if (eq) return eq.slice('--mode='.length).toLowerCase();
  return (process.env.PM_AGENT_ITERATOR_MODE || 'loopback').toLowerCase();
})();
const LIVE = MODE === 'live';
const REQUIRE_LIVE = process.argv.includes('--require-live');

function runCli(exe, args, { timeout = 120_000 } = {}) {
  const started = performance.now();
  console.log(`\n$ ${JSON.stringify(exe)} ${args.map((a) => JSON.stringify(a)).join(' ')}`);
  const result = spawnSync(exe, prepareCliArgs(exe, args), {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout,
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
    stdout: result.stdout || '',
    stderr: result.stderr || '',
    elapsedMs,
  };
}

function parseJson(text) {
  const raw = String(text || '').trim();
  const start = Math.min(...[raw.indexOf('{'), raw.indexOf('[')].filter((n) => n >= 0));
  if (!Number.isFinite(start) || start < 0) return null;
  try {
    return JSON.parse(raw.slice(start));
  } catch {
    return null;
  }
}

function readJson(path) {
  return JSON.parse(readFileSync(path, 'utf8'));
}

function hasEachSubcommand(exe) {
  const help = runCli(exe, [...DEFAULT_LOG_ARGS, 'llm', 'agent', 'each', '--help'], { timeout: 30_000 });
  return help.status === 0 && /--selector|--source/i.test(`${help.stdout}\n${help.stderr}`);
}

function suiteCoreProbe(assert, exe) {
  console.log('\n-- Iterator: test core iterator probe --\n');
  const file = join(FIXTURES, 'iterator-products.json');
  const r = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'test', 'core', '--json',
    'iterator',
    '--input-file', file,
    '--path', '.items[].name',
    '--transform', 'upper',
  ]);
  const json = parseJson(r.stdout);
  assert(r.status === 0 && json?.ok === true, `core iterator ok (status=${r.status})`);
  assert(json?.transformed === 3, `3 names transformed (got ${json?.transformed})`);
  assert(json?.output?.items?.[0]?.name === 'APPLE', 'apple → APPLE');
  assert(typeof json?.timingsMs?.collect === 'number', 'core probe reports collect timing');
  return r.elapsedMs;
}

function suiteEachInPlace(assert, exe) {
  console.log('\n-- Iterator: llm agent each in-place (dry-run) --\n');
  const src = join(RUN_DIR, 'products.src.json');
  const dst = join(RUN_DIR, 'products.out.json');
  copyFileSync(join(FIXTURES, 'iterator-products.json'), src);

  const r = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'each',
    '--source', src,
    '--selector', '.items[].name',
    '--dst', dst,
    '--dry-run',
    '--transform', 'upper',
    '--concurrency', '4',
    '--json',
  ]);
  const json = parseJson(r.stdout);
  assert(r.status === 0 && json?.ok === true, `each in-place ok (status=${r.status})`);
  assert(existsSync(dst), '--dst file written');
  const out = readJson(dst);
  assert(out.items[0].name === 'APPLE' && out.items[2].name === 'ORANGE', 'names uppercased on disk');
  assert(out.items[0].description === 'A fruit', 'non-selected fields preserved');
  assert(json?.concurrency === 4, `concurrency honored (got ${json?.concurrency})`);
  return r.elapsedMs;
}

function suiteEachTargetSibling(assert, exe) {
  console.log('\n-- Iterator: llm agent each --target sibling (commands-mini) --\n');
  const src = join(RUN_DIR, 'commands-mini.src.json');
  const dst = join(RUN_DIR, 'commands-mini.de.json');
  copyFileSync(join(FIXTURES, 'iterator-commands-mini.json'), src);

  const r = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'each',
    '-i', src,
    '--selector', '.ribbon.groups[].items[].label',
    '--target', 'label_de',
    '-o', dst,
    '--dry-run',
    '--transform', 'prefix',
    '--prefix', 'de',
    '--json',
  ]);
  const json = parseJson(r.stdout);
  assert(r.status === 0 && json?.ok === true, `each --target ok (status=${r.status} err=${json?.error || r.stderr})`);
  assert(json?.transformed === 4, `4 labels → label_de (got ${json?.transformed})`);

  const out = readJson(dst);
  const first = out.ribbon.groups[0].items[0];
  assert(first.label === 'Home', 'original label kept');
  assert(first.label_de === '[de] Home', `sibling label_de written (got ${first.label_de})`);
  assert(out.ribbon.groups[1].items[1].label_de === '[de] Next', 'nested group label_de written');
  return r.elapsedMs;
}

function suiteEachCache(assert, exe) {
  console.log('\n-- Iterator: identical-input cache --\n');
  const src = join(RUN_DIR, 'cache.src.json');
  const dst = join(RUN_DIR, 'cache.out.json');
  writeFileSync(src, JSON.stringify({
    items: [
      { label: 'Home' },
      { label: 'Home' },
      { label: 'Next' },
    ],
  }), 'utf8');

  const r = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'each',
    '--source', src,
    '--selector', '.items[].label',
    '--dst', dst,
    '--dry-run',
    '--transform', 'prefix',
    '--prefix', 'x',
    '--concurrency', '1', // serial so the duplicate Home is a guaranteed cache hit
    '--json',
  ]);
  const json = parseJson(r.stdout);
  assert(r.status === 0 && json?.ok === true, 'cache run ok');
  assert(json?.transformed === 3, 'three leaves transformed');
  assert(json?.cacheHits >= 1, `expected cache hit on duplicate Home (got ${json?.cacheHits})`);
  const out = readJson(dst);
  assert(out.items[0].label === '[x] Home' && out.items[1].label === '[x] Home', 'duplicates agree');
  return r.elapsedMs;
}

function suiteEachLlmStub(assert, exe) {
  console.log('\n-- Iterator: llm-stub prompt shaping --\n');
  const src = join(RUN_DIR, 'stub.src.json');
  const dst = join(RUN_DIR, 'stub.out.json');
  writeFileSync(src, JSON.stringify({ items: [{ label: 'Share' }] }), 'utf8');

  const r = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'each',
    '--source', src,
    '--selector', '.items[].label',
    '--target', 'label_fr',
    '--dst', dst,
    '--dry-run',
    '--transform', 'llm-stub',
    '--prompt', 'Translate to French',
    '--json',
  ]);
  const json = parseJson(r.stdout);
  assert(r.status === 0 && json?.ok === true, 'llm-stub exits ok');
  const out = readJson(dst);
  assert(out.items[0].label === 'Share', 'source leaf preserved with --target');
  assert(String(out.items[0].label_fr).includes('Share'),
    `llm-stub embeds original text (got ${out.items[0].label_fr})`);
  assert(String(out.items[0].label_fr).startsWith('[llm]'), 'llm-stub marker present');
  return r.elapsedMs;
}

function suiteEachMergeJsonStub(assert, exe) {
  console.log('\n-- Iterator: --merge-json dry-run (json-stub, hard overwrite) --\n');
  const src = join(RUN_DIR, 'merge-stub.src.json');
  const dst = join(RUN_DIR, 'merge-stub.out.json');
  writeFileSync(src, JSON.stringify({
    items: [
      { label: 'Home', label_de: 'OLD_DE', description: 'keep me' },
      { label: 'Next' },
    ],
  }, null, 2), 'utf8');

  const r = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'each',
    '--source', src,
    '--selector', '.items[].label',
    '--merge-json',
    '--merge-keys', 'label_de,label_fr',
    '--dst', dst,
    '--dry-run',
    '--transform', 'json-stub',
    '--json',
  ]);
  const json = parseJson(r.stdout);
  assert(r.status === 0 && json?.ok === true,
    `merge-json stub ok (status=${r.status} err=${json?.error || r.stderr})`);
  assert(json?.mergeJson === true, 'stdout reports mergeJson');
  assert(json?.transformed === 2, `2 leaves merged (got ${json?.transformed})`);

  const out = readJson(dst);
  assert(out.items[0].label === 'Home', 'selected leaf preserved (not in stub keys)');
  assert(out.items[0].label_de === '[de] Home',
    `label_de hard-overwritten (got ${out.items[0].label_de})`);
  assert(out.items[0].label_fr === '[fr] Home', `label_fr merged (got ${out.items[0].label_fr})`);
  assert(out.items[0].description === 'keep me', 'unrelated sibling untouched');
  assert(out.items[1].label_de === '[de] Next' && out.items[1].label_fr === '[fr] Next',
    'second item got both language keys');
  return r.elapsedMs;
}

function suiteLiveTranslate(assert, exe) {
  console.log('\n-- Iterator: LIVE translate labels (German siblings) --\n');
  const src = join(RUN_DIR, 'live.src.json');
  const dst = join(RUN_DIR, 'live.de.json');
  writeFileSync(src, JSON.stringify({
    ribbon: {
      groups: [{
        items: [
          { id: 'a', label: 'Home', description: 'Open home' },
          { id: 'b', label: 'Next', description: 'Go next' },
          { id: 'c', label: 'Home', description: 'duplicate for cache' },
        ],
      }],
    },
  }, null, 2), 'utf8');

  const r = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'each',
    '--source', src,
    '--selector', '.ribbon.groups[].items[].label',
    '--target', 'label_de',
    '--dst', dst,
    '--prompt',
    'Translate the text to German. Reply with ONLY the German translation. No quotes. No explanation.',
    '--concurrency', '2',
    '--json',
  ], { timeout: 180_000 });

  const json = parseJson(r.stdout);
  if (r.status !== 0 || json?.ok !== true) {
    const combined = `${r.stdout}\n${r.stderr}`;
    if (/no API key|api key/i.test(combined)) {
      if (REQUIRE_LIVE) {
        assert(false, `live translate missing API key: ${combined.slice(0, 400)}`);
      } else {
        assert(true, 'live translate skipped (no API key configured)');
        return r.elapsedMs;
      }
    }
  }

  assert(r.status === 0 && json?.ok === true,
    `live translate ok (status=${r.status} errors=${json?.errors} err=${r.stderr.slice(0, 200)})`);
  assert(json?.transformed >= 2, `expected >=2 transforms (got ${json?.transformed})`);
  assert(json?.router, `router resolved (got ${json?.router})`);
  assert(json?.model, `model resolved (got ${json?.model})`);

  const out = readJson(dst);
  const items = out.ribbon.groups[0].items;
  assert(items[0].label === 'Home', 'source label preserved');
  assert(typeof items[0].label_de === 'string' && items[0].label_de.length > 0,
    `label_de set (got ${items[0].label_de})`);
  assert(items[0].label_de !== 'Home', `translation differs from English (got ${items[0].label_de})`);
  assert(!/^["']/.test(items[0].label_de), 'sanitize strips wrapping quotes');
  assert(items[0].label_de === items[2].label_de, 'duplicate Home shares one translation');
  assert(json?.cacheHits >= 1,
    `concurrent/coalesced cache hit expected for duplicate Home (got ${json?.cacheHits})`);
  console.log(`   live: router=${json.router} model=${json.model} cacheHits=${json.cacheHits}`);
  console.log(`   live: Home→${items[0].label_de}  Next→${items[1].label_de}`);
  console.log(`   live: timings=${JSON.stringify(json.timingsMs)}`);
  return r.elapsedMs;
}

function suiteLiveMergeJson(assert, exe) {
  console.log('\n-- Iterator: LIVE --merge-json multi-language object --\n');
  const src = join(RUN_DIR, 'live-merge.src.json');
  const dst = join(RUN_DIR, 'live-merge.i18n.json');
  writeFileSync(src, JSON.stringify({
    items: [
      { id: 'a', label: 'Home', label_de: 'OLD', description: 'Open home' },
      { id: 'b', label: 'Next', description: 'Go next' },
      { id: 'c', label: 'Home', description: 'duplicate for cache' },
    ],
  }, null, 2), 'utf8');

  const r = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'each',
    '--source', src,
    '--selector', '.items[].label',
    '--merge-json',
    '--merge-keys', 'label_de,label_fr',
    '--dst', dst,
    '--prompt',
    'Reply with ONLY a JSON object (no markdown, no prose) with exactly these string keys: '
      + '"label_de" (German) and "label_fr" (French). Values are translations of the text. '
      + 'Example shape: {"label_de":"…","label_fr":"…"}.',
    '--concurrency', '2',
    '--json',
  ], { timeout: 240_000 });

  const json = parseJson(r.stdout);
  if (r.status !== 0 || json?.ok !== true) {
    const combined = `${r.stdout}\n${r.stderr}`;
    if (/no API key|api key/i.test(combined)) {
      if (REQUIRE_LIVE) {
        assert(false, `live merge-json missing API key: ${combined.slice(0, 400)}`);
      } else {
        assert(true, 'live merge-json skipped (no API key configured)');
        return r.elapsedMs;
      }
    }
  }

  assert(r.status === 0 && json?.ok === true,
    `live merge-json ok (status=${r.status} errors=${json?.errors} err=${r.stderr.slice(0, 240)})`);
  assert(json?.mergeJson === true, 'stdout reports mergeJson');
  assert(json?.transformed >= 2, `expected >=2 merges (got ${json?.transformed})`);
  assert(json?.errors === 0, `no leaf errors (got ${json?.errors})`);

  const out = readJson(dst);
  const a = out.items[0];
  const b = out.items[1];
  const c = out.items[2];
  assert(a.label === 'Home', 'source label preserved');
  assert(a.description === 'Open home', 'unrelated sibling untouched');
  assert(typeof a.label_de === 'string' && a.label_de.length > 0, `label_de set (got ${a.label_de})`);
  assert(typeof a.label_fr === 'string' && a.label_fr.length > 0, `label_fr set (got ${a.label_fr})`);
  assert(a.label_de !== 'OLD', `label_de hard-overwrote OLD (got ${a.label_de})`);
  assert(a.label_de !== 'Home' && a.label_fr !== 'Home',
    `translations differ from English (de=${a.label_de} fr=${a.label_fr})`);
  assert(!a.label_de.includes('{') && !a.label_fr.includes('{'),
    'values are plain strings, not raw JSON blobs');
  assert(typeof b.label_de === 'string' && typeof b.label_fr === 'string',
    'Next got both language keys');
  assert(a.label_de === c.label_de && a.label_fr === c.label_fr,
    'duplicate Home shares coalesced merge object');
  assert(json?.cacheHits >= 1,
    `cache/coalesce hit expected for duplicate Home (got ${json?.cacheHits})`);
  console.log(`   live-merge: router=${json.router} model=${json.model} cacheHits=${json.cacheHits}`);
  console.log(`   live-merge: Home→ de=${a.label_de} fr=${a.label_fr}`);
  console.log(`   live-merge: Next→ de=${b.label_de} fr=${b.label_fr}`);
  console.log(`   live-merge: timings=${JSON.stringify(json.timingsMs)}`);
  return r.elapsedMs;
}

function writeReport(stats, wallMs, exe, suites, metricsCollector, startedAtIso) {
  const finishedAtIso = new Date().toISOString();
  const metrics = buildMetricsBundle(metricsCollector, startedAtIso, finishedAtIso);
  const failed = stats.failed ?? 0;
  const ok = failed === 0;

  let md = renderMarkdownReport({
    ok,
    passed: stats.passed,
    failed,
    meta: {
      cwd: process.cwd(),
      displayName: 'llm-iterator',
      testName: 'llm-iterator',
      writtenAt: finishedAtIso,
      exe,
      brand: brand?.name || 'tanit',
      mode: LIVE ? 'live' : 'loopback',
      wallClockMs: Math.round(wallMs * 100) / 100,
    },
    metrics,
    images: [],
    mediaSuites: suites,
    integrationNotes: [
      LIVE
        ? 'Live mode: leaf transforms via LLMClient::execute_chat (no agent loop).'
        : 'Dry-run mode. Use --mode live / npm run test:llm:iterator:live for real completions.',
      '--dst = whole JSON document; --target = optional sibling key; --merge-json = checked object hard-merge onto parent.',
      'Optimizations: fixed worker pool, deferred JSON writes, identical-input cache, leaf sanitize.',
    ],
  });

  const acc = isAccumulateMode() && getChapterTitle();
  md += acc
    ? '\n---\n\n*Per-run: `tests/test-report-last.md` - also a chapter in `tests/test-report-all.md`.*\n'
    : '\n---\n\n*Artifact: `tests/test-report-last.md`.*\n';
  writeFileSync(TEST_REPORT_PATH, md, 'utf8');
  if (acc) {
    appendMarkdownChapter(getChapterTitle(), md, { npmScript: getNpmScriptLabel() || undefined });
    console.log(`  (aggregated report chapter: ${TEST_REPORT_ALL_PATH})`);
  }
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

  console.log('\nLLM agent iterator tests\n');
  console.log(`   exe: ${exe}`);
  console.log(`   run: ${RUN_DIR}`);
  console.log(`   mode: ${LIVE ? 'live' : 'loopback/dry-run'}`);

  if (!hasEachSubcommand(exe)) {
    throw new Error('llm agent each not available (rebuild with FEATURE_JQ)');
  }

  {
    const t = suiteCoreProbe(stats.assert, exe);
    suites.push({ name: 'core iterator probe', steps: [{ label: 'products names', ms: t }] });
  }
  {
    const t = suiteEachInPlace(stats.assert, exe);
    suites.push({ name: 'each in-place', steps: [{ label: 'upper names', ms: t }] });
  }
  {
    const t = suiteEachTargetSibling(stats.assert, exe);
    suites.push({ name: 'each --target sibling', steps: [{ label: 'label_de', ms: t }] });
  }
  {
    const t = suiteEachCache(stats.assert, exe);
    suites.push({ name: 'each cache', steps: [{ label: 'duplicate Home', ms: t }] });
  }
  {
    const t = suiteEachLlmStub(stats.assert, exe);
    suites.push({ name: 'each llm-stub', steps: [{ label: 'prompt shape', ms: t }] });
  }
  {
    const t = suiteEachMergeJsonStub(stats.assert, exe);
    suites.push({ name: 'each merge-json stub', steps: [{ label: 'label_de/fr hard overwrite', ms: t }] });
  }
  if (LIVE) {
    const t = suiteLiveTranslate(stats.assert, exe);
    suites.push({ name: 'each live translate', steps: [{ label: 'label_de DE', ms: t }] });
    const tMerge = suiteLiveMergeJson(stats.assert, exe);
    suites.push({ name: 'each live merge-json', steps: [{ label: 'label_de+label_fr', ms: tMerge }] });
  }
} catch (e) {
  console.error(e);
  stats.assert(false, e?.stack || String(e));
} finally {
  writeReport(stats, performance.now() - t0, exe, suites, metricsCollector, startedAtIso);
}

console.log(`\n${stats.failed === 0 ? 'PASS' : 'FAIL'} llm:iterator: ${stats.passed} passed, ${stats.failed} failed`);
if (stats.failed > 0) process.exitCode = 1;

#!/usr/bin/env node
import { llmExe, prepareCliArgs } from './exe.mjs';
import { REPO as PACKAGE_ROOT } from './repo.mjs';
/**
 * System One decisions harness (`llm agent decide` CLI).
 *
 *   npm run test:llm:decide            # dry-run stub (default)
 *   npm run test:llm:decide:live       # one live OpenRouter call, skipped without a key
 *   npm run test:llm:decide:build
 *
 * HTTP `POST /v1/llm/decide` lives in `test:llm-server` /
 * `test:llm-server:decide:live` (test-llm-server.mjs), not here.
 *
 * Pair mode and --selector are the batch the decisions API actually
 * supports: one state, one expanded question per value, split by --chunk.
 * OpenRouter's async batch jobs do not accept this endpoint.
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
import {
  appendMarkdownChapter,
  getChapterTitle,
  getNpmScriptLabel,
  isAccumulateMode,
  TEST_REPORT_ALL_PATH,
} from '@polymech/tanit/tests/orchestrator/test-report-accumulate.mjs';
const __dirname = dirname(fileURLToPath(import.meta.url));
const STEMP_DIR = join(PACKAGE_ROOT, 'stemp');
const RUN_DIR = join(STEMP_DIR, 'agent-decide');
const TEST_REPORT_PATH = join(PACKAGE_ROOT, 'tests', 'test-report-last.md');
const DEFAULT_LOG_ARGS = ['--log-level', 'warn'];

const MODE = (() => {
  const idx = process.argv.indexOf('--mode');
  if (idx >= 0 && process.argv[idx + 1]) return process.argv[idx + 1].toLowerCase();
  const eq = process.argv.find((a) => a.startsWith('--mode='));
  if (eq) return eq.slice('--mode='.length).toLowerCase();
  return (process.env.PM_AGENT_DECIDE_MODE || 'loopback').toLowerCase();
})();
const LIVE = MODE === 'live';
const REQUIRE_LIVE = process.argv.includes('--require-live');

const PAIR_QUESTIONS = {
  same: {
    type: 'noul',
    instructions: 'Does `pair.left` equal `pair.right`?',
  },
  relation: {
    type: 'choice',
    instructions: 'How does `pair.left` relate to `pair.right`?',
    criteria: {
      same: 'They are the same text',
      different: 'They differ',
    },
  },
  gap: {
    type: 'score',
    instructions: 'How far apart are `pair.left` and `pair.right`?',
    criteria: ['identical', 'unrelated'],
  },
};

const DOC_QUESTIONS = {
  bucket: {
    type: 'choice',
    instructions: 'Which bucket fits this document?',
    criteria: {
      alpha: 'First bucket',
      beta: 'Second bucket',
    },
  },
};

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

/** Rows for the Decisions performance table in reports.js. */
const perfRows = [];
const accuracyReports = [];
const CAPABILITY_FIXTURE = join(__dirname, 'fixtures', 'capability-adjudication-video.json');

function notePerf(label, ms, json, extra = {}) {
  const wall = roundMs(ms);
  const expanded = Number(json?.expandedQuestions);
  const row = {
    label,
    ms: wall,
    requests: json?.requests ?? null,
    expanded: Number.isFinite(expanded) ? expanded : null,
    pairs: json?.pairs || 0,
    items: json?.items || 0,
    msPerQuestion: Number.isFinite(expanded) && expanded > 0 ? roundMs(wall / expanded) : null,
    inputTokens: json?.usage?.inputTokens ?? null,
    outputTokens: json?.usage?.outputTokens ?? null,
    cost: json?.usage?.hasOwnProperty('cost') ? json.usage.cost : null,
    model: json?.resolvedModel || '',
    fitAccuracy: extra.fitAccuracy ?? null,
    supportAccuracy: extra.supportAccuracy ?? null,
  };
  if (json?.ok === true) perfRows.push(row);
  const parts = [];
  if (json?.provider) parts.push(String(json.provider));
  if (row.requests != null) parts.push(`${row.requests} req`);
  if (row.expanded != null) parts.push(`${row.expanded} expanded`);
  if (row.pairs) parts.push(`${row.pairs} pairs`);
  if (row.items) parts.push(`${row.items} items`);
  if (row.msPerQuestion != null) parts.push(`${row.msPerQuestion} ms/q`);
  if (row.inputTokens) parts.push(`${row.inputTokens} in / ${row.outputTokens ?? 0} out`);
  if (row.cost) parts.push(`cost ${row.cost}`);
  if (row.model && row.model !== 'stub') parts.push(row.model);
  if (extra.fitAccuracy != null) {
    parts.push(`fit ${(extra.fitAccuracy * 100).toFixed(1)}%`);
    parts.push(`supports ${(extra.supportAccuracy * 100).toFixed(1)}%`);
  }
  return { label, ms: wall, detail: parts.join(' · ') };
}

function suiteRecord(name, steps, llmLive) {
  const totalMs = roundMs(steps.reduce((sum, step) => sum + (Number(step.ms) || 0), 0));
  const row = { name, totalMs, steps };
  if (llmLive) row.llmLive = llmLive;
  return row;
}

function readJson(path) {
  return JSON.parse(readFileSync(path, 'utf8'));
}

function writeJson(path, value) {
  writeFileSync(path, `${JSON.stringify(value, null, 2)}\n`, 'utf8');
}

function hasDecideSubcommand(exe) {
  const help = runCli(exe, [...DEFAULT_LOG_ARGS, 'llm', 'agent', 'decide', '--help'], { timeout: 30_000 });
  return help.status === 0 && /--pairs|--questions/i.test(`${help.stdout}\n${help.stderr}`);
}

function pairDoc() {
  return {
    items: [
      { id: 'a', left: 'apple', right: 'apple' },
      { id: 'b', left: 'apple', right: 'orange' },
      { id: 'c', left: 'orange', right: 'orange' },
    ],
  };
}

function assertPairAnswers(assert, items) {
  assert(items[0].decisions?.same?.noul === 1, `a noul 1 (got ${items[0].decisions?.same?.noul})`);
  assert(items[0].decisions?.relation?.choice === 'same', `a choice same (got ${items[0].decisions?.relation?.choice})`);
  assert(items[0].decisions?.gap?.score === 0, `a score 0 (got ${items[0].decisions?.gap?.score})`);
  assert(items[1].decisions?.same?.noul === 0, `b noul 0 (got ${items[1].decisions?.same?.noul})`);
  assert(items[1].decisions?.relation?.choice === 'different',
    `b choice different (got ${items[1].decisions?.relation?.choice})`);
  assert(items[1].decisions?.gap?.score === 1, `b score 1 (got ${items[1].decisions?.gap?.score})`);
  assert(items[2].decisions?.same?.noul === 1 && items[2].decisions?.relation?.choice === 'same',
    'c matches the equal pair');
  assert(items[0].left === 'apple' && items[1].right === 'orange', 'source fields kept');
}

function suitePairsChunk(assert, exe, chunk, expectedRequests) {
  console.log(`\n-- Decide: dry-run pairs chunk=${chunk} --\n`);
  const src = join(RUN_DIR, `pairs-${chunk}.src.json`);
  const questions = join(RUN_DIR, 'pair-questions.json');
  const dst = join(RUN_DIR, `pairs-${chunk}.out.json`);
  writeJson(src, pairDoc());
  writeJson(questions, PAIR_QUESTIONS);
  const r = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'decide',
    '--source', src,
    '--questions', questions,
    '--pairs', '.items[]',
    '--chunk', String(chunk),
    '--dst', dst,
    '--dry-run',
    '--json',
  ]);
  const json = parseJson(r.stdout);
  assert(r.status === 0 && json?.ok === true, `pairs chunk ${chunk} ok (status=${r.status})`);
  assert(json?.provider === 'stub' && json?.dryRun === true, 'stub provider');
  assert(json?.batched === true, 'pair mode reports batched');
  assert(json?.pairs === 3, `3 pairs (got ${json?.pairs})`);
  assert(json?.questions === 3, `3 question templates (got ${json?.questions})`);
  assert(json?.expandedQuestions === 9, `9 expanded questions (got ${json?.expandedQuestions})`);
  assert(json?.requests === expectedRequests,
    `requests ${expectedRequests} (got ${json?.requests})`);
  assert(existsSync(dst), '--dst written');
  const out = readJson(dst);
  assertPairAnswers(assert, out.items);
  return { steps: [notePerf(`pairs chunk ${chunk}`, r.elapsedMs, json)] };
}

function suiteZip(assert, exe) {
  console.log('\n-- Decide: dry-run --left/--right zip --\n');
  const src = join(RUN_DIR, 'zip.src.json');
  const questions = join(RUN_DIR, 'pair-questions.json');
  const dst = join(RUN_DIR, 'zip.out.json');
  writeJson(src, pairDoc());
  const r = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'decide',
    '-i', src,
    '--questions', questions,
    '--left', '.items[].left',
    '--right', '.items[].right',
    '-o', dst,
    '--dry-run',
    '--json',
  ]);
  const json = parseJson(r.stdout);
  assert(r.status === 0 && json?.ok === true, `zip ok (status=${r.status} err=${json?.error || r.stderr})`);
  assert(json?.requests === 1, `one request for 3 pairs x 3 questions under default chunk (got ${json?.requests})`);
  const out = readJson(dst);
  assertPairAnswers(assert, out.items);
  return { steps: [notePerf('left/right zip', r.elapsedMs, json)] };
}

function suiteDocument(assert, exe) {
  console.log('\n-- Decide: dry-run whole document --\n');
  const src = join(RUN_DIR, 'doc.src.json');
  const questions = join(RUN_DIR, 'doc-questions.json');
  const dst = join(RUN_DIR, 'doc.out.json');
  writeJson(src, { title: 'Ticket', body: 'The API is down' });
  writeJson(questions, DOC_QUESTIONS);
  const r = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'decide',
    '--source', src,
    '--questions', questions,
    '--dst', dst,
    '--dry-run',
    '--json',
  ]);
  const json = parseJson(r.stdout);
  assert(r.status === 0 && json?.ok === true, `document ok (status=${r.status})`);
  assert(json?.batched === false && json?.pairs === 0 && json?.requests === 1, 'single-state request');
  const out = readJson(dst);
  assert(out.title === 'Ticket', 'document fields kept');
  assert(out.decisions?.bucket?.choice === 'alpha',
    `first criteria key (got ${out.decisions?.bucket?.choice})`);
  assert(out.decisions?.bucket?.confidence === 0.5, 'document stub confidence is 0.5');
  return { steps: [notePerf('whole document', r.elapsedMs, json)] };
}

const ITEM_QUESTIONS = {
  what: {
    type: 'choice',
    instructions: 'What is `item.name`?',
    criteria: {
      apple: 'A fruit',
      wrench: 'A tool',
      other: 'Something else',
    },
  },
  named: {
    type: 'noul',
    instructions: 'Does `item` have a name?',
  },
};

function suiteSelector(assert, exe) {
  console.log('\n-- Decide: dry-run --selector items --\n');
  const src = join(RUN_DIR, 'items.src.json');
  const questions = join(RUN_DIR, 'item-questions.json');
  const dst = join(RUN_DIR, 'items.out.json');
  const stringsDst = join(RUN_DIR, 'items-strings.out.json');
  const chunkDst = join(RUN_DIR, 'items-chunk.out.json');
  writeJson(src, {
    rows: [
      { id: 'a', name: 'apple' },
      { id: 'b', name: 'wrench' },
    ],
  });
  writeJson(questions, ITEM_QUESTIONS);

  const objects = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'decide',
    '--source', src,
    '--questions', questions,
    '--selector', '.rows[]',
    '--dst', dst,
    '--dry-run',
    '--json',
  ]);
  const json = parseJson(objects.stdout);
  assert(objects.status === 0 && json?.ok === true, `selector objects ok (status=${objects.status})`);
  assert(json?.batched === true && json?.items === 2 && json?.pairs === 0, 'item batch, not pairs');
  assert(json?.expandedQuestions === 4 && json?.requests === 1,
    `2 items x 2 questions in one request (got expanded=${json?.expandedQuestions} requests=${json?.requests})`);
  const out = readJson(dst);
  assert(out.rows[0].name === 'apple' && out.rows[1].id === 'b', 'source fields kept');
  assert(out.rows[0].decisions?.what?.choice === 'apple',
    `a choice apple (got ${out.rows[0].decisions?.what?.choice})`);
  assert(out.rows[1].decisions?.what?.choice === 'wrench',
    `b choice wrench (got ${out.rows[1].decisions?.what?.choice})`);
  assert(out.rows[0].decisions?.named?.noul === 1 && out.rows[1].decisions?.named?.noul === 1,
    'object items are present');
  assert(out.rows[0].decisions?.what?.confidence === 1, 'item stub confidence is 1');

  const strings = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'decide',
    '--source', src,
    '--questions', questions,
    '--selector', '.rows[].name',
    '--dst', stringsDst,
    '--dry-run',
    '--json',
  ]);
  const stringsJson = parseJson(strings.stdout);
  assert(strings.status === 0 && stringsJson?.ok === true, `selector strings ok (status=${strings.status})`);
  const stringsOut = readJson(stringsDst);
  assert(stringsOut.rows[0].decisions?.what?.choice === 'apple'
    && stringsOut.rows[1].decisions?.what?.choice === 'wrench',
    'string matches write on the parent');
  assert(stringsOut.rows[0].name === 'apple', 'selected string left in place');

  const chunked = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'decide',
    '--source', src,
    '--questions', questions,
    '--selector', '.rows[]',
    '--chunk', '2',
    '--dst', chunkDst,
    '--dry-run',
    '--json',
  ]);
  const chunkJson = parseJson(chunked.stdout);
  assert(chunked.status === 0 && chunkJson?.requests === 2,
    `chunk 2 splits 2 items x 2 questions (got ${chunkJson?.requests})`);
  assert(chunkJson?.expandedQuestions === 4, `expanded stays 4 (got ${chunkJson?.expandedQuestions})`);

  const mixed = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'decide',
    '--source', src,
    '--questions', questions,
    '--selector', '.rows[]',
    '--pairs', '.rows[]',
    '--dry-run',
    '--json',
  ]);
  assert(mixed.status !== 0, '--selector with --pairs fails');
  assert(/selector/i.test(`${mixed.stderr}\n${mixed.stdout}`), 'selector conflict names --selector');
  return {
    steps: [
      notePerf('selector objects', objects.elapsedMs, json),
      notePerf('selector strings', strings.elapsedMs, stringsJson),
      notePerf('selector chunk 2', chunked.elapsedMs, chunkJson),
      { label: 'selector conflicts with --pairs', ms: roundMs(mixed.elapsedMs), detail: 'rejected' },
    ],
  };
}

function suiteRejects(assert, exe) {
  console.log('\n-- Decide: reject bad pairs, openai, jev without base --\n');
  const src = join(RUN_DIR, 'bad.src.json');
  const questions = join(RUN_DIR, 'pair-questions.json');
  writeJson(src, { items: [{ id: 'a', name: 'only' }] });
  writeJson(questions, PAIR_QUESTIONS);
  const bad = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'decide',
    '--source', src,
    '--questions', questions,
    '--pairs', '.items[]',
    '--dry-run',
    '--json',
  ]);
  assert(bad.status !== 0, 'missing left/right fails');
  assert(/missing/i.test(`${bad.stderr}\n${bad.stdout}`), 'missing-key error');

  const openai = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'decide',
    '--source', src,
    '--questions', questions,
    '--provider', 'openai',
    '--json',
  ]);
  assert(openai.status !== 0, 'openai provider fails closed');
  assert(/not implemented/i.test(`${openai.stderr}\n${openai.stdout}`), 'openai adapter is reserved');

  const jev = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'decide',
    '--source', src,
    '--questions', questions,
    '--provider', 'jev',
    '--json',
  ]);
  assert(jev.status !== 0, 'jev without --base-url fails');
  assert(/base-url/i.test(`${jev.stderr}\n${jev.stdout}`), 'jev asks for --base-url');
  return {
    steps: [
      { label: 'missing left/right', ms: roundMs(bad.elapsedMs), detail: 'rejected' },
      { label: 'openai reserved', ms: roundMs(openai.elapsedMs), detail: 'not implemented' },
      { label: 'jev without base-url', ms: roundMs(jev.elapsedMs), detail: 'rejected' },
    ],
  };
}

function suiteTokenBudget(assert, exe) {
  console.log('\n-- Decide: token ceiling splits fat items --\n');
  const body = 'x'.repeat(6000);
  const src = join(RUN_DIR, 'tokens.src.json');
  const questions = join(RUN_DIR, 'tokens-questions.json');
  const dst = join(RUN_DIR, 'tokens.out.json');
  writeJson(src, {
    items: [0, 1, 2, 3].map((i) => ({ id: `f${i}`, body })),
  });
  writeJson(questions, {
    present: { type: 'noul', instructions: 'Is `item.body` present?' },
  });
  const packed = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'decide',
    '--source', src,
    '--questions', questions,
    '--selector', '.items[]',
    '--chunk', '32',
    '--max-tokens', '3000',
    '--dst', dst,
    '--dry-run',
    '--json',
  ]);
  const json = parseJson(packed.stdout);
  assert(packed.status === 0 && json?.ok === true, `token pack ok (status=${packed.status})`);
  assert(json.items === 4, `4 items (got ${json.items})`);
  assert(json.requests === 4, `one fat item per request (got ${json.requests})`);
  assert(json.expandedQuestions === 4, `4 expanded questions (got ${json.expandedQuestions})`);
  assert(json.maxTokens === 3000, `maxTokens echoed (got ${json.maxTokens})`);

  const huge = join(RUN_DIR, 'tokens-huge.src.json');
  writeJson(huge, { items: [{ id: 'too-big', body: 'y'.repeat(20000) }] });
  const rejected = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'decide',
    '--source', huge,
    '--questions', questions,
    '--selector', '.items[]',
    '--max-tokens', '500',
    '--dry-run',
    '--json',
  ]);
  assert(rejected.status !== 0, 'one value over the ceiling fails');
  assert(/not split/i.test(`${rejected.stderr}\n${rejected.stdout}`), 'error says a value is not split');

  const doc = join(RUN_DIR, 'tokens-doc.src.json');
  writeJson(doc, { note: 'z'.repeat(8000) });
  const docRejected = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'decide',
    '--source', doc,
    '--questions', questions,
    '--max-tokens', '500',
    '--dry-run',
    '--json',
  ]);
  assert(docRejected.status !== 0, 'oversized document fails');
  assert(/--selector/i.test(`${docRejected.stderr}\n${docRejected.stdout}`), 'document error points at --selector');
  return {
    steps: [
      notePerf('token ceiling 4 fat items', packed.elapsedMs, json),
      { label: 'value over ceiling', ms: roundMs(rejected.elapsedMs), detail: 'rejected' },
      { label: 'document over ceiling', ms: roundMs(docRejected.elapsedMs), detail: 'rejected' },
    ],
  };
}

function suiteLivePairs(assert, exe) {
  console.log('\n-- Decide: LIVE one pair batch --\n');
  const src = join(RUN_DIR, 'live.src.json');
  const questions = join(RUN_DIR, 'live-questions.json');
  const dst = join(RUN_DIR, 'live.out.json');
  writeJson(src, {
    items: [
      { id: 'same', left: 'apple', right: 'apple' },
      { id: 'diff', left: 'apple', right: 'wrench' },
    ],
  });
  writeJson(questions, {
    same_fruit: {
      type: 'noul',
      instructions: 'Does `pair.left` name the same fruit as `pair.right`?',
    },
    relation: {
      type: 'choice',
      instructions: 'How does `pair.left` relate to `pair.right`?',
      criteria: {
        same: 'They are the same thing',
        different: 'They are different things',
      },
    },
  });
  const r = runCli(exe, [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'decide',
    '--source', src,
    '--questions', questions,
    '--pairs', '.items[]',
    '--provider', 'openrouter',
    '--dst', dst,
    '--json',
  ], { timeout: 120_000 });
  const json = parseJson(r.stdout);
  const combined = `${r.stdout}\n${r.stderr}`;
  if (r.status !== 0 || json?.ok !== true) {
    if (/no API key|api key/i.test(combined)) {
      if (REQUIRE_LIVE) {
        assert(false, `live decide missing API key: ${combined.slice(0, 400)}`);
      } else {
        assert(true, 'live decide skipped (no API key configured)');
      }
      return { steps: [notePerf('openrouter pair batch', r.elapsedMs, json)] };
    }
  }
  assert(r.status === 0 && json?.ok === true,
    `live decide ok (status=${r.status} err=${combined.slice(0, 240)})`);
  assert(json?.requests === 1, `both pairs in one request (got ${json?.requests})`);
  assert(json?.provider === 'openrouter', `provider openrouter (got ${json?.provider})`);
  assert(typeof json?.resolvedModel === 'string' && json.resolvedModel.length > 0, 'resolved model');
  assert(typeof json?.usage?.inputTokens === 'number', 'usage.inputTokens');
  const out = readJson(dst);
  const same = out.items[0].decisions?.same_fruit?.noul;
  const diff = out.items[1].decisions?.same_fruit?.noul;
  assert(typeof same === 'number' && typeof diff === 'number',
    `noul numbers (same=${same} diff=${diff})`);
  assert(same > diff, `equal pair outranks the mismatch (same=${same} diff=${diff})`);
  assert(typeof out.items[0].decisions?.relation?.choice === 'string'
    && out.items[0].decisions.relation.choice.length > 0, 'choice answer present');
  console.log(`   live: model=${json.resolvedModel} same=${same} diff=${diff} cost=${json.usage?.cost}`);
  const choice = out.items[0].decisions?.relation?.choice;
  return {
    steps: [notePerf('openrouter pair batch', r.elapsedMs, json)],
    llmLive: {
      title: 'two pairs, one state',
      prompt: 'same_fruit noul + relation choice on apple/apple vs apple/wrench',
      expected: 'equal-pair noul > mismatch noul',
      actual: `same=${same} diff=${diff} choice=${choice} model=${json.resolvedModel}`,
      roundTripMs: r.elapsedMs,
    },
  };
}

const CAPABILITY_QUESTIONS = {
  fit: {
    type: 'choice',
    instructions: 'The user asked `item.phrase`. Product evidence is `item.evidence`. Matched outcome is `item.outcome`. Which fit is honest?',
    criteria: {
      direct: 'One listed capability does that job by itself.',
      composed: 'The listed capabilities together do that job, as a screen or video workflow would.',
      editorial: 'The evidence is only adjacent. It can support a comparison, not the requested job.',
      unsupported: 'The evidence does not support the request.',
      unknown: 'There is not enough evidence to judge.',
    },
  },
  supports: {
    type: 'noul',
    instructions: 'Can this product evidence actually perform the job in `item.phrase`, rather than only sit next to it?',
  },
};

function scoreCapability(fixture, outItems) {
  const byId = new Map(outItems.map((row) => [row.id, row]));
  const misses = [];
  const byExpected = {};
  let fitCorrect = 0;
  let supportCorrect = 0;
  for (const item of fixture.items) {
    const got = byId.get(item.id);
    const choice = got?.decisions?.fit?.choice ?? '';
    const noul = got?.decisions?.supports?.noul;
    const fitHit = choice === item.expectedFit;
    const expectSupport = item.expectedFit === 'composed' || item.expectedFit === 'direct';
    const supportHit = typeof noul === 'number' && (noul >= 0.5) === expectSupport;
    if (fitHit) fitCorrect += 1;
    if (supportHit) supportCorrect += 1;
    const bucket = byExpected[item.expectedFit] ?? { n: 0, fitCorrect: 0, supportCorrect: 0 };
    bucket.n += 1;
    if (fitHit) bucket.fitCorrect += 1;
    if (supportHit) bucket.supportCorrect += 1;
    byExpected[item.expectedFit] = bucket;
    if (!fitHit || !supportHit) {
      misses.push({
        phrase: item.phrase,
        expectedFit: item.expectedFit,
        choice,
        noul: typeof noul === 'number' ? noul : null,
        fitHit,
        supportHit,
      });
    }
  }
  const n = fixture.items.length;
  return {
    items: n,
    fitCorrect,
    fitAccuracy: n ? fitCorrect / n : 0,
    supportCorrect,
    supportAccuracy: n ? supportCorrect / n : 0,
    byExpected,
    misses,
  };
}

function suiteCapability(assert, exe, { live }) {
  console.log(`\n-- Decide: capability adjudication (${live ? 'LIVE' : 'dry-run'}) --\n`);
  const fixture = readJson(CAPABILITY_FIXTURE);
  const src = join(RUN_DIR, live ? 'capability-live.src.json' : 'capability.src.json');
  const questions = join(RUN_DIR, 'capability-questions.json');
  const dst = join(RUN_DIR, live ? 'capability-live.out.json' : 'capability.out.json');
  writeJson(src, {
    items: fixture.items.map((item) => ({
      id: item.id,
      phrase: item.phrase,
      outcome: item.outcome,
      evidence: item.evidence,
    })),
  });
  writeJson(questions, CAPABILITY_QUESTIONS);
  const args = [
    ...DEFAULT_LOG_ARGS,
    'llm', 'agent', 'decide',
    '--source', src,
    '--questions', questions,
    '--selector', '.items[]',
    '--dst', dst,
    '--json',
  ];
  if (!live) args.push('--dry-run');
  else args.push('--provider', 'openrouter');
  const r = runCli(exe, args, { timeout: live ? 180_000 : 120_000 });
  const json = parseJson(r.stdout);
  if (live && (r.status !== 0 || json?.ok !== true)) {
    const combined = `${r.stdout}\n${r.stderr}`;
    if (/no API key|api key/i.test(combined)) {
      if (REQUIRE_LIVE) assert(false, `capability live missing API key: ${combined.slice(0, 400)}`);
      else assert(true, 'capability live skipped (no API key configured)');
      return { steps: [{ label: 'capability live skipped', ms: roundMs(r.elapsedMs), detail: 'no API key' }] };
    }
  }
  assert(r.status === 0 && json?.ok === true, `capability ${live ? 'live' : 'stub'} ok (status=${r.status})`);
  assert(json?.items === fixture.items.length, `15 items (got ${json?.items})`);
  assert(json?.requests === 1, `one request for 15 items x 2 questions (got ${json?.requests})`);
  const out = readJson(dst);
  const scored = scoreCapability(fixture, out.items);
  const label = live ? 'live jev' : 'stub';
  accuracyReports.push({ label: `capability fit vs deterministic (${label})`, mode: label, ...scored });
  console.log(`   capability ${label}: fit ${scored.fitCorrect}/${scored.items} (${(scored.fitAccuracy * 100).toFixed(1)}%) supports ${scored.supportCorrect}/${scored.items} (${(scored.supportAccuracy * 100).toFixed(1)}%)`);
  for (const miss of scored.misses) {
    console.log(`   miss: expected=${miss.expectedFit} choice=${miss.choice || '—'} noul=${miss.noul} ${miss.phrase}`);
  }
  assert(scored.items === fixture.items.length, 'every fixture row was scored');
  const choices = new Set(out.items.map((row) => row.decisions?.fit?.choice));
  if (!live) {
    assert(choices.size === 1, `stub uses one choice for every row (got ${[...choices].join(',')})`);
  } else {
    assert([...choices].every((choice) => typeof choice === 'string' && choice.length > 0),
      'live fit choice present on every row');
  }
  return {
    steps: [notePerf(live ? 'capability live' : 'capability stub', r.elapsedMs, json, scored)],
    llmLive: live ? {
      title: 'capability adjudication, 15 phrases',
      prompt: 'fit choice + supports noul, expectedFit withheld',
      expected: 'match deterministic composed/editorial',
      actual: `fit ${scored.fitCorrect}/${scored.items} supports ${scored.supportCorrect}/${scored.items} model=${json.resolvedModel}`,
      roundTripMs: r.elapsedMs,
    } : undefined,
  };
}

async function writeReport(stats, wallMs, exe, suites, metricsCollector, startedAtIso) {
  const finishedAtIso = new Date().toISOString();
  const metrics = buildMetricsBundle(metricsCollector, startedAtIso, finishedAtIso);
  const failed = stats.failed ?? 0;
  const ok = failed === 0;
  const mode = LIVE ? 'live' : 'dry-run';
  const payload = {
    ok,
    passed: stats.passed,
    failed,
    meta: {
      cwd: process.cwd(),
      displayName: `llm-decide ${mode}`,
      testName: LIVE ? 'llm-decide-live' : 'llm-decide-dry',
      writtenAt: finishedAtIso,
      exe,
      brand: brand?.name || 'tanit',
      mode,
      wallClockMs: Math.round(wallMs * 100) / 100,
    },
    metrics,
    images: [],
    mediaSuites: suites,
    decisions: { mode, rows: perfRows, accuracy: accuracyReports },
    integrationNotes: [
      LIVE
        ? 'Live mode: one OpenRouter decisions call (~typesafe/jev-latest) over two pairs. ms/question is wall time divided by expanded questions.'
        : 'Dry-run mode. Use --mode live / npm run test:llm:decide:live for a real decisions call.',
      'Pair mode and --selector pack values into one state and expand questions. --chunk caps questions. --max-tokens (default 24000, chars/3) caps the body under Jev 32k. One value is not split.',
      'openai is reserved. jev uses the same JSON body and requires --base-url.',
    ],
  };
  let md = renderMarkdownReport(payload);
  const acc = isAccumulateMode() && getChapterTitle();
  md += acc
    ? '\n---\n\n*Per-run: `tests/test-report-last.md` - also a chapter in `tests/test-report-all.md`.*\n'
    : '\n---\n\n*Artifact: `tests/test-report-last.md`.*\n';
  writeFileSync(TEST_REPORT_PATH, md, 'utf8');
  const stamped = await writeTestReports(payload.meta.testName, payload, { cwd: PACKAGE_ROOT });
  console.log(`report: ${stamped.mdPath}`);
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

  console.log('\nLLM agent decide tests\n');
  console.log(`   exe: ${exe}`);
  console.log(`   run: ${RUN_DIR}`);
  console.log(`   mode: ${LIVE ? 'live' : 'loopback/dry-run'}`);

  if (!hasDecideSubcommand(exe)) {
    throw new Error('llm agent decide not available (rebuild with FEATURE_JQ)');
  }

  {
    const result = suitePairsChunk(stats.assert, exe, 6, 2);
    suites.push(suiteRecord('decide pairs chunk 6', result.steps));
  }
  {
    const result = suitePairsChunk(stats.assert, exe, 3, 3);
    suites.push(suiteRecord('decide pairs chunk 3', result.steps));
  }
  {
    const result = suiteZip(stats.assert, exe);
    suites.push(suiteRecord('decide zip', result.steps));
  }
  {
    const result = suiteDocument(stats.assert, exe);
    suites.push(suiteRecord('decide document', result.steps));
  }
  {
    const result = suiteSelector(stats.assert, exe);
    suites.push(suiteRecord('decide selector', result.steps));
  }
  {
    const result = suiteRejects(stats.assert, exe);
    suites.push(suiteRecord('decide rejects', result.steps));
  }
  {
    const result = suiteTokenBudget(stats.assert, exe);
    suites.push(suiteRecord('decide token ceiling', result.steps));
  }
  {
    const result = suiteCapability(stats.assert, exe, { live: false });
    suites.push(suiteRecord('decide capability stub', result.steps));
  }
  if (LIVE) {
    const result = suiteLivePairs(stats.assert, exe);
    suites.push(suiteRecord('decide live pairs', result.steps, result.llmLive));
    const capability = suiteCapability(stats.assert, exe, { live: true });
    suites.push(suiteRecord('decide capability live', capability.steps, capability.llmLive));
  }
} catch (e) {
  console.error(e);
  stats.assert(false, e?.stack || String(e));
} finally {
  await writeReport(stats, performance.now() - t0, exe, suites, metricsCollector, startedAtIso);
}

console.log(`\n${stats.failed === 0 ? 'PASS' : 'FAIL'} llm:decide: ${stats.passed} passed, ${stats.failed} failed`);
if (stats.failed > 0) process.exitCode = 1;

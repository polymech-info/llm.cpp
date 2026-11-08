#!/usr/bin/env node
import { llmExe } from './exe.mjs';
/**
 * Agent memory strategy harness — loopback (offline) or live (real LLM).
 *
 * Invoked via:
 *   npm run test:agent:memory:harness              # loopback (default)
 *   npm run test:agent:memory:harness:live         # live + quick preset
 *   npm run test:agent:memory:harness:live -- --preset superfast
 *   npm run test:agent:memory:harness:live:local-gemma
 *
 * Shared bootstrap lives in ./test-agent-commons.mjs
 *
 * Flags:
 *   --mode loopback|live
 *   --preset <chat-preset>
 *   --only loopback,memory-tools,memory-find-natural,memory-recall-pending,memory-recall-bench,tool-memory-bench
 *   --bench-size=200|1000|10000   (memory-recall-bench corpus size; default 200)
 *   --memory-features=<hex>
 *   --tool-memory-phase=baseline|candidate
 *   --require-live
 */

import { spawnSync } from 'node:child_process';
import {
  cpSync, existsSync, mkdirSync, readFileSync, readdirSync, unlinkSync, writeFileSync,
} from 'node:fs';
import { join } from 'node:path';

import { cliExePath, createAssert, brand, securityExePath } from '../../../../tests/orchestrator/test-commons.js';
import { createMetricsCollector } from '../../../../tests/orchestrator/reports.js';
import {
  TEST_REPORT_PATH,
  bootstrapAgentHarness,
  createAgentHarnessOptions,
  createAgentTestReport,
  finalText,
  generateHarnessSessionReports,
  printAgentHarnessBanner,
  runAgentRound,
  runHarnessSuites,
  toolCalls,
  toolResultEnvelope,
  truncateText,
  writeAgentTestReportFile,
  systemPromptFromBundle,
  memoryUpdatesFromBundle,
  recallTelemetryFromBundle,
} from './test-agent-commons.mjs';
import {
  RECALL_RICHNESS_PRESETS,
  parseBenchSize,
  seedMemoryCorpus,
} from './seed-memory-corpus.mjs';
import {
  createStdioMcpFixtureConfig,
  mcpExecuteGrant,
  mcpToolName,
} from './lib/mcp-fixture.mjs';

const LOOPBACK_PRESET = 'memory-harness-loopback';
const SUITE_NAMES = [
  'loopback',
  'memory-tools',
  'exact-turn-retrieval',
  'memory-find-natural',
  'memory-cross-session',
  'memory-recall-pending',
  'memory-recall-consumer',
  'memory-artifacts',
  'skill-resources',
  'continuation-context',
  'memory-recall-bench',
  'tool-memory-bench',
  'tool-memory-daily',
];
const BENCH_SIZE = parseBenchSize();
const TOOL_MEMORY_PHASE = (() => {
  const arg = process.argv.find((v) => v.startsWith('--tool-memory-phase='));
  const value = arg ? arg.slice('--tool-memory-phase='.length)
    : (process.env.PM_TOOL_MEMORY_BENCH_PHASE || 'baseline');
  return value === 'candidate' ? 'candidate' : 'baseline';
})();

const harness = createAgentHarnessOptions({
  harnessSubdir: 'agent-memory-harness',
  loopbackPresetId: LOOPBACK_PRESET,
  displayName: 'agent-memory',
});

function suiteLoopbackSmoke(rep, assert, ctx) {
  if (ctx.mode !== 'loopback') {
    rep.note('loopback smoke skipped in live mode');
    return;
  }
  console.log('\n-- loopback smoke --\n');
  rep.beginSuite('Loopback smoke');

  const r = runAgentRound({
    ...ctx,
    prompt: 'loopback-smoke',
    script: [{ text: 'hello from memory harness' }],
    roundKey: 'loopback-smoke',
    roundMeta: { suite: 'loopback-smoke', index: 1, name: 'immediate text' },
  });
  rep.step('immediate text', r.ms, `ok=${r.json?.ok} text=${r.json?.final_text}`);
  assert(r.status === 0, 'loopback-smoke: exits 0');
  assert(r.json?.ok === true, 'loopback-smoke: result.ok=true');
  assert(r.json?.final_text === 'hello from memory harness',
    `loopback-smoke: final_text matches script (got ${r.json?.final_text})`);
  assert(r.json?.iterations === 1, `loopback-smoke: 1 iteration (got ${r.json?.iterations})`);

  rep.endSuite();
}

function suiteMemoryTools(rep, assert, ctx) {
  const label = ctx.mode === 'live' ? 'live' : 'loopback';
  console.log(`\n-- memory tools (${label}) --\n`);
  rep.beginSuite(`Memory tools (${label})`);

  const sessionId = `${ctx.runId}-memory-tools`;
  const isLive = ctx.mode === 'live';

  const r1 = runAgentRound({
    ...ctx,
    sessionId,
    prompt: isLive
      ? 'Use the memory_write tool to store exactly: {"codename":"Nighthawk","phase":2}. After the tool succeeds reply: MEMORY_WRITTEN'
      : 'memory-write-test',
    script: isLive ? undefined : [
      {
        tool_calls: [{
          id: 'tc_mw1',
          name: 'memory_write',
          arguments: { state: { codename: 'Nighthawk', phase: 2 } },
        }],
      },
      { text: 'MEMORY_WRITTEN codename=Nighthawk phase=2' },
    ],
    roundKey: 'memory-tools-r01',
    roundMeta: { suite: 'memory-tools', index: 1, name: 'memory_write' },
  });
  rep.step('memory_write', r1.ms, `ok=${r1.json?.ok}`);
  assert(r1.status === 0, 'memory-tools r1: exits 0');
  assert(toolCalls(r1.bundlePath).includes('memory_write'), 'memory-tools r1: called memory_write');
  const writeResult = toolResultEnvelope(r1.bundlePath, 'memory_write');
  assert(writeResult?.ok === true, 'memory-tools r1: memory_write ok=true');
  assert(writeResult?.saved?.codename === 'Nighthawk', 'memory-tools r1: saved codename=Nighthawk');
  if (isLive) {
    assert(/MEMORY_WRITTEN/i.test(finalText(r1.bundlePath)),
      `memory-tools r1 live: final text contains MEMORY_WRITTEN (got ${truncateText(finalText(r1.bundlePath))})`);
  }

  const r2 = runAgentRound({
    ...ctx,
    sessionId,
    prompt: isLive
      ? 'Use the memory_read tool to read your stored memory. Then reply: RECALL codename=<value> phase=<value>'
      : 'memory-read-test',
    script: isLive ? undefined : [
      { tool_calls: [{ id: 'tc_mr1', name: 'memory_read', arguments: {} }] },
      { text: 'MEMORY_READ_DONE' },
    ],
    roundKey: 'memory-tools-r02',
    roundMeta: { suite: 'memory-tools', index: 2, name: 'memory_read' },
  });
  rep.step('memory_read', r2.ms, `ok=${r2.json?.ok}`);
  assert(r2.status === 0, 'memory-tools r2: exits 0');
  assert(toolCalls(r2.bundlePath).includes('memory_read'), 'memory-tools r2: called memory_read');
  const readResult = toolResultEnvelope(r2.bundlePath, 'memory_read');
  assert(readResult?.ok === true, 'memory-tools r2: memory_read ok=true');
  if (isLive) {
    assert(/Nighthawk/i.test(finalText(r2.bundlePath)),
      `memory-tools r2 live: reply mentions Nighthawk (got ${truncateText(finalText(r2.bundlePath))})`);
  } else {
    assert(JSON.stringify(readResult?.state ?? {}).includes('Nighthawk'),
      'memory-tools r2: state references Nighthawk');

    const r3 = runAgentRound({
      ...ctx,
      sessionId,
      prompt: 'memory-find-test',
      script: [
        {
          tool_calls: [{
            id: 'tc_mf1',
            name: 'memory_find',
            arguments: { query: 'Nighthawk' },
          }],
        },
        { text: 'MEMORY_FIND_DONE' },
      ],
      roundKey: 'memory-tools-r03',
      roundMeta: { suite: 'memory-tools', index: 3, name: 'memory_find' },
    });
    rep.step('memory_find', r3.ms, `ok=${r3.json?.ok} count=${toolResultEnvelope(r3.bundlePath, 'memory_find')?.count}`);
    assert(r3.status === 0, 'memory-tools r3: exits 0');
    assert(toolCalls(r3.bundlePath).includes('memory_find'), 'memory-tools r3: called memory_find');
    const findResult = toolResultEnvelope(r3.bundlePath, 'memory_find');
    assert(findResult?.ok === true, 'memory-tools r3: memory_find ok=true');
    assert(typeof findResult?.count === 'number' && findResult.count > 0,
      `memory-tools r3: memory_find count > 0 (got ${findResult?.count})`);
  }

  rep.endSuite();
}

function suiteExactTurnRetrieval(rep, assert, ctx) {
  if (ctx.mode !== 'loopback') {
    rep.note('exact-turn-retrieval skipped in live mode');
    return;
  }
  console.log('\n-- exact turn retrieval (loopback) --\n');
  rep.beginSuite('Exact turn retrieval');

  const sessionId = `${ctx.runId}-exact-turn`;
  const head = 'EXACT_BODY_HEAD_7A31';
  const tail = 'EXACT_BODY_TAIL_7A31';
  const longBody = `${head}\n${'canonical report paragraph '.repeat(600)}\n${tail}`;
  const r1 = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'Assemble a long report in chat only; do not write a file.',
    script: [{ text: longBody }],
    roundKey: 'exact-turn-r01',
    roundMeta: { suite: 'exact-turn-retrieval', index: 1, name: 'long chat-only body' },
  });
  assert(r1.status === 0 && r1.json?.ok === true, 'exact-turn r1: long response succeeds');
  assert(!toolCalls(r1.bundlePath).includes('write_file'), 'exact-turn r1: no artifact is written');

  const turnItems = memoryUpdatesFromBundle(r1.bundlePath).flatMap((update) =>
    Object.values(update?.patch?.items || {}).filter((item) => item?.role === 'turn_pair'));
  const longTurn = turnItems.find((item) => item?.metadata?.assistant_chars === longBody.length);
  assert(Boolean(longTurn?.id), 'exact-turn r1: turn_pair has a stable item ID');
  assert(longTurn?.metadata?.body_available === true,
    'exact-turn r1: turn_pair advertises body_available=true');
  assert(Boolean(longTurn?.metadata?.source_log?.log_id || longTurn?.metadata?.source_log?.path),
    'exact-turn r1: turn_pair persists a canonical log locator');
  const itemId = longTurn?.id || 'missing-turn-id';

  const r2 = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'Recover the exact prior report body.',
    script: [{
      tool_calls: [{
        id: 'tc_exact_body',
        name: 'memory_find',
        arguments: { itemId, includeBody: true, maxBytes: 262144 },
      }],
    }, { text: 'EXACT_BODY_RECOVERED' }],
    roundKey: 'exact-turn-r02',
    roundMeta: { suite: 'exact-turn-retrieval', index: 2, name: 'materialize full body' },
  });
  const full = toolResultEnvelope(r2.bundlePath, 'memory_find');
  assert(full?.ok === true && full?.body_found === true,
    'exact-turn r2: exact body materialization succeeds');
  assert(String(full?.final_text || '').startsWith(head), 'exact-turn r2: body preserves head sentinel');
  assert(String(full?.final_text || '').endsWith(tail), 'exact-turn r2: body preserves tail sentinel');
  const prompt2 = systemPromptFromBundle(r2.bundlePath);
  assert(prompt2.includes(`- ${itemId}:`), 'exact-turn r2: compact prompt exposes exact turn ID');
  assert(/body_available=true/.test(prompt2), 'exact-turn r2: compact prompt exposes body hint');
  assert(/memory_find\(itemId=<turn id>, includeBody=true\)/.test(prompt2),
    'exact-turn r2: prompt explains progressive body retrieval');
  assert(!prompt2.includes(tail), 'exact-turn r2: capped prompt does not inline the long tail');
  const shortTurn = memoryUpdatesFromBundle(r2.bundlePath).flatMap((update) =>
    Object.values(update?.patch?.items || {}).filter((item) => item?.role === 'turn_pair'))
    .find((item) => item?.metadata?.assistant_chars === 'EXACT_BODY_RECOVERED'.length);
  assert(shortTurn?.metadata?.body_available === true,
    'exact-turn r2: short successful responses retain the same body locator');
  assert(shortTurn?.metadata?.assistant === 'EXACT_BODY_RECOVERED',
    'exact-turn r2: short response remains inline without truncation');

  const r3 = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'Read a bounded first slice of the prior report.',
    script: [{
      tool_calls: [{
        id: 'tc_exact_slice_1',
        name: 'memory_find',
        arguments: { itemId, includeBody: true, maxBytes: 64, offset: 0 },
      }],
    }, { text: 'SLICE_ONE_DONE' }],
    roundKey: 'exact-turn-r03',
    roundMeta: { suite: 'exact-turn-retrieval', index: 3, name: 'bounded body slice' },
  });
  const firstSlice = toolResultEnvelope(r3.bundlePath, 'memory_find');
  assert(firstSlice?.truncated === true && String(firstSlice?.final_text || '').length <= 64,
    'exact-turn r3: maxBytes bounds the first slice');
  assert(Number(firstSlice?.next_offset) > 0, 'exact-turn r3: next_offset advances');

  const r4 = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'Continue reading the prior report from the supplied byte offset.',
    script: [{
      tool_calls: [{
        id: 'tc_exact_slice_2',
        name: 'memory_find',
        arguments: {
          itemId,
          includeBody: true,
          maxBytes: 262144,
          offset: Number(firstSlice?.next_offset || 0),
        },
      }],
    }, { text: 'SLICE_TWO_DONE' }],
    roundKey: 'exact-turn-r04',
    roundMeta: { suite: 'exact-turn-retrieval', index: 4, name: 'continued body slice' },
  });
  const secondSlice = toolResultEnvelope(r4.bundlePath, 'memory_find');
  assert(String(secondSlice?.final_text || '').endsWith(tail),
    'exact-turn r4: offset continuation reaches tail sentinel');
  assert(secondSlice?.truncated === false, 'exact-turn r4: continuation reports completion');

  const wrongSession = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'Try the exact ID under a wrong session.',
    script: [{
      tool_calls: [{
        id: 'tc_exact_wrong_session',
        name: 'memory_find',
        arguments: { itemId, sessionId: `${sessionId}-wrong`, includeBody: true },
      }],
    }, { text: 'WRONG_SESSION_DONE' }],
    roundKey: 'exact-turn-r05',
    roundMeta: { suite: 'exact-turn-retrieval', index: 5, name: 'wrong session rejected' },
  });
  const wrong = toolResultEnvelope(wrongSession.bundlePath, 'memory_find');
  assert(wrong?.ok === true && wrong?.count === 0,
    'exact-turn r5: wrong session cannot materialize the item');

  const canonicalLogPath = String(longTurn?.metadata?.source_log?.path || '');
  assert(canonicalLogPath && existsSync(canonicalLogPath),
    'exact-turn r6: canonical source log exists before deletion');
  if (canonicalLogPath && existsSync(canonicalLogPath)) unlinkSync(canonicalLogPath);
  const missingLog = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'Try the exact item after its canonical log was removed.',
    script: [{
      tool_calls: [{
        id: 'tc_exact_missing_log',
        name: 'memory_find',
        arguments: { itemId, includeBody: true },
      }],
    }, { text: 'MISSING_LOG_DONE' }],
    roundKey: 'exact-turn-r06',
    roundMeta: { suite: 'exact-turn-retrieval', index: 6, name: 'missing log handled' },
  });
  const missing = toolResultEnvelope(missingLog.bundlePath, 'memory_find');
  assert(missing?.ok === true && missing?.body_found === false,
    'exact-turn r6: missing log is reported without exposing another body');

  rep.endSuite();
}

function toolResultEnvelopes(bundlePath, toolName) {
  if (!bundlePath || !existsSync(bundlePath)) return [];
  const bundle = JSON.parse(readFileSync(bundlePath, 'utf8'));
  return (bundle?.communication?.tools || [])
    .filter((event) => event?.kind === 'tool_result' && event?.tool === toolName)
    .map((event) => {
      let detail = event.detail;
      if (typeof detail === 'string') {
        try { detail = JSON.parse(detail); } catch { return null; }
      }
      return detail?.envelope ?? detail ?? null;
    })
    .filter(Boolean);
}

function suiteMemoryFindNatural(rep, assert, ctx) {
  const label = ctx.mode === 'live' ? 'live' : 'loopback';
  console.log(`\n-- natural exact memory_find (${label}) --\n`);
  rep.beginSuite(`Natural exact memory_find (${label})`);

  const sessionId = `${ctx.runId}-memory-find-natural`;
  const isLive = ctx.mode === 'live';
  const seeds = [
    {
      subject: 'gardening',
      cue: 'balcony compost without attracting flies',
      marker: 'BODY_GARDEN_COMPOST_A17',
      prompt:
        'I have a small shaded balcony. How can I compost kitchen scraps without attracting flies? ' +
        'Give me a practical, friendly explanation with enough detail to follow next weekend.',
    },
    {
      subject: 'travel',
      cue: 'quiet Kyoto evening after the crowds',
      marker: 'BODY_TRAVEL_KYOTO_EVENING_B28',
      prompt:
        'Could you plan a quiet Kyoto evening after the busiest crowds leave, with a bookshop, ' +
        'a canal walk, a casual dinner, and a simple route back by public transport?',
    },
    {
      subject: 'software',
      cue: 'idempotency keys for payment APIs',
      marker: 'BODY_SOFTWARE_IDEMPOTENCY_C39',
      prompt:
        'Explain how idempotency keys should work in a payment API, including storage, retries, ' +
        'concurrent requests, and what response should be replayed.',
    },
    {
      subject: 'gardening',
      cue: 'drip irrigation for raised tomato beds',
      marker: 'BODY_GARDEN_IRRIGATION_D41',
      prompt:
        'What is a sensible drip-irrigation layout and watering routine for two raised tomato beds ' +
        'during a hot, dry summer?',
    },
    {
      subject: 'travel',
      cue: 'rainy Bergen afternoon',
      marker: 'BODY_TRAVEL_BERGEN_E52',
      prompt:
        'I have one rainy afternoon in Bergen with no car. Suggest a relaxed route with indoor stops, ' +
        'short walks, and somewhere cosy for coffee.',
    },
    {
      subject: 'travel',
      cue: 'Kyoto morning before crowds',
      marker: 'BODY_TRAVEL_KYOTO_F63',
      prompt:
        'Design an early Kyoto morning that starts before the crowds, uses public transport, and ' +
        'includes a calm breakfast and one garden.',
    },
    {
      subject: 'software',
      cue: 'idempotent payment webhook consumers',
      marker: 'BODY_SOFTWARE_WEBHOOK_IDEMPOTENCY_G74',
      prompt:
        'Explain how to make a payment-webhook consumer idempotent. Cover event identifiers, ' +
        'duplicate deliveries, concurrent workers, database transactions, and retry behavior.',
    },
    {
      subject: 'gardening',
      cue: 'overwintering rosemary in a cold climate',
      marker: 'BODY_GARDEN_ROSEMARY_H85',
      prompt:
        'How should I overwinter a large potted rosemary when winters are cold and my apartment has ' +
        'limited bright window space?',
    },
    {
      subject: 'software',
      cue: 'event-sourcing snapshot policy',
      marker: 'BODY_SOFTWARE_SNAPSHOTS_I96',
      prompt:
        'What is a good snapshot policy for an event-sourced aggregate? Discuss thresholds, schema ' +
        'changes, rebuilding, and how snapshots remain optional.',
    },
    {
      subject: 'gardening',
      cue: 'courtyard compost without attracting rodents',
      marker: 'BODY_GARDEN_COMPOST_COURTYARD_J07',
      prompt:
        'I share a sunny courtyard and want to compost kitchen scraps in a compact bin. How can I ' +
        'prevent smells and rodents while still producing useful compost?',
    },
  ];

  const seeded = [];
  for (let index = 0; index < seeds.length; index += 1) {
    const seed = seeds[index];
    const prompt =
      `${seed.prompt} Keep the answer in chat only and do not call tools. ` +
      `Write at least 180 words, then finish with this exact standalone verification marker: ${seed.marker}`;
    const loopbackBody =
      `Here is a practical answer about ${seed.cue}.\n\n` +
      `${(`The important details for ${seed.cue} are planning, observation, and gradual adjustment. ` +
        'Start with a simple baseline, note what changes, and avoid changing several variables at once. ' +
        'Use the constraints in the question to choose a routine that remains realistic over time. ').repeat(8)}\n` +
      `${seed.marker}`;
    const runSeed = (attempt) => runAgentRound({
        ...ctx,
        sessionId,
        prompt,
        script: isLive ? undefined : [{ text: loopbackBody }],
        roundKey:
          `memory-find-natural-seed-${String(index + 1).padStart(2, '0')}` +
          (attempt > 1 ? `-retry-${attempt}` : ''),
        roundMeta: {
          suite: 'memory-find-natural',
          index: index + 1,
          name: `seed ${seed.subject}: ${seed.cue}${attempt > 1 ? ` (retry ${attempt})` : ''}`,
        },
      });
    let round = runSeed(1);
    if (isLive &&
        (round.status !== 0 || round.json?.ok !== true ||
         !finalText(round.bundlePath).includes(seed.marker))) {
      rep.note(`Seed ${index + 1} failed transiently; retrying once`);
      round = runSeed(2);
    }
    rep.step(`seed ${index + 1}: ${seed.cue}`, round.ms, `ok=${round.json?.ok}`);
    assert(round.status === 0 && round.json?.ok === true,
      `memory-find-natural seed ${index + 1}: turn succeeds`);
    assert(finalText(round.bundlePath).includes(seed.marker),
      `memory-find-natural seed ${index + 1}: response preserves ${seed.marker}`);

    const turnItem = memoryUpdatesFromBundle(round.bundlePath)
      .flatMap((update) => Object.values(update?.patch?.items || {}))
      .find((item) =>
        item?.role === 'turn_pair' &&
        item?.metadata?.user === prompt &&
        item?.metadata?.body_available === true);
    assert(Boolean(turnItem?.id),
      `memory-find-natural seed ${index + 1}: captured retrievable turn_pair`);
    seeded.push({ ...seed, prompt, itemId: turnItem?.id || '' });
  }

  assert(new Set(seeded.map((seed) => seed.subject)).size === 3,
    'memory-find-natural: ten seed turns cover exactly three subjects');

  const questions = [
    { seedIndex: 0, distractorIndex: 9, question: 'What exact standalone line ended your earlier detailed answer about balcony compost?' },
    { seedIndex: 5, distractorIndex: 1, question: 'What exact standalone line ended your earlier answer about the quiet Kyoto morning?' },
    { seedIndex: 2, distractorIndex: 6, question: 'What exact standalone line ended your earlier explanation of idempotency keys for the payment API?' },
    { seedIndex: 3, question: 'What exact standalone line ended your earlier advice about tomato-bed drip irrigation?' },
    { seedIndex: 4, question: 'What exact standalone line ended your earlier plan for a rainy Bergen afternoon?' },
  ];

  for (let qIndex = 0; qIndex < questions.length; qIndex += 1) {
    const question = questions[qIndex];
    const target = seeded[question.seedIndex];
    const prompt = `${question.question} Reply with only that line.`;
    const script = isLive ? undefined : [
      {
        tool_calls: [{
          id: `tc_natural_query_${qIndex + 1}`,
          name: 'memory_find',
          arguments: { query: target.cue, sessionId },
        }],
      },
      {
        tool_calls: [{
          id: `tc_natural_body_${qIndex + 1}`,
          name: 'memory_find',
          arguments: {
            itemId: target.itemId,
            sessionId,
            includeBody: true,
            maxBytes: 262144,
          },
        }],
      },
      { text: target.marker },
    ];
    const roundIndex = seeds.length + qIndex + 1;
    const round = runAgentRound({
      ...ctx,
      sessionId,
      prompt,
      script,
      roundKey: `memory-find-natural-question-${String(qIndex + 1).padStart(2, '0')}`,
      roundMeta: {
        suite: 'memory-find-natural',
        index: roundIndex,
        name: `question ${qIndex + 1}: ${target.cue}`,
      },
    });
    rep.step(`question ${qIndex + 1}: ${target.cue}`, round.ms,
      `calls=${toolCalls(round.bundlePath).filter((name) => name === 'memory_find').length}`);
    assert(round.status === 0 && round.json?.ok === true,
      `memory-find-natural question ${qIndex + 1}: turn succeeds`);
    assert(toolCalls(round.bundlePath).includes('memory_find'),
      `memory-find-natural question ${qIndex + 1}: called memory_find`);

    const memoryFindResults = toolResultEnvelopes(round.bundlePath, 'memory_find');
    if (!isLive) {
      const ranked = memoryFindResults.find((envelope) => envelope?.ranking === 'bm25_dense_rrf');
      assert(Boolean(ranked),
        `memory-find-natural question ${qIndex + 1}: query uses shared hybrid ranker`);
      assert(ranked?.items?.[0]?.id === target.itemId,
        `memory-find-natural question ${qIndex + 1}: target ranks ahead of near duplicates`);
    }
    const bodyResults = memoryFindResults
      .filter((envelope) => envelope?.body_found === true);
    const correctBody = bodyResults.find((envelope) =>
      String(envelope?.final_text || '').includes(target.marker));
    assert(Boolean(correctBody),
      `memory-find-natural question ${qIndex + 1}: materialized the correct canonical body`);
    const wrongMarkers = seeded
      .filter((seed) => seed.marker !== target.marker)
      .filter((seed) => String(correctBody?.final_text || '').includes(seed.marker));
    assert(wrongMarkers.length === 0,
      `memory-find-natural question ${qIndex + 1}: body contains no distractor marker`);
    if (Number.isInteger(question.distractorIndex)) {
      const nearDuplicate = seeded[question.distractorIndex];
      assert(!String(correctBody?.final_text || '').includes(nearDuplicate.marker),
        `memory-find-natural question ${qIndex + 1}: rejected near-duplicate ${nearDuplicate.cue}`);
    }
    assert(finalText(round.bundlePath).includes(target.marker),
      `memory-find-natural question ${qIndex + 1}: final reply reports the recovered marker`);
  }

  const crossSessionId = `${sessionId}-reader`;
  const crossSessionQuestions = [
    { seedIndex: 0, question: 'In a previous chat, what exact standalone line ended your detailed answer about balcony compost?' },
    { seedIndex: 5, question: 'In a previous chat, what exact standalone line ended your answer about the quiet Kyoto morning?' },
    { seedIndex: 2, question: 'In a previous chat, what exact standalone line ended your explanation of payment API idempotency keys?' },
  ];
  for (let qIndex = 0; qIndex < crossSessionQuestions.length; qIndex += 1) {
    const question = crossSessionQuestions[qIndex];
    const target = seeded[question.seedIndex];
    const prompt = `${question.question} Reply with only that line.`;
    const script = isLive ? undefined : [
      {
        tool_calls: [{
          id: `tc_natural_cross_query_${qIndex + 1}`,
          name: 'memory_find',
          arguments: {
            query: target.cue,
            searchPastChats: true,
            sessionId,
          },
        }],
      },
      {
        tool_calls: [{
          id: `tc_natural_cross_body_${qIndex + 1}`,
          name: 'memory_find',
          arguments: {
            itemId: target.itemId,
            sessionId,
            includeBody: true,
            maxBytes: 262144,
          },
        }],
      },
      { text: target.marker },
    ];
    const round = runAgentRound({
      ...ctx,
      // Fresh reader session per question prevents turn N pending recall from
      // revealing turn N+1's answer without an explicit body materialization.
      sessionId: `${crossSessionId}-${qIndex + 1}`,
      prompt,
      script,
      roundKey: `memory-find-natural-cross-${String(qIndex + 1).padStart(2, '0')}`,
      roundMeta: {
        suite: 'memory-find-natural',
        index: seeds.length + questions.length + qIndex + 1,
        name: `cross-session question ${qIndex + 1}: ${target.cue}`,
      },
    });
    rep.step(`cross-session question ${qIndex + 1}: ${target.cue}`, round.ms,
      `calls=${toolCalls(round.bundlePath).filter((name) => name === 'memory_find').length}`);
    assert(round.status === 0 && round.json?.ok === true,
      `memory-find-natural cross-session ${qIndex + 1}: turn succeeds`);
    assert(toolCalls(round.bundlePath).includes('memory_find'),
      `memory-find-natural cross-session ${qIndex + 1}: called memory_find`);
    const memoryFindResults = toolResultEnvelopes(round.bundlePath, 'memory_find');
    if (!isLive) {
      const ranked = memoryFindResults.find((envelope) => envelope?.ranking === 'bm25_dense_rrf');
      assert(Boolean(ranked),
        `memory-find-natural cross-session ${qIndex + 1}: query uses shared hybrid ranker`);
      assert(ranked?.items?.[0]?.id === target.itemId,
        `memory-find-natural cross-session ${qIndex + 1}: source turn ranks first`);
    }
    const bodyResults = memoryFindResults
      .filter((envelope) => envelope?.body_found === true);
    const correctBody = bodyResults.find((envelope) =>
      String(envelope?.final_text || '').includes(target.marker));
    assert(Boolean(correctBody),
      `memory-find-natural cross-session ${qIndex + 1}: materialized source-session body`);
    assert(finalText(round.bundlePath).includes(target.marker),
      `memory-find-natural cross-session ${qIndex + 1}: final reply reports recovered marker`);
  }

  rep.endSuite();
}

function suiteMemoryCrossSession(rep, assert, ctx) {
  const label = ctx.mode === 'live' ? 'live' : 'loopback';
  console.log(`\n-- cross-session memory (${label}) --\n`);
  rep.beginSuite(`Memory cross-session (${label})`);

  const baseSession = `${ctx.runId}-cross`;
  const isLive = ctx.mode === 'live';

  const r1 = runAgentRound({
    ...ctx,
    sessionId: `${baseSession}-a`,
    prompt: isLive
      ? 'Use memory_write to store exactly: {"secret_token":"CrossSession42"}. After the tool succeeds reply: CROSS_WRITTEN'
      : 'cross-session-write',
    script: isLive ? undefined : [
      {
        tool_calls: [{
          id: 'tc_xw1',
          name: 'memory_write',
          arguments: { state: { secret_token: 'CrossSession42' } },
        }],
      },
      { text: 'CROSS_WRITTEN secret=CrossSession42' },
    ],
    roundKey: 'cross-session-r01',
    roundMeta: { suite: 'memory-cross-session', index: 1, name: 'session A write' },
  });
  rep.step('session A write', r1.ms, `ok=${r1.json?.ok}`);
  assert(r1.status === 0, 'cross-session r1: exits 0');
  const writeResult = toolResultEnvelope(r1.bundlePath, 'memory_write');
  assert(writeResult?.ok === true, 'cross-session r1: memory_write ok=true');
  assert(writeResult?.saved?.secret_token === 'CrossSession42',
    'cross-session r1: saved secret_token=CrossSession42');
  if (isLive) {
    assert(/CROSS_WRITTEN/i.test(finalText(r1.bundlePath)),
      `cross-session r1 live: final text contains CROSS_WRITTEN (got ${truncateText(finalText(r1.bundlePath))})`);
  }

  const r2 = runAgentRound({
    ...ctx,
    sessionId: `${baseSession}-b`,
    prompt: isLive
      ? 'Use memory_find with searchPastChats=true and query "CrossSession42" to search past sessions. Then reply: CROSS_FOUND'
      : 'cross-session-find',
    script: isLive ? undefined : [
      {
        tool_calls: [{
          id: 'tc_xf1',
          name: 'memory_find',
          arguments: { query: 'CrossSession42', searchPastChats: true },
        }],
      },
      { text: 'CROSS_FOUND' },
    ],
    roundKey: 'cross-session-r02',
    roundMeta: { suite: 'memory-cross-session', index: 2, name: 'session B find' },
  });
  rep.step('session B find', r2.ms, `ok=${r2.json?.ok} count=${toolResultEnvelope(r2.bundlePath, 'memory_find')?.count}`);
  assert(r2.status === 0, 'cross-session r2: exits 0');
  const findResult = toolResultEnvelope(r2.bundlePath, 'memory_find');
  assert(findResult?.ok === true, 'cross-session r2: memory_find ok=true');
  assert(typeof findResult?.count === 'number' && findResult.count > 0,
    `cross-session r2: found prior-session matches (count=${findResult?.count})`);
  const items = Array.isArray(findResult?.items) ? findResult.items : [];
  assert(items.some((it) => JSON.stringify(it).includes('CrossSession42')),
    'cross-session r2: results include CrossSession42');
  if (isLive) {
    assert(/CROSS_FOUND/i.test(finalText(r2.bundlePath)),
      `cross-session r2 live: final text contains CROSS_FOUND (got ${truncateText(finalText(r2.bundlePath))})`);
  }

  rep.endSuite();
}

function suiteMemoryRecallPending(rep, assert, ctx) {
  if (ctx.mode !== 'loopback') {
    rep.note('memory-recall-pending skipped in live mode');
    return;
  }
  console.log('\n-- hybrid recall pending inject (loopback) --\n');
  rep.beginSuite('Hybrid recall pending inject');

  const sessionId = `${ctx.runId}-recall-pending`;
  const env = ctx.env;

  const r1 = runAgentRound({
    ...ctx,
    env,
    sessionId,
    prompt: 'Please remember my dog is named Buddy and he is a golden retriever.',
    script: [
      {
        tool_calls: [{
          id: 'tc_pref1',
          name: 'memory_write',
          arguments: {
            state: {
              pet_name: 'Buddy',
              pet_type: 'golden retriever',
              pet_note: 'Loves fetch in the park',
            },
          },
        }],
      },
      { text: 'Got it - I saved Buddy the golden retriever.' },
    ],
    roundKey: 'recall-pending-r01',
    roundMeta: { suite: 'memory-recall-pending', index: 1, name: 'save preference' },
  });
  rep.step('turn 1 save', r1.ms, `ok=${r1.json?.ok}`);
  assert(r1.status === 0, 'recall-pending r1: exits 0');
  assert(r1.json?.ok === true, 'recall-pending r1: ok=true');
  assert(toolCalls(r1.bundlePath).includes('memory_write'), 'recall-pending r1: memory_write');

  const tel1 = recallTelemetryFromBundle(r1.bundlePath);
  assert(tel1, 'recall-pending r1: recall telemetry emitted');
  assert(Number(tel1.candidates) >= 1, 'recall-pending r1: candidate pool non-empty');
  assert(Number.isFinite(Number(tel1.bm25_ms)), 'recall-pending r1: bm25_ms present');
  assert(Number.isFinite(Number(tel1.dense_ms)), 'recall-pending r1: dense_ms present');
  assert(Number(tel1.overlap_suppressed) >= 1,
    'recall-pending r1: overlap suppression drops recent Session Items hits');
  assert(Number(tel1.ranked_hits) >= Number(tel1.overlap_suppressed),
    'recall-pending r1: suppressed count bounded by ranked hits');

  // memory_write facts live under default_memory (session_id=global), so they are
  // NOT in the Session Items window and remain eligible for Recalled memory.
  // Same-session recent IDs (e.g. turn_pair) are what overlap suppression drops.
  assert(Number(tel1.injected_hits) >= 1 && tel1.pending_enqueued === true,
    'recall-pending r1: global/default_memory hits still enqueue after overlap filter');

  const r2 = runAgentRound({
    ...ctx,
    env,
    sessionId,
    prompt: 'What is my dog\'s name and breed?',
    script: [{ text: 'Your dog is Buddy, a golden retriever.' }],
    roundKey: 'recall-pending-r02',
    roundMeta: { suite: 'memory-recall-pending', index: 2, name: 'inject pending' },
  });
  rep.step('turn 2 query', r2.ms, `ok=${r2.json?.ok}`);
  assert(r2.status === 0, 'recall-pending r2: exits 0');
  const sys2 = systemPromptFromBundle(r2.bundlePath);
  assert(/## Session Items/i.test(sys2),
    'recall-pending r2: Session Items present');
  assert(/## Recalled memory/i.test(sys2),
    'recall-pending r2: system prompt contains Recalled memory block');
  assert(/## Recalled memory[\s\S]*\[global\][\s\S]*Buddy/i.test(sys2),
    'recall-pending r2: recalled block injects global Buddy (not session-item duplicate)');

  const r3 = runAgentRound({
    ...ctx,
    env,
    sessionId,
    prompt: 'Thanks, that helps.',
    script: [{ text: 'You are welcome.' }],
    roundKey: 'recall-pending-r03',
    roundMeta: { suite: 'memory-recall-pending', index: 3, name: 'no duplicate inject' },
  });
  rep.step('turn 3 noop', r3.ms, `ok=${r3.json?.ok}`);
  const sys3 = systemPromptFromBundle(r3.bundlePath);
  assert(!/## Recalled memory/i.test(sys3),
    'recall-pending r3: pending block consumed once (no second inject)');

  rep.endSuite();
}

function suiteMemoryRecallConsumer(rep, assert, ctx) {
  if (ctx.mode !== 'loopback') {
    rep.note('memory-recall-consumer skipped in live mode');
    return;
  }
  console.log('\n-- consumer cross-session recall (loopback) --\n');
  rep.beginSuite('Consumer cross-session recall');

  const env = ctx.env;
  const base = `${ctx.runId}-consumer`;

  const r1 = runAgentRound({
    ...ctx,
    env,
    sessionId: `${base}-trip`,
    prompt: 'We are planning a family trip. Our vacation codeword is SunnyBeach2026.',
    script: [
      {
        tool_calls: [{
          id: 'tc_trip1',
          name: 'memory_write',
          arguments: { state: { vacation_codeword: 'SunnyBeach2026', trip: 'family beach week' } },
        }],
      },
      { text: 'Saved your trip codeword SunnyBeach2026.' },
    ],
    roundKey: 'recall-consumer-r01',
    roundMeta: { suite: 'memory-recall-consumer', index: 1, name: 'session A trip' },
  });
  assert(r1.status === 0 && r1.json?.ok === true, 'recall-consumer r1: ok');

  const r2 = runAgentRound({
    ...ctx,
    env,
    sessionId: `${base}-later`,
    prompt: 'What was our vacation codeword?',
    script: [{ text: 'Let me check what we saved.' }],
    roundKey: 'recall-consumer-r02',
    roundMeta: { suite: 'memory-recall-consumer', index: 2, name: 'session B rank' },
  });
  assert(r2.status === 0 && r2.json?.ok === true, 'recall-consumer r2: ok');
  const tel2 = recallTelemetryFromBundle(r2.bundlePath);
  assert(tel2, 'recall-consumer r2: recall telemetry emitted');
  assert(Number(tel2.candidates) >= 1, 'recall-consumer r2: candidates >= 1');
  assert(tel2.pending_enqueued === true,
    'recall-consumer r2: cross-session hit enqueued (not overlap-suppressed)');
  assert(Number(tel2.injected_bytes) > 0, 'recall-consumer r2: injected_bytes > 0');

  const r3 = runAgentRound({
    ...ctx,
    env,
    sessionId: `${base}-later`,
    prompt: 'Remind me again about the trip codeword.',
    script: [{ text: 'Your vacation codeword is SunnyBeach2026.' }],
    roundKey: 'recall-consumer-r03',
    roundMeta: { suite: 'memory-recall-consumer', index: 3, name: 'session B inject' },
  });
  assert(r3.status === 0 && r3.json?.ok === true, 'recall-consumer r3: ok');
  const sys3 = systemPromptFromBundle(r3.bundlePath);
  assert(/## Recalled memory/i.test(sys3),
    'recall-consumer r3: pending block injected on follow-up turn');
  assert(/## Recalled memory[\s\S]*SunnyBeach2026/i.test(sys3),
    'recall-consumer r3: recalled text mentions SunnyBeach2026');

  rep.endSuite();
}

function suiteMemoryArtifacts(rep, assert, ctx) {
  if (ctx.mode !== 'loopback') {
    rep.note('memory-artifacts skipped in live mode');
    return;
  }
  console.log('\n-- session artifacts (loopback) --\n');
  rep.beginSuite('Session write_file artifacts');

  const sessionId = `${ctx.runId}-artifacts`;
  const notePath = join(ctx.cwd, 'grocery-list.txt');
  const noteRel = 'grocery-list.txt';
  const stagedSystemPrompt = join(ctx.paths.packageRoot, 'dist', 'data', 'prompts', 'system-prompt.md');

  const r1 = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'Save my grocery list to a file.',
    script: [
      {
        tool_calls: [{
          id: 'tc_wf1',
          name: 'write_file',
          arguments: {
            path: noteRel,
            content: 'Grocery list:\n- milk\n- eggs\n- bread for sandwiches',
          },
        }],
      },
      { text: 'Saved your grocery list.' },
    ],
    roundKey: 'artifacts-r01',
    roundMeta: { suite: 'memory-artifacts', index: 1, name: 'write_file' },
  });
  assert(r1.status === 0 && r1.json?.ok === true, 'artifacts r1: ok');
  assert(toolCalls(r1.bundlePath).includes('write_file'), 'artifacts r1: write_file called');
  assert(existsSync(notePath), `artifacts r1: file exists at ${notePath}`);

  const updates = memoryUpdatesFromBundle(r1.bundlePath);
  assert(updates.length > 0, 'artifacts r1: memory_update emitted');
  const hasFileItem = updates.some((u) => {
    const items = u?.patch?.items;
    if (!items || typeof items !== 'object') return false;
    return Object.values(items).some((it) =>
      it?.kind === 'file' && String(it?.locator?.path || '').includes('grocery-list.txt'));
  });
  assert(hasFileItem, 'artifacts r1: memory_update includes file artifact with path');

  const r2 = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'What file did we just create?',
    script: [{ text: 'We created grocery-list.txt with your grocery list.' }],
    extraArgs: ['--system-prompt', `@${stagedSystemPrompt}`, '--include', notePath],
    roundKey: 'artifacts-r02',
    roundMeta: { suite: 'memory-artifacts', index: 2, name: 'session inject' },
  });
  assert(r2.status === 0 && r2.json?.ok === true, 'artifacts r2: ok');
  const sys2 = systemPromptFromBundle(r2.bundlePath);
  assert(/^\s*You are \*\*Tanit\*\*'s AI assistant/m.test(sys2),
    'artifacts r2: staged system-prompt.md is active');
  assert(sys2.includes(`Working folder: ${ctx.cwd}`),
    'artifacts r2: system prompt renders exact working folder');
  assert(/Selected files \(1\):/i.test(sys2) && sys2.includes(notePath),
    'artifacts r2: system prompt renders selected-file count and exact path');
  assert(!/\$\{(?:FOLDER|CURRENT_SELECTION|SELECTION_COUNT|OPEN_COUNT|CURRENT_OPEN_FILES|DATETIME)\}/.test(sys2),
    'artifacts r2: session-context template has no unresolved placeholders');

  const r3 = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'Open the file we created earlier.',
    script: [{ text: 'The existing grocery-list.txt is ready.' }],
    roundKey: 'artifacts-r03',
    roundMeta: { suite: 'memory-artifacts', index: 3, name: 'recent locator inject' },
  });
  assert(r3.status === 0 && r3.json?.ok === true, 'artifacts r3: ok');
  const sys3 = systemPromptFromBundle(r3.bundlePath);
  assert(/## Session Items[\s\S]*grocery-list\.txt/i.test(sys3),
    'artifacts r3: recent session items include exact artifact path');
  assert(!/## Session Items[\s\S]*kind=self_check/i.test(sys3),
    'artifacts r3: self-check bookkeeping does not consume recent-item prompt budget');

  rep.endSuite();
}

function suiteSkillResources(rep, assert, ctx) {
  const isLive = ctx.mode === 'live';
  console.log(`\n-- skill resources (${isLive ? 'live' : 'loopback'}) --\n`);
  rep.beginSuite(`Skills as typed resources (${isLive ? 'live' : 'loopback'})`);

  const workspace = join(ctx.cwd, 'skills', 'meal-helper');
  mkdirSync(workspace, { recursive: true });
  if (!isLive) {
    const roaming = join(ctx.configDir, 'skills', 'meal-helper');
    const legacy = join(ctx.configDir, 'skills', 'legacy-helper');
    mkdirSync(roaming, { recursive: true });
    mkdirSync(legacy, { recursive: true });
    writeFileSync(join(roaming, 'SKILL.md'), [
      'description: Roaming meal helper',
      '# ROAMING_SKILL_BODY_SENTINEL',
    ].join('\n'), 'utf8');
    writeFileSync(join(legacy, 'SKILL.md'), [
      'description: Legacy package compatibility fixture',
      '# LEGACY_SKILL_BODY_SENTINEL',
    ].join('\n'), 'utf8');
    for (const name of ['commands', 'brand-voice', 'videodb']) {
      cpSync(join(ctx.paths.packageRoot, 'dist', 'data', 'skills', name),
        join(ctx.configDir, 'skills', name), { recursive: true });
    }
    const unavailable = join(ctx.configDir, 'skills', 'unavailable-skill');
    const cycleA = join(ctx.configDir, 'skills', 'cycle-a');
    const cycleB = join(ctx.configDir, 'skills', 'cycle-b');
    mkdirSync(unavailable, { recursive: true });
    mkdirSync(cycleA, { recursive: true });
    mkdirSync(cycleB, { recursive: true });
    writeFileSync(join(unavailable, 'SKILL.md'),
      'description: unavailable fixture\nos: [definitely-not-an-os]\nUNAVAILABLE_BODY\n', 'utf8');
    writeFileSync(join(cycleA, 'SKILL.md'),
      `description: cycle fixture a\n[A to B](${brand.APP_ID_SLUG}://skill/config/cycle-b)\n`, 'utf8');
    writeFileSync(join(cycleB, 'SKILL.md'),
      `description: cycle fixture b\n[B to A](${brand.APP_ID_SLUG}://skill/config/cycle-a)\n`, 'utf8');
  }
  writeFileSync(join(workspace, 'resource.json'), JSON.stringify({
    type: 'skill',
    name: 'meal-helper',
    description: 'Plan simple family meals and grocery lists',
    body: 'instructions.txt',
    media_type: 'text/plain',
    declared_tools: ['write_file', 'imaginary_skill_tool'],
  }, null, 2), 'utf8');
  writeFileSync(join(workspace, 'instructions.txt'), [
    'WORKSPACE_SKILL_BODY_SENTINEL_7F31',
    'Use common ingredients and explain substitutions.',
    '[shopping reference](reference.md)',
  ].join('\n'), 'utf8');
  writeFileSync(join(workspace, 'reference.md'), '# Shopping reference\nBuy seasonal produce.\n', 'utf8');

  let resourceEnv = ctx.env;
  let vfsUri = '';
  let mockManifest = '';
  if (!isLive) {
    const mockRoot = join(ctx.runDir, 'mock-vfs');
    const mockMount = join(mockRoot, 'public-skills');
    mockManifest = join(mockMount, 'manifests', 'skills.json');
    mkdirSync(join(mockMount, 'manifests'), { recursive: true });
    mkdirSync(join(mockMount, 'packages'), { recursive: true });
    writeFileSync(mockManifest, JSON.stringify({
      api_token: 'VFS_TOKEN_MUST_NOT_PERSIST',
      resources: [
        {
          type: 'skill',
          id: 'remote-meals',
          title: 'Remote meal ideas',
          description: 'Public VFS meal planning skill',
          body: 'packages/remote-meals.md',
          declared_tools: ['write_file'],
        },
        { type: 'skill', id: 'escape', body: '../secret.md' },
      ],
    }, null, 2), 'utf8');
    writeFileSync(join(mockMount, 'packages', 'remote-meals.md'),
      `REMOTE_VFS_SKILL_SENTINEL\n[more](more.md)\n${'budget meals '.repeat(300)}`, 'utf8');
    writeFileSync(join(mockMount, 'packages', 'more.md'), '# More meals\n', 'utf8');
    const settingsPath = join(ctx.configDir, 'settings.json');
    const settings = JSON.parse(readFileSync(settingsPath, 'utf8'));
    settings.custom.agent_skills = {
      enabled: true,
      roaming_enabled: true,
      workspace_enabled: true,
      // Stable prompt catalog is pinned/always only; pin one fixture so r1 can
      // assert catalog emission without dumping the full discovered set.
      pinned: ['legacy-helper'],
      vfs_roots: [{
        mount: 'public-skills',
        manifest: 'manifests/skills.json',
        server_url: 'mock://local',
        public_only: true,
        trusted: true,
      }],
    };
    writeFileSync(settingsPath, JSON.stringify(settings, null, 2), 'utf8');
    resourceEnv = {
      ...ctx.env,
      PM_LLM_TEST_VFS_ROOT: mockRoot,
      PM_LLM_TEST_VFS_CONFIG: JSON.stringify(settings.custom.agent_skills.vfs_roots),
    };
    vfsUri = `${brand.APP_ID_SLUG}://skill/vfs/public-skills/remote-meals`;
  }

  const uri = `${brand.APP_ID_SLUG}://skill/workspace/meal-helper`;
  const sessionId = `${ctx.runId}-skill-resources`;
  if (isLive) {
    // Stage public VFS skills into the user's real roaming profile, then run
    // against that roaming config (no isolated --config-dir). Force skills on
    // for this turn because the quick preset currently has them disabled.
    const roamingSkills = join(process.env.APPDATA || '', 'PolyMech', brand.APP_ID_SLUG, 'skills');
    const stagePackages = ['meal-helper', 'legacy-helper', 'continuous-learning'];
    for (const name of stagePackages) {
      const dest = join(roamingSkills, name);
      mkdirSync(dest, { recursive: true });
      const pull = spawnSync(ctx.exe, [
        '--no-gui', '--no-mcp',
        'service', 'files', 'pull', `skills/${name}`,
        '--mount', 'assets',
        '--server-url', 'https://tanit.polymech.info',
        '--local-dir', dest,
        '--overwrite',
        '--json',
      ], { encoding: 'utf8', timeout: 60_000, cwd: ctx.paths.packageRoot });
      assert(pull.status === 0,
        `skill-resources live: stage ${name} from VFS (exit ${pull.status}); ${(pull.stderr || '').slice(0, 200)}`);
      assert(existsSync(join(dest, 'SKILL.md')) || existsSync(join(dest, 'resource.json')),
        `skill-resources live: staged package ${name} has a skill body/manifest`);
    }

    const mealUri = `${brand.APP_ID_SLUG}://skill/config/meal-helper`;
    const live = runAgentRound({
      ...ctx,
      configDir: '', // force user roaming settings / credentials
      sessionId,
      prompt: [
        'Use the skill resources from the current roaming catalog.',
        'Find the meal-helper skill (family meals / grocery planning).',
        'Load it with info_lookup(kind="skill") using its exact URI or qualified name',
        `(prefer ${mealUri} or config:meal-helper).`,
        'Then request only the write_file tool schema via request_tools if it is not already available.',
        'Do not create a file. Reply SKILL_RESOURCE_READY.',
      ].join(' '),
      extraArgs: ['--skills', '--max-iter', '8'],
      roundKey: 'skill-resources-live',
      roundMeta: { suite: 'skill-resources', index: 1, name: 'roaming VFS consumer flow' },
    });
    assert(live.status === 0 && live.json?.ok === true, 'skill-resources live: ok');
    const calls = toolCalls(live.bundlePath);
    assert(calls.includes('info_lookup'), 'skill-resources live: model discovers/loads through info_lookup');
    const lookup = toolResultEnvelope(live.bundlePath, 'info_lookup');
    const lookupText = JSON.stringify(lookup || {});
    assert(/meal-helper|ROAMING_SKILL_BODY_SENTINEL|grocery|meal/i.test(lookupText),
      'skill-resources live: info_lookup touched the staged meal-helper skill');
    // Without a planner seed the full path catalog is already present, so
    // request_tools may be skipped when write_file is already available.
    // If the model does call it, it must only ask for real schemas.
    if (calls.includes('request_tools')) {
      const grant = toolResultEnvelope(live.bundlePath, 'request_tools');
      assert(Array.isArray(grant?.granted) || Array.isArray(grant?.already_active)
          || Array.isArray(grant?.denied),
        'skill-resources live: request_tools returned a grant/deny payload');
      assert(!(grant?.granted || []).includes('imaginary_skill_tool'),
        'skill-resources live: request_tools does not inherit undeclared authority');
    }
    assert(/SKILL_RESOURCE_READY/i.test(finalText(live.bundlePath)),
      'skill-resources live: consumer flow completes');
    rep.endSuite();
    return;
  }

  const r1 = runAgentRound({
    ...ctx,
    env: resourceEnv,
    sessionId,
    prompt: 'Help plan affordable family meals.',
    script: [{ text: 'I can look up the matching meal skill.' }],
    roundKey: 'skill-resources-r01',
    roundMeta: { suite: 'skill-resources', index: 1, name: 'compact catalog' },
  });
  assert(r1.status === 0 && r1.json?.ok === true, 'skill-resources r1: ok');
  const sys = systemPromptFromBundle(r1.bundlePath);
  assert(sys.includes('prompt_catalog: pinned/always only'),
    'skill-resources r1: prompt states pinned/always catalog policy');
  assert(!sys.includes('top_matches:'),
    'skill-resources r1: no query top_matches in the prompt');
  assert(/\n {2}- legacy-helper:/.test(sys),
    'skill-resources r1: pinned legacy skill listed as name:description');
  assert(!sys.includes(`${brand.APP_ID_SLUG}://skill/`),
    'skill-resources r1: prompt catalog uses names, not skill URIs');
  assert(!/\n {2}- meal-helper:/.test(sys),
    'skill-resources r1: non-pinned skills are not mentioned until info_lookup');
  assert(['commands', 'brand-voice', 'videodb'].every((name) =>
    !new RegExp(`\\n {2}- ${name}:`).test(sys)),
    'skill-resources r1: non-pinned skills are not listed in the prompt');
  assert(!/\n {2}- unavailable-skill:/.test(sys),
    'skill-resources r1: unavailable resource is excluded from prompt catalog');
  assert(!sys.includes('REMOTE_VFS_SKILL_SENTINEL'),
    'skill-resources r1: remote body is not injected');
  assert(!sys.includes('WORKSPACE_SKILL_BODY_SENTINEL_7F31')
      && !sys.includes('ROAMING_SKILL_BODY_SENTINEL'),
    'skill-resources r1: prompt contains metadata but no skill bodies');

  const r2 = runAgentRound({
    ...ctx,
    env: resourceEnv,
    sessionId,
    prompt: 'Load the exact meal helper skill.',
    script: [
      { tool_calls: [{ id: 'tc_skill_lookup', name: 'info_lookup',
        arguments: { kind: 'skill', query: uri } }] },
      { text: 'Loaded the meal helper resource.' },
    ],
    roundKey: 'skill-resources-r02',
    roundMeta: { suite: 'skill-resources', index: 2, name: 'on-demand body' },
  });
  assert(r2.status === 0 && r2.json?.ok === true, 'skill-resources r2: ok');
  const lookup = toolResultEnvelope(r2.bundlePath, 'info_lookup');
  assert(lookup?.materialized === true, 'skill-resources r2: exact URI materializes');
  assert(lookup?.resource?.body?.includes('WORKSPACE_SKILL_BODY_SENTINEL_7F31'),
    'skill-resources r2: workspace overlay body returned');
  assert(!lookup?.resource?.body?.includes('ROAMING_SKILL_BODY_SENTINEL'),
    'skill-resources r2: roaming body is shadowed');
  assert(!lookup?.resource?.declared_tools?.length,
    'skill-resources r2: allowed-tools / declared_tools are ignored');
  assert(/allowed-tools|declared_tools are ignored/i.test(String(lookup?.resource?.policy_note || '')),
    'skill-resources r2: materialize states tool declarations are ignored');
  assert(lookup?.resource?.links?.some((link) =>
    link.target === 'reference.md' && link.available === true),
  'skill-resources r2: relative supporting link resolves on demand');

  const r3 = runAgentRound({
    ...ctx,
    env: resourceEnv,
    sessionId,
    prompt: 'Try the tool named by that skill.',
    script: [
      { tool_calls: [{ id: 'tc_skill_grant', name: 'request_tools',
        arguments: { names: ['imaginary_skill_tool'], reason: 'skill declaration test' } }] },
      { text: 'The undeclared runtime schema was not granted.' },
    ],
    roundKey: 'skill-resources-r03',
    roundMeta: { suite: 'skill-resources', index: 3, name: 'policy boundary' },
  });
  assert(r3.status === 0 && r3.json?.ok === true, 'skill-resources r3: ok');
  const grant = toolResultEnvelope(r3.bundlePath, 'request_tools');
  assert(grant?.denied?.includes('imaginary_skill_tool'),
    'skill-resources r3: skill tool names do not grant schemas via request_tools');

  const r4 = runAgentRound({
    ...ctx,
    env: resourceEnv,
    sessionId: `${sessionId}-vfs`,
    prompt: 'Load the configured remote meal skill.',
    script: [
      { tool_calls: [{ id: 'tc_vfs_lookup', name: 'info_lookup',
        arguments: { kind: 'skill', query: vfsUri, max_bytes: 1024 } }] },
      { text: 'Loaded remote meal metadata.' },
    ],
    roundKey: 'skill-resources-r04',
    roundMeta: { suite: 'skill-resources', index: 4, name: 'VFS on-demand fetch' },
  });
  assert(r4.status === 0 && r4.json?.ok === true, 'skill-resources r4: VFS lookup ok');
  const vfsLookup = toolResultEnvelope(r4.bundlePath, 'info_lookup');
  assert(vfsLookup?.resource?.body?.includes('REMOTE_VFS_SKILL_SENTINEL'),
    'skill-resources r4: remote body fetched only on exact lookup');
  assert(vfsLookup?.resource?.truncated === true
      && vfsLookup?.resource?.body?.length <= 1024,
    'skill-resources r4: remote body byte cap enforced');
  assert(vfsLookup?.resource?.links?.some((link) =>
    link.uri?.includes('resource/vfs/public-skills/packages/more.md')),
  'skill-resources r4: remote relative link resolves to branded resource URI');

  const cacheDir = join(ctx.configDir, 'agent-resource-cache');
  const cacheText = existsSync(cacheDir)
    ? readdirSync(cacheDir).map((name) => readFileSync(join(cacheDir, name), 'utf8')).join('\n')
    : '';
  assert(!cacheText.includes('VFS_TOKEN_MUST_NOT_PERSIST'),
    'skill-resources r4: VFS metadata cache redacts token fields');
  unlinkSync(mockManifest);
  const r5 = runAgentRound({
    ...ctx,
    env: resourceEnv,
    sessionId: `${sessionId}-vfs-offline`,
    prompt: 'Load the cached remote meal skill while its manifest is offline.',
    script: [
      { tool_calls: [{ id: 'tc_vfs_cached', name: 'info_lookup',
        arguments: { kind: 'skill', query: vfsUri, max_bytes: 1024 } }] },
      { text: 'Loaded from cached metadata.' },
    ],
    roundKey: 'skill-resources-r05',
    roundMeta: { suite: 'skill-resources', index: 5, name: 'VFS offline metadata cache' },
  });
  const cachedLookup = toolResultEnvelope(r5.bundlePath, 'info_lookup');
  assert(r5.status === 0 && cachedLookup?.resource?.body?.includes('REMOTE_VFS_SKILL_SENTINEL'),
    'skill-resources r5: cached metadata supports offline listing and later body fetch');

  const brandVoiceUri = `${brand.APP_ID_SLUG}://skill/config/brand-voice`;
  const r6 = runAgentRound({
    ...ctx,
    env: resourceEnv,
    sessionId: `${sessionId}-staged`,
    prompt: 'Load the staged brand voice skill twice.',
    script: [
      { tool_calls: [{ id: 'tc_staged_first', name: 'info_lookup',
        arguments: { kind: 'skill', query: brandVoiceUri } }] },
      { tool_calls: [{ id: 'tc_staged_second', name: 'info_lookup',
        arguments: { kind: 'skill', query: brandVoiceUri } }] },
      { text: 'Staged skill checked.' },
    ],
    roundKey: 'skill-resources-r06',
    roundMeta: { suite: 'skill-resources', index: 6, name: 'staged compatibility and dedup' },
  });
  const stagedResults = JSON.parse(readFileSync(r6.bundlePath, 'utf8'))
    ?.communication?.tools
    ?.filter((event) => event.kind === 'tool_result' && event.tool === 'info_lookup')
    ?.map((event) => event.detail?.envelope?.resource) || [];
  assert(stagedResults[0]?.body?.includes('VOICE PROFILE'),
    'skill-resources r6: staged real skill materializes');
  assert(stagedResults[0]?.links?.some((link) =>
    link.target === 'references/voice-profile-schema.md' && link.available === true),
  'skill-resources r6: staged real supporting reference resolves');
  assert(stagedResults[1]?.duplicate === true && stagedResults[1]?.body === '',
    'skill-resources r6: repeated content hash is suppressed per session');

  const r7 = runAgentRound({
    ...ctx,
    env: resourceEnv,
    sessionId: `${sessionId}-cycle`,
    prompt: 'Inspect the explicit cross-skill references.',
    script: [
      { tool_calls: [{ id: 'tc_cycle_a', name: 'info_lookup',
        arguments: { kind: 'skill', query: `${brand.APP_ID_SLUG}://skill/config/cycle-a`, depth: 0 } }] },
      { tool_calls: [{ id: 'tc_cycle_b', name: 'info_lookup',
        arguments: { kind: 'skill', query: `${brand.APP_ID_SLUG}://skill/config/cycle-b`, depth: 1 } }] },
      { tool_calls: [{ id: 'tc_cycle_a_again', name: 'info_lookup',
        arguments: { kind: 'skill', query: `${brand.APP_ID_SLUG}://skill/config/cycle-a`, depth: 2 } }] },
      { text: 'Cycle traversal remained explicit and bounded.' },
    ],
    roundKey: 'skill-resources-r07',
    roundMeta: { suite: 'skill-resources', index: 7, name: 'cross-skill cycle guard' },
  });
  const cycleResults = JSON.parse(readFileSync(r7.bundlePath, 'utf8'))
    ?.communication?.tools
    ?.filter((event) => event.kind === 'tool_result' && event.tool === 'info_lookup')
    ?.map((event) => event.detail?.envelope?.resource) || [];
  assert(cycleResults[0]?.links?.[0]?.uri?.endsWith('/cycle-b')
      && cycleResults[1]?.links?.[0]?.uri?.endsWith('/cycle-a'),
    'skill-resources r7: branded cross-skill links are returned but not recursively injected');
  assert(cycleResults[2]?.duplicate === true,
    'skill-resources r7: cycle closes through per-session duplicate suppression');

  rep.endSuite();
}

function toolResultByCallId(bundlePath, callId) {
  if (!bundlePath || !existsSync(bundlePath)) return null;
  const bundle = JSON.parse(readFileSync(bundlePath, 'utf8'));
  for (const event of bundle?.communication?.tools || []) {
    if (event?.kind !== 'tool_result') continue;
    let detail = event.detail;
    if (typeof detail === 'string') {
      try { detail = JSON.parse(detail); } catch { continue; }
    }
    if (String(detail?.id || '') === callId) return detail?.envelope ?? detail;
  }
  return null;
}

function toolCallDetails(bundlePath, toolName) {
  if (!bundlePath || !existsSync(bundlePath)) return [];
  const bundle = JSON.parse(readFileSync(bundlePath, 'utf8'));
  return (bundle?.communication?.tools || [])
    .filter((event) => event?.kind === 'tool_call' && event?.tool === toolName)
    .map((event) => {
      let detail = event.detail;
      if (typeof detail === 'string') {
        try { detail = JSON.parse(detail); } catch { return null; }
      }
      return detail;
    })
    .filter(Boolean);
}

function collectHttpUrls(value, out = new Set()) {
  if (typeof value === 'string') {
    for (const match of value.matchAll(/https?:\/\/[^\s"'\\}]+/g))
      out.add(match[0]);
  } else if (Array.isArray(value)) {
    for (const child of value) collectHttpUrls(child, out);
  } else if (value && typeof value === 'object') {
    for (const child of Object.values(value)) collectHttpUrls(child, out);
  }
  return [...out];
}

function turnPairFromBundle(bundlePath) {
  const updates = memoryUpdatesFromBundle(bundlePath);
  for (let i = updates.length - 1; i >= 0; --i) {
    const items = updates[i]?.patch?.items;
    if (!items || typeof items !== 'object') continue;
    const turn = Object.values(items).find((item) => item?.role === 'turn_pair');
    if (turn) return turn;
  }
  return null;
}

function suiteToolMemoryBench(rep, assert, ctx) {
  const phase = TOOL_MEMORY_PHASE;
  const gating = phase === 'candidate';
  const label = ctx.mode === 'live' ? 'live' : 'loopback';
  console.log(`\n-- tool memory qualification (${label}, ${phase}) --\n`);
  rep.beginSuite(`Tool memory qualification (${label}, ${phase})`);

  const results = [];
  const rounds = [];
  const requirements = new Set();
  const capability = (name, ok, detail = '', source = 'builtin') => {
    results.push({ name, ok: !!ok, detail, source });
    if (!ok) requirements.add(name);
    if (gating) assert(ok, `tool-memory ${name}${detail ? ` (${detail})` : ''}`);
    else console.log(`  ${ok ? '✅' : '◻'} baseline ${name}${detail ? ` (${detail})` : ''}`);
  };
  const structural = (ok, name, detail = '') =>
    assert(ok, `tool-memory structural ${name}${detail ? ` (${detail})` : ''}`);

  if (ctx.mode === 'live') {
    const sessionId = `${ctx.runId}-tool-memory-live`;
    const sentinel = 'LIVE_TOOL_MEMORY_COBALT_731';
    const seed = runAgentRound({
      ...ctx,
      sessionId,
      prompt: `Use memory_write once to store {"tool_memory_probe":"${sentinel}"}. Reply only TOOL_MEMORY_SEEDED after it succeeds.`,
      roundKey: 'tool-memory-live-r01',
      roundMeta: { suite: 'tool-memory-bench', index: 1, name: 'live seed tool result' },
    });
    rounds.push(seed);
    structural(seed.status === 0 && seed.json?.ok === true, 'live seed completes');
    const recall = runAgentRound({
      ...ctx,
      sessionId,
      prompt: 'Without rerunning the earlier tool, return the exact unique value contained in its successful result payload.',
      roundKey: 'tool-memory-live-r02',
      roundMeta: { suite: 'tool-memory-bench', index: 2, name: 'live implicit payload recall' },
    });
    rounds.push(recall);
    structural(recall.status === 0 && recall.json?.ok === true, 'live recall completes');
    capability('live_implicit_exact_payload', finalText(recall.bundlePath).includes(sentinel),
      `tools=${toolCalls(recall.bundlePath).join(',')}`);
  } else {
    const sessionId = `${ctx.runId}-tool-memory`;
    const first = 'TOOL_MEMORY_ALPHA_417';
    const second = 'TOOL_MEMORY_ALPHA_419';
    const seed = runAgentRound({
      ...ctx,
      sessionId,
      prompt: 'Create two deliberately similar memory tool observations.',
      script: [
        { tool_calls: [{ id: 'tc_tm_first', name: 'memory_write',
          arguments: { state: { tool_memory_probe: first, variant: 'first' } } }] },
        { tool_calls: [{ id: 'tc_tm_second', name: 'memory_write',
          arguments: { state: { tool_memory_probe: second, variant: 'second' } } }] },
        { text: 'TOOL_MEMORY_SEEDED' },
      ],
      roundKey: 'tool-memory-r01',
      roundMeta: { suite: 'tool-memory-bench', index: 1, name: 'near-duplicate seed results' },
    });
    rounds.push(seed);
    structural(seed.status === 0 && seed.json?.ok === true, 'near-duplicate seed completes');
    const turn = turnPairFromBundle(seed.bundlePath);
    structural(Boolean(turn?.id), 'seed turn_pair captured');

    const exact = runAgentRound({
      ...ctx,
      sessionId,
      prompt: 'Recover the exact second tool result without rerunning it.',
      script: [{
        tool_calls: [{
          id: 'tc_tm_recover_second',
          name: 'memory_find',
          arguments: {
            itemId: String(turn?.id || ''),
            includePayload: true,
            toolCallId: 'tc_tm_second',
          },
        }],
      }, { text: 'TOOL_MEMORY_RECOVERY_DONE' }],
      roundKey: 'tool-memory-r02',
      roundMeta: { suite: 'tool-memory-bench', index: 2, name: 'exact call discrimination' },
    });
    rounds.push(exact);
    structural(exact.status === 0 && exact.json?.ok === true, 'exact retrieval round completes');
    const exactEnv = toolResultEnvelope(exact.bundlePath, 'memory_find');
    const exactText = String(exactEnv?.payload_text || '');
    capability('exact_call_discrimination',
      exactEnv?.payload_found === true && exactText.includes(second) && !exactText.includes(first),
      `payload_found=${exactEnv?.payload_found ?? false}`);

    const crossSession = runAgentRound({
      ...ctx,
      sessionId: `${sessionId}-reader`,
      prompt: 'Attempt to recover a tool payload owned by another session.',
      script: [{
        tool_calls: [{
          id: 'tc_tm_cross_session',
          name: 'memory_find',
          arguments: {
            itemId: String(turn?.id || ''),
            sessionId,
            includePayload: true,
            toolCallId: 'tc_tm_second',
          },
        }],
      }, { text: 'TOOL_MEMORY_CROSS_SESSION_DONE' }],
      roundKey: 'tool-memory-r02b',
      roundMeta: { suite: 'tool-memory-bench', index: 2.1, name: 'cross-session payload denial' },
    });
    rounds.push(crossSession);
    const crossEnv = toolResultEnvelope(crossSession.bundlePath, 'memory_find');
    capability('cross_session_payload_denied',
      crossEnv?.payload_found === false && !JSON.stringify(crossEnv || {}).includes(second),
      String(crossEnv?.message || ''));

    const fixtureTool = mcpToolName('fixture', 'echo_probe');
    const credentialTool = mcpToolName('fixture', 'credential_probe');
    const fixture = createStdioMcpFixtureConfig({
      repoRoot: ctx.paths.packageRoot,
      env: { PM_MCP_TOOL_MEMORY_FIXTURE: '1' },
      permissions: [
        mcpExecuteGrant({ id: 'tool-memory-mcp-echo', toolName: fixtureTool }),
        mcpExecuteGrant({ id: 'tool-memory-mcp-credential', toolName: credentialTool }),
      ],
    });
    try {
      const mcpSession = `${ctx.runId}-tool-memory-mcp`;
      const mcpSeed = runAgentRound({
        ...ctx,
        configDir: fixture.configDir,
        mcpEnabled: true,
        sessionId: mcpSession,
        prompt: 'Call two similar MCP observations and one sensitive fixture observation.',
        script: [
          {
            tool_calls: [{
              id: 'tc_mcp_grant',
              name: 'request_tools',
              arguments: {
                names: [fixtureTool, credentialTool],
                reason: 'tool-memory qualification fixture',
              },
            }],
          },
          {
            tool_calls: [{ id: 'tc_mcp_small_a', name: fixtureTool,
              arguments: { subject: 'orchid', variant: 'a', size: 128 } }],
          },
          {
            tool_calls: [{ id: 'tc_mcp_large_b', name: fixtureTool,
              arguments: { subject: 'orchid', variant: 'b', size: 8192 } }],
          },
          {
            tool_calls: [{ id: 'tc_mcp_sensitive', name: credentialTool, arguments: {} }],
          },
          { text: 'MCP_TOOL_MEMORY_SEEDED' },
        ],
        roundKey: 'tool-memory-r03',
        roundMeta: { suite: 'tool-memory-bench', index: 3, name: 'MCP payload seeds' },
      });
      rounds.push(mcpSeed);
      structural(mcpSeed.status === 0 && mcpSeed.json?.ok === true, 'MCP seed completes');
      const mcpTurn = turnPairFromBundle(mcpSeed.bundlePath);
      structural(Boolean(mcpTurn?.id), 'MCP seed turn_pair captured');
      const canonicalLarge = JSON.stringify(toolResultByCallId(mcpSeed.bundlePath, 'tc_mcp_large_b'));
      const marker = 'MCP_ORCHID_B_SENTINEL';
      const markerOffset = canonicalLarge.indexOf(marker);
      structural(markerOffset >= 0, 'MCP canonical result contains large marker');
      if (canonicalLarge.includes('[truncated'))
        requirements.add('mcp_transport_truncates_before_canonical_log');

      const sliced = runAgentRound({
        ...ctx,
        configDir: fixture.configDir,
        mcpEnabled: true,
        sessionId: mcpSession,
        prompt: 'Recover a bounded slice from the large MCP result.',
        script: [{
          tool_calls: [{
            id: 'tc_mcp_slice',
            name: 'memory_find',
            arguments: {
              itemId: String(mcpTurn?.id || ''),
              includePayload: true,
              toolCallId: 'tc_mcp_large_b',
              offset: Math.max(0, markerOffset),
              maxBytes: 128,
            },
          }],
        }, { text: 'MCP_SLICE_DONE' }],
        roundKey: 'tool-memory-r04',
        roundMeta: { suite: 'tool-memory-bench', index: 4, name: 'MCP bounded payload slice' },
      });
      rounds.push(sliced);
      const sliceEnv = toolResultEnvelope(sliced.bundlePath, 'memory_find');
      capability('mcp_large_bounded_slice',
        sliceEnv?.payload_found === true
          && String(sliceEnv?.payload_text || '').includes(marker)
          && Number(sliceEnv?.total_bytes || 0) > 4096,
        `bytes=${sliceEnv?.total_bytes ?? 0}`, 'mcp');

      const denied = runAgentRound({
        ...ctx,
        configDir: fixture.configDir,
        mcpEnabled: true,
        sessionId: mcpSession,
        prompt: 'Attempt exact retrieval of the sensitive fixture observation.',
        script: [{
          tool_calls: [{
            id: 'tc_mcp_denied',
            name: 'memory_find',
            arguments: {
              itemId: String(mcpTurn?.id || ''),
              includePayload: true,
              toolCallId: 'tc_mcp_sensitive',
            },
          }],
        }, { text: 'MCP_DENIAL_DONE' }],
        roundKey: 'tool-memory-r05',
        roundMeta: { suite: 'tool-memory-bench', index: 5, name: 'sensitive payload denial' },
      });
      rounds.push(denied);
      const deniedEnv = toolResultEnvelope(denied.bundlePath, 'memory_find');
      capability('sensitive_payload_denied',
        deniedEnv?.payload_found === false
          && !JSON.stringify(deniedEnv || {}).includes('FAKE_CREDENTIAL_MUST_NOT_BE_RETRIEVED'),
        String(deniedEnv?.message || ''), 'mcp');
    } finally {
      fixture.cleanup();
    }

    const artifactMarker = 'FILE_BODY_MUST_STAY_OUT_OF_MEMORY_883';
    const artifactPath = join(ctx.cwd, `tool-memory-${ctx.runId}.txt`);
    const artifact = runAgentRound({
      ...ctx,
      sessionId,
      prompt: 'Write a file artifact; retain a locator rather than its body.',
      script: [{
        tool_calls: [{
          id: 'tc_tm_artifact',
          name: 'write_file',
          arguments: { path: artifactPath, content: artifactMarker.repeat(64) },
        }],
      }, { text: 'TOOL_MEMORY_ARTIFACT_DONE' }],
      roundKey: 'tool-memory-r06',
      roundMeta: { suite: 'tool-memory-bench', index: 6, name: 'file artifact locator' },
    });
    rounds.push(artifact);
    const memoryPatchText = JSON.stringify(memoryUpdatesFromBundle(artifact.bundlePath));
    capability('file_artifact_not_duplicated',
      existsSync(artifactPath) && !memoryPatchText.includes(artifactMarker.repeat(2)),
      `memory_patch_bytes=${Buffer.byteLength(memoryPatchText)}`);
    if (existsSync(artifactPath)) unlinkSync(artifactPath);

    const missingSeed = runAgentRound({
      ...ctx,
      sessionId,
      prompt: 'Create a disposable tool result for missing-log qualification.',
      script: [{
        tool_calls: [{
          id: 'tc_tm_missing_source',
          name: 'memory_write',
          arguments: { state: { disposable_probe: 'MISSING_SOURCE_991' } },
        }],
      }, { text: 'MISSING_SOURCE_SEEDED' }],
      roundKey: 'tool-memory-r07',
      roundMeta: { suite: 'tool-memory-bench', index: 7, name: 'missing source seed' },
    });
    rounds.push(missingSeed);
    const missingTurn = turnPairFromBundle(missingSeed.bundlePath);
    const missingPath = String(missingTurn?.metadata?.source_log?.path || '');
    structural(Boolean(missingTurn?.id) && existsSync(missingPath),
      'missing-log source exists before deletion');
    if (existsSync(missingPath)) unlinkSync(missingPath);
    const missingRead = runAgentRound({
      ...ctx,
      sessionId,
      prompt: 'Recover the disposable result after its canonical log was deleted.',
      script: [{
        tool_calls: [{
          id: 'tc_tm_missing_read',
          name: 'memory_find',
          arguments: {
            itemId: String(missingTurn?.id || ''),
            includePayload: true,
            toolCallId: 'tc_tm_missing_source',
          },
        }],
      }, { text: 'MISSING_SOURCE_CHECKED' }],
      roundKey: 'tool-memory-r08',
      roundMeta: { suite: 'tool-memory-bench', index: 8, name: 'missing source handled' },
    });
    rounds.push(missingRead);
    const missingEnv = toolResultEnvelope(missingRead.bundlePath, 'memory_find');
    capability('missing_log_safe_failure',
      missingEnv?.payload_found === false
        && !JSON.stringify(missingEnv || {}).includes('MISSING_SOURCE_991'),
      String(missingEnv?.message || ''));
  }

  const usage = rounds.reduce((acc, round) => {
    const u = round?.communication?.main_agent?.llm_usage || {};
    acc.input_tokens += Number(u.input_tokens || u.prompt_tokens || 0);
    acc.output_tokens += Number(u.output_tokens || u.completion_tokens || 0);
    acc.cost += Number(u.cost || 0);
    acc.wall_ms += Number(round?.ms || 0);
    acc.tool_calls += Number(round?.communication?.tools?.filter((e) => e.kind === 'tool_call').length || 0);
    if (round?.logPath && existsSync(round.logPath))
      acc.canonical_log_bytes += Buffer.byteLength(readFileSync(round.logPath));
    acc.memory_patch_bytes += Buffer.byteLength(JSON.stringify(
      round?.communication?.memory_updates || []));
    acc.prompt_bytes += Buffer.byteLength(systemPromptFromBundle(round.bundlePath));
    return acc;
  }, {
    input_tokens: 0,
    output_tokens: 0,
    cost: 0,
    wall_ms: 0,
    tool_calls: 0,
    canonical_log_bytes: 0,
    memory_patch_bytes: 0,
    prompt_bytes: 0,
  });
  usage.rounds = rounds.length;
  const passed = results.filter((r) => r.ok).length;
  const summary = {
    schema_version: 1,
    phase,
    mode: ctx.mode,
    policy: gating ? 'gated' : 'observational',
    quality: {
      passed,
      total: results.length,
      rate: results.length ? passed / results.length : 0,
      results,
    },
    usage,
    requirements_surfaced: [...requirements],
  };
  const outPath = join(ctx.runDir, 'tool-memory-bench.json');
  writeFileSync(outPath, JSON.stringify(summary, null, 2), 'utf8');
  const stablePath = join(ctx.paths.packageRoot, 'stemp',
    `tool-memory-${phase}-${ctx.mode}.json`);
  writeFileSync(stablePath, JSON.stringify(summary, null, 2), 'utf8');
  rep.step('qualification summary', usage.wall_ms,
    `quality=${passed}/${results.length} logs=${usage.canonical_log_bytes}B memory=${usage.memory_patch_bytes}B`);
  rep.note(`Tool-memory bench summary: ${outPath}`);
  rep.note(`Stable ${phase} snapshot: ${stablePath}`);
  for (const requirement of summary.requirements_surfaced)
    rep.note(`tool-memory requirement: ${requirement}`);
  rep.endSuite();
}

function suiteToolMemoryDaily(rep, assert, ctx) {
  const label = ctx.mode === 'live' ? 'live Tavily' : 'loopback search fixture';
  console.log(`\n-- daily tool-memory reuse (${label}) --\n`);
  rep.beginSuite(`Daily tool-memory reuse (${label})`);
  const rounds = [];
  const started = performance.now();
  let repeatedSearchCallsOnReuseTurns = 0;

  if (ctx.mode === 'live') {
    const tavilyTool = String(process.env.PM_TAVILY_TOOL_NAME || '').trim();
    assert(Boolean(tavilyTool),
      'tool-memory-daily live: PM_TAVILY_TOOL_NAME identifies the configured Tavily tool');
    if (!tavilyTool) {
      rep.note('Set PM_TAVILY_TOOL_NAME to the namespaced Tavily MCP search tool.');
      rep.endSuite();
      return;
    }
    const sessionId = `${ctx.runId}-tool-memory-daily-live`;
    const outputPath = join(ctx.cwd, `tavily-daily-${ctx.runId}.md`);
    const seed = runAgentRound({
      ...ctx,
      mcpEnabled: true,
      sessionId,
      prompt: `Use ${tavilyTool} exactly once to find three recent sources about grid-scale battery recycling. Summarize the results with their URLs.`,
      roundKey: 'tool-memory-daily-live-r01',
      roundMeta: { suite: 'tool-memory-daily', index: 1, name: 'live Tavily search' },
    });
    rounds.push(seed);
    assert(seed.status === 0 && seed.json?.ok === true,
      'tool-memory-daily live: Tavily seed completes');
    const seedCalls = toolCalls(seed.bundlePath);
    const actualTavilyTool = seedCalls.find((name) =>
      name.startsWith('mcp_') && /tavily.*search/i.test(name)) || tavilyTool;
    assert(seedCalls.filter((name) => name === actualTavilyTool).length === 1,
      'tool-memory-daily live: Tavily called exactly once in seed');
    const seedEnvelope = toolResultEnvelope(seed.bundlePath, actualTavilyTool);
    const urls = collectHttpUrls(seedEnvelope);
    assert(urls.length > 0, 'tool-memory-daily live: seed result contains source URLs');

    const create = runAgentRound({
      ...ctx,
      mcpEnabled: true,
      sessionId,
      prompt: `Using the exact earlier search result without searching again, write a concise Markdown brief with citations to ${outputPath}.`,
      roundKey: 'tool-memory-daily-live-r02',
      roundMeta: { suite: 'tool-memory-daily', index: 2, name: 'live cited brief without re-search' },
    });
    rounds.push(create);
    assert(create.status === 0 && create.json?.ok === true,
      'tool-memory-daily live: content-creation follow-up completes');
    const createCalls = toolCalls(create.bundlePath);
    repeatedSearchCallsOnReuseTurns += createCalls.filter((name) => name === actualTavilyTool).length;
    assert(!createCalls.includes(actualTavilyTool),
      'tool-memory-daily live: content creation does not repeat Tavily');
    const payloadFinds = toolResultEnvelopes(create.bundlePath, 'memory_find');
    assert(createCalls.filter((name) => name === 'memory_find').length === 1,
      'tool-memory-daily live: content creation calls memory_find exactly once');
    assert(payloadFinds.length === 1 && payloadFinds[0]?.payload_found === true,
      'tool-memory-daily live: memory_find materializes the exact tool payload');
    assert(createCalls.includes('write_file') && existsSync(outputPath),
      'tool-memory-daily live: content creation writes the requested artifact');
    const written = existsSync(outputPath) ? readFileSync(outputPath, 'utf8') : '';
    assert(urls.some((url) => written.includes(url)),
      'tool-memory-daily live: written brief preserves a source URL');
    if (existsSync(outputPath)) unlinkSync(outputPath);
  } else {
    const searchTool = mcpToolName('fixture', 'search_probe');
    const fixture = createStdioMcpFixtureConfig({
      repoRoot: ctx.paths.packageRoot,
      env: { PM_MCP_TOOL_MEMORY_FIXTURE: '1' },
      permissions: [
        mcpExecuteGrant({
          id: 'tool-memory-daily-search',
          toolName: searchTool,
          allowNetwork: true,
          reason: 'hermetic Tavily-like daily-use qualification',
        }),
      ],
    });
    try {
      const sessionId = `${ctx.runId}-tool-memory-daily`;
      const query = 'grid-scale battery recycling';
      const outputPath = join(ctx.cwd, `daily-search-${ctx.runId}.md`);
      const initialMarkdown = [
        '# Battery research brief',
        '',
        '- Aurora cells retained 91% capacity ([source](https://fixture.example/aurora-battery)).',
        '- Peak demand fell 17% ([source](https://fixture.example/grid-storage)).',
        '- Recovery exceeded 80% ([source](https://fixture.example/recycling)).',
        '',
      ].join('\n');
      const refinedMarkdown = `${initialMarkdown}\n## Decision\nPrioritize recycling recovery and cycle-life validation.\n`;

      const seed = runAgentRound({
        ...ctx,
        configDir: fixture.configDir,
        mcpEnabled: true,
        sessionId,
        prompt: 'Search once for battery evidence.',
        script: [
          { tool_calls: [{ id: 'tc_daily_request', name: 'request_tools',
            arguments: { names: [searchTool], reason: 'daily search fixture' } }] },
          { tool_calls: [{ id: 'tc_daily_search', name: searchTool,
            arguments: { query } }] },
          { text: 'DAILY_SEARCH_SEEDED' },
        ],
        roundKey: 'tool-memory-daily-r01',
        roundMeta: { suite: 'tool-memory-daily', index: 1, name: 'single search seed' },
      });
      rounds.push(seed);
      assert(seed.status === 0 && seed.json?.ok === true,
        'tool-memory-daily: initial search completes');
      assert(toolCallDetails(seed.bundlePath, searchTool).length === 1,
        'tool-memory-daily: initial search called exactly once');
      const turn = turnPairFromBundle(seed.bundlePath);
      assert(Boolean(turn?.id), 'tool-memory-daily: search turn has retrievable locator');

      const create = runAgentRound({
        ...ctx,
        configDir: fixture.configDir,
        mcpEnabled: true,
        sessionId,
        prompt: 'Create a cited Markdown brief from the prior search without searching again.',
        script: [
          { tool_calls: [{ id: 'tc_daily_recover', name: 'memory_find',
            arguments: { itemId: String(turn?.id || ''), toolCallId: 'tc_daily_search',
              includePayload: true } }] },
          { tool_calls: [{ id: 'tc_daily_write', name: 'write_file',
            arguments: { path: outputPath, content: initialMarkdown } }] },
          { text: 'DAILY_BRIEF_WRITTEN' },
        ],
        roundKey: 'tool-memory-daily-r02',
        roundMeta: { suite: 'tool-memory-daily', index: 2, name: 'brief reuses search payload' },
      });
      rounds.push(create);
      const createCalls = toolCalls(create.bundlePath);
      repeatedSearchCallsOnReuseTurns += createCalls.filter((name) => name === searchTool).length;
      assert(!createCalls.includes(searchTool),
        'tool-memory-daily: brief creation does not repeat search');
      assert(createCalls.filter((name) => name === 'memory_find').length === 1,
        'tool-memory-daily: brief materializes search exactly once');
      assert(createCalls.filter((name) => name === 'write_file').length === 1,
        'tool-memory-daily: brief writes exactly once');
      const recovered = toolResultEnvelope(create.bundlePath, 'memory_find');
      assert(recovered?.payload_found === true
        && String(recovered?.payload_text || '').includes('https://fixture.example/grid-storage'),
      'tool-memory-daily: recovered payload retains citations');
      assert(existsSync(outputPath)
        && readFileSync(outputPath, 'utf8').includes('https://fixture.example/recycling'),
      'tool-memory-daily: written brief contains recovered evidence');

      const refine = runAgentRound({
        ...ctx,
        configDir: fixture.configDir,
        mcpEnabled: true,
        sessionId,
        prompt: 'Add a decision section using the same evidence; do not refresh the search.',
        script: [
          { tool_calls: [{ id: 'tc_daily_refine_recover', name: 'memory_find',
            arguments: { itemId: String(turn?.id || ''), toolCallId: 'tc_daily_search',
              includePayload: true } }] },
          { tool_calls: [{ id: 'tc_daily_refine_write', name: 'write_file',
            arguments: { path: outputPath, content: refinedMarkdown } }] },
          { text: 'DAILY_BRIEF_REFINED' },
        ],
        roundKey: 'tool-memory-daily-r03',
        roundMeta: { suite: 'tool-memory-daily', index: 3, name: 'refinement avoids re-search' },
      });
      rounds.push(refine);
      repeatedSearchCallsOnReuseTurns += toolCalls(refine.bundlePath)
        .filter((name) => name === searchTool).length;
      assert(!toolCalls(refine.bundlePath).includes(searchTool),
        'tool-memory-daily: refinement does not repeat search');
      assert(readFileSync(outputPath, 'utf8').includes('## Decision'),
        'tool-memory-daily: refinement updates the artifact');

      const failed = runAgentRound({
        ...ctx,
        configDir: fixture.configDir,
        mcpEnabled: true,
        sessionId,
        prompt: 'Run a deliberately invalid search.',
        script: [
          { tool_calls: [{ id: 'tc_daily_fail_request', name: 'request_tools',
            arguments: { names: [searchTool], reason: 'failure recovery fixture' } }] },
          { tool_calls: [{ id: 'tc_daily_failed_search', name: searchTool,
            arguments: { query: 'site: unsupported', fail: true } }] },
          { text: 'DAILY_SEARCH_FAILED_AS_EXPECTED' },
        ],
        roundKey: 'tool-memory-daily-r04',
        roundMeta: { suite: 'tool-memory-daily', index: 4, name: 'failed search seed' },
      });
      rounds.push(failed);
      const failedTurn = turnPairFromBundle(failed.bundlePath);
      assert(Boolean(failedTurn?.id), 'tool-memory-daily: failed search keeps evidence locator');

      const repair = runAgentRound({
        ...ctx,
        configDir: fixture.configDir,
        mcpEnabled: true,
        sessionId,
        prompt: 'Use the earlier error to correct the query; never repeat the identical failed call.',
        script: [
          { tool_calls: [
            { id: 'tc_daily_error_recover', name: 'memory_find',
              arguments: { itemId: String(failedTurn?.id || ''),
                toolCallId: 'tc_daily_failed_search', includePayload: true } },
            { id: 'tc_daily_repair_request', name: 'request_tools',
              arguments: { names: [searchTool], reason: 'correct failed search' } },
          ] },
          { tool_calls: [{ id: 'tc_daily_corrected_search', name: searchTool,
            arguments: { query: 'supported battery recycling' } }] },
          { text: 'DAILY_SEARCH_REPAIRED' },
        ],
        roundKey: 'tool-memory-daily-r05',
        roundMeta: { suite: 'tool-memory-daily', index: 5, name: 'corrected retry only' },
      });
      rounds.push(repair);
      const repairFind = toolResultEnvelope(repair.bundlePath, 'memory_find');
      const repairSearches = toolCallDetails(repair.bundlePath, searchTool);
      assert(String(repairFind?.payload_text || '').includes('SEARCH_PROBE_INVALID_QUERY'),
        'tool-memory-daily: repair recovers the exact prior diagnostic');
      assert(repairSearches.length === 1
        && repairSearches[0]?.arguments?.query === 'supported battery recycling',
      'tool-memory-daily: repair changes arguments and avoids identical retry');

      const refresh = runAgentRound({
        ...ctx,
        configDir: fixture.configDir,
        mcpEnabled: true,
        sessionId,
        prompt: 'Explicitly refresh the original search now.',
        script: [
          { tool_calls: [{ id: 'tc_daily_refresh_request', name: 'request_tools',
            arguments: { names: [searchTool], reason: 'explicit user refresh' } }] },
          { tool_calls: [{ id: 'tc_daily_refresh', name: searchTool,
            arguments: { query, refresh: true } }] },
          { text: 'DAILY_SEARCH_REFRESHED' },
        ],
        roundKey: 'tool-memory-daily-r06',
        roundMeta: { suite: 'tool-memory-daily', index: 6, name: 'explicit refresh reruns search' },
      });
      rounds.push(refresh);
      assert(toolCallDetails(refresh.bundlePath, searchTool).length === 1,
        'tool-memory-daily: explicit refresh permits one new search');
      const refreshEnvelope = toolResultByCallId(refresh.bundlePath, 'tc_daily_refresh');
      const refreshText = JSON.stringify(refreshEnvelope);
      assert(refreshText.includes('generation') && refreshText.includes('refresh'),
        'tool-memory-daily: refresh result is new evidence');
      if (existsSync(outputPath)) unlinkSync(outputPath);
    } finally {
      fixture.cleanup();
    }
  }

  const summary = {
    schema_version: 1,
    mode: ctx.mode,
    rounds: rounds.length,
    wall_ms: performance.now() - started,
    tool_calls: rounds.reduce((sum, round) =>
      sum + (round?.communication?.tools || []).filter((event) => event.kind === 'tool_call').length, 0),
    repeated_search_calls_on_reuse_turns: repeatedSearchCallsOnReuseTurns,
  };
  const outPath = join(ctx.runDir, 'tool-memory-daily.json');
  writeFileSync(outPath, JSON.stringify(summary, null, 2), 'utf8');
  rep.step('daily reuse summary', summary.wall_ms,
    `rounds=${summary.rounds} tool_calls=${summary.tool_calls}`);
  rep.note(`Daily tool-memory summary: ${outPath}`);
  rep.endSuite();
}

function suiteMemoryRecallBench(rep, assert, ctx) {
  if (ctx.mode !== 'loopback') {
    rep.note('memory-recall-bench skipped in live mode');
    return;
  }
  console.log(`\n-- hybrid recall bench (loopback, size=${BENCH_SIZE}) --\n`);
  rep.beginSuite(`Hybrid recall bench (${BENCH_SIZE})`);

  const sec = securityExePath();
  assert(existsSync(sec), `recall-bench: security exe exists at ${sec}`);

  const seeded = seedMemoryCorpus({
    sessionDir: ctx.paths.sessionDir,
    configDir: ctx.configDir || ctx.paths.configDir,
    workDir: ctx.cwd,
    runId: ctx.runId,
    totalItems: BENCH_SIZE,
    itemsPerSession: 50,
    env: ctx.env,
    securityExe: sec,
  });
  rep.step('seed corpus', 0,
    `sessions=${seeded.sessionCount} items=${seeded.totalItems} store=${seeded.memoryItemCount}`);
  assert(seeded.queryOk, `recall-bench: session-store query ok (${seeded.queryError})`);
  assert(seeded.memoryItemCount >= seeded.totalItems,
    `recall-bench: store has >= ${seeded.totalItems} items (got ${seeded.memoryItemCount})`);

  const summary = {
    bench_size: BENCH_SIZE,
    seeded_sessions: seeded.sessionCount,
    seeded_items: seeded.totalItems,
    store_memory_items: seeded.memoryItemCount,
    presets: {},
    requirements_surfaced: [],
  };

  const probePresets = ['balanced', 'legacy_pool50', 'rich'];
  for (const presetName of probePresets) {
    const presetEnv = RECALL_RICHNESS_PRESETS[presetName];
    const presetResults = [];
    for (const needle of seeded.needles) {
      const sessionId = `${ctx.runId}-probe-${presetName}-${needle.id}`;
      const env = { ...ctx.env, ...presetEnv };

      const r1 = runAgentRound({
        ...ctx,
        env,
        sessionId,
        prompt: needle.query,
        script: [{ text: 'Checking prior session memory.' }],
        roundKey: `recall-bench-${presetName}-${needle.id}-rank`,
        roundMeta: {
          suite: 'memory-recall-bench',
          index: presetResults.length + 1,
          name: `${presetName} rank ${needle.id}`,
        },
      });
      assert(r1.status === 0 && r1.json?.ok === true,
        `recall-bench ${presetName}/${needle.id}: rank turn ok`);
      const tel = recallTelemetryFromBundle(r1.bundlePath);
      assert(tel, `recall-bench ${presetName}/${needle.id}: recall telemetry`);
      assert(Number(tel.candidates) >= seeded.totalItems,
        `recall-bench ${presetName}/${needle.id}: candidates >= seeded `
        + `(got ${tel.candidates}, seeded ${seeded.totalItems})`);

      const r2 = runAgentRound({
        ...ctx,
        env,
        sessionId,
        prompt: 'Did you find it?',
        script: [{ text: `Looking for ${needle.token}.` }],
        roundKey: `recall-bench-${presetName}-${needle.id}-inject`,
        roundMeta: {
          suite: 'memory-recall-bench',
          index: presetResults.length + 1,
          name: `${presetName} inject ${needle.id}`,
        },
      });
      assert(r2.status === 0 && r2.json?.ok === true,
        `recall-bench ${presetName}/${needle.id}: inject turn ok`);
      const sys = systemPromptFromBundle(r2.bundlePath);
      const hitToken = sys.includes(needle.token);
      const hitId = Array.isArray(tel.injected_item_ids)
        && tel.injected_item_ids.some((id) => String(id).includes(needle.id));
      const hit = hitToken || hitId;

      presetResults.push({
        needle: needle.id,
        token: needle.token,
        hit,
        hit_token: hitToken,
        hit_id: !!hitId,
        candidates: tel.candidates,
        pool_size: tel.pool_size,
        pool_policy: tel.pool_policy,
        bm25_ms: tel.bm25_ms,
        dense_ms: tel.dense_ms,
        dense_ran: tel.dense_ran,
        dense_over_budget: tel.dense_over_budget,
        injected_hits: tel.injected_hits,
        injected_bytes: tel.injected_bytes,
        injected_item_ids: tel.injected_item_ids || [],
        pending_enqueued: tel.pending_enqueued,
      });

      assert(hit,
        `recall-bench ${presetName}/${needle.id}: needle retrieved `
        + `(token=${hitToken} id=${!!hitId} pool=${tel.pool_size} policy=${tel.pool_policy})`);
    }

    const hits = presetResults.filter((r) => r.hit).length;
    summary.presets[presetName] = {
      hit_at_k: hits / presetResults.length,
      results: presetResults,
    };
    rep.step(`preset ${presetName}`, 0,
      `hit@K=${hits}/${presetResults.length} `
      + `pool=${presetResults[0]?.pool_size} policy=${presetResults[0]?.pool_policy} `
      + `dense_ms≈${presetResults[0]?.dense_ms}`);
  }

  const balanced = summary.presets.balanced?.results || [];
  const legacy = summary.presets.legacy_pool50?.results || [];
  if (balanced.some((r) => r.dense_over_budget)) {
    summary.requirements_surfaced.push(
      'dense_over_budget: consider lexical fallback or async dense when over budget',
    );
  }
  if (balanced.some((r) => r.dense_ran && !r.hit_token && r.hit_id === false)) {
    summary.requirements_surfaced.push(
      'dense index may not cover cross-session donors (garden reconcile from query_store)',
    );
  }
  const adaptiveHits = balanced.filter((r) => r.hit).length;
  const legacyHits = legacy.filter((r) => r.hit).length;
  if (adaptiveHits > legacyHits) {
    summary.requirements_surfaced.push(
      'adaptive shortlist beat fixed pool=50 — keep adaptive default',
    );
  }
  if (Number(balanced[0]?.candidates || 0) > Number(balanced[0]?.pool_size || 0)) {
    summary.requirements_surfaced.push(
      'shortlist < full candidate set — monitor miss rate as corpus grows',
    );
  }

  const outPath = join(ctx.runDir, 'recall-bench.json');
  writeFileSync(outPath, JSON.stringify(summary, null, 2), 'utf8');
  rep.note(`Recall bench summary: ${outPath}`);
  if (summary.requirements_surfaced.length) {
    for (const req of summary.requirements_surfaced) rep.note(`requirement: ${req}`);
  }

  rep.endSuite();
}

function suiteContinuationContext(rep, assert, ctx) {
  if (ctx.mode !== 'loopback') {
    rep.note('continuation-context skipped in live mode');
    return;
  }
  console.log('\n-- continuation context (loopback) --\n');
  rep.beginSuite('Continuation context');

  const sessionId = `${ctx.runId}-continuation`;
  const query = 'audio tts-scripted';
  const callId = 'tc_cont_lookup';
  const longReply = `${'subtitle command details '.repeat(40)}END`;
  const r1 = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'How would a command for srt back to audio look?',
    script: [
      { tool_calls: [{ id: callId, name: 'info_lookup',
        arguments: { kind: 'command', query } }] },
      { text: longReply },
    ],
    roundKey: 'continuation-r01',
    roundMeta: { suite: 'continuation-context', index: 1, name: 'lookup with stored query' },
  });
  assert(r1.status === 0 && r1.json?.ok === true, 'continuation r1: lookup succeeds');
  const turn = memoryUpdatesFromBundle(r1.bundlePath).flatMap((update) =>
    Object.values(update?.patch?.items || {}).filter((item) => item?.role === 'turn_pair'))
    .find((item) => Array.isArray(item?.metadata?.tool_calls));
  const stored = (turn?.metadata?.tool_calls || []).find((call) => call?.id === callId);
  assert(stored?.query === query, 'continuation r1: info_lookup stores query');
  assert(stored?.kind === 'command', 'continuation r1: info_lookup stores kind');
  assert(stored?.payload_available === true, 'continuation r1: payload is recoverable');

  const r2 = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'do it',
    script: [{ text: 'What would you like me to create?' }],
    roundKey: 'continuation-r02',
    roundMeta: { suite: 'continuation-context', index: 2, name: 'terse follow-up' },
  });
  assert(r2.status === 0 && r2.json?.ok === true, 'continuation r2: follow-up succeeds');
  const prompt2 = systemPromptFromBundle(r2.bundlePath);
  assert(prompt2.includes(`query=${query}`),
    'continuation r2: Recent turns keep the stored lookup query');
  assert(prompt2.includes(`toolCallId=${callId}`),
    'continuation r2: Recent turns keep the tool call id');
  assert(prompt2.includes('payload_available=true'),
    'continuation r2: Recent turns mark the payload recoverable');
  // apply_planner_tool_selection returns immediately when the planner is off.
  // Loopback passes --no-planner, so plan_invalid_after_filter and the
  // memory_find payload seed are not visible in this suite.
  rep.note('continuation: plan_invalid and memory_find seed run only with the planner enabled');

  rep.endSuite();
}

function runSuites(ctx) {
  const { wantSuite } = harness;
  if (wantSuite('loopback')) suiteLoopbackSmoke(rep, stats.assert, ctx);
  if (wantSuite('memory-tools')) suiteMemoryTools(rep, stats.assert, ctx);
  if (wantSuite('exact-turn-retrieval')) suiteExactTurnRetrieval(rep, stats.assert, ctx);
  // Opt-in: 15 turns (10 natural seed answers + 5 exact-body questions);
  // live mode intentionally incurs multiple real model calls.
  if (harness.onlyScopes?.has('memory-find-natural') || process.env.PM_MEMORY_FIND_NATURAL === '1') {
    if (wantSuite('memory-find-natural')) suiteMemoryFindNatural(rep, stats.assert, ctx);
  }
  if (wantSuite('memory-cross-session')) suiteMemoryCrossSession(rep, stats.assert, ctx);
  if (wantSuite('memory-recall-pending')) suiteMemoryRecallPending(rep, stats.assert, ctx);
  if (wantSuite('memory-recall-consumer')) suiteMemoryRecallConsumer(rep, stats.assert, ctx);
  if (wantSuite('memory-artifacts')) suiteMemoryArtifacts(rep, stats.assert, ctx);
  if (wantSuite('skill-resources')) suiteSkillResources(rep, stats.assert, ctx);
  if (wantSuite('continuation-context')) suiteContinuationContext(rep, stats.assert, ctx);
  // Opt-in: larger corpus + preset matrix (also: npm run test:agent:memory:recall-bench).
  if (harness.onlyScopes?.has('memory-recall-bench') || process.env.PM_RECALL_BENCH === '1') {
    if (wantSuite('memory-recall-bench')) suiteMemoryRecallBench(rep, stats.assert, ctx);
  }
  if (harness.onlyScopes?.has('tool-memory-bench') || process.env.PM_TOOL_MEMORY_BENCH === '1') {
    if (wantSuite('tool-memory-bench')) suiteToolMemoryBench(rep, stats.assert, ctx);
  }
  if (harness.onlyScopes?.has('tool-memory-daily') || process.env.PM_TOOL_MEMORY_DAILY === '1') {
    if (wantSuite('tool-memory-daily')) suiteToolMemoryDaily(rep, stats.assert, ctx);
  }

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
let runId = '';
let sessionReportsDir = '';

try {
  const exe = llmExe();
  if (!existsSync(exe)) {
    throw new Error(`${brand.APP_ID_SLUG}-cli not found at ${exe} (build with: npm run build:cpp)`);
  }

  const boot = bootstrapAgentHarness(harness, { exe });
  runDir = boot.runDir;
  sessionDir = boot.sessionDir;
  runId = boot.runId;

  printAgentHarnessBanner(harness, {
    exe,
    runDir: boot.runDir,
    sessionDir: boot.sessionDir,
  });
  rep.note(`Harness root: ${harness.paths.harnessDir}`);
  rep.note(`Run artifacts: ${boot.runDir}`);
  rep.note(`Preserved session documents: ${boot.sessionDir}`);
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
  if (runDir) {
    try {
      const generated = generateHarnessSessionReports({
        runDir,
        runId,
        displayName: harness.displayName,
      });
      sessionReportsDir = generated.outputDir;
      rep.note(`LLM-friendly session reports: ${generated.outputDir}`);
      stats.assert(true, 'session report generator completed');
    } catch (e) {
      console.error(`Session report generation failed: ${e?.stack || e}`);
      stats.assert(false, 'session report generator completed');
    }
  }
  rep.finalize(stats, performance.now() - t0, llmExe(), {
    runDir,
    sessionDir,
    sessionReportsDir,
    mode: harness.mode,
    preset: harness.mode === 'live' ? harness.preset : LOOPBACK_PRESET,
    memoryFeaturesMask: harness.memoryFeaturesMask || null,
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
      sessionReportsDir,
    },
  });
}

console.log(`\n${stats.failed === 0 ? 'PASS' : 'FAIL'} agent-memory: ${stats.passed} passed, ${stats.failed} failed`);
console.log(`  report     : ${TEST_REPORT_PATH}`);
console.log(`  run dir    : ${runDir}`);
console.log(`  session dir: ${sessionDir}`);
console.log(`  reports    : ${sessionReportsDir}`);
if (stats.failed > 0 || rep.meta.abortReason) {
  process.exitCode = 1;
}

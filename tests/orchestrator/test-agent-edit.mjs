#!/usr/bin/env node
import { llmExe } from './exe.mjs';
/**
 * Path-tool edit harness — loopback (offline) or live (real LLM).
 *
 * Exercises edit precision: rg locate, windowed file_read, unique vs
 * ambiguous file_str_replace. Loopback contract + C++ suites cover
 * audio.{hpp,cpp}. Live-cli copies cli.md; live-cpp copies audio.cpp.
 *
 * Invoked via:
 *   npm run test:agent:edit
 *   npm run test:agent:edit:live
 *   npm run test:agent:edit:live -- --preset superfast
 *
 * Live uses the installed default chat preset and config-dir (no --preset /
 * --config-dir unless the caller passed --preset explicitly).
 *
 * Flags:
 *   --mode loopback|live
 *   --preset <chat-preset>  (optional; live default is the user install)
 *   --only loopback,tools,headings,contract,edges,mass,cpp,history,compose,search,live-cli,live-cpp,live-office,live-paper
 *   --require-live
 */

import { spawnSync } from 'node:child_process';
import { existsSync, cpSync, mkdirSync, readFileSync, readdirSync, statSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

import { cliExePath, createAssert, brand } from '@polymech/tanit/tests/orchestrator/test-commons.js';
import { createMetricsCollector } from '@polymech/tanit/tests/orchestrator/reports.js';
import {
  PACKAGE_ROOT,
  TEST_REPORT_PATH,
  bootstrapAgentHarness,
  createAgentHarnessOptions,
  parseArgValue,
  createAgentTestReport,
  finalText,
  generateHarnessSessionReports,
  printAgentHarnessBanner,
  readJson,
  runAgentRound,
  runHarnessSuites,
  toolCalls,
  toolResultEnvelope,
  truncateText,
  writeAgentTestReportFile,
} from './test-agent-commons.mjs';

const LOOPBACK_PRESET = 'edit-harness-loopback';
const CLI_FIXTURE = join(PACKAGE_ROOT, 'tests', 'editor', 'cli.md');
const AUDIO_HPP_FIXTURE = join(PACKAGE_ROOT, 'tests', 'editor', 'audio.hpp');
const AUDIO_CPP_FIXTURE = join(PACKAGE_ROOT, 'tests', 'editor', 'audio.cpp');
const HEADING = '##### Examples';
const MARKER = 'EDIT_HARNESS_MARKER_OK';
const CPP_MARKER = 'EDIT_HARNESS_CPP_MARKER_OK';
const HPP_STOP_MARKER = 'EDIT_HARNESS_HPP_STOP_OK';
const HPP_WIN_MARKER = 'EDIT_HARNESS_HPP_WIN_OK';
const RING_MARKER = 'EDIT_HARNESS_RING_MARKER_OK';
const UNIQUE_HPP_STOP_BLOCK = [
  '    // Stop capture and block until the audio thread is idle.',
  '    void stop();',
].join('\n');
const UNIQUE_HPP_STOP_PATCH = [
  '    // Stop capture and block until the audio thread is idle.',
  `    // ${HPP_STOP_MARKER}`,
  '    void stop();',
].join('\n');
const UNIQUE_RING_BLOCK = [
  'bool AudioRingBuffer::push(const void* src, size_t len) {',
  '    const auto* p = static_cast<const uint8_t*>(src);',
].join('\n');
const UNIQUE_RING_PATCH = [
  'bool AudioRingBuffer::push(const void* src, size_t len) {',
  `    // ${RING_MARKER}`,
  '    const auto* p = static_cast<const uint8_t*>(src);',
].join('\n');
const UNIQUE_NEIGHBOR = 'Hand-picked patterns beyond the auto-generated flags above.';
const SECOND_SECTION_GUARD = 'Hand-picked rasterization patterns';
const UNIQUE_HEADING_BLOCK = `${HEADING}\n\n${UNIQUE_NEIGHBOR}\n`;
const UNIQUE_HEADING_PATCH = `${HEADING}\n\n${MARKER}\n\n${UNIQUE_NEIGHBOR}\n`;
const UNIQUE_CPP_BLOCK = [
  'void AudioInput::set_aec_stream_delay_ms(int ms)',
  '{',
  '    if (m_->echo_canceller && m_->echo_canceller->active()) {',
].join('\n');
const UNIQUE_CPP_PATCH = [
  'void AudioInput::set_aec_stream_delay_ms(int ms)',
  '{',
  `    // ${CPP_MARKER}`,
  '    if (m_->echo_canceller && m_->echo_canceller->active()) {',
].join('\n');
const EDIT_TOOLS = [
  '--enable-tools',
  'file_read,file_search,file_str_replace,file_delete,write_file',
];
const COMPOSE_TOOLS = [
  '--enable-tools',
  'file_read,file_compose',
];
const SUITE_NAMES = [
  'loopback', 'tools', 'headings', 'contract', 'edges', 'mass',
  'cpp', 'history', 'compose', 'search', 'live-cli', 'live-cpp', 'live-office', 'live-paper',
];
const SEARCH_TOOLS = [
  '--enable-tools',
  'file_read,search_index,search_query,search_detail',
];
const PAPER_TOOLS = [
  '--enable-tools',
  'info_lookup,run,download_url,file_read,file_search,search_index,search_query,search_detail,write_file,file_compose',
];
const RESEARCH_FIXTURE = join(PACKAGE_ROOT, 'tests/large-edits/markdown/edit-tools-research.md');
const PAPER_PROMPT = [
  'Chiwei Zhu, Benfeng Xu, Mingxuan Du, Shaohan Wang, Xiaorui Wang, Zhendong Mao, and Yongdong Zhang. FS-Researcher: Test-Time Scaling for Long-Horizon Research Tasks with File-System-Based Agents. ACL 2026 (long). arXiv:2602.01566. https://arxiv.org/abs/2602.01566 · ACL Anthology · code ',
  'capture essentials from their pdf as new markdown document - we have tools to convert pdfs to markdown and tools to download',
].join('\n');
const OFFICE_XLSX = 'sources/q4-sales.xlsx';
const OFFICE_DOCX = 'sources/q4-brief.docx';
const OFFICE_REPORT = 'q4-recap.md';
const OFFICE_SHEET = [
  ['A1', 'Region', true], ['B1', 'Product', true], ['C1', 'Units', true],
  ['D1', 'Price', true], ['E1', 'Revenue', true],
  ['A2', 'North'], ['B2', 'Widgets'], ['C2', '120'], ['D2', '120'], ['E2', '14400'],
  ['A3', 'South'], ['B3', 'Widgets'], ['C3', '80'], ['D3', '120'], ['E3', '9600'],
  ['A4', 'North'], ['B4', 'Gadgets'], ['C4', '40'], ['D4', '300'], ['E4', '12000'],
  ['A5', 'South'], ['B5', 'Gadgets'], ['C5', '55'], ['D5', '300'], ['E5', '16500'],
  ['A6', 'Total', true],
];
const OFFICE_BRIEF = [
  ['Q4 Operating Brief', ['style=Heading1', 'bold=true']],
  ['North region is the priority account for 2027. Hold the Gadgets list price at 300.', []],
  ['Renew the North Gadgets contract before 15 January 2027.', []],
  ['The board wants a one-page sales recap drawn from the workbook, not a formatting dump.', []],
];
const OFFICE_TOOLS = [
  '--enable-tools',
  'info_lookup,file_read,file_compose,file_search',
];
const EMPTY_SHA256 = 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855';
const MASS_SLICE_COUNT = 24;
const MASS_TOKEN_COUNT = 40;

const harness = createAgentHarnessOptions({
  harnessSubdir: 'agent-edit-harness',
  loopbackPresetId: LOOPBACK_PRESET,
  displayName: 'agent-edit',
});
// Live uses the user's installed default preset + config-dir. Only apply
// --preset when the caller passed it explicitly (never default to "quick").
if (!parseArgValue('--preset'))
  harness.preset = '';

function payloadFromEnv(env) {
  if (!env) return null;
  const file = Array.isArray(env.results) ? env.results[0] : null;
  return {
    ok: env.ok === true && (file ? file.ok !== false : true),
    error: env.error || file?.error || '',
    result: file?.result ?? env.result ?? env,
    file,
    env,
  };
}

function toolPayload(bundlePath, name) {
  return payloadFromEnv(toolResultEnvelope(bundlePath, name));
}

function toolPayloads(bundlePath, name) {
  const bundle = bundlePath ? readJson(bundlePath) : null;
  const events = bundle?.communication?.tools || [];
  const out = [];
  for (const ev of events) {
    if (ev.kind !== 'tool_result' || ev.tool !== name) continue;
    let d = ev.detail;
    if (typeof d === 'string') {
      try { d = JSON.parse(d); } catch { d = null; }
    }
    if (!d) continue;
    out.push(payloadFromEnv(d.envelope ?? d));
  }
  return out;
}

function countNeedle(text, needle) {
  if (!needle) return 0;
  let n = 0;
  for (let i = 0; ;) {
    const p = String(text).indexOf(needle, i);
    if (p < 0) return n;
    n += 1;
    i = p + needle.length;
  }
}

function copyCliFixture(workDir, name = 'cli.md') {
  if (!existsSync(CLI_FIXTURE)) {
    throw new Error(`missing live fixture ${CLI_FIXTURE}`);
  }
  const dest = join(workDir, name);
  cpSync(CLI_FIXTURE, dest);
  return dest;
}

function copyCppFixtures(workDir) {
  for (const src of [AUDIO_HPP_FIXTURE, AUDIO_CPP_FIXTURE]) {
    if (!existsSync(src)) {
      throw new Error(`missing cpp fixture ${src} (copy into tests/editor/, not stemp/)`);
    }
    cpSync(src, join(workDir, src.split(/[/\\]/).pop()));
  }
  return {
    hpp: join(workDir, 'audio.hpp'),
    cpp: join(workDir, 'audio.cpp'),
  };
}

function scriptTools(ctx, {
  sessionId, suite, index, name, calls, extraArgs = EDIT_TOOLS, include, timeout,
}) {
  return runAgentRound({
    ...ctx,
    sessionId,
    prompt: name,
    extraArgs: include ? [...extraArgs, '--include', include] : extraArgs,
    script: [{ tool_calls: calls }, { text: name }],
    timeout,
    roundKey: `${suite}-r${String(index).padStart(2, '0')}`,
    roundMeta: { suite, index, name },
  });
}

function usedRg(search) {
  const stats = search?.result?.stats;
  if (stats && typeof stats === 'object' && stats.rg_summary) return true;
  return JSON.stringify(search?.result || {}).includes('rg_summary');
}

function fatalErr(payload, round) {
  return String(payload?.error || round?.json?.error || '');
}

function asLf(text) {
  return String(text || '').replace(/\r\n/g, '\n');
}

function headingLines(text) {
  const lines = [];
  const parts = String(text || '').split(/\r?\n/);
  for (let i = 0; i < parts.length; i += 1) {
    if (parts[i] === HEADING) lines.push(i + 1);
  }
  return lines;
}

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
    script: [{ text: 'hello from edit harness' }],
    extraArgs: EDIT_TOOLS,
    roundKey: 'loopback-smoke',
    roundMeta: { suite: 'loopback-smoke', index: 1, name: 'immediate text' },
  });
  rep.step('immediate text', r.ms, `ok=${r.json?.ok} text=${r.json?.final_text}`);
  assert(r.status === 0, 'loopback-smoke: exits 0');
  assert(r.json?.ok === true, 'loopback-smoke: result.ok=true');
  assert(r.json?.final_text === 'hello from edit harness',
    `loopback-smoke: final_text matches script (got ${r.json?.final_text})`);

  rep.endSuite();
}

function suiteTools(rep, assert, ctx) {
  if (ctx.mode !== 'loopback') {
    rep.note('tools suite skipped in live mode');
    return;
  }
  console.log('\n-- edit tools (loopback) --\n');
  rep.beginSuite('Edit tools (loopback)');

  const sessionId = `${ctx.runId}-edit-tools`;
  const rel = 'notes.md';
  const abs = join(ctx.cwd, rel);
  const body = [
    '# Notes',
    '',
    '## Alpha',
    '',
    'alpha body',
    '',
    '## Beta',
    '',
    '**Example**',
    '',
    'beta one',
    '',
    '## Gamma',
    '',
    '**Example**',
    '',
    'gamma two',
    '',
  ].join('\n');

  const rWrite = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'write notes.md',
    extraArgs: EDIT_TOOLS,
    script: [
      {
        tool_calls: [{
          id: 'tc_wf',
          name: 'write_file',
          arguments: { path: rel, content: body },
        }],
      },
      { text: 'WROTE' },
    ],
    roundKey: 'tools-r01',
    roundMeta: { suite: 'tools', index: 1, name: 'write_file' },
  });
  const wroteTool = toolPayload(rWrite.bundlePath, 'write_file');
  rep.step('write_file', rWrite.ms, `ok=${rWrite.json?.ok} tool=${wroteTool?.ok}`);
  assert(rWrite.status === 0, `tools r1: exits 0 (status=${rWrite.status})`);
  assert(rWrite.json?.ok === true || wroteTool?.ok === true,
    `tools r1: write_file round ok (json.ok=${rWrite.json?.ok} tool.ok=${wroteTool?.ok})`);
  assert(toolCalls(rWrite.bundlePath).includes('write_file'), 'tools r1: called write_file');
  assert(existsSync(abs), 'tools r1: notes.md exists');

  const rRead = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'read notes.md window',
    extraArgs: EDIT_TOOLS,
    script: [
      {
        tool_calls: [{
          id: 'tc_rd',
          name: 'file_read',
          arguments: { path: rel, offset: 3, limit: 4 },
        }],
      },
      { text: 'READ' },
    ],
    roundKey: 'tools-r02',
    roundMeta: { suite: 'tools', index: 2, name: 'file_read window' },
  });
  const read = toolPayload(rRead.bundlePath, 'file_read');
  rep.step('file_read window', rRead.ms, `lines=${read?.result?.line_start}-${read?.result?.line_end}`);
  assert(rRead.status === 0, 'tools r2: exits 0');
  assert(toolCalls(rRead.bundlePath).includes('file_read'), 'tools r2: called file_read');
  assert(read?.ok === true, `tools r2: file_read ok (err=${read?.error})`);
  assert(read?.result?.line_start === 3, `tools r2: line_start=3 (got ${read?.result?.line_start})`);
  assert(read?.result?.line_end === 6, `tools r2: line_end=6 (got ${read?.result?.line_end})`);
  assert(read?.result?.total_lines >= 16, `tools r2: total_lines (got ${read?.result?.total_lines})`);
  assert(String(read?.result?.text || '').includes('## Alpha'), 'tools r2: window contains ## Alpha');
  assert(!String(read?.result?.text || '').includes('## Gamma'), 'tools r2: window excludes ## Gamma');

  const rBareSmall = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'read notes.md whole (small)',
    extraArgs: EDIT_TOOLS,
    script: [
      {
        tool_calls: [{
          id: 'tc_rd_all',
          name: 'file_read',
          arguments: { path: rel },
        }],
      },
      { text: 'READ_ALL' },
    ],
    roundKey: 'tools-r02b',
    roundMeta: { suite: 'tools', index: 2.5, name: 'file_read unwindowed small' },
  });
  const bareSmall = toolPayload(rBareSmall.bundlePath, 'file_read');
  rep.step('file_read unwindowed small', rBareSmall.ms,
    `ok=${bareSmall?.ok} lines=${bareSmall?.result?.total_lines}`);
  assert(bareSmall?.ok === true,
    `tools r2b: small unwindowed read should succeed (err=${bareSmall?.error})`);
  assert(bareSmall?.result?.truncated === false, 'tools r2b: not truncated');
  assert(String(bareSmall?.result?.text || '').includes('## Gamma'),
    'tools r2b: full file includes ## Gamma');

  const rAmbiguous = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'ambiguous replace',
    extraArgs: EDIT_TOOLS,
    script: [
      {
        tool_calls: [{
          id: 'tc_amb',
          name: 'file_str_replace',
          arguments: { path: rel, old_string: '**Example**', new_string: '**Changed**' },
        }],
      },
      { text: 'AMBIGUOUS' },
    ],
    roundKey: 'tools-r03',
    roundMeta: { suite: 'tools', index: 3, name: 'file_str_replace ambiguous' },
  });
  const amb = toolPayload(rAmbiguous.bundlePath, 'file_str_replace');
  const ambErr = String(amb?.error || rAmbiguous.json?.error || '');
  rep.step('file_str_replace ambiguous', rAmbiguous.ms, ambErr.slice(0, 80));
  assert(toolCalls(rAmbiguous.bundlePath).includes('file_str_replace'),
    'tools r3: called file_str_replace');
  assert(/matched 2 times/i.test(ambErr) || amb?.ok === false,
    `tools r3: ambiguous old_string fails (got ${ambErr || JSON.stringify(amb?.result)})`);
  assert(readFileSync(abs, 'utf8').includes('**Example**'),
    'tools r3: file unchanged after ambiguous replace');

  const rPatch = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'unique replace',
    extraArgs: EDIT_TOOLS,
    script: [
      {
        tool_calls: [{
          id: 'tc_rep',
          name: 'file_str_replace',
          arguments: {
            path: rel,
            old_string: '## Alpha\n\nalpha body\n',
            new_string: '## Alpha\n\nalpha body patched\n',
          },
        }],
      },
      { text: 'PATCHED' },
    ],
    roundKey: 'tools-r04',
    roundMeta: { suite: 'tools', index: 4, name: 'file_str_replace unique' },
  });
  const patch = toolPayload(rPatch.bundlePath, 'file_str_replace');
  rep.step('file_str_replace unique', rPatch.ms, `replacements=${patch?.result?.replacements}`);
  assert(rPatch.status === 0, 'tools r4: exits 0');
  assert(patch?.ok === true, `tools r4: replace ok (err=${patch?.error})`);
  assert(patch?.result?.replacements === 1, `tools r4: replacements=1 (got ${patch?.result?.replacements})`);
  assert(readFileSync(abs, 'utf8').includes('alpha body patched'),
    'tools r4: disk contains patched text');

  const rDel = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'delete notes.md',
    extraArgs: EDIT_TOOLS,
    script: [
      {
        tool_calls: [{
          id: 'tc_del',
          name: 'file_delete',
          arguments: { path: rel },
        }],
      },
      { text: 'DELETED' },
    ],
    roundKey: 'tools-r05',
    roundMeta: { suite: 'tools', index: 5, name: 'file_delete' },
  });
  const del = toolPayload(rDel.bundlePath, 'file_delete');
  const delErr = fatalErr(del, rDel);
  rep.step('file_delete', rDel.ms, `deleted=${del?.result?.deleted} err=${delErr.slice(0, 60)}`);
  assert(rDel.status === 0, 'tools r5: exits 0');
  if (/security policy/i.test(delErr)) {
    rep.note('file_delete blocked by security policy (not asserted)');
  } else {
    assert(del?.ok === true && del?.result?.deleted === true, `tools r5: file_delete ok (err=${delErr})`);
    assert(!existsSync(abs), 'tools r5: notes.md removed');
  }

  rep.endSuite();
}

function suiteHeadings(rep, assert, ctx) {
  if (ctx.mode !== 'loopback') {
    rep.note('headings suite skipped in live mode');
    return;
  }
  console.log('\n-- headings on tests/editor/cli.md (loopback) --\n');
  rep.beginSuite('Headings via file_search + windowed read (loopback)');

  const sessionId = `${ctx.runId}-edit-headings`;
  const dest = copyCliFixture(ctx.cwd);
  const rel = 'cli.md';
  const before = readFileSync(dest, 'utf8');
  const lines = headingLines(before);
  assert(lines.length >= 2,
    `headings fixture: expected >= 2 "${HEADING}" (got ${lines.join(', ')})`);
  const firstLine = lines[0];

  const rSearch = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'find Examples heading',
    extraArgs: [...EDIT_TOOLS, '--include', dest],
    script: [
      {
        tool_calls: [{
          id: 'tc_rg',
          name: 'file_search',
          arguments: {
            query: `^${HEADING}`,
            path: rel,
            regex: true,
            indexer: 'rg',
            head_limit: 20,
          },
        }],
      },
      { text: 'SEARCHED' },
    ],
    roundKey: 'headings-r01',
    roundMeta: { suite: 'headings', index: 1, name: 'file_search rg heading' },
  });
  const search = toolPayload(rSearch.bundlePath, 'file_search');
  const hits = search?.result?.hits || search?.result?.matches || [];
  const hitLines = hits.map((h) => h.line ?? h.line_number ?? h.lineNumber).filter(Boolean);
  rep.step('file_search rg', rSearch.ms, `hits=${hits.length} lines=${hitLines.join(',')}`);
  assert(rSearch.status === 0, 'headings r1: exits 0');
  assert(toolCalls(rSearch.bundlePath).includes('file_search'), 'headings r1: called file_search');
  assert(search?.ok === true, `headings r1: file_search ok (err=${search?.error})`);
  assert(usedRg(search),
    `headings r1: file_search used rg (stats=${truncateText(JSON.stringify(search?.result?.stats || {}), 160)})`);
  assert(hits.length >= 2 || hitLines.length >= 2 || JSON.stringify(search?.result || {}).includes(HEADING),
    `headings r1: rg found heading (payload=${truncateText(JSON.stringify(search?.result || {}), 200)})`);
  if (hitLines.length) {
    assert(hitLines.includes(firstLine),
      `headings r1: hits include fixture line ${firstLine} (got ${hitLines.join(',')})`);
  }

  const rRead = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'read heading window',
    extraArgs: [...EDIT_TOOLS, '--include', dest],
    script: [
      {
        tool_calls: [{
          id: 'tc_win',
          name: 'file_read',
          arguments: { path: rel, offset: firstLine, limit: 20 },
        }],
      },
      { text: 'WINDOW' },
    ],
    roundKey: 'headings-r02',
    roundMeta: { suite: 'headings', index: 2, name: 'file_read heading window' },
  });
  const read = toolPayload(rRead.bundlePath, 'file_read');
  rep.step('file_read window', rRead.ms,
    `start=${read?.result?.line_start} total=${read?.result?.total_lines} trunc=${read?.result?.truncated}`);
  assert(read?.ok === true, `headings r2: file_read ok (err=${read?.error})`);
  assert(read?.result?.line_start === firstLine,
    `headings r2: line_start=${firstLine} (got ${read?.result?.line_start})`);
  assert(read?.result?.total_lines > 400, `headings r2: large file total_lines (got ${read?.result?.total_lines})`);
  assert(String(read?.result?.text || '').includes(HEADING), 'headings r2: window includes heading');
  assert(String(read?.result?.text || '').includes(UNIQUE_NEIGHBOR),
    'headings r2: window includes unique neighbor line');

  const rBare = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'bare heading replace should fail',
    extraArgs: [...EDIT_TOOLS, '--include', dest],
    script: [
      {
        tool_calls: [{
          id: 'tc_bare',
          name: 'file_str_replace',
          arguments: { path: rel, old_string: `${HEADING}\n`, new_string: `${HEADING}\n\n${MARKER}\n` },
        }],
      },
      { text: 'BARE' },
    ],
    roundKey: 'headings-r03',
    roundMeta: { suite: 'headings', index: 3, name: 'ambiguous heading replace' },
  });
  const bare = toolPayload(rBare.bundlePath, 'file_str_replace');
  const bareErr = String(bare?.error || '');
  rep.step('ambiguous heading', rBare.ms, bareErr.slice(0, 100));
  assert(/matched \d+ times/i.test(bareErr) || bare?.ok === false,
    `headings r3: bare heading is ambiguous (got ${bareErr})`);

  const rPatch = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'unique heading replace',
    extraArgs: [...EDIT_TOOLS, '--include', dest],
    script: [
      {
        tool_calls: [{
          id: 'tc_ok',
          name: 'file_str_replace',
          arguments: {
            path: rel,
            old_string: UNIQUE_HEADING_BLOCK,
            new_string: UNIQUE_HEADING_PATCH,
          },
        }],
      },
      { text: 'EDIT_OK' },
    ],
    roundKey: 'headings-r04',
    roundMeta: { suite: 'headings', index: 4, name: 'unique heading replace' },
  });
  const patch = toolPayload(rPatch.bundlePath, 'file_str_replace');
  const after = readFileSync(dest, 'utf8');
  rep.step('unique replace', rPatch.ms, `replacements=${patch?.result?.replacements}`);
  assert(patch?.ok === true, `headings r4: replace ok (err=${patch?.error})`);
  assert(patch?.result?.replacements === 1, 'headings r4: one replacement');
  assert(after.includes(MARKER), 'headings r4: marker written');
  assert((after.match(new RegExp(MARKER, 'g')) || []).length === 1,
    'headings r4: marker appears once');
  assert(after.includes(SECOND_SECTION_GUARD), 'headings r4: second heading section intact');

  const rCap = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'read whole large file should fail until windowed',
    extraArgs: [...EDIT_TOOLS, '--include', dest],
    script: [
      {
        tool_calls: [{
          id: 'tc_cap',
          name: 'file_read',
          arguments: { path: rel },
        }],
      },
      { text: 'NEED_WINDOW' },
    ],
    roundKey: 'headings-r05',
    roundMeta: { suite: 'headings', index: 5, name: 'file_read unwindowed large fail' },
  });
  const cap = toolPayload(rCap.bundlePath, 'file_read');
  const capErr = fatalErr(cap, rCap);
  rep.step('file_read unwindowed large', rCap.ms, capErr.slice(0, 120));
  assert(/large file/i.test(capErr),
    `headings r5: expected large-file fatal (got ${capErr})`);
  assert(/offset\/limit/i.test(capErr),
    `headings r5: error tells agent to window (got ${capErr})`);

  rep.endSuite();
}

function suiteLiveCli(rep, assert, ctx) {
  if (ctx.mode !== 'live') {
    rep.note('live-cli skipped in loopback mode');
    return;
  }
  console.log('\n-- live edit against tests/editor/cli.md --\n');
  rep.beginSuite('Live edit (tests/editor/cli.md)');

  const dest = copyCliFixture(ctx.cwd);
  const sessionId = `${ctx.runId}-live-cli`;
  const prompt = [
    `The working folder contains cli.md (also selected). It is a command reference.`,
    `Do not rewrite the whole file.`,
    `1. Call file_search with indexer=rg, regex=true, query="^${HEADING}$", path=cli.md.`,
    `2. Call file_read with offset at the first heading hit and limit=40.`,
    `3. You MUST call file_str_replace. The exact heading appears more than once.`,
    `   Use this unique old_string (first section is followed by "${UNIQUE_NEIGHBOR}"):`,
    `   old_string is the first heading plus the blank line plus that neighbor sentence.`,
    `   new_string inserts a blank line and the exact marker ${MARKER} immediately after the heading.`,
    `   Do not reply until file_str_replace returns ok.`,
    `4. Only after a successful file_str_replace, reply exactly: EDIT_OK`,
    `Do not use write_file. Do not use run/sed.`,
  ].join(' ');

  const r = runAgentRound({
    ...ctx,
    sessionId,
    prompt,
    extraArgs: [...EDIT_TOOLS, '--include', dest],
    plannerEnabled: true,
    timeout: 240_000,
    roundKey: 'live-cli-r01',
    roundMeta: { suite: 'live-cli', index: 1, name: 'rg + window + replace' },
  });
  const calls = toolCalls(r.bundlePath);
  const after = existsSync(dest) ? readFileSync(dest, 'utf8') : '';
  const search = toolPayload(r.bundlePath, 'file_search');
  const read = toolPayload(r.bundlePath, 'file_read');
  const replace = toolPayload(r.bundlePath, 'file_str_replace');
  rep.step('live tools', r.ms, `calls=${calls.join(',')}`);
  if (r.status !== 0) {
    rep.note(`live-cli argv: tanit-cli ${r.args.join(' ')}`);
    rep.note(`live-cli stdout: ${truncateText(r.stdout, 400)}`);
    rep.note(`live-cli stderr: ${truncateText(r.stderr, 400)}`);
  }
  assert(r.status === 0, `live-cli: exits 0 (status=${r.status} stderr=${truncateText(r.stderr, 160)})`);
  assert(calls.includes('file_search'), `live-cli: called file_search (got ${calls.join(',')})`);
  assert(calls.includes('file_read') || calls.includes('file_str_replace'),
    `live-cli: called file_read or file_str_replace (got ${calls.join(',')})`);
  assert(!calls.includes('write_file'),
    `live-cli: must not rewrite via write_file (got ${calls.join(',')})`);
  if (search) {
    assert(search.ok === true, `live-cli: file_search ok (err=${search.error})`);
  }
  if (read) {
    assert(read.ok === true, `live-cli: file_read ok (err=${read.error})`);
    assert(read.result?.truncated !== undefined, 'live-cli: file_read returned window metadata');
  }
  assert(replace?.ok === true, `live-cli: file_str_replace ok (err=${replace?.error})`);
  assert(after.includes(MARKER), 'live-cli: marker written to work-dir copy of cli.md');
  assert((after.match(new RegExp(MARKER, 'g')) || []).length === 1,
    'live-cli: marker appears once (did not patch every heading)');
  assert(/EDIT_OK/i.test(finalText(r.bundlePath) || r.json?.final_text || ''),
    `live-cli: reply EDIT_OK (got ${truncateText(finalText(r.bundlePath))})`);

  rep.endSuite();
}

function suiteContract(rep, assert, ctx) {
  if (ctx.mode !== 'loopback') {
    rep.note('contract suite skipped in live mode');
    return;
  }
  console.log('\n-- edit tool contract (loopback) --\n');
  rep.beginSuite('Edit tool contract (loopback)');

  const sessionId = `${ctx.runId}-edit-contract`;
  const cwd = ctx.cwd;
  let i = 0;
  const next = (name, calls, extra = {}) => {
    i += 1;
    return scriptTools(ctx, { sessionId, suite: 'contract', index: i, name, calls, ...extra });
  };

  writeFileSync(join(cwd, 'rg.txt'), '# Title\nhello rg\n', 'utf8');
  const rRg = next('file_search indexer=rg', [{
    id: 'tc_rg',
    name: 'file_search',
    arguments: {
      query: '^# Title',
      path: 'rg.txt',
      regex: true,
      indexer: 'rg',
      head_limit: 8,
    },
  }]);
  const rg = toolPayload(rRg.bundlePath, 'file_search');
  rep.step('file_search rg', rRg.ms, `ok=${rg?.ok} usedRg=${usedRg(rg)}`);
  assert(rg?.ok === true, `contract rg: file_search ok (err=${rg?.error})`);
  assert(usedRg(rg),
    `contract rg: stats.rg_summary present (got ${truncateText(JSON.stringify(rg?.result?.stats || {}), 160)})`);

  writeFileSync(join(cwd, 'dup.txt'), 'aaa\nfoo\nbbb\nfoo\nccc\n', 'utf8');
  const rAll = next('replace_all', [{
    id: 'tc_all',
    name: 'file_str_replace',
    arguments: {
      path: 'dup.txt',
      old_string: 'foo',
      new_string: 'bar',
      replace_all: true,
    },
  }]);
  const all = toolPayload(rAll.bundlePath, 'file_str_replace');
  const afterAll = readFileSync(join(cwd, 'dup.txt'), 'utf8');
  rep.step('replace_all', rAll.ms, `replacements=${all?.result?.replacements}`);
  assert(all?.ok === true, `contract replace_all: ok (err=${all?.error})`);
  assert(all?.result?.replacements === 2, `contract replace_all: 2 hits (got ${all?.result?.replacements})`);
  assert(afterAll === 'aaa\nbar\nbbb\nbar\nccc\n', 'contract replace_all: both foos replaced');

  writeFileSync(join(cwd, 'win.txt'), 'aaa\nfoo\nbbb\nfoo\nccc\n', 'utf8');
  const rWin = next('start_line/end_line window', [{
    id: 'tc_win',
    name: 'file_str_replace',
    arguments: {
      path: 'win.txt',
      old_string: 'foo',
      new_string: 'bar',
      start_line: 4,
      end_line: 4,
    },
  }]);
  const win = toolPayload(rWin.bundlePath, 'file_str_replace');
  const afterWin = readFileSync(join(cwd, 'win.txt'), 'utf8');
  rep.step('line window', rWin.ms, `replacements=${win?.result?.replacements}`);
  assert(win?.ok === true, `contract window: ok (err=${win?.error})`);
  assert(win?.result?.replacements === 1, 'contract window: one replacement');
  assert(afterWin === 'aaa\nfoo\nbbb\nbar\nccc\n', 'contract window: only line 4 changed');

  writeFileSync(join(cwd, 'crlf.txt'), 'one\r\nfoo\r\ntwo\r\n', 'utf8');
  const rCrlf = next('CRLF preserved', [{
    id: 'tc_crlf',
    name: 'file_str_replace',
    arguments: { path: 'crlf.txt', old_string: 'foo', new_string: 'bar' },
  }]);
  const crlf = toolPayload(rCrlf.bundlePath, 'file_str_replace');
  const afterCrlf = readFileSync(join(cwd, 'crlf.txt'), 'utf8');
  rep.step('CRLF', rCrlf.ms, `ok=${crlf?.ok} crlf=${afterCrlf.includes('\r\n')}`);
  assert(crlf?.ok === true, `contract crlf: ok (err=${crlf?.error})`);
  assert(afterCrlf === 'one\r\nbar\r\ntwo\r\n', 'contract crlf: CRLF kept after replace');

  const rEmpty = next('empty old_string', [{
    id: 'tc_empty',
    name: 'file_str_replace',
    arguments: { path: 'win.txt', old_string: '', new_string: 'x' },
  }]);
  const empty = toolPayload(rEmpty.bundlePath, 'file_str_replace');
  const emptyErr = fatalErr(empty, rEmpty);
  rep.step('empty old_string', rEmpty.ms, emptyErr.slice(0, 80));
  assert(/old_string is empty/i.test(emptyErr),
    `contract empty: expected empty-string fatal (got ${emptyErr})`);

  const rIdent = next('identical strings', [{
    id: 'tc_ident',
    name: 'file_str_replace',
    arguments: { path: 'win.txt', old_string: 'aaa', new_string: 'aaa' },
  }]);
  const ident = toolPayload(rIdent.bundlePath, 'file_str_replace');
  const identErr = fatalErr(ident, rIdent);
  rep.step('identical strings', rIdent.ms, identErr.slice(0, 80));
  assert(/identical/i.test(identErr),
    `contract identical: expected identical fatal (got ${identErr})`);

  const rHalf = next('start_line without end_line', [{
    id: 'tc_half',
    name: 'file_str_replace',
    arguments: { path: 'win.txt', old_string: 'aaa', new_string: 'zzz', start_line: 1 },
  }]);
  const half = toolPayload(rHalf.bundlePath, 'file_str_replace');
  const halfErr = fatalErr(half, rHalf);
  rep.step('start_line alone', rHalf.ms, halfErr.slice(0, 80));
  assert(/must be passed together/i.test(halfErr),
    `contract half-window: expected pair fatal (got ${halfErr})`);

  writeFileSync(join(cwd, 'short.txt'), 'a\nb\nc\n', 'utf8');
  const rPast = next('offset past EOF', [{
    id: 'tc_past',
    name: 'file_read',
    arguments: { path: 'short.txt', offset: 99, limit: 4 },
  }]);
  const past = toolPayload(rPast.bundlePath, 'file_read');
  const pastErr = fatalErr(past, rPast);
  rep.step('offset past EOF', rPast.ms, pastErr.slice(0, 80));
  assert(/offset 99 past end/i.test(pastErr),
    `contract offset: expected past-end fatal (got ${pastErr})`);

  const longLines = Array.from({ length: 500 }, (_, n) => `line-${n + 1}`).join('\n') + '\n';
  writeFileSync(join(cwd, 'long.txt'), longLines, 'utf8');
  const rCap = next('limit 400 clamp', [{
    id: 'tc_cap',
    name: 'file_read',
    arguments: { path: 'long.txt', offset: 1, limit: 400 },
  }]);
  const cap = toolPayload(rCap.bundlePath, 'file_read');
  rep.step('limit 400', rCap.ms, `end=${cap?.result?.line_end} trunc=${cap?.result?.truncated}`);
  assert(cap?.ok === true, `contract cap: file_read ok (err=${cap?.error})`);
  assert(cap?.result?.total_lines === 500, `contract cap: total_lines=500 (got ${cap?.result?.total_lines})`);
  assert(cap?.result?.line_end === 400, `contract cap: line_end=400 (got ${cap?.result?.line_end})`);
  assert(cap?.result?.truncated === true, 'contract cap: truncated=true');

  const rBare = next('unwindowed large fail', [{
    id: 'tc_bare',
    name: 'file_read',
    arguments: { path: 'long.txt' },
  }]);
  const bare = toolPayload(rBare.bundlePath, 'file_read');
  const bareErr = fatalErr(bare, rBare);
  rep.step('unwindowed large', rBare.ms, bareErr.slice(0, 120));
  assert(/large file \(500 lines\)/i.test(bareErr),
    `contract bare: expected 500-line fatal (got ${bareErr})`);
  assert(/file_search/i.test(bareErr) && /offset\/limit/i.test(bareErr),
    `contract bare: hint should mention file_search + offset/limit (got ${bareErr})`);

  const rSmall = next('unwindowed small ok', [{
    id: 'tc_small',
    name: 'file_read',
    arguments: { path: 'short.txt' },
  }]);
  const small = toolPayload(rSmall.bundlePath, 'file_read');
  rep.step('unwindowed small', rSmall.ms,
    `ok=${small?.ok} lines=${small?.result?.total_lines}`);
  assert(small?.ok === true, `contract small: unwindowed short.txt should succeed (err=${small?.error})`);
  assert(small?.result?.total_lines === 3, `contract small: total_lines=3 (got ${small?.result?.total_lines})`);
  assert(small?.result?.truncated === false, 'contract small: not truncated');

  const fat = `${'x'.repeat(129 * 1024)}\n`;
  writeFileSync(join(cwd, 'fat.txt'), fat, 'utf8');
  const rFat = next('unwindowed fat fail', [{
    id: 'tc_fat',
    name: 'file_read',
    arguments: { path: 'fat.txt' },
  }]);
  const fatPay = toolPayload(rFat.bundlePath, 'file_read');
  const fatErr = fatalErr(fatPay, rFat);
  rep.step('unwindowed fat', rFat.ms, fatErr.slice(0, 120));
  assert(/large file/i.test(fatErr),
    `contract fat: expected large-file fatal for 129 KiB (got ${fatErr})`);

  const rFatWin = next('windowed fat ok', [{
    id: 'tc_fatw',
    name: 'file_read',
    arguments: { path: 'fat.txt', offset: 1, limit: 1 },
  }]);
  const fatWin = toolPayload(rFatWin.bundlePath, 'file_read');
  rep.step('windowed fat', rFatWin.ms, `ok=${fatWin?.ok} bytes=${fatWin?.result?.bytes}`);
  assert(fatWin?.ok === true, `contract fat window: should succeed (err=${fatWin?.error})`);

  rep.endSuite();
}

function suiteEdges(rep, assert, ctx) {
  if (ctx.mode !== 'loopback') {
    rep.note('edges suite skipped in live mode');
    return;
  }
  console.log('\n-- edit tool edges (loopback) --\n');
  rep.beginSuite('Edit tool edges (loopback)');

  const sessionId = `${ctx.runId}-edit-edges`;
  const cwd = ctx.cwd;
  let i = 0;
  const next = (name, calls) => {
    i += 1;
    return scriptTools(ctx, { sessionId, suite: 'edges', index: i, name, calls });
  };

  const rMiss = next('missing path', [{
    id: 'tc_miss',
    name: 'file_str_replace',
    arguments: { path: 'no-such-file.txt', old_string: 'a', new_string: 'b' },
  }]);
  const missErr = fatalErr(toolPayload(rMiss.bundlePath, 'file_str_replace'), rMiss);
  rep.step('missing path', rMiss.ms, missErr.slice(0, 80));
  assert(/path does not exist/i.test(missErr),
    `edges missing: expected path-does-not-exist (got ${missErr})`);

  mkdirSync(join(cwd, 'adir'), { recursive: true });
  const rDir = next('directory not a file', [{
    id: 'tc_dir',
    name: 'file_str_replace',
    arguments: { path: 'adir', old_string: 'a', new_string: 'b' },
  }]);
  const dirErr = fatalErr(toolPayload(rDir.bundlePath, 'file_str_replace'), rDir);
  rep.step('directory', rDir.ms, dirErr.slice(0, 80));
  assert(/not a regular file/i.test(dirErr),
    `edges dir: expected not-a-regular-file (got ${dirErr})`);

  writeFileSync(join(cwd, 'x.png'), Buffer.from([0x89, 0x50, 0x4e, 0x47]));
  const rPng = next('png extension refuse', [{
    id: 'tc_png',
    name: 'file_str_replace',
    arguments: { path: 'x.png', old_string: 'PNG', new_string: 'png' },
  }]);
  const pngErr = fatalErr(toolPayload(rPng.bundlePath, 'file_str_replace'), rPng);
  rep.step('png refuse', rPng.ms, pngErr.slice(0, 80));
  assert(/refusing binary or image extension/i.test(pngErr),
    `edges png: expected image-extension refuse (got ${pngErr})`);

  writeFileSync(join(cwd, 'tokens.md'), 'hello\n', 'utf8');
  const rTok = next('secret-like basename', [{
    id: 'tc_tok',
    name: 'file_str_replace',
    arguments: { path: 'tokens.md', old_string: 'hello', new_string: 'hallo' },
  }]);
  const tokErr = fatalErr(toolPayload(rTok.bundlePath, 'file_str_replace'), rTok);
  rep.step('secret basename', rTok.ms, tokErr.slice(0, 100));
  assert(/security policy/i.test(tokErr),
    `edges secret-name: *token* basename should be denied (got ${tokErr})`);
  assert(readFileSync(join(cwd, 'tokens.md'), 'utf8') === 'hello\n',
    'edges secret-name: file unchanged');

  writeFileSync(join(cwd, 'nul.txt'), Buffer.from('hello\0world'));
  const rBin = next('nul-byte content refuse', [{
    id: 'tc_bin',
    name: 'file_str_replace',
    arguments: { path: 'nul.txt', old_string: 'hello', new_string: 'hallo' },
  }]);
  const binErr = fatalErr(toolPayload(rBin.bundlePath, 'file_str_replace'), rBin);
  rep.step('binary content', rBin.ms, binErr.slice(0, 80));
  assert(/refusing binary content/i.test(binErr),
    `edges binary: expected binary-content refuse (got ${binErr})`);

  writeFileSync(join(cwd, 'winmiss.txt'), 'aaa\nfoo\nbbb\nccc\n', 'utf8');
  const rWinMiss = next('window excludes match', [{
    id: 'tc_wmiss',
    name: 'file_str_replace',
    arguments: {
      path: 'winmiss.txt',
      old_string: 'foo',
      new_string: 'bar',
      start_line: 4,
      end_line: 4,
    },
  }]);
  const winMissErr = fatalErr(toolPayload(rWinMiss.bundlePath, 'file_str_replace'), rWinMiss);
  rep.step('window miss', rWinMiss.ms, winMissErr.slice(0, 80));
  assert(/old_string not found/i.test(winMissErr),
    `edges window-miss: expected not-found (got ${winMissErr})`);
  assert(readFileSync(join(cwd, 'winmiss.txt'), 'utf8') === 'aaa\nfoo\nbbb\nccc\n',
    'edges window-miss: file unchanged');

  const rWinPast = next('window past EOF', [{
    id: 'tc_wpast',
    name: 'file_str_replace',
    arguments: {
      path: 'winmiss.txt',
      old_string: 'foo',
      new_string: 'bar',
      start_line: 20,
      end_line: 20,
    },
  }]);
  const winPastErr = fatalErr(toolPayload(rWinPast.bundlePath, 'file_str_replace'), rWinPast);
  rep.step('window past EOF', rWinPast.ms, winPastErr.slice(0, 90));
  assert(/past end/i.test(winPastErr),
    `edges window-past: expected past-end fatal (got ${winPastErr})`);

  writeFileSync(join(cwd, 'ambig.txt'), 'aaa\nfoo\nbbb\nfoo\nccc\n', 'utf8');
  const rAmbig = next('window still ambiguous', [{
    id: 'tc_ambig',
    name: 'file_str_replace',
    arguments: {
      path: 'ambig.txt',
      old_string: 'foo',
      new_string: 'bar',
      start_line: 2,
      end_line: 4,
    },
  }]);
  const ambigErr = fatalErr(toolPayload(rAmbig.bundlePath, 'file_str_replace'), rAmbig);
  rep.step('window ambiguous', rAmbig.ms, ambigErr.slice(0, 100));
  assert(/matched 2 times/i.test(ambigErr) && /replace_all/i.test(ambigErr),
    `edges ambig-window: expected 2-hit fatal (got ${ambigErr})`);

  writeFileSync(join(cwd, 'overlap.txt'), 'aaaa\n', 'utf8');
  const rOver = next('replace_all non-overlap aa', [{
    id: 'tc_over',
    name: 'file_str_replace',
    arguments: {
      path: 'overlap.txt',
      old_string: 'aa',
      new_string: 'bb',
      replace_all: true,
    },
  }]);
  const over = toolPayload(rOver.bundlePath, 'file_str_replace');
  const afterOver = readFileSync(join(cwd, 'overlap.txt'), 'utf8');
  rep.step('non-overlap replace_all', rOver.ms, `replacements=${over?.result?.replacements}`);
  assert(over?.ok === true, `edges overlap: ok (err=${over?.error})`);
  assert(over?.result?.replacements === 2, `edges overlap: 2 hits (got ${over?.result?.replacements})`);
  assert(afterOver === 'bbbb\n', `edges overlap: aaaa → bbbb (got ${JSON.stringify(afterOver)})`);

  writeFileSync(join(cwd, 'unicode.txt'), 'café — KEEP\n', 'utf8');
  const rUni = next('preserve surrounding unicode', [{
    id: 'tc_uni',
    name: 'file_str_replace',
    arguments: { path: 'unicode.txt', old_string: 'KEEP', new_string: 'KEPT' },
  }]);
  const uni = toolPayload(rUni.bundlePath, 'file_str_replace');
  const afterUni = readFileSync(join(cwd, 'unicode.txt'), 'utf8');
  rep.step('unicode preserved', rUni.ms, `ok=${uni?.ok}`);
  assert(uni?.ok === true, `edges unicode: ok (err=${uni?.error})`);
  assert(afterUni === 'café — KEPT\n',
    `edges unicode: surrounding café preserved (got ${JSON.stringify(afterUni)})`);

  writeFileSync(join(cwd, 'noneol.txt'), 'hello foo', 'utf8');
  const rEol = next('no trailing newline', [{
    id: 'tc_eol',
    name: 'file_str_replace',
    arguments: { path: 'noneol.txt', old_string: 'foo', new_string: 'bar' },
  }]);
  const eol = toolPayload(rEol.bundlePath, 'file_str_replace');
  const afterEol = readFileSync(join(cwd, 'noneol.txt'), 'utf8');
  rep.step('no trailing newline', rEol.ms, `ok=${eol?.ok}`);
  assert(eol?.ok === true, `edges noneol: ok (err=${eol?.error})`);
  assert(afterEol === 'hello bar', `edges noneol: kept no-EOL (got ${JSON.stringify(afterEol)})`);

  writeFileSync(join(cwd, 'curver.txt'), 'alpha\nbeta\n', 'utf8');
  const rVerRead = next('read current version', [{
    id: 'tc_vrd',
    name: 'file_read',
    arguments: { path: 'curver.txt' },
  }]);
  const ver = String(toolPayload(rVerRead.bundlePath, 'file_read')?.result?.version || '');
  assert(ver.startsWith('sha256:'), `edges version: file_read version (got ${ver})`);
  const rVerOk = next('current expected_version', [{
    id: 'tc_vok',
    name: 'file_str_replace',
    arguments: {
      path: 'curver.txt',
      old_string: 'beta\n',
      new_string: 'beta patched\n',
      expected_version: ver,
    },
  }]);
  const verOk = toolPayload(rVerOk.bundlePath, 'file_str_replace');
  rep.step('current version', rVerOk.ms, `ok=${verOk?.ok} next=${verOk?.result?.version}`);
  assert(verOk?.ok === true, `edges version: current expected_version should succeed (err=${verOk?.error})`);
  assert(readFileSync(join(cwd, 'curver.txt'), 'utf8') === 'alpha\nbeta patched\n',
    'edges version: unique replace applied');

  const prior = [
    '- [x] **S0** done zero',
    '- [x] **S1** done one',
    '- [x] **S2** done two',
    '- [x] **S3** done three',
    '',
  ].join('\n');
  const s4Line = '- [ ] **S4** (in progress) only this line is unique';
  writeFileSync(join(cwd, 'expand.md'), `${prior}${s4Line}\n`, 'utf8');
  const hugeNew = `- [x] **S4** (in progress) only this line is unique\n${prior}`;
  const rExp = next('short old + huge new accepted', [{
    id: 'tc_exp',
    name: 'file_str_replace',
    arguments: { path: 'expand.md', old_string: s4Line, new_string: hugeNew },
  }]);
  const exp = toolPayload(rExp.bundlePath, 'file_str_replace');
  const afterExp = readFileSync(join(cwd, 'expand.md'), 'utf8');
  rep.step('expansion accepted', rExp.ms,
    `ok=${exp?.ok} s0=${countNeedle(afterExp, '**S0**')} bytes=${afterExp.length}`);
  assert(exp?.ok === true,
    `edges expand: unique short old_string is applied even when new_string is huge (err=${exp?.error})`);
  assert(countNeedle(afterExp, '**S0**') === 2,
    'edges expand: tool is precise not smart — duplicated S0 is accepted');
  assert(afterExp.includes('- [x] **S4**'), 'edges expand: S4 flipped');

  rep.endSuite();
}

function suiteMass(rep, assert, ctx) {
  if (ctx.mode !== 'loopback') {
    rep.note('mass suite skipped in live mode');
    return;
  }
  console.log('\n-- mass edits (loopback) --\n');
  rep.beginSuite('Mass edits (loopback)');
  rep.note('Mass path is sequential unique replaces with version chaining. '
    + 'Same-round batch on one file is not a supported API '
    + '(parallel dispatch can last-write-wins; shared expected_version cannot apply cleanly). '
    + 'replace_all is the only multi-hit tool.');

  const sessionId = `${ctx.runId}-edit-mass`;
  const cwd = ctx.cwd;
  let i = 0;
  const next = (name, calls) => {
    i += 1;
    return scriptTools(ctx, { sessionId, suite: 'mass', index: i, name, calls });
  };

  const todos = Array.from({ length: MASS_SLICE_COUNT }, (_, n) =>
    `- [ ] **S${n + 1}** item ${n + 1}`).join('\n') + '\n';
  writeFileSync(join(cwd, 'todos.md'), todos, 'utf8');
  const rTodoRead = next('read todos version', [{
    id: 'tc_mtd',
    name: 'file_read',
    arguments: { path: 'todos.md' },
  }]);
  let ver = String(toolPayload(rTodoRead.bundlePath, 'file_read')?.result?.version || '');
  assert(ver.startsWith('sha256:'), `mass sequential: initial version (got ${ver})`);

  for (let n = 1; n <= MASS_SLICE_COUNT; n += 1) {
    const rFlip = next(`flip S${n}`, [{
      id: `tc_ms${n}`,
      name: 'file_str_replace',
      arguments: {
        path: 'todos.md',
        old_string: `- [ ] **S${n}**`,
        new_string: `- [x] **S${n}**`,
        expected_version: ver,
      },
    }]);
    const flip = toolPayload(rFlip.bundlePath, 'file_str_replace');
    const nextVer = String(flip?.result?.version || '');
    rep.step(`sequential S${n}`, rFlip.ms, `ok=${flip?.ok} ver=${nextVer.slice(0, 18)}`);
    assert(flip?.ok === true, `mass sequential S${n}: ok (err=${flip?.error})`);
    assert(nextVer.startsWith('sha256:') && nextVer !== ver,
      `mass sequential S${n}: version advanced`);
    ver = nextVer;
  }

  const afterTodos = readFileSync(join(cwd, 'todos.md'), 'utf8');
  const openLeft = countNeedle(afterTodos, '- [ ] **S');
  const doneHits = countNeedle(afterTodos, '- [x] **S');
  const s1Hits = countNeedle(afterTodos, '**S1**');
  const lineCount = afterTodos.split('\n').filter((line) => line.length > 0).length;
  rep.step('sequential result', 0,
    `done=${doneHits} open=${openLeft} s1=${s1Hits} lines=${lineCount}`);
  assert(doneHits === MASS_SLICE_COUNT, `mass sequential: ${MASS_SLICE_COUNT} done (got ${doneHits})`);
  assert(openLeft === 0, `mass sequential: no open checkboxes left (got ${openLeft})`);
  assert(s1Hits === 1, `mass sequential: S1 not duplicated (got ${s1Hits})`);
  assert(lineCount === MASS_SLICE_COUNT,
    `mass sequential: line count unchanged (got ${lineCount})`);

  const marks = Array.from({ length: MASS_TOKEN_COUNT }, (_, n) =>
    `MARK line ${n + 1}`).join('\n') + '\n';
  writeFileSync(join(cwd, 'marks.md'), marks, 'utf8');
  const rAll = next('replace_all identical marks', [{
    id: 'tc_mall',
    name: 'file_str_replace',
    arguments: {
      path: 'marks.md',
      old_string: 'MARK',
      new_string: 'DONE',
      replace_all: true,
    },
  }]);
  const all = toolPayload(rAll.bundlePath, 'file_str_replace');
  const afterMarks = readFileSync(join(cwd, 'marks.md'), 'utf8');
  rep.step('replace_all mass', rAll.ms, `replacements=${all?.result?.replacements}`);
  assert(all?.ok === true, `mass replace_all: ok (err=${all?.error})`);
  assert(all?.result?.replacements === MASS_TOKEN_COUNT,
    `mass replace_all: ${MASS_TOKEN_COUNT} hits (got ${all?.result?.replacements})`);
  assert(countNeedle(afterMarks, 'MARK') === 0, 'mass replace_all: no MARK left');
  assert(countNeedle(afterMarks, 'DONE') === MASS_TOKEN_COUNT,
    `mass replace_all: ${MASS_TOKEN_COUNT} DONE (got ${countNeedle(afterMarks, 'DONE')})`);

  writeFileSync(join(cwd, 'batch-ver.md'), 'AAA unique-a\nBBB unique-b\n', 'utf8');
  const rBatchRead = next('read batch-ver version', [{
    id: 'tc_bvr',
    name: 'file_read',
    arguments: { path: 'batch-ver.md' },
  }]);
  const batchVer = String(toolPayload(rBatchRead.bundlePath, 'file_read')?.result?.version || '');
  const rBatchVer = next('same-round shared expected_version', [
    {
      id: 'tc_bv1',
      name: 'file_str_replace',
      arguments: {
        path: 'batch-ver.md',
        old_string: 'AAA',
        new_string: 'AXX',
        expected_version: batchVer,
      },
    },
    {
      id: 'tc_bv2',
      name: 'file_str_replace',
      arguments: {
        path: 'batch-ver.md',
        old_string: 'BBB',
        new_string: 'BXX',
        expected_version: batchVer,
      },
    },
  ]);
  const batchPays = toolPayloads(rBatchVer.bundlePath, 'file_str_replace');
  const afterBatchVer = readFileSync(join(cwd, 'batch-ver.md'), 'utf8');
  const okCount = batchPays.filter((p) => p?.ok).length;
  const bothLanded = afterBatchVer.includes('AXX') && afterBatchVer.includes('BXX');
  rep.step('batch shared version', rBatchVer.ms,
    `results=${batchPays.length} ok=${okCount} bothLanded=${bothLanded} file=${JSON.stringify(afterBatchVer)}`);
  assert(batchPays.length === 2, `mass batch-ver: 2 tool results (got ${batchPays.length})`);
  assert(!(okCount === 2 && bothLanded),
    'mass batch-ver: shared expected_version same-round on one file must not apply cleanly '
    + '(serial → second stale; parallel → last-write-wins)');

  writeFileSync(join(cwd, 'batch-race.md'), 'CCC unique-c\nDDD unique-d\n', 'utf8');
  const rBatchRace = next('same-round no version', [
    {
      id: 'tc_br1',
      name: 'file_str_replace',
      arguments: { path: 'batch-race.md', old_string: 'CCC', new_string: 'CXX' },
    },
    {
      id: 'tc_br2',
      name: 'file_str_replace',
      arguments: { path: 'batch-race.md', old_string: 'DDD', new_string: 'DXX' },
    },
  ]);
  const racePays = toolPayloads(rBatchRace.bundlePath, 'file_str_replace');
  const afterRace = readFileSync(join(cwd, 'batch-race.md'), 'utf8');
  const landedC = afterRace.includes('CXX');
  const landedD = afterRace.includes('DXX');
  const raceOk = racePays.filter((p) => p?.ok).length;
  if (landedC && landedD) {
    rep.note('same-round no-version applied both (serial dispatch this run)');
  } else {
    rep.note('same-round no-version lost an edit (parallel last-write-wins)');
  }
  rep.step('batch no version', rBatchRace.ms,
    `ok=${raceOk} C=${landedC} D=${landedD} file=${JSON.stringify(afterRace)}`);
  assert(racePays.length === 2, `mass batch-race: 2 tool results (got ${racePays.length})`);
  assert(landedC || landedD, 'mass batch-race: at least one independent edit landed');

  writeFileSync(join(cwd, 'two-files-a.md'), 'SRC-A\n', 'utf8');
  writeFileSync(join(cwd, 'two-files-b.md'), 'SRC-B\n', 'utf8');
  const rTwoFiles = next('same-round two files', [
    {
      id: 'tc_tf1',
      name: 'file_str_replace',
      arguments: { path: 'two-files-a.md', old_string: 'SRC-A', new_string: 'DST-A' },
    },
    {
      id: 'tc_tf2',
      name: 'file_str_replace',
      arguments: { path: 'two-files-b.md', old_string: 'SRC-B', new_string: 'DST-B' },
    },
  ]);
  const twoPays = toolPayloads(rTwoFiles.bundlePath, 'file_str_replace');
  const aTxt = readFileSync(join(cwd, 'two-files-a.md'), 'utf8');
  const bTxt = readFileSync(join(cwd, 'two-files-b.md'), 'utf8');
  rep.step('batch two files', rTwoFiles.ms,
    `ok=${twoPays.filter((p) => p?.ok).length} a=${JSON.stringify(aTxt)} b=${JSON.stringify(bTxt)}`);
  assert(twoPays.length === 2 && twoPays.every((p) => p?.ok),
    `mass two-files: both replaces ok (errs=${twoPays.map((p) => p?.error).join(' | ')})`);
  assert(aTxt === 'DST-A\n' && bTxt === 'DST-B\n',
    'mass two-files: independent files can be edited in one round');

  rep.endSuite();
}

function suiteCpp(rep, assert, ctx) {
  if (ctx.mode !== 'loopback') {
    rep.note('cpp suite skipped in live mode');
    return;
  }
  console.log('\n-- C++ fixtures audio.hpp / audio.cpp (loopback) --\n');
  rep.beginSuite('C++ fixtures (loopback)');

  const files = copyCppFixtures(ctx.cwd);
  const sessionId = `${ctx.runId}-edit-cpp`;
  const hppBefore = readFileSync(files.hpp, 'utf8');
  const stopHits = (hppBefore.match(/void stop\(\);/g) || []).length;
  assert(stopHits >= 3, `cpp fixture: expected >= 3 "void stop();" (got ${stopHits})`);
  assert(hppBefore.includes('class AudioInput'), 'cpp fixture: class AudioInput present');
  assert(asLf(readFileSync(files.cpp, 'utf8')).includes(UNIQUE_CPP_BLOCK),
    'cpp fixture: unique AudioInput::set_aec_stream_delay_ms block present');
  assert(asLf(hppBefore).includes(UNIQUE_HPP_STOP_BLOCK),
    'cpp fixture: unique AudioInput stop() neighbor present');
  assert(asLf(readFileSync(files.cpp, 'utf8')).includes(UNIQUE_RING_BLOCK),
    'cpp fixture: unique AudioRingBuffer::push block present');

  const rClass = scriptTools(ctx, {
    sessionId,
    suite: 'cpp',
    index: 1,
    name: 'rg class AudioInput',
    include: files.hpp,
    calls: [{
      id: 'tc_cls',
      name: 'file_search',
      arguments: {
        query: '^class AudioInput',
        path: 'audio.hpp',
        regex: true,
        indexer: 'rg',
        head_limit: 8,
      },
    }],
  });
  const cls = toolPayload(rClass.bundlePath, 'file_search');
  const clsHits = cls?.result?.hits || [];
  const clsLines = clsHits.map((h) => h.line).filter(Boolean);
  rep.step('rg class AudioInput', rClass.ms, `hits=${clsHits.length} lines=${clsLines.join(',')}`);
  assert(cls?.ok === true, `cpp r1: file_search ok (err=${cls?.error})`);
  assert(usedRg(cls), 'cpp r1: used rg');
  assert(clsHits.length >= 1 || JSON.stringify(cls?.result || {}).includes('AudioInput'),
    `cpp r1: found class AudioInput (payload=${truncateText(JSON.stringify(cls?.result || {}), 200)})`);

  const rStart = scriptTools(ctx, {
    sessionId,
    suite: 'cpp',
    index: 2,
    name: 'rg AudioInput::start',
    include: files.cpp,
    calls: [{
      id: 'tc_start',
      name: 'file_search',
      arguments: {
        query: '^void AudioInput::start',
        path: 'audio.cpp',
        regex: true,
        indexer: 'rg',
        head_limit: 8,
      },
    }],
  });
  const start = toolPayload(rStart.bundlePath, 'file_search');
  const startHits = start?.result?.hits || [];
  const startLines = startHits.map((h) => h.line).filter(Boolean);
  rep.step('rg AudioInput::start', rStart.ms, `hits=${startHits.length} lines=${startLines.join(',')}`);
  assert(start?.ok === true, `cpp r2: file_search ok (err=${start?.error})`);
  assert(usedRg(start), 'cpp r2: used rg');
  assert(startHits.length >= 1 || startLines.length >= 1,
    `cpp r2: found AudioInput::start (payload=${truncateText(JSON.stringify(start?.result || {}), 200)})`);

  const rRead = scriptTools(ctx, {
    sessionId,
    suite: 'cpp',
    index: 3,
    name: 'windowed read of unique fn',
    include: files.cpp,
    calls: [{
      id: 'tc_rd',
      name: 'file_read',
      arguments: { path: 'audio.cpp', offset: 220, limit: 20 },
    }],
  });
  const read = toolPayload(rRead.bundlePath, 'file_read');
  const winText = String(read?.result?.text || '');
  rep.step('file_read window', rRead.ms,
    `start=${read?.result?.line_start} total=${read?.result?.total_lines}`);
  assert(read?.ok === true, `cpp r3: file_read ok (err=${read?.error})`);
  assert(read?.result?.line_start === 220, `cpp r3: line_start=220 (got ${read?.result?.line_start})`);
  assert(read?.result?.total_lines > 400, `cpp r3: large cpp total_lines (got ${read?.result?.total_lines})`);
  assert(winText.includes('void AudioInput::set_aec_stream_delay_ms'),
    'cpp r3: window includes unique method');

  const rAmb = scriptTools(ctx, {
    sessionId,
    suite: 'cpp',
    index: 4,
    name: 'ambiguous void stop();',
    include: files.hpp,
    calls: [{
      id: 'tc_amb',
      name: 'file_str_replace',
      arguments: {
        path: 'audio.hpp',
        old_string: '    void stop();\n',
        new_string: '    void stop_capture();\n',
      },
    }],
  });
  const amb = toolPayload(rAmb.bundlePath, 'file_str_replace');
  const ambErr = fatalErr(amb, rAmb);
  rep.step('ambiguous stop()', rAmb.ms, ambErr.slice(0, 100));
  assert(/matched 3 times/i.test(ambErr) || amb?.ok === false,
    `cpp r4: void stop(); is ambiguous (got ${ambErr})`);
  assert((readFileSync(files.hpp, 'utf8').match(/void stop\(\);/g) || []).length === stopHits,
    'cpp r4: header unchanged after ambiguous replace');

  const rWinStop = scriptTools(ctx, {
    sessionId,
    suite: 'cpp',
    index: 5,
    name: 'line-window second stop()',
    include: files.hpp,
    calls: [{
      id: 'tc_winstop',
      name: 'file_str_replace',
      arguments: {
        path: 'audio.hpp',
        old_string: '    void stop();\n',
        new_string: `    void stop(); // ${HPP_WIN_MARKER}\n`,
        start_line: 146,
        end_line: 146,
      },
    }],
  });
  const winStop = toolPayload(rWinStop.bundlePath, 'file_str_replace');
  const hppAfterWin = asLf(readFileSync(files.hpp, 'utf8'));
  rep.step('line-window stop()', rWinStop.ms, `replacements=${winStop?.result?.replacements}`);
  assert(winStop?.ok === true, `cpp r5: window replace ok (err=${winStop?.error})`);
  assert(winStop?.result?.replacements === 1, 'cpp r5: one replacement');
  assert(hppAfterWin.includes(`void stop(); // ${HPP_WIN_MARKER}`),
    'cpp r5: DesktopAudioCapture stop() marked');
  assert(hppAfterWin.includes(UNIQUE_HPP_STOP_BLOCK),
    'cpp r5: AudioInput stop() neighbor unchanged');
  assert(hppAfterWin.includes('    void stop();\n\n    bool is_playing()'),
    'cpp r5: AudioOutput stop() unchanged');

  const rHppStop = scriptTools(ctx, {
    sessionId,
    suite: 'cpp',
    index: 6,
    name: 'unique neighbor first stop()',
    include: files.hpp,
    calls: [{
      id: 'tc_hppstop',
      name: 'file_str_replace',
      arguments: {
        path: 'audio.hpp',
        old_string: UNIQUE_HPP_STOP_BLOCK,
        new_string: UNIQUE_HPP_STOP_PATCH,
      },
    }],
  });
  const hppStop = toolPayload(rHppStop.bundlePath, 'file_str_replace');
  const hppAfterStop = asLf(readFileSync(files.hpp, 'utf8'));
  rep.step('unique stop() neighbor', rHppStop.ms, `replacements=${hppStop?.result?.replacements}`);
  assert(hppStop?.ok === true, `cpp r6: unique neighbor ok (err=${hppStop?.error})`);
  assert(hppStop?.result?.replacements === 1, 'cpp r6: one replacement');
  assert((hppAfterStop.match(new RegExp(HPP_STOP_MARKER, 'g')) || []).length === 1,
    'cpp r6: hpp stop marker once');
  assert(hppAfterStop.includes(`void stop(); // ${HPP_WIN_MARKER}`),
    'cpp r6: windowed DesktopAudioCapture mark still present');

  const rRun = scriptTools(ctx, {
    sessionId,
    suite: 'cpp',
    index: 7,
    name: 'ambiguous is_running()',
    include: files.hpp,
    calls: [{
      id: 'tc_run',
      name: 'file_str_replace',
      arguments: {
        path: 'audio.hpp',
        old_string: '    bool is_running() const;\n',
        new_string: '    bool is_capturing() const;\n',
      },
    }],
  });
  const runAmb = toolPayload(rRun.bundlePath, 'file_str_replace');
  const runErr = fatalErr(runAmb, rRun);
  rep.step('ambiguous is_running()', rRun.ms, runErr.slice(0, 100));
  assert(/matched 2 times/i.test(runErr) || runAmb?.ok === false,
    `cpp r7: is_running() is ambiguous (got ${runErr})`);
  assert((readFileSync(files.hpp, 'utf8').match(/bool is_running\(\) const;/g) || []).length === 2,
    'cpp r7: is_running decls unchanged');

  const rPatch = scriptTools(ctx, {
    sessionId,
    suite: 'cpp',
    index: 8,
    name: 'unique method replace',
    include: files.cpp,
    calls: [{
      id: 'tc_ok',
      name: 'file_str_replace',
      arguments: {
        path: 'audio.cpp',
        old_string: UNIQUE_CPP_BLOCK,
        new_string: UNIQUE_CPP_PATCH,
      },
    }],
  });
  const patch = toolPayload(rPatch.bundlePath, 'file_str_replace');
  const after = readFileSync(files.cpp, 'utf8');
  rep.step('unique replace', rPatch.ms, `replacements=${patch?.result?.replacements}`);
  assert(patch?.ok === true, `cpp r8: replace ok (err=${patch?.error})`);
  assert(patch?.result?.replacements === 1, 'cpp r8: one replacement');
  assert(after.includes(CPP_MARKER), 'cpp r8: marker written');
  assert((after.match(new RegExp(CPP_MARKER, 'g')) || []).length === 1,
    'cpp r8: marker appears once');
  assert(after.includes('void AudioInput::start('), 'cpp r8: neighboring methods intact');

  const rRing = scriptTools(ctx, {
    sessionId,
    suite: 'cpp',
    index: 9,
    name: 'unique AudioRingBuffer::push',
    include: files.cpp,
    calls: [{
      id: 'tc_ring',
      name: 'file_str_replace',
      arguments: {
        path: 'audio.cpp',
        old_string: UNIQUE_RING_BLOCK,
        new_string: UNIQUE_RING_PATCH,
      },
    }],
  });
  const ring = toolPayload(rRing.bundlePath, 'file_str_replace');
  const afterRing = readFileSync(files.cpp, 'utf8');
  rep.step('unique ring push', rRing.ms, `replacements=${ring?.result?.replacements}`);
  assert(ring?.ok === true, `cpp r9: ring replace ok (err=${ring?.error})`);
  assert(ring?.result?.replacements === 1, 'cpp r9: one replacement');
  assert((afterRing.match(new RegExp(RING_MARKER, 'g')) || []).length === 1,
    'cpp r9: ring marker once');
  assert(afterRing.includes(CPP_MARKER), 'cpp r9: earlier aec marker still present');

  const rCap = scriptTools(ctx, {
    sessionId,
    suite: 'cpp',
    index: 10,
    name: 'unwindowed large cpp fail',
    include: files.cpp,
    calls: [{
      id: 'tc_cap',
      name: 'file_read',
      arguments: { path: 'audio.cpp' },
    }],
  });
  const cap = toolPayload(rCap.bundlePath, 'file_read');
  const capErr = fatalErr(cap, rCap);
  rep.step('file_read unwindowed large', rCap.ms, capErr.slice(0, 120));
  assert(/large file/i.test(capErr),
    `cpp r10: expected large-file fatal (got ${capErr})`);
  assert(/offset\/limit/i.test(capErr),
    `cpp r10: error tells agent to window (got ${capErr})`);

  rep.endSuite();
}

function suiteLiveCpp(rep, assert, ctx) {
  if (ctx.mode !== 'live') {
    rep.note('live-cpp skipped in loopback mode');
    return;
  }
  console.log('\n-- live edit against tests/editor/audio.cpp --\n');
  rep.beginSuite('Live edit (tests/editor/audio.cpp)');

  const files = copyCppFixtures(ctx.cwd);
  const sessionId = `${ctx.runId}-live-cpp`;
  const prompt = [
    'The working folder contains audio.cpp (also selected). It is C++ audio IO.',
    'Do not rewrite the whole file.',
    '1. Call file_search with indexer=rg, regex=true, query="^void AudioInput::set_aec_stream_delay_ms", path=audio.cpp.',
    '2. Call file_read with offset at that hit and limit=20.',
    `3. You MUST call file_str_replace. Use this unique old_string exactly:`,
    UNIQUE_CPP_BLOCK,
    `   new_string inserts a comment line // ${CPP_MARKER} immediately after the opening brace.`,
    '   Do not reply until file_str_replace returns ok.',
    '4. Only after a successful file_str_replace, reply exactly: EDIT_OK',
    'Do not use write_file. Do not use run/sed.',
  ].join(' ');

  const r = runAgentRound({
    ...ctx,
    sessionId,
    prompt,
    extraArgs: [...EDIT_TOOLS, '--include', files.cpp],
    plannerEnabled: true,
    timeout: 240_000,
    roundKey: 'live-cpp-r01',
    roundMeta: { suite: 'live-cpp', index: 1, name: 'rg + window + replace' },
  });
  const calls = toolCalls(r.bundlePath);
  const after = existsSync(files.cpp) ? readFileSync(files.cpp, 'utf8') : '';
  const search = toolPayload(r.bundlePath, 'file_search');
  const read = toolPayload(r.bundlePath, 'file_read');
  const replace = toolPayload(r.bundlePath, 'file_str_replace');
  rep.step('live tools', r.ms, `calls=${calls.join(',')}`);
  if (r.status !== 0) {
    rep.note(`live-cpp argv: tanit-cli ${r.args.join(' ')}`);
    rep.note(`live-cpp stdout: ${truncateText(r.stdout, 400)}`);
    rep.note(`live-cpp stderr: ${truncateText(r.stderr, 400)}`);
  }
  assert(r.status === 0, `live-cpp: exits 0 (status=${r.status} stderr=${truncateText(r.stderr, 160)})`);
  assert(calls.includes('file_search'), `live-cpp: called file_search (got ${calls.join(',')})`);
  assert(calls.includes('file_read') || calls.includes('file_str_replace'),
    `live-cpp: called file_read or file_str_replace (got ${calls.join(',')})`);
  assert(!calls.includes('write_file'),
    `live-cpp: must not rewrite via write_file (got ${calls.join(',')})`);
  if (search) {
    assert(search.ok === true, `live-cpp: file_search ok (err=${search.error})`);
  }
  if (read) {
    assert(read.ok === true, `live-cpp: file_read ok (err=${read.error})`);
    assert(read.result?.truncated !== undefined, 'live-cpp: file_read returned window metadata');
  }
  assert(replace?.ok === true, `live-cpp: file_str_replace ok (err=${replace?.error})`);
  assert(after.includes(CPP_MARKER), 'live-cpp: marker written to work-dir copy of audio.cpp');
  assert((after.match(new RegExp(CPP_MARKER, 'g')) || []).length === 1,
    'live-cpp: marker appears once');
  assert(/EDIT_OK/i.test(finalText(r.bundlePath) || r.json?.final_text || ''),
    `live-cpp: reply EDIT_OK (got ${truncateText(finalText(r.bundlePath))})`);

  rep.endSuite();
}

function walkFiles(dir, out = []) {
  if (!existsSync(dir)) return out;
  for (const name of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, name.name);
    if (name.isDirectory()) walkFiles(p, out);
    else out.push(p);
  }
  return out;
}

function suiteHistory(rep, assert, ctx) {
  if (ctx.mode !== 'loopback') {
    rep.note('history suite skipped in live mode');
    return;
  }
  console.log('\n-- ChangeStore history (loopback) --\n');
  rep.beginSuite('ChangeStore (loopback)');

  const historyDir = ctx.historyDir || join(ctx.paths?.configDir || '', 'history');
  const sessionId = `${ctx.runId}-edit-history`;
  const rel = 'hist.md';
  let i = 0;
  const next = (name, calls) => {
    i += 1;
    return scriptTools(ctx, { sessionId, suite: 'history', index: i, name, calls });
  };

  const rWrite = next('write_file hist.md', [{
    id: 'tc_hw',
    name: 'write_file',
    arguments: { path: rel, content: 'alpha\nbeta\n' },
  }]);
  const wrote = toolPayload(rWrite.bundlePath, 'write_file');
  rep.step('write_file', rWrite.ms, `version=${wrote?.result?.version} change=${wrote?.result?.change_id}`);
  assert(wrote?.ok === true, `history write: ok (err=${wrote?.error})`);
  assert(String(wrote?.result?.version || '').startsWith('sha256:'),
    `history write: version token (got ${wrote?.result?.version})`);
  assert(wrote?.result?.change_id, 'history write: change_id present');
  assert(wrote?.result?.history_ok !== false, 'history write: history_ok');

  const rRead = next('file_read version', [{
    id: 'tc_hr',
    name: 'file_read',
    arguments: { path: rel },
  }]);
  const read = toolPayload(rRead.bundlePath, 'file_read');
  const ver = String(read?.result?.version || '');
  rep.step('file_read version', rRead.ms, ver);
  assert(read?.ok === true, `history read: ok (err=${read?.error})`);
  assert(ver.startsWith('sha256:') && ver.length > 20,
    `history read: whole-file version (got ${ver})`);
  assert(ver === wrote?.result?.version, 'history read: version matches write after-hash');

  const rPatch = next('replace with expected_version', [{
    id: 'tc_hp',
    name: 'file_str_replace',
    arguments: {
      path: rel,
      old_string: 'beta\n',
      new_string: 'beta patched\n',
      expected_version: ver,
    },
  }]);
  const patch = toolPayload(rPatch.bundlePath, 'file_str_replace');
  rep.step('replace + version', rPatch.ms,
    `ok=${patch?.ok} plus=${patch?.result?.plus} minus=${patch?.result?.minus}`);
  assert(patch?.ok === true, `history replace: ok (err=${patch?.error})`);
  assert(patch?.result?.change_id, 'history replace: change_id');
  assert(String(patch?.result?.version || '').startsWith('sha256:'),
    'history replace: after version');
  assert(patch?.result?.version !== ver, 'history replace: version changed');
  assert((patch?.result?.plus || 0) >= 1 || (patch?.result?.minus || 0) >= 1,
    'history replace: plus/minus from generated diff');

  const objects = walkFiles(join(historyDir, 'objects'));
  const fileMans = walkFiles(join(historyDir, 'files'));
  const sessionFiles = walkFiles(join(historyDir, 'sessions'));
  rep.step('disk layout', 0,
    `objects=${objects.length} files=${fileMans.length} sessions=${sessionFiles.length}`);
  assert(objects.length >= 2, `history disk: >=2 objects (got ${objects.length})`);
  assert(fileMans.length >= 1, 'history disk: file manifest');
  assert(sessionFiles.some((p) => p.endsWith('changeset.json')),
    'history disk: session changeset.json');
  const sessionSlug = sessionId.replace(/[^A-Za-z0-9._-]/g, '_').slice(0, 80);
  const recPath = sessionFiles.find((p) => {
    const norm = p.replace(/\\/g, '/');
    return norm.includes(`/sessions/${sessionSlug}/`) && /[0-9a-f]{16}\.json$/i.test(norm);
  }) || sessionFiles.find((p) => /[0-9a-f]{16}\.json$/i.test(p.replace(/\\/g, '/')));
  assert(recPath, 'history disk: change record json');
  const rec = JSON.parse(readFileSync(recPath, 'utf8'));
  assert(rec.schema === 'pm.change.v1', `history record schema (got ${rec.schema})`);
  assert(rec.state === 'pending', 'history record state=pending');
  assert(rec.session_id === sessionId,
    `history record session_id=${sessionId} (got ${rec.session_id})`);
  assert(typeof rec.ts === 'string' && rec.ts.length >= 20, `history record ts (got ${rec.ts})`);
  assert(typeof rec.ts_ms === 'number' && rec.ts_ms > 0, `history record ts_ms (got ${rec.ts_ms})`);
  assert(rec.meta && rec.meta.source === 'agent',
    `history record meta.source=agent (got ${JSON.stringify(rec.meta)})`);
  assert(rec.tool === 'file_str_replace' || rec.tool === 'write_file',
    `history record tool (got ${rec.tool})`);
  assert(rec.before?.sha256 && rec.after?.sha256, 'history record before/after hashes');
  assert(rec.before.sha256 !== rec.after.sha256, 'history record hashes differ after edit');
  assert(Array.isArray(rec.hunks) && rec.hunks.length >= 1, 'history record generated hunks');
  assert(!String(rec.diff || '').includes('old_string'),
    'history record diff is not LLM patch text');

  const rStale = next('stale expected_version', [{
    id: 'tc_hs',
    name: 'file_str_replace',
    arguments: {
      path: rel,
      old_string: 'alpha\n',
      new_string: 'ALPHA\n',
      expected_version: ver,
    },
  }]);
  const stale = toolPayload(rStale.bundlePath, 'file_str_replace');
  const staleErr = fatalErr(stale, rStale);
  rep.step('stale version', rStale.ms, staleErr.slice(0, 100));
  assert(/stale version/i.test(staleErr),
    `history stale: expected stale-version fatal (got ${staleErr})`);
  assert(readFileSync(join(ctx.cwd, rel), 'utf8').includes('beta patched'),
    'history stale: file unchanged');

  const csFiles = walkFiles(join(historyDir, 'sessions')).filter((p) => p.endsWith('changeset.json'));
  const cs = JSON.parse(readFileSync(csFiles[csFiles.length - 1], 'utf8'));
  assert((cs.changes || []).length >= 2,
    `history changeset: write+replace (got ${(cs.changes || []).length})`);

  rep.endSuite();
}

function suiteCompose(rep, assert, ctx) {
  if (ctx.mode !== 'loopback') {
    rep.note('compose suite skipped in live mode');
    return;
  }
  console.log('\n-- file_compose (loopback) --\n');
  rep.beginSuite('file_compose (loopback)');

  const sessionId = `${ctx.runId}-edit-compose`;
  const cwd = ctx.cwd;
  let i = 0;
  const next = (name, calls) => {
    i += 1;
    return scriptTools(ctx, {
      sessionId, suite: 'compose', index: i, name, calls, extraArgs: COMPOSE_TOOLS,
    });
  };
  const compose = (name, args) => {
    const round = next(name, [{
      id: 'tc_cmp',
      name: 'file_compose',
      arguments: args,
    }]);
    const pay = toolPayload(round.bundlePath, 'file_compose');
    return { round, pay, err: fatalErr(pay, round) };
  };
  const readText = (rel) => {
    const p = join(cwd, rel);
    return existsSync(p) ? readFileSync(p, 'utf8') : null;
  };
  const absent = (rel) => {
    assert(!existsSync(join(cwd, rel)), `compose: ${rel} must not exist`);
    assert(!existsSync(join(cwd, `${rel}.tmp~`)), `compose: ${rel}.tmp~ must not remain`);
  };

  const marker = 'COMPOSE_MARKER_BODY';
  writeFileSync(join(cwd, 'src-body.txt'), `alpha\n${marker}\ngamma\n`, 'utf8');
  const assembled = compose('literal + range + whole file', {
    output: 'assembled.txt',
    parts: [
      { type: 'literal', text: '# head\n' },
      { type: 'file', path: 'src-body.txt', start_line: 2, end_line: 3 },
      { type: 'file', path: 'src-body.txt' },
      { type: 'literal', text: 'tail\n' },
    ],
  });
  const assembledText = readText('assembled.txt');
  const expectedAssembled = `# head\n${marker}\ngamma\nalpha\n${marker}\ngamma\ntail\n`;
  rep.step('assemble', assembled.round.ms,
    `ok=${assembled.pay?.ok} bytes=${assembled.pay?.result?.bytes_written} `
    + `ver=${String(assembled.pay?.result?.version || '').slice(0, 18)}`);
  assert(toolCalls(assembled.round.bundlePath).includes('file_compose'),
    'compose assemble: called file_compose');
  assert(assembled.pay?.ok === true, `compose assemble: ok (err=${assembled.err})`);
  assert(assembledText === expectedAssembled, 'compose assemble: parts concatenated with original endings');
  const meta = assembled.pay?.result || {};
  assert(meta.parts === 4, `compose assemble: parts=4 (got ${meta.parts})`);
  assert(meta.literal_parts === 2, `compose assemble: literal_parts=2 (got ${meta.literal_parts})`);
  assert(meta.line_range_parts === 1, `compose assemble: line_range_parts=1 (got ${meta.line_range_parts})`);
  assert(meta.whole_file_parts === 1, `compose assemble: whole_file_parts=1 (got ${meta.whole_file_parts})`);
  assert(meta.atomic === true, 'compose assemble: atomic');
  assert(meta.bytes_written === Buffer.byteLength(expectedAssembled),
    `compose assemble: bytes_written (got ${meta.bytes_written})`);
  assert(meta.source_bytes_copied + meta.literal_bytes_written === meta.bytes_written,
    'compose assemble: source + literal bytes == bytes_written');
  assert(String(meta.version || '').startsWith('sha256:'),
    `compose assemble: version token (got ${meta.version})`);
  assert(meta.change_id, 'compose assemble: change_id for a small snapshot');
  assert(meta.history_ok !== false, 'compose assemble: history recorded');
  assert(meta.content === undefined && meta.text === undefined,
    'compose assemble: result does not return file content');
  assert(!JSON.stringify(meta).includes(marker),
    'compose assemble: marker stays on disk, not in the tool result');
  const sources = meta.sources || [];
  assert(sources.length === 2, `compose assemble: 2 source rows (got ${sources.length})`);
  assert(String(sources[0]?.path || '').includes('src-body.txt')
    && sources[0]?.start_line === 2 && sources[0]?.end_line === 3,
    'compose assemble: ranged source records path and lines');
  assert(String(sources[1]?.path || '').includes('src-body.txt') && sources[1]?.start_line === undefined,
    'compose assemble: whole-file source has no line range');

  const rRead = next('file_read assembled version', [{
    id: 'tc_crd',
    name: 'file_read',
    arguments: { path: 'assembled.txt' },
  }]);
  const read = toolPayload(rRead.bundlePath, 'file_read');
  rep.step('read version', rRead.ms, String(read?.result?.version || '').slice(0, 18));
  assert(read?.ok === true, `compose read: ok (err=${read?.error})`);
  assert(read?.result?.version === meta.version, 'compose read: version matches compose hash');

  const glued = compose('no implicit newline', {
    output: 'glued.txt',
    parts: [
      { type: 'literal', text: 'A' },
      { type: 'literal', text: 'B' },
    ],
  });
  rep.step('no newline', glued.round.ms, `ok=${glued.pay?.ok}`);
  assert(glued.pay?.ok === true, `compose glue: ok (err=${glued.err})`);
  assert(readText('glued.txt') === 'AB', 'compose glue: literals join with no added newline');

  writeFileSync(join(cwd, 'crlf-src.txt'), 'one\r\ntwo\r\nthree\r\n', 'utf8');
  const crlf = compose('CRLF line range', {
    output: 'crlf-out.txt',
    parts: [{ type: 'file', path: 'crlf-src.txt', start_line: 2, end_line: 2 }],
  });
  rep.step('CRLF range', crlf.round.ms, `ok=${crlf.pay?.ok}`);
  assert(crlf.pay?.ok === true, `compose crlf: ok (err=${crlf.err})`);
  assert(readText('crlf-out.txt') === 'two\r\n', 'compose crlf: copied line keeps CRLF');

  writeFileSync(join(cwd, 'unterminated.txt'), 'a\nb', 'utf8');
  const tail = compose('unterminated final line', {
    output: 'tail-out.txt',
    parts: [{ type: 'file', path: 'unterminated.txt', start_line: 2, end_line: 2 }],
  });
  rep.step('unterminated', tail.round.ms, `ok=${tail.pay?.ok}`);
  assert(tail.pay?.ok === true, `compose tail: ok (err=${tail.err})`);
  assert(readText('tail-out.txt') === 'b', 'compose tail: final line without a newline still counts');

  const dup = compose('duplicate sources', {
    output: 'dup-out.txt',
    parts: [
      { type: 'file', path: 'unterminated.txt' },
      { type: 'file', path: 'unterminated.txt' },
    ],
  });
  rep.step('duplicate sources', dup.round.ms, `ok=${dup.pay?.ok}`);
  assert(dup.pay?.ok === true, `compose dup: ok (err=${dup.err})`);
  assert(readText('dup-out.txt') === 'a\nba\nb', 'compose dup: the same source may be copied twice');

  const again = compose('overwrite defaults false', {
    output: 'glued.txt',
    parts: [{ type: 'literal', text: 'ZZ' }],
  });
  rep.step('exists', again.round.ms, again.err.slice(0, 80));
  assert(/output already exists/i.test(again.err),
    `compose exists: expected already-exists (got ${again.err})`);
  assert(readText('glued.txt') === 'AB', 'compose exists: destination unchanged');

  const replaced = compose('overwrite true', {
    output: 'glued.txt',
    overwrite: true,
    parts: [{ type: 'literal', text: 'ZZ' }],
  });
  rep.step('overwrite', replaced.round.ms, `ok=${replaced.pay?.ok}`);
  assert(replaced.pay?.ok === true, `compose overwrite: ok (err=${replaced.err})`);
  assert(readText('glued.txt') === 'ZZ', 'compose overwrite: destination replaced');

  const ver = String(replaced.pay?.result?.version || '');
  const stale = compose('stale expected_version', {
    output: 'glued.txt',
    overwrite: true,
    expected_version: `sha256:${'ab'.repeat(32)}`,
    parts: [{ type: 'literal', text: 'NO' }],
  });
  rep.step('stale version', stale.round.ms, stale.err.slice(0, 100));
  assert(/stale version/i.test(stale.err),
    `compose stale: expected stale-version fatal (got ${stale.err})`);
  assert(readText('glued.txt') === 'ZZ', 'compose stale: destination unchanged');

  const fresh = compose('matching expected_version', {
    output: 'glued.txt',
    overwrite: true,
    expected_version: ver,
    parts: [{ type: 'literal', text: 'OK' }],
  });
  rep.step('matching version', fresh.round.ms, `ok=${fresh.pay?.ok}`);
  assert(fresh.pay?.ok === true, `compose version: ok (err=${fresh.err})`);
  assert(readText('glued.txt') === 'OK', 'compose version: matching expected_version writes');

  const created = compose('create with empty-file version', {
    output: 'empty-ver.txt',
    expected_version: `sha256:${EMPTY_SHA256}`,
    parts: [{ type: 'literal', text: 'new' }],
  });
  rep.step('empty version', created.round.ms, `ok=${created.pay?.ok}`);
  assert(created.pay?.ok === true, `compose empty-ver: ok (err=${created.err})`);
  assert(readText('empty-ver.txt') === 'new', 'compose empty-ver: missing file hashes as empty');

  const badNew = compose('wrong version on missing file', {
    output: 'missing-ver.txt',
    expected_version: `sha256:${'00'.repeat(32)}`,
    parts: [{ type: 'literal', text: 'nope' }],
  });
  rep.step('bad create version', badNew.round.ms, badNew.err.slice(0, 80));
  assert(/stale version/i.test(badNew.err),
    `compose bad-ver: expected stale-version fatal (got ${badNew.err})`);
  absent('missing-ver.txt');

  const half = compose('start_line without end_line', {
    output: 'half.txt',
    parts: [{ type: 'file', path: 'src-body.txt', start_line: 1 }],
  });
  rep.step('half range', half.round.ms, half.err.slice(0, 80));
  assert(/both start_line and end_line/i.test(half.err),
    `compose half: expected both-ends fatal (got ${half.err})`);
  absent('half.txt');

  const backwards = compose('end_line before start_line', {
    output: 'backwards.txt',
    parts: [{ type: 'file', path: 'src-body.txt', start_line: 3, end_line: 1 }],
  });
  rep.step('backwards range', backwards.round.ms, backwards.err.slice(0, 80));
  assert(/end_line is before start_line/i.test(backwards.err),
    `compose backwards: expected order fatal (got ${backwards.err})`);
  absent('backwards.txt');

  const pastStart = compose('start_line past EOF', {
    output: 'past-start.txt',
    parts: [
      { type: 'literal', text: 'kept-out' },
      { type: 'file', path: 'src-body.txt', start_line: 9, end_line: 9 },
    ],
  });
  rep.step('start past EOF', pastStart.round.ms, pastStart.err.slice(0, 80));
  assert(/start_line past end \(3 lines\)/i.test(pastStart.err),
    `compose past-start: expected past-end fatal (got ${pastStart.err})`);
  absent('past-start.txt');

  const pastEnd = compose('end_line past EOF', {
    output: 'past-end.txt',
    parts: [{ type: 'file', path: 'src-body.txt', start_line: 1, end_line: 9 }],
  });
  rep.step('end past EOF', pastEnd.round.ms, pastEnd.err.slice(0, 80));
  assert(/end_line past end \(3 lines\)/i.test(pastEnd.err),
    `compose past-end: expected past-end fatal (got ${pastEnd.err})`);
  absent('past-end.txt');

  const self = compose('source is output', {
    output: 'src-body.txt',
    overwrite: true,
    parts: [{ type: 'file', path: 'src-body.txt' }],
  });
  rep.step('self copy', self.round.ms, self.err.slice(0, 80));
  assert(/source path is the output path/i.test(self.err),
    `compose self: expected same-path fatal (got ${self.err})`);
  assert(readText('src-body.txt') === `alpha\n${marker}\ngamma\n`,
    'compose self: source left untouched');

  const missing = compose('missing source', {
    output: 'missing-src.txt',
    parts: [{ type: 'file', path: 'no-such-src.txt' }],
  });
  rep.step('missing source', missing.round.ms, missing.err.slice(0, 80));
  assert(/source is not a file/i.test(missing.err),
    `compose missing: expected not-a-file (got ${missing.err})`);
  absent('missing-src.txt');

  mkdirSync(join(cwd, 'a-dir'));
  const dir = compose('directory source', {
    output: 'dir-src.txt',
    parts: [{ type: 'file', path: 'a-dir' }],
  });
  rep.step('directory source', dir.round.ms, dir.err.slice(0, 80));
  assert(/source is not a file/i.test(dir.err),
    `compose dir: expected not-a-file (got ${dir.err})`);
  absent('dir-src.txt');

  const bytes = compose('byte range rejected', {
    output: 'bytes.txt',
    parts: [{ type: 'file', path: 'src-body.txt', offset: 0, length: 4 }],
  });
  rep.step('byte range', bytes.round.ms, bytes.err.slice(0, 80));
  assert(/byte ranges are not supported/i.test(bytes.err),
    `compose bytes: expected byte-range fatal (got ${bytes.err})`);
  absent('bytes.txt');

  const litPath = compose('literal with path', {
    output: 'lit-path.txt',
    parts: [{ type: 'literal', text: 'x', path: 'src-body.txt' }],
  });
  rep.step('literal path', litPath.round.ms, litPath.err.slice(0, 80));
  assert(/literal parts only accept text/i.test(litPath.err),
    `compose literal-path: expected literal fatal (got ${litPath.err})`);
  absent('lit-path.txt');

  const emptyParts = compose('empty parts', { output: 'empty-parts.txt', parts: [] });
  rep.step('empty parts', emptyParts.round.ms, emptyParts.err.slice(0, 80));
  assert(/at least one part/i.test(emptyParts.err),
    `compose empty: expected empty-parts fatal (got ${emptyParts.err})`);
  absent('empty-parts.txt');

  const nested = compose('create parent directory', {
    output: 'nested/made.txt',
    parts: [{ type: 'literal', text: 'made' }],
  });
  rep.step('nested output', nested.round.ms, `ok=${nested.pay?.ok}`);
  assert(nested.pay?.ok === true, `compose nested: ok (err=${nested.err})`);
  assert(readText('nested/made.txt') === 'made', 'compose nested: parent directory created');

  const big = 'Q'.repeat((512 * 1024) + 64);
  writeFileSync(join(cwd, 'big-src.txt'), big, 'utf8');
  const large = compose('snapshot skipped over 512 KiB', {
    output: 'big-out.txt',
    parts: [{ type: 'file', path: 'big-src.txt' }],
  });
  const bigPath = join(cwd, 'big-out.txt');
  const bigOut = existsSync(bigPath) ? readFileSync(bigPath) : Buffer.alloc(0);
  rep.step('large copy', large.round.ms,
    `ok=${large.pay?.ok} history=${large.pay?.result?.history_ok} bytes=${bigOut.length}`);
  assert(large.pay?.ok === true, `compose large: ok (err=${large.err})`);
  assert(bigOut.length === Buffer.byteLength(big), 'compose large: whole file copied');
  assert(large.pay?.result?.history_ok === false, 'compose large: history_ok false');
  assert(/512 KiB/.test(String(large.pay?.result?.history_note || '')),
    `compose large: snapshot note (got ${large.pay?.result?.history_note})`);
  assert(!large.pay?.result?.change_id, 'compose large: no change_id when the snapshot is skipped');
  assert(String(large.pay?.result?.version || '').startsWith('sha256:'),
    'compose large: version still returned from the streaming hash');

  rep.endSuite();
}

function toolCallDetails(bundlePath) {
  const bundle = bundlePath ? readJson(bundlePath) : null;
  const events = bundle?.communication?.tools || [];
  return events.filter((e) => e.kind === 'tool_call').map((e) => ({
    tool: e.tool,
    arguments: e.detail?.arguments || {},
  }));
}

function runOfficecli(cwd, args) {
  const r = spawnSync('officecli', args, {
    cwd,
    encoding: 'utf8',
    timeout: 60_000,
    windowsHide: true,
  });
  if (r.error || r.status !== 0) {
    const detail = [r.error?.message, r.stderr, r.stdout].filter(Boolean).join('\n');
    throw new Error(`officecli ${args[0]} failed (${r.status ?? 'error'}): ${detail.slice(0, 500)}`);
  }
}

function seedOfficeReportSources(cwd) {
  mkdirSync(join(cwd, 'sources'), { recursive: true });
  mkdirSync(join(cwd, 'extracts'), { recursive: true });
  runOfficecli(cwd, ['create', OFFICE_XLSX]);
  for (const [addr, value, bold] of OFFICE_SHEET) {
    const args = ['set', OFFICE_XLSX, `/Sheet1/${addr}`, '--prop', `value=${value}`];
    if (bold) args.push('--prop', 'bold=true');
    runOfficecli(cwd, args);
  }
  runOfficecli(cwd, ['set', OFFICE_XLSX, '/Sheet1/E6', '--prop', 'formula=SUM(E2:E5)', '--prop', 'bold=true']);
  runOfficecli(cwd, ['close', OFFICE_XLSX]);
  runOfficecli(cwd, ['create', OFFICE_DOCX]);
  for (const [text, props] of OFFICE_BRIEF) {
    const args = ['add', OFFICE_DOCX, '/body', '--type', 'paragraph', '--prop', `text=${text}`];
    for (const prop of props) args.push('--prop', prop);
    runOfficecli(cwd, args);
  }
  runOfficecli(cwd, ['close', OFFICE_DOCX]);
}

function suiteSearch(rep, assert, ctx) {
  if (ctx.mode !== 'loopback') {
    rep.note('search skipped outside loopback mode');
    return;
  }
  console.log('\n-- search index / query / detail --\n');
  rep.beginSuite('Search index');
  if (!existsSync(RESEARCH_FIXTURE)) {
    assert(false, `search: missing fixture ${RESEARCH_FIXTURE}`);
    rep.endSuite();
    return;
  }
  cpSync(RESEARCH_FIXTURE, join(ctx.cwd, 'edit-tools-research.md'));
  const sourceBytes = statSync(join(ctx.cwd, 'edit-tools-research.md')).size;
  const sessionId = `${ctx.runId}-search`;
  const indexed = scriptTools(ctx, {
    sessionId,
    suite: 'search',
    index: 1,
    name: 'index the research note',
    extraArgs: SEARCH_TOOLS,
    timeout: 180_000,
    calls: [{ name: 'search_index', arguments: { path: 'edit-tools-research.md' } }],
  });
  const indexPay = toolPayload(indexed.bundlePath, 'search_index');
  const outline = indexPay?.result?.outline || [];
  const headings = outline.flatMap((file) => file.headings || []);
  rep.step('search index', indexed.ms, `index=${indexPay?.result?.index || ''}`);
  assert(indexPay?.ok === true, `search: index ok (${indexPay?.error || indexed.json?.error || 'no result'})`);
  assert(/^[0-9a-f]{8,32}$/.test(String(indexPay?.result?.index || '')),
    `search: index id (got ${indexPay?.result?.index})`);
  assert(headings.some((h) => /file_compose|FS-Researcher/i.test(String(h))),
    `search: outline has a heading (got ${headings.slice(0, 6).join(' | ')})`);

  const queried = scriptTools(ctx, {
    sessionId,
    suite: 'search',
    index: 2,
    name: 'query file_compose',
    extraArgs: SEARCH_TOOLS,
    timeout: 180_000,
    calls: [{ name: 'search_query', arguments: { query: 'file_compose write-side retrieval' } }],
  });
  const queryPay = toolPayload(queried.bundlePath, 'search_query');
  const hits = queryPay?.result?.hits || [];
  const snippets = hits.map((h) => `${h.title || ''} ${h.snippet || ''}`).join('\n');
  rep.step('search query', queried.ms, `hits=${hits.length}`);
  assert(queryPay?.ok === true, `search: query ok (${queryPay?.error || 'no result'})`);
  assert(/write_file/.test(String(queryPay?.result?.next || '')),
    `search: query tells the model to write_file (got ${queryPay?.result?.next})`);
  assert(hits.length > 0, 'search: query returned a hit');
  assert(/file_compose|compose/i.test(snippets),
    `search: hit mentions file_compose (got ${truncateText(snippets, 180)})`);

  const docId = hits[0]?.doc_id;
  const detailed = scriptTools(ctx, {
    sessionId,
    suite: 'search',
    index: 3,
    name: 'open one passage',
    extraArgs: SEARCH_TOOLS,
    timeout: 120_000,
    calls: [{ name: 'search_detail', arguments: { doc_id: docId } }],
  });
  const detailPay = toolPayload(detailed.bundlePath, 'search_detail');
  const text = String(detailPay?.result?.text || '');
  rep.step('search detail', detailed.ms, `chars=${text.length}`);
  assert(detailPay?.ok === true, `search: detail ok (${detailPay?.error || 'no result'})`);
  assert(text.length > 40 && text.length < sourceBytes,
    `search: detail is one passage (${text.length} chars, source ${sourceBytes})`);

  const researchPath = join(ctx.cwd, 'edit-tools-research.md');
  cpSync(RESEARCH_FIXTURE, researchPath);
  const blocked = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'file_read of an indexed file is refused',
    extraArgs: SEARCH_TOOLS,
    timeout: 120_000,
    script: [
      { tool_calls: [{ name: 'search_index', arguments: { path: researchPath } }] },
      { tool_calls: [{ name: 'file_read', arguments: { path: researchPath } }] },
      { text: 'done' },
    ],
    roundKey: 'search-r04',
    roundMeta: { suite: 'search', index: 4, name: 'file_read of an indexed file is refused' },
  });
  const readPay = toolPayload(blocked.bundlePath, 'file_read');
  rep.step('search blocks file_read', blocked.ms, readPay?.error || '');
  assert(readPay?.ok !== true, 'search: file_read of an indexed file fails');
  assert(/search_query/.test(String(readPay?.error || '')),
    `search: refusal points at search_query (got ${readPay?.error})`);

  const partDir = join(ctx.cwd, 'paper_md');
  mkdirSync(partDir, { recursive: true });
  const partPath = join(partDir, 'paper_1.md');
  writeFileSync(partPath, '# Part\n\nA converted page.\n');
  const partRead = scriptTools(ctx, {
    sessionId,
    suite: 'search',
    index: 5,
    name: 'file_read of a converted part is refused',
    extraArgs: SEARCH_TOOLS,
    timeout: 60_000,
    calls: [{ name: 'file_read', arguments: { path: partPath } }],
  });
  const partPay = toolPayload(partRead.bundlePath, 'file_read');
  rep.step('converted part blocks file_read', partRead.ms, partPay?.error || '');
  assert(partPay?.ok !== true, 'search: file_read of a converted part fails');
  assert(/search_index/.test(String(partPay?.error || '')),
    `search: part refusal points at search_index (got ${partPay?.error})`);

  const second = runAgentRound({
    ...ctx,
    sessionId,
    prompt: 'a second search_query is refused',
    extraArgs: SEARCH_TOOLS,
    timeout: 120_000,
    script: [
      { tool_calls: [{ name: 'search_query', arguments: { query: 'file_compose' } }] },
      { tool_calls: [{ name: 'search_query', arguments: { query: 'file_compose again' } }] },
      { text: 'done' },
    ],
    roundKey: 'search-r06',
    roundMeta: { suite: 'search', index: 6, name: 'second search_query is refused' },
  });
  const secondPays = toolPayloads(second.bundlePath, 'search_query');
  rep.step('second query refused', second.ms, secondPays[1]?.error || '');
  assert(secondPays[0]?.ok === true, `search: first query in a pass ok (${secondPays[0]?.error || 'no result'})`);
  assert(secondPays[1]?.ok !== true && /write_file/.test(String(secondPays[1]?.error || '')),
    `search: second query points at write_file (got ${secondPays[1]?.error})`);
  rep.endSuite();
}

function suiteLivePaper(rep, assert, ctx) {
  if (ctx.mode !== 'live') {
    rep.note('live-paper skipped in loopback mode');
    return;
  }
  console.log('\n-- live paper capture --\n');
  rep.beginSuite('Live paper capture');
  if (existsSync(RESEARCH_FIXTURE))
    cpSync(RESEARCH_FIXTURE, join(ctx.cwd, 'edit-tools-research.md'));

  const sessionId = `${ctx.runId}-live-paper`;
  const r = runAgentRound({
    ...ctx,
    sessionId,
    prompt: PAPER_PROMPT,
    extraArgs: [...PAPER_TOOLS, '--max-iter', '20'],
    plannerEnabled: true,
    timeout: 720_000,
    roundKey: 'live-paper-r01',
    roundMeta: { suite: 'live-paper', index: 1, name: 'FS-Researcher essentials' },
  });
  const details = toolCallDetails(r.bundlePath);
  const calls = toolCalls(r.bundlePath);
  const readCalls = details.filter((c) => c.tool === 'file_read');
  const readPays = toolPayloads(r.bundlePath, 'file_read');
  let fullParts = 0;
  for (let i = 0; i < readCalls.length; i += 1) {
    const path = JSON.stringify(readCalls[i].arguments);
    if (!/_\d+\.md/i.test(path)) continue;
    if (readPays[i]?.ok === true) fullParts += 1;
  }
  const written = readdirSync(ctx.cwd).filter((name) =>
    name.toLowerCase().endsWith('.md') && name !== 'edit-tools-research.md');
  const nonempty = written.filter((name) => statSync(join(ctx.cwd, name)).size > 200);
  rep.step('live paper', r.ms, `calls=${calls.join(',')}`);
  if (r.status !== 0) {
    rep.note(`live-paper stdout: ${truncateText(r.stdout, 400)}`);
    rep.note(`live-paper stderr: ${truncateText(r.stderr, 400)}`);
  }
  if (nonempty[0]) {
    rep.note(`live-paper wrote ${nonempty[0]} (${statSync(join(ctx.cwd, nonempty[0])).size} bytes)`);
  }
  assert(r.status === 0, `live-paper: agent exit 0 (status ${r.status} ${r.error || ''})`);
  assert(calls.includes('search_index'), `live-paper: search_index was called (${calls.join(',')})`);
  assert(calls.includes('search_query'), `live-paper: search_query was called (${calls.join(',')})`);
  assert(fullParts === 0,
    `live-paper: converted parts were not loaded via file_read (got ${fullParts})`);
  assert(nonempty.length > 0, `live-paper: a new markdown file exists in the work folder (got ${written.join(', ')})`);
  rep.endSuite();
}

function suiteLiveOffice(rep, assert, ctx) {
  if (ctx.mode !== 'live') {
    rep.note('live-office skipped in loopback mode');
    return;
  }
  console.log('\n-- live officecli recap (xlsx + docx -> markdown) --\n');
  rep.beginSuite('Live officecli recap');

  try {
    seedOfficeReportSources(ctx.cwd);
  } catch (err) {
    assert(false, `live-office: seed sources (${err.message})`);
    rep.endSuite();
    return;
  }

  const sessionId = `${ctx.runId}-live-office`;
  const prompt = [
    'The working folder has two Office sources, already created. Do not rewrite them.',
    `- ${OFFICE_XLSX} — Sheet1. Columns Region, Product, Units, Price, Revenue, and a Total row whose Revenue is a formula.`,
    `- ${OFFICE_DOCX} — a short operating brief.`,
    'Write a one-page sales recap to q4-recap.md. The numbers and the board action must come from those files.',
    '1. Load the officecli skill (the office-cli skill). Call info_lookup with kind=skill and query=officecli,',
    '   then call info_lookup again with the exact skill URI or qualified name so the body is loaded.',
    '2. Read the Office files through the office-cli MCP server, not the shell.',
    '   Call info_lookup with kind=mcp and query=office-cli, then request_tools for mcp_office-cli__officecli.',
    '   Call that tool. Its command argument is an officecli command line without the officecli binary:',
    `   command "view ${OFFICE_XLSX} text"`,
    `   command "view ${OFFICE_DOCX} text"`,
    '   Do not call run. Do not paste officecli path prefixes into the report.',
    '3. file_compose output q4-recap.md from literal parts only (no file parts).',
    '   The markdown must contain:',
    '   - a heading "# Q4 Sales Recap"',
    '   - a GitHub-flavored pipe table of the four product rows (not the Total row),',
    '     columns Region, Product, Units, Price, Revenue, values copied from the sheet',
    '   - the Total revenue from the sheet, as a number',
    '   - the brief sentence that names the contract renewal date, quoted',
    '   - one recommendation that names the priority region and the Gadgets price to hold',
    '   Do not include officecli path prefixes such as [/Sheet1/...] or [/body/...].',
    '   Do not use write_file.',
    '4. After file_compose returns ok, reply exactly: OFFICE_OK',
  ].join('\n');

  const r = runAgentRound({
    ...ctx,
    sessionId,
    prompt,
    extraArgs: [...OFFICE_TOOLS, '--max-iter', '16'],
    plannerEnabled: true,
    timeout: 480_000,
    roundKey: 'live-office-r01',
    roundMeta: { suite: 'live-office', index: 1, name: 'officecli dump + file_compose' },
  });
  const calls = toolCalls(r.bundlePath);
  const details = toolCallDetails(r.bundlePath);
  const lookups = details.filter((c) => c.tool === 'info_lookup');
  const mcpCalls = details.filter((c) => /^mcp_office-cli__/.test(String(c.tool)));
  const shellOffice = details.filter((c) =>
    (c.tool === 'run' || c.tool === 'run_sequence')
    && /officecli/i.test(JSON.stringify(c.arguments)));
  const compose = toolPayload(r.bundlePath, 'file_compose');
  const reportPath = join(ctx.cwd, OFFICE_REPORT);
  const report = existsSync(reportPath) ? readFileSync(reportPath, 'utf8') : '';
  const mcpArgs = mcpCalls.map((c) => JSON.stringify(c.arguments)).join('\n');
  rep.step('live office', r.ms, `calls=${calls.join(',')}`);
  if (r.status !== 0) {
    rep.note(`live-office argv: tanit-cli ${r.args.join(' ')}`);
    rep.note(`live-office stdout: ${truncateText(r.stdout, 400)}`);
    rep.note(`live-office stderr: ${truncateText(r.stderr, 400)}`);
  }
  assert(r.status === 0, `live-office: exits 0 (status=${r.status} err=${r.error || ''} stderr=${truncateText(r.stderr, 160)})`);
  assert(lookups.some((c) => /officecli|office-cli/i.test(JSON.stringify(c.arguments))),
    `live-office: info_lookup loaded the officecli skill (got ${calls.join(',')})`);
  assert(mcpCalls.length >= 1,
    `live-office: called office-cli over MCP (got ${calls.join(',')})`);
  assert(shellOffice.length === 0,
    `live-office: did not shell out to officecli (got ${calls.join(',')})`);
  assert(/q4-sales\.xlsx/i.test(mcpArgs), `live-office: MCP read the workbook (got ${calls.join(',')})`);
  assert(/q4-brief\.docx/i.test(mcpArgs), `live-office: MCP read the brief (got ${calls.join(',')})`);
  assert(calls.includes('file_compose'), `live-office: called file_compose (got ${calls.join(',')})`);
  assert(!calls.includes('write_file'),
    `live-office: must not paste the report via write_file (got ${calls.join(',')})`);
  assert(compose?.ok === true, `live-office: file_compose ok (err=${compose?.error})`);
  assert((compose?.result?.literal_parts || 0) >= 1, 'live-office: report authored as literal parts');
  assert((compose?.result?.whole_file_parts || 0) === 0,
    'live-office: report is not a concat of officecli dumps');
  assert(report.length > 0, 'live-office: q4-recap.md written');
  assert(/#\s+Q4 Sales Recap/.test(report), 'live-office: recap heading');
  assert(/\|/.test(report) && /-{3,}/.test(report), 'live-office: markdown pipe table');
  for (const value of ['North', 'South', 'Widgets', 'Gadgets', '14400', '9600', '12000', '16500', '52500']) {
    assert(report.includes(value), `live-office: recap includes sheet value ${value}`);
  }
  assert(report.includes('15 January 2027'), 'live-office: recap quotes the brief renewal date');
  assert(/\[\/(body|Sheet1)\//.test(report) === false && !report.includes('paraId'),
    'live-office: recap has no officecli path dump');
  assert(existsSync(join(ctx.cwd, OFFICE_XLSX)) && existsSync(join(ctx.cwd, OFFICE_DOCX)),
    'live-office: workbook and brief left in place');
  assert(/OFFICE_OK/i.test(finalText(r.bundlePath) || r.json?.final_text || ''),
    `live-office: reply OFFICE_OK (got ${truncateText(finalText(r.bundlePath))})`);

  rep.endSuite();
}

function runSuites(ctx) {
  const { wantSuite } = harness;
  if (wantSuite('loopback')) suiteLoopbackSmoke(rep, stats.assert, ctx);
  if (wantSuite('tools')) suiteTools(rep, stats.assert, ctx);
  if (wantSuite('headings')) suiteHeadings(rep, stats.assert, ctx);
  if (wantSuite('contract')) suiteContract(rep, stats.assert, ctx);
  if (wantSuite('edges')) suiteEdges(rep, stats.assert, ctx);
  if (wantSuite('mass')) suiteMass(rep, stats.assert, ctx);
  if (wantSuite('cpp')) suiteCpp(rep, stats.assert, ctx);
  if (wantSuite('history')) suiteHistory(rep, stats.assert, ctx);
  if (wantSuite('compose')) suiteCompose(rep, stats.assert, ctx);
  if (wantSuite('search')) suiteSearch(rep, stats.assert, ctx);
  if (wantSuite('live-cli')) suiteLiveCli(rep, stats.assert, ctx);
  if (wantSuite('live-cpp')) suiteLiveCpp(rep, stats.assert, ctx);
  if (wantSuite('live-office')) suiteLiveOffice(rep, stats.assert, ctx);
  if (wantSuite('live-paper')) suiteLivePaper(rep, stats.assert, ctx);

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
  rep.note(`Fixture: ${CLI_FIXTURE}`);
  rep.note(`C++ fixtures: ${AUDIO_HPP_FIXTURE}, ${AUDIO_CPP_FIXTURE}`);
  if (boot.ctx.historyDir) rep.note(`ChangeStore: ${boot.ctx.historyDir}`);
  rep.note(`Mode: ${harness.mode}${harness.mode === 'live' ? `, preset=${harness.preset || 'user-default'}` : ''}`);

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
    const hasBundles = existsSync(runDir)
      && readdirSync(runDir).some((name) => name.endsWith('-communication.json'));
    if (!hasBundles) {
      rep.note('session reports skipped (no communication bundles this run)');
    } else {
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
  }
  rep.finalize(stats, performance.now() - t0, llmExe(), {
    runDir,
    sessionDir,
    sessionReportsDir,
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
      sessionReportsDir,
      fixture: CLI_FIXTURE,
    },
  });
}

console.log(`\n${stats.failed === 0 ? 'PASS' : 'FAIL'} agent-edit: ${stats.passed} passed, ${stats.failed} failed`);
console.log(`  report     : ${TEST_REPORT_PATH}`);
console.log(`  run dir    : ${runDir}`);
console.log(`  session dir: ${sessionDir}`);
console.log(`  reports    : ${sessionReportsDir}`);
if (stats.failed > 0 || rep.meta.abortReason) {
  process.exitCode = 1;
}

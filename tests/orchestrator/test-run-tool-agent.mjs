/**
 * Run-tool agent integration test: live LLM creates a TypeScript CLI app.
 *
 * The agent is asked to scaffold a TS+yargs CLI that outputs the size of a
 * target folder.  After the agent finishes, the test installs deps, runs the
 * app, and asserts the output contains a numeric byte size.
 *
 *   npm run test:tools:run:cli-app
 */

import { spawnSync } from 'node:child_process';
import {
  existsSync,
  mkdirSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { createAssert } from '../../../../tests/orchestrator/test-commons.js';
import { renderMarkdownReport } from '../../../../tests/orchestrator/reports.js';

import { REPO } from './repo.mjs';
const TEST_REPORT_PATH = join(REPO, 'tests', 'test-report-last.md');
const PMI = resolve(REPO, 'dist', 'pmi.cmd');
const stats = createAssert();
const { assert } = stats;

function createTestReport() {
  return {
    meta: {}, notes: [], suites: [], images: [], _cur: null,
    note(text) { this.notes.push(text); },
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
      this._cur.steps.push({ label, ms: Math.round(ms * 100) / 100, detail: detail || '' });
    },
    finalize(s, wallMs, exe) {
      this.meta = { passed: s.passed, failed: s.failed, wallMs: Math.round(wallMs), exe };
    },
  };
}

// ── Helpers ─────────────────────────────────────────────────────────────────

/**
 * Spawn pmi.cmd with global flags.  We call cmd.exe /s /c "..." so the
 * entire command line stays intact and .cmd wrappers work.
 */
function pmiRun(args, opts = {}) {
  // Build a single command string for cmd.exe so arguments with spaces stay quoted.
  const quotedArgs = args.map(a => /[\s"]/.test(a) ? `"${a.replace(/"/g, '""')}"` : a);
  const cmdLine = `"${PMI}" --no-gui --no-mcp --log-level trace ${quotedArgs.join(' ')}`;
  return spawnSync('cmd.exe', ['/s', '/c', `"${cmdLine}"`], {
    encoding: 'utf8',
    windowsVerbatimArguments: true,
    ...opts,
  });
}

function agentDryRunHasKey(folder) {
  const r = pmiRun(['llm', 'agent', '--prompt', 'ping', '--folder', folder, '--dry-run'],
    { timeout: 30_000 });
  const combined = (r.stdout || '') + (r.stderr || '');
  return /api_key\s*\|\s*set/i.test(combined) || /API key\s*:\s*<set>/i.test(combined);
}

// ── Test ────────────────────────────────────────────────────────────────────

async function testCliApp(rep) {
  console.log('\n── Run-tool agent: create TypeScript CLI app ──\n');
  rep.beginSuite('Run-tool agent: TS CLI app (yargs, folder-size)');

  const projectDir = resolve(REPO, 'tests', 'run-tool', 'cli-app');
  const testsDir = resolve(REPO, 'tests');

  // Clean slate
  if (existsSync(projectDir)) rmSync(projectDir, { recursive: true, force: true });
  mkdirSync(projectDir, { recursive: true });

  // ── 0. Pre-flight: API key check ──
  if (!agentDryRunHasKey(projectDir)) {
    console.log('  (skipped — no API key in portable chat settings)\n');
    rep.note('Run-tool agent test skipped: no API key configured');
    rep.endSuite();
    return;
  }

  // ── 1. Run the agent ──
  const prompt =
    'Create a TypeScript CLI application in this folder. ' +
    'Use yargs for argument parsing. ' +
    'The CLI takes one positional argument: a folder path. ' +
    'It calculates the total size of all files in that folder (recursively) and prints exactly one line: FOLDER_SIZE_BYTES= followed by the total byte count as an integer (no spaces around the equals sign). ' +
    'Create package.json with typescript, ts-node, yargs, @types/yargs, @types/node as dependencies. ' +
    'Create tsconfig.json. ' +
    'Create src/index.ts with the CLI logic. ' +
    'After creating all files, run npm install using the run tool. ' +
    'Do NOT run the app yourself after npm install, I will test it.';

  const agentLogFile = join(projectDir, '_agent-log.json');

  const agentArgs = [
    'llm', 'agent',
    '--folder', projectDir,
    '--prompt', prompt,
    '--max-iter', '30',
    '--log', agentLogFile,
    '--json',
  ];

  console.log(`  Running agent in ${projectDir} (this may take 1-3 minutes)...`);
  const t0 = performance.now();
  const agentResult = pmiRun(agentArgs, {
    cwd: projectDir,
    timeout: 300_000,
    maxBuffer: 10 * 1024 * 1024,
  });
  const agentMs = performance.now() - t0;
  rep.step('agent round-trip', agentMs);

  // Dump stderr for debugging
  if (agentResult.stderr) {
    const logPath = join(projectDir, '_agent-stderr.log');
    writeFileSync(logPath, agentResult.stderr);
    console.log(`  Agent stderr log: ${logPath}`);
  }

  // Parse agent output: try --json stdout first, fall back to --log file.
  let parsed = null;
  const rawStdout = agentResult.stdout || '';
  {
    const jsonStart = rawStdout.indexOf('{');
    const jsonStr = jsonStart >= 0 ? rawStdout.slice(jsonStart) : '';
    if (jsonStr) {
      try { parsed = JSON.parse(jsonStr); } catch { /* fall through to log file */ }
    }
  }
  if (!parsed && existsSync(agentLogFile)) {
    try {
      const logJson = readFileSync(agentLogFile, 'utf8');
      parsed = JSON.parse(logJson);
      console.log('  (parsed result from --log file)');
    } catch { /* ignore */ }
  }
  if (!parsed) {
    console.log('  Agent stdout (first 2000 chars):', rawStdout.slice(0, 2000));
    assert(false, 'agent produced parseable JSON (stdout or --log file)');
    rep.endSuite();
    return;
  }

  // Agent JSON may nest data under "result" (log file schema) or at top level (--json stdout).
  const result = parsed.result || parsed;

  if (existsSync(agentLogFile))
    console.log(`  Agent JSON log: ${agentLogFile}`);

  console.log(`  Agent ok=${result.ok}, iterations=${result.iterations}, error=${result.error || '(none)'}`);

  assert(result.ok === true,
    `agent ok=true (got ${result.ok}, error="${result.error || ''}", iterations=${result.iterations})`);

  // Verify tool calls include 'run' (npm install) and 'write_file'
  const msgs = result.transcript?.messages ?? [];
  const toolCalls = [];
  for (const m of msgs) {
    if (m.role === 'assistant' && Array.isArray(m.tool_calls)) {
      for (const tc of m.tool_calls) {
        const n = tc.function?.name;
        if (n) toolCalls.push(n);
      }
    }
  }
  console.log(`  Tool calls: ${JSON.stringify(toolCalls)}`);
  assert(toolCalls.includes('run'), `agent used 'run' tool (got ${JSON.stringify(toolCalls)})`);
  assert(toolCalls.includes('write_file'), `agent used 'write_file' tool (got ${JSON.stringify(toolCalls)})`);

  // ── 2. Verify files exist ──
  const pkgJsonPath = join(projectDir, 'package.json');
  assert(existsSync(pkgJsonPath), 'package.json created');

  const nodeModules = join(projectDir, 'node_modules');
  assert(existsSync(nodeModules), 'node_modules exists (npm install ran)');

  // Find the entry point
  let entryPoint = '';
  for (const candidate of ['src/index.ts', 'index.ts', 'src/main.ts', 'main.ts', 'src/cli.ts', 'cli.ts']) {
    if (existsSync(join(projectDir, candidate))) {
      entryPoint = candidate;
      break;
    }
  }
  assert(entryPoint !== '', 'TypeScript entry point found');
  console.log(`  Entry point: ${entryPoint}`);

  if (!entryPoint) {
    rep.endSuite();
    return;
  }

  // ── 3. Run the app against ../tests ──
  // Use --transpile-only to skip TS type-check — the agent's types may not be perfect.
  console.log(`  Running CLI app against ${testsDir}...`);
  const t1 = performance.now();
  const appResult = spawnSync('npx', ['ts-node', '--transpile-only', entryPoint, testsDir], {
    cwd: projectDir,
    encoding: 'utf8',
    timeout: 60_000,
    shell: true,
  });
  const appMs = performance.now() - t1;
  rep.step('cli-app execution', appMs);

  const appOutput = (appResult.stdout || '') + (appResult.stderr || '');
  console.log(`  App exit code: ${appResult.status}`);
  console.log(`  App output (first 500 chars): ${appOutput.slice(0, 500)}`);

  assert(appResult.status === 0, `app exits 0 (got ${appResult.status})`);

  // ── 4. Verify output contains FOLDER_SIZE_BYTES=<number> ──
  const sizeMatch = appOutput.match(/FOLDER_SIZE_BYTES=(\d+)/);
  assert(sizeMatch !== null, 'output contains FOLDER_SIZE_BYTES=<number>');

  if (sizeMatch) {
    const bytes = parseInt(sizeMatch[1], 10);
    assert(bytes > 0, `reported size > 0 (got ${bytes})`);
    console.log(`  Reported folder size: ${bytes} bytes`);
  }

  rep.endSuite();
}

// ── Main ────────────────────────────────────────────────────────────────────

const wallStart = performance.now();
const rep = createTestReport();

console.log('media-img integration tests — run-tool agent: CLI app');
console.log(`  pmi: ${PMI}`);

await testCliApp(rep);

const wallMs = performance.now() - wallStart;
rep.finalize(stats, wallMs, PMI);
console.log(`\nDone. Passed: ${stats.passed}  Failed: ${stats.failed}\n`);

writeFileSync(TEST_REPORT_PATH, renderMarkdownReport(rep));
console.log(`Test report written: ${TEST_REPORT_PATH}`);

process.exit(stats.failed > 0 ? 1 : 0);

#!/usr/bin/env node
import { llmExe } from './exe.mjs';
/**
 * Live coding-agent qualification harness.
 *
 *   node tests/orchestrator/test-agent-coding-qual.mjs \
 *     --mode live --case html-gallery-cli --require-live
 */

import { spawnSync } from 'node:child_process';
import { existsSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

import { brand, cliExePath, createAssert, securityExePath } from '@polymech/tanit/tests/orchestrator/test-commons.js';
import { createMetricsCollector } from '@polymech/tanit/tests/orchestrator/reports.js';
import {
  TEST_REPORT_PATH,
  bootstrapAgentHarness,
  createAgentHarnessOptions,
  createAgentTestReport,
  generateHarnessSessionReports,
  parseArgValue,
  printAgentHarnessBanner,
  runAgentRound,
  runHarnessSuites,
  writeAgentTestReportFile,
} from './test-agent-commons.mjs';
import {
  analyzeCodingAgentBundle,
  analyzeCodingFollowupBundle,
  collectCodingQualUsage,
  loadCodingQualCase,
  seedCodingQualWorkspace,
  verifyGeneratedCodingProject,
} from './lib/coding-qual.mjs';

const LOOPBACK_PRESET = 'coding-qual-loopback';
const CASE_NAME = parseArgValue('--case') || process.env.PM_CODING_QUAL_CASE || 'html-gallery-cli';
const harness = createAgentHarnessOptions({
  harnessSubdir: 'agent-coding-qual-harness',
  loopbackPresetId: LOOPBACK_PRESET,
  displayName: `agent-coding-qual:${CASE_NAME}`,
});

const startedAtIso = new Date().toISOString();
const metricsCollector = createMetricsCollector();
const stats = createAssert();
const rep = createAgentTestReport(harness.paths);
const t0 = performance.now();

let runDir = '';
let runId = '';
let sessionDir = '';
let sessionReportsDir = '';
let qualificationPath = '';
const grantIds = [];

function runSecurity(args) {
  const exe = securityExePath();
  const result = spawnSync(exe, ['--json', ...args], {
    cwd: harness.paths.packageRoot,
    encoding: 'utf8',
    timeout: 30_000,
    windowsHide: true,
  });
  let json = null;
  try { json = JSON.parse(String(result.stdout || '').trim()); } catch { /* reported by caller */ }
  return {
    status: result.status ?? -1,
    json,
    stdout: result.stdout || '',
    stderr: result.stderr || '',
  };
}

function addQualificationGrants(sessionId, workDir) {
  const normalizedWorkDir = workDir.replaceAll('\\', '/');
  const workspaceScope = `file://${normalizedWorkDir}/**`;
  const specs = [
    ['file-search', 'builtin.file_search', 'read', workspaceScope],
    ['file-read', 'builtin.file_read', 'read', workspaceScope],
    ['file-glob', 'builtin.file_glob', 'read', workspaceScope],
    ['write-file', 'builtin.write_file', 'write', workspaceScope],
    ['run', 'builtin.run', 'execute', 'cmd://**'],
  ];
  return specs.map(([suffix, tool, action, scope]) => {
    const id = `coding-qual-${suffix}-${runId}`;
    grantIds.push(id);
    const allowsNetwork = tool === 'builtin.run';
    return {
      id,
      result: runSecurity([
        '--profile', 'DEVELOPER',
        'grant', 'add',
        '--id', id,
        '--tool', tool,
        '--subject', 'llm',
        '--action', action,
        '--lifetime', 'session',
        '--scope', scope,
        '--bind-session', sessionId,
        '--workspace', normalizedWorkDir,
        ...(allowsNetwork ? ['--allow-network'] : []),
      ]),
    };
  });
}

function revokeQualificationGrants() {
  return grantIds.map((id) => ({
    id,
    result: runSecurity(['grant', 'revoke', '--id', id]),
  }));
}

function runCodingQualification(ctx) {
  if (ctx.mode !== 'live')
    throw new Error('coding qualification is live-only; pass --mode live');

  const loadedCase = loadCodingQualCase(ctx.paths.packageRoot, CASE_NAME);
  const seeded = seedCodingQualWorkspace(ctx.cwd, loadedCase.expectations);
  const sessionId = `${ctx.runId}-coding-${CASE_NAME}`;
  const grants = addQualificationGrants(sessionId, ctx.cwd);
  const failedGrant = grants.find(({ result }) =>
    result.status !== 0 || result.json?.ok !== true || result.json?.persisted !== true);
  if (failedGrant) {
    throw new Error(
      `failed to seed scoped grant ${failedGrant.id}: ` +
      `${failedGrant.result.stderr || failedGrant.result.stdout || JSON.stringify(failedGrant.result.json)}`,
    );
  }

  console.log(`\n-- live coding qualification: ${CASE_NAME} --\n`);
  rep.beginSuite(`Live coding qualification: ${CASE_NAME}`);
  rep.note(`Editable objective: ${loadedCase.objectivePath}`);
  rep.note(`Machine expectations: ${loadedCase.expectationsPath}`);
  rep.note(`Seed reference: ${seeded.referencePath}`);

  const round = runAgentRound({
    ...ctx,
    sessionId,
    prompt: loadedCase.objective,
    roundKey: `coding-qual-${CASE_NAME}`,
    roundMeta: {
      suite: 'coding-qual',
      case: CASE_NAME,
      index: 1,
      name: loadedCase.objective.split(/\r?\n/)[0].replace(/^#+\s*/, ''),
      objectivePath: loadedCase.objectivePath,
    },
    plannerEnabled: true,
    extraArgs: [
      '--max-iter', String(loadedCase.expectations.maxIterations || 30),
      '--consent-ui', 'auto-deny',
    ],
    timeout: Number(loadedCase.expectations.timeoutMs || 600_000),
  });
  rep.step('live agent implementation', round.ms,
    `status=${round.status} ok=${round.json?.ok} iterations=${round.communication?.main_agent?.iterations}`);

  const behavior = analyzeCodingAgentBundle(
    { communication: round.communication },
    ctx.cwd,
    loadedCase.expectations,
  );
  let followupRound = null;
  let followupBehavior = null;
  if (loadedCase.followup && round.status === 0 && round.json?.ok === true) {
    rep.note(`Editable follow-up: ${loadedCase.followupPath}`);
    followupRound = runAgentRound({
      ...ctx,
      sessionId,
      prompt: loadedCase.followup,
      roundKey: `coding-qual-${CASE_NAME}-followup`,
      roundMeta: {
        suite: 'coding-qual-followup',
        case: CASE_NAME,
        index: 2,
        name: loadedCase.followup.split(/\r?\n/)[0].replace(/^#+\s*/, ''),
        objectivePath: loadedCase.followupPath,
      },
      plannerEnabled: true,
      extraArgs: [
        '--max-iter', String(loadedCase.expectations.followupMaxIterations || 18),
        '--consent-ui', 'auto-deny',
      ],
      timeout: Number(loadedCase.expectations.timeoutMs || 600_000),
    });
    followupBehavior = analyzeCodingFollowupBundle({
      communication: followupRound.communication,
    });
    rep.step('live agent Markdown follow-up', followupRound.ms,
      `status=${followupRound.status} ok=${followupRound.json?.ok} ` +
      `iterations=${followupRound.communication?.main_agent?.iterations}`);
  }
  const verification = verifyGeneratedCodingProject(ctx.cwd, loadedCase.expectations);
  const usage = collectCodingQualUsage(round);
  const followupUsage = followupRound ? collectCodingQualUsage(followupRound) : null;

  stats.assert(round.status === 0, `coding-qual: agent exits 0 (got ${round.status})`);
  stats.assert(round.json?.ok === true,
    `coding-qual: agent result ok=true (error=${round.json?.error || ''})`);
  stats.assert(behavior.requiredToolsPresent,
    `coding-qual: required tools used (${loadedCase.expectations.requiredTools.join(', ')})`);
  stats.assert(behavior.searchBeforeReferenceRead,
    'coding-qual: workspace search precedes reading discovered reference');
  stats.assert(behavior.referenceRead, 'coding-qual: seeded reference is read');
  stats.assert(behavior.attemptedInstall, 'coding-qual: agent attempts dependency installation');
  stats.assert(behavior.attemptedBuild, 'coding-qual: agent attempts a build');
  stats.assert(behavior.attemptedTest, 'coding-qual: agent attempts automated tests');
  stats.assert(behavior.attemptedSmoke, 'coding-qual: agent attempts the required smoke generation');
  stats.assert(behavior.outsideWrites.length === 0,
    `coding-qual: no write_file target escapes workspace (${behavior.outsideWrites.join(', ')})`);

  if (loadedCase.followup) {
    stats.assert(followupRound?.status === 0,
      `coding-qual follow-up: agent exits 0 (got ${followupRound?.status ?? 'not run'})`);
    stats.assert(followupRound?.json?.ok === true,
      `coding-qual follow-up: agent result ok=true (error=${followupRound?.json?.error || ''})`);
    stats.assert(followupBehavior?.memoryFindCalls === 1,
      `coding-qual follow-up: exactly one memory_find call (got ${followupBehavior?.memoryFindCalls ?? 0})`);
    stats.assert(followupBehavior?.memoryPayloadRecovered === true,
      'coding-qual follow-up: exact prior tool payload is recovered');
    stats.assert(followupBehavior?.memoryValidationPayloadRecovered === true,
      'coding-qual follow-up: recovered payload contains prior validation evidence');
    stats.assert(followupBehavior?.toolCheckpointInjected === true,
      'coding-qual follow-up: compact npm test checkpoint is injected');
    stats.assert(followupBehavior?.toolCheckpointUsed === true,
      'coding-qual follow-up: exact payload retrieval uses the checkpoint locator');
    stats.assert(followupBehavior?.memoryBeforeMutation === true,
      'coding-qual follow-up: memory is recovered before mutation');
    stats.assert(followupBehavior?.repeatedDiscovery === false,
      'coding-qual follow-up: original workspace discovery is not repeated');
    stats.assert(followupBehavior?.repeatedInstall === false,
      'coding-qual follow-up: dependencies are not reinstalled');
    stats.assert(followupBehavior?.testRuns?.length === 1,
      `coding-qual follow-up: tests run exactly once (got ${followupBehavior?.testRuns?.length ?? 0})`);
    stats.assert(followupBehavior?.smokeAttempted === true,
      'coding-qual follow-up: Markdown smoke generation is attempted');
  }

  for (const result of verification.commands) {
    rep.step(`independent: ${result.command}`, result.ms,
      `status=${result.status} ${String(result.stderr || result.stdout).trim().slice(0, 240)}`);
  }
  for (const entry of verification.checks)
    stats.assert(entry.ok, `coding-qual: ${entry.name}${entry.detail ? ` (${entry.detail})` : ''}`);

  const summary = {
    schemaVersion: 1,
    generatedAt: new Date().toISOString(),
    case: CASE_NAME,
    mode: ctx.mode,
    preset: ctx.preset,
    runId: ctx.runId,
    objectivePath: loadedCase.objectivePath,
    workDir: ctx.cwd,
    communicationBundles: [
      round.bundlePath,
      ...(followupRound ? [followupRound.bundlePath] : []),
    ],
    agent: {
      status: round.status,
      ok: round.json?.ok === true,
      finalText: round.communication?.main_agent?.final_text || '',
    },
    behavior,
    followup: followupRound ? {
      objectivePath: loadedCase.followupPath,
      status: followupRound.status,
      ok: followupRound.json?.ok === true,
      finalText: followupRound.communication?.main_agent?.final_text || '',
      behavior: followupBehavior,
    } : null,
    verification,
    usage: {
      ...usage,
      followup: followupUsage,
      aggregate: {
        inputTokens: usage.inputTokens + Number(followupUsage?.inputTokens || 0),
        cachedInputTokens: usage.cachedInputTokens + Number(followupUsage?.cachedInputTokens || 0),
        uncachedInputTokens: usage.uncachedInputTokens + Number(followupUsage?.uncachedInputTokens || 0),
        outputTokens: usage.outputTokens + Number(followupUsage?.outputTokens || 0),
        modelCost: usage.modelCost + Number(followupUsage?.modelCost || 0),
        iterations: usage.iterations + Number(followupUsage?.iterations || 0),
        wallMs: usage.wallMs + Number(followupUsage?.wallMs || 0),
      },
      promptBytes: Buffer.byteLength(loadedCase.objective),
      followupPromptBytes: Buffer.byteLength(loadedCase.followup || ''),
      toolCalls: behavior.calls.length,
      followupToolCalls: followupBehavior?.calls?.length || 0,
      failedToolResults: behavior.failedResults.length,
      followupFailedToolResults: followupBehavior?.failedResults?.length || 0,
      repeatedIdenticalCalls: behavior.repeatedIdenticalCalls.length,
    },
  };
  qualificationPath = join(ctx.runDir, `coding-qual-${CASE_NAME}.json`);
  writeFileSync(qualificationPath, JSON.stringify(summary, null, 2), 'utf8');
  rep.step('qualification summary', 0,
    `tokens=${usage.inputTokens + Number(followupUsage?.inputTokens || 0)}+` +
    `${usage.outputTokens + Number(followupUsage?.outputTokens || 0)} ` +
    `calls=${behavior.calls.length + Number(followupBehavior?.calls?.length || 0)} ` +
    `failed=${behavior.failedResults.length + Number(followupBehavior?.failedResults?.length || 0)} ` +
    `repeats=${behavior.repeatedIdenticalCalls.length}`);
  rep.note(`Structured qualification report: ${qualificationPath}`);
  rep.endSuite();
}

try {
  const exe = llmExe();
  if (!existsSync(exe))
    throw new Error(`${brand.APP_ID_SLUG}-cli not found at ${exe} (build with: npm run build:cpp)`);
  if (!existsSync(securityExePath()))
    throw new Error(`security CLI not found at ${securityExePath()} (build with: npm run build:cpp)`);

  const boot = bootstrapAgentHarness(harness, { exe, perRunWorkDir: true });
  runDir = boot.runDir;
  runId = boot.runId;
  sessionDir = boot.sessionDir;
  printAgentHarnessBanner(harness, {
    exe,
    runDir,
    sessionDir,
  });
  rep.note(`Run artifacts: ${runDir}`);
  rep.note(`Preserved generated workspace: ${boot.workDir}`);
  rep.note(`Preserved session documents: ${sessionDir}`);

  runHarnessSuites(harness, {
    exe,
    ctx: boot.ctx,
    env: boot.env,
    rep,
    runSuites: runCodingQualification,
  });
} catch (error) {
  console.error(error);
  rep.meta.abortReason = 'uncaught exception';
  rep.meta.uncaughtError = error?.stack || String(error);
} finally {
  const revoked = revokeQualificationGrants();
  const failedRevoke = revoked.find(({ result }) =>
    result.status !== 0 || result.json?.removed !== true);
  if (failedRevoke) {
    console.error(`Failed to revoke coding qualification grant ${failedRevoke.id}`);
    stats.assert(false, 'coding-qual: temporary execution grant revoked');
  } else if (revoked.length) {
    stats.assert(true, 'coding-qual: temporary scoped grants revoked');
  }
  if (runDir) {
    try {
      const generated = generateHarnessSessionReports({
        runDir,
        runId,
        displayName: harness.displayName,
      });
      sessionReportsDir = generated.outputDir;
      rep.note(`LLM-friendly session reports: ${generated.outputDir}`);
      stats.assert(true, 'coding-qual: session report generator completed');
    } catch (error) {
      console.error(`Session report generation failed: ${error?.stack || error}`);
      stats.assert(false, 'coding-qual: session report generator completed');
    }
  }
  rep.finalize(stats, performance.now() - t0, llmExe(), {
    runDir,
    sessionDir,
    sessionReportsDir,
    qualificationPath,
    case: CASE_NAME,
    mode: harness.mode,
    preset: harness.preset,
  });
  rep.meta.failed = stats.failed;
  rep.meta.passed = stats.passed;
  writeAgentTestReportFile(rep, metricsCollector, startedAtIso, {
    runDir,
    displayName: harness.displayName,
    testName: 'agent-coding-qualification',
    extraMeta: {
      case: CASE_NAME,
      mode: harness.mode,
      preset: harness.preset,
      sessionDir,
      sessionReportsDir,
      qualificationPath,
    },
  });
}

console.log(`\n${stats.failed === 0 && !rep.meta.abortReason ? 'PASS' : 'FAIL'} coding qualification: ` +
  `${stats.passed} passed, ${stats.failed} failed`);
console.log(`  report       : ${TEST_REPORT_PATH}`);
console.log(`  run dir      : ${runDir}`);
console.log(`  session dir  : ${sessionDir}`);
console.log(`  qualification: ${qualificationPath}`);
console.log(`  reports      : ${sessionReportsDir}`);
if (stats.failed > 0 || rep.meta.abortReason) process.exitCode = 1;

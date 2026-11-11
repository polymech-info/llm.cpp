#!/usr/bin/env node
import { llmExe, prepareCliArgs } from './exe.mjs';
import { REPO as PACKAGE_ROOT } from './repo.mjs';
/**
 * Slim lifecycle harness for critical agent cancellation/visibility paths.
 *
 * Uses local fake /responses and MCP stdio servers so it does not depend on
 * external LLM/MCP providers.
 */

import { spawn, spawnSync } from 'node:child_process';
import { createServer } from 'node:http';
import { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import { cliExePath, securityExePath, brand } from '@polymech/tanit/tests/orchestrator/test-commons.js';

const __dirname = fileURLToPath(new URL('.', import.meta.url));
const STEMP_DIR = join(PACKAGE_ROOT, 'stemp', 'agent-lifecycle');

function assertOk(condition, message) {
  if (!condition) throw new Error(message);
  console.log(`ok - ${message}`);
}

function assertCliOk(condition, message, result) {
  if (!condition) {
    console.error('--- stdout ---');
    console.error((result.stdout || '').trimEnd());
    console.error('--- stderr ---');
    console.error((result.stderr || '').trimEnd());
    throw new Error(message);
  }
  console.log(`ok - ${message}`);
}

function parseJson(text) {
  try {
    return JSON.parse(String(text || '').trim());
  } catch {
    return null;
  }
}

function mcpToolName(serverName, remoteTool) {
  const slug = String(serverName || 'srv')
    .replace(/[^a-zA-Z0-9_-]/g, '_')
    .toLowerCase() || 'srv';
  return `mcp_${slug}__${remoteTool}`;
}

function resetDir(dir) {
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
}

function runCli(exe, args, { env = process.env, timeout = 10_000 } = {}) {
  const result = spawnSync(exe, prepareCliArgs(exe, args), {
    cwd: PACKAGE_ROOT,
    encoding: 'utf8',
    timeout,
    env,
  });
  return {
    status: result.status ?? -1,
    signal: result.signal || '',
    stdout: result.stdout || '',
    stderr: result.stderr || '',
    timedOut: Boolean(result.error && result.error.code === 'ETIMEDOUT'),
  };
}

function writeFakeMcpServer(scriptPath) {
  writeFileSync(scriptPath, String.raw`
const { mkdirSync, writeFileSync } = require('node:fs');
const { join } = require('node:path');
const readline = require('node:readline');
const mode = process.argv[2] || 'ok';
if (process.env.PM_LIFECYCLE_MCP_PID_DIR) {
  mkdirSync(process.env.PM_LIFECYCLE_MCP_PID_DIR, { recursive: true });
  writeFileSync(join(process.env.PM_LIFECYCLE_MCP_PID_DIR, String(process.pid) + '.pid'), String(process.pid), 'utf8');
}
const rl = readline.createInterface({ input: process.stdin });
function send(id, result) {
  process.stdout.write(JSON.stringify({ jsonrpc: '2.0', id, result }) + '\n');
}
rl.on('line', (line) => {
  let req;
  try { req = JSON.parse(line); } catch { return; }
  if (req.method === 'initialize') {
    send(req.id, { protocolVersion: '2025-11-25', capabilities: {}, serverInfo: { name: 'fake-mcp', version: '1' } });
  } else if (req.method === 'tools/list') {
    send(req.id, { tools: [
      { name: 'hang_tool', description: 'fake hanging tool', inputSchema: { type: 'object', properties: {} } },
      { name: 'probe_identity', description: 'fake identity probe', inputSchema: { type: 'object', properties: {} } },
    ] });
  } else if (req.method === 'tools/call') {
    if (mode === 'hang') return;
    if (req.params && req.params.name === 'probe_identity') {
      send(req.id, { content: [{ type: 'text', text: JSON.stringify({ ok: true, marker: 'LIFECYCLE_MCP_OK', pid: process.pid }) }] });
      return;
    }
    if (mode === 'large') {
      const nested = {
        results: Array.from({ length: 3 }, (_, i) => ({
          url: 'https://example.test/service-' + i,
          title: 'Large MCP result ' + i,
          raw_content: 'RAW-CONTENT-' + String(i).repeat(70_000),
        })),
      };
      send(req.id, { content: [{ type: 'text', text: JSON.stringify(nested) }] });
      return;
    }
    send(req.id, { content: [{ type: 'text', text: 'ok' }] });
  }
});
`, 'utf8');
}

function mcpExecuteGrant({ id, toolName }) {
  return {
    id,
    profile: 'DEVELOPER',
    subject: 'llm',
    tool: {
      toolId: toolName,
      provider: 'mcp',
      serverId: '',
      executableHash: '',
      manifestHash: '',
      signedManifest: false,
    },
    action: 'execute',
    resourceScopes: [`mcp://${toolName}`],
    readOnly: true,
    allowSecrets: false,
    allowNetwork: false,
    maxFiles: 100,
    maxBytes: 10485760,
    lifetime: 'session',
    sessionId: '',
    taskId: '',
    workspaceId: '',
    policyVersion: '2026-06-security-v1',
    reason: 'orchestrator lifecycle MCP stress fixture',
    sandboxBackend: '',
  };
}

function writeLoopbackMcpSettings(configDir, { permissions = [] } = {}) {
  writeFileSync(join(configDir, 'settings.json'), JSON.stringify({
    custom: {
      global_tools: {
        disabled_path_tools: [],
        mcp_tools_enabled: true,
        disabled_mcp_servers: [],
        disabled_mcp_tools: [],
      },
      settings_chat_presets: [
        {
          id: 'lifecycle-loopback',
          name: 'lifecycle-loopback',
          is_default: true,
          isDefault: true,
          chat: {
            router: 'loopback',
            model: 'loopback',
            timeout_ms: 30000,
            max_iterations: 6,
            planner_enabled: false,
          },
          tools: {
            disabled_path_tools: [],
            mcp_tools_enabled: true,
            disabled_mcp_servers: [],
            disabled_mcp_tools: [],
          },
        },
      ],
    },
    permissions,
  }, null, 2), 'utf8');
}

function writeSecureMcpConfig(configDir, config) {
  const source = join(configDir, 'mcp.plain.json');
  writeFileSync(source, JSON.stringify(config, null, 2), 'utf8');
  const securityExe = securityExePath();
  const result = spawnSync(securityExe, [
    '--json',
    '--config-dir', configDir,
    'mcp-store',
    'import-plain',
    '--file', source,
  ], { cwd: PACKAGE_ROOT, encoding: 'utf8', timeout: 10_000 });
  const json = parseJson(result.stdout);
  assertCliOk(result.status === 0 && json?.ok === true,
    `secure MCP config imported for ${configDir}`, {
      status: result.status ?? -1,
      stdout: result.stdout || '',
      stderr: result.stderr || '',
    });
}

function startFakeResponsesServer({ mode = 'mcp-timeout', delayMs = 0 } = {}) {
  let requestCount = 0;
  const requests = [];
  const server = createServer(async (req, res) => {
    if (req.method !== 'POST' || req.url !== '/v1/responses') {
      res.writeHead(404).end('not found');
      return;
    }
    let body = '';
    req.setEncoding('utf8');
    req.on('data', (chunk) => { body += chunk; });
    req.on('end', async () => {
      requestCount += 1;
      let parsed = {};
      try { parsed = JSON.parse(body); } catch {}
      requests.push(parsed);

      if (mode === 'hang') {
        return;
      }
      if (delayMs > 0) await delay(delayMs);

      const input = Array.isArray(parsed.input) ? parsed.input : [];
      const hasToolOutput = input.some((item) => item && item.type === 'function_call_output');
      if (!hasToolOutput && Array.isArray(parsed.tools) && parsed.tools.length > 0) {
        const tool = parsed.tools.find((t) => String(t.name || '').startsWith('mcp_')) || parsed.tools[0];
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({
          id: 'resp_fake_1',
          status: 'completed',
          output: [{
            type: 'function_call',
            id: 'fc_fake_1',
            call_id: 'call_fake_1',
            name: tool.name,
            arguments: '{}',
          }],
        }));
        return;
      }

      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({
        id: `resp_fake_${requestCount}`,
        status: 'completed',
        output: [{
          type: 'message',
          content: [{ type: 'output_text', text: 'LIFECYCLE_OK' }],
        }],
      }));
    });
  });

  return new Promise((resolveServer, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const { port } = server.address();
      resolveServer({
        url: `http://127.0.0.1:${port}/v1`,
        close: () => new Promise((resolveClose) => server.close(resolveClose)),
        requests,
      });
    });
  });
}

function spawnAgent(exe, args, env) {
  const child = spawn(exe, args, {
    cwd: PACKAGE_ROOT,
    env,
    stdio: ['ignore', 'pipe', 'pipe'],
    windowsHide: true,
  });
  let stdout = '';
  let stderr = '';
  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  child.stdout.on('data', (chunk) => { stdout += chunk; });
  child.stderr.on('data', (chunk) => { stderr += chunk; });
  return { child, get stdout() { return stdout; }, get stderr() { return stderr; } };
}

function spawnLoopbackAgent(exe, {
  configDir,
  cwdDir = configDir,
  script,
  prompt = 'loopback lifecycle stress prompt',
  presetName = '',
  extraAgentArgs = [],
  env = process.env,
}) {
  const args = [
    '--config-dir', configDir,
    '--log-level', 'trace',
    '--cwd', cwdDir,
    'llm', 'agent',
    ...(presetName ? ['--preset', presetName] : []),
    '--router', 'loopback',
    '--json',
    '--no-planner',
    '--prompt', prompt,
    ...extraAgentArgs,
  ];
  return spawnAgent(exe, args, { ...env, PM_LOOPBACK_SCRIPT: JSON.stringify(script) });
}

function waitForExit(child, timeoutMs) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      reject(new Error(`process did not exit within ${timeoutMs}ms`));
    }, timeoutMs);
    child.once('exit', (code, signal) => {
      clearTimeout(timer);
      resolve({ code, signal });
    });
  });
}

async function pollAgents(exe, configDir, predicate, timeoutMs = 8000) {
  const deadline = Date.now() + timeoutMs;
  let last = [];
  while (Date.now() < deadline) {
    const result = runCli(exe, ['--config-dir', configDir, 'run-ipc', '--json', 'agents'], { timeout: 5000 });
    try {
      last = JSON.parse(result.stdout || '[]');
    } catch {
      last = [];
    }
    if (predicate(last)) return last;
    await delay(250);
  }
  throw new Error(`timed out waiting for agents predicate; last=${JSON.stringify(last)}`);
}

async function pollPredicate(predicate, timeoutMs, label, intervalMs = 250) {
  const deadline = Date.now() + timeoutMs;
  let last;
  while (Date.now() < deadline) {
    last = predicate();
    if (last) return last;
    await delay(intervalMs);
  }
  throw new Error(`timed out waiting for ${label}`);
}

async function waitForRun(run, timeoutMs = 20_000) {
  let exit;
  try {
    exit = await waitForExit(run.child, timeoutMs);
  } catch (err) {
    if (run.child.exitCode === null) run.child.kill();
    return {
      status: -1,
      signal: '',
      stdout: run.stdout,
      stderr: `${run.stderr}\n${err.message}`,
      json: parseJson(run.stdout),
      timedOut: true,
    };
  }
  return {
    status: exit.code ?? -1,
    signal: exit.signal || '',
    stdout: run.stdout,
    stderr: run.stderr,
    json: parseJson(run.stdout),
    timedOut: false,
  };
}

async function assertNoAgentDescriptors(exe, configDir, label) {
  await pollAgents(exe, configDir, (arr) => arr.length === 0, 8000);
  assertOk(true, `${label}: run-ipc agents is empty after shutdown`);
}

function pidAlive(pid) {
  try {
    process.kill(pid, 0);
    return true;
  } catch {
    return false;
  }
}

function readPidMarkers(pidDir) {
  if (!existsSync(pidDir)) return [];
  return readdirSync(pidDir)
    .filter((name) => name.endsWith('.pid'))
    .map((name) => Number(readFileSync(join(pidDir, name), 'utf8').trim()))
    .filter((pid) => Number.isInteger(pid) && pid > 0);
}

function assertInterrupted(result, label) {
  const interrupted = result.status === 130
    || result.signal === 'SIGINT'
    || /interrupted|cancelled|canceled/i.test(result.stderr);
  assertCliOk(interrupted, label, result);
}

function baseAgentArgs(configDir, baseUrl, extra = []) {
  return [
    '--config-dir', configDir,
    '--log-level', 'trace',
    'llm', 'agent',
    '--router', 'openai',
    '--model', 'fake-responses',
    '--api-key', 'test-key',
    '--base-url', baseUrl,
    '--type', 'responses',
    '--timeout-ms', '30000',
    '--max-iter', '3',
    '--prompt', 'exercise lifecycle harness',
    ...extra,
  ];
}

async function testLoopbackConcurrentNoTools(exe) {
  const dir = join(STEMP_DIR, 'loopback-concurrent-no-tools');
  resetDir(dir);

  const runs = Array.from({ length: 5 }, (_, i) => spawnLoopbackAgent(exe, {
    configDir: dir,
    cwdDir: dir,
    prompt: `basic concurrent prompt ${i}`,
    script: [{ text: `LIFECYCLE_BASIC_${i}` }],
    extraAgentArgs: ['--no-tools', '--no-mcp', '--no-skills', '--max-iter', '1'],
  }));

  const results = await Promise.all(runs.map((run) => waitForRun(run)));
  results.forEach((result, i) => {
    assertCliOk(result.status === 0, `loopback basic concurrent agent ${i} exits cleanly`, result);
    assertCliOk(result.json?.ok === true, `loopback basic concurrent agent ${i} returns ok`, result);
    assertCliOk(result.json?.final_text === `LIFECYCLE_BASIC_${i}`,
      `loopback basic concurrent agent ${i} returns its scripted text`, result);
  });
  await assertNoAgentDescriptors(exe, dir, 'loopback basic concurrent agents');
}

async function testLoopbackConcurrentSkills(exe) {
  const dir = join(STEMP_DIR, 'loopback-concurrent-skills');
  resetDir(dir);
  const skillDir = join(dir, 'skills', 'lifecycle-stress');
  mkdirSync(skillDir, { recursive: true });
  writeFileSync(join(skillDir, 'SKILL.md'), [
    '# Lifecycle Stress Skill',
    '',
    'Use this skill only for lifecycle stress fixture prompts.',
    '',
  ].join('\n'), 'utf8');

  const runs = Array.from({ length: 5 }, (_, i) => spawnLoopbackAgent(exe, {
    configDir: dir,
    cwdDir: dir,
    prompt: `skills concurrent prompt ${i}`,
    script: [{ text: `LIFECYCLE_SKILLS_${i}` }],
    extraAgentArgs: ['--no-tools', '--no-mcp', '--max-iter', '1'],
  }));

  const results = await Promise.all(runs.map((run) => waitForRun(run)));
  results.forEach((result, i) => {
    assertCliOk(result.status === 0, `loopback skills concurrent agent ${i} exits cleanly`, result);
    assertCliOk(result.json?.final_text === `LIFECYCLE_SKILLS_${i}`,
      `loopback skills concurrent agent ${i} returns its scripted text`, result);
  });
  await assertNoAgentDescriptors(exe, dir, 'loopback skills concurrent agents');
}

async function testLoopbackConcurrentToolsAndFakeMcp(exe) {
  const dir = join(STEMP_DIR, 'loopback-concurrent-tools-mcp');
  resetDir(dir);
  const mcpScript = join(dir, 'fake-mcp.cjs');
  writeFakeMcpServer(mcpScript);
  const mcpProbeTool = mcpToolName('fixture', 'probe_identity');
  writeLoopbackMcpSettings(dir, {
    permissions: [mcpExecuteGrant({ id: 'lifecycle-mcp-probe', toolName: mcpProbeTool })],
  });
  writeSecureMcpConfig(dir, {
    mcpServers: {
      fixture: { type: 'stdio', command: process.execPath, args: [mcpScript, 'ok'] },
    },
  });

  const runs = Array.from({ length: 5 }, (_, i) => {
    const target = join(dir, `tool-output-${i}.txt`).replace(/\\/g, '/');
    return spawnLoopbackAgent(exe, {
      configDir: dir,
      cwdDir: dir,
      prompt: `tools and fake mcp concurrent prompt ${i}`,
      presetName: 'lifecycle-loopback',
      script: [
        { tool_calls: [{ id: `tc_write_${i}`, name: 'write_file', arguments: { path: target, content: `tool-${i}` } }] },
        { tool_calls: [{ id: `tc_mcp_${i}`, name: mcpProbeTool, arguments: {} }] },
        { text: `LIFECYCLE_TOOLS_MCP_${i}` },
      ],
      extraAgentArgs: ['--no-skills', '--max-iter', '6'],
      env: { ...process.env, PM_MCP_TIMEOUT_MS: '5000' },
    });
  });

  const results = await Promise.all(runs.map((run) => waitForRun(run, 30_000)));
  results.forEach((result, i) => {
    assertCliOk(result.status === 0, `loopback tools+mcp concurrent agent ${i} exits cleanly`, result);
    assertCliOk(result.json?.final_text === `LIFECYCLE_TOOLS_MCP_${i}`,
      `loopback tools+mcp concurrent agent ${i} returns its scripted final text`, result);
    assertCliOk(existsSync(join(dir, `tool-output-${i}.txt`)),
      `loopback tools+mcp concurrent agent ${i} executed write_file`, result);
    const transcript = JSON.stringify(result.json?.transcript ?? {});
    assertCliOk(transcript.includes('LIFECYCLE_MCP_OK'),
      `loopback tools+mcp concurrent agent ${i} received fake MCP result`, result);
  });
  await assertNoAgentDescriptors(exe, dir, 'loopback tools+mcp concurrent agents');
}

async function testConcurrentResponsesMidCancel(exe) {
  const dir = join(STEMP_DIR, 'stress-cancel-basic');
  resetDir(dir);
  const server = await startFakeResponsesServer({ mode: 'hang' });
  const runs = Array.from({ length: 5 }, () =>
    spawnAgent(exe, baseAgentArgs(dir, server.url, ['--no-tools', '--no-mcp', '--no-skills']), process.env)
  );
  try {
    await pollAgents(exe, dir, (arr) => runs.every((run) => arr.some((a) => a.pid === run.child.pid)), 10_000);
    assertOk(true, 'mid-cancel basic: all concurrent agents are visible in run-ipc');
    await delay(750);
    runs.forEach((run) => {
      if (run.child.exitCode === null) run.child.kill('SIGINT');
    });
    const results = await Promise.all(runs.map((run) => waitForRun(run, 10_000)));
    results.forEach((result, i) => {
      assertInterrupted(result, `mid-cancel basic agent ${i} exits via interrupt/cancel`);
    });
    await assertNoAgentDescriptors(exe, dir, 'mid-cancel basic agents');
  } finally {
    runs.forEach((run) => {
      if (run.child.exitCode === null) run.child.kill();
    });
    await server.close();
  }
}

async function testConcurrentMcpMidCancel(exe) {
  const dir = join(STEMP_DIR, 'stress-cancel-mcp');
  resetDir(dir);
  const pidDir = join(dir, 'mcp-pids');
  const mcpScript = join(dir, 'fake-mcp.cjs');
  writeFakeMcpServer(mcpScript);
  const hangTool = mcpToolName('cancel', 'hang_tool');
  writeLoopbackMcpSettings(dir, {
    permissions: [mcpExecuteGrant({ id: 'lifecycle-mcp-mid-cancel', toolName: hangTool })],
  });
  writeSecureMcpConfig(dir, {
    mcpServers: {
      cancel: { type: 'stdio', command: process.execPath, args: [mcpScript, 'hang'] },
    },
  });

  const server = await startFakeResponsesServer({ mode: 'mcp-timeout' });
  const env = {
    ...process.env,
    PM_MCP_TIMEOUT_MS: '60000',
    PM_LIFECYCLE_MCP_PID_DIR: pidDir,
  };
  const runs = Array.from({ length: 5 }, () => spawnAgent(exe, baseAgentArgs(dir, server.url), env));
  try {
    await pollAgents(exe, dir, (arr) => runs.every((run) => arr.some((a) => a.pid === run.child.pid)), 10_000);
    assertOk(true, 'mid-cancel MCP: all concurrent agents are visible in run-ipc');
    const mcpPids = await pollPredicate(() => {
      const pids = readPidMarkers(pidDir);
      return pids.length >= 5 ? pids : null;
    }, 10_000, 'all fake MCP child pid markers');
    assertOk(mcpPids.every(pidAlive), 'mid-cancel MCP: fake MCP stdio children are running before cancel');
    runs.forEach((run) => {
      if (run.child.exitCode === null) run.child.kill('SIGINT');
    });
    const results = await Promise.all(runs.map((run) => waitForRun(run, 12_000)));
    results.forEach((result, i) => {
      assertInterrupted(result, `mid-cancel MCP agent ${i} exits via interrupt/cancel`);
    });
    await assertNoAgentDescriptors(exe, dir, 'mid-cancel MCP agents');
    await pollPredicate(() => mcpPids.every((pid) => !pidAlive(pid)), 10_000, 'fake MCP stdio children to exit');
    assertOk(true, 'mid-cancel MCP: fake MCP stdio children are stopped');
  } finally {
    runs.forEach((run) => {
      if (run.child.exitCode === null) run.child.kill();
    });
    await server.close();
  }
}

async function testMcpTimeout(exe) {
  const dir = join(STEMP_DIR, 'mcp-timeout');
  resetDir(dir);
  const mcpScript = join(dir, 'fake-mcp.cjs');
  writeFakeMcpServer(mcpScript);
  writeLoopbackMcpSettings(dir, {
    permissions: [mcpExecuteGrant({ id: 'lifecycle-mcp-timeout', toolName: mcpToolName('hang', 'hang_tool') })],
  });
  writeSecureMcpConfig(dir, {
    mcpServers: {
      hang: { type: 'stdio', command: process.execPath, args: [mcpScript, 'hang'] },
    },
  });

  const server = await startFakeResponsesServer({ mode: 'mcp-timeout' });
  try {
    const env = { ...process.env, PM_MCP_TIMEOUT_MS: '1500' };
    const run = spawnAgent(exe, baseAgentArgs(dir, server.url), env);
    let exit;
    try {
      exit = await waitForExit(run.child, 20_000);
    } catch (err) {
      if (run.child.exitCode === null) run.child.kill();
      const result = { status: -1, stdout: run.stdout, stderr: run.stderr, timedOut: true };
      assertCliOk(false, `MCP timeout case does not hang the CLI: ${err.message}`, result);
    }
    const result = {
      status: exit.code ?? -1,
      signal: exit.signal || '',
      stdout: run.stdout,
      stderr: run.stderr,
      timedOut: false,
    };
    assertCliOk(!result.timedOut, 'MCP timeout case does not hang the CLI', result);
    assertCliOk(result.status === 0, `MCP timeout case exits cleanly (status=${result.status})`, result);
    assertCliOk(/LIFECYCLE_OK/.test(result.stdout), 'agent recovers after MCP timeout and finalizes', result);
    assertCliOk(/timed out after 1500ms/i.test(result.stderr), 'stderr/log includes MCP timeout', result);
  } finally {
    await server.close();
  }
}

async function testMcpLargeResultCompaction(exe) {
  const dir = join(STEMP_DIR, 'mcp-large-result');
  resetDir(dir);
  const mcpScript = join(dir, 'fake-mcp.cjs');
  writeFakeMcpServer(mcpScript);
  writeLoopbackMcpSettings(dir, {
    permissions: [mcpExecuteGrant({ id: 'lifecycle-mcp-large', toolName: mcpToolName('large', 'hang_tool') })],
  });
  writeSecureMcpConfig(dir, {
    mcpServers: {
      large: { type: 'stdio', command: process.execPath, args: [mcpScript, 'large'] },
    },
  });

  const server = await startFakeResponsesServer({ mode: 'mcp-large' });
  try {
    const env = {
      ...process.env,
      PM_MCP_RESULT_RAW_CONTENT_LIMIT: '500',
      PM_MCP_RESULT_JSON_LIMIT: '6000',
    };
    const run = spawnAgent(exe, baseAgentArgs(dir, server.url), env);
    const exit = await waitForExit(run.child, 20_000);
    const result = {
      status: exit.code ?? -1,
      signal: exit.signal || '',
      stdout: run.stdout,
      stderr: run.stderr,
      timedOut: false,
    };
    assertCliOk(result.status === 0, `large MCP result case exits cleanly (status=${result.status})`, result);
    const second = server.requests.find((r) =>
      Array.isArray(r.input) && r.input.some((item) => item && item.type === 'function_call_output')
    );
    assertOk(Boolean(second), 'large MCP result reaches second Responses round');
    const outputItem = second.input.find((item) => item && item.type === 'function_call_output');
    const output = String(outputItem?.output || '');
    assertOk(output.length < 12_000, `large MCP function_call_output is compacted (${output.length} chars)`);
    assertOk(/truncated/i.test(output), 'large MCP function_call_output marks truncation');
  } finally {
    await server.close();
  }
}

async function testIpcVisibility(exe) {
  const dir = join(STEMP_DIR, 'ipc-visibility');
  resetDir(dir);
  const server = await startFakeResponsesServer({ delayMs: 3500 });
  const run = spawnAgent(exe, baseAgentArgs(dir, server.url, ['--no-tools']), process.env);
  try {
    const agents = await pollAgents(exe, dir, (arr) => arr.some((a) => a.pid === run.child.pid));
    const agent = agents.find((a) => a.pid === run.child.pid);
    assertOk(Boolean(agent), 'run-ipc agents sees live CLI agent pid');
    assertOk(typeof agent.descriptorPath === 'string' && agent.descriptorPath.length > 0, 'agent descriptor exposes descriptorPath');
    assertOk(typeof agent.cmdline === 'string' && /llm\s+agent/i.test(agent.cmdline), 'agent descriptor exposes command line');
    assertOk('processImageName' in agent, 'agent descriptor exposes processImageName');
    const exit = await waitForExit(run.child, 12_000);
    assertOk(exit.code === 0, `visible CLI agent exits cleanly (code=${exit.code})`);
    await pollAgents(exe, dir, (arr) => !arr.some((a) => a.pid === run.child.pid));
    assertOk(true, 'run-ipc agents reaps/removes descriptor after exit');
  } finally {
    if (run.child.exitCode === null) run.child.kill();
    await server.close();
  }
}

async function testResponsesCancel(exe) {
  const dir = join(STEMP_DIR, 'responses-cancel');
  resetDir(dir);
  const server = await startFakeResponsesServer({ mode: 'hang' });
  const run = spawnAgent(exe, baseAgentArgs(dir, server.url, ['--no-tools']), process.env);
  try {
    await pollAgents(exe, dir, (arr) => arr.some((a) => a.pid === run.child.pid));
    await delay(750);
    run.child.kill('SIGINT');
    const exit = await waitForExit(run.child, 10_000);
    const interrupted = exit.code === 130 || exit.signal === 'SIGINT' || /interrupted|cancelled/i.test(run.stderr);
    assertOk(interrupted, `Responses hang is interruptible (code=${exit.code}, signal=${exit.signal || ''})`);
  } finally {
    if (run.child.exitCode === null) run.child.kill();
    await server.close();
  }
}

async function main() {
  const exe = llmExe();
  assertOk(existsSync(exe), `${brand.APP_ID_SLUG}-cli exists: ${exe}`);
  resetDir(STEMP_DIR);

  const selected = new Set(
    (process.argv.find((arg) => arg.startsWith('--case='))?.slice('--case='.length)
      || 'stress-basic,stress-skills,stress-tools-mcp,stress-cancel-basic,stress-cancel-mcp,mcp,mcp-large,ipc,responses')
      .split(',')
      .map((s) => s.trim())
      .filter(Boolean),
  );

  if (selected.has('stress-basic')) await testLoopbackConcurrentNoTools(exe);
  if (selected.has('stress-skills')) await testLoopbackConcurrentSkills(exe);
  if (selected.has('stress-tools-mcp')) await testLoopbackConcurrentToolsAndFakeMcp(exe);
  if (selected.has('stress-cancel-basic')) await testConcurrentResponsesMidCancel(exe);
  if (selected.has('stress-cancel-mcp')) await testConcurrentMcpMidCancel(exe);
  if (selected.has('mcp')) await testMcpTimeout(exe);
  if (selected.has('mcp-large')) await testMcpLargeResultCompaction(exe);
  if (selected.has('ipc')) await testIpcVisibility(exe);
  if (selected.has('responses')) await testResponsesCancel(exe);
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : err);
  process.exit(1);
});

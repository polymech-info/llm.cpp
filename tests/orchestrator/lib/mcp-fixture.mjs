import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

export const stdioFixtureServerName = 'fixture';
export const stdioFixturePresetName = 'mcp-fixture-loopback';

export function parseJson(text) {
  try {
    return JSON.parse(text);
  } catch {
    return null;
  }
}

export function mcpToolName(serverName, remoteTool) {
  const slug = String(serverName || 'srv')
    .replace(/[^a-zA-Z0-9_-]/g, '_')
    .toLowerCase() || 'srv';
  return `mcp_${slug}__${remoteTool}`;
}

export function writeStdioMcpConfig({
  configDir,
  serverName = stdioFixtureServerName,
  command,
  args = [],
  env = {},
  serverConfig = {},
  rootConfig = {},
}) {
  mkdirSync(configDir, { recursive: true });
  writeFileSync(join(configDir, 'mcp.json'), JSON.stringify({
    ...rootConfig,
    mcpServers: {
      [serverName]: {
        type: 'stdio',
        command,
        args,
        env,
        ...serverConfig,
      },
    },
  }, null, 2), 'utf8');
}

export function mcpExecuteGrant({
  id,
  toolName,
  scope = `mcp://${toolName}`,
  reason = 'orchestrator stdio MCP fixture baseline',
  sandboxBackend = '',
  allowNetwork = false,
}) {
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
    resourceScopes: [scope],
    readOnly: true,
    allowSecrets: false,
    allowNetwork,
    maxFiles: 100,
    maxBytes: 10485760,
    lifetime: 'session',
    sessionId: '',
    taskId: '',
    workspaceId: '',
    policyVersion: '2026-06-security-v1',
    reason,
    sandboxBackend,
  };
}

export function writeLoopbackMcpSettings({
  configDir,
  presetName = stdioFixturePresetName,
  permissions = [],
  maxIterations = 4,
}) {
  mkdirSync(configDir, { recursive: true });
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
          id: presetName,
          name: presetName,
          is_default: true,
          isDefault: true,
          chat: {
            router: 'loopback',
            model: 'loopback',
            timeout_ms: 30000,
            max_iterations: maxIterations,
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

export function createStdioMcpFixtureConfig({
  repoRoot,
  prefix = 'pm-mcp-stdio-fixture-',
  serverName = stdioFixtureServerName,
  marker = `fixture-${Date.now()}`,
  permissions = [],
  command = 'node',
  fixturePath = resolve(repoRoot, 'tests', 'fixtures', 'mcp', 'stdio-fixture-server.mjs'),
  args = [fixturePath],
  env = {},
  serverConfig = {},
  rootConfig = {},
} = {}) {
  const configDir = mkdtempSync(join(tmpdir(), prefix));
  if (!existsSync(fixturePath)) {
    rmSync(configDir, { recursive: true, force: true });
    throw new Error(`missing stdio fixture ${fixturePath}`);
  }
  writeLoopbackMcpSettings({ configDir, permissions });
  writeStdioMcpConfig({
    configDir,
    serverName,
    command,
    args,
    env: { PM_MCP_FIXTURE_MARKER: marker, ...env },
    serverConfig,
    rootConfig,
  });
  return {
    configDir,
    fixturePath,
    marker,
    serverName,
    cleanup() {
      rmSync(configDir, { recursive: true, force: true });
    },
  };
}

export function loopbackScriptForToolCall({ id, name, arguments: toolArgs = {}, finalText = 'done' }) {
  return [
    {
      tool_calls: [{
        id: `${id}_request`,
        name: 'request_tools',
        arguments: { names: [name], reason: 'orchestrator MCP fixture call' },
      }],
    },
    { tool_calls: [{ id, name, arguments: toolArgs }] },
    { text: finalText },
  ];
}

export function runLoopbackAgent({
  exe,
  configDir,
  pmImgArgs,
  script,
  presetName = stdioFixturePresetName,
  prompt = 'mcp-stdio-fixture-test',
  globalArgs = [],
  agentArgs = [],
  cwd,
  timeout = 30000,
  logLevel = 'debug',
  extraEnv = {},
}) {
  const args = [
    ...globalArgs,
    '--config-dir', configDir,
    '--log-level', logLevel,
    'llm', 'agent',
    '--router', 'loopback',
    '--json',
    '--no-planner',
    '--prompt', prompt,
  ];
  if (presetName)
    args.splice(args.indexOf('--router'), 0, '--preset', presetName);
  args.push(...agentArgs);
  const argv = pmImgArgs(...args);
  const result = spawnSync(exe, argv, {
    cwd,
    encoding: 'utf8',
    timeout,
    maxBuffer: 16 << 20,
    env: { ...process.env, ...extraEnv, PM_LOOPBACK_SCRIPT: JSON.stringify(script) },
  });
  return {
    status: result.status ?? -1,
    stdout: result.stdout || '',
    stderr: result.stderr || '',
    json: parseJson(result.stdout || ''),
  };
}

export function transcriptText(agentResult) {
  return JSON.stringify(agentResult?.json ?? null);
}

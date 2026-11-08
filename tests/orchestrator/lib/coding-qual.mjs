import { spawnSync } from 'node:child_process';
import {
  existsSync, mkdirSync, readFileSync, readdirSync, statSync, writeFileSync,
} from 'node:fs';
import { dirname, isAbsolute, join, relative, resolve } from 'node:path';

const PNG_1X1 = Buffer.from(
  'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Y9Z2S8AAAAASUVORK5CYII=',
  'base64',
);
const JPEG_1X1 = Buffer.from(
  '/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAP//////////////////////////////////////////////////////////////////////////////////////2wBDAf//////////////////////////////////////////////////////////////////////////////////////wAARCAABAAEDASIAAhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAAAAAAX/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oADAMBAAIQAxAAAAEf/8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABBQJ//8QAFBEBAAAAAAAAAAAAAAAAAAAAAP/aAAgBAwEBPwF//8QAFBEBAAAAAAAAAAAAAAAAAAAAAP/aAAgBAgEBPwF//8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQAGPwJ//8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABPyF//9oADAMBAAIAAwAAABAf/8QAFBEBAAAAAAAAAAAAAAAAAAAAAP/aAAgBAwEBPxB//8QAFBEBAAAAAAAAAAAAAAAAAAAAAP/aAAgBAgEBPxB//8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABPxB//9k=',
  'base64',
);

function readJson(path) {
  return JSON.parse(readFileSync(path, 'utf8'));
}

function listFiles(root, dir = root) {
  const out = [];
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const path = join(dir, entry.name);
    if (entry.isDirectory()) out.push(...listFiles(root, path));
    else out.push(relative(root, path).replaceAll('\\', '/'));
  }
  return out;
}

export function loadCodingQualCase(packageRoot, caseName) {
  if (!/^[a-z0-9][a-z0-9-]*$/i.test(caseName))
    throw new Error(`invalid coding qualification case: ${caseName}`);
  const caseDir = join(packageRoot, 'packages', 'llm.cpp', 'tests', 'orchestrator', 'fixtures', 'coding-qual', caseName);
  const objectivePath = join(caseDir, 'objective.md');
  const expectationsPath = join(caseDir, 'expectations.json');
  if (!existsSync(objectivePath) || !existsSync(expectationsPath))
    throw new Error(`coding qualification case is incomplete: ${caseDir}`);
  const expectations = readJson(expectationsPath);
  const followupPath = expectations.followupPrompt
    ? join(caseDir, expectations.followupPrompt) : '';
  if (followupPath && !existsSync(followupPath))
    throw new Error(`coding qualification follow-up is missing: ${followupPath}`);
  return {
    caseDir,
    objectivePath,
    expectationsPath,
    objective: readFileSync(objectivePath, 'utf8').trim(),
    expectations,
    followupPath,
    followup: followupPath ? readFileSync(followupPath, 'utf8').trim() : '',
  };
}

export function seedCodingQualWorkspace(workDir, expectations) {
  const qualificationDir = join(workDir, '.qualification');
  const probeDir = join(qualificationDir, 'probe-input');
  mkdirSync(join(probeDir, 'nested'), { recursive: true });
  const referencePath = join(workDir, expectations.referencePath);
  mkdirSync(dirname(referencePath), { recursive: true });
  writeFileSync(referencePath, [
    `Marker: ${expectations.referenceMarker}`,
    '',
    'Required product detail:',
    `- The generated document title and visible H1 must be exactly "${expectations.requiredTitle}".`,
    '- Image previews must include useful alt text based on each relative path.',
    '- Empty directories must remain visible in the listing.',
    '',
  ].join('\n'), 'utf8');
  writeFileSync(join(probeDir, 'README & notes.txt'), 'probe text <safe> & deterministic\n', 'utf8');
  writeFileSync(join(probeDir, 'cover.jpg'), JPEG_1X1);
  writeFileSync(join(probeDir, 'nested', 'photo one.png'), PNG_1X1);
  mkdirSync(join(probeDir, 'nested', 'empty-dir'), { recursive: true });
  return { qualificationDir, probeDir, referencePath };
}

export function analyzeCodingAgentBundle(bundle, workDir, expectations) {
  const events = Array.isArray(bundle?.communication?.tools) ? bundle.communication.tools : [];
  const calls = events
    .map((event, eventIndex) => ({ ...event, eventIndex }))
    .filter((event) => event.kind === 'tool_call')
    .map((event) => ({
      name: String(event.tool || ''),
      id: String(event.detail?.id || ''),
      args: event.detail?.arguments && typeof event.detail.arguments === 'object'
        ? event.detail.arguments : {},
      eventIndex: event.eventIndex,
    }));
  const results = events
    .map((event, eventIndex) => ({ ...event, eventIndex }))
    .filter((event) => event.kind === 'tool_result')
    .map((event) => ({
      name: String(event.tool || ''),
      id: String(event.detail?.id || ''),
      envelope: event.detail?.envelope && typeof event.detail.envelope === 'object'
        ? event.detail.envelope : {},
      eventIndex: event.eventIndex,
    }));
  const callNames = calls.map((call) => call.name);
  const searchIndex = calls.findIndex((call) => call.name === 'file_search');
  const referenceReadIndex = calls.findIndex((call) =>
    call.name === 'file_read' &&
    resolve(workDir, String(call.args.path || '')) === resolve(workDir, expectations.referencePath));
  const runCommands = calls
    .filter((call) => call.name === 'run')
    .map((call) => String(call.args.command || ''));
  const runResultText = results
    .filter((result) => result.name === 'run')
    .map((result) => JSON.stringify(result.envelope))
    .join('\n');
  const writeCalls = calls.filter((call) => call.name === 'write_file');
  const outsideWrites = writeCalls.filter((call) => {
    const raw = String(call.args.path || '');
    const target = isAbsolute(raw) ? resolve(raw) : resolve(workDir, raw);
    const rel = relative(resolve(workDir), target);
    return rel === '..' || rel.startsWith(`..\\`) || rel.startsWith('../') || isAbsolute(rel);
  });
  const failedResults = results.filter(({ envelope }) =>
    envelope.ok === false ||
    envelope.security_denied === true ||
    (Number.isInteger(envelope.exit_code) && envelope.exit_code !== 0));
  const signatures = new Map();
  for (const call of calls) {
    const signature = `${call.name}:${JSON.stringify(call.args)}`;
    signatures.set(signature, (signatures.get(signature) || 0) + 1);
  }
  const repeatedIdenticalCalls = [...signatures.entries()]
    .filter(([, count]) => count > 1)
    .map(([signature, count]) => ({ signature, count }));
  return {
    calls,
    results,
    callNames,
    toolCounts: Object.fromEntries(
      [...new Set(callNames)].map((name) => [name, callNames.filter((value) => value === name).length]),
    ),
    requiredToolsPresent: expectations.requiredTools.every((name) => callNames.includes(name)),
    searchBeforeReferenceRead:
      searchIndex >= 0 && referenceReadIndex >= 0 && searchIndex < referenceReadIndex,
    referenceRead: referenceReadIndex >= 0,
    runCommands,
    attemptedInstall: runCommands.some((command) => /\bnpm(?:\.cmd)?\s+(?:i|install)\b/i.test(command)),
    attemptedBuild:
      runCommands.some((command) => /\bnpm(?:\.cmd)?\s+run\s+build\b/i.test(command)) ||
      /(?:npm run build|>\s*\S+\s+build\b|\btsc\s+-p\b)/i.test(runResultText),
    attemptedTest: runCommands.some((command) => /\bnpm(?:\.cmd)?\s+(?:run\s+)?test\b/i.test(command)),
    attemptedSmoke: runCommands.some((command) =>
      command.includes('.qualification') && /agent-gallery\.html/i.test(command)),
    outsideWrites: outsideWrites.map((call) => String(call.args.path || '')),
    failedResults: failedResults.map(({ name, envelope }) => ({
      name,
      error: envelope.error || envelope.message || '',
      exitCode: envelope.exit_code,
      securityDenied: envelope.security_denied === true,
    })),
    repeatedIdenticalCalls,
  };
}

export function analyzeCodingFollowupBundle(bundle) {
  const events = Array.isArray(bundle?.communication?.tools) ? bundle.communication.tools : [];
  const promptText = JSON.stringify(bundle?.communication?.main_agent?.messages || []);
  const calls = events
    .map((event, eventIndex) => ({ ...event, eventIndex }))
    .filter((event) => event.kind === 'tool_call')
    .map((event) => ({
      name: String(event.tool || ''),
      args: event.detail?.arguments && typeof event.detail.arguments === 'object'
        ? event.detail.arguments : {},
      eventIndex: event.eventIndex,
    }));
  const results = events
    .map((event, eventIndex) => ({ ...event, eventIndex }))
    .filter((event) => event.kind === 'tool_result')
    .map((event) => ({
      name: String(event.tool || ''),
      envelope: event.detail?.envelope && typeof event.detail.envelope === 'object'
        ? event.detail.envelope : {},
      eventIndex: event.eventIndex,
    }));
  const memoryCall = calls.find((call) => call.name === 'memory_find');
  const firstMutation = calls.find((call) => call.name === 'write_file' || call.name === 'run');
  const runCommands = calls
    .filter((call) => call.name === 'run')
    .map((call) => String(call.args.command || ''));
  const memoryResults = results.filter((result) => result.name === 'memory_find');
  const recoveredValidationPayload = memoryResults.some((result) => {
    const envelope = result.envelope;
    return envelope.payload_found === true &&
      envelope.tool_name === 'run' &&
      /(?:\btests?\b|TAP version|npm(?:\.cmd)?\s+(?:run\s+)?test\b)/i
        .test(String(envelope.payload_text || ''));
  });
  const testRuns = runCommands.filter((command) =>
    /\bnpm(?:\.cmd)?\s+(?:run\s+)?test\b/i.test(command));
  return {
    calls,
    results,
    toolCounts: Object.fromEntries(
      [...new Set(calls.map((call) => call.name))]
        .map((name) => [name, calls.filter((call) => call.name === name).length]),
    ),
    memoryFindCalls: calls.filter((call) => call.name === 'memory_find').length,
    memoryPayloadRecovered: memoryResults.some((result) =>
      result.envelope.payload_found === true || result.envelope.body_found === true),
    memoryValidationPayloadRecovered: recoveredValidationPayload,
    toolCheckpointInjected: /checkpoint_run_test/.test(promptText) &&
      /kind=tool_checkpoint/.test(promptText),
    toolCheckpointUsed: /^checkpoint_run_test$/i.test(
      String(memoryCall?.args?.itemId || '')),
    memoryBeforeMutation: Boolean(memoryCall && firstMutation &&
      memoryCall.eventIndex < firstMutation.eventIndex),
    repeatedDiscovery: calls.some((call) =>
      call.name === 'file_search' || call.name === 'file_glob'),
    repeatedInstall: runCommands.some((command) =>
      /\bnpm(?:\.cmd)?\s+(?:i|install)\b/i.test(command)),
    testRuns,
    smokeAttempted: runCommands.some((command) =>
      /--md\b/i.test(command) && /agent-listing\.md/i.test(command)),
    failedResults: results.filter(({ envelope }) =>
      envelope.ok === false ||
      envelope.security_denied === true ||
      (Number.isInteger(envelope.exit_code) && envelope.exit_code !== 0)),
  };
}

function run(command, args, cwd, timeout = 120_000) {
  const started = performance.now();
  const result = spawnSync(command, args, {
    cwd,
    encoding: 'utf8',
    timeout,
    maxBuffer: 16 << 20,
    windowsHide: true,
    shell: process.platform === 'win32' && /\.(?:cmd|bat)$/i.test(command),
  });
  return {
    command: [command, ...args].join(' '),
    status: result.status ?? -1,
    stdout: result.stdout || '',
    stderr: result.stderr || '',
    ms: Math.round((performance.now() - started) * 100) / 100,
  };
}

function check(checks, name, condition, detail = '') {
  checks.push({ name, ok: Boolean(condition), detail: String(detail || '') });
  return Boolean(condition);
}

export function verifyGeneratedCodingProject(workDir, expectations) {
  const checks = [];
  const commands = [];
  for (const path of expectations.requiredFiles)
    check(checks, `required file: ${path}`, existsSync(join(workDir, path)));
  check(checks, 'node_modules installed', existsSync(join(workDir, 'node_modules')));
  check(checks, 'agent smoke HTML exists', existsSync(join(workDir, expectations.agentSmokeOutput)));

  const packagePath = join(workDir, 'package.json');
  const pkg = existsSync(packagePath) ? readJson(packagePath) : {};
  const dependencies = pkg.dependencies && typeof pkg.dependencies === 'object' ? pkg.dependencies : {};
  const devDependencies = pkg.devDependencies && typeof pkg.devDependencies === 'object'
    ? pkg.devDependencies : {};
  const runtimeNames = Object.keys(dependencies).sort();
  const directDependencyCount = runtimeNames.length + Object.keys(devDependencies).length;
  check(checks, 'package uses ESM', pkg.type === 'module', `type=${pkg.type}`);
  check(checks, 'runtime dependencies are bounded',
    runtimeNames.every((name) => expectations.onlyRuntimeDependencies.includes(name)),
    runtimeNames.join(', '));
  check(checks, 'yargs is a runtime dependency', runtimeNames.includes('yargs'));
  check(checks, 'TypeScript is a development dependency', Boolean(devDependencies.typescript));
  check(checks, 'direct dependency count is bounded',
    directDependencyCount <= expectations.maxDirectDependencies,
    `${directDependencyCount}/${expectations.maxDirectDependencies}`);
  check(checks, 'build script exists', typeof pkg.scripts?.build === 'string');
  check(checks, 'test script exists and is substantive',
    typeof pkg.scripts?.test === 'string' && !/\becho\b/i.test(pkg.scripts.test));

  const projectFiles = listFiles(workDir).filter((path) => !path.startsWith('node_modules/'));
  check(checks, 'automated test source exists',
    projectFiles.some((path) => /(?:^|\/)(?:test|tests|__tests__)(?:\/|$)|\.(?:test|spec)\.[cm]?[jt]s$/i.test(path)));

  const npm = process.platform === 'win32' ? 'npm.cmd' : 'npm';
  if (pkg.scripts?.test) {
    const result = run(npm, ['test'], workDir);
    commands.push(result);
    check(checks, 'independent npm test passes', result.status === 0,
      result.stderr || result.stdout);
  }
  if (pkg.scripts?.build) {
    const result = run(npm, ['run', 'build'], workDir);
    commands.push(result);
    check(checks, 'independent npm build passes', result.status === 0,
      result.stderr || result.stdout);
  }

  const binValue = typeof pkg.bin === 'string'
    ? pkg.bin
    : (pkg.bin && typeof pkg.bin === 'object' ? Object.values(pkg.bin)[0] : '');
  const builtEntry = binValue ? join(workDir, String(binValue)) : join(workDir, 'dist', 'index.js');
  check(checks, 'built CLI entry exists', existsSync(builtEntry), relative(workDir, builtEntry));
  if (existsSync(builtEntry)) {
    const help = run(process.execPath, [builtEntry, '--help'], workDir);
    commands.push(help);
    check(checks, 'CLI help succeeds', help.status === 0, help.stderr || help.stdout);

    const outputPath = join(workDir, expectations.independentOutput);
    mkdirSync(dirname(outputPath), { recursive: true });
    const generate = run(process.execPath, [
      builtEntry,
      join(workDir, '.qualification', 'probe-input'),
      '--output',
      outputPath,
    ], workDir);
    commands.push(generate);
    check(checks, 'independent gallery generation succeeds', generate.status === 0,
      generate.stderr || generate.stdout);

    if (expectations.independentMarkdownOutput) {
      const markdownPath = join(workDir, expectations.independentMarkdownOutput);
      const markdownGenerate = run(process.execPath, [
        builtEntry,
        join(workDir, '.qualification', 'probe-input'),
        '--output',
        markdownPath,
        '--md',
      ], workDir);
      commands.push(markdownGenerate);
      check(checks, 'independent Markdown generation succeeds', markdownGenerate.status === 0,
        markdownGenerate.stderr || markdownGenerate.stdout);
    }
  }

  const outputPath = join(workDir, expectations.independentOutput);
  const html = existsSync(outputPath) ? readFileSync(outputPath, 'utf8') : '';
  check(checks, 'independent HTML exists', Boolean(html));
  check(checks, 'HTML has doctype', /^<!doctype html>/i.test(html.trimStart()));
  check(checks, 'reference title is applied in title and H1',
    new RegExp(`<title[^>]*>\\s*${expectations.requiredTitle}\\s*</title>`, 'i').test(html) &&
    new RegExp(`<h1[^>]*>\\s*${expectations.requiredTitle}\\s*</h1>`, 'i').test(html));
  for (const entry of expectations.probeEntries) {
    const escapedEntry = entry.replace(/[&<>"']/g, (character) => ({
      '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
    })[character]);
    check(checks, `HTML lists ${entry}`, html.includes(escapedEntry), escapedEntry);
  }
  check(checks, 'filesystem text is HTML escaped',
    html.includes('README &amp; notes.txt') && !html.includes('README & notes.txt'));
  for (const mime of expectations.expectedImageMimeTypes)
    check(checks, `HTML embeds ${mime}`, html.includes(`data:${mime};base64,`));
  check(checks, 'HTML has no external HTTP assets',
    !/(?:src|href)\s*=\s*["']https?:\/\//i.test(html));
  check(checks, 'HTML output is non-trivial', Buffer.byteLength(html) > 1000,
    `${Buffer.byteLength(html)} bytes`);

  if (expectations.independentMarkdownOutput) {
    const markdownPath = join(workDir, expectations.independentMarkdownOutput);
    const markdown = existsSync(markdownPath) ? readFileSync(markdownPath, 'utf8') : '';
    check(checks, 'agent Markdown smoke exists',
      existsSync(join(workDir, expectations.agentMarkdownOutput)));
    check(checks, 'independent Markdown exists', Boolean(markdown));
    check(checks, 'Markdown has required title',
      markdown.trimStart().startsWith(`# ${expectations.requiredTitle}`));
    for (const entry of expectations.probeEntries)
      check(checks, `Markdown lists ${entry}`, markdown.includes(entry), entry);
    check(checks, 'Markdown identifies entry kinds', /\bfile\b/i.test(markdown) &&
      /\bdirector(?:y|ies)\b/i.test(markdown));
    check(checks, 'Markdown includes byte sizes',
      /\b\d+\s*(?:B|bytes?)\b/i.test(markdown));
    check(checks, 'Markdown is not HTML',
      !/<(?:!doctype|html|body|img)\b/i.test(markdown));
    check(checks, 'Markdown output is non-trivial', Buffer.byteLength(markdown) > 200,
      `${Buffer.byteLength(markdown)} bytes`);
  }

  return {
    checks,
    ok: checks.every((entry) => entry.ok),
    commands,
    package: {
      runtimeDependencies: runtimeNames,
      developmentDependencies: Object.keys(devDependencies).sort(),
      directDependencyCount,
      builtEntry: relative(workDir, builtEntry).replaceAll('\\', '/'),
    },
    projectFiles,
    outputBytes: Buffer.byteLength(html),
  };
}

export function collectCodingQualUsage(round) {
  const usage = round?.communication?.main_agent?.llm_usage || {};
  const roundUsage = Array.isArray(usage.llm_rounds)
    ? usage.llm_rounds.map((entry) => entry?.usage || {}) : [];
  const cachedInputTokens = roundUsage.reduce(
    (sum, entry) => sum + Number(entry.input_tokens_details?.cached_tokens || 0), 0);
  const cacheWriteTokens = roundUsage.reduce(
    (sum, entry) => sum + Number(entry.input_tokens_details?.cache_write_tokens || 0), 0);
  const inputTokens = Number(usage.input_tokens || usage.prompt_tokens || 0);
  return {
    inputTokens,
    cachedInputTokens,
    cacheWriteTokens,
    uncachedInputTokens: Math.max(0, inputTokens - cachedInputTokens),
    outputTokens: Number(usage.output_tokens || usage.completion_tokens || 0),
    totalTokens: Number(usage.total_tokens || 0),
    modelCost: Number(usage.model_cost || usage.cost || 0),
    iterations: Number(round?.communication?.main_agent?.iterations || 0),
    wallMs: Math.round(Number(round?.ms || 0) * 100) / 100,
  };
}

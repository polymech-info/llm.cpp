/**
 * Seed synthetic cross-session memory for recall benchmarks.
 *
 * Uses tanit-security `session-store memory-put` so documents land in the same
 * secure session-memory store the agent harness reads via PM_IMAGE_SESSION_DIR.
 */
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

import { securityExePath } from '../../../../tests/orchestrator/test-commons.js';

const TOPICS = [
  'family trip packing list',
  'grocery shopping notes',
  'photo edit color grade',
  'meeting follow-up actions',
  'recipe spice substitutions',
  'travel booking confirmation',
  'project milestone checklist',
  'garden watering schedule',
];

function parseJson(text) {
  const s = String(text || '').trim();
  if (!s) return null;
  try { return JSON.parse(s); } catch { return null; }
}

function runSec(exe, args, env, timeout = 30_000) {
  const r = spawnSync(exe, ['--json', ...args], {
    encoding: 'utf8',
    timeout,
    env: { ...process.env, ...env },
  });
  return { status: r.status, stdout: r.stdout, stderr: r.stderr, json: parseJson(r.stdout) };
}

function isoOffset(baseMs, index) {
  return new Date(baseMs + index * 1000).toISOString().replace(/\.\d{3}Z$/, 'Z');
}

/**
 * @param {{
 *   sessionDir: string,
 *   configDir: string,
 *   workDir: string,
 *   runId: string,
 *   totalItems?: number,
 *   itemsPerSession?: number,
 *   env?: Record<string, string>,
 *   securityExe?: string,
 * }} opts
 */
export function seedMemoryCorpus(opts) {
  const {
    sessionDir,
    configDir,
    workDir,
    runId,
    totalItems = 200,
    itemsPerSession = 50,
    env = {},
    securityExe = securityExePath(),
  } = opts;

  if (!securityExe) {
    throw new Error('tanit-security executable not found (build with: npm run build:cpp:security)');
  }

  const seedDir = join(workDir, 'recall-bench-seed');
  mkdirSync(seedDir, { recursive: true });

  const needles = [
    {
      id: 'needle_alpha',
      token: `NEEDLE_ALPHA_${runId}`,
      paraphrase: 'secret vacation beach codeword from last year',
      query: 'What was our secret vacation beach codeword?',
    },
    {
      id: 'needle_beta',
      token: `NEEDLE_BETA_${runId}`,
      paraphrase: 'preferred export color profile for client deliverables',
      query: 'Which export color profile should I use for client deliverables?',
    },
    {
      id: 'needle_gamma',
      token: `NEEDLE_GAMMA_${runId}`,
      paraphrase: 'wifi guest password for the studio loft',
      query: 'Remind me of the studio loft guest wifi password.',
    },
  ];

  const sessionCount = Math.max(1, Math.ceil(totalItems / itemsPerSession));
  const baseMs = Date.UTC(2026, 0, 1, 12, 0, 0);
  const sessions = [];
  let remaining = totalItems;
  let globalIndex = 0;

  // Plant needles in early donor sessions so they are not "recent" in the probe session.
  const needleOwners = needles.map((n, i) => ({
    ...n,
    sessionId: `${runId}-donor-${String((i % sessionCount) + 1).padStart(3, '0')}`,
  }));

  for (let s = 0; s < sessionCount; s++) {
    const sessionId = `${runId}-donor-${String(s + 1).padStart(3, '0')}`;
    const count = Math.min(itemsPerSession, remaining);
    remaining -= count;
    const items = {};

    for (let i = 0; i < count; i++) {
      const topic = TOPICS[(globalIndex + i) % TOPICS.length];
      const id = `noise_${String(globalIndex + i).padStart(5, '0')}`;
      const created = isoOffset(baseMs, globalIndex + i);
      items[id] = {
        id,
        kind: 'note',
        role: 'user_fact',
        summary: `${topic} entry ${globalIndex + i}: routine distractor without unique tokens`,
        created_at: created,
        last_used_at: created,
        use_count: 1,
        metadata: { topic, bench: true },
      };
    }

    for (const needle of needleOwners.filter((n) => n.sessionId === sessionId)) {
      const created = isoOffset(baseMs, 50_000 + globalIndex);
      items[needle.id] = {
        id: needle.id,
        kind: 'preference',
        role: 'user_fact',
        summary: `${needle.paraphrase}. Canonical token ${needle.token}.`,
        created_at: created,
        last_used_at: created,
        use_count: 1,
        metadata: { token: needle.token, bench: true, needle: true },
      };
    }

    const doc = {
      id: sessionId,
      title: `recall-bench donor ${s + 1}`,
      created_at: isoOffset(baseMs, s),
      updated_at: isoOffset(baseMs, s + count),
      memory_state: { items },
      events: [],
    };
    const file = join(seedDir, `${sessionId}.json`);
    writeFileSync(file, JSON.stringify(doc, null, 2), 'utf8');

    const put = runSec(
      securityExe,
      ['--config-dir', configDir, 'session-store', 'memory-put', '--id', sessionId, '--file', file],
      { ...env, PM_IMAGE_SESSION_DIR: sessionDir },
    );
    if (put.json?.ok !== true) {
      throw new Error(
        `memory-put failed for ${sessionId}: status=${put.status} err=${put.json?.error || put.stderr}`,
      );
    }
    sessions.push({ sessionId, itemCount: Object.keys(items).length, file });
    globalIndex += count;
  }

  const query = runSec(
    securityExe,
    ['--config-dir', configDir, 'session-store', 'query'],
    { ...env, PM_IMAGE_SESSION_DIR: sessionDir },
  );

  return {
    totalItems,
    sessionCount,
    sessions,
    needles: needleOwners,
    memoryItemCount: Number(query.json?.memoryItemCount || 0),
    queryOk: query.json?.ok === true,
    queryError: query.json?.error || null,
  };
}

export function parseBenchSize(argv = process.argv.slice(2)) {
  const raw = argv.find((a) => a.startsWith('--bench-size='))?.slice('--bench-size='.length)
    || (argv.includes('--bench-size') ? argv[argv.indexOf('--bench-size') + 1] : null);
  if (!raw) return 1000; // plan default (~1k)
  const n = Number(raw);
  if (!Number.isFinite(n) || n < 50) return 1000;
  return Math.floor(n);
}

export const RECALL_RICHNESS_PRESETS = {
  concise: { PM_LLM_RECALL_TOP_K: '3', PM_LLM_RECALL_INJECT_CHARS: '800' },
  balanced: { PM_LLM_RECALL_TOP_K: '5', PM_LLM_RECALL_INJECT_CHARS: '1500' },
  rich: { PM_LLM_RECALL_TOP_K: '8', PM_LLM_RECALL_INJECT_CHARS: '2500' },
  legacy_pool50: {
    PM_LLM_RECALL_TOP_K: '5',
    PM_LLM_RECALL_INJECT_CHARS: '1500',
    PM_LLM_RECALL_POOL: '50',
  },
};

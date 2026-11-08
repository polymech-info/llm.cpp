import { existsSync } from 'node:fs';
import { basename, resolve } from 'node:path';
import { cliExePath } from '../../../../tests/orchestrator/test-commons.js';

/** Tanit CLI, or pm-llm when PM_LLM_EXE is set. */
export function llmExe() {
  const fromEnv = process.env.PM_LLM_EXE;
  if (fromEnv && fromEnv.trim() && existsSync(fromEnv.trim()))
    return resolve(fromEnv.trim());
  return cliExePath();
}

export function isPmLlm(exe) {
  return /^pm-llm(\.exe)?$/i.test(basename(String(exe || '')));
}

/** Tanit argv is `llm agent …`. pm-llm argv is `agent …` or `serve …`. */
export function prepareCliArgs(exe, args) {
  if (!isPmLlm(exe))
    return args;
  const out = [];
  for (let i = 0; i < args.length; i++) {
    if (args[i] === 'llm' && args[i + 1] === 'agent') {
      const rest = args.slice(i + 2);
      const serveAt = rest.indexOf('--serve');
      if (serveAt >= 0) {
        out.push('serve', ...rest.filter((arg) => arg !== '--serve'));
      } else {
        out.push('agent', ...rest);
      }
      break;
    }
    out.push(args[i]);
  }
  return out;
}

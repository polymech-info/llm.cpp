import { existsSync } from 'node:fs';
import { basename, resolve } from 'node:path';
import { cliExePath } from '@polymech/tanit/tests/orchestrator/test-commons.js';

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

/** pm-llm registers the same `llm agent` tree as tanit-cli, including `--serve`. */
export function prepareCliArgs(exe, args) {
  return args;
}

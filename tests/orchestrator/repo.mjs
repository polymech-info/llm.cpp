import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

/** pixlwiz checkout that owns dist/, stemp/, and tests/. */
export const REPO = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..', '..', '..');

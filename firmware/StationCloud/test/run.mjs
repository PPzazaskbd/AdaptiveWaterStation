// Compiles core_test.cpp with g++, runs it, and checks every JSON line it prints with demo/contract.js,
// the same checker the cloud demo applies before storing a message.
// Run from the repo root:  node firmware/StationCloud/test/run.mjs
import { execFileSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const { validate } = await import(pathToFileURL(join(here, '../../../demo/contract.js')));

const exe = join(mkdtempSync(join(tmpdir(), 'stationcloud-')), process.platform === 'win32' ? 'core_test.exe' : 'core_test');
execFileSync('g++', ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1', join(here, 'core_test.cpp'), '-o', exe], { stdio: 'inherit' });
const out = execFileSync(exe, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'inherit'] });

let bad = 0;
const lines = out.trim().split(/\r?\n/);
for (const line of lines) {
  try {
    const m = JSON.parse(line);
    validate(m, Date.parse('2026-11-15T18:03:00Z'));
    console.log(`ok  ${m.node_id} seq ${m.seq} ${m.event?.type ?? 'sample'}`);
  } catch (error) {
    bad++;
    console.log(`BAD ${error.message}\n    ${line}`);
  }
}
console.log(bad ? `${bad} of ${lines.length} messages rejected` : `all ${lines.length} messages pass demo/contract.js`);
process.exit(bad ? 1 : 0);

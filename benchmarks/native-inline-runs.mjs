// Run: node benchmarks/native-inline-runs.mjs
// The old full scan and current range selection run in the same release binary.
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { arch, cpus, platform, tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const cpp = resolve(dirname(fileURLToPath(import.meta.url)), '../src/cpp');
const source = join(cpp, 'tests/bench_inline_runs.cpp');
const directory = mkdtempSync(join(tmpdir(), 'downstream-inline-bench-'));
const compiler = process.env.CXX ?? 'c++';
const flags = ['-std=c++20', '-O3', '-DNDEBUG', '-Wall', '-Wextra', '-Werror'];
try {
  const binary = join(directory, 'bench_inline_runs');
  execFileSync(compiler, [...flags, `-I${cpp}`, source, '-o', binary]);
  const workloads = JSON.parse(execFileSync(binary, { encoding: 'utf8' }));
  const result = {
    measuredAt: new Date().toISOString(),
    scope:
      'inline run selection only; excludes native layout, allocations and paint',
    baseline: 'PR #18 full row scan reproduced in the same binary',
    host: { platform: platform(), arch: arch(), cpu: cpus()[0]?.model },
    compiler: execFileSync(compiler, ['--version'], {
      encoding: 'utf8',
    }).trim(),
    flags,
    harnessSha256: createHash('sha256')
      .update(readFileSync(source))
      .digest('hex'),
    workloads,
  };
  process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
} finally {
  rmSync(directory, { recursive: true, force: true });
}

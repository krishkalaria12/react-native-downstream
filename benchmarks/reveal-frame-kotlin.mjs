// Production Kotlin frame CPU benchmark. Host stubs record interpolation calls;
// these results are not Android frame-rate or rasterization measurements.
// --baseline REF compiles the two production files from a git ref for comparison.
import { execFileSync } from 'node:child_process';
import { mkdtempSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const build = mkdtempSync(join(tmpdir(), 'downstream-reveal-bench-'));
const compiler = process.env.KOTLIN_HOME
  ? join(process.env.KOTLIN_HOME, 'bin', 'kotlinc')
  : 'kotlinc';
const baselineIndex = process.argv.indexOf('--baseline');
const baseline =
  baselineIndex >= 0 ? process.argv[baselineIndex + 1] : undefined;
try {
  const sources = ['Reveal.kt', 'RevealCurve.kt'].map((name) => {
    const relative = `src/android/src/main/java/com/downstream/${name}`;
    if (!baseline) return join(root, relative);
    const path = join(build, name);
    writeFileSync(
      path,
      execFileSync('git', ['show', `${baseline}:${relative}`], { cwd: root })
    );
    return path;
  });
  const stubs = join(root, 'tests/android/reveal-stubs');
  execFileSync(
    compiler,
    [
      '-nowarn',
      '-include-runtime',
      '-d',
      join(build, 'bench.jar'),
      ...sources,
      join(root, 'benchmarks/RevealFrameBench.kt'),
      ...readdirSync(stubs)
        .filter((name) => name.endsWith('.kt'))
        .map((name) => join(stubs, name)),
    ],
    { stdio: 'inherit' }
  );
  execFileSync(
    'java',
    ['-cp', join(build, 'bench.jar'), 'com.downstream.RevealFrameBenchKt'],
    { stdio: 'inherit' }
  );
} finally {
  rmSync(build, { recursive: true, force: true });
}

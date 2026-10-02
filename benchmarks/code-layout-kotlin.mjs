// Real Kotlin line-index helpers compared with their PR #19 implementations.
// Requires kotlinc and Java, like pnpm test:kotlin. Outputs raw JSON samples.
import { execFileSync } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const build = mkdtempSync(join(tmpdir(), 'downstream-code-layout-bench-'));
const compiler = process.env.KOTLIN_HOME
  ? join(process.env.KOTLIN_HOME, 'bin', 'kotlinc')
  : 'kotlinc';
try {
  execFileSync(
    compiler,
    [
      '-include-runtime',
      '-d',
      join(build, 'bench.jar'),
      join(root, 'src/android/src/main/java/com/downstream/CodeLayout.kt'),
      join(root, 'benchmarks/CodeLayoutBench.kt'),
    ],
    { stdio: 'inherit' }
  );
  execFileSync(
    'java',
    ['-cp', join(build, 'bench.jar'), 'com.downstream.CodeLayoutBenchKt'],
    { stdio: 'inherit' }
  );
} finally {
  rmSync(build, { recursive: true, force: true });
}

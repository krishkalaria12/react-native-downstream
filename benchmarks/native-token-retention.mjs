// Highlighted retained-snapshot RSS and payload copies, using identical release
// harnesses in fresh processes. --build-only does not run measurements.
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { arch, cpus, loadavg, platform } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

process.env.DOWNSTREAM_ENGINE_RELEASE = '1';
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const source = join(root, 'benchmarks/stream-token-retention.cpp');
const compareRoot = process.env.DOWNSTREAM_BENCH_COMPARE_ROOT;
const buildOnly = process.argv.includes('--build-only');

async function build(candidateRoot) {
  const { CPP, ENGINE, MD4C_SOURCES, prepareTargets, compile } = await import(
    pathToFileURL(join(candidateRoot, 'src/cpp/tests/build.mjs')).href
  );
  const sources = [
    source,
    ...ENGINE,
    ...MD4C_SOURCES,
    join(CPP, 'session/IncrementalParser.cpp'),
  ];
  await prepareTargets({ token_retention: sources });
  return compile(sources, 'token_retention');
}

const candidates = {};
if (compareRoot) candidates.baseline = await build(resolve(compareRoot));
candidates.current = await build(root);
const order = compareRoot
  ? ['baseline', 'current', 'current', 'baseline']
  : ['current'];
const loadBefore = loadavg();
const runs = [];
if (!buildOnly) {
  for (const mode of ['latest', 'retained']) {
    for (let round = 0; round < 2; round++) {
      for (const variant of order) {
        const workload = JSON.parse(
          execFileSync(candidates[variant], [mode], { encoding: 'utf8' })
        );
        runs.push({ round, variant, workload });
      }
    }
  }
  for (const mode of ['latest', 'retained']) {
    const checksums = runs
      .filter((run) => run.workload.mode === mode)
      .map((run) => run.workload.checksum);
    if (!checksums.every((value) => value === checksums[0]))
      throw new Error(`Token output differs for ${mode}`);
  }
}
process.stdout.write(
  `${JSON.stringify(
    {
      measuredAt: new Date().toISOString(),
      mode: buildOnly ? 'build-only' : 'host-token-retention',
      scope:
        'fresh host process peak RSS and parser/highlighter wall time before contiguous consumers or finish; 46,800 bytes of TypeScript in 5,200 complete 9-byte line updates; latest versus retaining every snapshot; token payload counter excludes persistent index metadata; RSS includes process overhead and allocator retention, not isolated live heap or device memory; two ABBA rounds per mode; correctness checks occur outside timing and RSS capture',
      compareRoot,
      sourceRoot: root,
      binaries: candidates,
      host: {
        platform: platform(),
        arch: arch(),
        cpu: cpus()[0]?.model,
        loadBefore,
        loadAfter: loadavg(),
      },
      compiler: execFileSync(process.env.CXX ?? 'c++', ['--version'], {
        encoding: 'utf8',
      }).trim(),
      harnessSha256: createHash('sha256')
        .update(readFileSync(source))
        .digest('hex'),
      runs,
    },
    null,
    2
  )}\n`
);

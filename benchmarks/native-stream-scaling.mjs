// Identical release harness, optional ABBA comparison, raw samples retained.
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { arch, cpus, loadavg, platform } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

process.env.DOWNSTREAM_ENGINE_RELEASE = '1';
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const source = join(root, 'benchmarks/stream-scaling.cpp');
const check = process.argv.includes('--check');
const compareRoot = process.env.DOWNSTREAM_BENCH_COMPARE_ROOT;

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
  await prepareTargets({ stream_scaling: sources });
  return compile(sources, 'stream_scaling');
}

const candidates = {};
if (compareRoot) candidates.baseline = await build(resolve(compareRoot));
candidates.current = await build(root);
const order = compareRoot
  ? ['baseline', 'current', 'current', 'baseline']
  : ['current'];
// Verification is outside every timed executable invocation.
for (const binary of Object.values(candidates))
  execFileSync(binary, ['--check']);
const loadBefore = loadavg();
const runs = order.map((variant) => ({
  variant,
  workloads: execFileSync(candidates[variant], check ? ['--check'] : [], {
    encoding: 'utf8',
  })
    .trim()
    .split('\n')
    .map((line) => JSON.parse(line)),
}));
process.stdout.write(
  `${JSON.stringify(
    {
      measuredAt: new Date().toISOString(),
      mode: check ? 'correctness-only' : 'timings',
      scope:
        'host synchronous parser/highlighter elapsed work; excludes finish, worker scheduling, native rendering and JS delivery; one warmup/five samples; live tokens may be explicitly provisional',
      compareRoot,
      sourceRoot: root,
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

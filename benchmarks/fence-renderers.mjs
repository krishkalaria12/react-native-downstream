// Paired production React host benchmark. Startups are outside measured work.
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { arch, cpus, loadavg, platform } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const sourceRoot = process.env.DOWNSTREAM_BENCH_SOURCE_ROOT ?? root;
const compareRoot = process.env.DOWNSTREAM_BENCH_COMPARE_ROOT;
const validateOnly = process.argv.includes('--check');
const loadBefore = loadavg();
const order = compareRoot
  ? ['baseline', 'optimized', 'optimized', 'baseline']
  : ['optimized'];
const runs = order.map((variant) => {
  let output;
  try {
    output = execFileSync(
      process.execPath,
      [
        join(root, 'node_modules/vitest/vitest.mjs'),
        'run',
        '--config',
        'benchmarks/fence-renderers.config.mts',
        '--disableConsoleIntercept',
      ],
      {
        cwd: root,
        env: {
          ...process.env,
          NODE_ENV: 'production',
          DOWNSTREAM_BENCH_SOURCE_ROOT:
            variant === 'baseline' ? compareRoot : sourceRoot,
          DOWNSTREAM_BENCH_CHECK: validateOnly ? '1' : '0',
        },
        stdio: 'pipe',
        encoding: 'utf8',
      }
    );
  } catch (error) {
    process.stderr.write(error.stdout?.toString() ?? '');
    process.stderr.write(error.stderr?.toString() ?? '');
    throw error;
  }
  const match = output.match(/DOWNSTREAM_FENCE_RESULTS=(\[[^\n]+\])/);
  if (!match) throw new Error('React benchmark emitted no results');
  return {
    variant,
    workloads: JSON.parse(match[1]).map((work) => ({
      ...work,
      fingerprint: createHash('sha256').update(work.fingerprint).digest('hex'),
    })),
  };
});
const first = runs[0].workloads;
for (const run of runs) {
  if (
    run.workloads.length !== first.length ||
    run.workloads.some(
      (work, i) =>
        work.name !== first[i].name || work.fingerprint !== first[i].fingerprint
    )
  ) {
    throw new Error('Candidate React output fingerprints differ');
  }
}
process.stdout.write(
  `${JSON.stringify(
    {
      measuredAt: new Date().toISOString(),
      mode: validateOnly ? 'correctness-only' : 'timings',
      scope:
        'production React DOM useFenceSlots and FenceSlots reconciliation with fresh synthetic native answers; sums synchronous 16-commit batches with task yields between batches; excludes scheduler waits, native parsing, serialization, layout and paint; includes slot-fixture allocation',
      sourceRoot,
      compareRoot,
      host: {
        platform: platform(),
        arch: arch(),
        cpu: cpus()[0]?.model,
        loadBefore,
        loadAfter: loadavg(),
      },
      node: process.version,
      harnessSha256: createHash('sha256')
        .update(
        readFileSync(join(root, 'benchmarks/fence-renderers.workload.tsx'))
        )
        .digest('hex'),
      runs,
    },
    null,
    2
  )}\n`
);

// One release harness, identical flags, ABBA order, and output fingerprints.
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { arch, cpus, loadavg, platform, tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const sourceRoot = process.env.DOWNSTREAM_BENCH_SOURCE_ROOT ?? root;
const compareRoot = process.env.DOWNSTREAM_BENCH_COMPARE_ROOT;
const validateOnly = process.argv.includes('--check');
const source = join(root, 'src/cpp/tests/bench_fence_slots.cpp');
const directory = mkdtempSync(join(tmpdir(), 'downstream-fence-slots-bench-'));
const compiler = process.env.CXX ?? 'c++';
const prefixes = [
  '-DMD4C_USE_UTF8',
  '-Dmd_parse=downstream_md_parse',
  '-Dentity_lookup=downstream_entity_lookup',
];
const flags = [
  '-std=c++20',
  '-O3',
  '-DNDEBUG',
  '-Wall',
  '-Wextra',
  '-Werror',
  '-pthread',
  ...prefixes,
];

function build(candidateRoot, name) {
  const cpp = join(candidateRoot, 'src/cpp');
  const objects = ['md4c.c', 'entity.c'].map((file) => {
    const object = join(directory, `${name}-${file}.o`);
    execFileSync(process.env.CC ?? 'cc', [
      '-std=c11',
      '-O3',
      '-DNDEBUG',
      ...prefixes,
      '-c',
      join(cpp, 'md4c', file),
      '-o',
      object,
    ]);
    return object;
  });
  const binary = join(directory, name);
  const workerCache = readFileSync(
    join(cpp, 'downstream/ControlledSlots.h'),
    'utf8'
  ).includes('resolvedFenceSlots(');
  execFileSync(compiler, [
    ...flags,
    ...(workerCache ? ['-DDOWNSTREAM_BENCH_RESOLVED_SLOTS'] : []),
    ...['', 'md4c', 'downstream', 'session', 'highlight', 'theme', 'math'].map(
      (path) => `-I${join(cpp, path)}`
    ),
    source,
    ...[
      'MarkdownParser.cpp',
      'Unicode.cpp',
      'LinkPolicy.cpp',
      'Scan.cpp',
      'MathScan.cpp',
      'Compare.cpp',
      'FenceMeta.cpp',
      'FenceSlots.cpp',
      'ControlledSlots.cpp',
      'Export.cpp',
      'ExportNames.cpp',
    ].map((file) => join(cpp, 'downstream', file)),
    ...objects,
    '-o',
    binary,
  ]);
  return { binary, workerCache };
}

try {
  const baseline = compareRoot ? build(compareRoot, 'baseline') : undefined;
  const optimized = build(sourceRoot, 'optimized');
  const candidates = { baseline, optimized };
  const loadBefore = loadavg();
  const order = baseline
    ? ['baseline', 'optimized', 'optimized', 'baseline']
    : ['optimized'];
  const runs = order.map((variant) => ({
    variant,
    workerCache: candidates[variant].workerCache,
    workloads: execFileSync(
      candidates[variant].binary,
      validateOnly ? ['--check'] : [],
      { encoding: 'utf8' }
    )
      .trim()
      .split('\n')
      .map((line) => JSON.parse(line)),
  }));
  const first = runs[0].workloads;
  for (const run of runs) {
    for (const work of run.workloads) {
      const many = work.name.match(/many_fences_(\d+)$/);
      const expectedSlots = many ? Number(many[1]) : 1;
      const expectedPending =
        work.name.startsWith('controlled_') && work.bytes > 16 * 1024;
      if (
        work.pending !== expectedPending ||
        work.slots !== (expectedPending ? 0 : expectedSlots)
      ) {
        throw new Error(`Unexpected fence fixture result: ${work.name}`);
      }
    }
    if (
      run.workloads.length !== first.length ||
      run.workloads.some(
        (work, i) =>
          work.name !== first[i].name ||
          work.fingerprint !== first[i].fingerprint
      )
    ) {
      throw new Error('Candidate output fingerprints differ');
    }
  }
  process.stdout.write(
    `${JSON.stringify(
      {
        measuredAt: new Date().toISOString(),
        mode: validateOnly ? 'correctness-only' : 'timings',
        scope:
          'host controlled-slot fallback, bare parse, and worker-call costs; excludes prepared-document cache lookup, native layout and paint; validation fingerprints outside timing',
        sourceRoot,
        compareRoot,
        host: {
          platform: platform(),
          arch: arch(),
          cpu: cpus()[0]?.model,
          loadBefore,
          loadAfter: loadavg(),
        },
        compiler: execFileSync(compiler, ['--version'], {
          encoding: 'utf8',
        }).trim(),
        flags,
        harnessSha256: createHash('sha256')
          .update(readFileSync(source))
          .digest('hex'),
        runs,
      },
      null,
      2
    )}\n`
  );
} finally {
  rmSync(directory, { recursive: true, force: true });
}

// Compare exact source roots with one release harness, in alternating order.
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { arch, cpus, loadavg, platform, tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const sourceRoot = process.env.DOWNSTREAM_BENCH_SOURCE_ROOT ?? root;
const compareRoot = process.env.DOWNSTREAM_BENCH_COMPARE_ROOT;
const source = join(root, 'src/cpp/tests/bench_inline_html.cpp');
const directory = mkdtempSync(join(tmpdir(), 'downstream-inline-html-bench-'));
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
  execFileSync(compiler, [
    ...flags,
    `-I${cpp}`,
    `-I${join(cpp, 'md4c')}`,
    `-I${join(cpp, 'downstream')}`,
    source,
    ...[
      'MarkdownParser.cpp',
      'Unicode.cpp',
      'LinkPolicy.cpp',
      'Scan.cpp',
      'MathScan.cpp',
      'Compare.cpp',
      'FenceMeta.cpp',
    ].map((file) => join(cpp, 'downstream', file)),
    join(cpp, 'remend/Preview.cpp'),
    ...objects,
    '-o',
    binary,
  ]);
  return binary;
}

try {
  const baseline = compareRoot ? build(compareRoot, 'baseline') : undefined;
  const current = build(sourceRoot, 'optimized');
  const loadBefore = loadavg();
  const runs = (
    baseline
      ? ['baseline', 'optimized', 'optimized', 'baseline']
      : ['optimized']
  ).map((variant) => ({
    variant,
    workloads: execFileSync(variant === 'baseline' ? baseline : current, {
      encoding: 'utf8',
    })
      .trim()
      .split('\n')
      .map((line) => JSON.parse(line)),
  }));
  const first = runs[0].workloads;
  for (const run of runs) {
    if (
      run.workloads.length !== first.length ||
      run.workloads.some(
        (workload, i) =>
          workload.name !== first[i].name ||
          workload.fingerprint !== first[i].fingerprint
      )
    ) {
      throw new Error('Candidate output fingerprints differ');
    }
  }
  process.stdout.write(
    `${JSON.stringify(
      {
        measuredAt: new Date().toISOString(),
        scope:
          'host inline HTML parse and preview including model fingerprinting; excludes native layout and paint',
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

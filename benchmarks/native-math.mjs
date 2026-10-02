// Run current sources or DOWNSTREAM_BENCH_SOURCE_ROOT with the same harness.
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { arch, cpus, loadavg, platform, tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const sourceRoot = process.env.DOWNSTREAM_BENCH_SOURCE_ROOT ?? root;
const compareRoot = process.env.DOWNSTREAM_BENCH_COMPARE_ROOT;
const source = join(root, 'src/cpp/tests/bench_math.cpp');
const ratex = join(root, 'math/vendor/host/libratex_ffi.a');
const directory = mkdtempSync(join(tmpdir(), 'downstream-math-bench-'));
const compiler = process.env.CXX ?? 'c++';
const cc = process.env.CC ?? 'cc';
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
const withRatex = platform() === 'darwin' && existsSync(ratex);
function build(candidateRoot, binaryName) {
  const cpp = join(candidateRoot, 'src', 'cpp');
  const includes = [`-I${cpp}`, `-I${join(cpp, 'md4c')}`];
  const objects = ['md4c.c', 'entity.c'].map((name) => {
    const object = join(
      directory,
      `${createHash('sha256')
        .update(candidateRoot)
        .digest('hex')
        .slice(0, 12)}-${name}.o`
    );
    execFileSync(cc, [
      '-std=c11',
      '-O3',
      '-DNDEBUG',
      ...prefixes,
      '-c',
      join(cpp, 'md4c', name),
      '-o',
      object,
    ]);
    return object;
  });
  const binary = join(directory, binaryName);
  execFileSync(compiler, [
    ...flags,
    ...includes,
    ...(withRatex ? ['-DDOWNSTREAM_BENCH_RATEX'] : []),
    source,
    ...[
      'MarkdownParser.cpp',
      'MathScan.cpp',
      'Unicode.cpp',
      'LinkPolicy.cpp',
      'Scan.cpp',
      'Compare.cpp',
      'FenceMeta.cpp',
    ].map((name) => join(cpp, 'downstream', name)),
    ...['MathEngine.cpp', 'MathDisplayList.cpp', 'MathLayout.cpp'].map((name) =>
      join(cpp, 'math', name)
    ),
    ...objects,
    ...(withRatex ? [ratex] : []),
    '-o',
    binary,
  ]);
  return binary;
}
function run(binary) {
  return execFileSync(binary, { encoding: 'utf8' })
    .trim()
    .split('\n')
    .map((line) => JSON.parse(line));
}
try {
  // Compile both candidates before timing; alternate their execution order.
  const baseline = compareRoot ? build(compareRoot, 'baseline') : undefined;
  const current = build(sourceRoot, 'current');
  const loadBefore = loadavg();
  const measurements = baseline
    ? {
        runs: ['baseline', 'optimized', 'optimized', 'baseline'].map(
          (variant) => ({
            variant,
            workloads: run(variant === 'baseline' ? baseline : current),
          })
        ),
      }
    : { workloads: run(current) };
  const result = {
    measuredAt: new Date().toISOString(),
    scope:
      '100 KiB bracket scan and complete parse; 5,000 repeated RaTeX calls when vendored; excludes native layout and paint',
    sourceRoot,
    ...(compareRoot ? { compareRoot } : {}),
    host: {
      platform: platform(),
      arch: arch(),
      cpu: cpus()[0]?.model,
      logicalCpus: cpus().length,
      loadBefore,
      loadAfter: loadavg(),
    },
    compiler: execFileSync(compiler, ['--version'], {
      encoding: 'utf8',
    }).trim(),
    flags,
    ratex: withRatex
      ? JSON.parse(readFileSync(join(root, 'math/ratex-version.json'), 'utf8'))
      : null,
    harnessSha256: createHash('sha256')
      .update(readFileSync(source))
      .digest('hex'),
    ...measurements,
  };
  process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
} finally {
  rmSync(directory, { recursive: true, force: true });
}

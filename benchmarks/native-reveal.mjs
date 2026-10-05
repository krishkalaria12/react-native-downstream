// Paired release builds of PR #20 and the working tree using the same harness.
// Run: node benchmarks/native-reveal.mjs
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import {
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { arch, cpus, platform, tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const cpp = join(root, 'src/cpp');
const source = join(cpp, 'tests/bench_reveal.cpp');
const baseline = process.env.REVEAL_BASELINE ?? '6d2aa70';
const directory = mkdtempSync(join(tmpdir(), 'downstream-reveal-bench-'));
const compiler = process.env.CXX ?? 'c++';
const flags = ['-std=c++20', '-O3', '-DNDEBUG', '-Wall', '-Wextra', '-Werror'];
try {
  const old = join(directory, 'baseline');
  mkdirSync(join(old, 'downstream'), { recursive: true });
  for (const name of ['Reveal.cpp', 'Reveal.h']) {
    writeFileSync(
      join(old, 'downstream', name),
      execFileSync('git', ['show', `${baseline}:src/cpp/downstream/${name}`], {
        cwd: root,
      })
    );
  }
  const binaries = {};
  for (const [label, engine] of [
    ['before', join(old, 'downstream/Reveal.cpp')],
    ['after', join(cpp, 'downstream/Reveal.cpp')],
  ]) {
    binaries[label] = join(directory, label);
    execFileSync(compiler, [
      ...flags,
      ...(label === 'before' ? [`-I${old}`] : []),
      `-I${cpp}`,
      `-I${cpp}/downstream`,
      source,
      engine,
      '-o',
      binaries[label],
    ]);
    execFileSync(binaries[label]);
  }
  const samples = { before: [], after: [] };
  for (let sample = 0; sample < 7; ++sample) {
    for (const label of sample % 2
      ? ['after', 'before']
      : ['before', 'after']) {
      samples[label].push(
        JSON.parse(execFileSync(binaries[label], { encoding: 'utf8' }))
      );
    }
  }
  const median = (values) =>
    [...values].sort((a, b) => a - b)[Math.floor(values.length / 2)];
  const workloads = samples.before[0].map((workload, index) => {
    const beforeMs = samples.before.map((sample) => sample[index].milliseconds);
    const afterMs = samples.after.map((sample) => sample[index].milliseconds);
    const checksums = [...samples.before, ...samples.after].map(
      (sample) => sample[index].checksum
    );
    const semanticChecksums = [...samples.before, ...samples.after].map(
      (sample) => sample[index].semanticChecksum
    );
    if (!semanticChecksums.every((value) => value === semanticChecksums[0]))
      throw new Error(`units or timings differ for ${workload.name}`);
    if (!checksums.every((value) => value === checksums[0]))
      throw new Error(`output differs for ${workload.name}`);
    return {
      ...workload,
      milliseconds: undefined,
      beforeMs,
      afterMs,
      speedup: median(beforeMs) / median(afterMs),
    };
  });
  process.stdout.write(
    `${JSON.stringify(
      {
        measuredAt: new Date().toISOString(),
        scope:
          'RevealTracker snapshot application and pruning, including per-pass tracker initialization/cleanup; excludes document snapshot construction, parsing, layout and drawing',
        baseline: execFileSync('git', ['rev-parse', baseline], {
          cwd: root,
          encoding: 'utf8',
        }).trim(),
        host: { platform: platform(), arch: arch(), cpu: cpus()[0]?.model },
        compiler: execFileSync(compiler, ['--version'], {
          encoding: 'utf8',
        }).trim(),
        flags,
        engineSha256: createHash('sha256')
          .update(readFileSync(join(cpp, 'downstream/Reveal.cpp')))
          .digest('hex'),
        harnessSha256: createHash('sha256')
          .update(readFileSync(source))
          .digest('hex'),
        workloads,
      },
      null,
      2
    )}\n`
  );
} finally {
  rmSync(directory, { recursive: true, force: true });
}

// Run: node benchmarks/native-incremental-brackets.mjs [--baseline <checkout>]
// A baseline checkout replaces IncrementalParser.cpp only, isolating its scan
// from other parser changes. All binaries use the same fixture and -O3 -DNDEBUG.
import { execFileSync } from 'node:child_process';
import { arch, cpus, platform } from 'node:os';
import { join, resolve } from 'node:path';

process.env.DOWNSTREAM_ENGINE_RELEASE = '1';
const { CPP, ENGINE, MD4C_SOURCES, compile, prepareTargets } =
  await import('../src/cpp/tests/build.mjs');
const args = process.argv.slice(2);
if (args.length !== 0 && (args.length !== 2 || args[0] !== '--baseline')) {
  throw new Error(
    'usage: node benchmarks/native-incremental-brackets.mjs [--baseline <checkout>]'
  );
}
const common = [
  join(CPP, 'tests', 'bench_incremental_brackets.cpp'),
  ...ENGINE,
  ...MD4C_SOURCES,
];
const targets = {
  optimized: [...common, join(CPP, 'session', 'IncrementalParser.cpp')],
};
if (args.length !== 0) {
  targets.baseline = [
    ...common,
    join(resolve(args[1]), 'src', 'cpp', 'session', 'IncrementalParser.cpp'),
  ];
}
await prepareTargets(targets);
const compiler = process.env.CXX ?? 'c++';
console.log(
  JSON.stringify({
    scope:
      'host C++ incremental parse and commit; excludes native layout and paint',
    baselineScope:
      'IncrementalParser.cpp only; all other engine sources are shared',
    host: { platform: platform(), arch: arch(), cpu: cpus()[0]?.model },
    compiler: execFileSync(compiler, ['--version'], {
      encoding: 'utf8',
    }).trim(),
    optimization: ['-O3', '-DNDEBUG'],
  })
);
// Alternate execution order over three rounds after every build has completed.
const order = targets.baseline ? ['baseline', 'optimized'] : ['optimized'];
for (let round = 0; round < 3; round++) {
  for (const name of round % 2 === 0 ? order : [...order].reverse()) {
    console.log(`round=${round} variant=${name}`);
    process.stdout.write(
      execFileSync(compile(targets[name], `brackets-${name}`), {
        encoding: 'utf8',
      })
    );
  }
}

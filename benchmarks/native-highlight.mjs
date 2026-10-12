// Release-mode host benchmark. Compare the same compiler, machine, and workload.
// Run: pnpm bench:native
import { execFile } from 'node:child_process';
import { mkdtemp, readdir, rm } from 'node:fs/promises';
import { availableParallelism, tmpdir } from 'node:os';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';

const run = promisify(execFile);
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const cpp = join(
  process.env.DOWNSTREAM_BENCH_SOURCE_ROOT ?? root,
  'src',
  'cpp'
);
const build = await mkdtemp(join(tmpdir(), 'downstream-native-bench-'));
const cc = process.env.CC ?? 'cc';
const cxx = process.env.CXX ?? 'c++';
const prefixes = [
  '-DMD4C_USE_UTF8',
  '-Dmd_parse=downstream_md_parse',
  '-Dentity_lookup=downstream_entity_lookup',
];
const flags = [
  '-std=c++20',
  '-O3',
  '-DNDEBUG',
  '-pthread',
  ...prefixes,
  '-DLexilla=downstream_lexilla',
  '-DScintilla=downstream_scintilla',
  ...[
    '',
    'downstream',
    'session',
    'remend',
    'highlight',
    'math',
    'md4c',
    'lexilla',
    'lexilla/lexlib',
  ].map((directory) => `-I${join(cpp, directory)}`),
];

// Bound compilation so a benchmark does not oversubscribe smaller CI hosts.
async function compileAll(sources) {
  let next = 0;
  let failed = false;
  const objects = new Array(sources.length);
  const results = await Promise.allSettled(
    Array.from({ length: Math.min(availableParallelism(), 4) }, async () => {
      while (!failed && next < sources.length) {
        const index = next++;
        const source = sources[index];
        const object = join(build, `${index}-${basename(source)}.o`);
        try {
          await run(source.endsWith('.c') ? cc : cxx, [
            ...(source.endsWith('.c')
              ? ['-std=c11', '-O3', '-DNDEBUG', ...prefixes]
              : flags),
            '-c',
            source,
            '-o',
            object,
          ]);
        } catch (error) {
          failed = true;
          throw error;
        }
        objects[index] = object;
      }
    })
  );
  const failure = results.find((result) => result.status === 'rejected');
  if (failure) throw failure.reason;
  return objects;
}

try {
  const lexilla = [];
  for (const directory of ['lexilla/lexers', 'lexilla/lexlib']) {
    const location = join(cpp, directory);
    for (const name of (await readdir(location)).sort()) {
      if (name.endsWith('.cxx')) lexilla.push(join(location, name));
    }
  }
  const sources = [
    ...lexilla,
    ...[
      'CodeBlocks.cpp',
      'CodeDocument.cpp',
      'CodeHighlighter.cpp',
      'LanguageRegistry.cpp',
      'LanguageTables.cpp',
    ].map((name) => join(cpp, 'highlight', name)),
    ...[
      'Unicode.cpp',
      'MarkdownParser.cpp',
      'Direction.cpp',
      'BidiTable.cpp',
      'LinkPolicy.cpp',
      'Scan.cpp',
      'Compare.cpp',
      'Export.cpp',
      'ExportNames.cpp',
      'FenceMeta.cpp',
      'MathScan.cpp',
    ].map((name) => join(cpp, 'downstream', name)),
    ...[
      'MathBlocks.cpp',
      'MathEngine.cpp',
      'MathDisplayList.cpp',
      'MathLayout.cpp',
      'MathSpeech.cpp',
    ].map((name) => join(cpp, 'math', name)),
    join(cpp, 'remend', 'Preview.cpp'),
    join(cpp, 'session', 'IncrementalParser.cpp'),
    join(cpp, 'md4c', 'md4c.c'),
    join(cpp, 'md4c', 'entity.c'),
  ];
  const objects = await compileAll(sources);
  const binary = join(build, 'bench_highlight');
  await run(cxx, [
    ...flags,
    join(cpp, 'tests', 'bench_highlight.cpp'),
    ...objects,
    '-o',
    binary,
  ]);
  const { stdout } = await run(binary, [], { maxBuffer: 1024 * 1024 });
  process.stdout.write(stdout);
} finally {
  await rm(build, { recursive: true, force: true });
}

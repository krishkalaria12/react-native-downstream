// Shared native test builds. Both suites use content-addressed objects.
import { execFile, execFileSync } from 'node:child_process';
import { createHash, randomUUID } from 'node:crypto';
import {
  existsSync,
  mkdirSync,
  readdirSync,
  readFileSync,
  realpathSync,
  renameSync,
  rmSync,
} from 'node:fs';
import { availableParallelism, tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';

export const CPP = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const MD4C = join(CPP, 'md4c');
const LEXILLA = join(CPP, 'lexilla');

/** Symbol renames, matching Downstream.podspec and the Android CMakeLists. */
const SYMBOL_PREFIXES = [
  '-Dmd_parse=downstream_md_parse',
  '-Dentity_lookup=downstream_entity_lookup',
];

/**
 * The vendored lexers rename their namespaces too, the way Lexilla and
 * Scintilla do to keep two copies in one binary apart.
 */
const LEXILLA_PREFIXES = [
  '-DLexilla=downstream_lexilla',
  '-DScintilla=downstream_scintilla',
];

const CFLAGS = ['-std=c11', '-O1', '-DMD4C_USE_UTF8', ...SYMBOL_PREFIXES];
const CXXFLAGS = [
  '-std=c++20',
  '-O1',
  '-Wall',
  '-Wextra',
  '-Werror',
  '-pthread',
  '-DMD4C_USE_UTF8',
  ...SYMBOL_PREFIXES,
  ...LEXILLA_PREFIXES,
  `-I${MD4C}`,
  `-I${CPP}`,
  `-I${join(CPP, 'downstream')}`,
  `-I${join(CPP, 'session')}`,
  `-I${join(CPP, 'remend')}`,
  `-I${join(CPP, 'highlight')}`,
  `-I${join(CPP, 'theme')}`,
  `-I${join(CPP, 'math')}`,
  `-I${LEXILLA}`,
  `-I${join(LEXILLA, 'lexlib')}`,
];

export const ENGINE = [
  ...[
    'MarkdownParser.cpp',
    'Unicode.cpp',
    'LinkPolicy.cpp',
    'Scan.cpp',
    'Compare.cpp',
    'Export.cpp',
    'ExportNames.cpp',
    'FenceMeta.cpp',
    'FenceSlots.cpp',
    'CodeLayout.cpp',
    'Reveal.cpp',
    'Caret.cpp',
    'ImageLayout.cpp',
    'ImageLoaderRegistry.cpp',
    'ImageSizeCache.cpp',
    'MathScan.cpp',
  ].map((name) => join(CPP, 'downstream', name)),
  ...[
    'MathLayout.cpp',
    'MathEngine.cpp',
    'MathDisplayList.cpp',
    'MathBlocks.cpp',
    'MathSpeech.cpp',
  ].map((name) => join(CPP, 'math', name)),
  join(CPP, 'remend', 'Preview.cpp'),
];

/**
 * The highlighter and the vendored lexers, which together are a library the
 * three binaries below link rather than sources they each recompile.
 */
export const HIGHLIGHT_SOURCES = [
  ...['lexers', 'lexlib'].flatMap((directory) =>
    readdirSync(join(LEXILLA, directory))
      .filter((name) => name.endsWith('.cxx'))
      .sort()
      .map((name) => join(LEXILLA, directory, name))
  ),
  ...[
    'CodeBlocks.cpp',
    'CodeDocument.cpp',
    'CodeHighlighter.cpp',
    'LanguageRegistry.cpp',
    'LanguageTables.cpp',
  ].map((name) => join(CPP, 'highlight', name)),
];

const binaries = new Map();
const objects = new Map();
const compilerKeys = new Map();
const execFileAsync = promisify(execFile);
let headersKey;

function buildRoot() {
  return (
    process.env.DOWNSTREAM_ENGINE_CACHE ??
    join(tmpdir(), 'downstream-engine-cache')
  );
}

function digest(value) {
  return createHash('sha256').update(value).digest('hex').slice(0, 20);
}

/** Hash headers once per process, rather than scanning them on every parse. */
function headerKey() {
  if (headersKey !== undefined) return headersKey;
  const hash = createHash('sha256');
  function visit(directory) {
    for (const entry of readdirSync(directory, { withFileTypes: true }).sort(
      (a, b) => a.name.localeCompare(b.name)
    )) {
      const path = join(directory, entry.name);
      if (entry.isDirectory()) visit(path);
      else if (/\.(h|hpp|hxx|inc)$/.test(entry.name)) {
        hash.update(path).update(readFileSync(path));
      }
    }
  }
  visit(CPP);
  headersKey = hash.digest('hex');
  return headersKey;
}

function compilerKey(compiler) {
  let key = compilerKeys.get(compiler);
  if (key === undefined) {
    key = JSON.stringify([
      realpathSync(execFileSync('which', [compiler]).toString().trim()),
      execFileSync(compiler, ['--version']).toString(),
      process.platform,
      process.arch,
      process.env.SDKROOT,
      process.env.CPATH,
      process.env.C_INCLUDE_PATH,
      process.env.CPLUS_INCLUDE_PATH,
      process.env.LIBRARY_PATH,
    ]);
    compilerKeys.set(compiler, key);
  }
  return key;
}

function compileCommand(compiler, args) {
  const launcher = process.env.DOWNSTREAM_COMPILER_LAUNCHER;
  return launcher && args.includes('-c')
    ? [launcher, [compiler, ...args]]
    : [compiler, args];
}

/** Publish complete files atomically when separate test runs share the cache. */
function compileInto(target, compiler, args) {
  const pending = `${target}.${process.pid}.${randomUUID()}`;
  try {
    const [command, commandArgs] = compileCommand(compiler, [
      ...args,
      '-o',
      pending,
    ]);
    execFileSync(command, commandArgs);
    renameSync(pending, target);
  } finally {
    rmSync(pending, { force: true });
  }
}

function objectBuild(source) {
  const c = source.endsWith('.c');
  const compiler = c ? process.env.CC ?? 'cc' : process.env.CXX ?? 'c++';
  const flags = c ? CFLAGS : CXXFLAGS;
  let target = objects.get(source);
  if (target === undefined) {
    const key = digest(
      JSON.stringify([
        source,
        digest(readFileSync(source)),
        headerKey(),
        flags,
        compilerKey(compiler),
      ])
    );
    target = join(buildRoot(), `${key}.o`);
    objects.set(source, target);
  }
  return { target, compiler, args: [...flags, '-c', source] };
}

function compileObject(source) {
  const { target, compiler, args } = objectBuild(source);
  if (!existsSync(target)) compileInto(target, compiler, args);
  return target;
}

/** A prebuilt archive, which is linked as it is rather than compiled. */
const isArchive = (source) => source.endsWith('.a');

/** Reuse the same engine objects in every binary and in ccache. */
export function compile(sources, name) {
  const cached = binaries.get(name);
  if (cached !== undefined) return cached;
  mkdirSync(buildRoot(), { recursive: true });
  const linkObjects = sources.map((source) =>
    isArchive(source) ? source : compileObject(source)
  );
  const archive = highlightArchive();
  const compiler = process.env.CXX ?? 'c++';
  const key = digest(
    JSON.stringify([linkObjects, archive, CXXFLAGS, compilerKey(compiler)])
  );
  const binary = join(buildRoot(), `${name}-${key}`);
  if (!existsSync(binary)) {
    compileInto(binary, compiler, [...CXXFLAGS, ...linkObjects, archive]);
  }
  binaries.set(name, binary);
  return binary;
}

function highlightArchive() {
  const linkObjects = HIGHLIGHT_SOURCES.map(compileObject);
  const archive = join(
    buildRoot(),
    `downstream-highlight-${digest(JSON.stringify(linkObjects))}.a`
  );
  if (!existsSync(archive)) {
    const pending = `${archive}.${process.pid}.${randomUUID()}`;
    try {
      execFileSync('ar', ['rcs', pending, ...linkObjects]);
      renameSync(pending, archive);
    } finally {
      rmSync(pending, { force: true });
    }
  }
  return archive;
}

export const MD4C_SOURCES = ['md4c.c', 'entity.c'].map((name) =>
  join(MD4C, name)
);
/** Compile missing objects once before test workers start. */
export async function prepareTargets(targets) {
  mkdirSync(buildRoot(), { recursive: true });
  const sources = [
    ...new Set([...Object.values(targets).flat(), ...HIGHLIGHT_SOURCES]),
  ].filter((source) => !isArchive(source));
  let next = 0;
  let failed = false;
  let failure;
  await Promise.allSettled(
    Array.from({ length: Math.min(4, availableParallelism()) }, async () => {
      try {
        while (!failed && next < sources.length) {
          const source = sources[next++];
          const { target, compiler, args } = objectBuild(source);
          if (existsSync(target)) continue;
          const pending = `${target}.${process.pid}.${randomUUID()}`;
          try {
            const [command, commandArgs] = compileCommand(compiler, [
              ...args,
              '-o',
              pending,
            ]);
            await execFileAsync(command, commandArgs);
            renameSync(pending, target);
          } finally {
            rmSync(pending, { force: true });
          }
        }
      } catch (error) {
        if (!failed) {
          failed = true;
          failure = error;
        }
        throw error;
      }
    })
  );
  if (failed) throw failure;
  for (const [name, targetSources] of Object.entries(targets)) {
    compile(targetSources, name);
  }
}

/* eslint-disable no-bitwise -- inline styles are a bitmask */
/**
 * The real C++ engine, compiled once and driven from tests.
 *
 * Every other test in this repository can assert on a recorded string or on a
 * mock's call log. This module is the only one that runs the parser that ships
 * in the app: it compiles `src/cpp` with the system compiler and exposes a
 * `parse` that returns the real render model, plus an `append` that keeps a
 * real incremental session, so a test can assert the text a user would see.
 *
 * Global setup builds shared objects before test workers start. Content keys
 * include headers, compiler identity, and flags, so later runs reuse the build
 * without executing stale code after a header or compiler change.
 */
import { execFile, execFileSync } from 'node:child_process';
import { createHash, randomUUID } from 'node:crypto';
import {
  existsSync,
  mkdirSync,
  readdirSync,
  readFileSync,
  renameSync,
  rmSync,
} from 'node:fs';
import { availableParallelism, tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';

const CPP = resolve(dirname(fileURLToPath(import.meta.url)), '../src/cpp');
const MD4C = join(CPP, 'md4c');
const LEXILLA = join(CPP, 'lexilla');

/** Symbol renames, matching Downstream.podspec and the Android CMakeLists. */
const SYMBOL_PREFIXES = [
  '-Dmd_parse=downstream_md_parse',
  '-Dentity_lookup=downstream_entity_lookup',
] as const;

/**
 * The vendored lexers rename their namespaces too, the way Lexilla and
 * Scintilla do to keep two copies in one binary apart.
 */
const LEXILLA_PREFIXES = [
  '-DLexilla=downstream_lexilla',
  '-DScintilla=downstream_scintilla',
] as const;

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
  `-I${LEXILLA}`,
  `-I${join(LEXILLA, 'lexlib')}`,
];

const ENGINE = [
  ...[
    'MarkdownParser.cpp',
    'Unicode.cpp',
    'LinkPolicy.cpp',
    'Scan.cpp',
    'Compare.cpp',
  ].map((name) => join(CPP, 'downstream', name)),
  join(CPP, 'remend', 'Preview.cpp'),
];

/**
 * The highlighter and the vendored lexers, which together are a library the
 * three binaries below link rather than sources they each recompile.
 */
const HIGHLIGHT_SOURCES = [
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

/** Block kinds, matching `Block::Kind` in the engine. */
export const KIND = {
  paragraph: 0,
  heading: 1,
  code: 2,
  html: 3,
  hr: 4,
  table: 5,
} as const;

/** Inline style bits, matching `InlineStyle` in the engine. */
export const STYLE = {
  strong: 1 << 0,
  emphasis: 1 << 1,
  strikethrough: 1 << 2,
  code: 1 << 3,
  link: 1 << 4,
  imageAlt: 1 << 5,
  footnote: 1 << 6,
} as const;

/** Container kinds, matching `Container::Kind`. */
export const CONTAINER = { quote: 0, listItem: 1 } as const;

/** Task states, matching `TaskState`. */
export const TASK = ['none', 'unchecked', 'checked'] as const;

/** An inline style span, as `[start, end, styleBits, linkIndex]` on the wire. */
export type InlineRun = [
  start: number,
  end: number,
  styles: number,
  link: number
];

/** A syntax token, as `[start, end, class, text]` on the wire. */
export type TokenRun = [
  start: number,
  end: number,
  token: string,
  text: string
];

export interface Block {
  kind: number;
  headingLevel: number;
  tableHeader: boolean;
  table: number;
  spacing: number;
  language: string;
  text: string;
  containers: {
    kind: number;
    id: number;
    showsMarker: boolean;
    task: number;
    marker: string;
  }[];
  runs: InlineRun[];
  tokens: TokenRun[];
  cells: [number, number, number][];
}

export interface Link {
  url: string;
  range: [number, number];
}

/** What the highlighter did over a session's passes. */
export interface HighlightCounters {
  blocks: number;
  resumes: number;
  complete: number;
  skipped: number;
  budget: number;
}

export interface RenderModel {
  blocks: Block[];
  links: Link[];
  error?: string;
  highlight?: HighlightCounters;
}

const binaries = new Map<string, string>();
const objects = new Map<string, string>();
const compilerKeys = new Map<string, string>();
const execFileAsync = promisify(execFile);
let headersKey: string | undefined;

function buildRoot(): string {
  return (
    process.env.DOWNSTREAM_ENGINE_CACHE ??
    join(tmpdir(), 'downstream-engine-cache')
  );
}

function digest(value: string | Buffer): string {
  return createHash('sha256').update(value).digest('hex').slice(0, 20);
}

/** Hash headers once per process, rather than scanning them on every parse. */
function headerKey(): string {
  if (headersKey !== undefined) return headersKey;
  const hash = createHash('sha256');
  function visit(directory: string): void {
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

function compilerKey(compiler: string): string {
  let key = compilerKeys.get(compiler);
  if (key === undefined) {
    key = JSON.stringify([
      compiler,
      execFileSync(compiler, ['--version']).toString(),
      process.platform,
      process.arch,
      process.env.PATH,
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

function compileCommand(compiler: string, args: string[]): [string, string[]] {
  const launcher = process.env.DOWNSTREAM_COMPILER_LAUNCHER;
  return launcher && args.includes('-c')
    ? [launcher, [compiler, ...args]]
    : [compiler, args];
}

/** Publish complete files atomically when separate test runs share the cache. */
function compileInto(target: string, compiler: string, args: string[]): void {
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

function objectBuild(source: string): {
  target: string;
  compiler: string;
  args: string[];
} {
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

function compileObject(source: string): string {
  const { target, compiler, args } = objectBuild(source);
  if (!existsSync(target)) compileInto(target, compiler, args);
  return target;
}

/** Reuse the same engine objects in every binary and in ccache. */
function compile(sources: string[], name: string): string {
  const cached = binaries.get(name);
  if (cached !== undefined) return cached;
  mkdirSync(buildRoot(), { recursive: true });
  const linkObjects = sources.map(compileObject);
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

function highlightArchive(): string {
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

const MD4C_SOURCES = ['md4c.c', 'entity.c'].map((name) => join(MD4C, name));
const TARGETS = {
  'downstream-dump': [
    join(CPP, 'tests', 'dump_document.cpp'),
    ...ENGINE,
    ...MD4C_SOURCES,
  ],
  'downstream-session': [
    join(CPP, 'tests', 'dump_session.cpp'),
    join(CPP, 'session', 'IncrementalParser.cpp'),
    ...ENGINE,
    ...MD4C_SOURCES,
  ],
  'downstream-incremental': [
    join(CPP, 'tests', 'test_incremental.cpp'),
    join(CPP, 'session', 'IncrementalParser.cpp'),
    join(CPP, 'downstream', 'SegmentPlan.cpp'),
    ...ENGINE,
    ...MD4C_SOURCES,
  ],
};

/** Vitest global setup runs once before workers can request native binaries. */
export default async function prepareEngineTests(): Promise<void> {
  mkdirSync(buildRoot(), { recursive: true });
  const sources = [
    ...new Set([...Object.values(TARGETS).flat(), ...HIGHLIGHT_SOURCES]),
  ];
  let next = 0;
  let failed = false;
  let failure: unknown;
  await Promise.allSettled(
    Array.from({ length: Math.min(4, availableParallelism()) }, async () => {
      try {
        while (!failed && next < sources.length) {
          const source = sources[next++]!;
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
  for (const [name, targetSources] of Object.entries(TARGETS)) {
    compile(targetSources, name);
  }
}

function engineBinary(): string {
  return compile(TARGETS['downstream-dump'], 'downstream-dump');
}

/** Feature bitmask for the real provisional-styling pass. */
export const PREVIEW_EMPHASIS = 1 << 0;
export const PREVIEW_INLINE_CODE = 1 << 1;
export const PREVIEW_LINKS = 1 << 2;
export const PREVIEW_IMAGES = 1 << 3;
export const PREVIEW_ALL =
  PREVIEW_EMPHASIS | PREVIEW_INLINE_CODE | PREVIEW_LINKS | PREVIEW_IMAGES;

function sessionBinary(): string {
  return compile(TARGETS['downstream-session'], 'downstream-session');
}

/** The parser strategies a native session can be created with. */
export type SessionMode = 'full' | 'incremental';

/** One step a native session's worker applies to its parser, in order. */
export type SessionOp =
  | { op: 'append'; chunk: string }
  | { op: 'replace'; source: string }
  | { op: 'preview'; features: number }
  | { op: 'parse'; finished: boolean };

function frame(op: string, payload: Buffer): Buffer {
  const header = Buffer.alloc(5);
  header.write(op, 0, 'latin1');
  header.writeUInt32LE(payload.length, 1);
  return Buffer.concat([header, payload]);
}

function encodeOp(step: SessionOp): Buffer {
  switch (step.op) {
    case 'append':
      return frame('a', Buffer.from(step.chunk, 'utf8'));
    case 'replace':
      return frame('r', Buffer.from(step.source, 'utf8'));
    case 'preview': {
      const mask = Buffer.alloc(4);
      mask.writeUInt32LE(step.features >>> 0);
      return frame('p', mask);
    }
    case 'parse':
      return frame(step.finished ? 'f' : 'u', Buffer.alloc(0));
  }
}

/**
 * Replays a session through the real `IncrementalParser` and returns the
 * document its last pass published.
 *
 * This is the parser the native worker owns, in the mode the stream asked for,
 * so the finish rule, the preview policy, and the commit path are the engine's
 * own rather than a restatement of them in the test.
 */
export function replaySession(
  mode: SessionMode,
  steps: readonly SessionOp[]
): RenderModel {
  return JSON.parse(
    execFileSync(sessionBinary(), [mode], {
      input: Buffer.concat(steps.map(encodeOp)),
      maxBuffer: 256 << 20,
    }).toString('utf8')
  );
}

/**
 * The first frame of a stream that previews: `source` arrives in one chunk of
 * an unfinished full-mode session with `features` enabled.
 */
export function preview(source: string, features: number): RenderModel {
  return replaySession('full', [
    { op: 'preview', features },
    { op: 'append', chunk: source },
    { op: 'parse', finished: false },
  ]);
}

function incrementalBinary(): string {
  return compile(TARGETS['downstream-incremental'], 'downstream-incremental');
}

/** Parses a complete document with the engine that ships in the app. */
export function parse(source: string | Buffer): RenderModel {
  const input =
    typeof source === 'string' ? Buffer.from(source, 'utf8') : source;
  return JSON.parse(
    execFileSync(engineBinary(), { input, maxBuffer: 256 << 20 }).toString(
      'utf8'
    )
  );
}

/**
 * Feeds chunks to the real incremental parser and checks every prefix against
 * a full parse. Throws if any prefix disagrees, so a caller can assert that a
 * document streamed in pieces renders exactly as the finished document would.
 */
export function assertStreamingMatchesFullParse(
  chunks: readonly string[]
): void {
  const frames = chunks.map((chunk) => {
    const length = Buffer.alloc(4);
    length.writeUInt32LE(Buffer.byteLength(chunk, 'utf8'));
    return Buffer.concat([length, Buffer.from(chunk, 'utf8')]);
  });
  execFileSync(incrementalBinary(), {
    input: Buffer.concat(frames),
    stdio: ['pipe', 'pipe', 'inherit'],
  });
}

/** Closes a document in the `--batch` wire format. */
const END_OF_DOCUMENT = 0xffffffff;

/**
 * Runs {@link assertStreamingMatchesFullParse} over many documents in one
 * process. Spawning a process per document would dominate a large corpus, so
 * the binary reads a separator-delimited stream when given `--batch`. Throws
 * on the first document that diverges, naming its index.
 */
export function assertStreamingBatchMatchesFullParse(
  documents: readonly (readonly string[])[]
): void {
  const frames: Buffer[] = [];
  for (const chunks of documents) {
    for (const chunk of chunks) {
      const length = Buffer.alloc(4);
      length.writeUInt32LE(Buffer.byteLength(chunk, 'utf8'));
      frames.push(length, Buffer.from(chunk, 'utf8'));
    }
    const end = Buffer.alloc(4);
    end.writeUInt32LE(END_OF_DOCUMENT);
    frames.push(end);
  }
  try {
    execFileSync(incrementalBinary(), ['--batch'], {
      input: Buffer.concat(frames),
      stdio: ['pipe', 'pipe', 'pipe'],
      maxBuffer: 256 << 20,
    });
  } catch (error) {
    // The binary names the failing document, so surface stderr rather than the
    // opaque non-zero exit.
    const stderr = (error as { stderr?: Buffer }).stderr?.toString('utf8');
    throw new Error(stderr || String(error));
  }
}

/** Joins the non-table block text, which is what a reader sees. */
export function visibleText(model: RenderModel): string {
  return model.blocks
    .filter((block) => block.kind !== KIND.table)
    .map((block) => block.text)
    .join('\n');
}

/** The style runs that overlap a substring of a block. */
export function stylesOf(block: Block, substring: string): InlineRun[] {
  const start = block.text.indexOf(substring);
  const end = start + substring.length;
  // `runs` crosses the boundary as a JSON tuple, so read it positionally.
  return block.runs.filter((run) => run[0] < end && run[1] > start);
}

export function hasStyle(
  block: Block,
  substring: string,
  style: number
): boolean {
  return stylesOf(block, substring).some((run) => (run[2] & style) !== 0);
}

/** The table cells of the first table, as text. */
export function tableRows(model: RenderModel): string[][] {
  return model.blocks
    .filter((block) => block.kind === KIND.table)
    .map((block) =>
      block.cells.map(([start, end]) => block.text.slice(start, end))
    );
}

/** The URLs the renderer would allow the user to open. */
export function linkUrls(model: RenderModel): string[] {
  return model.links.map((link) => link.url);
}

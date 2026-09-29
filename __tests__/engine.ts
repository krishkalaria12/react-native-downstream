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
 * Build cost is paid once per run and cached in a temp directory keyed by the
 * mtime of the sources, so a second run reuses the binary.
 */
import { execFileSync } from 'node:child_process';
import { createHash, randomUUID } from 'node:crypto';
import { existsSync, mkdirSync, renameSync, rmSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const CPP = resolve(dirname(fileURLToPath(import.meta.url)), '../src/cpp');
const MD4C = join(CPP, 'md4c');

/** Symbol renames, matching Downstream.podspec and the Android CMakeLists. */
const SYMBOL_PREFIXES = [
  '-Dmd_parse=downstream_md_parse',
  '-Dentity_lookup=downstream_entity_lookup',
] as const;

const CFLAGS = ['-std=c11', '-O1', '-DMD4C_USE_UTF8', ...SYMBOL_PREFIXES];
const CXXFLAGS = [
  '-std=c++20',
  '-O1',
  '-pthread',
  '-DMD4C_USE_UTF8',
  ...SYMBOL_PREFIXES,
  `-I${MD4C}`,
  `-I${CPP}`,
  `-I${join(CPP, 'downstream')}`,
  `-I${join(CPP, 'session')}`,
  `-I${join(CPP, 'remend')}`,
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
  cells: [number, number, number][];
}

export interface Link {
  url: string;
  range: [number, number];
}

export interface RenderModel {
  blocks: Block[];
  links: Link[];
  error?: string;
}

let cached: { binary: string; key: string } | undefined;

/** A directory for built binaries, stable across runs so work is reused. */
function buildRoot(): string {
  return join(tmpdir(), 'downstream-engine-cache');
}

function sourcesKey(targets: string[]): string {
  return targets
    .map((file) => {
      const stats = statSync(file);
      return `${file}:${stats.mtimeMs}:${stats.size}`;
    })
    .join('|');
}

/**
 * Runs a compiler and publishes the result at `target` atomically.
 *
 * Vitest runs test files in parallel processes and every one of them needs the
 * engine, so on a cold cache they all build it at once. Writing straight to
 * `target` would let a second process exec a half-written binary, which shows up
 * as a parse that returns nonsense rather than as a build error. The compiler
 * therefore writes to a name only this process uses, and the result is renamed
 * into place. Two processes building the same target both publish a complete
 * binary, so the last rename simply wins.
 */
function compileInto(target: string, compiler: string, args: string[]): void {
  const pending = `${target}.${process.pid}.${randomUUID()}`;
  try {
    execFileSync(compiler, [...args, '-o', pending]);
    renameSync(pending, target);
  } catch (error) {
    rmSync(pending, { force: true });
    throw error;
  }
}

function compile(
  sources: string[],
  name: string,
  flags: string[],
  linkObjects: string[]
): string {
  const key = sourcesKey([...sources, ...linkObjects]);
  if (cached?.key === key) return cached.binary;
  const root = buildRoot();
  mkdirSync(root, { recursive: true });
  // The key is part of the name, so the file existing proves it was built from
  // these sources. A separate stamp file could disagree with the binary it
  // describes if a process died between the two writes.
  const digest = createHash('sha256').update(key).digest('hex').slice(0, 16);
  const binary = join(root, `${name}-${digest}`);
  if (!existsSync(binary)) {
    compileInto(binary, process.env.CXX ?? 'c++', [
      ...flags,
      ...sources,
      ...linkObjects,
    ]);
  }
  cached = { binary, key };
  return binary;
}

function compileObjects(): string[] {
  const root = buildRoot();
  mkdirSync(root, { recursive: true });
  return ['md4c.c', 'entity.c'].map((source) => {
    const sourcePath = join(MD4C, source);
    // The source mtime is part of the name for the same reason it is part of a
    // binary's name: existence then proves the object is current.
    const object = join(
      root,
      `${source}-${String(statSync(sourcePath).mtimeMs)}.o`
    );
    if (!existsSync(object)) {
      compileInto(object, process.env.CC ?? 'cc', [
        ...CFLAGS,
        '-c',
        sourcePath,
      ]);
    }
    return object;
  });
}

function engineBinary(): string {
  const objects = compileObjects();
  return compile(
    [join(CPP, 'tests', 'dump_document.cpp'), ...ENGINE],
    'downstream-dump',
    CXXFLAGS,
    objects
  );
}

/** Feature bitmask for the real provisional-styling pass. */
export const PREVIEW_EMPHASIS = 1 << 0;
export const PREVIEW_INLINE_CODE = 1 << 1;
export const PREVIEW_ALL = PREVIEW_EMPHASIS | PREVIEW_INLINE_CODE;

function previewBinary(): string {
  const objects = compileObjects();
  return compile(
    [join(CPP, 'tests', 'dump_preview.cpp'), ...ENGINE],
    'downstream-preview',
    CXXFLAGS,
    objects
  );
}

/**
 * Parses a complete document through the real provisional-styling pass.
 *
 * This is the pass a session runs on its unstable tail. The whole document is
 * the tail here, because nothing has committed, so it is the same path.
 */
export function preview(
  source: string | Buffer,
  features: number
): RenderModel {
  const input =
    typeof source === 'string' ? Buffer.from(source, 'utf8') : source;
  return JSON.parse(
    execFileSync(previewBinary(), [String(features)], {
      input,
      maxBuffer: 256 << 20,
    }).toString('utf8')
  );
}

function incrementalBinary(): string {
  const objects = compileObjects();
  return compile(
    [
      join(CPP, 'tests', 'test_incremental.cpp'),
      join(CPP, 'session', 'IncrementalParser.cpp'),
      join(CPP, 'downstream', 'SegmentPlan.cpp'),
      ...ENGINE,
    ],
    'downstream-incremental',
    CXXFLAGS,
    objects
  );
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

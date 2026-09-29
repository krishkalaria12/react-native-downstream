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
import { existsSync, mkdtempSync, readFileSync, statSync } from 'node:fs';
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
  `-I${join(CPP, 'downstream')}`,
  `-I${join(CPP, 'session')}`,
];

const ENGINE = ['MarkdownParser.cpp', 'Unicode.cpp', 'LinkPolicy.cpp'];

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

/** List container kinds, matching `Container::Kind`. */
export const CONTAINER = { listItem: 1 } as const;

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

function compile(
  sources: string[],
  name: string,
  flags: string[],
  linkObjects: string[]
): string {
  const key = sourcesKey([...sources, ...linkObjects.map((o) => `${o}.stamp`)]);
  if (cached?.key === key) return cached.binary;
  const root = buildRoot();
  const binary = join(root, name);
  if (existsSync(binary) && statSync(binary).mtimeMs > 0) {
    // A binary from a previous run is reused when the sources are unchanged,
    // which the key proves only if the binary was written for this key. Keep a
    // stamp next to it so a rebuild is detected.
    const stamp = `${binary}.key`;
    if (existsSync(stamp) && readFileSync(stamp, 'utf8') === key) {
      cached = { binary, key };
      return binary;
    }
  }
  execFileSync(process.env.CXX ?? 'c++', [
    ...flags,
    ...sources,
    ...linkObjects,
    '-o',
    binary,
  ]);
  const { writeFileSync, mkdirSync } =
    require('node:fs') as typeof import('node:fs');
  mkdirSync(root, { recursive: true });
  writeFileSync(`${binary}.key`, key);
  cached = { binary, key };
  return binary;
}

function compileObjects(): string[] {
  const root = buildRoot();
  const { mkdirSync } = require('node:fs') as typeof import('node:fs');
  mkdirSync(root, { recursive: true });
  return ['md4c.c', 'entity.c'].map((source) => {
    const object = join(root, `${source}.o`);
    const sourcePath = join(MD4C, source);
    const stamp = `${object}.stamp`;
    if (
      existsSync(object) &&
      existsSync(stamp) &&
      readFileSync(stamp, 'utf8') === String(statSync(sourcePath).mtimeMs)
    ) {
      return object;
    }
    execFileSync(process.env.CC ?? 'cc', [
      ...CFLAGS,
      '-c',
      sourcePath,
      '-o',
      object,
    ]);
    const { writeFileSync } = require('node:fs') as typeof import('node:fs');
    writeFileSync(stamp, String(statSync(sourcePath).mtimeMs));
    return object;
  });
}

function engineBinary(): string {
  const objects = compileObjects();
  return compile(
    [
      join(CPP, 'tests', 'dump_document.cpp'),
      ...ENGINE.map((f) => join(CPP, 'downstream', f)),
    ],
    'downstream-dump',
    CXXFLAGS,
    objects
  );
}

function incrementalBinary(): string {
  const objects = compileObjects();
  return compile(
    [
      join(CPP, 'tests', 'test_incremental.cpp'),
      join(CPP, 'session', 'IncrementalParser.cpp'),
      join(CPP, 'downstream', 'SegmentPlan.cpp'),
      ...ENGINE.map((f) => join(CPP, 'downstream', f)),
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

export { mkdtempSync };

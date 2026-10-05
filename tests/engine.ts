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
import { execFileSync } from 'node:child_process';
import { join } from 'node:path';
import {
  CPP,
  ENGINE,
  MD4C_SOURCES,
  compile,
  prepareTargets,
} from '../src/cpp/tests/build.mjs';

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
  math: 1 << 7,
  superscript: 1 << 8,
  subscript: 1 << 9,
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
  /** The whole fence info string, which is where a download name can come from. */
  info: string;
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

/** An image the document records, as the drivers print them. */
export interface ImageRecord {
  url: string;
  /** Plain alt text, as a reader would see it. */
  alt: string;
  loadable: boolean;
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
  /** Every image the document records, whether or not a block names it. */
  images: ImageRecord[];
  /** Leading blocks no later input can change, as `Document::settledBlocks`. */
  settled: number;
  error?: string;
  highlight?: HighlightCounters;
}

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
  'downstream-export': [
    join(CPP, 'tests', 'dump_export.cpp'),
    join(CPP, 'downstream', 'SegmentPlan.cpp'),
    ...ENGINE,
    ...MD4C_SOURCES,
  ],
  'downstream-export-rules': [
    join(CPP, 'tests', 'test_export.cpp'),
    join(CPP, 'downstream', 'SegmentPlan.cpp'),
    ...ENGINE,
    ...MD4C_SOURCES,
  ],
  'downstream-fence-meta': [
    join(CPP, 'tests', 'test_fence_meta.cpp'),
    join(CPP, 'downstream', 'SegmentPlan.cpp'),
    ...ENGINE,
    ...MD4C_SOURCES,
  ],
};

/** Build the engine once before Vitest workers request native binaries. */
export default async function prepareEngineTests(): Promise<void> {
  await prepareTargets(TARGETS);
}

function engineBinary(): string {
  return compile(TARGETS['downstream-dump'], 'downstream-dump');
}

/** Feature bitmask for the real provisional-styling pass. */
export const PREVIEW_EMPHASIS = 1 << 0;
export const PREVIEW_INLINE_CODE = 1 << 1;
export const PREVIEW_LINKS = 1 << 2;
export const PREVIEW_IMAGES = 1 << 3;
export const PREVIEW_MATH = 1 << 4;
export const PREVIEW_HTML_TAGS = 1 << 5;
export const PREVIEW_ALL =
  PREVIEW_EMPHASIS |
  PREVIEW_INLINE_CODE |
  PREVIEW_LINKS |
  PREVIEW_IMAGES |
  PREVIEW_MATH |
  PREVIEW_HTML_TAGS;

/**
 * Bits of the parse-level `parseFlags` prop, mirroring `src/parseFlags.ts` and
 * `downstream::ParseFlags`.
 */
export const PARSE_ENABLED = 1 << 0;
export const PARSE_SINGLE_DOLLAR = 1 << 1;
export const PARSE_BRACKETS = 1 << 2;
/** `<br>`, `<sub>` and `<sup>` are recognized. On unless `inlineHtml` is false. */
export const PARSE_INLINE_HTML = 1 << 3;
export const PARSE_SUPERSCRIPT_SYNTAX = 1 << 4;
export const PARSE_SUBSCRIPT_SYNTAX = 1 << 5;
export const PARSE_FLAGS_DEFAULT = PARSE_ENABLED | PARSE_BRACKETS;

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
  steps: readonly SessionOp[],
  parseFlags = 0
): RenderModel {
  return JSON.parse(
    execFileSync(sessionBinary(), [mode, String(parseFlags)], {
      input: Buffer.concat(steps.map(encodeOp)),
      maxBuffer: 256 << 20,
    }).toString('utf8')
  );
}

/**
 * The first frame of a stream that previews: `source` arrives in one chunk of
 * an unfinished full-mode session with `features` enabled.
 */
export function preview(
  source: string,
  features: number,
  parseFlags = 0
): RenderModel {
  return replaySession(
    'full',
    [
      { op: 'preview', features },
      { op: 'append', chunk: source },
      { op: 'parse', finished: false },
    ],
    parseFlags
  );
}

function incrementalBinary(): string {
  return compile(TARGETS['downstream-incremental'], 'downstream-incremental');
}

/** Parses a complete document with the engine that ships in the app. */
export function parse(source: string | Buffer, parseFlags = 0): RenderModel {
  const input =
    typeof source === 'string' ? Buffer.from(source, 'utf8') : source;
  return JSON.parse(
    execFileSync(engineBinary(), parseFlags ? [String(parseFlags)] : [], {
      input,
      maxBuffer: 256 << 20,
    }).toString('utf8')
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

function exportBinary(): string {
  return compile(TARGETS['downstream-export'], 'downstream-export');
}

/** One `dump_export` operation's result. */
export interface ExportResult {
  op: 'e' | 'd' | 't' | 'r';
  text: string;
  /** Present on a `r` operation: the same document, re-parsed from the export. */
  reparsed?: RenderModel;
}

export type ExportOperation =
  | { op: 'e'; from: number; to: number }
  | { op: 'd'; block: number }
  | { op: 't'; extension: string };

/**
 * Serializes ranges of a source with the exporter that ships in the app.
 *
 * The engine tests in `src/cpp/tests/engine.test.mjs` cover the same rules
 * against a standalone binary; this exists so a suite in this directory can
 * assert on the real bytes without shelling out to that harness.
 */
export function exportSource(
  source: string,
  format: 'text' | 'csv' | 'tsv' | 'markdown' | 'html',
  operations: readonly ExportOperation[],
  options: { separator?: string; base?: string; fallback?: string } = {}
): { model: RenderModel; exports: ExportResult[] } {
  const separator = options.separator ?? ',';
  const base = options.base ?? '';
  const fallback = options.fallback ?? 'file';
  const args = [
    format,
    separator,
    base,
    fallback,
    ...operations.map((operation) =>
      operation.op === 'e'
        ? `e:${operation.from}:${operation.to}`
        : operation.op === 'd'
        ? `d:${operation.block}`
        : `t:${operation.extension}`
    ),
  ];
  const model: RenderModel & { exports?: ExportResult[] } = JSON.parse(
    execFileSync(exportBinary(), args, {
      input: Buffer.from(source, 'utf8'),
      maxBuffer: 256 << 20,
    }).toString('utf8')
  );
  return { model, exports: model.exports ?? [] };
}

/** The code text a copy button would put on the clipboard. */
export function codeText(source: string, block = 0): string {
  const { exports } = exportSource(source, 'text', [
    { op: 'e', from: block, to: block + 1 },
  ]);
  return exports[0]?.text ?? '';
}

/** The file name a code download saves as. */
export function codeDownloadName(
  source: string,
  block = 0,
  options: { base?: string; fallback?: string } = {}
): string {
  const { exports } = exportSource(
    source,
    'text',
    [{ op: 'd', block }],
    options
  );
  return exports[0]?.text ?? '';
}

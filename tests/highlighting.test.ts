/**
 * Syntax highlighting, exercised through the real C++ engine.
 *
 * The native suite in `src/cpp/tests` pins the tokens themselves, one golden
 * fixture per language. This file covers what a reader of the library can
 * observe from JavaScript: that a fenced block is colored, that highlighting
 * never changes the text a reader sees or copies, and that a document with no
 * code block stays plain.
 */
import { describe, expect, it } from 'vitest';

import {
  KIND,
  parse,
  replaySession,
  type SessionOp,
  visibleText,
} from './engine';

const fence = (info: string, code: string) =>
  '```' + info + '\n' + code + '```\n';

describe('fenced code', () => {
  it('colors a block whose language the engine knows', () => {
    const block = parse(fence('ts', 'const a: number = 1;\n')).blocks[0];
    expect(block?.kind).toBe(KIND.code);
    const classes = block?.tokens.map((token) => token[2]) ?? [];
    expect(classes).toContain('keyword');
    expect(classes).toContain('number');
  });

  it('matches each run text to its offsets', () => {
    const block = parse(fence('py', 'def greet(name):\n    return f"hi"\n'))
      .blocks[0];
    expect(block?.tokens.length).toBeGreaterThan(0);
    for (const [start, end, , text] of block?.tokens ?? []) {
      expect(block?.text.slice(start, end)).toBe(text);
    }
  });

  it('tells a call, a property, and a type apart', () => {
    const block = parse(fence('ts', 'loadUser(id);\nres.status;\n')).blocks[0];
    const runs = (block?.tokens ?? []).filter(
      ([, , token]) =>
        token === 'function' || token === 'property' || token === 'type'
    );
    expect(runs.map(([, , , text]) => text)).toEqual(['loadUser', 'status']);
  });

  it('leaves a block with an unknown language plain', () => {
    for (const info of ['brainfuck', '']) {
      expect(parse(fence(info, '+++++\n')).blocks[0]?.tokens).toEqual([]);
    }
  });

  it('leaves an indented code block plain, since it has no language', () => {
    const block = parse('    const a = 1;\n').blocks[0];
    expect(block?.kind).toBe(KIND.code);
    expect(block?.language).toBe('');
    expect(block?.tokens).toEqual([]);
  });

  it('ignores a meta string after the language', () => {
    const block = parse(fence('ts title="a.ts"', 'const a = 1;\n')).blocks[0];
    expect(block?.language).toBe('ts');
    expect(block?.tokens.length).toBeGreaterThan(0);
  });

  it('never changes the text, the links, or the inline runs', () => {
    const code = '// see https://example.com\nexport const a = 1;\n';
    const model = parse(fence('ts', code));
    const block = model.blocks[0];
    // Highlighting only sets colors, so a URL in code stays text, a code block
    // has no inline runs, and the visible text is what the writer sent.
    expect(block?.text).toBe(code.slice(0, -1));
    expect(model.links).toEqual([]);
    expect(block?.runs).toEqual([]);
    expect(visibleText(model)).toContain('export const a = 1;');
  });

  it('leaves prose off the highlighting path', () => {
    const model = parse(
      '# Title\n\nProse with `inline code` and a [link](https://x.dev).\n'
    );
    expect(model.blocks.every((block) => block.tokens.length === 0)).toBe(true);
  });
});

describe('streaming', () => {
  const code =
    [
      'export async function loadUser(id: number) {',
      '  const res = await fetch(`/api/users/${id}`);',
      '  return res.json();',
      '}',
    ].join('\n') + '\n';
  const source = fence('ts', code);

  it('colors an open fence as it streams', () => {
    const open = replaySession('incremental', [
      { op: 'append', chunk: source.slice(0, 40) },
      { op: 'parse', finished: false },
    ]);
    const block = open.blocks[0];
    expect(block?.kind).toBe(KIND.code);
    // The block is still growing, so it ends mid-line and has no fence yet.
    expect(block?.text).toBe('export async function loadUser(id:');
    expect(block?.tokens.length).toBeGreaterThan(0);
  });

  it('ends with the tokens a one-shot parse of the finished text gives', () => {
    const streamed = replaySession('incremental', [
      { op: 'append', chunk: source.slice(0, 17) },
      { op: 'append', chunk: source.slice(17, 63) },
      { op: 'append', chunk: source.slice(63) },
      { op: 'parse', finished: true },
    ]);
    const settled = parse(source);
    expect(streamed.blocks[0]?.tokens).toEqual(settled.blocks[0]?.tokens);
    expect(streamed.blocks[0]?.text).toBe(settled.blocks[0]?.text);
  });

  it('agrees with a full-mode session and with a one-shot parse', () => {
    const full = replaySession('full', [
      { op: 'append', chunk: source },
      { op: 'parse', finished: true },
    ]);
    expect(full.blocks[0]?.tokens).toEqual(parse(source).blocks[0]?.tokens);
  });

  it('resumes a growing block instead of coloring it again', () => {
    // A pass on an open fence colors only its last line, so the resume count
    // grows with the number of passes while the work per pass stays flat.
    const steps: SessionOp[] = [];
    for (let at = 1; at < source.length; at += 9) {
      steps.push(
        ...[
          { op: 'append' as const, chunk: source.slice(at - 1, at + 8) },
          { op: 'parse' as const, finished: false },
        ]
      );
    }
    const streamed = replaySession('incremental', [
      ...steps,
      { op: 'parse', finished: true },
    ]);
    const counters = streamed.highlight;
    expect(counters?.blocks).toBeGreaterThan(0);
    expect(counters?.resumes).toBeGreaterThan(0);
    expect(counters?.complete).toBeGreaterThan(0);
    expect(streamed.blocks[0]?.tokens).toEqual(parse(source).blocks[0]?.tokens);
  });
});

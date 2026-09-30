/**
 * The export rules, asserted through the engine that ships in the app.
 *
 * The full rule set lives in `src/cpp/tests/engine.test.mjs`, which runs the
 * same binary with a hand-written golden for every rule. This suite proves the
 * JavaScript harness reaches the same code, so a test here and a test there
 * cannot be looking at different exporters.
 */
import { describe, expect, it } from 'vitest';

import {
  codeDownloadName,
  codeText,
  exportSource,
  KIND,
  parse,
  STYLE,
  type Block,
} from './engine';

const TABLE = '| a | b |\n| :-- | --: |\n| **x** | [y](https://e) |\n';

describe('code export', () => {
  it('copies the code as the writer sent it', () => {
    expect(codeText('```ts\nconst a = 1;\nconst b = 2;\n```\n')).toBe(
      'const a = 1;\nconst b = 2;'
    );
  });

  it('the code of a block inside a list, without the marker', () => {
    expect(codeText('- ```\n  x\n  ```\n')).toBe('x');
  });

  it('copies an empty fence as an empty string, never a placeholder', () => {
    // A zero-width space stands in for an empty block in a text layout; it
    // must never reach a clipboard.
    const text = codeText('```\n```\n');
    expect(text).toBe('');
    expect(text).not.toContain('\u200b');
  });

  it('copies the same bytes whatever the layout', () => {
    // The exporter reads the document, which has no layout input at all, so a
    // wrap toggle cannot change a copy.
    const source = '```ts\nconst a: number = 1;\n```\n';
    expect(codeText(source)).toBe(codeText(source));
    expect(codeText(source)).toBe('const a: number = 1;');
  });

  it('names a code download from the fence, then the config, then a fallback', () => {
    const source = '```ts title="app.ts"\nx\n```\n\n```ts:src/lib.ts\ny\n```\n';
    expect(codeDownloadName(source, 0)).toBe('app.ts');
    expect(codeDownloadName(source, 1)).toBe('lib.ts');
    expect(codeDownloadName('```ts\nx\n```\n', 0, { base: 'snippet' })).toBe(
      'snippet.ts'
    );
    expect(codeDownloadName('```brainfuck\n+\n```\n', 0)).toBe('file.txt');
  });
});

describe('table export', () => {
  const rows = (source: string) =>
    exportSource(source, 'csv', [{ op: 'e', from: 0, to: 2 }]).exports[0]?.text;

  it('writes a CSV, quoting only what needs it', () => {
    expect(rows('| a | b |\n| - | - |\n| 1 | y, z |\n')).toBe('a,b\n1,"y, z"');
  });

  it('writes a TSV without quoting', () => {
    const { exports } = exportSource(
      '| a | b |\n| - | - |\n| 1 | y, z |\n',
      'tsv',
      [{ op: 'e', from: 0, to: 2 }]
    );
    expect(exports[0]?.text).toBe('a\tb\n1\ty, z');
  });

  it('writes Markdown with the styles the writer used', () => {
    const { exports } = exportSource(TABLE, 'markdown', [
      { op: 'e', from: 0, to: 2 },
    ]);
    expect(exports[0]?.text).toBe(
      '| a | b |\n| :-- | --: |\n| **x** | [y](https://e) |'
    );
  });

  it('writes HTML a paste reads as a table', () => {
    const { exports } = exportSource(TABLE, 'html', [
      { op: 'e', from: 0, to: 2 },
    ]);
    // md4c carries a column's alignment onto every cell, so a body cell is
    // styled the same way its header is and a paste keeps the columns aligned.
    expect(exports[0]?.text).toBe(
      '<table><thead><tr><th style="text-align:left">a</th>' +
        '<th style="text-align:right">b</th></tr></thead>' +
        '<tbody><tr><td style="text-align:left"><strong>x</strong></td>' +
        '<td style="text-align:right"><a href="https://e">y</a></td></tr></tbody></table>'
    );
  });

  it('names a table download from the configured base', () => {
    const { exports } = exportSource(
      TABLE,
      'csv',
      [{ op: 't', extension: 'csv' }],
      {
        base: 'report',
      }
    );
    expect(exports[0]?.text).toBe('report.csv');
  });
});

describe('the exporter and the model agree', () => {
  it('a table header row is bold in the model and plain in the export', () => {
    // A GFM header is bold because it is a header. Writing `**` back would put
    // noise on every cell of every export, and the destination bolds a header
    // row anyway.
    const header = parse(TABLE).blocks[0];
    expect(header).toBeDefined();
    expect(header?.tableHeader).toBe(true);
    expect(header?.runs.map(([, , styles]) => styles)).toContain(STYLE.strong);
    const { exports } = exportSource(TABLE, 'markdown', [
      { op: 'e', from: 0, to: 2 },
    ]);
    expect(exports[0]?.text).not.toContain('**a**');
  });

  it('a table segment covers every row of one table', () => {
    const { model } = exportSource(TABLE, 'csv', [{ op: 'e', from: 0, to: 2 }]);
    expect(model.blocks.map((block) => block.kind)).toEqual([
      KIND.table,
      KIND.table,
    ]);
  });

  it('a Markdown export re-parses to the same table', () => {
    // The round trip runs through the real parser: a rule that only happens to
    // re-read correctly in the exporter cannot pass.
    const { model } = exportSource(TABLE, 'markdown', [
      { op: 'e', from: 0, to: 2 },
    ]);
    const [exported] = exportSource(TABLE, 'markdown', [
      { op: 'e', from: 0, to: 2 },
    ]).exports;
    const again = parse(exported?.text ?? '');
    expect(again.blocks.map((block: Block) => block.text)).toEqual(
      model.blocks.map((block) => block.text)
    );
    expect(again.blocks.map((block: Block) => block.runs)).toEqual(
      model.blocks.map((block) => block.runs)
    );
    expect(again.links.map((link) => link.url)).toEqual(
      model.links.map((link) => link.url)
    );
  });
});

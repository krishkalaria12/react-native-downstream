/* eslint-disable no-bitwise -- inline styles are a bitmask */
/**
 * The parser that ships in the app, exercised directly.
 *
 * These assertions are about what a reader sees: the visible text, the links
 * that can be opened, the block structure, and the inline styles. They run the
 * real C++ engine through `tests/engine.ts`, so a regression in parsing
 * fails here rather than on a device.
 */
import { describe, expect, it } from 'vitest';

import { samples } from './samples';
import {
  assertStreamingMatchesFullParse,
  CONTAINER,
  hasStyle,
  KIND,
  linkUrls,
  parse,
  STYLE,
  tableRows,
  visibleText,
} from './engine';

describe('block structure', () => {
  it('reads a heading and a paragraph', () => {
    const model = parse('# Title\n\nA paragraph.\n');
    expect(model.blocks.map((block) => block.kind)).toEqual([
      KIND.heading,
      KIND.paragraph,
    ]);
    expect(visibleText(model)).toBe('Title\nA paragraph.');
  });

  it('keeps an open code fence readable while it streams', () => {
    const model = parse('```ts\nconst x = 1;\n');
    expect(model.blocks).toHaveLength(1);
    expect(model.blocks[0]?.kind).toBe(KIND.code);
    expect(model.blocks[0]?.language).toBe('ts');
    expect(model.blocks[0]?.text).toContain('const x = 1;');
  });

  it('reads a table once its separator row arrives', () => {
    // Before the separator the rows are paragraphs; after, a table.
    expect(parse('| A | B |\n').blocks[0]?.kind).toBe(KIND.paragraph);
    const model = parse('| A | B |\n| --- | --- |\n| one | two |\n');
    expect(model.blocks[0]?.kind).toBe(KIND.table);
    expect(tableRows(model)).toEqual([
      ['A', 'B'],
      ['one', 'two'],
    ]);
  });

  it('marks a table header row', () => {
    const model = parse('| A | B |\n| --- | --- |\n| one | two |\n');
    expect(model.blocks[0]?.tableHeader).toBe(true);
  });

  it('reads list items and their task state', () => {
    // Each item is its own block, and a task list marks the state on it.
    const model = parse('- [ ] open\n- [x] done\n');
    expect(model.blocks.map((block) => block.text)).toEqual(['open', 'done']);
    expect(model.blocks.map((block) => block.containers[0]?.task)).toEqual([
      1, 2,
    ]);
  });

  it('nests list items', () => {
    // The nested item becomes a second block with its own container.
    const model = parse('- one\n  - two\n');
    expect(model.blocks.map((block) => block.text)).toEqual(['one', 'two']);
    expect(model.blocks[0]?.containers[0]?.kind).toBe(CONTAINER.listItem);
  });
});

describe('inline styles', () => {
  it('marks strong, emphasis, code, and strikethrough', () => {
    const model = parse('a **b** c *d* e `f` g ~~h~~\n');
    const block = model.blocks[0]!;
    expect(block.text).toBe('a b c d e f g h');
    expect(hasStyle(block, 'b', STYLE.strong)).toBe(true);
    expect(hasStyle(block, 'd', STYLE.emphasis)).toBe(true);
    expect(hasStyle(block, 'f', STYLE.code)).toBe(true);
    expect(hasStyle(block, 'h', STYLE.strikethrough)).toBe(true);
  });

  it('leaves the visible text free of markers', () => {
    const model = parse('a **b** c *d*\n');
    expect(visibleText(model)).toBe('a b c d');
  });

  it('handles nested emphasis in one span', () => {
    const model = parse('**bold _and italic_**\n');
    const block = model.blocks[0]!;
    expect(block.text).toBe('bold and italic');
    expect(hasStyle(block, 'bold', STYLE.strong)).toBe(true);
    expect(hasStyle(block, 'italic', STYLE.strong | STYLE.emphasis)).toBe(true);
  });
});

describe('links', () => {
  it('allows absolute http, https, and mailto targets', () => {
    const model = parse(
      '[a](https://example.com) [b](http://example.com) [c](mailto:a@b.com)\n'
    );
    expect(linkUrls(model)).toEqual([
      'https://example.com',
      'http://example.com',
      'mailto:a@b.com',
    ]);
  });

  it('refuses a script target', () => {
    const model = parse('[trap](javascript:alert(1))\n');
    expect(linkUrls(model)).toEqual([]);
  });

  it('refuses a data target', () => {
    expect(linkUrls(parse('[x](data:text/html,<b>hi</b>)\n'))).toEqual([]);
  });

  it('reports the source range of a link', () => {
    const model = parse('see [docs](https://example.com) here\n');
    const [link] = model.links;
    expect(link?.range).toBeDefined();
    const source = 'see [docs](https://example.com) here\n';
    const [start, end] = link!.range;
    expect(source.slice(start, end)).toBe('[docs](https://example.com)');
  });

  it('drops a link whose destination is still arriving', () => {
    expect(linkUrls(parse('[a](https://exam'))).toEqual([]);
  });
});

describe('safety', () => {
  it('shows raw HTML as text, never as markup', () => {
    const model = parse('Raw <b>bold</b> stays literal.\n');
    expect(model.blocks.some((block) => block.kind === KIND.html)).toBe(false);
    expect(visibleText(model)).toContain('<b>bold</b>');
  });

  it('shows math delimiters as literal text', () => {
    expect(visibleText(parse('cost is $x^2$ today\n'))).toContain('$x^2$');
  });

  it('makes an image show its alt text and request nothing', () => {
    const model = parse('![diagram](https://example.com/a.png)\n');
    // The destination is not a link the user can open.
    expect(linkUrls(model)).toEqual([]);
    expect(visibleText(model)).toContain('diagram');
  });
});

describe('unicode', () => {
  it('keeps astral characters intact', () => {
    expect(visibleText(parse('Hello 👩‍💻 é 漢字 שלום\n'))).toBe(
      'Hello 👩‍💻 é 漢字 שלום'
    );
  });

  it('normalises CRLF', () => {
    expect(visibleText(parse('a\r\nb\r\n'))).toBe('a\nb');
  });
});

describe('streaming', () => {
  for (const sample of samples) {
    it(`renders ${sample.id} the same streamed as complete`, () => {
      // The engine itself checks every prefix against a full parse; a failure
      // throws with the prefix that diverged.
      expect(() =>
        assertStreamingMatchesFullParse(sample.chunks)
      ).not.toThrow();
      // And the joined chunks are the document the sample describes.
      expect(sample.chunks.join('')).toBe(sample.source);
      expect(parse(sample.source).error).toBeUndefined();
    });
  }
});

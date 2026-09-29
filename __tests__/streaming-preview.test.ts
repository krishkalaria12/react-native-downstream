/* eslint-disable no-bitwise -- inline styles are a bitmask */
/**
 * How the engine behaves on markdown that is still arriving.
 *
 * `remend-corpus.ts` holds several hundred shapes of incomplete markdown ported
 * from remend's suite. Those inputs are the interesting part: a model can stop
 * emitting after any of them, and a renderer has to decide what to show without
 * inventing syntax the writer did not ask for.
 *
 * The corpus is asserted as invariants rather than expected output, because
 * the invariants are what must never break. What a reader *should* see while a
 * span is open is a policy question, settled in `docs/streaming-semantics.md`,
 * and the cases that encode an answer are written out at the bottom of this
 * file instead of being inferred from a repair pass.
 */
import { describe, expect, it } from 'vitest';

import {
  assertStreamingBatchMatchesFullParse,
  CONTAINER,
  KIND,
  linkUrls,
  parse,
  STYLE,
  stylesOf,
  visibleText,
} from './engine';
import { CORPUS, MAX_INPUT_BYTES } from './remend-corpus';

/** Splits a source into single characters, the worst case a token stream has. */
const byCharacter = (source: string): string[] => [...source];

/** Splits on line boundaries, the shape a real model emits in. */
const byLine = (source: string): string[] => source.split(/(?<=\n)/);

/** Truncates an input that a single concatenated document would make too long. */
const bounded = (input: string): string =>
  Buffer.byteLength(input, 'utf8') <= MAX_INPUT_BYTES
    ? input
    : input.slice(0, MAX_INPUT_BYTES);

const overSized = CORPUS.filter(
  (input) => Buffer.byteLength(input, 'utf8') > MAX_INPUT_BYTES
);

describe('ported corpus', () => {
  it('holds a corpus worth sweeping', () => {
    // A silently emptied corpus would leave every assertion below vacuous.
    expect(CORPUS.length).toBeGreaterThan(400);
    expect(new Set(CORPUS).size).toBe(CORPUS.length);
    expect(overSized).toEqual([]);
  });

  it('parses every input without reporting an error', () => {
    const failures: string[] = [];
    for (const input of CORPUS) {
      const model = parse(input);
      if (model.error)
        failures.push(`${JSON.stringify(input)}: ${model.error}`);
    }
    expect(failures).toEqual([]);
  });

  it('never offers a link the reader could not have meant', () => {
    // A partial URL is still a syntactically valid one, so the scheme check in
    // `LinkPolicy.cpp` is not enough on its own: nothing in a half-written link
    // should be tappable.
    const failures: string[] = [];
    for (const input of CORPUS) {
      for (const url of linkUrls(parse(input))) {
        if (!/^(?:https?|mailto):/i.test(url)) {
          failures.push(`${JSON.stringify(input)} -> ${url}`);
        }
      }
    }
    expect(failures).toEqual([]);
  });

  it('emits no link at all for an unterminated link target', () => {
    // The corpus is full of half-written links. None of them is a destination
    // the writer finished typing, so none may produce a target.
    const unfinished = CORPUS.filter(
      (input) => /!?\[[^\]]*\]\([^)]*$/.test(input) || /!?\[[^\]]*$/.test(input)
    );
    expect(unfinished.length).toBeGreaterThan(25);
    for (const input of unfinished) {
      // A complete URL inside an unfinished image is the one exception, and it
      // is covered on its own below.
      if (/https?:\/\/[^)\s]+\.[^)\s]*$/.test(input)) continue;
      expect(linkUrls(parse(input)), JSON.stringify(input)).toEqual([]);
    }
  });

  it('autolinks a complete URL that an unfinished image left behind', () => {
    // `![a](http://ex` has no dot, so there is no URL to find and nothing is
    // linked. `![a](http://example.com/x` does contain a complete URL, and
    // permissive autolinks are enabled, so the URL is linked on its own. The
    // image itself is not recognised: no alt text is extracted and no request
    // is made, because the `![` is still literal text.
    expect(linkUrls(parse('![a](http://ex'))).toEqual([]);
    const model = parse('See ![diagram](http://example.com/img');
    expect(linkUrls(model)).toEqual(['http://example.com/img']);
    expect(model.blocks[0]?.text).toBe('See ![diagram](http://example.com/img');
    // Style 16 is Link with an ImageAlt-free run, so the alt text was not taken.
    expect(model.blocks[0]?.runs).toEqual([[15, 37, STYLE.link, 0]]);
  });

  it('reaches the same document whether it arrives by char or by line', () => {
    // The incremental parser commits a proven prefix and reparses the rest. A
    // chunk boundary must never be able to change the finished document, so
    // every prefix of every corpus input has to agree with a full parse.
    const documents = CORPUS.flatMap((input) => [
      byCharacter(input),
      byLine(input),
      byCharacter(`${input}\n`),
    ]);
    assertStreamingBatchMatchesFullParse(documents);
  });

  it('survives a corpus grown into one long stream', () => {
    // Concatenation is where a repair pass would leak: a stray marker in one
    // input is adjacent to real syntax in the next, so the parser cannot treat
    // each entry as isolated.
    const joined = CORPUS.map((input) => bounded(`${input}\n\n`));
    assertStreamingBatchMatchesFullParse([byCharacter(joined.join(''))]);
  });
});

describe('open inline spans stay literal', () => {
  // `docs/streaming-semantics.md` allows a provisional style only when the
  // opener is unambiguous, and otherwise says to show literal markers. The
  // engine currently takes the literal branch everywhere, so these pin it. A
  // future provisional-styling pass has to relax exactly these, and nothing
  // else.
  const open = [
    '**bold wor',
    '*ital',
    '`code',
    '~~strike',
    '***both',
    'text**bold and *ital',
  ];

  for (const input of open) {
    it(`keeps ${JSON.stringify(input)} literal`, () => {
      const model = parse(input);
      expect(visibleText(model)).toBe(input);
      expect(model.blocks[0]?.runs).toEqual([]);
    });
  }

  it('keeps a marker that is complete on its own literal', () => {
    // A bare marker carries no content, so there is nothing to style even
    // provisionally. `*` and `-` are excluded because they are list markers,
    // and a run of three or more is a thematic break.
    for (const input of ['**', '__', '_', '~~', '`', '``']) {
      expect(visibleText(parse(input)), JSON.stringify(input)).toBe(input);
    }
    // A bare `*` or `-` opens a list item with no content, which produces no
    // block rather than a paragraph of literal text.
    for (const input of ['*', '-', '+']) {
      expect(parse(input).blocks, JSON.stringify(input)).toEqual([]);
    }
  });

  it('reads a run of three or more markers as a rule', () => {
    // The counterpart to the case above, so it is not vacuous: enough markers
    // form a block, and the text is the empty string because there is none.
    for (const input of ['***', '****', '*****', '___', '____']) {
      expect(parse(input).blocks[0]?.kind, JSON.stringify(input)).toBe(KIND.hr);
    }
  });

  it('applies the rule of three to a half-closed marker', () => {
    // `**bold*` is emphasis, not strong. The opening run of two can open, the
    // closing run of one can close, and the rule of three forbids using a
    // three-character run as both. The single `*` therefore becomes the opener
    // and the run of two cannot also be one, so one asterisk stays literal.
    const block = parse('**bold*').blocks[0]!;
    expect(block.text).toBe('*bold');
    expect(stylesOf(block, 'bold').some((run) => run[2] & STYLE.emphasis)).toBe(
      true
    );
    expect(stylesOf(block, 'bold').some((run) => run[2] & STYLE.strong)).toBe(
      false
    );
  });

  it('leaves a complete span alone', () => {
    // The corpus also contains finished spans. They must still be styled, or
    // the cases above would pass for the wrong reason.
    const model = parse('**bold** and *ital* and ~~gone~~');
    const block = model.blocks[0]!;
    expect(stylesOf(block, 'bold').some((run) => run[2] & STYLE.strong)).toBe(
      true
    );
    expect(stylesOf(block, 'ital').some((run) => run[2] & STYLE.emphasis)).toBe(
      true
    );
    expect(
      stylesOf(block, 'gone').some((run) => run[2] & STYLE.strikethrough)
    ).toBe(true);
  });

  it('keeps an unterminated backtick literal, and its markers with it', () => {
    // A code span needs both delimiters, so an unterminated one leaves the
    // backtick in the text. The `**` after it is then ordinary text too, since
    // it is no longer inside a span.
    const block = parse('`code **not bold').blocks[0]!;
    expect(block.text).toBe('`code **not bold');
    expect(block.runs).toEqual([]);
  });

  it('does not style markers inside a closed fence', () => {
    // The contrast case: a closed fence really is verbatim, so the markers
    // inside it stay unstyled for a different and better reason.
    const block = parse('```\n**not bold**\n').blocks[0]!;
    expect(block.kind).toBe(KIND.code);
    expect(block.text).toBe('**not bold**');
    expect(block.runs).toEqual([]);
  });
});

describe('structure that is still arriving', () => {
  it('reads a setext heading only once the underline line arrives', () => {
    // A paragraph on its own is a paragraph, however it is chunked.
    expect(parse('Guide').blocks[0]?.kind).toBe(KIND.paragraph);
    expect(parse('Guide\n').blocks[0]?.kind).toBe(KIND.paragraph);
    // One or two dashes already form a complete setext underline, so the
    // reclassification happens on the first dash, not on the third.
    for (const underline of ['-', '--', '---', '=']) {
      const heading = parse(`Guide\n${underline}\n`);
      expect(heading.blocks[0]?.kind, JSON.stringify(underline)).toBe(
        KIND.heading
      );
      expect(heading.blocks[0]?.headingLevel, JSON.stringify(underline)).toBe(
        underline === '=' ? 1 : 2
      );
    }
  });

  it('reads a table only once its delimiter row completes', () => {
    // Already covered by the `gfm-table` oracle case, but the corpus is where
    // a partial delimiter row shows up, so the boundary is pinned here too.
    expect(parse('| A | B |\n').blocks[0]?.kind).toBe(KIND.paragraph);
    expect(parse('| A | B |\n| --- |\n').blocks[0]?.kind).toBe(KIND.table);
  });

  it('keeps an open fence as code with its language', () => {
    for (const [input, language, text] of [
      ['```ts\nconst x = 1;', 'ts', 'const x = 1;'],
      ['```javascript\nconst x = 5;', 'javascript', 'const x = 5;'],
      ['```python\ndef hello():', 'python', 'def hello():'],
      ['~~~py\nprint(1)', 'py', 'print(1)'],
    ] as const) {
      const block = parse(input).blocks[0]!;
      expect(block.kind, JSON.stringify(input)).toBe(KIND.code);
      expect(block.language, JSON.stringify(input)).toBe(language);
      expect(block.text, JSON.stringify(input)).toBe(text);
    }
  });

  it('keeps a rule as a rule without eating the text around it', () => {
    for (const input of ['---', '***', '___', '- - -', '_ _ _', '   ---']) {
      const model = parse(`${input}\n`);
      expect(model.blocks[0]?.kind, JSON.stringify(input)).toBe(KIND.hr);
    }
    const around = parse('Before\n***\nAfter\n');
    expect(around.blocks.map((block) => block.kind)).toEqual([
      KIND.paragraph,
      KIND.hr,
      KIND.paragraph,
    ]);
  });

  it('does not let a partial rule reclassify a paragraph', () => {
    // `Text with --` is a paragraph mid-stream, not a heading and not a rule.
    expect(parse('Text with --\n').blocks.map((b) => b.kind)).toEqual([
      KIND.paragraph,
    ]);
    expect(parse('This is not a --- horizontal rule\n').blocks[0]?.kind).toBe(
      KIND.paragraph
    );
  });

  it('keeps math source literal', () => {
    for (const input of [
      '$x +',
      '$$\nx +',
      'cost is $50 and $100',
      '$$formula$',
    ]) {
      expect(visibleText(parse(input)), JSON.stringify(input)).toBe(input);
    }
  });
});

describe('where a repair pass would change the parse', () => {
  // remend rewrites the source string before parsing. These are the cases where
  // doing so would change what the writer typed. Two of them, the comparison
  // operator and the half-closed marker, are not bugs in this engine at all:
  // md4c follows CommonMark and remend does not. They are pinned here so the
  // reasoning survives a future attempt to vendor a repair pass.

  it('reads a greater-than after a list marker as a nested quote', () => {
    // The list item's content is `> 25: rich`, which is a blockquote. That is
    // the CommonMark answer, so the containers are a list item and a quote.
    // The marker itself is decoration, so it is not part of the block text.
    for (const input of [
      '- > 25: rich',
      '* > 25: rich',
      '+ > 25: rich',
      '1. > 25: rich',
      '2) > 10: high',
      '  - > 25: rich',
    ]) {
      const model = parse(input);
      expect(
        model.blocks[0]?.containers.map((c) => c.kind),
        JSON.stringify(input)
      ).toEqual([CONTAINER.listItem, CONTAINER.quote]);
      expect(visibleText(model), JSON.stringify(input)).toBe(
        input.replace(/^\s*(?:[-*+]|\d+[.)])\s+>\s+/, '')
      );
    }
  });

  it('keeps a less-than sign as text', () => {
    // The contrast case. `<` is not a quote marker, so the item stays a plain
    // list item, which is what makes the case above structural rather than a
    // blanket rule.
    const model = parse('- < 5\n');
    expect(model.blocks[0]?.containers.map((c) => c.kind)).toEqual([
      CONTAINER.listItem,
    ]);
    expect(visibleText(model)).toBe('< 5');
  });

  it('reads a real blockquote as a quote', () => {
    expect(parse('> Some blockquote\n').blocks[0]?.containers).toHaveLength(1);
    expect(visibleText(parse('> Some blockquote\n'))).toBe('Some blockquote');
  });

  it('does not strike through a single tilde between words', () => {
    for (const input of [
      'temp was 20~25 degrees',
      '20~25°C',
      '~hello',
      'hello~',
      'foo~bar',
      '日本~語',
      'α~β',
      'é~x',
      '𐐀~a',
    ]) {
      const block = parse(input).blocks[0]!;
      expect(
        block.runs.some((run) => run[2] & STYLE.strikethrough),
        JSON.stringify(input)
      ).toBe(false);
      expect(visibleText(parse(input)), JSON.stringify(input)).toBe(input);
    }
    // Two tildes are a real strikethrough, so the check above is not vacuous.
    expect(
      stylesOf(parse('a ~~b~~ c').blocks[0]!, 'b').some(
        (run) => run[2] & STYLE.strikethrough
      )
    ).toBe(true);
  });

  it('keeps a star that is an arithmetic operator out of emphasis', () => {
    for (const input of ['3 + 2 - 5 * 0 = ?', '5 * 0', 'x * y', '2 * 3 * 4']) {
      expect(visibleText(parse(input)), JSON.stringify(input)).toBe(input);
    }
  });

  it('consumes a backslash escape, as CommonMark requires', () => {
    // The backslash is syntax, so it does not reach the reader. A repair pass
    // that counts markers without tracking escapes would close the span and
    // show the backslash instead.
    expect(visibleText(parse('\\*not italic'))).toBe('*not italic');
    expect(visibleText(parse('\\\\*actually italic'))).toBe(
      '\\*actually italic'
    );
    expect(visibleText(parse('\\*escaped\\* but *real italic'))).toBe(
      '*escaped* but *real italic'
    );
    // An escaped pair opens nothing, so the trailing `*bold` is emphasis only
    // because of the real, unescaped marker later in the line.
    const mixed = parse('\\**not bold').blocks[0]!;
    expect(mixed.runs).toEqual([]);
  });

  it('keeps a list marker out of emphasis', () => {
    for (const input of [
      '* Item 1\n* Item 2\n* Item 3',
      '- Item 1\n- Item 2 with *italic*\n- Item 3',
      '- __',
      '- **',
      '- ***',
      '- *',
      '- _',
      '- ~~',
      '- `',
    ]) {
      const model = parse(input);
      expect(
        model.blocks.every((block) => block.containers.length > 0),
        JSON.stringify(input)
      ).toBe(true);
    }
  });

  it('leaves an unterminated span that opens after a complete one alone', () => {
    // `**bold** then **more` has a finished span and an open one. The finished
    // span is styled and the open one is not, so the two do not bleed together.
    const block = parse('**bold** then **more').blocks[0]!;
    expect(stylesOf(block, 'bold').some((run) => run[2] & STYLE.strong)).toBe(
      true
    );
    expect(stylesOf(block, 'more').some((run) => run[2] & STYLE.strong)).toBe(
      false
    );
  });

  it('resolves a reference link only when the definition arrives', () => {
    // A half-written reference must not borrow a definition that has not been
    // typed yet, and must not make one up either.
    expect(linkUrls(parse('See [entry][ref].\n'))).toEqual([]);
    const resolved = parse(
      'See [entry][ref].\n\n[ref]: https://example.com/r\n'
    );
    expect(linkUrls(resolved)).toEqual(['https://example.com/r']);
  });
});

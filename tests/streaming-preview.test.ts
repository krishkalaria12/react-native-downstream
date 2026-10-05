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
  PREVIEW_ALL,
  PREVIEW_EMPHASIS,
  PREVIEW_HTML_TAGS,
  preview,
  replaySession,
  stylesOf,
  STYLE,
  visibleText,
} from './engine';
import { CORPUS } from './remend-corpus';

/** Splits a source into single characters, the worst case a token stream has. */
const byCharacter = (source: string): string[] => [...source];

/** Splits on line boundaries, the shape a real model emits in. */
const byLine = (source: string): string[] => source.split(/(?<=\n)/);

/**
 * A ceiling for the one document built by joining the whole corpus, which is
 * otherwise a single source the size of every input at once. The adjacency it
 * checks needs a few thousand bytes to be true of every boundary, and the sweep
 * is one parse per byte, so the whole corpus would cost more than the rest of
 * the suite put together.
 */
const MAX_JOINED_BYTES = 4 * 1024;

/** Joins inputs with blank lines, up to a ceiling, each ending a block. */
const joined = (inputs: readonly string[]): string => {
  const parts: string[] = [];
  let bytes = 0;
  for (const input of inputs) {
    if (bytes >= MAX_JOINED_BYTES) break;
    parts.push(`${input}\n\n`);
    bytes += Buffer.byteLength(input, 'utf8') + 2;
  }
  return parts.join('');
};

describe('ported corpus', () => {
  it('holds a corpus worth sweeping', () => {
    // A silently emptied corpus would leave every assertion below vacuous.
    expect(CORPUS.length).toBeGreaterThan(800);
    expect(new Set(CORPUS).size).toBe(CORPUS.length);
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

  it('leaves an unfinished destination as text, not a tap target', () => {
    // md4c autolinks a bare URL anywhere in text, so the `](` of a link or
    // image that never closed used to leave a tappable URL behind a target the
    // next chunk could move or delete. `![a](http://ex` never had a URL to find
    // because there is no dot, and `![a](http://example.com/x` did.
    for (const input of [
      '![a](http://ex',
      '![a](http://example.com/x',
      '[a](http://example.com/x',
      'See ![diagram](http://example.com/img',
      'read [the docs](http://example.com/guide now',
    ]) {
      const model = parse(input);
      expect(linkUrls(model), JSON.stringify(input)).toEqual([]);
      expect(model.blocks[0]?.runs, JSON.stringify(input)).toEqual([]);
      expect(visibleText(model), JSON.stringify(input)).toBe(input);
    }
  });

  it("keeps a bare URL in prose, where it is the writer's destination", () => {
    // The contrast case. Suppressing only the ones inside an unfinished `](`
    // would be a bug if it reached prose, so every autolink shape that has to
    // survive is listed here.
    const kept: [string, string[]][] = [
      ['see http://example.com/x now', ['http://example.com/x']],
      ['mail someone@example.com now', ['mailto:someone@example.com']],
      ['see <http://example.com/x> here', ['http://example.com/x']],
      ['<http://example.com/x>', ['http://example.com/x']],
      ['[a](http://example.com/x)', ['http://example.com/x']],
      ['[a][r]\n\n[r]: http://example.com/x', ['http://example.com/x']],
      [
        '![a](http://example.com/y) http://example.com/x',
        ['http://example.com/x'],
      ],
    ];
    for (const [input, urls] of kept) {
      expect(linkUrls(parse(input)), JSON.stringify(input)).toEqual(urls);
    }
    // A completed image is alt text, and it never becomes a target of its own.
    const image = parse('![a](http://example.com/x)');
    expect(linkUrls(image)).toEqual([]);
    expect(image.blocks[0]?.text).toBe('a');
    // A URL inside a code span or fence is verbatim, so it is never a target.
    for (const input of [
      '`http://example.com/x`',
      '```\nhttp://example.com/x\n```',
    ]) {
      expect(linkUrls(parse(input)), JSON.stringify(input)).toEqual([]);
    }
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
    assertStreamingBatchMatchesFullParse([byCharacter(joined(CORPUS))]);
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

describe('escapes', () => {
  // The corpus covers `\*` and `\``. It does not cover `_`, `~`, `$`, `[` or a
  // backtick, so these are pinned here. A backslash is syntax: it does not
  // reach the reader, and it stops the character after it from opening a span.

  it('does not open emphasis from an underscore inside a word', () => {
    // CommonMark flanking rules, not a repair decision. `snake_case_name` has
    // two underscores and neither can open, so nothing is styled.
    for (const input of [
      'The value is 1_000_000',
      'snake_case_name',
      'a_b_c',
      'some\\_text_with_underscores',
    ]) {
      const block = parse(input).blocks[0]!;
      expect(block.runs, JSON.stringify(input)).toEqual([]);
      expect(visibleText(parse(input)), JSON.stringify(input)).toBe(
        input.replace(/\\_/g, '_')
      );
    }
    // Two underscores around a whole word still emphasize it, so the cases
    // above are not passing because underscores never work.
    const emphasis = parse('_real emphasis_').blocks[0]!;
    expect(
      stylesOf(emphasis, 'real emphasis').some((r) => r[2] & STYLE.emphasis)
    ).toBe(true);
  });

  it('consumes the backslash on an escaped marker', () => {
    for (const [input, text] of [
      ['α\\~β', 'α~β'],
      ['value \\[a<b\\] end', 'value [a<b] end'],
      ['Text with \\* escaped asterisk', 'Text with * escaped asterisk'],
      // `(` and `)` are punctuation, so an escaped paren is a literal paren.
      // This is why inline LaTeX loses its delimiters to the reader.
      ['\\(x_1, x_t\\)', '(x_1, x_t)'],
    ] as const) {
      expect(visibleText(parse(input)), JSON.stringify(input)).toBe(text);
    }
  });

  it('keeps a real span alive beside an escaped one', () => {
    // The escaped `$` is literal text and the underscores still emphasize, so
    // one escape does not disarm the rest of the line.
    const currency = parse('Price is \\$50 and _this is italic_').blocks[0]!;
    expect(currency.text).toBe('Price is $50 and this is italic');
    expect(
      stylesOf(currency, 'this is italic').some((r) => r[2] & STYLE.emphasis)
    ).toBe(true);
    // An unterminated span after an escape stays literal either way.
    const incomplete = parse('Cost \\$100 with _incomplete').blocks[0]!;
    expect(incomplete.runs).toEqual([]);
    expect(incomplete.text).toBe('Cost $100 with _incomplete');
  });

  it('does not let an escaped backtick open a code span', () => {
    // The backticks are literal, so the `**` beside them is real emphasis.
    const block = parse('\\`not code\\` **bold**').blocks[0]!;
    expect(block.text).toBe('`not code` bold');
    expect(stylesOf(block, 'bold').some((r) => r[2] & STYLE.strong)).toBe(true);
  });
});

describe('a fence needs a line break', () => {
  // An info string is only an info string once a line break follows it. With no
  // break the backticks are an inline code span, which reads very differently
  // and is worth pinning.

  it('reads a fence opener with no line break as inline code', () => {
    for (const input of ['```python code```', '```code```', '```code```\n']) {
      const block = parse(input).blocks[0]!;
      expect(block.kind, JSON.stringify(input)).toBe(KIND.paragraph);
      expect(block.language, JSON.stringify(input)).toBe('');
      expect(
        stylesOf(block, block.text).some((r) => r[2] & STYLE.code),
        JSON.stringify(input)
      ).toBe(true);
    }
  });

  it('reads the same opener as a fence once a line break arrives', () => {
    const block = parse('```python code\n```\n').blocks[0]!;
    expect(block.kind).toBe(KIND.code);
    expect(block.language).toBe('python');
    expect(block.text).toBe('');
  });

  it('reads a bare backtick run as an open fence, but not mid-word', () => {
    // Six backticks on their own line is an opener with an empty info string.
    expect(parse('``````').blocks[0]?.kind).toBe(KIND.code);
    // Three or more backticks need to start a line, so a trailing run is text.
    const inline = parse('text``````').blocks[0]!;
    expect(inline.kind).toBe(KIND.paragraph);
    expect(inline.text).toBe('text``````');
    expect(inline.runs).toEqual([]);
  });
});

describe('a half-arrived tag at the tail', () => {
  // The `htmlTags` preview feature, through the real engine. It is the one
  // place a fragment of source is hidden rather than restyled, so the cases
  // below are answers rather than invariants: what is hidden, what comes back,
  // and what is never touched.
  const INLINE_HTML = 1 << 3;
  const shown = (source: string, features: number, parseFlags = INLINE_HTML) =>
    visibleText(preview(source, features, parseFlags));
  /** The same tail, once the stream has finished and the policy is dropped. */
  const settled = (
    source: string,
    features: number,
    parseFlags = INLINE_HTML
  ) =>
    visibleText(
      replaySession(
        'full',
        [
          { op: 'preview', features },
          { op: 'append', chunk: source },
          { op: 'parse', finished: false },
          { op: 'parse', finished: true },
        ],
        parseFlags
      )
    );

  it('hides a fragment that could still become an allowed tag', () => {
    for (const [source, expected] of [
      ['Mix<br', 'Mix'],
      ['Mix<b', 'Mix'],
      ['H<sub>2</sub', 'H2'],
      ['x<sup>2</sup', 'x2'],
    ] as const) {
      expect(shown(source, PREVIEW_HTML_TAGS), JSON.stringify(source)).toBe(
        expected
      );
    }
    // A bare `<` after a space is not hidden: dropping it would also drop the
    // space, because a parse trims a line's trailing whitespace, and the proof
    // asks for the fragment and nothing else.
    expect(shown('a <', PREVIEW_HTML_TAGS)).toBe('a <');
  });

  it('returns the fragment the moment the stream finishes', () => {
    // finish() drops the policy entirely, so the settled document is a plain
    // parse of the bytes the writer sent, with the tags literal because the
    // inline HTML bit is off in this case.
    expect(settled('Mix<br', PREVIEW_ALL, 0)).toBe('Mix<br');
  });

  it('leaves a fragment that cannot become a tag alone', () => {
    for (const source of ['a<b and 2<3', 'a<blink', 'a<subtitle>x']) {
      expect(shown(source, PREVIEW_ALL), JSON.stringify(source)).toBe(source);
    }
    // The `<br>` before it is complete, so it is a break, and the fragment after
    // it is refused because removing it would change the line in front of it.
    expect(shown('a<br><su', PREVIEW_ALL)).toBe('a\n<su');
  });

  it('leaves a fragment that is code alone', () => {
    // An unterminated code span claims the fragment as its content, and hiding
    // it would delete characters the writer typed inside backticks.
    expect(shown('`<br`', PREVIEW_ALL)).toBe('<br');
    expect(shown('`<sup>1</sup`', PREVIEW_ALL)).toBe('<sup>1</sup');
  });

  it('does nothing without the feature', () => {
    expect(shown('Mix<br', PREVIEW_EMPHASIS)).toBe('Mix<br');
  });

  it('holds the html-tags fragment cases to the incremental rule', () => {
    // Every chunk boundary of each case, through the incremental parser.
    for (const source of ['Mix<br', 'H<sub>2</sub', 'x<sup>2</sup']) {
      assertStreamingBatchMatchesFullParse([source.split('')]);
    }
  });
});

describe('inputs too large or too composed for a corpus file', () => {
  it('handles a long run of text before an open span', () => {
    // Ten thousand characters, then an unterminated span at the end. The sweep
    // would need a parse per byte, so this asserts the shape instead.
    const long = `${'a'.repeat(10_000)} **bold`;
    const block = parse(long).blocks[0]!;
    expect(block.kind).toBe(KIND.paragraph);
    expect(block.runs).toEqual([]);
    expect(block.text).toBe(long);
  });

  it('streams a long run of text to the same document', () => {
    const long = `${'a'.repeat(10_000)} **bold`;
    assertStreamingBatchMatchesFullParse([long.split('')]);
  });

  it('keeps LaTeX literal while the bold around it completes', () => {
    // A composed document rather than a single literal, because that is the
    // shape it has in the suite it came from. Math stays literal by policy, and
    // the span beside it settles when its closer arrives.
    const document = [
      '> Given tokens \\(x_1, ..., x_t\\), maximize \\(P(x_{t+1} | x_1, ..., x_t)\\)',
      '',
      '**2. Instruction Tuning (SFT)**',
      '- Dataset size is much smaller than pre-training',
    ].join('\n');

    const finished = parse(document);
    expect(finished.blocks.map((b) => b.text)).toEqual([
      // The `\\(` and `\\)` are escaped punctuation, so the delimiters are
      // consumed and the reader sees bare parens. The math itself stays
      // literal either way, which is the point.
      'Given tokens (x_1, ..., x_t), maximize (P(x_{t+1} | x_1, ..., x_t))',
      '2. Instruction Tuning (SFT)',
      'Dataset size is much smaller than pre-training',
    ]);
    expect(
      stylesOf(finished.blocks[1]!, 'Instruction Tuning').some(
        (r) => r[2] & STYLE.strong
      )
    ).toBe(true);

    // Cut mid-word and the span is open, so it stays literal.
    const partial = document.slice(
      0,
      document.indexOf('(SFT)') + '(SFT'.length
    );
    const open = parse(partial);
    expect(open.blocks[1]?.text).toBe('**2. Instruction Tuning (SFT');
    expect(open.blocks[1]?.runs).toEqual([]);
    assertStreamingBatchMatchesFullParse([partial.split('')]);
  });
});

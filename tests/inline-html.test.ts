/* eslint-disable no-bitwise -- parse options and inline styles are bitmasks */
/**
 * `<br>`, `<sub>` and `<sup>` through the engine that ships in the app.
 *
 * `src/cpp/tests/engine.test.mjs` holds the same grammar and break tables
 * against the same C++ parse, one layer down. These cases run through the
 * JavaScript harness the view tests use, so a document rendered by a view is
 * the document these describe: the same driver, the same flags, the same
 * offsets.
 *
 * The opt-in md4c syntaxes are checked here too, because the bit that turns
 * them on is one a caller can set from JavaScript and nothing else would notice
 * it resolving.
 */
import { describe, expect, it } from 'vitest';

import {
  assertStreamingMatchesFullParse,
  KIND,
  PARSE_INLINE_HTML,
  PARSE_SUBSCRIPT_SYNTAX,
  PARSE_SUPERSCRIPT_SYNTAX,
  parse,
  STYLE,
  stylesOf,
  visibleText,
} from './engine';

const ON = PARSE_INLINE_HTML;

/** [source, visible text, [[substring, style mask], …]] over one paragraph. */
const GRAMMAR: [string, string, [string, number][]][] = [
  ['H<sub>2</sub>O', 'H2O', [['2', STYLE.subscript]]],
  ['x<sup>2</sup>', 'x2', [['2', STYLE.superscript]]],
  ['a<br>b', 'a\nb', []],
  ['a<br/>b', 'a\nb', []],
  ['a<br />b', 'a\nb', []],
  ['a<BR>b', 'a\nb', []],
  ['a</br>b', 'a\nb', []],
  ['a<br clear="all">b', 'a\nb', []],
  ['<sup class="x">y</sup>', 'y', [['y', STYLE.superscript]]],
  // A stray closer and a self-closed script are swallowed, as a browser does.
  ['a</sub>b', 'ab', []],
  ['a<sub/>b', 'ab', []],
  // An unclosed tag runs to the end of its block.
  ['H<sub>2 open', 'H2 open', [['2 open', STYLE.subscript]]],
  // An escaped opener leaves a stray closer, which is swallowed.
  ['\\<sub>y</sub>', '<sub>y', []],
  // The innermost tag wins, and nesting one kind twice does not shift further.
  [
    '<sup>a<sub>b</sub>c</sup>d',
    'abcd',
    [
      ['a', STYLE.superscript],
      ['b', STYLE.subscript],
      ['c', STYLE.superscript],
      ['d', 0],
    ],
  ],
  ['<sup>**y**</sup>', 'y', [['y', STYLE.superscript | STYLE.strong]]],
  // A name that merely starts with one is not one.
  ['<subtitle>n</subtitle>', '<subtitle>n</subtitle>', []],
  // Every other tag stays literal, and code is verbatim.
  ['a <b>bold</b>', 'a <b>bold</b>', []],
  ['`<sub>x</sub>`', '<sub>x</sub>', [['<sub>x</sub>', STYLE.code]]],
];

describe('the three tags', () => {
  for (const [source, expected, runs] of GRAMMAR) {
    it(`renders ${JSON.stringify(source)}`, () => {
      const model = parse(source, ON);
      expect(visibleText(model)).toBe(expected);
      const block = model.blocks[0]!;
      for (const [substring, mask] of runs) {
        const bits = stylesOf(block, substring).reduce(
          (all, run) => all | run[2],
          0
        );
        expect(bits & mask, substring).toBe(mask);
      }
    });
  }

  it('keeps a tag in a table cell and drops the spaces around it', () => {
    const model = parse(
      '| Step | Notes |\n| --- | --- |\n| 1 | Mix <br> Then bake |\n',
      ON
    );
    const row = model.blocks.at(-1)!;
    expect(row.kind).toBe(KIND.table);
    const [start, end] = row.cells[1]!;
    expect(row.text.slice(start, end)).toBe('Mix\nThen bake');
  });

  it('turns a block of only tags into an empty paragraph', () => {
    const model = parse('before\n\n<br>\n<br>\n\nafter\n', ON);
    expect(model.blocks).toHaveLength(3);
    expect(model.blocks[1]!.kind).toBe(KIND.paragraph);
    // One tag is one empty line, so two tags are one newline.
    expect(model.blocks[1]!.text).toBe('\n');
    // Beside a real tag the html block keeps its source.
    const mixed = parse('before\n\n<br>\n<div>x</div>\n\nafter\n', ON);
    expect(mixed.blocks[1]!.kind).toBe(KIND.html);
  });

  it('raises a footnote reference once, not twice', () => {
    const model = parse('<sup>[^1]</sup>\n\n[^1]: note\n', ON);
    const run = model.blocks[0]!.runs[0]!;
    expect(run[2] & STYLE.footnote).toBe(STYLE.footnote);
    expect(run[2] & STYLE.superscript).toBe(0);
  });

  it('keeps tags out of alt text', () => {
    const model = parse('![a<sup>2</sup>b](https://example.com/i.png)', ON);
    expect(model.images[0]!.alt).toBe('a2b');
  });

  it('holds every case to the incremental rule', () => {
    for (const [source] of GRAMMAR) {
      assertStreamingMatchesFullParse([...source]);
    }
  });
});

describe('line breaks', () => {
  const CASES: [string, string][] = [
    ['a<br>b', 'a\nb'],
    ['a<br>\nb', 'a\nb'],
    ['a\n<br>b', 'a\nb'],
    ['a\n<br>\nb', 'a\nb'],
    ['a<br><br>b', 'a\n\nb'],
    ['a<br>\n<br>\nb', 'a\n\nb'],
    ['a\n<br>\n<br>\nb', 'a\n\nb'],
    ['a  \n<br>b', 'a\n\nb'],
    ['a <br> b', 'a\nb'],
    ['a<br>', 'a'],
    ['a<br><br>', 'a\n'],
  ];

  for (const [source, expected] of CASES) {
    it(`breaks ${JSON.stringify(source)} once per hard mark`, () => {
      expect(visibleText(parse(source, ON))).toBe(expected);
    });
  }

  it('holds the break cases to the incremental rule', () => {
    for (const [source] of CASES) {
      assertStreamingMatchesFullParse(source.split(/(?<=\n)/));
    }
  });
});

describe('the opt-in syntaxes', () => {
  it('leaves ^x^ alone until asked for', () => {
    expect(visibleText(parse('2^10 and 3^5', ON))).toBe('2^10 and 3^5');
    // md4c pairs the carets greedily once the flag is on, which is exactly why
    // it is not the default: exponents in prose are common.
    expect(
      visibleText(parse('2^10 and 3^5', ON | PARSE_SUPERSCRIPT_SYNTAX))
    ).toBe('210 and 35');
  });

  it('leaves ~x~ as strikethrough until asked for', () => {
    // remark reads `~x~` as strikethrough, and so does md4c's GitHub dialect.
    const strike = parse('~strike~', ON);
    expect(
      stylesOf(strike.blocks[0]!, 'strike')[0]![2] & STYLE.strikethrough
    ).toBe(STYLE.strikethrough);
    const script = parse('~x~', ON | PARSE_SUBSCRIPT_SYNTAX);
    expect(stylesOf(script.blocks[0]!, 'x')[0]![2] & STYLE.subscript).toBe(
      STYLE.subscript
    );
  });
});

describe('with inlineHtml off', () => {
  it('shows every tag as the source text it was', () => {
    for (const source of [
      'a<br>b',
      'H<sub>2</sub>O',
      'x<sup>2</sup>',
      // The emphasis inside the tag is still emphasis when the tag is text, so
      // this case is the parser's own answer rather than the source echoed.
      '<sup>y</sup>',
    ]) {
      expect(visibleText(parse(source, 0))).toBe(source);
      expect(visibleText(parse(source))).toBe(source);
    }
    // A strong span around a tag is bold either way; only the tag changes.
    expect(
      stylesOf(parse('**a<sup>b</sup>c**', 0).blocks[0]!, 'b')[0]![2] &
        STYLE.strong
    ).toBe(STYLE.strong);
    expect(
      stylesOf(parse('**a<sup>b</sup>c**', ON).blocks[0]!, 'b')[0]![2] &
        STYLE.superscript
    ).toBe(STYLE.superscript);
  });

  it('is byte-identical to a document with none of the five tags', () => {
    // The claim behind making the bit default on: nothing else moves.
    const sources = [
      'a paragraph\n',
      '| a | b |\n| - | - |\n| 1 | 2 |\n',
      '```ts\nconst x = 1;\n```\n',
      '- one\n- two\n',
      '> quoted\n\n[^1]: note\n',
    ];
    for (const source of sources) {
      expect(parse(source, ON)).toEqual(parse(source, 0));
    }
  });
});

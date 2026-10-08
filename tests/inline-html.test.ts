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

import type { Block, RenderModel } from './engine';
import {
  assertStreamingBatchMatchesFullParse,
  assertStreamingMatchesFullParse,
  CONTAINER,
  hasStyle,
  KIND,
  PARSE_INLINE_HTML,
  PARSE_LITERAL_TAG_CONTENT,
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
  // A name that merely starts with a real one is not one.
  ['<supplier>n</supplier>', '<supplier>n</supplier>', []],
  // Phase 4: the styles, the unwraps, the drop, the comment and the anchor.
  ['a <b>bold</b>', 'a bold', [['bold', STYLE.strong]]],
  [
    '<mark>m</mark><u>u</u><ins>i</ins>',
    'mui',
    [
      ['m', STYLE.highlight],
      ['u', STYLE.underline],
      ['i', STYLE.underline],
    ],
  ],
  ['a <span>b</span> c', 'a b c', []],
  ['a <!-- note --> b', 'a  b', []],
  ['<!-- note -->', '', []],
  ['a <script>x</script> b', 'a  b', []],
  ['<a href="https://x.com">x</a>', 'x', [['x', STYLE.link]]],
  // A name that is not an HTML element is a generic, not markup.
  ['Array<string> <T> <Vector<int>>', 'Array<string> <T> <Vector<int>>', []],
  // Code is verbatim, so a tag inside it is content.
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

  it('splits the one-block form: summary from the tag, body as Markdown', () => {
    // No blank lines, so md4c delivers one HTML block — and the body is parsed
    // as Markdown rather than shown as source, which goes past GitHub and
    // Streamdown. Models write it this way.
    const model = parse(
      '<details><summary>Click</summary>Body **text**.</details>\n',
      ON
    );
    expect(visibleText(model)).toBe('Click\nBody text.');
    expect(model.blocks).toHaveLength(2);
    const [summary, body] = model.blocks as [Block, Block];
    expect(
      summary.containers.find((c) => c.kind === CONTAINER.details)?.showsMarker
    ).toBe(true);
    // The body's emphasis parsed: this is what "parsed as Markdown" proves.
    expect(body.text).toBe('Body text.');
  });

  it("reads the writer's open attribute in the one-block form", () => {
    const model = parse('<details open><summary>A</summary>B.</details>\n', ON);
    expect(
      model.blocks[0]!.containers.find((c) => c.kind === CONTAINER.details)
        ?.open
    ).toBe(true);
    expect(visibleText(model)).toBe('A\nB.');
  });

  it('summarises a summary-less one-block form as Details', () => {
    // What a browser shows for `<details>body</details>`: the disclosure still
    // needs a name.
    const model = parse('<details>Body only.</details>\n', ON);
    expect(visibleText(model)).toBe('Details\nBody only.');
  });

  it('holds every case to the incremental rule', () => {
    // One spawn for the whole table: a spawn runs the binary's own suite
    // beside the fixture, so a spawn per case spends the run there rather
    // than on the rule this test names.
    assertStreamingBatchMatchesFullParse(
      GRAMMAR.map(([source]) => [...source])
    );
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
    assertStreamingBatchMatchesFullParse(
      CASES.map(([source]) => source.split(/(?<=\n)/))
    );
  });

  it('clips only trailing styled whitespace while preserving earlier runs', () => {
    const prefix = 'H<sub>2</sub>O **bold** <br> '.repeat(128);
    const model = parse(
      `${prefix}<sup>😀e\u0301 \t</sup><sub> \t</sub><br>終`,
      ON
    );
    const block = model.blocks[0]!;
    expect(block.text).toBe(`${'H2O bold\n'.repeat(128)}😀e\u0301\n終`);
    expect(block.runs).toHaveLength(257);
    expect(stylesOf(block, '😀e\u0301')[0]![2]).toBe(STYLE.superscript);
    for (const [index, run] of block.runs.entries()) {
      expect(run[0]).toBeLessThan(run[1]);
      expect(run[1]).toBeLessThanOrEqual(block.text.length);
      if (index > 0)
        expect(block.runs[index - 1]![1]).toBeLessThanOrEqual(run[0]);
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

/**
 * `<details>` and `<summary>`.
 *
 * md4c gives an HTML block up to a blank line, so the tags and the body arrive as
 * separate blocks and the pairing is the builder's job. These cases pin the four
 * decisions that pairing makes: a container per opener, nesting, a summary that is
 * a synthetic first leaf, and a closer with no opener dropped rather than shown.
 */
describe('<details>', () => {
  const DETAILS = [
    '<details>',
    '<summary>Click</summary>',
    '',
    'Body **text**.',
    '',
    '</details>',
    '',
  ].join('\n');

  it('turns the tags into a container around the body', () => {
    const model = parse(DETAILS, ON);
    expect(visibleText(model)).toBe('Click\nBody text.');
    // The summary and the body are the two blocks the reader sees, and neither
    // carries the tags: a `<details>` is a control, not source text.
    expect(model.blocks).toHaveLength(2);
    for (const block of model.blocks) {
      expect(
        block.containers.some(
          (c: { kind: number }) => c.kind === CONTAINER.details
        )
      ).toBe(true);
    }
  });

  it("marks the summary as the container's own leaf, so the chevron has a home", () => {
    const model = parse(DETAILS, ON);
    const [summary, body] = model.blocks as [Block, Block];
    const detailsOf = (block: {
      containers: { kind: number; showsMarker: boolean }[];
    }) => block.containers.find((c) => c.kind === CONTAINER.details);
    expect(detailsOf(summary)?.showsMarker).toBe(true);
    // Only the first leaf carries it: a marker is drawn once per container, the
    // same rule a list item's bullet follows.
    expect(detailsOf(body)?.showsMarker).toBe(false);
  });

  it("keeps the writer's open attribute as the starting state", () => {
    const opened = parse(DETAILS.replace('<details>', '<details open>'), ON);
    const closed = parse(DETAILS, ON);
    const openOf = (model: RenderModel) =>
      model.blocks[0]!.containers.find((c) => c.kind === CONTAINER.details)
        ?.open;
    expect(openOf(opened)).toBe(true);
    expect(openOf(closed)).toBe(false);
    // `open="open"` is the same attribute as a bare `open`, which is what HTML
    // says and what a writer typing it meant.
    expect(
      openOf(parse(DETAILS.replace('<details>', '<details open="open">'), ON))
    ).toBe(true);
    // An attribute that merely starts with the name is not the attribute.
    expect(
      openOf(parse(DETAILS.replace('<details>', '<details opened>'), ON))
    ).toBe(false);
  });

  it('nests, and each level names its own summary', () => {
    const nested = [
      '<details>',
      '<summary>Outer</summary>',
      '',
      '<details>',
      '<summary>Inner</summary>',
      '',
      'Deep.',
      '',
      '</details>',
      '',
      '</details>',
      '',
    ].join('\n');
    const model = parse(nested, ON);
    expect(visibleText(model)).toBe('Outer\nInner\nDeep.');
    // Two containers, and the inner block is inside both — which is what makes
    // the outer one collapsible as a whole.
    expect(
      model.blocks[2]!.containers.filter((c) => c.kind === CONTAINER.details)
    ).toHaveLength(2);
    const summaries = model.blocks
      .map((block: { containers: { kind: number; showsMarker: boolean }[] }) =>
        block.containers.find(
          (c) => c.kind === CONTAINER.details && c.showsMarker
        )
      )
      .filter(Boolean);
    expect(summaries).toHaveLength(2);
  });

  it('drops a closing tag with no opener rather than showing it as source', () => {
    // A reader cannot act on a tag whose block is not there, and Streamdown's
    // sanitizer drops it too.
    expect(visibleText(parse('Text.\n\n</details>\n\nMore.\n', ON))).toBe(
      'Text.\nMore.'
    );
  });

  it('keeps a line that merely mentions the tag as prose', () => {
    // A tag counts only when the line is nothing but the tag, so prose that names
    // one is not turned into a container.
    const model = parse('The word <details> in prose.\n', ON);
    expect(visibleText(model)).toBe('The word  in prose.');
    expect(model.blocks[0]!.containers).toHaveLength(0);
  });

  it('unwraps the tags inside a summary, so it reads as the writer meant', () => {
    const model = parse(
      '<details>\n<summary>a <code>b</code></summary>\n\nBody.\n\n</details>\n',
      ON
    );
    expect(visibleText(model)).toBe('a b\nBody.');
  });

  it('sits inside a list item when the writer put it there', () => {
    const inList = [
      '- item',
      '',
      '  <details>',
      '  <summary>Click</summary>',
      '',
      '  Body.',
      '',
      '  </details>',
      '',
    ].join('\n');
    const model = parse(inList, ON);
    const body = model.blocks[2]!;
    // The list item's frame is still the outer one, so the collapsible block is
    // indented and bulleted exactly as it was before it became a container.
    expect(body.containers.map((c) => c.kind)).toEqual([
      CONTAINER.listItem,
      CONTAINER.details,
    ]);
  });

  it('is a plain HTML block with inline HTML off', () => {
    // The tags are HTML, so turning HTML off turns the container off with it and
    // the source is what a reader sees.
    const model = parse(DETAILS, 0);
    expect(visibleText(model)).toContain('<details>');
  });

  it('keeps a closed body in the text a copy would produce', () => {
    // Collapsing is a drawing decision and the document is untouched, so the
    // export and a selection either side of the toggle produce the same words.
    // That is the property that lets the toggle be a height change and nothing
    // else: a reader who copies a closed block gets its body.
    const model = parse(DETAILS, ON);
    expect(visibleText(model)).toBe('Click\nBody text.');
    expect(model.blocks.map((block) => block.text)).toEqual([
      'Click',
      'Body text.',
    ]);
  });

  it('keeps a closed body in the text a copy would produce', () => {
    // Collapsing is a drawing decision and the document is untouched, so the
    // export and a selection either side of the toggle produce the same words.
    // That is the property that lets the toggle be a height change and nothing
    // else: a reader who copies a closed block gets its body.
    const model = parse(DETAILS, ON);
    expect(visibleText(model)).toBe('Click\nBody text.');
    expect(model.blocks.map((block) => block.text)).toEqual([
      'Click',
      'Body text.',
    ]);
  });

  it('keeps a closed body in the text a copy would produce', () => {
    // Collapsing is a drawing decision and the document is untouched, so the
    // export and a selection either side of the toggle produce the same words.
    // That is the property that lets the toggle be a height change and nothing
    // else: a reader who copies a closed block gets its body.
    expect(parse(DETAILS, ON).blocks.map((block) => block.text)).toEqual([
      'Click',
      'Body text.',
    ]);
  });

  it('splits the one-block form: summary from the tag, body as Markdown', () => {
    // No blank lines, so md4c delivers one HTML block — and the body is parsed
    // as Markdown rather than shown as source, which goes past GitHub and
    // Streamdown. Models write it this way.
    const model = parse(
      '<details><summary>Click</summary>Body **text**.</details>\n',
      ON
    );
    expect(visibleText(model)).toBe('Click\nBody text.');
    expect(model.blocks).toHaveLength(2);
    const [summary, body] = model.blocks as [Block, Block];
    expect(
      summary.containers.find((c) => c.kind === CONTAINER.details)?.showsMarker
    ).toBe(true);
    // The body's emphasis parsed: this is what "parsed as Markdown" proves.
    expect(body.text).toBe('Body text.');
  });

  it("reads the writer's open attribute in the one-block form", () => {
    const model = parse('<details open><summary>A</summary>B.</details>\n', ON);
    expect(
      model.blocks[0]!.containers.find((c) => c.kind === CONTAINER.details)
        ?.open
    ).toBe(true);
    expect(visibleText(model)).toBe('A\nB.');
  });

  it('summarises a summary-less one-block form as Details', () => {
    // What a browser shows for `<details>body</details>`: the disclosure still
    // needs a name.
    const model = parse('<details>Body only.</details>\n', ON);
    expect(visibleText(model)).toBe('Details\nBody only.');
  });

  it('holds every case to the incremental rule', () => {
    // One byte at a time, because a collapsible block is the case the rule exists
    // for: the tags arrive as their own blocks, so an unstable tail that committed
    // between them would show a summary with no body and a body with no summary.
    assertStreamingMatchesFullParse([...DETAILS]);
    assertStreamingMatchesFullParse([
      ...'<details><summary>Click</summary>Body **text**.</details>\n',
    ]);
    assertStreamingMatchesFullParse([
      ...'<details open>\n<summary>a <code>b</code></summary>\n\nBody.\n\n</details>\n',
    ]);
  });
});

/**
 * A tag the app allowed, used as a *block* container.
 *
 * The same frame a `<details>` is: the pair brackets the blocks between them, and
 * the container names the tag so a React child can be mounted in its place. What
 * makes it different is that the app chose the tag, so the policy has to name it
 * — without one, the tag is an unknown element and the source stays the source.
 */
describe('an allowed block tag', () => {
  const USER = { user: [] as readonly string[] };

  it('brackets the blocks between the pair', () => {
    const model = parse('<user>\n\nHello **there**.\n\n</user>\n', ON, USER);
    expect(visibleText(model)).toBe('Hello there.');
    // One block, inside one container that names the tag: the index is the
    // document's own record, so a child knows what it is standing in.
    expect(model.blocks).toHaveLength(1);
    expect(model.blocks[0]!.containers.map((c) => [c.kind, c.tag])).toEqual([
      [CONTAINER.custom, 0],
    ]);
  });

  it('leaves an unallowed tag as the source it was', () => {
    // The same document with no policy: an unknown element is unwrapped as
    // source, which is what phase 4 decided and 5.2 does not change.
    expect(visibleText(parse('<user>\n\nHello.\n\n</user>\n', ON))).toBe(
      '<user>\nHello.\n</user>'
    );
  });

  it('closes the innermost frame whatever tag named the closer', () => {
    // HTML has no rule for a mismatched closer and CommonMark's own containers
    // behave the same way, so `</user>` inside a `<details>` ends the details. The
    // alternative — matching closers by name — costs a line that then has to be put
    // back into the block, and buys a case no writer means.
    const model = parse(
      '<details>\n<summary>S</summary>\n\n</user>\n\nBody.\n\n</details>\n',
      ON,
      USER
    );
    expect(visibleText(model)).toBe('S\nBody.');
    // The details was popped by the stray closer, so the closing `</details>` had
    // nothing left to close and `Body.` is back at the top level.
    expect(
      model.blocks[1]!.containers.some((c) => c.kind === CONTAINER.details)
    ).toBe(false);
  });

  it('splits the one-block form: summary from the tag, body as Markdown', () => {
    // No blank lines, so md4c delivers one HTML block — and the body is parsed
    // as Markdown rather than shown as source, which goes past GitHub and
    // Streamdown. Models write it this way.
    const model = parse(
      '<details><summary>Click</summary>Body **text**.</details>\n',
      ON
    );
    expect(visibleText(model)).toBe('Click\nBody text.');
    expect(model.blocks).toHaveLength(2);
    const [summary, body] = model.blocks as [Block, Block];
    expect(
      summary.containers.find((c) => c.kind === CONTAINER.details)?.showsMarker
    ).toBe(true);
    // The body's emphasis parsed: this is what "parsed as Markdown" proves.
    expect(body.text).toBe('Body text.');
  });

  it("reads the writer's open attribute in the one-block form", () => {
    const model = parse('<details open><summary>A</summary>B.</details>\n', ON);
    expect(
      model.blocks[0]!.containers.find((c) => c.kind === CONTAINER.details)
        ?.open
    ).toBe(true);
    expect(visibleText(model)).toBe('A\nB.');
  });

  it('summarises a summary-less one-block form as Details', () => {
    // What a browser shows for `<details>body</details>`: the disclosure still
    // needs a name.
    const model = parse('<details>Body only.</details>\n', ON);
    expect(visibleText(model)).toBe('Details\nBody only.');
  });

  it('holds every case to the incremental rule', () => {
    assertStreamingMatchesFullParse([...'<user>\n\nHello.\n\n</user>\n']);
  });
});

/**
 * An allowed inline tag with `literalTagContent`.
 *
 * The flag travels beside the policy rather than in it, so both cross here:
 * the bit the view resolves and the wire form `allowedTags` writes. Without
 * the bit the content is Markdown; with it the same bytes are unstyled text.
 * The tag's own record is the C++ `literalTagContentRun`'s case, because the
 * JSON runs carry no tag index to assert on here.
 */
describe('an allowed inline tag with literalTagContent', () => {
  const USER = { user: [] as readonly string[] };
  const LITERAL = ON | PARSE_LITERAL_TAG_CONTENT;

  it('parses the content without the bit', () => {
    const model = parse('a <user>**bob**</user>', ON, USER);
    expect(visibleText(model)).toBe('a bob');
    expect(hasStyle(model.blocks[0]!, 'bob', STYLE.strong)).toBe(true);
  });

  it('leaves the content unstyled with the bit', () => {
    const model = parse('a <user>**bob**</user>', LITERAL, USER);
    expect(visibleText(model)).toBe('a bob');
    expect(hasStyle(model.blocks[0]!, 'bob', STYLE.strong)).toBe(false);
  });

  it('forms no link inside with the bit', () => {
    const model = parse('<user>[x](https://e.com)</user>', LITERAL, USER);
    expect(visibleText(model)).toBe('x');
    expect(hasStyle(model.blocks[0]!, 'x', STYLE.link)).toBe(false);
    expect(model.links).toHaveLength(0);
  });
});

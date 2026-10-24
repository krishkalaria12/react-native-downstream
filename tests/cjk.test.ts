/* eslint-disable no-bitwise -- parse options are a bitmask */
/**
 * The CJK parse option, held to its own contract.
 *
 * `src/cpp/md4c/md4c.c`'s `MD_FLAG_CJKFRIENDLY` changes what a reader sees in
 * CJK text and must change nothing else. That is the property this file proves:
 * the CJK cases from the amendment, and then a differential sweep over every
 * corpus the repository holds with the bit on and off.
 *
 * Everything here goes through the engine that ships in the app, so a regression
 * fails here rather than on a device.
 */
import { describe, expect, it } from 'vitest';

import {
  CORPUS,
  brokenVariants,
  coverageGaps,
  intrawordEmphasis,
  intrawordUnderscores,
  mixedMarkers,
  singleTildeBetweenWords,
  unterminatedBold,
  unterminatedItalic,
  unterminatedStrikethrough,
} from './remend-corpus';
import {
  PARSE_CJK,
  PARSE_INLINE_HTML,
  PARSE_FLAGS_DEFAULT,
  parse,
} from './engine';

/** The flags a view sends for a document with no math, tags or syntax named. */
const VIEW = PARSE_INLINE_HTML | PARSE_CJK;
const WITHOUT_CJK = PARSE_INLINE_HTML;

/** Every block's text, runs, kind, containers and cells: the render model. */
function model(source: string, flags: number) {
  const { blocks } = parse(source, flags);
  return blocks;
}

describe('the markdown-cjk-friendly amendment', () => {
  const emphasis: ReadonlyArray<
    readonly [
      string,
      string,
      ((runs: ReadonlyArray<readonly number[]>) => boolean) | null,
    ]
  > = [
    // Worked before the amendment and must keep working.
    ['**太字**。', '太字。', (runs) => runs.length === 1],
    ['中文**粗体**中文', '中文粗体中文', (runs) => runs.length === 1],
    // The three that were literal.
    [
      'これは**「強調」**です',
      'これは「強調」です',
      (runs) => runs.length === 1,
    ],
    ['**[链接](https://x.com)**です', '链接です', (runs) => runs.length === 1],
    // CommonMark's own answers, which the amendment does not change.
    ['**“quoted”**text', '**“quoted”**text', null],
    ['a**太字', 'a**太字', null],
    // The `_` intraword rule runs first and is left alone.
    ['__テスト__です', '__テスト__です', null],
    ['これは_強調_です', 'これは_強調_です', null],
  ];

  for (const [source, text, styled] of emphasis) {
    it(`${JSON.stringify(source)} → ${JSON.stringify(text)}`, () => {
      const blocks = model(source, VIEW);
      expect(blocks.map((block) => block.text)).toEqual([text]);
      const runs = blocks[0]?.runs ?? [];
      if (styled === null) {
        expect(runs).toEqual([]);
      } else {
        expect(styled(runs)).toBe(true);
      }
    });
  }

  it('bolds a span next to a fullwidth comma', () => {
    // `。` is CJK punctuation, so it is already level 1 under md4c's own
    // classification and the amendment has nothing to add; the case is here to
    // hold that line.
    expect(model('太字**太字**。', VIEW)[0]?.text).toBe('太字太字。');
  });

  it('pairs ~~ beside a CJK neighbour and not beside a Latin one', () => {
    expect(model('これは~~削除~~です', VIEW)[0]?.text).toBe('これは削除です');
    expect(model('~~del~~ ok', VIEW)[0]?.text).toBe('del ok');
    // GFM's answer, which the amendment keeps.
    expect(model('~~del~~ok', VIEW)[0]?.text).toBe('~~del~~ok');
  });

  it('ends a bare URL before a CJK character', () => {
    // md4c's host and path scans stop at the first non-ASCII character, so the
    // URL ends where the prose starts. The third case keeps its directory
    // slash, because md4c's own rule keeps a trailing `/` and a link to
    // `example.com/` is one a reader can follow; the alternative is no link.
    const cases: ReadonlyArray<readonly [string, string]> = [
      ['請訪問 https://example.com。謝謝', 'https://example.com'],
      ['see https://example.com） ok', 'https://example.com'],
      ['見て https://example.com/中文 ok', 'https://example.com/'],
    ];
    for (const [source, url] of cases) {
      const { links, blocks } = parse(source, VIEW);
      expect(links.map((link) => link.url)).toEqual([url]);
      const block = blocks[0];
      const [run] = block?.runs ?? [];
      expect(block?.text.slice(run?.[0], run?.[1])).toBe(url);
    }
  });

  it('leaves an angle autolink alone', () => {
    expect(parse('see <https://example.com>。', VIEW).links[0]?.url).toBe(
      'https://example.com'
    );
  });
});

/**
 * The differential test.
 *
 * Every corpus the repository holds, parsed both ways, with the CJK cases
 * removed. Any difference is a change to what a reader sees that the amendment
 * does not account for, which is exactly the failure mode a patch to a vendored
 * parser has.
 */
describe('a document with no CJK in it parses identically either way', () => {
  /** Enough to catch Han, Kana, Hangul, and the fullwidth forms. */
  const HAS_CJK = /[ᄀ-ᇿ⺀-鿿ꥠ-ꥼ가-힯豈-﫿＀-￯]|[\u{16FE0}-\u{3FFFD}]/u;

  const corpora: ReadonlyArray<readonly [string, readonly string[]]> = [
    ['remend', CORPUS],
    ['broken variants', brokenVariants],
    ['coverage gaps', coverageGaps],
    ['intraword emphasis', intrawordEmphasis],
    ['intraword underscores', intrawordUnderscores],
    ['mixed markers', mixedMarkers],
    ['single tilde between words', singleTildeBetweenWords],
    ['unterminated bold', unterminatedBold],
    ['unterminated italic', unterminatedItalic],
    ['unterminated strikethrough', unterminatedStrikethrough],
  ];

  let checked = 0;
  for (const [name, corpus] of corpora) {
    it(`${name} (${corpus.length} sources)`, () => {
      for (const source of corpus) {
        if (HAS_CJK.test(source)) continue;
        checked++;
        expect(model(source, VIEW | PARSE_FLAGS_DEFAULT)).toEqual(
          model(source, WITHOUT_CJK | PARSE_FLAGS_DEFAULT)
        );
      }
    });
  }

  it('checked more than a thousand sources', () => {
    // A differential test that silently matched nothing proves nothing, and the
    // corpora grow as remend's suite grows.
    expect(checked).toBeGreaterThan(1000);
  });
});

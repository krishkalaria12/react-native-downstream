import { describe, expect, it } from 'vitest';

import {
  PARSE_FLAG_BRACKETS,
  PARSE_FLAG_CJK,
  PARSE_FLAG_ENABLED,
  PARSE_FLAG_INLINE_HTML,
  PARSE_FLAG_LINK_RELATIVE,
  PARSE_FLAG_NO_AUTOLINK_HTTP,
  PARSE_FLAG_NO_AUTOLINK_HTTPS,
  PARSE_FLAG_NO_AUTOLINK_MAILTO,
  PARSE_FLAG_SINGLE_DOLLAR,
  PARSE_FLAG_SUBSCRIPT_SYNTAX,
  PARSE_FLAG_SUPERSCRIPT_SYNTAX,
  resolveMath,
  resolveParseFlags,
} from '../src/parseFlags';
import { assertStreamingMatchesFullParse, parse } from './engine';

describe('resolveMath', () => {
  it('is off unless the engine is installed or math is asked for', () => {
    expect(resolveMath(undefined, false)).toEqual({ flags: 0, enabled: false });
  });

  it('is on with brackets when the engine is installed', () => {
    expect(resolveMath(undefined, true).flags).toBe(
      PARSE_FLAG_ENABLED | PARSE_FLAG_BRACKETS
    );
  });

  it('is exactly nothing when it is false, which is a no-op for existing apps', () => {
    expect(resolveMath(false, true)).toEqual({ flags: 0, enabled: false });
  });

  it('keeps each option on its own default', () => {
    const { flags } = resolveMath({ brackets: false }, true);
    expect(flags).toBe(PARSE_FLAG_ENABLED);
  });

  it('turns single-dollar math on only when it is named', () => {
    expect(
      resolveMath({ singleDollarTextMath: true }, false).flags &
        PARSE_FLAG_SINGLE_DOLLAR
    ).toBe(PARSE_FLAG_SINGLE_DOLLAR);
  });

  it('treats an object naming no options as true', () => {
    expect(resolveMath({}, false).flags).toBe(
      PARSE_FLAG_ENABLED | PARSE_FLAG_BRACKETS
    );
  });
});

describe('resolveParseFlags', () => {
  const base = {
    math: false as const,
    inlineHtml: undefined,
    syntax: undefined,
    cjk: undefined,
    links: undefined,
    autolinkProtocols: undefined,
  };

  it('renders the inline tags unless inlineHtml is false', () => {
    expect(resolveParseFlags(base, false).flags).toBe(
      PARSE_FLAG_INLINE_HTML | PARSE_FLAG_CJK
    );
    expect(
      resolveParseFlags({ ...base, inlineHtml: true }, false).flags &
        PARSE_FLAG_INLINE_HTML
    ).toBe(PARSE_FLAG_INLINE_HTML);
    expect(resolveParseFlags({ ...base, inlineHtml: false }, false).flags).toBe(
      PARSE_FLAG_CJK
    );
  });

  it('keeps the math bits exactly as resolveMath resolves them', () => {
    const { flags, enabled } = resolveParseFlags(
      { ...base, math: { singleDollarTextMath: true } },
      true
    );
    expect(enabled).toBe(true);
    expect(flags & PARSE_FLAG_ENABLED).toBe(PARSE_FLAG_ENABLED);
    expect(flags & PARSE_FLAG_SINGLE_DOLLAR).toBe(PARSE_FLAG_SINGLE_DOLLAR);
    expect(flags & PARSE_FLAG_BRACKETS).toBe(PARSE_FLAG_BRACKETS);
    expect(flags & PARSE_FLAG_INLINE_HTML).toBe(PARSE_FLAG_INLINE_HTML);
  });

  it('leaves the md4c syntax off unless it is named', () => {
    const off = resolveParseFlags(base, false).flags;
    expect(off & PARSE_FLAG_SUPERSCRIPT_SYNTAX).toBe(0);
    expect(off & PARSE_FLAG_SUBSCRIPT_SYNTAX).toBe(0);
    // An empty object names no syntax, so it asks for nothing.
    expect(resolveParseFlags({ ...base, syntax: {} }, false).flags).toBe(
      PARSE_FLAG_INLINE_HTML | PARSE_FLAG_CJK
    );
  });

  it('sets each syntax bit only when asked for', () => {
    expect(
      resolveParseFlags({ ...base, syntax: { superscript: true } }, false)
        .flags & PARSE_FLAG_SUPERSCRIPT_SYNTAX
    ).toBe(PARSE_FLAG_SUPERSCRIPT_SYNTAX);
    expect(
      resolveParseFlags({ ...base, syntax: { subscript: true } }, false).flags &
        PARSE_FLAG_SUBSCRIPT_SYNTAX
    ).toBe(PARSE_FLAG_SUBSCRIPT_SYNTAX);
    expect(
      resolveParseFlags(
        { ...base, syntax: { superscript: true, subscript: true } },
        false
      ).flags
    ).toBe(
      PARSE_FLAG_INLINE_HTML |
        PARSE_FLAG_CJK |
        PARSE_FLAG_SUPERSCRIPT_SYNTAX |
        PARSE_FLAG_SUBSCRIPT_SYNTAX
    );
  });

  it('reports math as unavailable when the engine is not installed', () => {
    // The tags and CJK are on by default whatever the math engine is doing, so
    // the two options are independent.
    expect(resolveParseFlags({ ...base, math: undefined }, false)).toEqual({
      flags: PARSE_FLAG_INLINE_HTML | PARSE_FLAG_CJK,
      enabled: false,
    });
  });

  it('ignores values that are not the shapes the options allow', () => {
    const flags = resolveParseFlags(
      {
        ...base,
        math: 'yes' as unknown as boolean,
        syntax: null as unknown as undefined,
      },
      false
    ).flags;
    // A truthy value that names no options is `true`, so math is on with its
    // defaults, and nothing throws.
    expect(flags & PARSE_FLAG_ENABLED).toBe(PARSE_FLAG_ENABLED);
    expect(flags & PARSE_FLAG_BRACKETS).toBe(PARSE_FLAG_BRACKETS);
    expect(flags & PARSE_FLAG_INLINE_HTML).toBe(PARSE_FLAG_INLINE_HTML);
    expect(flags & PARSE_FLAG_SUPERSCRIPT_SYNTAX).toBe(0);
  });

  it('turns CJK off only for an explicit false', () => {
    expect(
      resolveParseFlags({ ...base, cjk: true }, false).flags & PARSE_FLAG_CJK
    ).toBe(PARSE_FLAG_CJK);
    expect(
      resolveParseFlags({ ...base, cjk: false }, false).flags & PARSE_FLAG_CJK
    ).toBe(0);
  });

  it('turns relative links on for relative: true or for an origin', () => {
    expect(
      resolveParseFlags(base, false).flags & PARSE_FLAG_LINK_RELATIVE
    ).toBe(0);
    expect(
      resolveParseFlags({ ...base, links: { relative: true } }, false).flags &
        PARSE_FLAG_LINK_RELATIVE
    ).toBe(PARSE_FLAG_LINK_RELATIVE);
    expect(
      resolveParseFlags(
        { ...base, links: { defaultOrigin: 'https://example.com' } },
        false
      ).flags & PARSE_FLAG_LINK_RELATIVE
    ).toBe(PARSE_FLAG_LINK_RELATIVE);
  });

  it('normalizes a disabled protocol as Streamdown does', () => {
    // Trimmed, lower cased, and the colon is optional, so all three spellings
    // are the same entry.
    for (const spelling of ['mailto', 'MAILTO', 'mailto:', '  MailTo:  ']) {
      expect(
        resolveParseFlags({ ...base, autolinkProtocols: [spelling] }, false)
          .flags & PARSE_FLAG_NO_AUTOLINK_MAILTO
      ).toBe(PARSE_FLAG_NO_AUTOLINK_MAILTO);
    }
    expect(
      resolveParseFlags({ ...base, autolinkProtocols: ['mailto'] }, false)
        .flags & PARSE_FLAG_NO_AUTOLINK_HTTP
    ).toBe(0);
    expect(
      resolveParseFlags(
        { ...base, autolinkProtocols: ['http', 'https'] },
        false
      ).flags &
        (PARSE_FLAG_NO_AUTOLINK_HTTP | PARSE_FLAG_NO_AUTOLINK_HTTPS)
    ).toBe(PARSE_FLAG_NO_AUTOLINK_HTTP | PARSE_FLAG_NO_AUTOLINK_HTTPS);
  });

  it('ignores a non-array protocol list rather than throwing', () => {
    expect(
      resolveParseFlags(
        {
          ...base,
          autolinkProtocols: 'mailto' as unknown as readonly string[],
        },
        false
      ).flags & PARSE_FLAG_NO_AUTOLINK_MAILTO
    ).toBe(0);
    expect(
      resolveParseFlags(
        {
          ...base,
          autolinkProtocols: [null as unknown as string, 'mailto'],
        },
        false
      ).flags & PARSE_FLAG_NO_AUTOLINK_MAILTO
    ).toBe(PARSE_FLAG_NO_AUTOLINK_MAILTO);
  });
});

/**
 * The layout direction, decided once in the engine.
 *
 * A block reads the way the majority of its strong characters read, and on a tie
 * the way its first strong one does. The engine's own table is pinned in
 * `testDirection` in `src/cpp/tests/test_export.cpp`; these are the cases that
 * came from the two platforms disagreeing with each other.
 */
describe('the layout direction', () => {
  const LTR = 1;
  const RTL = 2;

  const directions = (source: string) =>
    parse(source).blocks.map((block) => block.direction);

  it('reads a paragraph the way its words read', () => {
    expect(directions('hello world\n')).toEqual([LTR]);
    expect(directions('שלום עולם\n')).toEqual([RTL]);
  });

  it('counts the majority, not the first character', () => {
    // The case iOS and Android disagreed on: iOS took the first strong letter and
    // Android used `Character.getDirectionality` over the whole string.
    expect(directions('one two three אב\n')).toEqual([LTR]);
    expect(directions('אבגדה one\n')).toEqual([RTL]);
    expect(directions('אבג one two three\n')).toEqual([LTR]);
  });

  it('sends a tie to the first strong character', () => {
    expect(directions('a א\n')).toEqual([LTR]);
    expect(directions('א a\n')).toEqual([RTL]);
  });

  it('lets a block with no strong character inherit the one before it', () => {
    // A line of numbers between two paragraphs takes the paragraph above it, and
    // a document that opens with one starts left to right.
    expect(directions('hello\n\n123 456\n\nworld\n')).toEqual([LTR, LTR, LTR]);
    expect(directions('שלום\n\n123\n\nעולם\n')).toEqual([RTL, RTL, RTL]);
    expect(directions('123\n\nhello\n')).toEqual([LTR, LTR]);
  });

  it('skips inline code when deciding, and a code block entirely', () => {
    // The other disagreement: a paragraph that opens with `x` then Arabic came out
    // two ways, because one platform counted the identifier and the other did not.
    expect(directions('`x` שלום\n')).toEqual([RTL]);
    // A code block takes the direction already in force rather than deciding
    // from its contents, and does not change what follows it either.
    expect(directions('```\nשלום\n```\n')).toEqual([LTR]);
    expect(directions('שלום\n\n```\nx\n```\n')).toEqual([RTL, RTL]);
  });

  it('holds every case to the incremental rule', () => {
    assertStreamingMatchesFullParse([...'hello\n\n123\n\nשלום עולם\n']);
  });
});

/**
 * md4c's own three syntaxes: `==highlight==`, `__underline__` and `||spoiler||`.
 *
 * Each behind its own bit because each changes what the delimiters mean
 * everywhere — and `__underline__` in particular takes `_` away from emphasis.
 * The bits are off by default, so a document that never asks for them parses
 * exactly as it did before they existed.
 */
describe('the extra syntaxes', () => {
  const HIGHLIGHT = 1 << 11;
  const UNDERLINE = 1 << 12;
  const SPOILER = 1 << 13;

  const styles = (source: string, flags: number) =>
    parse(source, flags).blocks[0]!.runs.map((run) => run[2]);

  it('leaves all three alone until asked for', () => {
    expect(styles('a ==b== c', 0)).toEqual([]);
    // `__b__` without the bit is Strong, which is what it has always been: the
    // bit does not turn the syntax off, it changes what it means.
    expect(styles('a __b__ c', 0)).toEqual([1]);
    expect(styles('a ||b|| c', 0)).toEqual([]);
  });

  it('marks highlighted text once the bit is on', () => {
    expect(styles('a ==b== c', HIGHLIGHT)).toEqual([HIGHLIGHT]);
    // And off again with any other bit: the three do not imply each other.
    expect(styles('a ==b== c', UNDERLINE)).toEqual([]);
  });

  it('underlines once the bit is on, and takes `_` from emphasis with it', () => {
    expect(styles('a __b__ c', UNDERLINE)).toEqual([UNDERLINE]);
    // `_x_` is emphasis without the bit and underline with it, which is why the
    // bit is never on unless the app asked for it by name.
    expect(styles('a _b_ c', 0)).toEqual([2]);
    expect(styles('a _b_ c', UNDERLINE)).toEqual([UNDERLINE]);
  });

  it('marks spoilers once the bit is on', () => {
    expect(styles('a ||b|| c', SPOILER)).toEqual([SPOILER]);
    expect(styles('a ||b|| c', 0)).toEqual([]);
  });

  it('resolves the three from the syntax options', () => {
    const base = {
      math: false as const,
      inlineHtml: undefined,
      syntax: undefined,
      cjk: undefined,
      links: undefined,
      autolinkProtocols: undefined,
    };
    expect(
      resolveParseFlags({ ...base, syntax: { highlight: true } }, false).flags &
        HIGHLIGHT
    ).toBe(HIGHLIGHT);
    expect(
      resolveParseFlags({ ...base, syntax: { underline: true } }, false).flags &
        UNDERLINE
    ).toBe(UNDERLINE);
    expect(
      resolveParseFlags({ ...base, syntax: { spoiler: true } }, false).flags &
        SPOILER
    ).toBe(SPOILER);
    expect(
      resolveParseFlags(base, false).flags & (HIGHLIGHT | UNDERLINE | SPOILER)
    ).toBe(0);
  });
});

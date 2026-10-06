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

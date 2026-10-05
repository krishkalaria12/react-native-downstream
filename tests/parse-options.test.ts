/* eslint-disable no-bitwise -- the native `parseFlags` prop is a bitmask */
/**
 * The parse options, resolved into the flags the parser reads.
 *
 * Every option keeps the default its type documents, whichever other option is
 * named alongside it, because the flags decide what the parser treats as math,
 * as a line break, and as a script run.
 */
import { describe, expect, it } from 'vitest';

import {
  PARSE_FLAG_BRACKETS,
  PARSE_FLAG_ENABLED,
  PARSE_FLAG_INLINE_HTML,
  PARSE_FLAG_SINGLE_DOLLAR,
  PARSE_FLAG_SUBSCRIPT_SYNTAX,
  PARSE_FLAG_SUPERSCRIPT_SYNTAX,
  resolveMath,
  resolveParseFlags,
} from '../src/parseFlags';

describe('resolveMath', () => {
  it('is off when the engine is missing and nothing was asked for', () => {
    expect(resolveMath(undefined, false)).toEqual({ flags: 0, enabled: false });
  });

  it('is on with brackets when the engine is installed', () => {
    expect(resolveMath(undefined, true).flags).toBe(
      PARSE_FLAG_ENABLED | PARSE_FLAG_BRACKETS
    );
  });

  it('is off for false, even with an engine', () => {
    expect(resolveMath(false, true)).toEqual({ flags: 0, enabled: false });
  });

  it('leaves single dollars off when another option is named', () => {
    const { flags } = resolveMath({ brackets: false }, true);
    expect(flags & PARSE_FLAG_SINGLE_DOLLAR).toBe(0);
    expect(flags & PARSE_FLAG_BRACKETS).toBe(0);
    expect(flags & PARSE_FLAG_ENABLED).toBe(PARSE_FLAG_ENABLED);
  });

  it('turns single dollars on only when asked', () => {
    expect(
      resolveMath({ singleDollarTextMath: true }, false).flags &
        PARSE_FLAG_SINGLE_DOLLAR
    ).toBe(PARSE_FLAG_SINGLE_DOLLAR);
    expect(resolveMath({}, false).flags).toBe(
      PARSE_FLAG_ENABLED | PARSE_FLAG_BRACKETS
    );
  });
});

describe('resolveParseFlags', () => {
  it('renders the three tags unless inlineHtml is false', () => {
    expect(resolveParseFlags(false, undefined, undefined, false).flags).toBe(
      PARSE_FLAG_INLINE_HTML
    );
    expect(
      resolveParseFlags(false, true, undefined, false).flags &
        PARSE_FLAG_INLINE_HTML
    ).toBe(PARSE_FLAG_INLINE_HTML);
    expect(resolveParseFlags(false, false, undefined, false).flags).toBe(0);
  });

  it('keeps the math bits exactly as resolveMath resolves them', () => {
    const { flags, enabled } = resolveParseFlags(
      { singleDollarTextMath: true },
      undefined,
      undefined,
      true
    );
    expect(enabled).toBe(true);
    expect(flags & PARSE_FLAG_ENABLED).toBe(PARSE_FLAG_ENABLED);
    expect(flags & PARSE_FLAG_SINGLE_DOLLAR).toBe(PARSE_FLAG_SINGLE_DOLLAR);
    expect(flags & PARSE_FLAG_BRACKETS).toBe(PARSE_FLAG_BRACKETS);
    expect(flags & PARSE_FLAG_INLINE_HTML).toBe(PARSE_FLAG_INLINE_HTML);
  });

  it('leaves the md4c syntax off unless it is named', () => {
    const off = resolveParseFlags(false, undefined, undefined, false).flags;
    expect(off & PARSE_FLAG_SUPERSCRIPT_SYNTAX).toBe(0);
    expect(off & PARSE_FLAG_SUBSCRIPT_SYNTAX).toBe(0);
    // An empty object names no syntax, so it asks for nothing.
    expect(resolveParseFlags(false, undefined, {}, false).flags).toBe(
      PARSE_FLAG_INLINE_HTML
    );
  });

  it('sets each syntax bit only when asked for', () => {
    expect(
      resolveParseFlags(false, undefined, { superscript: true }, false).flags &
        PARSE_FLAG_SUPERSCRIPT_SYNTAX
    ).toBe(PARSE_FLAG_SUPERSCRIPT_SYNTAX);
    expect(
      resolveParseFlags(false, undefined, { subscript: true }, false).flags &
        PARSE_FLAG_SUBSCRIPT_SYNTAX
    ).toBe(PARSE_FLAG_SUBSCRIPT_SYNTAX);
    expect(
      resolveParseFlags(
        false,
        undefined,
        { superscript: true, subscript: true },
        false
      ).flags
    ).toBe(
      PARSE_FLAG_INLINE_HTML |
        PARSE_FLAG_SUPERSCRIPT_SYNTAX |
        PARSE_FLAG_SUBSCRIPT_SYNTAX
    );
  });

  it('reports math as unavailable when the engine is not installed', () => {
    // The tags are on by default whatever the math engine is doing, so the two
    // options are independent.
    expect(resolveParseFlags(undefined, undefined, undefined, false)).toEqual({
      flags: PARSE_FLAG_INLINE_HTML,
      enabled: false,
    });
  });

  it('ignores values that are not the shapes the options allow', () => {
    const flags = resolveParseFlags(
      'yes' as unknown as boolean,
      undefined,
      null as unknown as undefined,
      false
    ).flags;
    // A truthy value that names no options is `true`, so math is on with its
    // defaults, and nothing throws.
    expect(flags & PARSE_FLAG_ENABLED).toBe(PARSE_FLAG_ENABLED);
    expect(flags & PARSE_FLAG_BRACKETS).toBe(PARSE_FLAG_BRACKETS);
    expect(flags & PARSE_FLAG_INLINE_HTML).toBe(PARSE_FLAG_INLINE_HTML);
    expect(flags & PARSE_FLAG_SUPERSCRIPT_SYNTAX).toBe(0);
  });
});

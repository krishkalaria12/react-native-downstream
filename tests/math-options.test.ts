/* eslint-disable no-bitwise -- the native `mathFlags` prop is a bitmask */
/**
 * The `math` prop, resolved into the flags the parser reads.
 *
 * Every option keeps the default its type documents, whichever other option is
 * named alongside it, because the flags decide what the parser treats as math.
 */
import { describe, expect, it } from 'vitest';

import {
  MATH_FLAG_BRACKETS,
  MATH_FLAG_ENABLED,
  MATH_FLAG_SINGLE_DOLLAR,
  resolveMath,
} from '../src/math';

describe('resolveMath', () => {
  it('is off when the engine is missing and nothing was asked for', () => {
    expect(resolveMath(undefined, false)).toEqual({ flags: 0, enabled: false });
  });

  it('is on with brackets when the engine is installed', () => {
    expect(resolveMath(undefined, true).flags).toBe(
      MATH_FLAG_ENABLED | MATH_FLAG_BRACKETS
    );
  });

  it('is off for false, even with an engine', () => {
    expect(resolveMath(false, true)).toEqual({ flags: 0, enabled: false });
  });

  it('leaves single dollars off when another option is named', () => {
    const { flags } = resolveMath({ brackets: false }, true);
    expect(flags & MATH_FLAG_SINGLE_DOLLAR).toBe(0);
    expect(flags & MATH_FLAG_BRACKETS).toBe(0);
    expect(flags & MATH_FLAG_ENABLED).toBe(MATH_FLAG_ENABLED);
  });

  it('turns single dollars on only when asked', () => {
    expect(
      resolveMath({ singleDollarTextMath: true }, false).flags &
        MATH_FLAG_SINGLE_DOLLAR
    ).toBe(MATH_FLAG_SINGLE_DOLLAR);
    expect(resolveMath({}, false).flags).toBe(
      MATH_FLAG_ENABLED | MATH_FLAG_BRACKETS
    );
  });
});

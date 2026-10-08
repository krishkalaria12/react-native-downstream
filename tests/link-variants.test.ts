/**
 * Per-URL link styles, resolved from `markdownStyle.linkVariants`.
 *
 * The first prefix a URL starts with wins, so ordering is meaningful. These
 * cases pin the resolution and the matching; the platforms draw from the
 * resolved arrays, and their own tests pin that they read them.
 */
import { describe, expect, it } from 'vitest';

import { linkVariantFor, resolveLinkVariants } from '../src/linkVariants';

describe('resolveLinkVariants', () => {
  it('resolves nothing when no variants are named', () => {
    expect(resolveLinkVariants(undefined)).toEqual([]);
    expect(resolveLinkVariants({})).toEqual([]);
    expect(resolveLinkVariants({ linkVariants: [] })).toEqual([]);
  });

  it('resolves one variant with its prefixes in order', () => {
    expect(
      resolveLinkVariants({
        linkVariants: [{ match: 'https://x.com', color: '#ff0000' }],
      })
    ).toEqual([
      {
        match: ['https://x.com'],
        color: expect.any(Number),
        underline: null,
        fontFamily: undefined,
        fontWeight: undefined,
      },
    ]);
  });

  it('takes several prefixes per variant, and drops an empty one', () => {
    expect(
      resolveLinkVariants({
        linkVariants: [{ match: ['https://a.com', '', 'https://b.com'] }],
      })
    ).toEqual([
      expect.objectContaining({ match: ['https://a.com', 'https://b.com'] }),
    ]);
  });

  it('keeps underline when named, and null when not', () => {
    expect(
      resolveLinkVariants({
        linkVariants: [{ match: 'x', underline: false }],
      })[0]?.underline
    ).toBe(false);
    expect(
      resolveLinkVariants({ linkVariants: [{ match: 'x' }] })[0]?.underline
    ).toBe(null);
  });
});

describe('linkVariantFor', () => {
  const variants = resolveLinkVariants({
    linkVariants: [
      { match: 'https://x.com/a', color: '#ff0000' },
      { match: 'https://', underline: false },
    ],
  });

  it('matches the first prefix that fits, not the longest', () => {
    // Ordering is meaningful: the specific host before the scheme it shares.
    expect(linkVariantFor(variants, 'https://x.com/a/1')?.color).toBe(
      variants[0]?.color
    );
    expect(linkVariantFor(variants, 'https://y.com/')?.underline).toBe(false);
  });

  it('matches nothing it was not told about', () => {
    expect(linkVariantFor(variants, 'http://y.com/')).toBe(null);
    expect(linkVariantFor([], 'https://x.com/a')).toBe(null);
    expect(linkVariantFor(variants, '')).toBe(null);
  });
});

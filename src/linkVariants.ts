import { processColor as rnProcessColor } from 'react-native';

import type { TextStyle } from 'react-native';

import type { MarkdownStyle } from './types';

const processColor = rnProcessColor as (value: unknown) => unknown;

/**
 * One link variant, resolved to what the native views draw with.
 *
 * The prefixes stay strings because matching is `startsWith` on the URL the
 * engine recorded, which is the same string a press reports. The color is
 * processed here rather than on the platform because the platforms already take
 * processed colors for every other link, and a second color pipeline for
 * variants is how one host's red becomes another host's.
 */
export type ResolvedLinkVariant = Readonly<{
  /** URL prefixes, in the order the app listed them. */
  match: ReadonlyArray<string>;
  /** A processed color, or null when the variant names none. */
  color: number | null;
  /** Whether links under this variant are underlined, when named. */
  underline: boolean | null;
  fontFamily?: string;
  fontWeight?: TextStyle['fontWeight'];
}>;

/**
 * The `linkVariants` an app named, resolved and in order.
 *
 * The first prefix a URL starts with wins, so an app lists specific hosts
 * before the scheme they share. An empty list is the common case — no variants —
 * and costs the platforms nothing, because there is nothing to match against.
 */
export function resolveLinkVariants(
  markdownStyle: MarkdownStyle | undefined
): ReadonlyArray<ResolvedLinkVariant> {
  const variants = markdownStyle?.linkVariants;
  if (!variants || variants.length === 0) {
    return [];
  }
  return variants.flatMap((variant) => {
    const match = (
      Array.isArray(variant.match) ? variant.match : [variant.match]
    ).filter((prefix) => typeof prefix === 'string' && prefix.length > 0);
    if (match.length === 0) {
      return [];
    }
    const color =
      variant.color === undefined
        ? null
        : (processColor(variant.color) as number | null);
    return [
      {
        match,
        color: typeof color === 'number' ? color : null,
        underline: variant.underline ?? null,
        fontFamily: variant.fontFamily,
        fontWeight: variant.fontWeight,
      },
    ];
  });
}

/**
 * The variant styling one URL, or null.
 *
 * First prefix wins across the whole list, which is what makes ordering
 * meaningful: `['https://x.com/a', 'https://']` styles one page one way and the
 * rest of the web another.
 */
export function linkVariantFor(
  variants: ReadonlyArray<ResolvedLinkVariant>,
  url: string
): ResolvedLinkVariant | null {
  for (const variant of variants) {
    if (variant.match.some((prefix) => url.startsWith(prefix))) {
      return variant;
    }
  }
  return null;
}

/* eslint-disable no-bitwise -- tag styles are a bitmask the native views read */
import { processColor as rnProcessColor } from 'react-native';

import type { MarkdownStyle, MarkdownViewProps } from './types';
import { entries } from './theme';

const processColor = rnProcessColor as (value: unknown) => unknown;

/** Style flag bits, as the native views read them. */
export const TAG_STYLE_BOLD = 1 << 0;
export const TAG_STYLE_ITALIC = 1 << 1;

/**
 * One tag's style, resolved to what the native views draw with.
 *
 * Names are lowercased because tag names compare that way; colors are
 * processed here rather than on the platforms because every other color the
 * platforms draw is processed, and a second color pipeline is how one host's
 * red becomes another host's.
 */
export type ResolvedTagStyle = Readonly<{
  name: string;
  /** A processed color, or null when the entry names none. */
  color: number | null;
  /** Bold, italic and underline as bits, for the same reason colors cross. */
  flags: number;
}>;

/**
 * The `tags` an app styled, resolved and in order.
 *
 * An empty list is the common case — no styled tags — and costs the platforms
 * nothing, because there is nothing to match against.
 */
export function resolveTagStyles(
  markdownStyle: MarkdownViewProps['markdownStyle']
): ReadonlyArray<ResolvedTagStyle> {
  const tags = new Map<string, NonNullable<MarkdownStyle['tags']>[string]>();
  for (const entry of entries<MarkdownStyle>(markdownStyle)) {
    for (const [name, style] of Object.entries(entry.tags ?? {})) {
      const key = name.toLowerCase();
      tags.set(key, { ...tags.get(key), ...style });
    }
  }
  const resolved: ResolvedTagStyle[] = [];
  for (const [name, style] of tags) {
    if (typeof name !== 'string' || name.length === 0 || !style) {
      continue;
    }
    const color =
      style.color === undefined
        ? null
        : (processColor(style.color) as number | null);
    let flags = 0;
    const weight = style.fontWeight;
    if (
      weight === 'bold' ||
      weight === '700' ||
      weight === '800' ||
      weight === '900' ||
      (typeof weight === 'number' && weight >= 700)
    ) {
      flags |= TAG_STYLE_BOLD;
    }
    if (style.fontStyle === 'italic') {
      flags |= TAG_STYLE_ITALIC;
    }
    resolved.push({
      name: name.toLowerCase(),
      color: typeof color === 'number' ? color : null,
      flags,
    });
  }
  return resolved;
}

/**
 * The entry styling `name`, or null.
 *
 * First entry wins when two names collide after lowercasing, which is what
 * makes the order meaningful rather than accidental.
 */
export function tagStyleFor(
  styles: ReadonlyArray<ResolvedTagStyle>,
  name: string
): ResolvedTagStyle | null {
  const normalized = name.toLowerCase();
  for (const style of styles) {
    if (style.name === normalized) {
      return style;
    }
  }
  return null;
}

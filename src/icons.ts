import {
  CONTROL_ICON_KEYS,
  type MarkdownControlIcon,
  type MarkdownIconSource,
} from './types';

/**
 * The glyph names a control button draws, resolved from the `icons` prop.
 *
 * The array travels beside `controlLabels` positionally, in `CONTROL_ICON_KEYS`
 * order, so both platforms read one button's glyph from one place. Two entries
 * per key when the prop allows both forms:
 *
 * - A **name** is the platform's own symbol, or an asset name when it contains a
 *   dot. An SF Symbol on iOS, a drawable on Android. This is the default and the
 *   form an app reaches for when it wants a different shape from the system's.
 * - An **image** is an app's own picture. It cannot cross into a native button's
 *   header — the loader only fetches HTTPS and a glyph comes from the app's
 *   bundle — so it is resolved here, in JavaScript, and handed over as the URI the
 *   image loader already knows how to read.
 *
 * `null` for a key the app set to `null`, which means "no icon at all": a
 * text-only button is how a quiet header is built, and a name the platform does
 * not recognise would otherwise silently fall back to the default.
 */
export type ResolvedIcon = string;

/**
 * One resolved glyph, or `null` when the button draws no icon.
 *
 * An image becomes a `data:` URI because the loader takes a source and a native
 * button cannot hold one: a bundled image in a release build has no stable file
 * path, so the bytes travel with the name rather than being looked up later.
 */
export function resolveIcon(
  source: MarkdownIconSource | undefined
): ResolvedIcon {
  if (source === undefined) {
    // Nothing sent: the platform draws its own default, which is why this is an
    // empty string rather than a name the platform would have to guess at.
    return '';
  }
  if ('name' in source) {
    return source.name.trim();
  }
  const image = source.image as { uri?: unknown } | undefined;
  const uri = typeof image?.uri === 'string' ? image.uri : '';
  if (uri.length === 0) {
    return '';
  }
  // Already a URI the loader can read, whether a remote one or a `data:` one the
  // app built itself.
  return /^[a-z][a-z0-9+.-]*:/i.test(uri) ? uri : `asset:/${uri}`;
}

/**
 * Every glyph, in `CONTROL_ICON_KEYS` order.
 *
 * An empty string means "no entry, not the default": the platforms own their
 * defaults, so a key the app did not name sends nothing rather than a name the
 * platform would then have to guess at.
 */
export function resolveIcons(
  icons?: Partial<Record<MarkdownControlIcon, MarkdownIconSource>>
): ReadonlyArray<ResolvedIcon> {
  const source = icons ?? {};
  return CONTROL_ICON_KEYS.map((key) => resolveIcon(source[key]));
}

/** The index of a key in the resolved array, for a caller that reads one glyph. */
export function iconIndex(key: MarkdownControlIcon): number {
  return CONTROL_ICON_KEYS.indexOf(key);
}

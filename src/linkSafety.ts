/**
 * Resolving a link destination, and opening one when the app passed no
 * `onLinkPress`.
 *
 * Two concerns that meet in one press and are kept apart here. Resolution is
 * pure: it turns a relative reference and an origin into a URL, and it does so
 * without `URL`, because React Native's polyfill is a partial implementation
 * whose base-resolution behaviour has changed between releases and is not the
 * same as a browser's. Opening is a side effect: `Linking`, an alert, and the
 * clipboard, each of which the app may replace.
 */
import { Alert, Linking, Platform } from 'react-native';

import NativeDownstreamSession from './NativeDownstreamSession';
import {
  defaultTranslations,
  type MarkdownLinkSafetyModalProps,
  type MarkdownLinksOptions,
  type MarkdownTranslations,
} from './types';

/** An origin that already names a scheme, so `//host` and `https://host` both work. */
const ORIGIN_SCHEME = /^[a-z][a-z0-9+.-]*:\/\//i;

/**
 * Resolves `href` against `origin`, or returns it unchanged when there is
 * nothing to resolve against.
 *
 * The four forms are the ones the link policy accepts, and the rules are the
 * ones a URL would apply:
 *
 * - `#frag` and `?q` replace the current document's fragment and query.
 * - `/path` replaces the path, keeping the origin.
 * - `./x` and `../x` walk up from the origin's own directory, which is the one
 *   case where a plain string manipulation and a URL disagree, and the reason
 *   the walk below exists rather than a `split('/')`.
 * - A bare `docs/a` is not relative as far as the policy is concerned, so it
 *   never reaches here.
 *
 * A `..` that would climb above the root stops at it, which is what both
 * browsers and `URL` do: `https://h/../x` is `https://h/x`.
 */
export function resolveLinkUrl(
  href: string,
  origin: string | undefined
): string {
  if (origin === undefined || origin === '') return href;
  if (!isRelativeHref(href)) return href;

  const base = normalizeOrigin(origin);
  if (base === '') return href;

  const hashAt = href.indexOf('#');
  const queryAt = href.indexOf('?');
  // Whichever comes first delimits the part that replaces the path; the other
  // one, if any, is carried through untouched.
  const cut =
    hashAt === -1
      ? queryAt
      : queryAt === -1
        ? hashAt
        : Math.min(hashAt, queryAt);
  const path = cut === -1 ? href : href.slice(0, cut);
  const rest = cut === -1 ? '' : href.slice(cut);

  if (path === '') {
    // A bare `#frag` or `?q` names the origin's own document.
    return base + rest;
  }
  if (path.startsWith('/')) {
    return base + collapseDots(path.slice(1)) + rest;
  }
  // A relative path resolves against the origin's directory, and the origin is
  // a host rather than a document, so the directory is `/`. `base` already ends
  // in one, which is why nothing is added here.
  return base + collapseDots(path) + rest;
}

/** The forms the link policy accepts as relative; mirrors `isRelativeLink`. */
function isRelativeHref(href: string): boolean {
  if (href === '') return false;
  const first = href[0];
  if (first === '#' || first === '/' || first === '?') return true;
  if (first !== '.') return false;
  if (href[1] === '/') return true;
  return href[1] === '.' && href[2] === '/';
}

/** An origin as a prefix that ends in a slash, which is what joining needs. */
function normalizeOrigin(origin: string): string {
  const trimmed = origin.trim();
  if (trimmed === '') return '';
  const withScheme = ORIGIN_SCHEME.test(trimmed)
    ? trimmed
    : `https://${trimmed.replace(/^\/+/, '')}`;
  return withScheme.endsWith('/') ? withScheme : `${withScheme}/`;
}

/**
 * Removes `.` and `..` segments from a path, clamping at the root.
 *
 * Trailing `.` and `..` are kept as a directory: `/a/b/..` is `/a/`, which is
 * what a browser requests, and dropping the segment would ask for `/a`. A path
 * that collapses away entirely becomes the empty path, because the root is
 * already the `/` the origin ends with.
 */
function collapseDots(path: string): string {
  if (!path.includes('./')) return path;
  const out: string[] = [];
  for (const segment of path.split('/')) {
    if (segment === '.') continue;
    if (segment === '..') {
      if (out.length > 0) out.pop();
      continue;
    }
    if (segment === '' && out.length === 0) continue;
    out.push(segment);
  }
  if (out.length === 0) return '';
  const joined = out.join('/');
  const trailing = path.endsWith('/.') || path.endsWith('/..');
  if (trailing && !joined.endsWith('/')) return `${joined}/`;
  return joined;
}

/** Whether `links` asks for relative destinations to resolve. */
export function resolvesLinks(
  links: MarkdownLinksOptions | undefined
): boolean {
  return (
    links !== undefined &&
    (links.relative === true || typeof links.defaultOrigin === 'string')
  );
}

/** `links` as a view prop: two strings, or nothing at all. */
export function resolveLinkOptions(
  links: MarkdownLinksOptions | undefined
): Readonly<{ origin: string }> {
  const origin = links?.defaultOrigin?.trim() ?? '';
  return { origin };
}

/**
 * Opens `url` through the platform, reporting whether the platform accepted it.
 *
 * `Linking.canOpenURL` is asked first because `openURL` rejects asynchronously
 * on both platforms, and a rejection the caller cannot see is how a tap on a
 * scheme nothing handles looks like a crash.
 */
export async function openExternalLink(url: string): Promise<boolean> {
  try {
    if (!(await Linking.canOpenURL(url))) return false;
    await Linking.openURL(url);
    return true;
  } catch {
    return false;
  }
}

/** Whether the clipboard is available on this platform. */
export function clipboardAvailable(): boolean {
  try {
    return NativeDownstreamSession.copyText !== undefined;
  } catch {
    return false;
  }
}

/** Copies `text`, resolving whether the platform has a clipboard to put it on. */
export async function copyToClipboard(text: string): Promise<boolean> {
  try {
    await NativeDownstreamSession.copyText(text);
    return true;
  } catch {
    return false;
  }
}

/** The strings the confirmation is built from, so an app's modal matches. */
export function linkSafetyLabels(
  translations: Partial<MarkdownTranslations>
): Readonly<{
  title: string;
  openLabel: string;
  copyLabel: string;
  cancelLabel: string;
}> {
  return {
    title:
      translations.externalLinkWarning ??
      defaultTranslations.externalLinkWarning,
    openLabel: translations.openLink ?? defaultTranslations.openLink,
    copyLabel: translations.copyLink ?? defaultTranslations.copyLink,
    cancelLabel: translations.close ?? defaultTranslations.close,
  };
}

/**
 * The default confirmation.
 *
 * A native alert rather than a React one, because it is the only modal that is
 * above the keyboard, above a native text selection menu, and above the app's
 * own navigation without this library having to know where that is. Its buttons
 * are the three Streamdown's modal has, in the order a reader expects them.
 */
function defaultLinkModal(props: MarkdownLinkSafetyModalProps): void {
  const buttons = [
    {
      text: props.cancelLabel,
      style: 'cancel' as const,
      onPress: props.onCancel,
    },
    { text: props.copyLabel, onPress: () => void copyToClipboard(props.url) },
    { text: props.openLabel, onPress: props.onConfirm },
  ];
  if (Platform.OS === 'ios') {
    Alert.alert(props.title, props.url, buttons, {
      cancelable: true,
      onDismiss: props.onCancel,
    });
    return;
  }
  Alert.alert(props.title, props.url, buttons, { cancelable: true });
}

/**
 * Asks, then opens, in that order.
 *
 * Nothing here dedupes presses: a reader who taps twice on a slow alert has two
 * alerts, which is what the platform's own modal does and what a reader expects
 * from a tap. Dedupe would need a lock the app cannot clear, and a stuck lock
 * is worse than a second alert.
 */
export async function confirmAndOpenLink(
  url: string,
  labels: Readonly<{
    title: string;
    openLabel: string;
    copyLabel: string;
    cancelLabel: string;
  }>,
  onLinkCheck: ((url: string) => boolean | Promise<boolean>) | undefined,
  renderModal: ((props: MarkdownLinkSafetyModalProps) => unknown) | undefined
): Promise<void> {
  if (onLinkCheck) {
    let trusted = false;
    try {
      trusted = (await onLinkCheck(url)) === true;
    } catch {
      // A check that throws is a check that did not pass. Opening on a rejected
      // promise would make a network failure the one case that skips the
      // confirmation.
      trusted = false;
    }
    if (trusted) {
      await openExternalLink(url);
      return;
    }
  }
  const props: MarkdownLinkSafetyModalProps = {
    url,
    ...labels,
    onConfirm: () => void openExternalLink(url),
    onCancel: () => {},
  };
  if (renderModal) {
    renderModal(props);
    return;
  }
  defaultLinkModal(props);
}

/**
 * What a press does when the app passed no `onLinkPress`.
 *
 * With link safety on, a URL the app trusts opens and every other one is
 * confirmed. With it off the link opens directly, which is the pre-link-safety
 * behaviour a reader who has told the view it trusts its own content wants.
 */
export async function openLinkSafely(
  url: string,
  config: Readonly<{
    enabled: boolean;
    origin: string;
    onLinkCheck?: (url: string) => boolean | Promise<boolean>;
    renderModal?: (props: MarkdownLinkSafetyModalProps) => unknown;
    translations?: Partial<MarkdownTranslations>;
  }>
): Promise<void> {
  const resolved = resolveLinkUrl(url, config.origin);
  if (!config.enabled) {
    await openExternalLink(resolved);
    return;
  }
  await confirmAndOpenLink(
    resolved,
    linkSafetyLabels(config.translations ?? {}),
    config.onLinkCheck,
    config.renderModal
  );
}

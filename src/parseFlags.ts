/* eslint-disable no-bitwise -- the native `parseFlags` prop is a bitmask */
/**
 * Resolution of normalized parse options. Public views resolve feature plugins
 * first and pass `math: false` when no math plugin is selected. Raw native
 * fixture helpers retain their installed-engine default when math is omitted.
 * This module owns the bit layout shared by JavaScript and the parser.
 */
import NativeDownstreamSession from './NativeDownstreamSession';
import type {
  MarkdownAutolinkProtocols,
  MarkdownLinksOptions,
  MarkdownMathOptions,
  MarkdownSyntaxOptions,
} from './types';

/**
 * Bits of the native `parseFlags` prop. Parsing and drawing both read them, so
 * they are the parse options rather than a resolved boolean: the parser needs
 * to know whether single dollars are math before anything is laid out.
 */
export const PARSE_FLAG_ENABLED = 1 << 0;
export const PARSE_FLAG_SINGLE_DOLLAR = 1 << 1;
export const PARSE_FLAG_BRACKETS = 1 << 2;
export const PARSE_FLAG_INLINE_HTML = 1 << 3;
export const PARSE_FLAG_SUPERSCRIPT_SYNTAX = 1 << 4;
export const PARSE_FLAG_SUBSCRIPT_SYNTAX = 1 << 5;
/** `/…`, `./…`, `../…`, `?…` and `#…` are links. See `MarkdownLinksOptions`. */
export const PARSE_FLAG_LINK_RELATIVE = 1 << 6;
export const PARSE_FLAG_NO_AUTOLINK_MAILTO = 1 << 7;
export const PARSE_FLAG_NO_AUTOLINK_HTTP = 1 << 8;
export const PARSE_FLAG_NO_AUTOLINK_HTTPS = 1 << 9;
/**
 * CJK-friendly emphasis and autolink boundaries. On by default, and off by
 * `cjk: false`. The engine runs the CJK range table only when neither
 * neighbour is whitespace, so a document with no CJK in it parses identically
 * either way — which is what `tests/parse-options.test.ts` asserts.
 */
export const PARSE_FLAG_CJK = 1 << 10;
/** `==highlight==`, md4c's own mark syntax. Off by default. */
export const PARSE_FLAG_HIGHLIGHT_SYNTAX = 1 << 11;
/** `__underline__`, md4c's own underline syntax. Off by default. */
export const PARSE_FLAG_UNDERLINE_SYNTAX = 1 << 12;
/** `||spoiler||`, md4c's own spoiler syntax. Off by default. */
export const PARSE_FLAG_SPOILER_SYNTAX = 1 << 13;
/**
 * An allowed tag's content is unstyled text, not parsed Markdown. Off by
 * default; see `literalTagContent`.
 */
export const PARSE_FLAG_LITERAL_TAG_CONTENT = 1 << 14;
/** Skip lexer work for views that did not select the code plugin. */
export const PARSE_FLAG_NO_CODE_HIGHLIGHT = 1 << 15;

/** Every flag the engine understands. */
export const PARSE_FLAGS_ALL =
  PARSE_FLAG_ENABLED |
  PARSE_FLAG_SINGLE_DOLLAR |
  PARSE_FLAG_BRACKETS |
  PARSE_FLAG_INLINE_HTML |
  PARSE_FLAG_SUPERSCRIPT_SYNTAX |
  PARSE_FLAG_SUBSCRIPT_SYNTAX |
  PARSE_FLAG_LINK_RELATIVE |
  PARSE_FLAG_NO_AUTOLINK_MAILTO |
  PARSE_FLAG_NO_AUTOLINK_HTTP |
  PARSE_FLAG_NO_AUTOLINK_HTTPS |
  PARSE_FLAG_CJK |
  PARSE_FLAG_HIGHLIGHT_SYNTAX |
  PARSE_FLAG_UNDERLINE_SYNTAX |
  PARSE_FLAG_SPOILER_SYNTAX |
  PARSE_FLAG_LITERAL_TAG_CONTENT |
  PARSE_FLAG_NO_CODE_HIGHLIGHT;

export type ResolvedMath = Readonly<{
  /** Bitmask for the native `parseFlags` prop. */
  flags: number;
  /** True when at least one delimiter form is recognized. */
  enabled: boolean;
}>;

type Section = Readonly<Record<string, unknown>>;

/**
 * A value that is not one of the shapes `MarkdownMathOptions` allows. A caller
 * that reads the option out of JSON gets one, and reading a key off it would
 * throw where every other accessor treats it as absent.
 */
function options(value: unknown): Section | undefined {
  return typeof value === 'object' && value !== null
    ? (value as Section)
    : undefined;
}

/**
 * Whether the math engine is linked into this process.
 *
 * Synchronous on purpose: the default has to be settled during render, because
 * a view that decided "off" and then turned "on" would lay a document out twice
 * with two different answers. The platform reads one flag it set when the engine
 * was installed, so this is a field read rather than a call across the bridge.
 */
export function mathAvailable(): boolean {
  try {
    return NativeDownstreamSession.mathAvailable() === true;
  } catch {
    // A host with no TurboModule at all — a bare test renderer, a server render
    // — has no engine, which is the same answer as a platform that reports the
    // package is not installed.
    return false;
  }
}

/** Whether the optional native highlighter is installed in this binary. */
export function codeAvailable(): boolean {
  try {
    return NativeDownstreamSession.codeAvailable() === true;
  } catch {
    return false;
  }
}

/**
 * Resolves `math` into the flags the parser and the platforms read.
 *
 * `undefined` means the caller said nothing, so the default applies: on when
 * the engine is installed, off when it is not. `false` is exactly today's parse
 * and display, which is what makes turning math off a no-op for existing users.
 */
export function resolveMath(
  math: boolean | MarkdownMathOptions | undefined,
  available: boolean = mathAvailable()
): ResolvedMath {
  if (math === false) {
    return { flags: 0, enabled: false };
  }
  if (math === true) {
    return {
      flags: PARSE_FLAG_ENABLED | PARSE_FLAG_BRACKETS,
      enabled: true,
    };
  }
  if (math === undefined) {
    // Installing the package is the opt-in, as `@streamdown/math` is. Brackets
    // default on because they are what OpenAI-family models emit.
    return available
      ? { flags: PARSE_FLAG_ENABLED | PARSE_FLAG_BRACKETS, enabled: true }
      : { flags: 0, enabled: false };
  }
  // An object form always turns math on: naming an option is asking for math.
  // Each option keeps its own default: single dollars off, as in Streamdown, so
  // `{ brackets: false }` does not quietly make `$5 and $10` a formula.
  const section = options(math);
  if (!section) {
    // A truthy value that is not one of the shapes, read out of JSON or out of
    // JavaScript that skipped a type. It asked for math and named no options,
    // which is what `true` means, so it resolves as `true` rather than
    // throwing where every other accessor treats the value as absent.
    return { flags: PARSE_FLAG_ENABLED | PARSE_FLAG_BRACKETS, enabled: true };
  }
  const flags =
    PARSE_FLAG_ENABLED |
    (section.singleDollarTextMath === true ? PARSE_FLAG_SINGLE_DOLLAR : 0) |
    (section.brackets === false ? 0 : PARSE_FLAG_BRACKETS);
  return { flags, enabled: true };
}

let warnedAboutMissingEngine = false;
let warnedAboutUnusedEngine = false;

/** Installing the engine alone no longer activates math for a public view. */
export function warnAboutUnusedMathEngine(): void {
  if (typeof __DEV__ !== 'boolean' || !__DEV__ || warnedAboutUnusedEngine)
    return;
  warnedAboutUnusedEngine = true;
  console.warn(
    'Downstream: the math engine is installed but this view has no math plugin. Import { math } from @react-native-downstream/math and pass plugins={[math()]}.'
  );
}

/**
 * One development warning per process, naming the package to install.
 *
 * Only when a document actually contains math: a view that never shows a
 * formula has no reason to tell an app anything, and a warning every render
 * would train an app to ignore the one warning that matters.
 */
export function warnAboutMissingMathEngine(): void {
  if (warnedAboutMissingEngine) return;
  warnedAboutMissingEngine = true;
  console.warn(
    'Downstream: this document contains math, but no math engine is installed. ' +
      'Add `@react-native-downstream/math` to typeset it; formulas are shown as ' +
      'their source until then.'
  );
}

/** Test hook: forgets the warning, so a case can observe it more than once. */
export function resetMathEngineWarning(): void {
  warnedAboutMissingEngine = false;
  warnedAboutUnusedEngine = false;
}

/** The props that decide which syntaxes a parse recognizes. */
export type MarkdownParseInput = Readonly<{
  math: boolean | MarkdownMathOptions | undefined;
  inlineHtml: boolean | undefined;
  syntax: MarkdownSyntaxOptions | undefined;
  cjk: boolean | undefined;
  links: MarkdownLinksOptions | undefined;
  autolinkProtocols: MarkdownAutolinkProtocols | undefined;
  literalTagContent: boolean | undefined;
  syntaxHighlighting?: boolean;
}>;

/**
 * Resolves the parse-affecting props into the flag bitmask.
 *
 * One resolver, so the JS default and the parser default cannot disagree:
 * `inlineHtml` and `cjk` are on unless explicitly false, the syntax bits are set
 * only when the caller names them, and the three autolink bits are set only when
 * a protocol is named. `resolveMath`'s answer is the base, so a document with no
 * math parses identically under any combination of the others.
 *
 * An options object rather than positional arguments, because the later ones
 * read identically to `undefined` at every call site and a caller would
 * have to count them.
 */
export function resolveParseFlags(
  input: MarkdownParseInput,
  available: boolean = mathAvailable()
): ResolvedMath {
  const base = resolveMath(input.math, available);
  let flags = base.flags;
  if (input.inlineHtml !== false) {
    flags |= PARSE_FLAG_INLINE_HTML;
  }
  if (input.cjk !== false) {
    flags |= PARSE_FLAG_CJK;
  }
  if (input.syntax?.superscript === true) {
    flags |= PARSE_FLAG_SUPERSCRIPT_SYNTAX;
  }
  if (input.syntax?.subscript === true) {
    flags |= PARSE_FLAG_SUBSCRIPT_SYNTAX;
  }
  if (input.syntax?.highlight === true) {
    flags |= PARSE_FLAG_HIGHLIGHT_SYNTAX;
  }
  if (input.syntax?.underline === true) {
    flags |= PARSE_FLAG_UNDERLINE_SYNTAX;
  }
  if (input.syntax?.spoiler === true) {
    flags |= PARSE_FLAG_SPOILER_SYNTAX;
  }
  if (input.literalTagContent === true) {
    flags |= PARSE_FLAG_LITERAL_TAG_CONTENT;
  }
  if (input.syntaxHighlighting === false) flags |= PARSE_FLAG_NO_CODE_HIGHLIGHT;
  if (
    input.links?.relative === true ||
    typeof input.links?.defaultOrigin === 'string'
  ) {
    flags |= PARSE_FLAG_LINK_RELATIVE;
  }
  const disabled = normalizeAutolinkProtocols(input.autolinkProtocols);
  if (disabled.has('mailto')) flags |= PARSE_FLAG_NO_AUTOLINK_MAILTO;
  if (disabled.has('http')) flags |= PARSE_FLAG_NO_AUTOLINK_HTTP;
  if (disabled.has('https')) flags |= PARSE_FLAG_NO_AUTOLINK_HTTPS;
  return { flags, enabled: base.enabled };
}

/**
 * Streamdown's normalization of a `disableAutolinkProtocols` list: trimmed,
 * lower cased, and with the trailing colon optional, so `mailto`, `MailTo:` and
 * `mailto:` are one entry.
 *
 * A protocol this engine has no bit for is kept in the set rather than dropped,
 * so a caller disabling `ftp` is not silently told it worked; it simply has no
 * effect, because there was no autolink for it to stop in the first place.
 */
function normalizeAutolinkProtocols(
  value: MarkdownAutolinkProtocols | undefined
): ReadonlySet<string> {
  const out = new Set<string>();
  if (!Array.isArray(value)) return out;
  for (const entry of value) {
    if (typeof entry !== 'string') continue;
    const scheme = entry.trim().toLowerCase().replace(/:$/, '');
    if (scheme !== '') out.add(scheme);
  }
  return out;
}

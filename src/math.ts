/* eslint-disable no-bitwise -- the native `mathFlags` prop is a bitmask */
/**
 * Resolution of the `math` prop.
 *
 * Math is opt-in by installation: `react-native-downstream-math` ships the
 * engine, and a view with it typesets formulas while a view without it shows
 * every formula's source. That mirrors `@streamdown/math`, and it means the
 * default has to be asked of the platform rather than written here, so one
 * module owns the bit layout the parser reads and this file is the only place
 * that writes those bits.
 */
import NativeDownstreamSession from './NativeDownstreamSession';
import type { MarkdownMathOptions } from './types';

/**
 * Bits of the native `mathFlags` prop. Parsing and drawing both read them, so
 * they are the parse options rather than a resolved boolean: the parser needs
 * to know whether single dollars are math before anything is laid out.
 */
export const MATH_FLAG_ENABLED = 1 << 0;
export const MATH_FLAG_SINGLE_DOLLAR = 1 << 1;
export const MATH_FLAG_BRACKETS = 1 << 2;

/** Every flag the engine understands. */
export const MATH_FLAGS_ALL =
  MATH_FLAG_ENABLED | MATH_FLAG_SINGLE_DOLLAR | MATH_FLAG_BRACKETS;

export type ResolvedMath = Readonly<{
  /** Bitmask for the native `mathFlags` prop. */
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
      flags: MATH_FLAG_ENABLED | MATH_FLAG_BRACKETS,
      enabled: true,
    };
  }
  if (math === undefined) {
    // Installing the package is the opt-in, as `@streamdown/math` is. Brackets
    // default on because they are what OpenAI-family models emit.
    return available
      ? { flags: MATH_FLAG_ENABLED | MATH_FLAG_BRACKETS, enabled: true }
      : { flags: 0, enabled: false };
  }
  // An object form always turns math on: naming an option is asking for math.
  const section = options(math)!;
  const flags =
    MATH_FLAG_ENABLED |
    (section.singleDollarTextMath === false ? 0 : MATH_FLAG_SINGLE_DOLLAR) |
    (section.brackets === false ? 0 : MATH_FLAG_BRACKETS);
  return { flags, enabled: true };
}

let warnedAboutMissingEngine = false;

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
      'Add `react-native-downstream-math` to typeset it; formulas are shown as ' +
      'their source until then.'
  );
}

/** Test hook: forgets the warning, so a case can observe it more than once. */
export function resetMathEngineWarning(): void {
  warnedAboutMissingEngine = false;
}

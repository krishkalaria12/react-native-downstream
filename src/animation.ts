/**
 * Resolution of the `animated` and `caret` props.
 *
 * Streamdown resolves `animated` with three small helpers: an animation name, a
 * duration, a stagger, and a CSS timing keyword. A value written for its web
 * component is reused here as it is, so the same names and the same defaults
 * decide what the native draw pass does.
 *
 * Everything here is pure. The platforms receive one animation name, one split,
 * one flat array of numbers, one caret shape, and whether text is still arriving.
 * No CSS keyword and no JavaScript object crosses into native code, because
 * neither has a meaning there.
 */
import type { MarkdownAnimateOptions, MarkdownViewProps } from './types';

/** The native `animation` prop's values. `none` means no fade at all. */
export type ResolvedAnimation = 'none' | 'fadeIn' | 'slideUp';

/** The native `animationSplit` prop's values. */
export type ResolvedSplit = 'word' | 'char';

/** The native `animating` prop's values. */
export type ResolvedAnimating = 'auto' | 'on' | 'off';

/** The native `caret` prop's values. */
export type ResolvedCaret = 'none' | 'block' | 'circle';

export const defaultDuration = 150;
export const defaultStagger = 40;
export const defaultMaxBacklogMs = 320;

/**
 * One resolved animation, and the seven numbers the native `animationTiming`
 * prop carries: `[duration, stagger, maxBacklogMs, x1, y1, x2, y2]`.
 *
 * An array rather than a struct of props for the same reason the theme travels as
 * three parallel arrays: it compares as a vector, so an inline `animated` object
 * that has not changed sends nothing on a re-render, and an unchanged
 * configuration costs nothing per revision.
 */
export type ResolvedAnimate = Readonly<{
  animation: ResolvedAnimation;
  split: ResolvedSplit;
  timing: readonly number[];
}>;

/** The four control points of a CSS cubic-bezier easing, in `CubicBezier` order. */
type CubicBezier = readonly [number, number, number, number];

/**
 * The CSS timing keywords, as their own control points.
 *
 * These are the values the CSS specification gives each keyword, and the engine
 * solves the same curve natively, so a `cubic-bezier` written by hand and an
 * `ease-out` produce the same fade.
 */
const EASINGS: Readonly<Record<string, CubicBezier>> = {
  'linear': [0, 0, 1, 1],
  'ease': [0.25, 0.1, 0.25, 1],
  'ease-in': [0.42, 0, 1, 1],
  'ease-out': [0, 0, 0.58, 1],
  'ease-in-out': [0.42, 0, 0.58, 1],
};

/** The default curve, which is `ease`. */
export const CSS_EASE: CubicBezier = EASINGS.ease!;

const CUBIC_BEZIER =
  /^cubic-bezier\(\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)\s*\)$/;

/**
 * Reports a value once in development, rather than throwing.
 *
 * An unrecognised animation name or easing is a typo in the app's config, and
 * falling back to the default silently is how a fade that never appears goes
 * unnoticed. The fallback is the documented one; the warning is what makes it
 * findable.
 */
export type Warn = (message: string) => void;

let warned = new Set<string>();

/** Forgets what has been warned about, so a test can see each warning once. */
export function resetAnimationWarnings(): void {
  warned = new Set();
}

function warnOnce(message: string, onWarning?: Warn): void {
  if (onWarning) {
    onWarning(message);
    return;
  }
  if (warned.has(message)) return;
  warned.add(message);
  if (process.env.NODE_ENV === 'production') return;

  // the app's own configuration.
  console.warn(`Downstream: ${message}`);
}

/**
 * A duration, or the default.
 *
 * Streamdown takes these as plain numbers and does not check them. A negative or
 * `NaN` value is not a duration, and passing one through would either invert the
 * fade or make it never finish, so it falls back with one warning.
 */
function duration(
  value: number | undefined,
  fallback: number,
  name: string,
  onWarning?: Warn
): number {
  if (value === undefined) return fallback;
  if (typeof value !== 'number' || !Number.isFinite(value) || value < 0) {
    warnOnce(
      `${name} must be a non-negative number; using ${fallback}.`,
      onWarning
    );
    return fallback;
  }
  return value;
}

/**
 * Resolves a CSS timing keyword or `cubic-bezier()` into its four control points.
 *
 * `steps()` and `linear()` with stops have no meaning to a native draw pass,
 * which can only interpolate continuously, so they fall back to `ease` with one
 * warning rather than being rejected. The x components are clamped to [0, 1]
 * because a curve that doubles back has no single y for an x, which is the one
 * thing a fade needs.
 */
export function resolveEasing(
  value: string | undefined,
  onWarning?: Warn
): CubicBezier {
  if (value === undefined) return CSS_EASE;
  const keyword = EASINGS[value.trim().toLowerCase()];
  if (keyword) return keyword;
  const match = CUBIC_BEZIER.exec(value.trim());
  if (!match) {
    warnOnce(
      `Unknown easing "${value}". Use a CSS timing keyword or cubic-bezier(); using ease.`,
      onWarning
    );
    return CSS_EASE;
  }
  const points = match.slice(1, 5).map(Number) as unknown as number[];
  const [x1, y1, x2, y2] = points;
  if (points.some((point) => !Number.isFinite(point))) {
    warnOnce(
      `cubic-bezier("${value}") needs four numbers; using ease.`,
      onWarning
    );
    return CSS_EASE;
  }
  const clamp = (n: number) => Math.min(1, Math.max(0, n));
  return [clamp(x1!), y1!, clamp(x2!), y2!];
}

/**
 * The animation name, or `fadeIn`.
 *
 * `blurIn` is the one name Streamdown ships that this cannot do: neither platform
 * can blur one glyph run cheaply inside a text draw pass, and a blur would also
 * have to move glyph positions rather than only their alpha. It draws as `fadeIn`
 * and says so, because a caller who asked for a blur should know they are not
 * getting one. Any other name draws as `fadeIn` too, for the same reason.
 */
export function resolveAnimation(
  value: MarkdownAnimateOptions['animation'],
  onWarning?: Warn
): ResolvedAnimation {
  if (value === undefined) return 'fadeIn';
  // `MarkdownAnimateOptions['animation']` admits any string so a value read from
  // a config file is not a type error, which means the comparison does not narrow
  // it. The two names are checked as plain strings for that reason.
  if ((value as string) === 'fadeIn' || (value as string) === 'slideUp') {
    return value as ResolvedAnimation;
  }
  warnOnce(
    `Unknown animation "${value}". Use fadeIn, slideUp or blurIn; blurIn draws as fadeIn.`,
    onWarning
  );
  return 'fadeIn';
}

/**
 * Resolves `animated` into what the native draw pass reads.
 *
 * `true` is every default and `false` is no fade at all, which the platforms read
 * as one nil check per draw. An object enables the fade and names its options.
 */
export function resolveAnimate(
  animated: MarkdownViewProps['animated'],
  onWarning?: Warn
): ResolvedAnimate {
  if (animated === undefined || animated === false) {
    return NONE;
  }
  if (animated !== true && typeof animated !== 'object') {
    warnOnce(
      'animated must be a boolean or an options object; no animation will run.',
      onWarning
    );
    return NONE;
  }
  const options: MarkdownAnimateOptions =
    animated === true ? {} : (animated as MarkdownAnimateOptions);
  const easing = resolveEasing(options.easing, onWarning);
  return {
    animation: resolveAnimation(options.animation, onWarning),
    split: options.sep === 'char' ? 'char' : 'word',
    timing: [
      duration(options.duration, defaultDuration, 'duration', onWarning),
      duration(options.stagger, defaultStagger, 'stagger', onWarning),
      duration(
        options.maxBacklogMs,
        defaultMaxBacklogMs,
        'maxBacklogMs',
        onWarning
      ),
      ...easing,
    ],
  };
}

/** No fade. The platforms' draw pass costs one nil check. */
export const NONE: ResolvedAnimate = {
  animation: 'none',
  split: 'word',
  timing: [],
};

export type { MarkdownViewProps };

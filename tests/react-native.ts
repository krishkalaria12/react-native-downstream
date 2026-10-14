/**
 * Minimal React Native stand-in for unit tests.
 *
 * The library touches four runtime values from `react-native`: the TurboModule
 * registry, the codegen component factory, `__DEV__`, and the display metrics
 * the trace records. Everything else in the public surface is types, which
 * erase at build time, so nothing else needs stubbing.
 */
import { createElement } from 'react';
import type React from 'react';
import { unstable_batchedUpdates as batchedUpdates } from 'react-dom';
import { vi } from 'vitest';

export const unstable_batchedUpdates = vi.fn(batchedUpdates);

/** The TurboModule surface the stream controller calls. */
export interface TurboModuleStub {
  /** `src/parseFlags.ts` settles the `math` default from this. */
  mathAvailable(): boolean;
  /** `src/linkSafety.ts`'s confirmation offers "Copy link" through this. */
  copyText(text: string): Promise<boolean>;
  create(sessionId: string, mode: string): void;
  append(
    sessionId: string,
    chunk: string,
    generation: number,
    sequence: number
  ): void;
  reset(sessionId: string, source: string, generation: number): void;
  finish(sessionId: string, generation: number, sequence: number): void;
  dispose(sessionId: string): void;
  setTracing(sessionId: string, enabled: boolean): void;
  setPreview(sessionId: string, features: number): void;
  onSnapshotReady(callback: (event: object) => void): { remove(): void };
  onSessionError(callback: (event: object) => void): { remove(): void };
  onTrace(callback: (event: object) => void): { remove(): void };
}

export const TurboModuleRegistry: {
  get(name: string): unknown;
  getEnforcing(name: string): TurboModuleStub;
} = {
  get: vi.fn(() => null),
  getEnforcing: vi.fn(() => ({
    mathAvailable: () => false,
    copyText: vi.fn(async () => true),
    create: vi.fn(),
    append: vi.fn(),
    reset: vi.fn(),
    finish: vi.fn(),
    dispose: vi.fn(),
    setTracing: vi.fn(),
    setPreview: vi.fn(),
    onSnapshotReady: vi.fn(() => ({ remove: vi.fn() })),
    onSessionError: vi.fn(() => ({ remove: vi.fn() })),
    onTrace: vi.fn(() => ({ remove: vi.fn() })),
  })),
};

export const codegenNativeComponent = vi.fn((name: string) => name);
export const codegenNativeCommands = vi.fn(() => ({}));

export const Platform = {
  OS: 'ios' as const,
  Version: '18.0',
  constants: {} as Record<string, unknown>,
  select: (spec: Record<string, unknown>) => spec.ios,
};

export const Dimensions = {
  get: () => ({ width: 390, height: 844, scale: 3, fontScale: 1 }),
};

export const PixelRatio = { get: () => 3, getFontScale: () => 1 };

export const StyleSheet = {
  create: <T>(styles: T): T => styles,
  hairlineWidth: 0.5,
  flatten: (style: unknown) => style,
  absoluteFill: {},
};

export const Linking = {
  openURL: vi.fn(async () => true),
  canOpenURL: vi.fn(async () => true),
  getInitialURL: vi.fn(async () => null),
};

/**
 * A native alert, recorded rather than shown.
 *
 * `src/linkSafety.ts` uses one for the link confirmation, so a test needs to
 * answer it. The recorded call is what a test asserts on, and `answer` lets it
 * press one of the buttons the way a reader would.
 */
export interface AlertButton {
  text?: string;
  style?: string;
  onPress?: () => void;
}

export const Alert = {
  alert: vi.fn(
    (
      title: string,
      message?: string,
      buttons?: AlertButton[],
      options?: { cancelable?: boolean; onDismiss?: () => void }
    ) => {
      void title;
      void message;
      void options;
      // `alert.answer` is a test hook: the index of the button to press, or -1
      // to dismiss. Absent, the alert is recorded and nothing is pressed.
      const answer = (Alert as unknown as { answer?: number }).answer;
      if (answer !== undefined && buttons?.[answer]?.onPress) {
        buttons[answer].onPress?.();
      }
    }
  ),
};
export const Settings = { get: vi.fn(), set: vi.fn() };
export const I18nManager = { isRTL: false };

/**
 * `processColor`, as React Native's own behaves.
 *
 * Faithful enough for the theme resolver, which is the one part of the library
 * that reads a processed color: a hex string, a named color and a number all
 * become an ARGB integer, a `DynamicColorIOS` object comes back as its
 * `dynamic` pair, a `PlatformColor` comes back as its opaque form, and anything
 * unreadable becomes `undefined`. The comparison bench's renderer only ever
 * hands these to a web implementation, so it does not care about the details.
 */
export function processColor(color?: unknown): unknown {
  if (color === undefined || color === null) return color;
  if (typeof color === 'object') {
    // A DynamicColorIOS arrives already processed, as `{ dynamic: {...} }`, and
    // PlatformColor as `{ semantic: [...] }` or `{ resource_paths: [...] }`.
    return color;
  }
  if (typeof color === 'number') return swap(color);
  if (typeof color !== 'string') return null;
  const text = color.trim().toLowerCase();
  const hex = /^#([0-9a-f]{3,8})$/.exec(text);
  if (hex?.[1] !== undefined) {
    const digits = hex[1];
    // #abc is #aabbcc and #abcd is #aabbccdd, then an omitted alpha is opaque.
    const expanded =
      digits.length === 3 || digits.length === 4
        ? [...digits].map((digit) => digit + digit).join('')
        : digits;
    const withAlpha = expanded.length === 6 ? `${expanded}ff` : expanded;
    return swap(Number.parseInt(withAlpha, 16));
  }
  const named = NAMED[text];
  // The table holds 0xaarrggbb already, so only the number path is swapped.
  if (named !== undefined) return named;
  // An rgb(), rgba() or hsl() string is readable to the platform but not to this
  // stand-in, and returning undefined makes the resolver refuse it rather than
  // draw a color nobody asked for.
  return undefined;
}

/**
 * 0xrrggbbaa becomes 0xaarrggbb, which is the order both platforms read a
 * processed color in. JavaScript numbers hold the unsigned value, so the shift
 * is done in 32 bits and read back unsigned.
 */
function swap(color: number): number {
  return ((color << 24) | (color >>> 8)) >>> 0;
}

/** The CSS named colors, as the table React Native's own parser carries. */
const NAMED: Readonly<Record<string, number>> = {
  transparent: 0x00000000,
  aliceblue: 0xfff0f8ff,
  black: 0xff000000,
  blue: 0xff0000ff,
  brown: 0xffa52a2a,
  crimson: 0xffdc143c,
  cyan: 0xff00ffff,
  gray: 0xff808080,
  green: 0xff008000,
  grey: 0xff808080,
  indigo: 0xff4b0082,
  magenta: 0xffff00ff,
  maroon: 0xff800000,
  navy: 0xff000080,
  olive: 0xff808000,
  orange: 0xffffa500,
  pink: 0xffffc0cb,
  purple: 0xff800080,
  red: 0xffff0000,
  rebeccapurple: 0xff663399,
  silver: 0xffc0c0c0,
  teal: 0xff008080,
  violet: 0xffee82ee,
  white: 0xffffffff,
  yellow: 0xffffff00,
};

export function processColorArray(colors: unknown): unknown {
  return colors;
}
export function flattenStyle(style: unknown): unknown {
  return style;
}

export const View = 'View';
export const Text = 'Text';
export const ScrollView = 'ScrollView';
export const TextInput = 'TextInput';
export const FlatList = 'FlatList';
export const SafeAreaView = 'SafeAreaView';
export const StatusBar = 'StatusBar';
export const KeyboardAvoidingView = 'KeyboardAvoidingView';

export const useColorScheme = () => 'light';

export const requireNativeComponent = vi.fn((name: string) => name);

export const Animated = { View: 'Animated.View' };
export const Easing = { linear: (t: number) => t };
export const findNodeHandle = vi.fn();

/**
 * A pressable, as a real button.
 *
 * A component rather than a host name, because `accessibilityLabel` is how React
 * Native names a control and `aria-label` is how the DOM does, and a test should
 * be able to find a control by the same string a screen reader would read out.
 * The props the platform reads are translated rather than passed through: a test
 * DOM would render them as unknown attributes, and that noise says nothing about
 * whether a reader can use the button.
 */
export function Pressable({
  accessibilityLabel,
  accessibilityRole,
  onPress,
  style,
  children,
  ...rest
}: {
  accessibilityLabel?: string;
  accessibilityRole?: string;
  onPress?: () => void;
  style?: unknown;
  children?: unknown;
  [key: string]: unknown;
}): React.ReactElement {
  return createElement(
    'button',
    {
      'aria-label': accessibilityLabel,
      'data-role': accessibilityRole,
      style,
      'onClick': onPress,
      ...rest,
    },
    children as React.ReactNode
  );
}

/**
 * A modal, as a dialog.
 *
 * A component rather than a host name, because `role="dialog"` is what an
 * accessible modal is and the tests should be able to ask the question a reader
 * would: is a dialog open, and what does it say. The props the platform reads are
 * dropped rather than passed through, because a test DOM would read them as
 * unknown attributes and that noise says nothing.
 */
export function Modal({
  children,
}: {
  children?: unknown;
}): React.ReactElement {
  return createElement('div', { role: 'dialog' }, children as React.ReactNode);
}

export const __DEV__ = true;

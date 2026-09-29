/**
 * Minimal React Native stand-in for unit tests and benchmarks.
 *
 * The library touches four runtime values from `react-native`: the TurboModule
 * registry, the codegen component factory, `__DEV__`, and the display metrics
 * the trace records. Everything else in the public surface is types, which
 * erase at build time, so nothing else needs stubbing.
 */
import { vi } from 'vitest';

/** The TurboModule surface the stream controller calls. */
export interface TurboModuleStub {
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
    create: vi.fn(),
    append: vi.fn(),
    reset: vi.fn(),
    finish: vi.fn(),
    dispose: vi.fn(),
    setTracing: vi.fn(),
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
  create: <T,>(styles: T): T => styles,
  hairlineWidth: 0.5,
  flatten: (style: unknown) => style,
  absoluteFill: {},
};

export const Linking = { openURL: vi.fn(), getInitialURL: vi.fn(async () => null) };
export const Settings = { get: vi.fn(), set: vi.fn() };
export const I18nManager = { isRTL: false };

/** Style helpers the comparison bench's renderer reaches for at import time. */
export function processColor(color: unknown): unknown {
  return color;
}
export function processColorArray(colors: unknown): unknown {
  return colors;
}
export function flattenStyle(style: unknown): unknown {
  return style;
}

export const View = 'View';
export const Text = 'Text';
export const ScrollView = 'ScrollView';
export const Pressable = 'Pressable';
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

export const __DEV__ = true;

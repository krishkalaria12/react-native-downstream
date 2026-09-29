import { createContext, useContext, type ReactNode } from 'react';
import { Platform, StyleSheet, useColorScheme } from 'react-native';
import type { ViewStyle } from 'react-native';

/**
 * The app reads as a measurement instrument: a recessed canvas, bright paper
 * wells that float above it, one deep pine accent, and a mint reserved for
 * "running". Surfaces carry hierarchy, so nothing is a rounded card for the
 * sake of it, and every figure uses tabular numerals so columns line up.
 */
export interface Palette {
  /** Recessed background that content sits on. */
  canvas: string;
  /** Bright paper, for content that should lift off the canvas. */
  surface: string;
  /** A well inside a surface, for raw text and code. */
  sunken: string;
  ink: string;
  inkSoft: string;
  inkFaint: string;
  line: string;
  lineSoft: string;
  accent: string;
  accentPress: string;
  accentSoft: string;
  /** Text drawn on a filled accent surface. */
  onAccent: string;
  /** Reserved for live runs and active selection. */
  live: string;
  liveSoft: string;
  danger: string;
  dangerSoft: string;
  warn: string;
  warnSoft: string;
  /** Marks the faster side of a comparison. */
  win: string;
  scrim: string;
}

const light: Palette = {
  canvas: '#EFEFE9',
  surface: '#FBFBF8',
  sunken: '#E5E5DD',
  ink: '#12201C',
  inkSoft: '#4C5F58',
  inkFaint: '#5F7269',
  line: '#DCDBD0',
  lineSoft: '#E8E7DD',
  accent: '#0C6656',
  accentPress: '#084A3F',
  accentSoft: '#D5E9E3',
  onAccent: '#FFFFFF',
  live: '#0A8E6D',
  liveSoft: '#D3F1E8',
  danger: '#A32B2B',
  dangerSoft: '#F7E5E2',
  warn: '#8A5A08',
  warnSoft: '#F6EBD5',
  win: '#DBEAE1',
  scrim: 'rgba(12, 24, 20, 0.42)',
};

const dark: Palette = {
  canvas: '#0A100E',
  surface: '#141C19',
  sunken: '#0D1412',
  ink: '#E8F0EC',
  inkSoft: '#93A7A0',
  inkFaint: '#7B8E86',
  line: '#222C29',
  lineSoft: '#1A2321',
  accent: '#3EC3A6',
  accentPress: '#6FE0C4',
  accentSoft: '#0F2A25',
  onAccent: '#04201A',
  live: '#68F0CE',
  liveSoft: '#0C2C26',
  danger: '#F0908A',
  dangerSoft: '#2C1817',
  warn: '#E3B463',
  warnSoft: '#2A2214',
  win: '#12291F',
  scrim: 'rgba(0, 0, 0, 0.6)',
};

const mono = Platform.select({ ios: 'Menlo', default: 'monospace' })!;

/** 4pt spacing scale. Only these values appear in screen layouts. */
export const space = {
  xs: 4,
  sm: 8,
  md: 12,
  lg: 16,
  xl: 20,
  xxl: 28,
  xxxl: 40,
} as const;

/** Three radii, so shape carries hierarchy instead of repeating one value. */
export const radius = {
  pill: 999,
  well: 12,
  card: 18,
} as const;

const cardShadow: ViewStyle = Platform.select({
  ios: {
    shadowColor: '#04120D',
    shadowOpacity: 0.07,
    shadowRadius: 18,
    shadowOffset: { width: 0, height: 8 },
  },
  android: { elevation: 2 },
  default: {},
})!;

function createStyles(c: Palette) {
  return StyleSheet.create({
    // --- text ---------------------------------------------------------
    display: {
      color: c.ink,
      fontSize: 32,
      lineHeight: 36,
      fontWeight: '700',
      letterSpacing: -0.8,
      fontVariant: ['tabular-nums'],
    },
    figure: {
      color: c.ink,
      fontSize: 26,
      lineHeight: 30,
      fontWeight: '700',
      letterSpacing: -0.5,
      fontVariant: ['tabular-nums'],
    },
    figureUnit: {
      color: c.inkFaint,
      fontSize: 14,
      fontWeight: '600',
      fontVariant: ['tabular-nums'],
    },
    title: {
      color: c.ink,
      fontSize: 22,
      lineHeight: 27,
      fontWeight: '700',
      letterSpacing: -0.4,
    },
    heading: {
      color: c.ink,
      fontSize: 16,
      lineHeight: 21,
      fontWeight: '700',
      letterSpacing: -0.2,
    },
    body: {
      color: c.inkSoft,
      fontSize: 15,
      lineHeight: 22,
    },
    label: {
      color: c.ink,
      fontSize: 15,
      lineHeight: 20,
      fontWeight: '600',
    },
    caption: {
      color: c.inkSoft,
      fontSize: 13,
      lineHeight: 18,
    },
    /** Right-aligned metadata in a label row. */
    meta: {
      color: c.inkFaint,
      fontSize: 13,
      lineHeight: 18,
      fontWeight: '600',
      fontVariant: ['tabular-nums'],
    },
    mono: {
      color: c.inkSoft,
      fontFamily: mono,
      fontSize: 12,
      lineHeight: 18,
    },
    menuBar: { backgroundColor: c.ink },
    accent: { color: c.accent },
    danger: { color: c.danger },

    // --- surfaces -----------------------------------------------------
    card: {
      backgroundColor: c.surface,
      borderRadius: radius.card,
      borderWidth: StyleSheet.hairlineWidth,
      borderColor: c.line,
      overflow: 'hidden',
      ...cardShadow,
    },
    well: {
      backgroundColor: c.surface,
      borderRadius: radius.well,
      borderWidth: StyleSheet.hairlineWidth,
      borderColor: c.line,
    },
    sunken: {
      backgroundColor: c.sunken,
      borderRadius: radius.well,
    },
    /** Hairline that separates rows inside one surface. */
    divider: {
      height: StyleSheet.hairlineWidth,
      backgroundColor: c.lineSoft,
    },
  });
}

export type ThemeStyles = ReturnType<typeof createStyles>;

export interface Theme {
  scheme: 'light' | 'dark';
  c: Palette;
  s: ThemeStyles;
}

const themes: Record<'light' | 'dark', Theme> = {
  light: { scheme: 'light', c: light, s: createStyles(light) },
  dark: { scheme: 'dark', c: dark, s: createStyles(dark) },
};

const ThemeContext = createContext<Theme>(themes.light);

export function ThemeProvider({ children }: { children: ReactNode }) {
  const scheme = useColorScheme() === 'dark' ? 'dark' : 'light';
  return (
    <ThemeContext.Provider value={themes[scheme]}>
      {children}
    </ThemeContext.Provider>
  );
}

export function useTheme(): Theme {
  return useContext(ThemeContext);
}

export type Tone = 'neutral' | 'accent' | 'live' | 'warn' | 'danger' | 'win';

/** Resolves a semantic tone to a readable pair of foreground and fill. */
export function tone(c: Palette, name: Tone): { fg: string; bg: string } {
  switch (name) {
    case 'accent':
      return { fg: c.accent, bg: c.accentSoft };
    case 'live':
      return { fg: c.live, bg: c.liveSoft };
    case 'warn':
      return { fg: c.warn, bg: c.warnSoft };
    case 'danger':
      return { fg: c.danger, bg: c.dangerSoft };
    case 'win':
      return { fg: c.accent, bg: c.win };
    default:
      return { fg: c.inkSoft, bg: c.sunken };
  }
}

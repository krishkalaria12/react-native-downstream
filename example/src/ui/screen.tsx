import { useCallback, useRef, type ReactNode } from 'react';
import {
  Animated,
  Pressable,
  StyleSheet,
  Text,
  View,
  type NativeScrollEvent,
  type NativeSyntheticEvent,
  type StyleProp,
  type ViewStyle,
} from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';

import { useShell } from './shell';
import { space, useTheme } from './theme';

/** Height of the app bar below the top inset. */
export const APP_BAR_HEIGHT = 46;

/**
 * Scroll-linked state for the app bar. The large title lives in the content and
 * scrolls away; the compact title fades in behind the bar to replace it, so the
 * screen is always identifiable without the bar growing.
 */
export interface ScreenScroll {
  onScroll: (event: NativeSyntheticEvent<NativeScrollEvent>) => void;
  /** 0 while the large title is still visible, 1 once it has scrolled past. */
  barOpacity: Animated.AnimatedInterpolation<number> | null;
}

export function useScreenScroll(): ScreenScroll {
  const offset = useRef(new Animated.Value(0)).current;
  const onScroll = useCallback(
    (event: NativeSyntheticEvent<NativeScrollEvent>) => {
      offset.setValue(event.nativeEvent.contentOffset.y);
    },
    [offset]
  );
  return {
    onScroll,
    barOpacity: offset.interpolate({
      inputRange: [12, 46],
      outputRange: [0, 1],
      extrapolate: 'clamp',
    }),
  };
}

/** The large title block that opens a screen's scrolling content. */
export function Hero({
  title,
  summary,
  meta,
  style,
}: {
  title: string;
  summary?: string;
  meta?: ReactNode;
  style?: StyleProp<ViewStyle>;
}) {
  const t = useTheme();
  return (
    <View style={[styles.hero, style]}>
      <Text style={t.s.display}>{title}</Text>
      {summary !== undefined && (
        <Text style={[t.s.body, styles.heroSummary]}>{summary}</Text>
      )}
      {meta}
    </View>
  );
}

/** A label above a block of content, with a hairline that carries the section. */
export function SectionLabel({
  children,
  trailing,
  style,
}: {
  children: ReactNode;
  trailing?: ReactNode;
  style?: StyleProp<ViewStyle>;
}) {
  const t = useTheme();
  return (
    <View style={[styles.section, style]}>
      <View style={[styles.sectionRule, { backgroundColor: t.c.line }]} />
      <View style={styles.sectionRow}>
        <Text style={t.s.heading}>{children}</Text>
        {trailing}
      </View>
    </View>
  );
}

function MenuButton() {
  const t = useTheme();
  const { open, toggle } = useShell();
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityLabel={open ? 'Close menu' : 'Open menu'}
      onPress={toggle}
      hitSlop={6}
      style={({ pressed }) => [styles.menu, pressed && styles.menuPressed]}
    >
      <View style={styles.menuBars}>
        <View style={[styles.menuBar, styles.menuBarLong, t.s.menuBar]} />
        <View style={[styles.menuBar, t.s.menuBar]} />
        <View style={[styles.menuBar, styles.menuBarLong, t.s.menuBar]} />
      </View>
    </Pressable>
  );
}

/**
 * Screen chrome: an app bar over a scrolling body. The bar owns the top inset so
 * no screen has to guess one, and it draws a hairline only once content has
 * slid under it.
 */
export function Screen({
  title,
  scroll,
  children,
}: {
  title: string;
  scroll?: ScreenScroll;
  children: ReactNode;
}) {
  const t = useTheme();
  const insets = useSafeAreaInsets();
  return (
    <View style={[styles.screen, { backgroundColor: t.c.canvas }]}>
      <View style={[styles.bar, { paddingTop: insets.top }]}>
        <View style={styles.barRow}>
          <MenuButton />
        </View>
        {scroll?.barOpacity ? (
          // The compact title sits over the whole bar, so the overlay has to
          // stay out of the way of the buttons underneath it.
          <View
            pointerEvents="none"
            style={[styles.barOverlay, { top: insets.top }]}
          >
            <View style={styles.barOverlayRow}>
              <Animated.Text
                numberOfLines={1}
                accessibilityElementsHidden
                importantForAccessibility="no-hide-descendants"
                style={[
                  styles.barTitle,
                  t.s.label,
                  { opacity: scroll.barOpacity },
                ]}
              >
                {title}
              </Animated.Text>
            </View>
            <Animated.View
              style={[
                styles.barRule,
                { backgroundColor: t.c.line, opacity: scroll.barOpacity },
              ]}
            />
          </View>
        ) : null}
      </View>
      <View style={styles.body}>{children}</View>
    </View>
  );
}

/** Bottom padding that clears the home indicator without a hard-coded inset. */
export function useBottomInset(extra: number = space.xxl): number {
  return useSafeAreaInsets().bottom + extra;
}

const styles = StyleSheet.create({
  screen: { flex: 1 },

  bar: { position: 'relative', zIndex: 2 },
  barRow: {
    height: APP_BAR_HEIGHT,
    flexDirection: 'row',
    alignItems: 'center',
    paddingHorizontal: space.sm,
  },
  barOverlay: { position: 'absolute', left: 0, right: 0, bottom: 0 },
  barOverlayRow: {
    height: APP_BAR_HEIGHT,
    alignItems: 'center',
    justifyContent: 'center',
  },
  barTitle: { paddingHorizontal: 72, textAlign: 'center' },
  barRule: {
    height: StyleSheet.hairlineWidth,
    marginHorizontal: space.xl,
  },

  body: { flex: 1 },

  hero: { paddingHorizontal: space.xl, paddingTop: space.md },
  heroSummary: { marginTop: space.md, maxWidth: 560 },

  section: { paddingHorizontal: space.xl, marginTop: space.xxl },
  sectionRule: { height: 1, marginBottom: space.md },
  sectionRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: space.md,
  },

  menu: {
    width: 46,
    height: 46,
    alignItems: 'center',
    justifyContent: 'center',
    borderRadius: 23,
  },
  menuPressed: { opacity: 0.5 },
  menuBars: { gap: 4, alignItems: 'flex-start' },
  menuBar: { width: 14, height: 2, borderRadius: 1 },
  menuBarLong: { width: 18 },
});

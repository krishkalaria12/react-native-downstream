import { useEffect, useMemo, useRef, useState, type ReactNode } from 'react';
import {
  AccessibilityInfo,
  Animated,
  BackHandler,
  PanResponder,
  Platform,
  Pressable,
  StyleSheet,
  Text,
  View,
  useWindowDimensions,
} from 'react-native';
import { LinearGradient } from 'expo-linear-gradient';
import { Ionicons } from '@expo/vector-icons';
import { useSafeAreaInsets } from 'react-native-safe-area-context';

import { radius, space, useTheme } from './theme';
import type { Route } from '../navigation';

const EDGE = 28;

function clamp01(value: number): number {
  return value < 0 ? 0 : value > 1 ? 1 : value;
}

/**
 * The brand mark: three bars that shorten like a stream settling into a tail.
 * It is the only gradient in the app.
 */
function Mark() {
  const t = useTheme();
  return (
    <LinearGradient
      colors={[t.c.accent, t.c.live]}
      start={{ x: 0, y: 0 }}
      end={{ x: 1, y: 1 }}
      style={styles.mark}
    >
      {[20, 14, 8].map((width) => (
        <View
          key={width}
          style={[styles.markBar, { width, backgroundColor: t.c.onAccent }]}
        />
      ))}
    </LinearGradient>
  );
}

function NavItem({
  route,
  active,
  onPress,
}: {
  route: Route;
  active: boolean;
  onPress: () => void;
}) {
  const t = useTheme();
  return (
    <Pressable
      accessibilityRole="menuitem"
      accessibilityLabel={route.label}
      accessibilityState={{ selected: active }}
      onPress={onPress}
      style={({ pressed }) => [
        styles.navItem,
        { backgroundColor: active ? t.c.accentSoft : 'transparent' },
        pressed && styles.navItemPressed,
      ]}
    >
      <Ionicons
        name={route.icon}
        size={20}
        color={active ? t.c.accent : t.c.inkFaint}
      />
      <View style={styles.navCopy}>
        <Text style={[t.s.label, { color: active ? t.c.accent : t.c.ink }]}>
          {route.label}
        </Text>
        <Text numberOfLines={2} style={t.s.caption}>
          {route.summary}
        </Text>
      </View>
    </Pressable>
  );
}

/**
 * The app's only navigation surface: a drawer, opened from the app bar or by
 * dragging in from the left edge. The gesture runs on the responder system
 * rather than a native navigation library, so it costs nothing at mount and
 * behaves identically on every platform.
 */
export function NavigationDrawer({
  open,
  onOpenChange,
  route,
  routes,
  onSelect,
  children,
}: {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  route: Route;
  routes: readonly Route[];
  onSelect: (id: Route['id']) => void;
  children: ReactNode;
}) {
  const t = useTheme();
  const insets = useSafeAreaInsets();
  const { width: windowWidth } = useWindowDimensions();
  const panelWidth = Math.min(windowWidth * 0.84, 336);
  const progress = useRef(new Animated.Value(open ? 1 : 0)).current;
  const openRef = useRef(open);
  const changeRef = useRef(onOpenChange);
  const widthRef = useRef(panelWidth);
  const [reduceMotion, setReduceMotion] = useState(false);

  useEffect(() => {
    openRef.current = open;
    changeRef.current = onOpenChange;
    widthRef.current = panelWidth;
  }, [open, onOpenChange, panelWidth]);

  useEffect(() => {
    AccessibilityInfo.isReduceMotionEnabled().then(
      (value) => setReduceMotion(value),
      () => {}
    );
    const subscription = AccessibilityInfo.addEventListener(
      'reduceMotionChanged',
      setReduceMotion
    );
    return () => subscription.remove();
  }, []);

  useEffect(() => {
    if (reduceMotion) {
      progress.setValue(open ? 1 : 0);
      return;
    }
    const animation = Animated.spring(progress, {
      toValue: open ? 1 : 0,
      useNativeDriver: true,
      speed: 18,
      bounciness: 0,
      restDisplacementThreshold: 0.0005,
    });
    animation.start();
    return () => animation.stop();
  }, [open, progress, reduceMotion]);

  useEffect(() => {
    if (!open || Platform.OS !== 'android') return;
    const subscription = BackHandler.addEventListener(
      'hardwareBackPress',
      () => {
        changeRef.current(false);
        return true;
      }
    );
    return () => subscription.remove();
  }, [open]);

  const panResponder = useMemo(
    () =>
      PanResponder.create({
        // A tap must never open the drawer, so nothing is claimed until a
        // drag actually starts.
        onStartShouldSetPanResponderCapture: () => false,
        // Only a horizontal drag counts, so taps and vertical scrolling reach
        // the screen underneath untouched.
        onMoveShouldSetPanResponderCapture: (_event, gesture) => {
          const horizontal = Math.abs(gesture.dx) > Math.abs(gesture.dy) * 1.5;
          if (openRef.current) return horizontal && gesture.dx < -6;
          return horizontal && gesture.dx > 6 && gesture.x0 <= EDGE;
        },
        onPanResponderGrant: () => {
          progress.stopAnimation();
        },
        onPanResponderMove: (_event, gesture) => {
          const base = openRef.current ? 1 : 0;
          progress.setValue(clamp01(base + gesture.dx / widthRef.current));
        },
        onPanResponderRelease: (_event, gesture) => {
          const base = openRef.current ? 1 : 0;
          const next = clamp01(base + gesture.dx / widthRef.current);
          // A flick wins over distance, so a short fast swipe still opens it.
          if (gesture.vx > 0.4) changeRef.current(true);
          else if (gesture.vx < -0.4) changeRef.current(false);
          else changeRef.current(next > 0.5);
        },
        onPanResponderTerminate: () => {
          changeRef.current(openRef.current);
        },
      }),
    [progress]
  );

  return (
    <View
      style={[styles.root, { backgroundColor: t.c.canvas }]}
      {...panResponder.panHandlers}
    >
      <Animated.View
        style={[
          styles.content,
          {
            transform: [
              {
                translateX: progress.interpolate({
                  inputRange: [0, 1],
                  outputRange: [0, panelWidth * 0.34],
                }),
              },
              {
                scale: progress.interpolate({
                  inputRange: [0, 1],
                  outputRange: [1, 0.94],
                }),
              },
            ],
          },
        ]}
      >
        {children}
      </Animated.View>

      <Animated.View
        pointerEvents={open ? 'auto' : 'none'}
        style={[styles.scrim, { opacity: progress }]}
      >
        <Pressable
          accessibilityRole="button"
          accessibilityLabel="Close menu"
          style={[StyleSheet.absoluteFill, { backgroundColor: t.c.scrim }]}
          onPress={() => onOpenChange(false)}
        />
      </Animated.View>

      <Animated.View
        pointerEvents={open ? 'auto' : 'none'}
        accessibilityViewIsModal={open}
        style={[
          styles.panel,
          {
            width: panelWidth,
            paddingTop: insets.top + space.xxl,
            backgroundColor: t.c.surface,
            borderColor: t.c.line,
            transform: [
              {
                translateX: progress.interpolate({
                  inputRange: [0, 1],
                  outputRange: [-panelWidth, 0],
                }),
              },
            ],
          },
        ]}
      >
        <View style={styles.panelHead}>
          <Mark />
          <View style={styles.panelTitle}>
            <Text style={[t.s.title, styles.panelName]}>Downstream</Text>
            <Text style={t.s.caption}>Markdown streaming lab</Text>
          </View>
        </View>

        <View style={styles.nav}>
          {routes.map((item) => (
            <NavItem
              key={item.id}
              route={item}
              active={item.id === route.id}
              onPress={() => {
                onSelect(item.id);
                onOpenChange(false);
              }}
            />
          ))}
        </View>

        <View style={[styles.panelFoot, { borderTopColor: t.c.lineSoft }]}>
          <Text style={t.s.caption}>
            Every screen replays the same fixtures and schedules, so two builds
            can be compared on one device.
          </Text>
        </View>
      </Animated.View>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, overflow: 'hidden' },
  content: { flex: 1, transformOrigin: 'left center' },
  scrim: { ...StyleSheet.absoluteFillObject },

  panel: {
    position: 'absolute',
    top: 0,
    bottom: 0,
    left: 0,
    borderRightWidth: StyleSheet.hairlineWidth,
    paddingBottom: space.xxl,
    justifyContent: 'space-between',
  },

  panelHead: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: space.md,
    paddingHorizontal: space.xl,
  },
  panelTitle: { flex: 1 },
  panelName: { letterSpacing: -0.5 },
  panelFoot: {
    paddingHorizontal: space.xl,
    paddingTop: space.lg,
    borderTopWidth: StyleSheet.hairlineWidth,
  },

  mark: {
    width: 42,
    height: 42,
    borderRadius: radius.well,
    alignItems: 'flex-start',
    justifyContent: 'center',
    gap: 3,
    paddingLeft: 11,
  },
  markBar: { height: 2.5, borderRadius: 2 },

  nav: { flex: 1, paddingTop: space.xxl, gap: 2 },
  navItem: {
    flexDirection: 'row',
    alignItems: 'flex-start',
    gap: space.md,
    marginHorizontal: space.md,
    paddingHorizontal: space.md,
    paddingVertical: space.md,
    borderRadius: radius.well,
  },
  navItemPressed: { opacity: 0.6 },
  navCopy: { flex: 1, gap: 2 },
});

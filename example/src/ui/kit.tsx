import { useEffect, useRef, type ReactNode } from 'react';
import {
  Animated,
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native';
import type { TextInputProps, ViewStyle } from 'react-native';

import { radius, space, tone as toneOf, useTheme } from './theme';
import type { Tone } from './theme';

/** iOS and Material both treat 44pt as the smallest comfortable target. */
const HIT = 44;

type ButtonVariant = 'primary' | 'secondary' | 'ghost' | 'danger';

export function Button({
  label,
  onPress,
  variant = 'secondary',
  disabled,
  grow,
  style,
  hint,
}: {
  label: string;
  onPress: () => void;
  variant?: ButtonVariant;
  disabled?: boolean;
  /** Shares the remaining width of a button row. */
  grow?: boolean;
  style?: ViewStyle;
  hint?: string;
}) {
  const t = useTheme();
  const fg =
    variant === 'primary'
      ? t.c.onAccent
      : variant === 'danger'
        ? t.c.danger
        : variant === 'ghost'
          ? t.c.inkSoft
          : t.c.ink;
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityLabel={label}
      accessibilityHint={hint}
      accessibilityState={{ disabled: !!disabled }}
      disabled={disabled}
      onPress={onPress}
      style={({ pressed }) => [
        styles.button,
        grow && styles.grow,
        variant === 'secondary' && {
          backgroundColor: t.c.surface,
          borderColor: t.c.line,
        },
        variant === 'ghost' && { backgroundColor: 'transparent' },
        variant === 'primary' && {
          backgroundColor: t.c.accent,
          borderColor: t.c.accent,
        },
        variant === 'danger' && {
          backgroundColor: t.c.dangerSoft,
          borderColor: t.c.dangerSoft,
        },
        disabled && styles.faded,
        pressed && !disabled && styles.pressed,
        style,
      ]}
    >
      <Text numberOfLines={1} style={[t.s.label, { color: fg }]}>
        {label}
      </Text>
    </Pressable>
  );
}

export function ButtonRow({ children }: { children: ReactNode }) {
  return <View style={styles.buttonRow}>{children}</View>;
}

export interface ChipOption<T extends string | number> {
  value: T;
  label: string;
}

/**
 * A single-select row of pills. Rows scroll horizontally rather than wrapping, so
 * the layout never reflows under the thumb and a long option set stays reachable
 * without the row growing into a wall of boxes.
 */
export function ChipRow<T extends string | number>({
  options,
  value,
  onChange,
  inset,
}: {
  options: readonly ChipOption<T>[];
  value: T;
  onChange: (value: T) => void;
  /** Aligns the row to a `Group` title instead of the page gutter. */
  inset?: boolean;
}) {
  const t = useTheme();
  return (
    <ScrollView
      horizontal
      showsHorizontalScrollIndicator={false}
      keyboardShouldPersistTaps="handled"
      contentContainerStyle={[
        styles.chipScroll,
        { paddingHorizontal: inset ? space.lg : space.xl },
      ]}
    >
      {options.map((option) => {
        const selected = option.value === value;
        return (
          <Pressable
            key={String(option.value)}
            accessibilityRole="radio"
            accessibilityLabel={option.label}
            accessibilityState={{ selected }}
            onPress={() => onChange(option.value)}
            style={({ pressed }) => [
              styles.chip,
              {
                backgroundColor: selected ? t.c.accent : t.c.surface,
                borderColor: selected ? t.c.accent : t.c.line,
              },
              pressed && styles.pressed,
            ]}
          >
            <Text
              style={[
                styles.chipText,
                { color: selected ? t.c.onAccent : t.c.ink },
              ]}
            >
              {option.label}
            </Text>
          </Pressable>
        );
      })}
    </ScrollView>
  );
}

/**
 * A grouped settings surface: a title, the control, and an optional explanation.
 * Related groups share one card and are split by hairlines, so a screen with six
 * option groups reads as one form instead of six floating cards.
 */
export function Group({
  title,
  children,
  style,
}: {
  title?: string;
  children: ReactNode;
  style?: ViewStyle;
}) {
  const t = useTheme();
  return (
    <View style={[t.s.well, styles.group, style]}>
      {title !== undefined && (
        <View style={styles.groupHead}>
          <Text style={t.s.label}>{title}</Text>
        </View>
      )}
      {children}
    </View>
  );
}

export function Divider({ inset = true }: { inset?: boolean }) {
  const t = useTheme();
  return <View style={[t.s.divider, inset && styles.dividerInset]} />;
}

export function Field({
  label,
  children,
}: {
  label: string;
  children: ReactNode;
}) {
  const t = useTheme();
  return (
    <View style={styles.field}>
      <Text style={[t.s.caption, styles.fieldLabel]}>{label}</Text>
      {children}
    </View>
  );
}

export function Input({ style, ...rest }: TextInputProps) {
  const t = useTheme();
  return (
    <TextInput
      placeholderTextColor={t.c.inkFaint}
      selectionColor={t.c.accent}
      {...rest}
      style={[
        styles.input,
        { backgroundColor: t.c.canvas, borderColor: t.c.line, color: t.c.ink },
        style,
      ]}
    />
  );
}

/** A status pill. `live` tones pulse, because they mean "right now". */
export function Badge({
  label,
  tone = 'neutral',
  pulse,
}: {
  label: string;
  tone?: Tone;
  pulse?: boolean;
}) {
  const t = useTheme();
  const { fg, bg } = toneOf(t.c, tone);
  const fade = useRef(new Animated.Value(1)).current;

  useEffect(() => {
    if (!pulse) return;
    const loop = Animated.loop(
      Animated.sequence([
        Animated.timing(fade, {
          toValue: 0.4,
          duration: 650,
          useNativeDriver: true,
        }),
        Animated.timing(fade, {
          toValue: 1,
          duration: 650,
          useNativeDriver: true,
        }),
      ])
    );
    loop.start();
    return () => loop.stop();
  }, [fade, pulse]);

  return (
    <Animated.View
      style={[styles.badge, { backgroundColor: bg, opacity: fade }]}
    >
      {pulse && <View style={[styles.badgeDot, { backgroundColor: fg }]} />}
      <Text style={[styles.badgeText, { color: fg }]}>{label}</Text>
    </Animated.View>
  );
}

export function Notice({
  tone = 'neutral',
  title,
  children,
  style,
}: {
  tone?: Tone;
  title?: string;
  children: ReactNode;
  style?: ViewStyle;
}) {
  const t = useTheme();
  const { fg, bg } = toneOf(t.c, tone);
  return (
    <View
      accessibilityRole="alert"
      style={[styles.notice, { backgroundColor: bg }, style]}
    >
      <View style={[styles.noticeBar, { backgroundColor: fg }]} />
      <View style={styles.noticeBody}>
        {title !== undefined && (
          <Text style={[t.s.label, { color: fg }]}>{title}</Text>
        )}
        <Text style={t.s.caption}>{children}</Text>
      </View>
    </View>
  );
}

export function EmptyState({
  title,
  children,
}: {
  title: string;
  children: ReactNode;
}) {
  const t = useTheme();
  return (
    <View style={styles.empty}>
      <View style={[styles.emptyRule, { backgroundColor: t.c.line }]} />
      <Text style={[t.s.heading, styles.emptyTitle]}>{title}</Text>
      <Text style={[t.s.body, styles.emptyBody]}>{children}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  faded: { opacity: 0.38 },
  pressed: { opacity: 0.6 },
  grow: { flexGrow: 1, flexBasis: 0, flexShrink: 1 },

  button: {
    minHeight: HIT,
    paddingHorizontal: space.xl,
    alignItems: 'center',
    justifyContent: 'center',
    borderRadius: radius.pill,
    borderWidth: 1,
    borderColor: 'transparent',
  },
  buttonRow: { flexDirection: 'row', flexWrap: 'wrap', gap: space.sm },

  chip: {
    minHeight: 38,
    paddingHorizontal: space.lg,
    justifyContent: 'center',
    borderRadius: radius.pill,
    borderWidth: 1,
  },
  chipText: { fontSize: 14, fontWeight: '600' },
  chipScroll: {
    flexDirection: 'row',
    gap: space.sm,
    paddingVertical: 2,
  },

  group: { overflow: 'hidden' },
  groupHead: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: space.md,
    paddingHorizontal: space.lg,
    paddingTop: space.lg,
    paddingBottom: space.md,
  },
  dividerInset: { marginLeft: space.lg },

  field: { paddingHorizontal: space.lg, paddingVertical: space.md },
  fieldLabel: { marginBottom: space.sm },
  input: {
    minHeight: HIT,
    borderWidth: 1,
    borderRadius: radius.well,
    paddingHorizontal: space.lg,
    paddingVertical: space.md,
    fontSize: 15,
    lineHeight: 20,
  },

  badge: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 6,
    alignSelf: 'flex-start',
    paddingHorizontal: space.md,
    paddingVertical: 5,
    borderRadius: radius.pill,
  },
  badgeDot: { width: 6, height: 6, borderRadius: 3 },
  badgeText: { fontSize: 12, fontWeight: '700' },

  notice: {
    flexDirection: 'row',
    gap: space.md,
    borderRadius: radius.well,
    padding: space.lg,
  },
  noticeBar: { width: 3, borderRadius: 2 },
  noticeBody: { flex: 1, gap: 2 },

  empty: { paddingVertical: space.xxl, paddingHorizontal: space.xl },
  emptyRule: { width: 28, height: 2, borderRadius: 1, marginBottom: space.lg },
  emptyTitle: { marginBottom: space.sm },
  emptyBody: { maxWidth: 480 },
});

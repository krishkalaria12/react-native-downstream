import type { ReactNode } from 'react';
import { StyleSheet, Text, View } from 'react-native';
import type { StyleProp, ViewStyle } from 'react-native';

import { space, useTheme } from './theme';
import type { Tone } from './theme';
import { Badge } from './kit';

/** A readout figure with an optional unit. */
function Figure({
  value,
  unit,
  tone,
}: {
  value: string;
  unit?: string;
  tone?: Tone;
}) {
  const t = useTheme();
  return (
    <View style={styles.figure}>
      <Text
        numberOfLines={1}
        style={[
          t.s.figure,
          tone === 'live' && { color: t.c.live },
          tone === 'accent' && { color: t.c.accent },
        ]}
      >
        {value}
      </Text>
      {unit !== undefined && <Text style={t.s.figureUnit}>{unit}</Text>}
    </View>
  );
}

export function ProgressBar({ value }: { value: number }) {
  const t = useTheme();
  const clamped = value < 0 ? 0 : value > 1 ? 1 : value;
  return (
    <View
      accessibilityRole="progressbar"
      accessibilityValue={{ min: 0, max: 100, now: Math.round(clamped * 100) }}
      style={[styles.track, { backgroundColor: t.c.sunken }]}
    >
      <View
        style={[
          styles.fill,
          {
            width: `${clamped * 100}%`,
            backgroundColor: t.c.live,
          },
        ]}
      />
    </View>
  );
}

export interface ReadoutFigure {
  label: string;
  value: string;
  unit?: string;
  tone?: Tone;
}

/**
 * The one loud element in the app: a card that states what a run is doing right
 * now. Bench, Testing, and Stress all show one, so a glance at any of them
 * answers "is it running, and how far along".
 */
export function RunReadout({
  status,
  statusTone,
  meta,
  progress,
  figures,
}: {
  status: string;
  statusTone: Tone;
  meta?: string;
  progress?: { value: number; label: string };
  figures?: readonly ReadoutFigure[];
}) {
  const t = useTheme();
  return (
    <View style={[t.s.card, styles.readout]}>
      <View style={styles.readoutHead}>
        <Badge label={status} tone={statusTone} pulse={statusTone === 'live'} />
        {meta !== undefined && (
          <Text numberOfLines={1} style={[t.s.meta, styles.readoutMeta]}>
            {meta}
          </Text>
        )}
      </View>

      {progress !== undefined && (
        <View style={styles.progressBlock}>
          <ProgressBar value={progress.value} />
          <Text style={[t.s.meta, styles.progressLabel]}>{progress.label}</Text>
        </View>
      )}

      {figures !== undefined && figures.length > 0 && (
        <View style={[styles.figures, { borderTopColor: t.c.lineSoft }]}>
          {figures.map((figure, index) => (
            <View key={figure.label} style={styles.figureCell}>
              {index > 0 && (
                <View
                  style={[
                    styles.figureDivider,
                    { backgroundColor: t.c.lineSoft },
                  ]}
                />
              )}
              <Figure
                value={figure.value}
                unit={figure.unit}
                tone={figure.tone}
              />
              <Text numberOfLines={2} style={[t.s.caption, styles.figureLabel]}>
                {figure.label}
              </Text>
            </View>
          ))}
        </View>
      )}
    </View>
  );
}

/**
 * A framed Markdown surface. The two candidates in Testing sit in identical
 * panes, in the same place in the tree, so neither renderer gets a layout
 * advantage from where it is.
 */
export function DocumentPane({
  label,
  state,
  tone = 'neutral',
  pulse,
  children,
  style,
}: {
  label: string;
  state?: string;
  tone?: Tone;
  pulse?: boolean;
  children: ReactNode;
  style?: StyleProp<ViewStyle>;
}) {
  const t = useTheme();
  return (
    <View style={[t.s.card, styles.pane, style]}>
      <View style={[styles.paneHead, { borderBottomColor: t.c.lineSoft }]}>
        <Text numberOfLines={1} style={[t.s.label, styles.paneLabel]}>
          {label}
        </Text>
        {state !== undefined && (
          <Badge label={state} tone={tone} pulse={pulse} />
        )}
      </View>
      <View style={styles.paneBody}>{children}</View>
    </View>
  );
}

const styles = StyleSheet.create({
  track: { height: 6, borderRadius: 3, overflow: 'hidden' },
  fill: { height: '100%', borderRadius: 3 },

  readout: { padding: space.lg, gap: space.lg },
  readoutHead: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: space.md,
  },
  readoutMeta: { flexShrink: 1, textAlign: 'right' },

  progressBlock: { gap: space.sm },
  progressLabel: { alignSelf: 'flex-end' },

  figures: { flexDirection: 'row', borderTopWidth: StyleSheet.hairlineWidth },
  figureCell: { flex: 1, gap: 2, paddingTop: space.md },
  figureLabel: { fontSize: 12, lineHeight: 16 },
  figure: { flexDirection: 'row', alignItems: 'baseline', gap: 4 },
  figureDivider: {
    position: 'absolute',
    left: 0,
    top: space.md,
    bottom: 0,
    width: StyleSheet.hairlineWidth,
  },

  pane: { overflow: 'hidden' },
  paneHead: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: space.md,
    paddingHorizontal: space.lg,
    paddingVertical: space.md,
    borderBottomWidth: StyleSheet.hairlineWidth,
  },
  paneLabel: { flexShrink: 1 },
  paneBody: { padding: space.lg },
});

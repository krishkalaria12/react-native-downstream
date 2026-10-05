import { lazy } from 'react';
import { StyleSheet, Text, View } from 'react-native';

/**
 * The renderers this screen registers.
 *
 * They are the reference for "parse the JSON, guard `isIncomplete`, draw", and
 * the three shapes worth seeing: one that draws, one that loads lazily, and one
 * that throws. Nothing here depends on a library: a bar chart of `Views` is
 * enough to show that a fence became a component, that it is measured rather
 * than guessed at, and that the code card is underneath whenever the component
 * is not up.
 */

/** One bar's label and height, as the fenced JSON gives them. */
export interface Bar {
  label: string;
  value: number;
}

/** The spec a chart fence carries, after parsing and clamping. */
function parseChart(code: string): Bar[] | null {
  let parsed: unknown;
  try {
    parsed = JSON.parse(code);
  } catch {
    return null;
  }
  if (!Array.isArray(parsed)) {
    return null;
  }
  const bars: Bar[] = [];
  for (const entry of parsed) {
    if (
      typeof entry !== 'object' ||
      entry === null ||
      typeof (entry as Bar).label !== 'string' ||
      typeof (entry as Bar).value !== 'number'
    ) {
      return null;
    }
    bars.push({ label: (entry as Bar).label, value: (entry as Bar).value });
  }
  return bars.length > 0 ? bars : null;
}

/** A resolved spec, or the reason it is not one yet. */
type ChartSpec =
  | { state: 'ready'; bars: Bar[] }
  | { state: 'incomplete' }
  | { state: 'invalid'; reason: string };

function readChart(code: string, isIncomplete: boolean): ChartSpec {
  if (isIncomplete) {
    // A half-written spec is not a failure, so it is not drawn as one: the
    // renderer reserves a fixed height and says so, which is what keeps the text
    // below it from jumping twice in a stream.
    return { state: 'incomplete' };
  }
  const bars = parseChart(code);
  return bars === null
    ? { state: 'invalid', reason: 'Expected an array of {label, value}.' }
    : { state: 'ready', bars };
}

export const CHART_HEIGHT = 140;

/**
 * The bar chart renderer, about sixty lines and dependency-free.
 *
 * Every branch here is the pattern the docs recommend: guard `isIncomplete` and
 * reserve the height you will use, so a streaming spec never reflows the
 * message, and treat a spec you cannot read as a sentence rather than as a crash.
 */
export function ChartRenderer(props: {
  code: string;
  language: string;
  meta?: string;
  isIncomplete: boolean;
}) {
  const spec = readChart(props.code, props.isIncomplete);
  if (spec.state === 'invalid') {
    return (
      <ChartFrame caption={props.meta ?? props.language}>
        <Text style={styles.error}>{spec.reason}</Text>
      </ChartFrame>
    );
  }
  if (spec.state === 'incomplete') {
    return (
      <ChartFrame caption={props.meta ?? props.language} placeholder>
        <Text style={styles.placeholder}>Reading the chart…</Text>
      </ChartFrame>
    );
  }
  const peak = Math.max(...spec.bars.map((bar) => bar.value), 1);
  return (
    <ChartFrame caption={props.meta ?? props.language}>
      <View style={styles.bars}>
        {spec.bars.map((bar) => (
          <View key={bar.label} style={styles.bar}>
            <View
              style={[
                styles.fill,
                { height: Math.max(2, (bar.value / peak) * CHART_HEIGHT) },
              ]}
            />
            <Text style={styles.label}>{bar.label}</Text>
          </View>
        ))}
      </View>
    </ChartFrame>
  );
}

function ChartFrame({
  children,
  caption,
  placeholder,
}: {
  children: React.ReactNode;
  caption: string;
  placeholder?: boolean;
}) {
  return (
    <View
      accessible
      accessibilityLabel={`Chart, ${caption}`}
      // The height is fixed whether or not there is anything to draw yet, which
      // is what keeps the text below a streaming chart from moving twice: once
      // for the placeholder, once for the chart.
      style={[styles.frame, placeholder === true && styles.framePlaceholder]}
    >
      <View style={styles.captionRow}>
        <Text style={styles.caption}>{caption}</Text>
      </View>
      {children}
    </View>
  );
}

/**
 * A renderer that throws, so the screen can show the fallback the feature is
 * built around: the code card, with its copy and download, under a message.
 */
export function BrokenRenderer(props: {
  code: string;
  language: string;
  meta?: string;
  isIncomplete: boolean;
}): React.ReactNode {
  throw new Error(`this renderer always fails: ${props.code.slice(0, 24)}`);
}

/**
 * A lazily loaded renderer.
 *
 * `lazy` is the ordinary way an app splits a heavy renderer out of its bundle,
 * and it is also the case the code card exists for: until the module resolves no
 * child is mounted at all, so the fence draws the card rather than an empty box.
 */
export const LazyChart = lazy(async () => ({
  default: ChartRenderer as React.ComponentType<{
    code: string;
    language: string;
    meta?: string;
    isIncomplete: boolean;
  }>,
}));

const styles = StyleSheet.create({
  frame: {
    borderRadius: 10,
    borderWidth: 1,
    borderColor: '#3b3f46',
    backgroundColor: '#15181d',
    padding: 12,
    minHeight: CHART_HEIGHT + 46,
    justifyContent: 'center',
  },
  framePlaceholder: {
    backgroundColor: '#1b1e24',
  },
  captionRow: {
    paddingHorizontal: 3,
    paddingBottom: 6,
  },
  caption: {
    fontSize: 11,
    opacity: 0.6,
  },
  bars: {
    flexDirection: 'row',
    alignItems: 'flex-end',
    height: CHART_HEIGHT,
  },
  bar: {
    flex: 1,
    paddingHorizontal: 3,
  },
  fill: {
    borderRadius: 4,
    backgroundColor: '#3b82f6',
  },
  label: {
    fontSize: 11,
    textAlign: 'center',
    marginTop: 4,
  },
  error: {
    color: '#e5484d',
  },
  placeholder: {
    opacity: 0.6,
  },
});

import { useEffect, useRef, useState } from 'react';
import { Platform, ScrollView, StyleSheet, Text, View } from 'react-native';
import { File, Paths } from 'expo-file-system';
import {
  createMarkdownStream,
  DownstreamMarkdown,
  type MarkdownStreamController,
} from 'react-native-downstream';

import {
  setStreamTracing,
  subscribeStreamTrace,
  type StreamTraceEvent,
} from '../../../src/streamTrace';
import { EmptyState } from '../ui/kit';
import { RunReadout } from '../ui/readout';
import { Hero, Screen, useBottomInset } from '../ui/screen';
import { space, useTheme } from '../ui/theme';
import { nowMs } from './trace';

// A chat screen under load: a conversation's history mounts at once, then
// several answers stream at the same time. The run records how long the
// JavaScript thread went without a frame and where native time went, so two
// builds can be compared on the same device.
const HISTORY_MESSAGES = 40;
const STREAMS = 4;
const STREAM_BYTES = 40 * 1024;
const CHUNK_CHARS = 48;
const CHUNK_INTERVAL_MS = 16;

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(() => resolve(), ms));
}

function section(index: number): string {
  return `## Step ${index}: tune the renderer

When a model streams an answer, the **renderer** must keep up without blocking input. This paragraph mixes *emphasis*, \`inline code\`, and a [reference](https://example.com/docs/${index}) so every inline style is measured.

- Parse on a worker, never on the UI thread
- Reuse the layout of blocks that stopped changing
  - Nested detail for list layout
- Measure once, mount many times

\`\`\`ts
export function step${index}(input: string): number {
  return input.length * ${index};
}
\`\`\`

| Metric | Before | After |
| :----- | -----: | ----: |
| Parse | ${index} ms | ${index % 7} ms |
| Layout | ${index * 2} ms | ${index % 3} ms |

> Tip: long answers are where full reparsing hurts the most.

`;
}

function answer(bytes: number, seed: number): string {
  let text = '';
  for (let index = seed; text.length < bytes; index++) text += section(index);
  return text;
}

type Stats = {
  count: number;
  p50: number;
  p95: number;
  max: number;
  over32: number;
};

function stats(values: number[]): Stats {
  if (values.length === 0)
    return { count: 0, p50: 0, p95: 0, max: 0, over32: 0 };
  const sorted = [...values].sort((a, b) => a - b);
  const at = (q: number) =>
    sorted[Math.min(sorted.length - 1, Math.floor(q * sorted.length))]!;
  const round = (value: number) => Math.round(value * 100) / 100;
  return {
    count: values.length,
    p50: round(at(0.5)),
    p95: round(at(0.95)),
    max: round(sorted[sorted.length - 1]!),
    over32: values.filter((value) => value > 32).length,
  };
}

/** Records gaps between JavaScript animation frames until stopped. */
function frameGaps() {
  const gaps: number[] = [];
  let last = nowMs();
  let frame = requestAnimationFrame(function tick(now) {
    gaps.push(now - last);
    last = now;
    frame = requestAnimationFrame(tick);
  });
  return () => {
    cancelAnimationFrame(frame);
    return gaps;
  };
}

/** Pairs native trace markers per snapshot and reports each span's duration in ms. */
function nativeSpans(events: StreamTraceEvent[]) {
  const starts = new Map<string, number>();
  const spans: Record<string, number[]> = {};
  const add = (name: string, value: number) => (spans[name] ??= []).push(value);
  const pairs: Record<string, string> = {
    parse_end: 'parse_start',
    prepare_end: 'prepare_start',
    measure_end: 'measure_start',
    apply_end: 'apply_start',
    frame_estimate: 'apply_start',
  };
  for (const event of events) {
    if (!('timeNs' in event)) continue;
    const key = `${event.sessionId}:${event.generation}:${event.sequence}`;
    const time = Number(event.timeNs) / 1e6;
    const startName = pairs[event.event];
    if (startName) {
      const start = starts.get(`${key}:${startName}`);
      if (start !== undefined)
        add(
          `${startName.replace('_start', '')}${
            event.event === 'frame_estimate' ? '_to_frame' : ''
          }`,
          time - start
        );
    } else {
      starts.set(`${key}:${event.event}`, time);
    }
    if (event.event === 'snapshot_ready') add('snapshots', 1);
  }
  return Object.fromEntries(
    Object.entries(spans).map(([name, values]) => [
      name,
      name === 'snapshots' ? values.length : stats(values),
    ])
  );
}

interface StressSummary {
  history: { allLaidOutMs: number };
  streams: { wallMs: number };
}

export function StressScreen({ autoStart = false }: { autoStart?: boolean }) {
  const t = useTheme();
  const [history, setHistory] = useState<string[]>([]);
  const [streams, setStreams] = useState<MarkdownStreamController[]>([]);
  const [phase, setPhase] = useState<'idle' | 'history' | 'streaming' | 'done'>(
    'idle'
  );
  const [log, setLog] = useState('');
  const [summary, setSummary] = useState<StressSummary | null>(null);
  const laidOut = useRef(new Set<number>());
  const historyDone = useRef<() => void>(() => {});

  useEffect(() => {
    if (!autoStart) return;
    let cancelled = false;
    const traces: StreamTraceEvent[] = [];
    const unsubscribe = subscribeStreamTrace((event) => traces.push(event));
    const controllers: MarkdownStreamController[] = [];
    const timers: ReturnType<typeof setInterval>[] = [];

    (async () => {
      await sleep(1500);
      // Phase 1: a conversation's history mounts in one commit.
      setPhase('history');
      // One long answer is prepared without blocking layout and appears
      // when it is ready; the run waits until every message has a height.
      const messages = Array.from({ length: HISTORY_MESSAGES }, (_, index) =>
        answer(
          index === 0 ? 64 * 1024 : 2 * 1024 + (index % 5) * 1024,
          index * 3
        )
      );
      const stopHistoryGaps = frameGaps();
      const historyStart = nowMs();
      const allLaidOut = new Promise<void>((resolve) => {
        historyDone.current = () => resolve();
      });
      laidOut.current.clear();
      setHistory(messages);
      await allLaidOut;
      const historyMs = nowMs() - historyStart;
      await sleep(500);
      const historyGaps = stats(stopHistoryGaps());
      if (cancelled) return;

      // Phase 2: several answers stream at once.
      setPhase('streaming');
      const sources = Array.from({ length: STREAMS }, (_, index) =>
        answer(STREAM_BYTES, 1000 + index * 50)
      );
      for (let index = 0; index < STREAMS; index++) {
        const controller = createMarkdownStream();
        setStreamTracing(controller, true);
        controllers.push(controller);
      }
      setStreams(controllers);
      const stopStreamGaps = frameGaps();
      const streamStart = nowMs();
      await Promise.all(
        controllers.map(
          (controller, index) =>
            new Promise<void>((resolve) => {
              const source = sources[index]!;
              let offset = 0;
              const timer = setInterval(() => {
                if (offset >= source.length) {
                  clearInterval(timer);
                  controller.finish();
                  resolve();
                  return;
                }
                controller.append(source.slice(offset, offset + CHUNK_CHARS));
                offset += CHUNK_CHARS;
              }, CHUNK_INTERVAL_MS);
              timers.push(timer);
            })
        )
      );
      await sleep(1500);
      const streamMs = nowMs() - streamStart;
      const streamGaps = stats(stopStreamGaps());
      if (cancelled) return;

      const report = {
        platform: Platform.OS,
        history: {
          messages: HISTORY_MESSAGES,
          allLaidOutMs: Math.round(historyMs),
          jsFrameGapsMs: historyGaps,
        },
        streams: {
          count: STREAMS,
          bytesEach: STREAM_BYTES,
          wallMs: Math.round(streamMs),
          jsFrameGapsMs: streamGaps,
          native: nativeSpans(traces),
        },
      };
      console.log(`DOWNSTREAM_STRESS ${JSON.stringify(report)}`);
      // Release builds may not log; the file is read from the app container.
      new File(Paths.cache, 'downstream-stress.json').write(
        JSON.stringify(report)
      );
      setSummary(report);
      setPhase('done');
      setLog(JSON.stringify(report, null, 1));
    })();

    return () => {
      cancelled = true;
      timers.forEach(clearInterval);
      controllers.forEach((controller) => controller.dispose());
      unsubscribe();
    };
  }, [autoStart]);

  const done = phase === 'done';
  const bottom = useBottomInset();
  const label =
    phase === 'history'
      ? 'mounting history'
      : phase === 'streaming'
      ? 'streaming'
      : done
      ? 'finished'
      : 'idle';

  return (
    <Screen title="Stress">
      {/* The readout stays put while the documents scroll, so the numbers can
          be watched without losing the run that produced them. */}
      <View style={styles.header}>
        <Hero
          title="Concurrent stress"
          summary="Mounts a conversation's history in one commit, then streams four answers at once, so two builds can be compared on the same device."
          meta={
            <Text style={[t.s.meta, styles.meta]}>
              {HISTORY_MESSAGES} archived · {STREAMS} live ·{' '}
              {Math.round(STREAM_BYTES / 1024)} KiB each
            </Text>
          }
        />

        {phase !== 'idle' && (
          <View style={styles.statusBlock}>
            <RunReadout
              status={label}
              statusTone={done ? 'accent' : 'live'}
              figures={
                done && summary
                  ? [
                      {
                        label: 'history laid out',
                        value: String(summary.history.allLaidOutMs),
                        unit: 'ms',
                      },
                      {
                        label: 'streaming wall',
                        value: String(summary.streams.wallMs),
                        unit: 'ms',
                      },
                    ]
                  : undefined
              }
            />
            {log !== '' && (
              <View style={[t.s.sunken, styles.raw]}>
                <Text selectable style={t.s.mono}>
                  {log}
                </Text>
              </View>
            )}
          </View>
        )}
      </View>
      <ScrollView
        style={styles.flex}
        contentContainerStyle={[styles.documents, { paddingBottom: bottom }]}
      >
        {streams.length === 0 && history.length === 0 && (
          <EmptyState title="This screen runs itself">
            It has no controls on purpose. Launch the app with{' '}
            <Text style={t.s.mono}>-downstreamStress 1</Text> on iOS, or a{' '}
            <Text style={t.s.mono}>downstream://stress</Text> VIEW intent on
            Android, and the whole sequence runs unattended.
          </EmptyState>
        )}
        {streams.map((stream, index) => (
          <View key={`stream-${index}`} style={styles.message}>
            <DownstreamMarkdown stream={stream} />
          </View>
        ))}
        {history.map((markdown, index) => (
          <View
            key={`history-${index}`}
            style={styles.message}
            onLayout={(event) => {
              if (event.nativeEvent.layout.height <= 0) return;
              laidOut.current.add(index);
              if (laidOut.current.size === history.length)
                historyDone.current();
            }}
          >
            <DownstreamMarkdown markdown={markdown} />
          </View>
        ))}
      </ScrollView>
    </Screen>
  );
}

const styles = StyleSheet.create({
  flex: { flex: 1 },
  header: { flexShrink: 0, paddingTop: space.sm },
  meta: { marginTop: space.md },
  statusBlock: {
    paddingHorizontal: space.xl,
    paddingTop: space.xxl,
    gap: space.md,
  },
  raw: { padding: space.lg, maxHeight: 180 },
  documents: {
    paddingHorizontal: space.xl,
    paddingTop: space.lg,
  },
  message: { paddingBottom: space.xxl },
});

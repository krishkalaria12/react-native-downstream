import { memo, useCallback, useLayoutEffect, useState } from 'react';
import {
  Alert,
  FlatList,
  Platform,
  StyleSheet,
  Text,
  View,
} from 'react-native';
import { File, Paths } from 'expo-file-system';
import * as Sharing from 'expo-sharing';
import { DownstreamMarkdown } from 'react-native-downstream';

import { fixtures } from '../shared/fixtures';
import type { ScheduleKind } from '../shared/schedule';
import type { WorkloadShape, WorkloadSize } from '../shared/workload';
import type {
  CompletedMessage,
  LiveMessage,
  PlaybackStatus,
} from './usePlayback';
import { usePlayback } from './usePlayback';
import { nowMs, type RendererCandidate, type TraceRun } from './trace';
import {
  Button,
  ButtonRow,
  ChipRow,
  Divider,
  Field,
  Group,
  Input,
  type ChipOption,
} from '../ui/kit';
import { DocumentPane, RunReadout } from '../ui/readout';
import {
  Hero,
  Screen,
  SectionLabel,
  useBottomInset,
  useScreenScroll,
} from '../ui/screen';
import { space, useTheme } from '../ui/theme';
import type { Tone } from '../ui/theme';

const SEED = 20260928;
const STREAM_INDICES = [0, 1, 2, 3, 4, 5, 6, 7];

const SCHEDULES: ChipOption<ScheduleKind>[] = [
  { value: 'manifest', label: 'Exact chunks' },
  { value: 'character', label: '1 character' },
  { value: 'range', label: '1 to 20 characters' },
  { value: 'bursty', label: 'Bursty network' },
];

const RENDERERS: ChipOption<RendererCandidate>[] = [
  { value: 'downstream-full', label: 'Full parse' },
  { value: 'downstream-incremental', label: 'Incremental tail' },
];

const SHAPES: ChipOption<WorkloadShape>[] = [
  { value: 'repeat', label: 'Repeat fixture' },
  { value: 'open-fence', label: 'Open code fence' },
];

const SIZES: ChipOption<WorkloadSize>[] = [
  { value: 'fixture', label: 'Fixture' },
  { value: 10, label: '10 KiB' },
  { value: 50, label: '50 KiB' },
  { value: 100, label: '100 KiB' },
];

const CONDITIONS: ChipOption<'cold' | 'warm'>[] = [
  { value: 'warm', label: 'Warm' },
  { value: 'cold', label: 'Cold' },
];

const STREAM_COUNTS: ChipOption<1 | 4 | 8>[] = [
  { value: 1, label: '1' },
  { value: 4, label: '4' },
  { value: 8, label: '8' },
];

const STATUS_TONE: Record<PlaybackStatus, Tone> = {
  idle: 'neutral',
  running: 'live',
  paused: 'warn',
  finishing: 'warn',
  complete: 'accent',
};

const TracedMarkdown = memo(function TracedMarkdownInner({
  message,
  stream,
  trace,
}: {
  message: LiveMessage;
  stream: number;
  trace?: TraceRun;
}) {
  useLayoutEffect(() => {
    if (
      !trace ||
      message.revision === 0 ||
      !trace.shouldSampleBatch(message.batch)
    )
      return;
    const batch = message.batch;
    const sessionId = trace.data.sessionIds[stream];
    trace.mark('renderer-update', { stream, batch, sessionId });
    let observed = false;
    const frame = requestAnimationFrame(() => {
      observed = true;
      trace.mark('first-visible-frame-estimate', { stream, batch, sessionId });
      const frameAt = nowMs();
      for (const receipt of message.receipts) {
        trace.mark('arrival-to-frame-sample', {
          stream,
          batch,
          chunk: receipt.chunk,
          value: frameAt - receipt.receivedAt,
          sessionId,
        });
      }
    });
    return () => {
      cancelAnimationFrame(frame);
      if (!observed)
        trace.mark('superseded-before-frame', { stream, batch, sessionId });
    };
  }, [message.batch, message.receipts, message.revision, stream, trace]);

  return (
    <View
      onLayout={() => {
        if (message.revision > 0 && trace?.shouldSampleBatch(message.batch)) {
          trace?.mark('layout-callback', {
            stream,
            batch: message.batch,
            sessionId: trace.data.sessionIds[stream],
          });
        }
      }}
    >
      <DownstreamMarkdown stream={message.controllers[stream]!} />
    </View>
  );
});

const CompletedRow = memo(function CompletedRowInner({
  message,
}: {
  message: CompletedMessage;
}) {
  const t = useTheme();
  return (
    <View style={[t.s.card, styles.completed]}>
      <Text style={[t.s.caption, styles.completedLabel]}>
        {message.fixture} · stream {message.stream + 1}
      </Text>
      <DownstreamMarkdown markdown={message.markdown} />
    </View>
  );
});

const completedKey = (message: CompletedMessage) => message.id;

export function BenchScreen() {
  const [renderer, setRenderer] =
    useState<RendererCandidate>('downstream-full');
  const [deviceId, setDeviceId] = useState('');
  const [runCondition, setRunCondition] = useState<'cold' | 'warm'>('warm');
  const [fixtureId, setFixtureId] = useState(fixtures[0]?.id ?? '');
  const [workloadShape, setWorkloadShape] = useState<WorkloadShape>('repeat');
  const [schedule, setSchedule] = useState<ScheduleKind>('manifest');
  const [workloadSize, setWorkloadSize] = useState<WorkloadSize>('fixture');
  const [streams, setStreams] = useState<1 | 4 | 8>(1);
  const [traceEnabled, setTraceEnabled] = useState(true);
  const [exporting, setExporting] = useState(false);
  const {
    view,
    activeTrace,
    play,
    pause,
    resume,
    finish,
    reset,
    exportTrace,
    acknowledgeCommit,
  } = usePlayback();
  const scroll = useScreenScroll();
  const bottom = useBottomInset();
  const t = useTheme();
  const fixture = fixtures.find((item) => item.id === fixtureId) ?? fixtures[0];

  useLayoutEffect(() => {
    if (view.live && view.live.revision > 0) {
      acknowledgeCommit(view.run, view.live.batch);
    }
  }, [view.live, view.run, acknowledgeCommit]);

  const renderCompleted = useCallback(
    ({ item }: { item: CompletedMessage }) => <CompletedRow message={item} />,
    []
  );

  const start = () => {
    if (!fixture) return;
    play({
      fixture,
      renderer,
      deviceId,
      runCondition,
      schedule,
      seed: SEED,
      streams,
      traceEnabled,
      workloadSize,
      workloadShape,
    });
  };

  const shareTrace = async () => {
    if (exporting) return;
    setExporting(true);
    const json = exportTrace();
    try {
      if (Platform.OS === 'web') {
        console.log(json);
        Alert.alert(
          'Trace logged',
          'Raw trace JSON is in the browser console.'
        );
      } else {
        if (!(await Sharing.isAvailableAsync())) {
          throw new Error('No file sharing app is available');
        }
        const file = new File(Paths.cache, 'downstream-trace.json');
        file.write(json);
        await Sharing.shareAsync(file.uri, {
          dialogTitle: 'Export Markdown playback trace',
          mimeType: 'application/json',
        });
      }
    } catch (error) {
      Alert.alert('Trace export failed', String(error));
    } finally {
      setExporting(false);
    }
  };

  const running = view.status === 'running' || view.status === 'paused';
  const live = view.live;
  const streamCount = activeTrace?.data.streams ?? streams;
  const progress =
    view.totalChunks > 0 ? view.receivedChunks / view.totalChunks : 0;

  const header = (
    <View>
      <Hero
        title="Playback bench"
        summary="Replay one source across a fixed chunk schedule and export the trace that produced the numbers."
        meta={<Text style={[t.s.meta, styles.seed]}>Seed {SEED}</Text>}
      />

      <View style={styles.readoutBlock}>
        <RunReadout
          status={view.status}
          statusTone={STATUS_TONE[view.status]}
          meta={`Run ${view.run || '—'}`}
          progress={
            view.totalChunks > 0
              ? {
                  value: progress,
                  label: `${view.receivedChunks.toLocaleString()} / ${view.totalChunks.toLocaleString()} chunks`,
                }
              : undefined
          }
          figures={[
            {
              label: 'archived messages',
              value: String(view.completed.length),
            },
            { label: 'streams', value: String(streamCount) },
          ]}
        />
      </View>

      <View style={styles.controls}>
        <Group title="Parse mode">
          <ChipRow
            inset
            options={RENDERERS}
            value={renderer}
            onChange={setRenderer}
          />
        </Group>

        <Group title="Run metadata">
          <Field label="Device ID">
            <Input
              accessibilityLabel="Device ID"
              placeholder="Pixel 9, iPhone 16 Pro"
              value={deviceId}
              onChangeText={setDeviceId}
              autoCapitalize="none"
              autoCorrect={false}
            />
          </Field>
          <Divider />
          <ChipRow
            inset
            options={CONDITIONS}
            value={runCondition}
            onChange={setRunCondition}
          />
          <Text style={[t.s.caption, styles.groupHint]}>
            Mark cold only after restarting the app process. Set
            EXPO_PUBLIC_DOWNSTREAM_REVISION when building to record a revision.
          </Text>
        </Group>

        <Group title="Source">
          <ChipRow
            inset
            options={SHAPES}
            value={workloadShape}
            onChange={setWorkloadShape}
          />
          {workloadShape === 'repeat' && (
            <>
              <Divider />
              <ChipRow
                inset
                options={fixtures.map((item) => ({
                  value: item.id,
                  label: item.id,
                }))}
                value={fixtureId}
                onChange={setFixtureId}
              />
              {fixture !== undefined && (
                <Text style={[t.s.caption, styles.groupHint]}>
                  {fixture.description}
                </Text>
              )}
            </>
          )}
        </Group>

        <Group title="Workload size">
          <ChipRow
            inset
            options={
              workloadShape === 'repeat'
                ? SIZES
                : SIZES.filter((size) => size.value !== 'fixture')
            }
            value={workloadSize}
            onChange={setWorkloadSize}
          />
          <Text style={[t.s.caption, styles.groupHint]}>
            {workloadShape === 'open-fence'
              ? 'One code fence stays open for the entire source.'
              : 'Larger sources repeat the selected fixture to an exact byte size.'}
          </Text>
        </Group>

        <Group title="Chunk schedule">
          <ChipRow
            inset
            options={SCHEDULES.map((item) => ({
              value: item.value,
              label:
                item.value === 'manifest' && workloadSize !== 'fixture'
                  ? 'Pattern chunks'
                  : item.label,
            }))}
            value={schedule}
            onChange={setSchedule}
          />
        </Group>

        <Group title="Simultaneous streams">
          <ChipRow
            inset
            options={STREAM_COUNTS}
            value={streams}
            onChange={setStreams}
          />
        </Group>

        <Group title="Trace next run">
          <View style={styles.traceRow}>
            <Text style={[t.s.caption, styles.traceNote]}>
              Receipts, batches, renderer commits, JS frames, and frame
              estimates.
            </Text>
            <Button
              label={traceEnabled ? 'Tracing on' : 'Tracing off'}
              variant={traceEnabled ? 'primary' : 'secondary'}
              onPress={() => setTraceEnabled((current) => !current)}
              hint="Turns per-batch trace sampling on or off for the next run"
            />
          </View>
        </Group>

        <ButtonRow>
          <Button
            label={view.run > 0 ? 'Replay' : 'Play'}
            variant="primary"
            onPress={start}
          />
          <Button
            label={view.status === 'paused' ? 'Resume' : 'Pause'}
            onPress={view.status === 'paused' ? resume : pause}
            disabled={!running}
          />
          <Button label="Finish" onPress={finish} disabled={!running} />
          <Button label="Reset" onPress={reset} />
        </ButtonRow>
        <ButtonRow>
          <Button
            label={exporting ? 'Exporting' : 'Export traces'}
            onPress={shareTrace}
            disabled={exporting}
            grow
          />
        </ButtonRow>
      </View>

      {live !== null && (
        <View>
          <SectionLabel
            style={styles.section}
            trailing={<Text style={t.s.meta}>Downstream alone</Text>}
          >
            {view.status === 'complete' ? 'Completed run' : 'Live streams'}
          </SectionLabel>
          {STREAM_INDICES.slice(0, streamCount).map((stream) => (
            <DocumentPane
              key={`${view.run}-${stream}`}
              style={styles.pane}
              label={`Run ${view.run} · stream ${stream + 1}`}
              state={
                view.status === 'complete'
                  ? 'complete'
                  : view.status === 'running'
                    ? 'streaming'
                    : 'paused'
              }
              tone={
                view.status === 'running'
                  ? 'live'
                  : view.status === 'complete'
                    ? 'accent'
                    : 'warn'
              }
              pulse={view.status === 'running'}
            >
              <TracedMarkdown
                message={live}
                stream={stream}
                trace={activeTrace}
              />
            </DocumentPane>
          ))}
        </View>
      )}

      {view.completed.length > 0 && (
        <SectionLabel style={styles.section}>
          Earlier completed messages
        </SectionLabel>
      )}

      <Text style={[t.s.caption, styles.footer]}>
        The frame timestamp is a JS callback estimate. Use native profiling for
        parse, layout, CPU, memory, and UI frame timing.
      </Text>
    </View>
  );

  return (
    <Screen title="Bench" scroll={scroll}>
      <FlatList
        data={view.completed}
        keyExtractor={completedKey}
        renderItem={renderCompleted}
        onScroll={scroll.onScroll}
        scrollEventThrottle={16}
        contentContainerStyle={[styles.content, { paddingBottom: bottom }]}
        keyboardShouldPersistTaps="handled"
        ListHeaderComponent={header}
      />
    </Screen>
  );
}

const styles = StyleSheet.create({
  content: { paddingTop: space.sm },
  seed: { marginTop: space.md },
  readoutBlock: { paddingHorizontal: space.xl, paddingTop: space.xxl },
  controls: {
    paddingHorizontal: space.xl,
    paddingTop: space.xl,
    gap: space.md,
  },
  groupHint: {
    paddingHorizontal: space.lg,
    paddingTop: space.md,
    paddingBottom: space.lg,
  },
  traceRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: space.lg,
    paddingHorizontal: space.lg,
    paddingBottom: space.lg,
  },
  section: { marginTop: space.xxxl },
  pane: { marginHorizontal: space.xl, marginTop: space.md },
  completed: {
    marginHorizontal: space.xl,
    marginTop: space.md,
    padding: space.lg,
  },
  completedLabel: { fontWeight: '700', marginBottom: space.md },
  traceNote: { flex: 1 },
  footer: {
    fontSize: 12,
    lineHeight: 18,
    paddingHorizontal: space.xl,
    paddingTop: space.xxl,
  },
});

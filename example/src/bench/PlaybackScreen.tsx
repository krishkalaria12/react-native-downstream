import { memo, useCallback, useLayoutEffect, useState } from 'react';
import {
  Alert,
  FlatList,
  Platform,
  Pressable,
  StyleSheet,
  Text,
  TextInput,
  View,
} from 'react-native';
import { StatusBar } from 'expo-status-bar';
import { File, Paths } from 'expo-file-system';
import * as Sharing from 'expo-sharing';
import { DownstreamMarkdown } from 'react-native-downstream';
import { BaselineMarkdown } from './BaselineMarkdown';
import { fixtures } from '../shared/fixtures';
import type { CompletedMessage, LiveMessage } from './usePlayback';
import { usePlayback } from './usePlayback';
import type { ScheduleKind } from '../shared/schedule';
import { nowMs, type TraceRun } from './trace';
import type { RendererCandidate } from './trace';
import type { WorkloadShape, WorkloadSize } from '../shared/workload';

const SEED = 20260928;
const STREAM_INDICES = [0, 1, 2, 3, 4, 5, 6, 7];
const SCHEDULES: { id: ScheduleKind; label: string }[] = [
  { id: 'manifest', label: 'Exact chunks' },
  { id: 'character', label: '1 character' },
  { id: 'range', label: '1–20 characters' },
  { id: 'bursty', label: 'Bursty network' },
];
const RENDERERS: { id: RendererCandidate; label: string }[] = [
  { id: 'baseline', label: 'Streamdown + Enriched' },
  { id: 'downstream-full', label: 'Downstream full' },
  { id: 'downstream-incremental', label: 'Downstream incremental' },
];

function Choice<T extends string | number>({
  label,
  value,
  selected,
  onPress,
}: {
  label: string;
  value: T;
  selected: boolean;
  onPress: (value: T) => void;
}) {
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityState={{ selected }}
      onPress={() => onPress(value)}
      style={[styles.choice, selected && styles.choiceSelected]}
    >
      <Text style={[styles.choiceText, selected && styles.choiceTextSelected]}>
        {label}
      </Text>
    </Pressable>
  );
}

function Action({
  label,
  onPress,
  disabled = false,
  primary = false,
}: {
  label: string;
  onPress: () => void;
  disabled?: boolean;
  primary?: boolean;
}) {
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityState={{ disabled }}
      disabled={disabled}
      onPress={onPress}
      style={[
        styles.action,
        primary && styles.actionPrimary,
        disabled && styles.actionDisabled,
      ]}
    >
      <Text style={[styles.actionText, primary && styles.actionTextPrimary]}>
        {label}
      </Text>
    </Pressable>
  );
}

const TracedMarkdown = memo(function TracedMarkdownInner({
  message,
  stream,
  trace,
  renderer,
}: {
  message: LiveMessage;
  stream: number;
  trace?: TraceRun;
  renderer: RendererCandidate;
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
      {renderer === 'baseline' ? (
        <BaselineMarkdown markdown={message.markdown} />
      ) : (
        <DownstreamMarkdown stream={message.controllers[stream]!} />
      )}
    </View>
  );
});

const CompletedRow = memo(function CompletedRowInner({
  message,
}: {
  message: CompletedMessage;
}) {
  return (
    <View style={styles.message}>
      <Text style={styles.messageLabel}>
        {message.fixture} / stream {message.stream + 1}
      </Text>
      {message.renderer === 'baseline' ? (
        <BaselineMarkdown markdown={message.markdown} />
      ) : (
        <DownstreamMarkdown markdown={message.markdown} />
      )}
    </View>
  );
});

const completedKey = (message: CompletedMessage) => message.id;

export function PlaybackScreen() {
  const [renderer, setRenderer] = useState<RendererCandidate>('baseline');
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

  return (
    <View style={styles.screen}>
      <StatusBar style="dark" />
      <FlatList
        data={view.completed}
        keyExtractor={completedKey}
        renderItem={renderCompleted}
        contentContainerStyle={styles.content}
        keyboardShouldPersistTaps="handled"
        ListHeaderComponent={
          <View>
            <Text style={styles.kicker}>Downstream / benchmark</Text>
            <Text style={styles.title}>Markdown playback</Text>
            <Text style={styles.intro}>
              Replay one source across fixed chunk schedules. Each run uses seed{' '}
              {SEED}.
            </Text>

            <Text style={styles.sectionTitle}>Renderer</Text>
            <View style={styles.choices}>
              {RENDERERS.map((item) => (
                <Choice
                  key={item.id}
                  label={item.label}
                  value={item.id}
                  selected={renderer === item.id}
                  onPress={setRenderer}
                />
              ))}
            </View>

            <Text style={styles.sectionTitle}>Run metadata</Text>
            <TextInput
              accessibilityLabel="Device ID"
              placeholder="Device ID"
              value={deviceId}
              onChangeText={setDeviceId}
              style={styles.input}
            />
            <View style={styles.choices}>
              {(['cold', 'warm'] as const).map((condition) => (
                <Choice
                  key={condition}
                  label={condition}
                  value={condition}
                  selected={runCondition === condition}
                  onPress={setRunCondition}
                />
              ))}
            </View>
            <Text style={styles.helper}>
              Mark cold only after restarting the app process. Set
              EXPO_PUBLIC_DOWNSTREAM_REVISION when building to record a
              revision.
            </Text>

            <Text style={styles.sectionTitle}>Source</Text>
            <View style={styles.choices}>
              <Choice
                label="Fixture"
                value="repeat"
                selected={workloadShape === 'repeat'}
                onPress={() => setWorkloadShape('repeat')}
              />
              <Choice
                label="Open code fence"
                value="open-fence"
                selected={workloadShape === 'open-fence'}
                onPress={() => {
                  setWorkloadShape('open-fence');
                  if (workloadSize === 'fixture') setWorkloadSize(10);
                }}
              />
            </View>

            {workloadShape === 'repeat' && (
              <>
                <Text style={styles.sectionTitle}>Fixture</Text>
                <View style={styles.choices}>
                  {fixtures.map((item) => (
                    <Choice
                      key={item.id}
                      label={item.id}
                      value={item.id}
                      selected={item.id === fixtureId}
                      onPress={setFixtureId}
                    />
                  ))}
                </View>
                <Text style={styles.helper}>{fixture?.description}</Text>
              </>
            )}

            <Text style={styles.sectionTitle}>Workload size</Text>
            <View style={styles.choices}>
              {(workloadShape === 'repeat'
                ? (['fixture', 10, 50, 100] as const)
                : ([10, 50, 100] as const)
              ).map((size) => (
                <Choice
                  key={size}
                  label={size === 'fixture' ? 'Fixture' : `${size} KiB`}
                  value={size}
                  selected={size === workloadSize}
                  onPress={setWorkloadSize}
                />
              ))}
            </View>
            <Text style={styles.helper}>
              {workloadShape === 'open-fence'
                ? 'One code fence stays open for the entire exact-size source.'
                : 'Larger sources repeat the selected fixture to an exact byte size.'}
            </Text>

            <Text style={styles.sectionTitle}>Chunk schedule</Text>
            <View style={styles.choices}>
              {SCHEDULES.map((item) => (
                <Choice
                  key={item.id}
                  label={
                    item.id === 'manifest' && workloadSize !== 'fixture'
                      ? 'Pattern chunks'
                      : item.label
                  }
                  value={item.id}
                  selected={item.id === schedule}
                  onPress={setSchedule}
                />
              ))}
            </View>

            <Text style={styles.sectionTitle}>Simultaneous streams</Text>
            <View style={styles.choices}>
              {([1, 4, 8] as const).map((count) => (
                <Choice
                  key={count}
                  label={String(count)}
                  value={count}
                  selected={count === streams}
                  onPress={setStreams}
                />
              ))}
            </View>

            <View style={styles.traceRow}>
              <View style={styles.traceCopy}>
                <Text style={styles.sectionTitle}>Trace next run</Text>
                <Text style={styles.helper}>
                  Receipts, batches, renderer commits, JS frames, and frame
                  estimates.
                </Text>
              </View>
              <Choice
                label={traceEnabled ? 'On' : 'Off'}
                value={traceEnabled ? 'on' : 'off'}
                selected={traceEnabled}
                onPress={() => setTraceEnabled((current) => !current)}
              />
            </View>

            <View style={styles.actions}>
              <Action
                label={view.run ? 'Replay' : 'Play'}
                onPress={start}
                primary
              />
              <Action
                label={view.status === 'paused' ? 'Resume' : 'Pause'}
                onPress={view.status === 'paused' ? resume : pause}
                disabled={view.status !== 'running' && view.status !== 'paused'}
              />
              <Action
                label="Finish"
                onPress={finish}
                disabled={view.status !== 'running' && view.status !== 'paused'}
              />
              <Action label="Reset" onPress={reset} />
              <Action
                label={exporting ? 'Exporting…' : 'Export traces'}
                onPress={shareTrace}
                disabled={exporting}
              />
            </View>

            <View style={styles.readout}>
              <Text style={styles.readoutValue}>{view.status}</Text>
              <Text style={styles.readoutDetail}>
                Run {view.run || '—'} · {view.receivedChunks}/{view.totalChunks}{' '}
                chunks · {view.completed.length} archived messages
              </Text>
            </View>

            {view.live && (
              <View style={styles.messageSection}>
                <Text style={styles.sectionTitle}>
                  {view.status === 'complete'
                    ? 'Completed run'
                    : 'Live streams'}
                </Text>
                {STREAM_INDICES.slice(
                  0,
                  activeTrace?.data.streams ?? streams
                ).map((stream) => (
                  <View style={styles.message} key={`${view.run}-${stream}`}>
                    <Text style={styles.messageLabel}>
                      Run {view.run} / stream {stream + 1}
                    </Text>
                    <TracedMarkdown
                      message={view.live!}
                      stream={stream}
                      trace={activeTrace}
                      renderer={activeTrace?.data.renderer ?? renderer}
                    />
                  </View>
                ))}
              </View>
            )}

            {view.completed.length > 0 && (
              <Text style={styles.sectionTitle}>
                Earlier completed messages
              </Text>
            )}
          </View>
        }
        ListFooterComponent={
          <Text style={styles.footer}>
            The frame timestamp is a JS callback estimate. Use native profiling
            for parse, layout, CPU, memory, and UI frame timing.
          </Text>
        }
      />
    </View>
  );
}

const colors = {
  paper: '#F2F1EB',
  ink: '#172D2A',
  muted: '#52645F',
  line: '#CBD4CB',
  teal: '#14695E',
  white: '#FFFFFF',
};

const styles = StyleSheet.create({
  screen: { flex: 1, backgroundColor: colors.paper },
  content: {
    paddingTop: Platform.OS === 'ios' ? 58 : 38,
    paddingHorizontal: 20,
    paddingBottom: 60,
  },
  kicker: {
    color: colors.teal,
    fontSize: 12,
    fontWeight: '700',
    letterSpacing: 1.2,
  },
  title: { color: colors.ink, fontSize: 32, fontWeight: '800', marginTop: 10 },
  intro: {
    color: colors.muted,
    fontSize: 15,
    lineHeight: 22,
    marginTop: 7,
    marginBottom: 16,
  },
  sectionTitle: {
    color: colors.ink,
    fontSize: 16,
    fontWeight: '700',
    marginTop: 18,
    marginBottom: 9,
  },
  choices: { flexDirection: 'row', flexWrap: 'wrap', gap: 8 },
  choice: {
    borderWidth: 1,
    borderColor: colors.line,
    borderRadius: 8,
    paddingVertical: 8,
    paddingHorizontal: 11,
    backgroundColor: colors.white,
  },
  choiceSelected: { backgroundColor: colors.ink, borderColor: colors.ink },
  choiceText: { color: colors.ink, fontSize: 13, fontWeight: '600' },
  choiceTextSelected: { color: colors.white },
  helper: { color: colors.muted, fontSize: 13, lineHeight: 18, marginTop: 7 },
  input: {
    borderWidth: 1,
    borderColor: colors.line,
    borderRadius: 8,
    backgroundColor: colors.white,
    color: colors.ink,
    padding: 10,
  },
  traceRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 14,
    marginTop: 7,
  },
  traceCopy: { flex: 1 },
  actions: { flexDirection: 'row', flexWrap: 'wrap', gap: 8, marginTop: 26 },
  action: {
    borderWidth: 1,
    borderColor: colors.ink,
    borderRadius: 8,
    paddingVertical: 10,
    paddingHorizontal: 13,
  },
  actionPrimary: { backgroundColor: colors.teal, borderColor: colors.teal },
  actionDisabled: { opacity: 0.35 },
  actionText: { color: colors.ink, fontSize: 14, fontWeight: '700' },
  actionTextPrimary: { color: colors.white },
  readout: {
    marginTop: 23,
    borderTopWidth: 1,
    borderBottomWidth: 1,
    borderColor: colors.line,
    paddingVertical: 14,
  },
  readoutValue: { color: colors.teal, fontSize: 20, fontWeight: '700' },
  readoutDetail: { color: colors.muted, fontSize: 13, marginTop: 3 },
  messageSection: { marginTop: 15 },
  message: {
    backgroundColor: colors.white,
    borderRadius: 10,
    borderWidth: 1,
    borderColor: colors.line,
    padding: 16,
    marginTop: 10,
  },
  messageLabel: {
    color: colors.teal,
    fontSize: 12,
    fontWeight: '700',
    marginBottom: 12,
  },
  footer: { color: colors.muted, fontSize: 12, lineHeight: 18, marginTop: 30 },
});

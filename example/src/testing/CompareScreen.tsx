import { useState } from 'react';
import {
  Platform,
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  View,
} from 'react-native';
import { DownstreamMarkdown } from 'react-native-downstream';
import type { MarkdownStreamController } from 'react-native-downstream';

import { fixtures } from '../shared/fixtures';
import type { ScheduleKind } from '../shared/schedule';
import type { WorkloadShape, WorkloadSize } from '../shared/workload';
import {
  buildComparison,
  formatValue,
  type ComparisonCandidate,
} from './comparison';
import {
  CANDIDATE_LABELS,
  CANDIDATES,
  type ComparisonRun,
  type ParseMode,
} from './comparisonRunner';
import { StreamdownMarkdown } from './StreamdownMarkdown';
import { useComparison } from './useComparison';

const SEED = 20260928;
const SCHEDULES: { id: ScheduleKind; label: string }[] = [
  { id: 'manifest', label: 'Exact chunks' },
  { id: 'character', label: '1 character' },
  { id: 'range', label: '1–20 characters' },
  { id: 'bursty', label: 'Bursty network' },
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

function Pane({
  candidate,
  run,
  liveSource,
  liveStream,
  running,
  error,
}: {
  candidate: ComparisonCandidate;
  run: ComparisonRun | null;
  liveSource: string;
  liveStream: MarkdownStreamController | null;
  running: boolean;
  error: string | null;
}) {
  const source = run ? run.metrics.source : liveSource;
  const stream = run ? run.stream : liveStream;
  return (
    <View style={styles.pane}>
      <View style={styles.paneHeader}>
        <Text style={styles.paneLabel}>{CANDIDATE_LABELS[candidate]}</Text>
        <Text style={styles.paneState}>
          {run
            ? error
              ? 'failed'
              : `${run.metrics.totalMs.toFixed(1)} ms`
            : running
            ? 'running'
            : 'waiting'}
        </Text>
      </View>
      {candidate === 'streamdown' ? (
        <StreamdownMarkdown markdown={source} />
      ) : stream ? (
        <DownstreamMarkdown stream={stream} />
      ) : (
        // An empty view keeps the Downstream pane mounted, and in the same
        // place in the tree, while Streamdown is the candidate under test.
        <DownstreamMarkdown markdown="" />
      )}
      {error && <Text style={styles.paneError}>{error}</Text>}
    </View>
  );
}

export function CompareScreen() {
  const { view, run, reset } = useComparison();
  const [fixtureId, setFixtureId] = useState(fixtures[0]?.id ?? '');
  const [workloadShape, setWorkloadShape] = useState<WorkloadShape>('repeat');
  const [workloadSize, setWorkloadSize] = useState<WorkloadSize>('fixture');
  const [schedule, setSchedule] = useState<ScheduleKind>('manifest');
  const [parseMode, setParseMode] = useState<ParseMode>('full');
  const fixture = fixtures.find((item) => item.id === fixtureId) ?? fixtures[0];
  const [streamdownRun, downstreamRun] = [
    view.runs[0] ?? null,
    view.runs[1] ?? null,
  ];
  const bothDone = streamdownRun !== null && downstreamRun !== null;
  const anyFailed = !!(streamdownRun?.error || downstreamRun?.error);
  const comparison =
    bothDone && !anyFailed && streamdownRun && downstreamRun
      ? buildComparison(downstreamRun.metrics, streamdownRun.metrics)
      : null;
  const running = view.status === 'running';

  return (
    <ScrollView contentContainerStyle={styles.content}>
      <Text style={styles.kicker}>Downstream / testing</Text>
      <Text style={styles.title}>Streamdown compared with Downstream</Text>
      <Text style={styles.intro}>
        One source, one seeded chunk schedule, two renderers. Streamdown runs
        first, Downstream second, so the second run controls for whatever the
        first left on screen.
      </Text>

      <Text style={styles.sectionTitle}>Workload</Text>
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
          onPress={() => setWorkloadShape('open-fence')}
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

      <Text style={styles.sectionTitle}>Chunk schedule</Text>
      <View style={styles.choices}>
        {SCHEDULES.map((item) => (
          <Choice
            key={item.id}
            label={item.label}
            value={item.id}
            selected={item.id === schedule}
            onPress={setSchedule}
          />
        ))}
      </View>

      <Text style={styles.sectionTitle}>Downstream parse mode</Text>
      <View style={styles.choices}>
        {(['full', 'incremental'] as const).map((mode) => (
          <Choice
            key={mode}
            label={mode === 'full' ? 'Full' : 'Incremental'}
            value={mode}
            selected={mode === parseMode}
            onPress={setParseMode}
          />
        ))}
      </View>
      <Text style={styles.helper}>
        Each run uses seed {SEED} and one stream.
      </Text>

      <View style={styles.actions}>
        <Action
          label={running ? 'Running' : 'Run comparison'}
          onPress={() =>
            fixture &&
            run({
              fixture,
              workloadShape,
              workloadSize:
                workloadShape === 'open-fence' && workloadSize === 'fixture'
                  ? 10
                  : workloadSize,
              schedule,
              seed: SEED,
              parseMode,
            })
          }
          disabled={running || !fixture}
          primary
        />
        <Action label="Reset" onPress={reset} />
      </View>

      {view.error && <Text style={styles.error}>{view.error}</Text>}

      <Text style={styles.sectionTitle}>Both renderers</Text>
      {CANDIDATES.map((candidate, index) => {
        const finished = view.runs[index] ?? null;
        return (
          <Pane
            key={candidate}
            candidate={candidate}
            run={finished}
            liveSource={view.active === index ? view.liveSource : ''}
            liveStream={view.active === index ? view.liveStream : null}
            running={view.active === index}
            error={finished?.error ?? null}
          />
        );
      })}

      {comparison && (
        <>
          <Text style={styles.sectionTitle}>Comparison</Text>
          <View style={styles.table}>
            <View style={[styles.row, styles.rowHead]}>
              <Text style={[styles.cell, styles.cellHead, styles.cellName]}>
                Metric
              </Text>
              <Text style={[styles.cell, styles.cellHead]}>Downstream</Text>
              <Text style={[styles.cell, styles.cellHead]}>Streamdown</Text>
            </View>
            {comparison.rows.map((row) => (
              <View
                key={row.id}
                style={[
                  styles.row,
                  row.faster === 'downstream' && styles.rowWin,
                ]}
              >
                <Text style={[styles.cell, styles.cellName]}>{row.label}</Text>
                <Text style={[styles.cell, styles.cellValue]}>
                  {formatValue(row.downstream, row.unit)}
                </Text>
                <Text style={[styles.cell, styles.cellValue]}>
                  {formatValue(row.streamdown, row.unit)}
                </Text>
              </View>
            ))}
          </View>

          <View style={styles.verdict}>
            <Text style={styles.verdictTitle}>
              {comparison.verdict === 'downstream'
                ? 'Downstream reached the final frame first'
                : comparison.verdict === 'streamdown'
                ? 'Streamdown reached the final frame first'
                : comparison.verdict === 'tie'
                ? 'Both reached the final frame together'
                : 'The run did not finish'}
            </Text>
            <Text style={styles.verdictDetail}>
              {comparison.parity.match
                ? 'Both renderers received the same source.'
                : `Sources differ${
                    comparison.parity.firstDifference === null
                      ? ''
                      : ` at character ${comparison.parity.firstDifference}`
                  }.`}
            </Text>
          </View>
        </>
      )}

      {!comparison && running && (
        <Text style={styles.helper}>
          The comparison appears once both renderers have run.
        </Text>
      )}

      <Text style={styles.footer}>
        Timings come from JavaScript animation frame callbacks and end two
        frames after the last update. They bound JS-thread work; they are not
        proof that a glyph reached the display. Run at least two pairs per
        device and report the spread, not the best pair.
      </Text>
    </ScrollView>
  );
}

const colors = {
  paper: '#F2F1EB',
  ink: '#172D2A',
  muted: '#52645F',
  line: '#CBD4CB',
  teal: '#14695E',
  white: '#FFFFFF',
  win: '#E4EFE9',
};

const styles = StyleSheet.create({
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
  error: { color: '#9B2C2C', fontSize: 13, marginTop: 12 },
  pane: {
    backgroundColor: colors.white,
    borderRadius: 10,
    borderWidth: 1,
    borderColor: colors.line,
    padding: 16,
    marginTop: 10,
  },
  paneHeader: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    marginBottom: 12,
  },
  paneLabel: { color: colors.teal, fontSize: 12, fontWeight: '700' },
  paneState: { color: colors.muted, fontSize: 12, fontWeight: '600' },
  paneError: { color: '#9B2C2C', fontSize: 12, marginTop: 10 },
  table: {
    borderWidth: 1,
    borderColor: colors.line,
    borderRadius: 10,
    overflow: 'hidden',
  },
  row: {
    flexDirection: 'row',
    alignItems: 'center',
    borderTopWidth: StyleSheet.hairlineWidth,
    borderTopColor: colors.line,
    paddingVertical: 9,
    paddingHorizontal: 12,
  },
  rowHead: { borderTopWidth: 0, backgroundColor: colors.paper },
  rowWin: { backgroundColor: colors.win },
  cell: { flex: 1, color: colors.ink, fontSize: 13 },
  cellHead: { fontWeight: '700' },
  cellName: { flex: 2 },
  cellValue: { fontVariant: ['tabular-nums'], textAlign: 'right' },
  verdict: { marginTop: 14 },
  verdictTitle: { color: colors.ink, fontSize: 15, fontWeight: '700' },
  verdictDetail: { color: colors.muted, fontSize: 13, marginTop: 4 },
  footer: { color: colors.muted, fontSize: 12, lineHeight: 18, marginTop: 30 },
});

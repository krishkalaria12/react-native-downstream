import { useMemo, useState } from 'react';
import { ScrollView, StyleSheet, Text, View } from 'react-native';
import { DownstreamMarkdown } from 'react-native-downstream';
import type { MarkdownStreamController } from 'react-native-downstream';

import { fixtures } from '../shared/fixtures';
import type { ScheduleKind } from '../shared/schedule';
import type { WorkloadShape, WorkloadSize } from '../shared/workload';
import {
  buildComparison,
  formatValue,
  type Comparison,
  type ComparisonCandidate,
} from './comparison';
import {
  CANDIDATE_LABELS,
  CANDIDATES,
  type ComparisonRun,
  type ComparisonStatus,
  type ParseMode,
} from './comparisonRunner';
import { StreamdownMarkdown } from './StreamdownMarkdown';
import { useComparison } from './useComparison';
import {
  Button,
  ButtonRow,
  ChipRow,
  Divider,
  Group,
  Notice,
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

const SHAPES: ChipOption<WorkloadShape>[] = [
  { value: 'repeat', label: 'Repeat fixture' },
  { value: 'open-fence', label: 'Open code fence' },
];

const SCHEDULES: ChipOption<ScheduleKind>[] = [
  { value: 'manifest', label: 'Exact chunks' },
  { value: 'character', label: '1 character' },
  { value: 'range', label: '1 to 20 characters' },
  { value: 'bursty', label: 'Bursty network' },
];

const PARSE_MODES: ChipOption<ParseMode>[] = [
  { value: 'full', label: 'Full parse' },
  { value: 'incremental', label: 'Incremental tail' },
];

const HIGHLIGHTING: ChipOption<'on' | 'off'>[] = [
  { value: 'on', label: 'On' },
  { value: 'off', label: 'Off' },
];

/**
 * Both renderers animate, or neither does, so a comparison is of the renderers.
 *
 * The two features are not the same, and the labels say so. The native peer
 * available here is `react-native-streamdown@0.2.0`, whose only animation prop is
 * enriched-markdown's `streamingAnimation`: a whole-tail fade with no caret.
 * Downstream's is a per-word cascade on one shared timeline, with a caret. Turning
 * both on is how the difference is visible, which is the point of the toggle.
 */
const ANIMATION: ChipOption<'on' | 'off'>[] = [
  { value: 'off', label: 'Off' },
  { value: 'on', label: 'On' },
];

const SIZES: ChipOption<WorkloadSize>[] = [
  { value: 'fixture', label: 'Fixture' },
  { value: 10, label: '10 KiB' },
  { value: 50, label: '50 KiB' },
  { value: 100, label: '100 KiB' },
];

const STATUS_TONE: Record<ComparisonStatus, Tone> = {
  idle: 'neutral',
  running: 'live',
  complete: 'accent',
  failed: 'danger',
};

const VERDICT: Record<Comparison['verdict'], { title: string; tone: Tone }> = {
  downstream: {
    title: 'Downstream reached the final frame first',
    tone: 'win',
  },
  streamdown: {
    title: 'Streamdown reached the final frame first',
    tone: 'neutral',
  },
  tie: { title: 'Both reached the final frame together', tone: 'neutral' },
  unavailable: { title: 'The run did not finish', tone: 'danger' },
};

function Pane({
  candidate,
  run,
  liveSource,
  liveStream,
  running,
  syntaxHighlighting,
  animated,
}: {
  candidate: ComparisonCandidate;
  run: ComparisonRun | null;
  liveSource: string;
  liveStream: MarkdownStreamController | null;
  running: boolean;
  syntaxHighlighting: boolean;
  /** Both panes animate together, so a comparison is of the renderers. */
  animated: boolean;
}) {
  const t = useTheme();
  const source = run ? run.metrics.source : liveSource;
  const stream = run ? run.stream : liveStream;
  const state = run
    ? run.error
      ? 'failed'
      : `${run.metrics.totalMs.toFixed(1)} ms`
    : running
    ? 'running'
    : 'waiting';
  return (
    <DocumentPane
      label={CANDIDATE_LABELS[candidate]}
      state={state}
      tone={run?.error ? 'danger' : running ? 'live' : run ? 'win' : 'neutral'}
      pulse={running}
      style={styles.pane}
    >
      {candidate === 'streamdown' ? (
        // The peer's whole-tail fade, which is the whole of its animation support.
        <StreamdownMarkdown markdown={source} streamingAnimation={animated} />
      ) : stream ? (
        <DownstreamMarkdown
          stream={stream}
          syntaxHighlighting={syntaxHighlighting}
          animated={animated}
          caret={animated ? 'block' : undefined}
        />
      ) : (
        // An empty view keeps the Downstream pane mounted, and in the same
        // place in the tree, while Streamdown is the candidate under test.
        <DownstreamMarkdown markdown="" animated={animated} />
      )}
      {run?.error != null && (
        <Text style={[t.s.caption, styles.paneError, { color: t.c.danger }]}>
          {run.error}
        </Text>
      )}
    </DocumentPane>
  );
}

/** One candidate's time to the final frame, or why it has none. */
function candidateFigure(label: string, run: ComparisonRun | null) {
  if (run === null) return { label, value: 'waiting' };
  if (run.error !== null) return { label, value: 'failed' };
  return {
    label,
    value: run.metrics.totalMs.toFixed(1),
    unit: 'ms',
    tone: label === 'downstream' ? ('accent' as const) : undefined,
  };
}

function ComparisonTable({ comparison }: { comparison: Comparison }) {
  const t = useTheme();
  return (
    <View style={[t.s.well, styles.table]}>
      <View style={styles.row}>
        <Text style={[t.s.caption, styles.cellName, styles.headerCell]}>
          Metric
        </Text>
        <Text style={[t.s.caption, styles.cellValue, styles.headerCell]}>
          Downstream
        </Text>
        <Text style={[t.s.caption, styles.cellValue, styles.headerCell]}>
          Streamdown
        </Text>
      </View>
      <Divider inset={false} />
      {comparison.rows.map((row) => (
        <View key={row.id}>
          <Divider inset={false} />
          <View
            style={[
              styles.row,
              row.faster === 'downstream' && { backgroundColor: t.c.win },
            ]}
          >
            <Text style={[t.s.body, styles.cellName, styles.cellLabel]}>
              {row.label}
            </Text>
            <Text
              style={[
                t.s.meta,
                styles.cellValue,
                row.faster === 'downstream' && { color: t.c.accent },
              ]}
            >
              {formatValue(row.downstream, row.unit)}
            </Text>
            <Text
              style={[
                t.s.meta,
                styles.cellValue,
                row.faster === 'streamdown' && { color: t.c.inkSoft },
              ]}
            >
              {formatValue(row.streamdown, row.unit)}
            </Text>
          </View>
        </View>
      ))}
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
  const [highlighting, setHighlighting] = useState<'on' | 'off'>('on');
  const [animation, setAnimation] = useState<'on' | 'off'>('on');
  const scroll = useScreenScroll();
  const bottom = useBottomInset();
  const t = useTheme();

  const fixture = useMemo(
    () => fixtures.find((item) => item.id === fixtureId) ?? fixtures[0],
    [fixtureId]
  );
  const [streamdownRun, downstreamRun] = [
    view.runs[0] ?? null,
    view.runs[1] ?? null,
  ];
  const anyFailed = !!(streamdownRun?.error || downstreamRun?.error);
  const comparison =
    streamdownRun && downstreamRun && !anyFailed
      ? buildComparison(downstreamRun.metrics, streamdownRun.metrics)
      : null;
  const running = view.status === 'running';
  const verdict = comparison ? VERDICT[comparison.verdict] : null;

  const start = () => {
    if (!fixture) return;
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
    });
  };

  return (
    <Screen title="Testing" scroll={scroll}>
      <ScrollView
        onScroll={scroll.onScroll}
        scrollEventThrottle={16}
        contentContainerStyle={[styles.content, { paddingBottom: bottom }]}
        keyboardShouldPersistTaps="handled"
      >
        <Hero
          title="Streamdown vs Downstream"
          summary="One source, one seeded chunk schedule, two renderers. Streamdown runs first, so the second run is a control for whatever the first left on screen."
          meta={<Text style={[t.s.meta, styles.seed]}>Seed {SEED}</Text>}
        />

        <View style={styles.controls}>
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

          <Group title="Size">
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
          </Group>

          <Group title="Chunk schedule">
            <ChipRow
              inset
              options={SCHEDULES}
              value={schedule}
              onChange={setSchedule}
            />
          </Group>

          <Group title="Downstream parse mode">
            <ChipRow
              inset
              options={PARSE_MODES}
              value={parseMode}
              onChange={setParseMode}
            />
          </Group>

          <Group title="Downstream code colors">
            <ChipRow
              inset
              options={HIGHLIGHTING}
              value={highlighting}
              onChange={setHighlighting}
            />
          </Group>

          <Group title="Streaming animation, both panes">
            <Text style={[t.s.caption, styles.groupNote]}>
              The peer fades its whole tail and has no caret; Downstream fades
              per word and draws one. Turn both on to see the difference.
            </Text>
            <ChipRow
              inset
              options={ANIMATION}
              value={animation}
              onChange={setAnimation}
            />
          </Group>

          <ButtonRow>
            <Button
              label={running ? 'Running' : 'Run comparison'}
              variant="primary"
              onPress={start}
              disabled={running || fixture === undefined}
            />
            <Button label="Reset" onPress={reset} />
          </ButtonRow>
        </View>

        {view.error !== null && (
          <Notice
            tone="danger"
            title="The run stopped early"
            style={styles.notice}
          >
            {view.error}
          </Notice>
        )}

        <View style={styles.results}>
          <RunReadout
            status={view.status}
            statusTone={STATUS_TONE[view.status]}
            meta={
              running
                ? `${
                    CANDIDATE_LABELS[CANDIDATES[view.active] ?? 'downstream']
                  } is running`
                : view.status === 'idle'
                ? 'Streamdown first, Downstream second'
                : undefined
            }
            figures={
              streamdownRun === null && downstreamRun === null
                ? undefined
                : [
                    candidateFigure('streamdown', streamdownRun),
                    candidateFigure('downstream', downstreamRun),
                  ]
            }
          />

          <SectionLabel
            style={styles.section}
            trailing={
              running ? (
                <Text style={t.s.meta}>one source, two passes</Text>
              ) : null
            }
          >
            Both renderers
          </SectionLabel>

          {CANDIDATES.map((candidate, index) => (
            <Pane
              key={candidate}
              candidate={candidate}
              run={view.runs[index] ?? null}
              liveSource={view.active === index ? view.liveSource : ''}
              liveStream={view.active === index ? view.liveStream : null}
              running={view.active === index}
              syntaxHighlighting={highlighting === 'on'}
              animated={animation === 'on'}
            />
          ))}

          {comparison !== null && verdict !== null && (
            <>
              <SectionLabel style={styles.section}>Comparison</SectionLabel>
              <View style={styles.tableBlock}>
                <ComparisonTable comparison={comparison} />
              </View>
              <Notice
                tone={verdict.tone}
                title={verdict.title}
                style={styles.notice}
              >
                {comparison.parity.match
                  ? 'Both renderers received the same source.'
                  : `Sources differ${
                      comparison.parity.firstDifference === null
                        ? ''
                        : ` at character ${comparison.parity.firstDifference}`
                    }.`}
              </Notice>
            </>
          )}

          {!running && comparison === null && (
            <Notice
              tone="neutral"
              title="No comparison yet"
              style={styles.notice}
            >
              Run a pair to see both renderers side by side. The verdict appears
              once both passes finish.
            </Notice>
          )}

          <Text style={[t.s.caption, styles.footer]}>
            Timings come from JavaScript animation frame callbacks and end two
            frames after the last update. They bound JS-thread work; they are
            not proof that a glyph reached the display. Run at least two pairs
            per device and report the spread, not the best pair.
          </Text>
        </View>
      </ScrollView>
    </Screen>
  );
}

const styles = StyleSheet.create({
  groupNote: {
    paddingHorizontal: space.lg,
    paddingBottom: space.sm,
    opacity: 0.7,
  },
  content: { paddingTop: space.sm },
  seed: { marginTop: space.md },
  controls: {
    paddingHorizontal: space.xl,
    paddingTop: space.xxl,
    gap: space.md,
  },
  groupHint: {
    paddingHorizontal: space.lg,
    paddingTop: space.md,
    paddingBottom: space.lg,
  },
  notice: { marginHorizontal: space.xl, marginTop: space.lg },
  results: { paddingTop: space.lg },
  section: { marginTop: space.xxl },
  tableBlock: { paddingHorizontal: space.xl, paddingTop: space.md },
  pane: { marginHorizontal: space.xl, marginTop: space.md },
  paneError: { marginTop: space.sm },
  table: { overflow: 'hidden' },
  row: {
    flexDirection: 'row',
    alignItems: 'center',
    paddingVertical: space.md,
    paddingHorizontal: space.lg,
  },
  cellName: { flex: 2, paddingRight: space.sm },
  cellValue: { flex: 1, textAlign: 'right', paddingLeft: space.sm },
  cellLabel: { fontSize: 14, lineHeight: 19 },
  headerCell: { fontSize: 11, letterSpacing: 0.2 },
  footer: {
    fontSize: 12,
    lineHeight: 18,
    paddingHorizontal: space.xl,
    paddingTop: space.xxl,
  },
});

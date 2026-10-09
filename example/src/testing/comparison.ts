/** Metrics collected for one candidate while it replays the shared source. */
export interface CandidateMetrics {
  /** Milliseconds from the first chunk to the next JS animation callback. */
  firstFrameMs: number | null;
  /** Scheduled run duration, including delivery and candidate-specific drain. */
  totalMs: number;
  /** Time spent delivering the schedule, measured from run start. */
  deliveryMs: number;
  /** Post-delivery drain to the completion callback, not native presentation. */
  drainMs: number;
  /** Animation frames observed between the first chunk and the final frame. */
  frames: number;
  /** Frame gaps longer than the 60 Hz budget of about 20 ms. */
  longFrames: number;
  /** Longest single frame gap, in milliseconds. */
  longestFrameMs: number;
  /** Worst distance between a scheduled batch and its actual delivery. */
  maxLatenessMs: number;
  /** Chunks handed to the renderer. */
  chunks: number;
  /** Final source the renderer was asked to display. */
  source: string;
}

export type ComparisonCandidate = 'streamdown' | 'downstream';

export type Faster = 'downstream' | 'streamdown' | 'tie' | 'unavailable';

export interface MetricRow {
  id: string;
  label: string;
  /** Rendered unit suffix, empty for counts. */
  unit: 'ms' | '';
  downstream: number | null;
  streamdown: number | null;
  /** Downstream minus Streamdown. Negative means Downstream spent less. */
  delta: number | null;
  faster: Faster;
}

export interface TextParity {
  match: boolean;
  /** Length of the shorter of the two sources. */
  sharedPrefix: number;
  /** Offset of the first character the two sources disagree on. */
  firstDifference: number | null;
}

export type ComparisonVerdict =
  | 'downstream'
  | 'streamdown'
  | 'tie'
  | 'unavailable';

export interface Comparison {
  rows: MetricRow[];
  parity: TextParity;
  verdict: ComparisonVerdict;
}

const METRICS: {
  id: string;
  label: string;
  unit: 'ms' | '';
  read: (metrics: CandidateMetrics) => number | null;
  /** A lower value is better. Counts are reported without a verdict. */
  lowerIsBetter: boolean;
}[] = [
  {
    id: 'totalMs',
    label: 'Scheduled run duration',
    unit: 'ms',
    read: (metrics) => metrics.totalMs,
    lowerIsBetter: false,
  },
  {
    id: 'deliveryMs',
    label: 'Schedule delivery duration',
    unit: 'ms',
    read: (metrics) => metrics.deliveryMs,
    lowerIsBetter: false,
  },
  {
    id: 'drainMs',
    label: 'Post-delivery drain estimate',
    unit: 'ms',
    read: (metrics) => metrics.drainMs,
    lowerIsBetter: false,
  },
  {
    id: 'firstFrameMs',
    label: 'First JS callback after arrival',
    unit: 'ms',
    read: (metrics) => metrics.firstFrameMs,
    lowerIsBetter: true,
  },
  {
    id: 'longestFrameMs',
    label: 'Longest JS callback gap',
    unit: 'ms',
    read: (metrics) => metrics.longestFrameMs,
    lowerIsBetter: true,
  },
  {
    id: 'maxLatenessMs',
    label: 'Worst delivery delay',
    unit: 'ms',
    read: (metrics) => metrics.maxLatenessMs,
    lowerIsBetter: true,
  },
  {
    id: 'longFrames',
    label: 'JS callback gaps over 20 ms',
    unit: '',
    read: (metrics) => metrics.longFrames,
    lowerIsBetter: false,
  },
  {
    id: 'frames',
    label: 'JS callback intervals',
    unit: '',
    read: (metrics) => metrics.frames,
    lowerIsBetter: false,
  },
];

/** Frame gaps below this are within the 60 Hz budget of about 16.7 ms. */
export const LONG_FRAME_MS = 20;

function verdictFor(
  downstream: number | null,
  streamdown: number | null,
  lowerIsBetter: boolean
): Faster {
  if (downstream === null || streamdown === null) return 'unavailable';
  if (downstream === streamdown) return 'tie';
  const downstreamWins = lowerIsBetter
    ? downstream < streamdown
    : downstream > streamdown;
  return downstreamWins ? 'downstream' : 'streamdown';
}

function round(value: number | null): number | null {
  return value === null ? null : Math.round(value * 1000) / 1000;
}

export function buildRows(
  downstream: CandidateMetrics,
  streamdown: CandidateMetrics
): MetricRow[] {
  return METRICS.map((metric) => {
    const ours = round(metric.read(downstream));
    const theirs = round(metric.read(streamdown));
    const delta =
      ours === null || theirs === null ? null : round(ours - theirs);
    return {
      id: metric.id,
      label: metric.label,
      unit: metric.unit,
      downstream: ours,
      streamdown: theirs,
      delta,
      faster: metric.lowerIsBetter
        ? verdictFor(ours, theirs, true)
        : delta === null
          ? 'unavailable'
          : 'tie',
    };
  });
}

/** Compares two final sources and reports where they first disagree. */
export function compareText(ours: string, theirs: string): TextParity {
  const shared = Math.min(ours.length, theirs.length);
  for (let index = 0; index < shared; index += 1) {
    if (ours[index] !== theirs[index]) {
      return { match: false, sharedPrefix: index, firstDifference: index };
    }
  }
  if (ours.length === theirs.length) {
    return { match: true, sharedPrefix: shared, firstDifference: null };
  }
  return { match: false, sharedPrefix: shared, firstDifference: shared };
}

export function buildComparison(
  downstream: CandidateMetrics,
  streamdown: CandidateMetrics
): Comparison {
  return {
    rows: buildRows(downstream, streamdown),
    parity: compareText(downstream.source, streamdown.source),
    // The candidates do not expose equivalent native completion fences.
    // Callback responsiveness and input equality cannot establish a renderer
    // presentation winner.
    verdict: 'unavailable',
  };
}

export function formatValue(value: number | null, unit: 'ms' | ''): string {
  if (value === null) return '—';
  return unit === 'ms' ? `${value.toFixed(1)} ms` : String(value);
}

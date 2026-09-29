import type { MarkdownStreamController } from 'react-native-downstream';

import {
  createMarkdownStreamWithMode,
  getStreamState,
} from '../../../src/createMarkdownStream';
import type { Fixture } from '../shared/fixtures';
import {
  afterTwoAnimationFrames,
  allFinalSnapshotsReady,
} from '../shared/sessionLifecycle';
import {
  createSchedule,
  nextScheduledBatch,
  type PlaybackSchedule,
  type ScheduleKind,
} from '../shared/schedule';
import {
  createWorkload,
  type WorkloadShape,
  type WorkloadSize,
} from '../shared/workload';
import {
  LONG_FRAME_MS,
  type CandidateMetrics,
  type ComparisonCandidate,
} from './comparison';

/** Streamdown runs first so Downstream never inherits a warm renderer. */
export const CANDIDATES: readonly ComparisonCandidate[] = [
  'streamdown',
  'downstream',
];

export const CANDIDATE_LABELS: Record<ComparisonCandidate, string> = {
  streamdown: 'Streamdown RN',
  downstream: 'Downstream',
};

export type ParseMode = 'full' | 'incremental';

export interface ComparisonConfig {
  fixture: Fixture;
  workloadSize: WorkloadSize;
  workloadShape: WorkloadShape;
  schedule: ScheduleKind;
  seed: number;
  parseMode: ParseMode;
}

export interface ComparisonRun {
  candidate: ComparisonCandidate;
  /** Session backing the Downstream pane; null for Streamdown. */
  stream: MarkdownStreamController | null;
  metrics: CandidateMetrics;
  /** Set when the candidate threw, so the comparison reports it as missing. */
  error: string | null;
}

export type ComparisonStatus = 'idle' | 'running' | 'complete' | 'failed';

export interface ComparisonView {
  status: ComparisonStatus;
  /**
   * One slot per candidate, always in the same order, so both panes sit in the
   * same place in the view tree no matter which candidate is running.
   */
  runs: (ComparisonRun | null)[];
  /** Source handed to the candidate that is running now. */
  liveSource: string;
  /** Session driving the running Downstream pane; null between runs. */
  liveStream: MarkdownStreamController | null;
  /** Index of the running candidate, or -1 between runs. */
  active: number;
  error: string | null;
}

const EMPTY_METRICS: CandidateMetrics = {
  firstFrameMs: null,
  totalMs: 0,
  frames: 0,
  longFrames: 0,
  longestFrameMs: 0,
  maxLatenessMs: 0,
  chunks: 0,
  source: '',
};

const IDLE_VIEW: ComparisonView = {
  status: 'idle',
  runs: [null, null],
  liveSource: '',
  liveStream: null,
  active: -1,
  error: null,
};

function nowMs(): number {
  return (
    (
      globalThis as { performance?: { now: () => number } }
    ).performance?.now() ?? Date.now()
  );
}

function describe(cause: unknown): string {
  if (cause instanceof Error) return cause.message;
  return String(cause);
}

interface ActiveCandidate {
  index: number;
  candidate: ComparisonCandidate;
  controller: MarkdownStreamController | null;
  unsubscribe: (() => void) | null;
  schedule: PlaybackSchedule;
  cursor: number;
  batchIndex: number;
  source: string;
  startedAt: number;
  firstFrameMs: number | null;
  lastFrameAt: number | null;
  frames: number;
  longFrames: number;
  longestFrameMs: number;
  maxLatenessMs: number;
  frameHandle: number | null;
  timer: ReturnType<typeof setTimeout> | null;
  completionRequested: boolean;
  cancelCompletion: (() => void) | null;
}

/**
 * Replays one deterministic schedule through Streamdown RN and then Downstream,
 * so the second run is a control for the first. Only the renderer changes
 * between the two runs; the source, chunk boundaries, and due times do not.
 */
export class ComparisonRunner {
  private active: ActiveCandidate | null = null;
  private readonly controllers = new Set<MarkdownStreamController>();
  private parseMode: ParseMode = 'full';
  private view: ComparisonView = IDLE_VIEW;
  private notify: (() => void) | null = null;

  subscribe(notify: () => void): void {
    this.notify = notify;
  }

  getView(): ComparisonView {
    return this.view;
  }

  run(config: ComparisonConfig): void {
    this.cancelActive();
    this.disposeControllers();
    this.parseMode = config.parseMode;
    this.view = {
      status: 'running',
      runs: [null, null],
      liveSource: '',
      liveStream: null,
      active: 0,
      error: null,
    };
    this.publish();
    this.startCandidate(
      0,
      createSchedule(
        config.schedule,
        config.seed,
        createWorkload(
          config.fixture,
          config.workloadSize,
          config.workloadShape
        )
      )
    );
  }

  reset(): void {
    this.cancelActive();
    this.disposeControllers();
    this.view = IDLE_VIEW;
    this.publish();
  }

  dispose(): void {
    this.cancelActive();
    this.disposeControllers();
    this.notify = null;
  }

  private startCandidate(index: number, schedule: PlaybackSchedule): void {
    const candidate: ComparisonCandidate = CANDIDATES[index]!;
    const controller =
      candidate === 'downstream'
        ? createMarkdownStreamWithMode({}, this.parseMode)
        : null;
    if (controller) this.controllers.add(controller);
    const active: ActiveCandidate = {
      index,
      candidate,
      controller,
      unsubscribe: null,
      schedule,
      cursor: 0,
      batchIndex: 0,
      source: '',
      startedAt: nowMs(),
      firstFrameMs: null,
      lastFrameAt: null,
      frames: 0,
      longFrames: 0,
      longestFrameMs: 0,
      maxLatenessMs: 0,
      frameHandle: null,
      timer: null,
      completionRequested: false,
      cancelCompletion: null,
    };
    this.active = active;
    this.view = {
      ...this.view,
      status: 'running',
      liveStream: controller,
      active: index,
    };
    this.publish();
    if (controller) {
      active.unsubscribe = getStreamState(controller).subscribe(() =>
        this.maybeFinish(active)
      );
    }
    const first = schedule.batches[0];
    if (first) active.timer = setTimeout(() => this.tick(active), first.dueMs);
    else this.requestCompletion(active);
  }

  private tick(active: ActiveCandidate): void {
    if (this.active !== active) return;
    active.timer = null;
    // A throw here would otherwise escape a timer callback and take the app
    // down, losing both runs. Report it and keep the panes readable.
    try {
      this.advance(active);
    } catch (error) {
      this.fail(active, error);
    }
  }

  private advance(active: ActiveCandidate): void {
    const next = nextScheduledBatch(
      active.schedule.batches,
      active.batchIndex,
      nowMs() - active.startedAt
    );
    if (!next) {
      this.requestCompletion(active);
      return;
    }
    if (next.waitMs > 0) {
      active.timer = setTimeout(() => this.tick(active), next.waitMs);
      return;
    }
    active.batchIndex += 1;
    this.receive(active, next.batch.end, next.batch.dueMs);
    if (active.cursor >= active.schedule.chunks.length) {
      this.requestCompletion(active);
      return;
    }
    const following = nextScheduledBatch(
      active.schedule.batches,
      active.batchIndex,
      nowMs() - active.startedAt
    );
    active.timer = setTimeout(
      () => this.tick(active),
      Math.max(1, following?.waitMs ?? 0)
    );
  }

  private receive(active: ActiveCandidate, end: number, dueMs: number): void {
    if (active.cursor >= end) return;
    const lateness = nowMs() - active.startedAt - dueMs;
    if (lateness > active.maxLatenessMs) active.maxLatenessMs = lateness;
    const start = active.cursor;
    while (active.cursor < end) {
      const chunk = active.schedule.chunks[active.cursor];
      if (!chunk) break;
      active.controller?.append(chunk.text);
      active.source += chunk.text;
      active.cursor += 1;
    }
    if (active.cursor === start) return;
    if (active.controller) getStreamState(active.controller).flush();
    if (active.frameHandle === null) {
      active.frameHandle = requestAnimationFrame(() => this.onFrame(active));
    }
    this.view = { ...this.view, liveSource: active.source };
    this.publish();
  }

  /** Frame gaps measure JS-thread responsiveness, which is identical for both. */
  private onFrame(active: ActiveCandidate): void {
    if (this.active !== active) return;
    const at = nowMs();
    if (active.lastFrameAt === null) {
      active.firstFrameMs = at - active.startedAt;
    } else {
      const gap = at - active.lastFrameAt;
      active.frames += 1;
      if (gap > LONG_FRAME_MS) active.longFrames += 1;
      if (gap > active.longestFrameMs) active.longestFrameMs = gap;
    }
    active.lastFrameAt = at;
    active.frameHandle = requestAnimationFrame(() => this.onFrame(active));
  }

  private requestCompletion(active: ActiveCandidate): void {
    if (active.completionRequested) return;
    active.controller?.finish();
    active.completionRequested = true;
    this.maybeFinish(active);
  }

  private maybeFinish(active: ActiveCandidate): void {
    if (this.active !== active || !active.completionRequested) return;
    if (
      active.controller &&
      !allFinalSnapshotsReady(
        [getStreamState(active.controller).binding().readySequence],
        active.cursor
      )
    )
      return;
    if (active.cancelCompletion) return;
    active.cancelCompletion = afterTwoAnimationFrames(() => {
      active.cancelCompletion = null;
      this.complete(active);
    });
  }

  private complete(active: ActiveCandidate): void {
    if (this.active !== active) return;
    this.stopFrames(active);
    active.unsubscribe?.();
    active.unsubscribe = null;
    this.active = null;
    const next = active.index + 1;
    this.view = {
      ...this.view,
      runs: this.recordRun(active, null),
      liveStream: null,
      liveSource: active.source,
      active: next,
    };
    this.publish();
    if (next < CANDIDATES.length) this.startCandidate(next, active.schedule);
    else this.view = { ...this.view, status: 'complete', active: -1 };
    this.publish();
  }

  /** Keep both panes readable after a failed run instead of losing the data. */
  private fail(active: ActiveCandidate, cause: unknown): void {
    if (this.active !== active) return;
    this.stopFrames(active);
    active.unsubscribe?.();
    active.unsubscribe = null;
    this.active = null;
    this.view = {
      ...this.view,
      runs: this.recordRun(active, describe(cause)),
      liveStream: null,
      status: 'failed',
      active: -1,
      error: `${CANDIDATE_LABELS[active.candidate]} failed: ${describe(cause)}`,
    };
    this.publish();
  }

  private recordRun(
    active: ActiveCandidate,
    error: string | null
  ): (ComparisonRun | null)[] {
    const runs: (ComparisonRun | null)[] = [...this.view.runs];
    runs[active.index] = {
      candidate: active.candidate,
      stream: active.controller,
      metrics: {
        ...EMPTY_METRICS,
        firstFrameMs: active.firstFrameMs,
        totalMs: nowMs() - active.startedAt,
        frames: active.frames,
        longFrames: active.longFrames,
        longestFrameMs: active.longestFrameMs,
        maxLatenessMs: active.maxLatenessMs,
        chunks: active.cursor,
        source: active.source,
      },
      error,
    };
    return runs;
  }

  private stopFrames(active: ActiveCandidate): void {
    if (active.frameHandle !== null) {
      cancelAnimationFrame(active.frameHandle);
      active.frameHandle = null;
    }
    active.lastFrameAt = null;
  }

  private cancelActive(): void {
    const active = this.active;
    if (!active) return;
    if (active.timer !== null) clearTimeout(active.timer);
    active.cancelCompletion?.();
    active.unsubscribe?.();
    this.stopFrames(active);
    this.active = null;
  }

  private disposeControllers(): void {
    this.controllers.forEach((controller) => controller.dispose());
    this.controllers.clear();
  }

  private publish(): void {
    this.view = { ...this.view };
    this.notify?.();
  }
}

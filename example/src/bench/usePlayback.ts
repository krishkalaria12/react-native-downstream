import { useCallback, useEffect, useRef, useState } from 'react';
import type { MarkdownStreamController } from 'react-native-downstream';
import {
  createMarkdownStreamWithMode,
  getStreamState,
} from '../../../src/createMarkdownStream';
import type { ParseMode } from '../../../src/createMarkdownStream';
import {
  setStreamTracing,
  subscribeStreamTrace,
} from '../../../src/streamTrace';
import type { Fixture } from '../shared/fixtures';
import {
  afterTwoAnimationFrames,
  allFinalSnapshotsReady,
  resetSessionControllers,
} from '../shared/sessionLifecycle';
import {
  createSchedule,
  BATCH_WINDOW_MS,
  nextScheduledBatch,
  type PlaybackSchedule,
  type ScheduleKind,
} from '../shared/schedule';
import {
  nowMs,
  TraceRun,
  traceEnvironment,
  type RendererCandidate,
} from './trace';
import {
  createWorkload,
  type WorkloadShape,
  type WorkloadSize,
} from '../shared/workload';

export type PlaybackStatus =
  | 'idle'
  | 'running'
  | 'paused'
  | 'finishing'
  | 'complete';

export interface PlaybackConfig {
  fixture: Fixture;
  renderer: RendererCandidate;
  deviceId: string;
  runCondition: 'cold' | 'warm';
  schedule: ScheduleKind;
  seed: number;
  streams: number;
  traceEnabled: boolean;
  workloadSize: WorkloadSize;
  workloadShape: WorkloadShape;
}

export interface Receipt {
  chunk: number;
  receivedAt: number;
}

export interface LiveMessage {
  markdown: string;
  controllers: MarkdownStreamController[];
  revision: number;
  batch: number;
  receipts: Receipt[];
}

export interface CompletedMessage {
  id: string;
  fixture: string;
  stream: number;
  markdown: string;
  renderer: RendererCandidate;
}

interface PlaybackView {
  status: PlaybackStatus;
  live: LiveMessage | null;
  completed: CompletedMessage[];
  receivedChunks: number;
  totalChunks: number;
  run: number;
}

interface ActiveRun {
  id: number;
  config: PlaybackConfig;
  schedule: PlaybackSchedule;
  trace: TraceRun;
  cursor: number;
  prefix: string;
  batch: number;
  batchIndex: number;
  committedBatch: number;
  completionRequested: boolean;
  controllers: MarkdownStreamController[];
  unsubscribeSnapshots: (() => void)[];
  unsubscribeStreamTrace?: () => void;
  cancelCompletionFrame?: () => void;
  elapsedMs: number;
  resumedAt: number;
  timer?: ReturnType<typeof setTimeout>;
  status: PlaybackStatus;
}

const initialView: PlaybackView = {
  status: 'idle',
  live: null,
  completed: [],
  receivedChunks: 0,
  totalChunks: 0,
  run: 0,
};

function activeElapsedMs(run: ActiveRun): number {
  return (
    run.elapsedMs +
    (run.status === 'running' || run.status === 'finishing'
      ? nowMs() - run.resumedAt
      : 0)
  );
}

function sameScenario(a: PlaybackConfig, b: PlaybackConfig): boolean {
  return (
    a.renderer === b.renderer &&
    a.fixture.id === b.fixture.id &&
    a.schedule === b.schedule &&
    a.seed === b.seed &&
    a.streams === b.streams &&
    a.workloadSize === b.workloadSize &&
    a.workloadShape === b.workloadShape
  );
}

const PARSE_MODES: Record<RendererCandidate, ParseMode> = {
  'downstream-full': 'full',
  'downstream-incremental': 'incremental',
};

export function usePlayback() {
  const [view, setView] = useState(initialView);
  const active = useRef<ActiveRun | null>(null);
  const runs = useRef<TraceRun[]>([]);
  const nextRunId = useRef(1);
  const discardedRuns = useRef(0);

  const stopTimer = useCallback((run: ActiveRun) => {
    if (run.timer !== undefined) clearTimeout(run.timer);
    run.timer = undefined;
  }, []);

  const stopRun = useCallback(
    (run: ActiveRun) => {
      stopTimer(run);
      run.trace.stopFrames();
      run.cancelCompletionFrame?.();
      run.cancelCompletionFrame = undefined;
      run.unsubscribeSnapshots.forEach((unsubscribe) => unsubscribe());
      run.unsubscribeSnapshots = [];
      run.unsubscribeStreamTrace?.();
      run.unsubscribeStreamTrace = undefined;
      if (run.config.traceEnabled) {
        run.controllers.forEach((controller) =>
          setStreamTracing(controller, false)
        );
      }
    },
    [stopTimer]
  );

  const completeRun = useCallback(
    (run: ActiveRun) => {
      if (run.status === 'complete') return;
      const duration = activeElapsedMs(run);
      stopRun(run);
      run.status = 'complete';
      run.elapsedMs = duration;
      run.trace.end('complete', duration);
      setView((previous) =>
        previous.run === run.id
          ? { ...previous, status: 'complete', receivedChunks: run.cursor }
          : previous
      );
    },
    [stopRun]
  );

  const maybeComplete = useCallback(
    (run: ActiveRun) => {
      if (
        active.current !== run ||
        !run.completionRequested ||
        run.committedBatch !== run.batch ||
        !allFinalSnapshotsReady(
          run.controllers.map(
            (controller) => getStreamState(controller).binding().readySequence
          ),
          run.cursor
        )
      )
        return;
      if (run.cancelCompletionFrame) return;
      run.cancelCompletionFrame = afterTwoAnimationFrames(() => {
        run.cancelCompletionFrame = undefined;
        if (active.current !== run || run.status !== 'finishing') return;
        run.trace.mark('completion-frame-estimate', {
          sequence: run.cursor,
        });
        completeRun(run);
      });
    },
    [completeRun]
  );

  const requestCompletion = useCallback(
    (run: ActiveRun) => {
      if (run.completionRequested) return;
      run.controllers.forEach((controller) => controller.finish());
      run.completionRequested = true;
      if (run.status === 'paused') run.resumedAt = nowMs();
      run.status = 'finishing';
      setView((previous) =>
        previous.run === run.id
          ? { ...previous, status: 'finishing' }
          : previous
      );
      run.trace.mark('finish-sent');
      maybeComplete(run);
    },
    [maybeComplete]
  );

  const receiveThrough = useCallback(
    (
      run: ActiveRun,
      end: number,
      scheduledDueMs?: number,
      latenessMs?: number
    ) => {
      if (run.cursor >= end) return;
      const receipts: Receipt[] = [];
      const batch = ++run.batch;
      const start = run.cursor;
      while (run.cursor < end) {
        const chunk = run.schedule.chunks[run.cursor];
        if (!chunk) break;
        if (run.config.traceEnabled) {
          const receivedAt = nowMs();
          const activeElapsed =
            run.status === 'running'
              ? run.elapsedMs + receivedAt - run.resumedAt
              : run.elapsedMs;
          for (let stream = 0; stream < run.config.streams; stream++) {
            run.trace.mark('receipt', {
              stream,
              batch,
              chunk: chunk.index,
              scheduledDueMs: chunk.dueMs,
              latenessMs: Math.max(0, activeElapsed - chunk.dueMs),
            });
          }
          receipts.push({ chunk: chunk.index, receivedAt });
        }
        run.prefix += chunk.text;
        run.controllers.forEach((controller, stream) => {
          controller.append(chunk.text);
          run.trace.mark('append', {
            stream,
            batch,
            chunk: chunk.index,
            sessionId: run.trace.data.sessionIds[stream],
            sequence: run.cursor + 1,
          });
        });
        run.cursor += 1;
      }
      if (run.cursor === start) return;
      run.controllers.forEach((controller) =>
        getStreamState(controller).flush()
      );
      if (run.config.traceEnabled) {
        run.trace.mark('batch-delivery', { batch, scheduledDueMs, latenessMs });
        for (let stream = 0; stream < run.config.streams; stream++) {
          run.trace.mark('batch-flush', {
            stream,
            batch,
            value: run.cursor - start,
            scheduledDueMs,
            latenessMs,
            sessionId: run.trace.data.sessionIds[stream],
            sequence: run.cursor,
          });
        }
      }
      setView((previous) =>
        previous.run === run.id
          ? {
              ...previous,
              live: {
                markdown: run.prefix,
                controllers: run.controllers,
                revision: run.cursor,
                batch,
                receipts,
              },
              receivedChunks: run.cursor,
            }
          : previous
      );
    },
    []
  );

  const tick = useCallback(
    (run: ActiveRun) => {
      if (active.current !== run || run.status !== 'running') return;
      run.timer = undefined;
      const elapsed = run.elapsedMs + nowMs() - run.resumedAt;
      const next = nextScheduledBatch(
        run.schedule.batches,
        run.batchIndex,
        elapsed
      );
      if (!next) {
        requestCompletion(run);
        return;
      }
      if (next.waitMs > 0) {
        run.timer = setTimeout(() => tick(run), Math.max(1, next.waitMs));
        return;
      }
      run.batchIndex += 1;
      receiveThrough(run, next.batch.end, next.batch.dueMs, next.latenessMs);
      const following = nextScheduledBatch(
        run.schedule.batches,
        run.batchIndex,
        run.elapsedMs + nowMs() - run.resumedAt
      );
      if (following) {
        run.timer = setTimeout(() => tick(run), Math.max(1, following.waitMs));
      } else {
        requestCompletion(run);
      }
    },
    [receiveThrough, requestCompletion]
  );

  const acknowledgeCommit = useCallback(
    (runId: number, batch: number) => {
      const run = active.current;
      if (!run || run.id !== runId || batch <= run.committedBatch) return;
      run.committedBatch = batch;
      run.trace.mark('react-commit', { batch });
      maybeComplete(run);
    },
    [maybeComplete]
  );

  const play = useCallback(
    (config: PlaybackConfig) => {
      const previous = active.current;
      if (previous) {
        stopRun(previous);
        previous.controllers.forEach((controller) => controller.dispose());
        if (previous.status !== 'complete') {
          previous.trace.end('cancelled', activeElapsedMs(previous));
        }
      }
      const workload = createWorkload(
        config.fixture,
        config.workloadSize,
        config.workloadShape
      );
      const schedule = createSchedule(config.schedule, config.seed, workload);
      const id = nextRunId.current++;
      const mode = PARSE_MODES[config.renderer];
      const controllers: MarkdownStreamController[] = Array.from(
        { length: config.streams },
        () => createMarkdownStreamWithMode({}, mode)
      );
      const trace = new TraceRun({
        run: id,
        renderer: config.renderer,
        parseMode: mode,
        deviceId: config.deviceId.trim(),
        runCondition: config.runCondition,
        sessionIds: controllers.map(
          (controller) => getStreamState(controller).sessionId
        ),
        sampleOrder: id,
        fixture:
          workload.shape === 'open-fence' ? 'open-fence' : config.fixture.id,
        source:
          workload.shape === 'open-fence'
            ? 'generated/open-fence'
            : config.fixture.source,
        sourceBytes: workload.bytes,
        sourceHash: workload.hash,
        workloadSize: workload.size,
        workloadShape: workload.shape,
        schedule: config.schedule,
        seed: config.seed,
        streams: config.streams,
        traceEnabled: config.traceEnabled,
        sampleStride: Math.max(
          1,
          Math.ceil((schedule.chunks.length * config.streams) / 700)
        ),
        batchWindowMs: BATCH_WINDOW_MS,
      });
      runs.current.push(trace);
      if (runs.current.length > 10) {
        runs.current.shift();
        discardedRuns.current += 1;
      }
      const run: ActiveRun = {
        id,
        config,
        schedule,
        trace,
        cursor: 0,
        prefix: '',
        batch: 0,
        batchIndex: 0,
        committedBatch: 0,
        completionRequested: false,
        controllers,
        unsubscribeSnapshots: [],
        unsubscribeStreamTrace: undefined,
        elapsedMs: 0,
        resumedAt: nowMs(),
        status: 'running',
      };
      active.current = run;
      run.unsubscribeSnapshots = controllers.map((controller) =>
        getStreamState(controller).subscribe(() => maybeComplete(run))
      );
      if (config.traceEnabled) {
        run.unsubscribeStreamTrace = subscribeStreamTrace((event) =>
          trace.recordStreamEvent(event)
        );
        controllers.forEach((controller) => setStreamTracing(controller, true));
      }
      trace.mark('run-start');
      trace.startFrames();
      setView((current) => {
        const keepArchive = previous && sameScenario(previous.config, config);
        const completed = keepArchive ? [...current.completed] : [];
        if (keepArchive && previous.status === 'complete' && current.live) {
          for (let stream = 0; stream < previous.config.streams; stream++) {
            completed.push({
              id: `${previous.id}-${stream}`,
              fixture: previous.trace.data.fixture,
              stream,
              markdown: current.live.markdown,
              renderer: previous.config.renderer,
            });
          }
        }
        return {
          status: 'running',
          live: {
            markdown: '',
            controllers,
            revision: 0,
            batch: 0,
            receipts: [],
          },
          completed: completed.slice(-64),
          receivedChunks: 0,
          totalChunks: schedule.chunks.length,
          run: id,
        };
      });
      const first = schedule.batches[0];
      if (first) run.timer = setTimeout(() => tick(run), first.dueMs);
      else requestCompletion(run);
    },
    [maybeComplete, requestCompletion, stopRun, tick]
  );

  const pause = useCallback(() => {
    const run = active.current;
    if (!run || run.status !== 'running') return;
    run.elapsedMs = activeElapsedMs(run);
    run.status = 'paused';
    stopTimer(run);
    run.trace.stopFrames();
    run.trace.mark('pause');
    setView((previous) => ({ ...previous, status: 'paused' }));
  }, [stopTimer]);

  const resume = useCallback(() => {
    const run = active.current;
    if (!run || run.status !== 'paused') return;
    run.status = 'running';
    run.resumedAt = nowMs();
    run.trace.mark('resume');
    run.trace.startFrames();
    setView((previous) => ({ ...previous, status: 'running' }));
    tick(run);
  }, [tick]);

  const finish = useCallback(() => {
    const run = active.current;
    if (!run || run.status === 'complete' || run.completionRequested) return;
    stopTimer(run);
    run.trace.mark('finish-request');
    receiveThrough(run, run.schedule.chunks.length);
    run.batchIndex = run.schedule.batches.length;
    requestCompletion(run);
  }, [receiveThrough, requestCompletion, stopTimer]);

  const reset = useCallback(() => {
    const run = active.current;
    if (!run) {
      setView(initialView);
      return;
    }
    run.trace.mark('reset-request');
    run.completionRequested = false;
    resetSessionControllers(run.controllers);
    stopRun(run);
    if (run.status !== 'complete') {
      run.trace.end('cancelled', activeElapsedMs(run));
    }
    run.status = 'idle';
    run.cursor = 0;
    run.prefix = '';
    run.batch = 0;
    run.batchIndex = 0;
    run.committedBatch = 0;
    setView({
      ...initialView,
      run: run.id,
      live: {
        markdown: '',
        controllers: run.controllers,
        revision: 0,
        batch: 0,
        receipts: [],
      },
    });
  }, [stopRun]);

  const exportTrace = useCallback(
    () =>
      JSON.stringify(
        {
          schemaVersion: 1,
          environment: traceEnvironment(),
          discardedRuns: discardedRuns.current,
          runs: runs.current.map((run) => run.data),
        },
        null,
        2
      ),
    []
  );

  useEffect(
    () => () => {
      if (active.current) {
        stopRun(active.current);
        active.current.controllers.forEach((controller) =>
          controller.dispose()
        );
        if (active.current.status !== 'complete') {
          active.current.trace.end(
            'cancelled',
            activeElapsedMs(active.current)
          );
        }
      }
    },
    [stopRun]
  );

  return {
    view,
    activeTrace: active.current?.trace,
    play,
    pause,
    resume,
    finish,
    reset,
    exportTrace,
    acknowledgeCommit,
  };
}

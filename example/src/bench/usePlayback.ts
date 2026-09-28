import { useCallback, useEffect, useRef, useState } from 'react';
import type { Fixture } from './fixtures';
import {
  createSchedule,
  BATCH_WINDOW_MS,
  nextScheduledBatch,
  type PlaybackSchedule,
  type ScheduleKind,
} from './schedule';
import { nowMs, TraceRun, traceEnvironment } from './trace';
import {
  createWorkload,
  type WorkloadShape,
  type WorkloadSize,
} from './workload';

export type PlaybackStatus = 'idle' | 'running' | 'paused' | 'complete';

export interface PlaybackConfig {
  fixture: Fixture;
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
  revision: number;
  batch: number;
  receipts: Receipt[];
}

export interface CompletedMessage {
  id: string;
  fixture: string;
  stream: number;
  markdown: string;
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
  awaitingCommit: boolean;
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
    run.elapsedMs + (run.status === 'running' ? nowMs() - run.resumedAt : 0)
  );
}

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
    },
    [stopTimer]
  );

  const finishRun = useCallback(
    (run: ActiveRun) => {
      if (run.status === 'complete') return;
      const duration = activeElapsedMs(run);
      stopRun(run);
      run.awaitingCommit = false;
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

  const receiveThrough = useCallback(
    (
      run: ActiveRun,
      end: number,
      scheduledDueMs?: number,
      latenessMs?: number
    ) => {
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
        run.cursor += 1;
      }
      if (run.cursor === start) return;
      if (run.config.traceEnabled) {
        run.trace.mark('batch-delivery', { batch, scheduledDueMs, latenessMs });
        for (let stream = 0; stream < run.config.streams; stream++) {
          run.trace.mark('batch-flush', {
            stream,
            batch,
            value: run.cursor - start,
            scheduledDueMs,
            latenessMs,
          });
        }
      }
      setView((previous) =>
        previous.run === run.id
          ? {
              ...previous,
              live: {
                markdown: run.prefix,
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
      if (
        active.current !== run ||
        run.status !== 'running' ||
        run.awaitingCommit
      )
        return;
      run.timer = undefined;
      const elapsed = run.elapsedMs + nowMs() - run.resumedAt;
      const next = nextScheduledBatch(
        run.schedule.batches,
        run.batchIndex,
        elapsed
      );
      if (!next) {
        finishRun(run);
        return;
      }
      if (next.waitMs > 0) {
        run.timer = setTimeout(() => tick(run), Math.max(1, next.waitMs));
        return;
      }
      run.batchIndex += 1;
      run.awaitingCommit = true;
      receiveThrough(run, next.batch.end, next.batch.dueMs, next.latenessMs);
    },
    [finishRun, receiveThrough]
  );

  const acknowledgeCommit = useCallback(
    (runId: number, batch: number) => {
      const run = active.current;
      if (
        !run ||
        run.id !== runId ||
        !run.awaitingCommit ||
        batch !== run.batch
      )
        return;
      run.awaitingCommit = false;
      run.trace.mark('react-commit', { batch });
      if (run.cursor === run.schedule.chunks.length) {
        finishRun(run);
        return;
      }
      if (run.status !== 'running') return;
      const elapsed = run.elapsedMs + nowMs() - run.resumedAt;
      const next = nextScheduledBatch(
        run.schedule.batches,
        run.batchIndex,
        elapsed
      );
      if (next)
        run.timer = setTimeout(() => tick(run), Math.max(1, next.waitMs));
    },
    [finishRun, tick]
  );

  const play = useCallback(
    (config: PlaybackConfig) => {
      const previous = active.current;
      if (previous) {
        stopRun(previous);
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
      const trace = new TraceRun({
        run: id,
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
        sampleStride: Math.max(1, Math.ceil(schedule.chunks.length / 700)),
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
        awaitingCommit: false,
        elapsedMs: 0,
        resumedAt: nowMs(),
        status: 'running',
      };
      active.current = run;
      trace.mark('run-start');
      trace.startFrames();
      setView((current) => {
        const completed = [...current.completed];
        if (previous?.status === 'complete' && current.live) {
          for (let stream = 0; stream < previous.config.streams; stream++) {
            completed.push({
              id: `${previous.id}-${stream}`,
              fixture: previous.trace.data.fixture,
              stream,
              markdown: current.live.markdown,
            });
          }
        }
        return {
          status: 'running',
          live: { markdown: '', revision: 0, batch: 0, receipts: [] },
          completed: completed.slice(-64),
          receivedChunks: 0,
          totalChunks: schedule.chunks.length,
          run: id,
        };
      });
      const first = schedule.batches[0];
      if (first) run.timer = setTimeout(() => tick(run), first.dueMs);
      else finishRun(run);
    },
    [finishRun, stopRun, tick]
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
    if (!run.awaitingCommit) tick(run);
  }, [tick]);

  const finish = useCallback(() => {
    const run = active.current;
    if (!run || run.status === 'complete') return;
    stopTimer(run);
    run.trace.mark('finish-request');
    receiveThrough(run, run.schedule.chunks.length);
    finishRun(run);
  }, [finishRun, receiveThrough, stopTimer]);

  const reset = useCallback(() => {
    if (active.current) {
      stopRun(active.current);
      if (active.current.status !== 'complete') {
        active.current.trace.end('cancelled', activeElapsedMs(active.current));
      }
    }
    active.current = null;
    setView(initialView);
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

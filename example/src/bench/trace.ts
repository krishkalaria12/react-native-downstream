import { Dimensions, PixelRatio, Platform } from 'react-native';
import type { ScheduleKind } from './schedule';
import type { WorkloadShape, WorkloadSize } from './workload';

export interface TraceEvent {
  atMs: number;
  type: string;
  stream?: number;
  batch?: number;
  chunk?: number;
  value?: number;
  scheduledDueMs?: number;
  latenessMs?: number;
}

export interface TraceRunData {
  run: number;
  fixture: string;
  source: string;
  sourceBytes: number;
  sourceHash: string;
  workloadSize: WorkloadSize;
  workloadShape: WorkloadShape;
  schedule: ScheduleKind;
  seed: number;
  streams: number;
  startedAt: string;
  traceEnabled: boolean;
  sampleStride: number;
  batchWindowMs: number;
  completionStatus: 'running' | 'complete' | 'cancelled';
  runDurationMs: number;
  wallDurationMs: number;
  droppedEvents: number;
  events: TraceEvent[];
}

const MAX_EVENTS = 50000;

export function nowMs(): number {
  return (
    (
      globalThis as { performance?: { now: () => number } }
    ).performance?.now() ?? Date.now()
  );
}

export class TraceRun {
  readonly data: TraceRunData;
  private readonly origin = nowMs();
  private frameHandle: number | undefined;
  private lastFrame: number | undefined;
  private readonly sampledBatches = new Set<number>();

  constructor(
    data: Omit<
      TraceRunData,
      | 'startedAt'
      | 'completionStatus'
      | 'runDurationMs'
      | 'wallDurationMs'
      | 'droppedEvents'
      | 'events'
    >
  ) {
    this.data = {
      ...data,
      startedAt: new Date().toISOString(),
      completionStatus: 'running',
      runDurationMs: 0,
      wallDurationMs: 0,
      droppedEvents: 0,
      events: [],
    };
  }

  mark(type: string, details: Omit<TraceEvent, 'atMs' | 'type'> = {}) {
    if (!this.data.traceEnabled) return;
    if (details.chunk !== undefined) {
      if (details.chunk % this.data.sampleStride !== 0) return;
      if (details.batch !== undefined) this.sampledBatches.add(details.batch);
    }
    if (
      details.chunk === undefined &&
      details.batch !== undefined &&
      !this.sampledBatches.has(details.batch)
    )
      return;
    if (this.data.events.length >= MAX_EVENTS) {
      this.data.droppedEvents += 1;
      return;
    }
    this.data.events.push({ atMs: nowMs() - this.origin, type, ...details });
  }

  shouldSampleBatch(batch: number) {
    return this.data.traceEnabled && this.sampledBatches.has(batch);
  }

  end(status: 'complete' | 'cancelled', activeDurationMs: number) {
    if (this.data.completionStatus !== 'running') return;
    this.data.completionStatus = status;
    this.data.runDurationMs = activeDurationMs;
    this.data.wallDurationMs = nowMs() - this.origin;
    this.mark(status === 'complete' ? 'run-complete' : 'run-cancelled');
  }

  startFrames() {
    if (!this.data.traceEnabled || this.frameHandle !== undefined) return;
    const onFrame = (timestamp: number) => {
      if (this.lastFrame !== undefined) {
        this.mark('js-frame-interval', { value: timestamp - this.lastFrame });
      }
      this.lastFrame = timestamp;
      this.frameHandle = requestAnimationFrame(onFrame);
    };
    this.frameHandle = requestAnimationFrame(onFrame);
  }

  stopFrames() {
    if (this.frameHandle !== undefined) cancelAnimationFrame(this.frameHandle);
    this.frameHandle = undefined;
    this.lastFrame = undefined;
  }
}

export function traceEnvironment() {
  const window = Dimensions.get('window');
  const highResolutionClock = (
    globalThis as { performance?: { now: () => number } }
  ).performance?.now;
  return {
    platform: Platform.OS,
    platformVersion: String(Platform.Version),
    screen: {
      width: window.width,
      height: window.height,
      scale: PixelRatio.get(),
    },
    recordedAt: new Date().toISOString(),
    clock: highResolutionClock ? 'performance.now' : 'Date.now',
    frameMethod: 'requestAnimationFrame on JS; visible frame is an estimate',
    rendererUpdateMethod:
      'React prop commit; Streamdown may update asynchronously',
    durationMethod:
      'runDurationMs excludes pauses; wallDurationMs includes pauses',
  };
}

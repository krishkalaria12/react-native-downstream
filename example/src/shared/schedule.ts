import type { Workload } from './workload';

export type ScheduleKind = 'manifest' | 'character' | 'range' | 'bursty';

export interface ScheduledChunk {
  text: string;
  dueMs: number;
  index: number;
}

export interface PlaybackSchedule {
  chunks: ScheduledChunk[];
  batches: ScheduledBatch[];
  source: string;
  durationMs: number;
}

export interface ScheduledBatch {
  index: number;
  start: number;
  end: number;
  dueMs: number;
}

export const BATCH_WINDOW_MS = 16;

export function createBatches(
  chunks: readonly ScheduledChunk[]
): ScheduledBatch[] {
  const batches: ScheduledBatch[] = [];
  for (const chunk of chunks) {
    const dueMs = Math.ceil(chunk.dueMs / BATCH_WINDOW_MS) * BATCH_WINDOW_MS;
    const last = batches[batches.length - 1];
    if (last?.dueMs === dueMs) {
      last.end = chunk.index + 1;
    } else {
      batches.push({
        index: batches.length,
        start: chunk.index,
        end: chunk.index + 1,
        dueMs,
      });
    }
  }
  return batches;
}

export function nextScheduledBatch(
  batches: readonly ScheduledBatch[],
  index: number,
  elapsedMs: number
): { batch: ScheduledBatch; waitMs: number; latenessMs: number } | null {
  const batch = batches[index];
  if (!batch) return null;
  return {
    batch,
    waitMs: Math.max(0, batch.dueMs - elapsedMs),
    latenessMs: Math.max(0, elapsedMs - batch.dueMs),
  };
}

function random(seed: number): () => number {
  const modulus = 0x100000000;
  let state = ((Math.trunc(seed) % modulus) + modulus) % modulus;
  return () => {
    state = (state * 1664525 + 1013904223) % modulus;
    return state / modulus;
  };
}

function splitRange(source: string, next: () => number, max: number): string[] {
  const points = Array.from(source);
  const chunks: string[] = [];
  for (let cursor = 0; cursor < points.length;) {
    const length = 1 + Math.floor(next() * max);
    chunks.push(points.slice(cursor, cursor + length).join(''));
    cursor += length;
  }
  return chunks;
}

export function createSchedule(
  kind: ScheduleKind,
  seed: number,
  workload: Workload
): PlaybackSchedule {
  const source = workload.source;
  const next = random(seed);
  const texts =
    kind === 'manifest'
      ? workload.manifestChunks
      : kind === 'character'
        ? Array.from(source)
        : splitRange(source, next, kind === 'range' ? 20 : 32);

  let dueMs = 0;
  const chunks = texts.map((text, index) => {
    if (kind === 'manifest') {
      dueMs += 24;
    } else if (kind === 'character') {
      dueMs += 24;
    } else if (kind === 'range') {
      dueMs += 10 + Math.floor(next() * 41);
    } else {
      // Short clusters separated by reproducible network stalls.
      dueMs += index % 5 === 0 ? 160 + Math.floor(next() * 220) : 2;
    }
    return { text, dueMs, index };
  });

  return { chunks, batches: createBatches(chunks), source, durationMs: dueMs };
}

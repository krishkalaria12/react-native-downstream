// Read-only comparison. Usage: node benchmarks/phone/tail-compare.mjs <result-dir>
import { readFileSync, readdirSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  lastDeliveryWall,
  lastFinishWall,
  nativePhaseSummary,
} from './bench.mjs';

/** Legacy host/app span difference; this is not an app finish receipt. */
export function estimateCompletionWait(record) {
  const live = record.live ?? [];
  if (
    !live.length ||
    !Number.isFinite(record.app?.deliveryDurationMs) ||
    live.some(
      (thread) =>
        !Number.isFinite(thread.firstDeltaMs) ||
        !Number.isFinite(thread.lastDeltaMs)
    )
  )
    return null;
  const textSpanMs =
    Math.max(...live.map((thread) => thread.lastDeltaMs)) -
    Math.min(...live.map((thread) => thread.firstDeltaMs));
  return Math.max(0, record.app.deliveryDurationMs - textSpanMs);
}

export function tailRow(record) {
  const app = record.app ?? {};
  const lastText = lastDeliveryWall(app, app.startWallTimeMs);
  const finish = lastFinishWall(app);
  return {
    index: record.config.index,
    workload: record.config.workload,
    profile: record.config.profile,
    finishDelayMs: record.config.finishDelayMs ?? 0,
    settleMs: record.metrics?.settleMs ?? null,
    textToFinishMs:
      lastText != null && finish != null
        ? Math.max(0, finish - lastText)
        : null,
    settleAfterFinishMs: record.metrics?.settleAfterFinishMs ?? null,
    finishToDoneMs:
      Number.isFinite(app.streamDone?.wallTimeMs) && finish != null
        ? Math.max(0, app.streamDone.wallTimeMs - finish)
        : null,
    estimatedCompletionWaitMs:
      finish == null ? estimateCompletionWait(record) : null,
    tailWindowCensored: record.metrics?.tailWindowCensored ?? null,
    tail: record.metrics?.tail ?? null,
    nativePhases:
      record.metrics?.nativePhases ??
      (record.nativeTrace ? nativePhaseSummary(record.nativeTrace) : null),
    trace: record.trace ?? null,
  };
}

if (
  process.argv[1] &&
  resolve(process.argv[1]) === fileURLToPath(import.meta.url)
) {
  if (!process.argv[2])
    throw new Error(
      'Usage: node benchmarks/phone/tail-compare.mjs <result-dir>'
    );
  const directory = join(resolve(process.argv[2]), 'runs');
  const rows = readdirSync(directory)
    .filter((file) => file.endsWith('.json'))
    .map((file) => JSON.parse(readFileSync(join(directory, file), 'utf8')))
    .filter(
      (record) =>
        record.status === 'ok' &&
        !record.config.warmup &&
        record.config.candidate === 'downstream-default' &&
        ['ai-feature-matrix', 'ai-huge-concurrent-5'].includes(
          record.config.workload
        )
    )
    .map(tailRow)
    .sort(
      (a, b) =>
        a.workload.localeCompare(b.workload) ||
        a.profile.localeCompare(b.profile) ||
        a.finishDelayMs - b.finishDelayMs ||
        a.settleMs - b.settleMs
    );
  console.log(
    JSON.stringify(
      {
        notes: [
          'Each workload/profile/finish-delay group is sorted from fastest to slowest last-text settle.',
          'estimatedCompletionWaitMs mixes host text span and phone duration through host done. It includes transport, scheduling, iterator close and usage metadata waits. It is not an exact finish or render measurement.',
          'A delayed finish/done does not prove continuous drawing; only retained traces can identify the work. Frame counts in adjacent tail phases can overlap when a frame crosses the boundary.',
        ],
        runs: rows,
      },
      null,
      2
    )
  );
}

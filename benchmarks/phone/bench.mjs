#!/usr/bin/env node
/* eslint-disable no-bitwise -- the seeded random number generator is integer bit arithmetic */
// Phone benchmark for React Native Markdown renderers, in one file.
//
//   node benchmarks/phone/bench.mjs                 build, install, run live and report
//   node benchmarks/phone/bench.mjs --replay <dir>  same, replaying recorded responses instead of live ones
//   node benchmarks/phone/bench.mjs capture --replay <dir>   record responses for --replay
//   node benchmarks/phone/bench.mjs build           build and fingerprint the release APK only
//   node benchmarks/phone/bench.mjs run             run on the phone with an existing build
//   node benchmarks/phone/bench.mjs resume <dir>    retry failed and missing runs using the saved APK
//   node benchmarks/phone/bench.mjs report <dir>    recompute statistics and REPORT.md from raw runs
//   node benchmarks/phone/bench.mjs selftest        check the statistics and workload code
//   node benchmarks/phone/tail-compare.mjs <dir>    compare saved matrix/huge tails without a phone
//
// Defaults: highlighted and highlighted-animated, with 5 measured repeats plus
// one warmup per regular workload. Animated runs include Downstream, Streamdown
// and Enriched; huge workloads still run once per candidate and profile.
//
// Animation comparison, identical replay inputs with traces retained:
//   node benchmarks/phone/bench.mjs all --replay <recording-dir> --output <new-result-dir> \
//     --profiles highlighted,highlighted-animated --candidates downstream-default,streamdown,enriched \
//     --workloads ai-feature-matrix,ai-huge-concurrent-5 --keep-traces
// Terminal-wait experiment: repeat that command in new result directories with
//   --finish-delay-ms 0, then 1000, then 3000 (keep the same replay recording).
// Diagnose native phases separately, with timings labeled as diagnostic:
//   node benchmarks/phone/bench.mjs all --replay <recording-dir> --output <diagnostic-dir> \
//     --candidates downstream-default --workloads ai-feature-matrix,ai-huge-concurrent-5 \
//     --diagnostic-trace --finish-delay-ms 1000 --keep-traces
//
// Options: --device <serial> --profiles highlighted,highlighted-animated[,plain,plain-animated] --repeats 5 --seed 1
//          --workloads ai-single,ai-code-heavy,ai-concurrent-5 --candidates <ids>
//          --model gpt-6-luna --reasoning medium --output <dir> --keep-traces --skip-build --no-device-setup
//          --settle-ms 2000 (same observation budget for every selected profile)
//          --finish-delay-ms 0 (replay-only terminal delay; use 1000 and 3000 in separate result dirs)
//          --diagnostic-trace (Downstream only; optional native phases, timings include tracing overhead)
// Retirement measures release of session-owned superseded snapshots on a worker.
// UI-held payload references can be released later; this is not all-platform deallocation timing.
//
// Method, in short. Each run asks the model for fresh replies through the Vercel
// AI SDK on this Mac, and every text delta goes to the phone the moment it
// arrives. All libraries share one release APK, one cold process per run.
// Perfetto records the run: SurfaceFlinger FrameTimeline for every frame, the
// scheduler for CPU time per thread, and process memory every 100 ms. Runs are
// ordered in seeded random blocks, start only when the phone is cool, and the
// report gives medians with bootstrap confidence intervals.

import { spawn, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import {
  copyFileSync,
  existsSync,
  mkdirSync,
  readdirSync,
  readFileSync,
  renameSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';
import assert from 'node:assert/strict';
import { FEATURE_ORDERS, PROMPTS, WORKLOADS } from './workloads.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, '../..');
const EXAMPLE = join(ROOT, 'example');
const PACKAGE = 'downstream.example';
const PORT = 8765;
const APK = join(
  EXAMPLE,
  'android/app/build/outputs/apk/release/app-release.apk'
);
const DEVICE_TRACE = '/data/misc/perfetto-traces/downstream-bench.pftrace';
const TRACE_PROCESSOR = {
  version: 'v51.2',
  url: 'https://commondatastorage.googleapis.com/perfetto-luci-artifacts/v51.2/mac-arm64/trace_processor_shell',
  sha256: 'f29d80cd9c9fb400ed29bc014ce573c85b69ca50ad3679b1ae3d2eab76d4f399',
  path: join(HERE, '.tools/trace_processor_shell'),
};

let CANDIDATES = {
  'downstream-default': 'Downstream',
  'streamdown': 'RN Streamdown',
  'enriched': 'Enriched Markdown',
  'markdown-display': 'Markdown Display',
  'marked': 'Marked',
  'markdown-renderer': 'Markdown Renderer',
};
let BASELINE = 'downstream-default';
let benchmarkSuite;

function suiteFingerprint() {
  return sha(
    JSON.stringify({
      id: benchmarkSuite.id,
      candidates: CANDIDATES,
      baseline: BASELINE,
      profiles: benchmarkSuite.profiles,
    })
  );
}

function assertSessionSuite(session) {
  if ((session.benchmarkVersion ?? null) !== (benchmarkSuite?.id ?? null))
    throw new Error(
      'This session belongs to a different benchmark entrypoint.'
    );
  if (benchmarkSuite && session.suiteFingerprint !== suiteFingerprint())
    throw new Error(
      'Benchmark definitions changed; use a new result directory.'
    );
}

function assertBuildSuite(record, profile) {
  if (
    (record.benchmarkVersion ?? null) !== (benchmarkSuite?.id ?? null) ||
    (benchmarkSuite && record.suiteFingerprint !== suiteFingerprint()) ||
    record.profile !== profile
  )
    throw new Error(
      'Saved APK belongs to a different benchmark/profile; rebuild it.'
    );
}

export function profileConfig(profile) {
  if (benchmarkSuite) {
    const config = benchmarkSuite.profiles[profile];
    if (!config) throw new Error(`Unknown profile ${profile}`);
    return { ...config };
  }
  if (
    ![
      'highlighted',
      'plain',
      'highlighted-animated',
      'plain-animated',
    ].includes(profile)
  )
    throw new Error(`Unknown profile ${profile}`);
  return {
    syntaxHighlighting: profile.startsWith('highlighted'),
    animated: profile.endsWith('-animated'),
  };
}

export function candidatesForProfile(candidates, profile) {
  return profileConfig(profile).animated
    ? candidates.filter((candidate) =>
        (
          benchmarkSuite?.animatedCandidates ?? [
            'downstream-default',
            'streamdown',
            'enriched',
          ]
        ).includes(candidate)
      )
    : candidates;
}

export function validateFinishDelay(finishDelayMs, replay) {
  if (
    !Number.isFinite(finishDelayMs) ||
    finishDelayMs < 0 ||
    finishDelayMs > 2147483647
  )
    throw new Error(
      '--finish-delay-ms must be a nonnegative finite timer duration'
    );
  if (finishDelayMs > 0 && !replay)
    throw new Error('--finish-delay-ms requires --replay');
  return finishDelayMs;
}

// ---------------------------------------------------------------------------
// Workloads: real AI responses recorded with `capture`, replayed exactly.

function loadRecordings(folder) {
  return readdirSync(folder)
    .filter((name) => /^response-\d+\.json$/.test(name))
    .sort((a, b) => Number(a.match(/\d+/)[0]) - Number(b.match(/\d+/)[0]))
    .map((name) => {
      const recording = JSON.parse(readFileSync(join(folder, name), 'utf8'));
      const sha256 = sha(recording.source);
      if (
        recording.chunks.map((chunk) => chunk.text).join('') !==
          recording.source ||
        sha256 !== recording.sha256
      ) {
        throw new Error(
          `${name}: chunks or sha256 do not match the recorded source`
        );
      }
      return { ...recording, file: join(folder, name) };
    });
}

/**
 * Interleaves the chosen responses into one delivery plan. A single response is
 * replayed from its first delta; several keep their real offsets from the
 * concurrent capture. Chunks name the view (`thread`) that receives them.
 */
export function buildWorkload(recordings, responses) {
  const chosen = responses.map((index) => {
    if (!recordings[index])
      throw new Error(`Recording response-${index}.json is missing`);
    return recordings[index];
  });
  const first = Math.min(
    ...chosen.map((recording) => recording.firstArrivalMs)
  );
  const plan = [];
  chosen.forEach((recording, thread) => {
    const offset = chosen.length > 1 ? recording.firstArrivalMs - first : 0;
    recording.chunks.forEach((chunk, order) =>
      plan.push({
        text: chunk.text,
        dueMs: chunk.dueMs + offset,
        thread,
        order,
      })
    );
  });
  plan.sort(
    (a, b) => a.dueMs - b.dueMs || a.thread - b.thread || a.order - b.order
  );
  const chunks = plan.map(({ text, dueMs, thread }) =>
    chosen.length > 1 ? { text, dueMs, thread } : { text, dueMs }
  );
  return {
    source: chunks.map((chunk) => chunk.text).join(''),
    chunks,
    threads: chosen.map((recording, thread) => ({
      thread,
      recording: relative(ROOT, recording.file),
      sha256: recording.sha256,
      model: recording.model,
      bytes: Buffer.byteLength(recording.source),
    })),
  };
}

// ---------------------------------------------------------------------------
// Statistics

function mulberry32(seed) {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6d2b79f5) >>> 0;
    let t = state;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function shuffle(items, random) {
  const copy = [...items];
  for (let index = copy.length - 1; index > 0; index -= 1) {
    const swap = Math.floor(random() * (index + 1));
    [copy[index], copy[swap]] = [copy[swap], copy[index]];
  }
  return copy;
}

/** Linear-interpolated quantile (type 7, as in R and NumPy). */
export function quantile(values, q) {
  const sorted = values.filter(Number.isFinite).sort((a, b) => a - b);
  if (!sorted.length) return null;
  const position = (sorted.length - 1) * q;
  const low = Math.floor(position);
  const high = Math.ceil(position);
  return sorted[low] + (sorted[high] - sorted[low]) * (position - low);
}

const median = (values) => quantile(values, 0.5);

/** Percentile bootstrap of a statistic over one sample. */
export function bootstrap(values, statistic, random, resamples = 4000) {
  const finite = values.filter(Number.isFinite);
  if (finite.length < 2) return null;
  const estimates = [];
  for (let round = 0; round < resamples; round += 1) {
    const sample = finite.map(
      () => finite[Math.floor(random() * finite.length)]
    );
    estimates.push(statistic(sample));
  }
  return [quantile(estimates, 0.025), quantile(estimates, 0.975)];
}

/** Bootstrap interval for median(b) / median(a), resampling both groups. */
export function ratioInterval(a, b, random, resamples = 4000) {
  const x = a.filter(Number.isFinite);
  const y = b.filter(Number.isFinite);
  if (x.length < 2 || y.length < 2) return null;
  const ratios = [];
  for (let round = 0; round < resamples; round += 1) {
    const mx = median(x.map(() => x[Math.floor(random() * x.length)]));
    const my = median(y.map(() => y[Math.floor(random() * y.length)]));
    if (mx > 0) ratios.push(my / mx);
  }
  return ratios.length
    ? [quantile(ratios, 0.025), quantile(ratios, 0.975)]
    : null;
}

function normalCdf(z) {
  // Abramowitz and Stegun 7.1.26
  const t = 1 / (1 + (0.3275911 * Math.abs(z)) / Math.SQRT2);
  const erf =
    1 -
    ((((1.061405429 * t - 1.453152027) * t + 1.421413741) * t - 0.284496736) *
      t +
      0.254829592) *
      t *
      Math.exp(-(z * z) / 2);
  return z >= 0 ? (1 + erf) / 2 : (1 - erf) / 2;
}

/** Two-sided Mann-Whitney U test, normal approximation with tie correction. */
export function mannWhitney(a, b) {
  const x = a.filter(Number.isFinite);
  const y = b.filter(Number.isFinite);
  const n1 = x.length;
  const n2 = y.length;
  // One run per group gives no test; huge workloads run once.
  if (n1 < 2 || n2 < 2) return null;
  const all = [
    ...x.map((value) => ({ value, group: 0 })),
    ...y.map((value) => ({ value, group: 1 })),
  ].sort((p, q) => p.value - q.value);
  let rankSum = 0;
  let tieTerm = 0;
  for (let start = 0; start < all.length;) {
    let end = start;
    while (end + 1 < all.length && all[end + 1].value === all[start].value)
      end += 1;
    const rank = (start + end + 2) / 2;
    const ties = end - start + 1;
    tieTerm += ties ** 3 - ties;
    for (let index = start; index <= end; index += 1)
      if (all[index].group === 0) rankSum += rank;
    start = end + 1;
  }
  const u = rankSum - (n1 * (n1 + 1)) / 2;
  const n = n1 + n2;
  const variance = ((n1 * n2) / 12) * (n + 1 - tieTerm / (n * (n - 1)));
  if (variance <= 0) return { u, p: 1 };
  const z =
    (u - (n1 * n2) / 2 - Math.sign(u - (n1 * n2) / 2) * 0.5) /
    Math.sqrt(variance);
  return { u, p: Math.min(1, 2 * (1 - normalCdf(Math.abs(z)))) };
}

// ---------------------------------------------------------------------------
// Per-run metrics from the Perfetto trace and the app's own samples

/** Buckets an app thread for the CPU breakdown. */
function threadGroup(thread, pid) {
  if (thread?.tid === pid) return 'main';
  if (thread?.name === 'RenderThread') return 'render';
  if (/^mqt_(v_)?js/.test(thread?.name ?? '')) return 'js';
  return 'other';
}

/**
 * Turns raw trace rows into metrics for the measured window. `window` holds
 * trace timestamps (ns): start, deliveryEnd, optional finishEnd, end.
 */
export function traceMetrics(raw, window) {
  const { start, deliveryEnd, finishEnd, end } = window;
  const span = end - start;
  const overlap = (ts, dur) =>
    Math.max(0, Math.min(ts + dur, end) - Math.max(ts, start));

  // CPU: scheduler time of every app thread inside the window.
  const threads = new Map(raw.threads.map((thread) => [thread.utid, thread]));
  const cpu = { main: 0, js: 0, render: 0, other: 0 };
  const byThread = new Map();
  for (const [utid, ts, dur] of raw.sched) {
    const used = overlap(ts, dur);
    if (!used) continue;
    const thread = threads.get(utid);
    cpu[threadGroup(thread, raw.pid)] += used;
    const key = thread?.name ?? `utid ${utid}`;
    byThread.set(key, (byThread.get(key) ?? 0) + used);
  }
  const cpuTotal = cpu.main + cpu.js + cpu.render + cpu.other;

  // Frames: SurfaceFlinger FrameTimeline for the app's layers. Overrun is the
  // actual end minus the expected (deadline) end, as in Macrobenchmark.
  const frames = raw.frames.filter(
    ([ts, dur]) => dur > 0 && ts >= start && ts < end
  );
  const durations = frames.map(([, dur]) => dur / 1e6);
  const overruns = frames.map(([ts, dur, , , expectedTs, expectedDur]) =>
    expectedTs == null ? null : (ts + dur - (expectedTs + expectedDur)) / 1e6
  );
  const appJank = frames.filter(([, , jank]) =>
    /App Deadline Missed/i.test(jank ?? '')
  ).length;
  const anyJank = frames.filter(
    ([, , jank]) =>
      jank &&
      jank !== 'None' &&
      !/^(Buffer|SurfaceFlinger) Stuffing$/.test(jank)
  ).length;
  // A frame's expected duration is its deadline window, not the refresh
  // period; consecutive expected starts are one vsync apart while drawing.
  const expectedStarts = frames
    .map(([, , , , expectedTs]) => expectedTs)
    .filter((value) => value != null)
    .sort((a, b) => a - b);
  const vsync = median(
    expectedStarts
      .slice(1)
      .map((ts, index) => (ts - expectedStarts[index]) / 1e6)
      .filter((gap) => gap > 0 && gap < 50)
  );
  const lastFrameEnd = raw.frames.reduce(
    (last, [ts, dur]) =>
      dur > 0 && ts >= start && ts < end ? Math.max(last, ts + dur) : last,
    0
  );

  // Memory: anonymous RSS plus swap, sampled every 100 ms. Anonymous memory is
  // what the renderer allocates; file-backed pages are shared code and fonts.
  const series = (name) =>
    raw.memory
      .filter(([track]) => track === name)
      .map(([, ts, value]) => [ts, value]);
  const anon = series('mem.rss.anon');
  const swap = series('mem.swap');
  const valueAt = (points, ts) => {
    let value = points[0]?.[1] ?? 0;
    for (const [at, current] of points) {
      if (at > ts) break;
      value = current;
    }
    return value;
  };
  const used = (ts) => valueAt(anon, ts) + valueAt(swap, ts);
  const inside = anon
    .filter(([ts]) => ts >= start && ts <= end)
    .map(([ts]) => used(ts));
  const before = used(start);
  const peak = Math.max(before, ...inside);
  const MiB = 1024 * 1024;

  return {
    windowMs: span / 1e6,
    cpuTotalS: cpuTotal / 1e9,
    cpuMainS: cpu.main / 1e9,
    cpuJsS: cpu.js / 1e9,
    cpuRenderS: cpu.render / 1e9,
    cpuOtherS: cpu.other / 1e9,
    cpuCores: cpuTotal / span,
    cpuByThreadS: Object.fromEntries(
      [...byThread]
        .sort((a, b) => b[1] - a[1])
        .map(([name, value]) => [name, value / 1e9])
    ),
    frames: frames.length,
    jankAppPct: frames.length ? (100 * appJank) / frames.length : null,
    jankAnyPct: frames.length ? (100 * anyJank) / frames.length : null,
    frameP50Ms: quantile(durations, 0.5),
    frameP95Ms: quantile(durations, 0.95),
    frameP99Ms: quantile(durations, 0.99),
    frameMaxMs: durations.length ? Math.max(...durations) : null,
    overrunP95Ms: quantile(
      overruns.filter((value) => value != null),
      0.95
    ),
    overrunP99Ms: quantile(
      overruns.filter((value) => value != null),
      0.99
    ),
    slowFrames: durations.filter((value) => value > 50).length,
    vsyncMs: vsync,
    settleMs:
      lastFrameEnd > deliveryEnd ? (lastFrameEnd - deliveryEnd) / 1e6 : 0,
    textToFinishMs: Number.isFinite(finishEnd)
      ? Math.max(0, finishEnd - deliveryEnd) / 1e6
      : null,
    settleAfterFinishMs: Number.isFinite(finishEnd)
      ? Math.max(0, lastFrameEnd - finishEnd) / 1e6
      : null,
    // Open frames have unknown end times. Later app frames prove that the
    // fixed observation window cannot establish quiescence by itself.
    tailWindowCensored: raw.frames.some(
      ([ts, dur]) => ts >= start && (dur < 0 || (dur > 0 && ts + dur >= end))
    ),
    tail: {
      afterLastText: phaseAttribution(raw, deliveryEnd, end),
      beforeFinish: Number.isFinite(finishEnd)
        ? phaseAttribution(raw, deliveryEnd, finishEnd)
        : null,
      afterFinish: Number.isFinite(finishEnd)
        ? phaseAttribution(raw, finishEnd, end)
        : null,
    },
    memStartMiB: before / MiB,
    memPeakMiB: peak / MiB,
    memPeakIncreaseMiB: (peak - before) / MiB,
    memEndMiB: used(end) / MiB,
    memSamples: inside.length,
  };
}

/** CPU clipped to a phase; frame counts include frames crossing its start. */
export function phaseAttribution(raw, start, end) {
  const threads = new Map(raw.threads.map((thread) => [thread.utid, thread]));
  const cpu = { main: 0, js: 0, render: 0, other: 0 };
  const byThread = new Map();
  for (const [utid, ts, dur] of raw.sched) {
    const used = Math.max(0, Math.min(ts + dur, end) - Math.max(ts, start));
    if (dur <= 0 || !used) continue;
    const thread = threads.get(utid);
    cpu[threadGroup(thread, raw.pid)] += used;
    const name = thread?.name ?? `utid ${utid}`;
    byThread.set(name, (byThread.get(name) ?? 0) + used);
  }
  const frames =
    end > start
      ? raw.frames.filter(
          ([ts, dur]) => dur > 0 && ts < end && ts + dur > start
        )
      : [];
  return {
    windowMs: Math.max(0, end - start) / 1e6,
    cpuTotalS: Object.values(cpu).reduce((sum, value) => sum + value, 0) / 1e9,
    cpuMainS: cpu.main / 1e9,
    cpuJsS: cpu.js / 1e9,
    cpuRenderS: cpu.render / 1e9,
    cpuOtherS: cpu.other / 1e9,
    cpuByThreadS: Object.fromEntries(
      [...byThread]
        .sort((a, b) => b[1] - a[1])
        .map(([name, value]) => [name, value / 1e9])
    ),
    frames: frames.length,
    slowFrames: frames.filter(([, dur]) => dur > 50e6).length,
    appDeadlineMissedFrames: frames.filter(([, , jank]) =>
      /App Deadline Missed/i.test(jank ?? '')
    ).length,
    // Frame counts can overlap adjacent phases; CPU slices cannot.
    lastFrameAfterStartMs:
      frames.reduce((last, [ts, dur]) => Math.max(last, ts + dur - start), 0) /
      1e6,
  };
}

/** Opt-in native wall-time phases. Never join native and JS clock domains. */
export function nativePhaseSummary(trace) {
  const native = (trace.events ?? [])
    .filter(
      (event) =>
        event.clockDomain === 'native-monotonic' &&
        /^\d+$/.test(event.timeNs ?? '')
    )
    .map((event) => ({ ...event, at: BigInt(event.timeNs) }))
    .sort((a, b) => (a.at < b.at ? -1 : a.at > b.at ? 1 : 0));
  const specs = {
    queueWait: ['worker_queued', 'worker_start'],
    parse: ['parse_start', 'parse_end'],
    prepare: ['prepare_start', 'prepare_end'],
    retirement: ['retire_start', 'retire_end'],
    workerPass: ['worker_start', 'snapshot_staged'],
    publicationWait: ['snapshot_staged', 'snapshot_ready'],
    measure: ['measure_start', 'measure_end'],
    apply: ['apply_start', 'apply_end'],
  };
  const describe = (values) => ({
    count: values.length,
    totalMs: values.reduce((sum, value) => sum + value, 0),
    p50Ms: quantile(values, 0.5),
    p95Ms: quantile(values, 0.95),
    maxMs: values.length ? Math.max(...values) : null,
  });
  const finishBySession = new Map();
  const key = (event) => `${event.sessionId}:${event.generation}`;
  for (const event of native) {
    if (event.event === 'finish_received')
      finishBySession.set(key(event), event);
  }
  const phases = {};
  let unmatchedEvents = 0;
  for (const [name, [begin, end]] of Object.entries(specs)) {
    const pending = new Map();
    const durations = [];
    const afterFinish = [];
    for (const event of native) {
      if (event.event !== begin && event.event !== end) continue;
      // A session serializes workers. Dispatch and captured input sequences
      // can differ, so queue pairs use lifecycle order, not sequence equality.
      // Retirement and Fabric phases can overlap across worker/UI threads and
      // need the exact publication/snapshot sequence and emitting thread.
      const phaseKey = ['retirement', 'measure', 'apply'].includes(name)
        ? `${key(event)}:${event.sequence}:${event.threadId}`
        : key(event);
      const starts = pending.get(phaseKey) ?? [];
      if (event.event === begin) {
        starts.push(event);
        pending.set(phaseKey, starts);
      } else {
        const start = starts.shift();
        if (!start) {
          ++unmatchedEvents;
          continue;
        }
        durations.push(Number(event.at - start.at) / 1e6);
        const finish = finishBySession.get(key(event));
        if (finish && event.at > finish.at) {
          const after = start.at > finish.at ? start.at : finish.at;
          afterFinish.push(Number(event.at - after) / 1e6);
        }
      }
    }
    for (const starts of pending.values()) unmatchedEvents += starts.length;
    phases[name] = {
      all: describe(durations),
      afterFinish: describe(afterFinish),
    };
  }
  const finishToLastObservedReady = [];
  for (const [sessionKey, finish] of finishBySession) {
    const ready = native
      .filter(
        (event) =>
          key(event) === sessionKey &&
          event.event === 'snapshot_ready' &&
          event.sequence === finish.sequence &&
          event.at >= finish.at
      )
      .at(-1);
    if (ready)
      finishToLastObservedReady.push(Number(ready.at - finish.at) / 1e6);
    else ++unmatchedEvents;
  }
  return {
    clockDomain: 'native-monotonic',
    nativeEvents: native.length,
    intervalComplete: trace.intervalComplete === true,
    droppedEvents: trace.droppedEvents ?? 0,
    unmatchedEvents,
    phases,
    finishToLastObservedReady: describe(finishToLastObservedReady),
    notes:
      'Phase durations are elapsed wall time, not CPU. Summaries describe retained events; counts are lower bounds when events are dropped. Worker and publication pairs follow per-session/generation lifecycle order, including same-sequence finish. Retirement and Fabric phases also match sequence/thread. Retirement measures release of session-owned superseded snapshots and can run inside workerPass or as a standalone worker job; do not add overlapping phase totals. UI-held payload references can be released later, so retirement is not all-platform deallocation timing. Finish-to-ready uses the last observed same-sequence publication and cannot prove complete drain if events are dropped or the trace ends early.',
  };
}

function appMetrics(app) {
  const lateness = (app.deliveries ?? []).map(
    (delivery) => delivery.latenessMs
  );
  return {
    deliveryP95Ms: quantile(lateness, 0.95),
    deliveryMaxMs: lateness.length ? Math.max(...lateness) : null,
    rafGapP95Ms: quantile(
      (app.rafSamples ?? []).map((sample) => sample.gapMs),
      0.95
    ),
    commits: app.commits?.length ?? 0,
  };
}

function parseMeminfo(text) {
  const read = (label) => {
    const match = text.match(new RegExp(`^\\s*${label}:?\\s+(\\d+)`, 'm'));
    return match ? Number(match[1]) / 1024 : null;
  };
  return {
    pssMiB: read('TOTAL PSS') ?? read('TOTAL'),
    javaHeapMiB: read('Java Heap'),
    nativeHeapMiB: read('Native Heap'),
    graphicsMiB: read('Graphics'),
  };
}

// ---------------------------------------------------------------------------
// Report

const METRICS = [
  { key: 'cpuTotalS', label: 'App CPU', unit: 's', digits: 2, headline: true },
  {
    key: 'cpuMainS',
    label: 'UI thread CPU',
    unit: 's',
    digits: 2,
    headline: true,
  },
  { key: 'cpuJsS', label: 'JS thread CPU', unit: 's', digits: 2 },
  { key: 'cpuRenderS', label: 'RenderThread CPU', unit: 's', digits: 2 },
  { key: 'cpuOtherS', label: 'Other threads CPU', unit: 's', digits: 2 },
  {
    key: 'memPeakIncreaseMiB',
    label: 'Peak memory increase',
    unit: 'MiB',
    digits: 1,
    headline: true,
  },
  { key: 'memPeakMiB', label: 'Peak anonymous memory', unit: 'MiB', digits: 1 },
  { key: 'pssEndMiB', label: 'PSS after run', unit: 'MiB', digits: 1 },
  {
    key: 'jankAppPct',
    label: 'Janky frames (app deadline missed)',
    unit: '%',
    digits: 1,
    headline: true,
  },
  {
    key: 'jankAnyPct',
    label: 'Janky frames (any cause)',
    unit: '%',
    digits: 1,
  },
  {
    key: 'frameP95Ms',
    label: 'Frame time p95',
    unit: 'ms',
    digits: 1,
    headline: true,
  },
  {
    key: 'frameP99Ms',
    label: 'Frame time p99',
    unit: 'ms',
    digits: 1,
    headline: true,
  },
  { key: 'frameMaxMs', label: 'Longest frame', unit: 'ms', digits: 0 },
  { key: 'overrunP99Ms', label: 'Deadline overrun p99', unit: 'ms', digits: 1 },
  { key: 'slowFrames', label: 'Frames over 50 ms', unit: '', digits: 0 },
  { key: 'frames', label: 'Frames drawn', unit: '', digits: 0 },
  {
    key: 'settleMs',
    label: 'Settle after last chunk',
    unit: 'ms',
    digits: 0,
    headline: true,
  },
  {
    key: 'textToFinishMs',
    label: 'Last chunk to finish receipt',
    unit: 'ms',
    digits: 0,
  },
  {
    key: 'settleAfterFinishMs',
    label: 'Settle after finish receipt',
    unit: 'ms',
    digits: 0,
    headline: true,
  },
  {
    key: 'deliveryP95Ms',
    label: 'JS delivery lateness p95',
    unit: 'ms',
    digits: 1,
    headline: 'replay',
  },
  {
    key: 'textKiB',
    label: 'Text streamed',
    unit: 'KiB',
    digits: 1,
    headline: 'live',
  },
  {
    key: 'cpuMsPerKiB',
    label: 'CPU per KiB',
    unit: 'ms',
    digits: 0,
    headline: 'live',
  },
  { key: 'rafGapP95Ms', label: 'JS frame gap p95', unit: 'ms', digits: 1 },
];

const FEATURES = {
  columns: [
    'Downstream',
    'RN Streamdown',
    'Enriched',
    'Markdown Display',
    'Marked',
    'Markdown Renderer',
  ],
  rows: [
    [
      'Native text rendering',
      'yes',
      'yes',
      'yes',
      'no (RN Text tree)',
      'no (RN Text tree)',
      'no (RN Text tree)',
    ],
    [
      'Append-only stream API (append / finish)',
      'yes',
      'no',
      'no',
      'no',
      'no',
      'no',
    ],
    [
      'Reuses finished blocks while streaming',
      'yes',
      'no, full re-parse',
      'no, full re-parse',
      'no',
      'no',
      'no',
    ],
    [
      'Repairs unfinished Markdown mid-stream',
      'yes, per syntax',
      'yes (remend)',
      'no',
      'no',
      'no',
      'no',
    ],
    ['Streaming reveal animation', 'yes', 'yes', 'yes', 'no', 'no', 'no'],
    ['Streaming caret', 'yes', 'no', 'no', 'no', 'no', 'no'],
    [
      'Syntax highlighting',
      'yes, built in',
      'yes, build-time grammars',
      'yes, build-time grammars',
      'no',
      'custom renderer',
      'no',
    ],
    [
      'Code block line numbers, filename, download, wrap, expand',
      'yes',
      'copy only',
      'copy only',
      'no',
      'no',
      'no',
    ],
    ['Table copy, export and full-screen', 'yes', 'no', 'no', 'no', 'no', 'no'],
    ['Footnotes', 'yes', 'no', 'no', 'no', 'no', 'no'],
    ['Alerts / admonitions', 'yes', 'yes', 'yes', 'no', 'no', 'no'],
    ['Math', 'optional package', 'yes', 'yes', 'no', 'custom tokenizer', 'no'],
    ['Collapsible <details>', 'yes', 'no', 'no', 'no', 'no', 'no'],
    [
      'Your own React components in fences and tags',
      'yes',
      'no',
      'no',
      'per-node overrides',
      'per-node overrides',
      'per-node overrides',
    ],
  ],
  notes: [
    'Enriched Markdown has streaming display options (`streamingAnimation`, table and code-block modes) but no append API: every update passes the whole string and md4c re-parses it, and unfinished syntax is not repaired. Its docs point to RN Streamdown for that.',
    'RN Streamdown runs remend on a worklet thread, then hands the whole repaired string to Enriched on every update.',
    'Checked against the installed packages: react-native-streamdown 0.3.0, react-native-enriched-markdown 1.1.1, react-native-markdown-display 7.0.2, react-native-marked 8.3.2, react-native-markdown-renderer 4.1.1.',
  ],
};

function summarize(runs, seed = 1) {
  const random = mulberry32(seed);
  const groups = new Map();
  for (const run of runs) {
    const key = `${run.config.profile}/${run.config.workload}/${run.config.candidate}`;
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key).push(run);
  }
  const summary = {};
  for (const [key, members] of groups) {
    const metrics = {};
    for (const { key: metric } of METRICS) {
      const values = members
        .map((run) => run.metrics[metric])
        .filter(Number.isFinite);
      metrics[metric] = {
        n: values.length,
        median: median(values),
        ci95: bootstrap(values, median, random),
        p25: quantile(values, 0.25),
        p75: quantile(values, 0.75),
        min: values.length ? Math.min(...values) : null,
        max: values.length ? Math.max(...values) : null,
        values,
      };
    }
    summary[key] = { runs: members.length, metrics };
  }
  const comparisons = {};
  for (const key of Object.keys(summary)) {
    const [profile, workload, candidate] = key.split('/');
    const base = summary[`${profile}/${workload}/${BASELINE}`];
    if (!base || candidate === BASELINE) continue;
    comparisons[key] = {};
    for (const { key: metric } of METRICS) {
      const a = base.metrics[metric].values;
      const b = summary[key].metrics[metric].values;
      const ma = median(a);
      const mb = median(b);
      comparisons[key][metric] = {
        ratio: ma > 0 ? mb / ma : null,
        ci95: ratioInterval(a, b, random),
        mannWhitney: mannWhitney(a, b),
      };
    }
  }
  return { groups: summary, comparisons };
}

const fmt = (value, digits) =>
  value == null || !Number.isFinite(value) ? '–' : value.toFixed(digits);

function renderReport(session, runs, stats) {
  if (benchmarkSuite?.renderReport)
    return benchmarkSuite.renderReport(session, runs, stats);
  const lines = [];
  const device = session.device;
  const measured = runs.filter((run) => !run.config.warmup);
  lines.push('# Phone streaming benchmark');
  lines.push('');
  lines.push(
    `${device.model} · Android ${device.release} · ${session.vsyncHz ?? '?'} Hz · arm64 Hermes release · ${session.startedAt.slice(0, 10)}. ` +
      `${measured.length} measured runs, ${runs.length - measured.length} warmups excluded. ` +
      `Every number is the median of ${session.repeats} cold-process runs, with its 95% bootstrap confidence interval in brackets. Lower is better everywhere.`
  );
  lines.push('');
  if (session.diagnosticTrace) {
    lines.push(
      'Diagnostic native tracing is enabled. These timings include tracing overhead and should not be compared to trace-off performance measurements. Native phase summaries and bounded raw events are saved with each run. Retirement separately measures release of session-owned superseded snapshots on a worker; UI-held payload references may be released later, so it does not prove that all platform payloads were deallocated off the UI thread.'
    );
    lines.push('');
  }
  if (session.input.mode === 'replay') {
    lines.push(
      `Each thread receives its finish marker ${session.finishDelayMs ?? 0} ms after its last delivered chunk. Other threads continue on the original replay schedule. Aggregate done follows all finish markers.`
    );
    lines.push('');
  }
  lines.push('## Workloads');
  lines.push('');
  const input = session.input;
  if (input.mode === 'live') {
    lines.push(
      '| Workload | What streams | Replies at once | Runs per library |'
    );
    lines.push('|---|---|---:|---:|');
    for (const [id, workload] of Object.entries(session.workloads)) {
      lines.push(
        `| ${id} | ${WORKLOADS[id]?.label ?? id} | ${workload.prompts} | ${workload.repeats ?? session.repeats} |`
      );
    }
    lines.push('');
    lines.push(
      'Huge workloads run once per library, so their rows have no confidence interval or significance test.'
    );
    lines.push('');
    lines.push(
      `Live: every run asks \`${input.model}\` (reasoning ${input.reasoningEffort}) for fresh replies through the Vercel AI SDK \`streamText\` on the Mac, and each text delta reaches the phone over USB the moment it arrives. Replies differ from run to run, so "CPU per KiB" normalizes CPU by the text each run actually streamed.`
    );
  } else {
    lines.push('| Workload | What streams | Deltas | Duration |');
    lines.push('|---|---|---:|---:|');
    for (const [id, workload] of Object.entries(session.workloads)) {
      lines.push(
        `| ${id} | ${WORKLOADS[id]?.label ?? id} | ${workload.chunks} | ${(workload.lastDueMs / 1000).toFixed(1)} s |`
      );
    }
    lines.push('');
    lines.push(
      `Replay: responses recorded from \`${input.model}\` (reasoning ${input.reasoningEffort}) through the Vercel AI SDK \`streamText\`. Each library gets the same deltas at the same recorded times.`
    );
  }
  for (const profile of session.profiles) {
    lines.push('');
    lines.push(
      `## ${profileConfig(profile).syntaxHighlighting ? 'Code colors on (library defaults)' : 'Code colors off'}${profileConfig(profile).animated ? ', default streaming animations on' : ', streaming animations off'}`
    );
    for (const workload of session.workloadIds) {
      lines.push('');
      lines.push(`### ${workload}: ${WORKLOADS[workload]?.label ?? ''}`);
      lines.push('');
      const censored = measured.filter(
        (run) =>
          run.config.profile === profile &&
          run.config.workload === workload &&
          run.metrics.tailWindowCensored
      ).length;
      if (censored) {
        lines.push(
          `${censored} run(s) have unfinished frames or app drawing at/after the observation boundary. Their measured settle values cannot prove completion; inspect the retained traces and repeat with a longer window.`
        );
        lines.push('');
      }
      const headline = METRICS.filter(
        (metric) =>
          metric.headline === true || metric.headline === session.input.mode
      );
      lines.push(
        `| Library | ${headline.map((metric) => `${metric.label}${metric.unit ? ` (${metric.unit})` : ''}`).join(' | ')} |`
      );
      lines.push(`|---|${headline.map(() => '---:').join('|')}|`);
      for (const candidate of session.candidates) {
        const group = stats.groups[`${profile}/${workload}/${candidate}`];
        if (!group) continue;
        const cells = headline.map(({ key, digits }) => {
          const metric = group.metrics[key];
          const ci = metric.ci95
            ? ` [${fmt(metric.ci95[0], digits)}–${fmt(metric.ci95[1], digits)}]`
            : '';
          return `${fmt(metric.median, digits)}${ci}`;
        });
        const name =
          candidate === BASELINE
            ? `**${CANDIDATES[candidate]}**`
            : CANDIDATES[candidate];
        lines.push(`| ${name} | ${cells.join(' | ')} |`);
      }
      lines.push('');
      lines.push(
        `Relative to Downstream (× its median; 95% CI; * = Mann-Whitney p < 0.05):`
      );
      lines.push('');
      const compared = [
        'cpuTotalS',
        'cpuMainS',
        'memPeakIncreaseMiB',
        'jankAppPct',
        'frameP99Ms',
      ];
      lines.push(
        `| Library | ${compared.map((key) => METRICS.find((metric) => metric.key === key).label).join(' | ')} |`
      );
      lines.push(`|---|${compared.map(() => '---:').join('|')}|`);
      for (const candidate of session.candidates) {
        const comparison =
          stats.comparisons[`${profile}/${workload}/${candidate}`];
        if (!comparison) continue;
        const cells = compared.map((key) => {
          const { ratio, ci95, mannWhitney: test } = comparison[key];
          if (ratio == null) return '–';
          const star = test && test.p < 0.05 ? '*' : '';
          return `${fmt(ratio, 2)}×${star}${ci95 ? ` [${fmt(ci95[0], 2)}–${fmt(ci95[1], 2)}]` : ''}`;
        });
        lines.push(`| ${CANDIDATES[candidate]} | ${cells.join(' | ')} |`);
      }
    }
  }
  lines.push('');
  lines.push('## Features');
  lines.push('');
  lines.push(`| Feature | ${FEATURES.columns.join(' | ')} |`);
  lines.push(`|---|${FEATURES.columns.map(() => ':---:').join('|')}|`);
  for (const [feature, ...cells] of FEATURES.rows)
    lines.push(`| ${feature} | ${cells.join(' | ')} |`);
  lines.push('');
  for (const note of FEATURES.notes) lines.push(`- ${note}`);
  lines.push('');
  lines.push('## How it was measured');
  lines.push('');
  lines.push(
    [
      session.input.mode === 'live'
        ? '- **Live input.** One release APK holds every library. Each run is a cold process showing one library full screen (five stacked panes for the concurrent workload, all visible). The Mac streams fresh model replies and forwards each delta over USB as it arrives; the app appends it immediately and the harness checks that the phone rendered exactly the text the model streamed. The window starts at the first delta, so model thinking time is not measured.'
        : '- **Same input.** One release APK holds every library. Each run is a cold process showing one library full screen (five stacked panes for the concurrent workload, all visible). The host serves the recorded deltas; the app delivers them on their recorded schedule and checks that the text it delivered equals the recording.',
      '- **Feature coverage.** New captures and live runs request all eight subsets of math, code and tables, including every ordering, in one 16-section reply. Existing replies also mix nested formatting, lists, tasks, links, alerts, footnotes and Unicode. Huge replies rotate the six three-feature orderings. This covers content combinations, not every renderer option. Model output can omit requested constructs; inspect the recorded or live text before claiming complete coverage. Older captures retain their original content and must be recaptured for this matrix.',
      '- **Math.** Math parsing is enabled for the matrix, concurrent and huge workloads. Downstream uses its installed math package; Streamdown and Enriched use their native math engines. Other libraries receive the same source with their default handling. Unsupported features can remain literal, so rendering and performance are not equivalent.',
      '- **Frames.** Perfetto records SurfaceFlinger FrameTimeline, which classifies every frame the app draws. "Janky (app deadline missed)" is the share of frames Android itself blames on the app; "any cause" adds SurfaceFlinger, display and prediction misses but not buffer or SurfaceFlinger stuffing (queueing latency, not a visible stutter). Frame time is FrameTimeline\'s actual frame duration (CPU start to GPU done or post). Deadline overrun is actual end minus expected end, as in Jetpack Macrobenchmark.',
      '- **CPU.** Scheduler slices for every thread of the app process inside the measured window, split into the UI thread, the JS thread (mqt_js), RenderThread and everything else (native workers, GC, worklets). This is exact on-CPU time, not sampling.',
      '- **Memory.** Anonymous RSS plus swap, polled by Perfetto every 100 ms. "Peak increase" is the highest value during the window minus the value when streaming started. PSS from `dumpsys meminfo` is read after the window so it cannot disturb it.',
      '- **Settle.** Time from the last delivered chunk to the last completed frame the app drew, from the trace. New runs also split last text to app finish receipt from finish receipt to last frame. Finish receipt is separate from host done, which can wait for provider usage metadata. Raw runs contain per-thread boundaries and scheduler/frame attribution before and after finish. Missing boundaries in older runs remain unknown.',
      "- **Animation profiles.** `highlighted` and `plain` disable streaming animations. `highlighted-animated` and `plain-animated` enable each supported library's default effect and include only Downstream, Streamdown and Enriched. These effects differ: Enriched also changes incomplete block presentation and defers code highlighting. Compare profiles with the same replay recording; live replies differ. The 150 ms renderer-tail target applies to animation-off runs. Open frames or app drawing at/after the window conservatively mark the tail as censored. Later frames can include benchmark activity; inspect the retained trace before attributing them to the renderer. Use `--settle-ms` to extend the same observation budget for all profiles.",
      '- **JS lateness.** How late the app delivered each chunk against its schedule, a proxy for a busy JS thread.',
      `- **Window.** From the moment streaming starts until ${session.settleMs / 1000} s after stream done is received (after the final chunk in replay runs). App start-up and mounting are outside it.`,
      `- **Order and noise.** ${session.repeats} measured blocks plus one warmup block; every block runs each library and workload once in a seeded random order (seed ${session.seed}). Before each run the app is force-stopped, background apps are killed and the harness waits until the thermal status is 0 and the battery is at most ${session.thermal.maxBatteryC} °C. App code is AOT-compiled (\`cmd package compile -m speed\`). The screen stays on at fixed brightness${session.deviceSetup ? ' with the refresh rate pinned' : ''}.`,
      '- **Statistics.** Medians of per-run values, percentile bootstrap 95% intervals (4,000 resamples). Ratios are bootstrapped from both groups. Mann-Whitney U is two-sided with a normal approximation; no multiple-comparison correction, so read single stars with care.',
      '- **Limits.** One phone model. The common app contains every library, so memory is not a per-package size. Clock speeds are not locked (needs root). Markdown output differs between libraries (see Features); equal input is not equal rendering.',
    ].join('\n')
  );
  lines.push('');
  lines.push(
    `Raw runs, traces and the full statistics are in \`${relative(HERE, session.output)}\` (summary.json). Build: \`${session.builds.map((record) => `${record.profile} ${record.apkSha256.slice(0, 12)}`).join(', ')}\`, revision \`${session.builds[0]?.revision.slice(0, 10)}\`${session.builds[0]?.dirty ? ' with local changes' : ''}.`
  );
  return lines.join('\n') + '\n';
}

// ---------------------------------------------------------------------------
// Process helpers

const sha = (value) => createHash('sha256').update(value).digest('hex');
const sleep = (ms) => new Promise((done) => setTimeout(done, ms));
const log = (...parts) =>
  console.log(`[${new Date().toISOString().slice(11, 19)}]`, ...parts);

function runCommand(
  command,
  args,
  { cwd = ROOT, env, input, allowFailure = false, timeout = 0 } = {}
) {
  return new Promise((done, fail) => {
    const child = spawn(command, args, {
      cwd,
      env: env ?? process.env,
      stdio: ['pipe', 'pipe', 'pipe'],
    });
    const out = [];
    const err = [];
    let timer;
    if (timeout) timer = setTimeout(() => child.kill('SIGKILL'), timeout);
    child.stdout.on('data', (chunk) => out.push(chunk));
    child.stderr.on('data', (chunk) => err.push(chunk));
    child.on('error', fail);
    child.on('close', (code) => {
      clearTimeout(timer);
      const result = {
        code,
        stdout: Buffer.concat(out).toString(),
        stderr: Buffer.concat(err).toString(),
      };
      if (code !== 0 && !allowFailure) {
        fail(
          new Error(
            `${command} ${args.join(' ')} exited ${code}: ${result.stderr.slice(0, 2000)}`
          )
        );
      } else done(result);
    });
    child.stdin.end(input ?? '');
  });
}

function runVisible(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: ROOT,
    stdio: 'inherit',
    ...options,
  });
  if (result.status !== 0)
    throw new Error(`${command} ${args.join(' ')} exited ${result.status}`);
}

class Device {
  constructor(serial) {
    this.serial = serial;
  }
  adb(args, options) {
    return runCommand('adb', ['-s', this.serial, ...args], options);
  }
  async shell(command, options) {
    return (await this.adb(['shell', command], options)).stdout.replace(
      /\r/g,
      ''
    );
  }
  async setting(namespace, key) {
    const value = (await this.shell(`settings get ${namespace} ${key}`)).trim();
    return value === 'null' ? null : value;
  }
  async installedSha256() {
    const path = (await this.shell(`pm path ${PACKAGE}`))
      .split('\n')
      .find((line) => line.endsWith('base.apk'));
    if (!path) throw new Error('Benchmark app is not installed');
    return (
      await this.shell(`sha256sum ${path.replace('package:', '')}`)
    ).split(/\s+/)[0];
  }
  async thermal() {
    const thermal = await this.shell('dumpsys thermalservice');
    const battery = await this.shell('dumpsys battery');
    return {
      status: Number(
        thermal.match(/Current Thermal Status:\s*(\d+)/)?.[1] ?? NaN
      ),
      batteryC: Number(battery.match(/temperature:\s*(\d+)/)?.[1] ?? NaN) / 10,
      batteryLevel: Number(battery.match(/level:\s*(\d+)/)?.[1] ?? NaN),
    };
  }
}

async function pickDevice(requested) {
  const { stdout } = await runCommand('adb', ['devices']);
  const serials = stdout
    .split('\n')
    .slice(1)
    .map((line) => line.trim().split(/\s+/))
    .filter(([serial, state]) => serial && state === 'device')
    .map(([serial]) => serial);
  if (requested) {
    if (!serials.includes(requested))
      throw new Error(
        `Device ${requested} is not connected (adb devices: ${serials.join(', ') || 'none'})`
      );
    return new Device(requested);
  }
  if (serials.length !== 1)
    throw new Error(
      `Connect exactly one phone or pass --device (found: ${serials.join(', ') || 'none'})`
    );
  return new Device(serials[0]);
}

// ---------------------------------------------------------------------------
// capture: real AI responses through the Vercel AI SDK

// Room for medium reasoning plus a 60-point reply; a limit, not a target.
const MAX_OUTPUT_TOKENS = 16000;

/**
 * Version of a package the example app depends on. Read from disk because some
 * packages (remend) do not export their package.json to require().
 */
function exampleVersion(name) {
  return JSON.parse(
    readFileSync(join(EXAMPLE, 'node_modules', name, 'package.json'), 'utf8')
  ).version;
}

/** The Vercel AI SDK from the example app, with the key from example/.env. */
export async function loadModel(options) {
  const requireExample = createRequire(join(EXAMPLE, 'package.json'));
  const load = (name) =>
    import(pathToFileURL(requireExample.resolve(name)).href);
  const { streamText } = await load('ai');
  const { createOpenAI } = await load('@ai-sdk/openai');
  try {
    process.loadEnvFile(join(EXAMPLE, '.env'));
  } catch {}
  const apiKey =
    process.env.OPENAI_API_KEY || process.env.EXPO_PUBLIC_OPENAI_API_KEY;
  if (!apiKey)
    throw new Error('Set EXPO_PUBLIC_OPENAI_API_KEY in example/.env');
  return {
    streamText,
    model: createOpenAI({ apiKey })(options.model),
    modelId: options.model,
    reasoningEffort: options.reasoning,
    client: `ai ${exampleVersion('ai')} streamText · @ai-sdk/openai ${exampleVersion('@ai-sdk/openai')}`,
  };
}

/** JSON with every non-ASCII character escaped, so one byte is one character. */
export function asciiJson(value) {
  return JSON.stringify(value).replace(
    /[\u007f-\uffff]/g,
    (character) => `\\u${character.charCodeAt(0).toString(16).padStart(4, '0')}`
  );
}

/**
 * Requests every prompt at once and writes each text delta to the phone the
 * moment the model produces it, one NDJSON line per delta. Nothing is buffered.
 */
export async function relayLive(ai, prompts, response) {
  response.writeHead(200, {
    'Content-Type': 'application/x-ndjson',
    'Cache-Control': 'no-store',
  });
  response.socket?.setNoDelay(true);
  const write = (message) => response.write(`${asciiJson(message)}\n`);
  const abort = new AbortController();
  const started = performance.now();
  try {
    const threads = await Promise.all(
      prompts.map(async (prompt, thread) => {
        const failures = [];
        const result = ai.streamText({
          model: ai.model,
          prompt,
          maxOutputTokens: MAX_OUTPUT_TOKENS,
          abortSignal: abort.signal,
          providerOptions: {
            openai: { reasoningEffort: ai.reasoningEffort, store: false },
          },
          onError: ({ error }) => failures.push(error),
        });
        let text = '';
        const arrivals = [];
        for await (const delta of result.textStream) {
          if (!delta) continue;
          write({ thread, text: delta });
          text += delta;
          arrivals.push(performance.now() - started);
        }
        if (failures.length) throw failures[0];
        write({ thread, finished: true });
        const [usage, finishReason] = await Promise.all([
          result.totalUsage,
          result.finishReason,
        ]);
        return {
          thread,
          text,
          sha256: sha(text),
          bytes: Buffer.byteLength(text),
          deltas: arrivals.length,
          firstDeltaMs: arrivals[0] ?? null,
          lastDeltaMs: arrivals.at(-1) ?? null,
          finishReason,
          usage,
        };
      })
    );
    write({ done: true });
    return threads;
  } catch (error) {
    abort.abort();
    write({ error: String(error?.message ?? error) });
    throw error;
  } finally {
    response.end();
  }
}

async function capture(options) {
  const outputDir = resolve(
    options.replay ?? join(HERE, 'recordings', 'luna-medium')
  );
  if (existsSync(outputDir))
    throw new Error(`${outputDir} already exists; pass a new --replay folder`);
  const ai = await loadModel(options);
  const { streamText, client } = ai;
  const model = ai.modelId;
  const reasoningEffort = ai.reasoningEffort;
  log(
    `Requesting ${PROMPTS.length} responses at once from ${model} (reasoning ${reasoningEffort})`
  );
  const started = performance.now();
  const recordings = await Promise.all(
    PROMPTS.map(async (prompt, index) => {
      const chunks = [];
      const failures = [];
      const result = streamText({
        model: ai.model,
        prompt,
        maxOutputTokens: MAX_OUTPUT_TOKENS,
        providerOptions: { openai: { reasoningEffort, store: false } },
        onChunk: ({ chunk }) => {
          if (chunk.type === 'text-delta' && chunk.text)
            chunks.push({
              text: chunk.text,
              dueMs: performance.now() - started,
            });
        },
        onError: ({ error }) => failures.push(error),
      });
      await result.consumeStream();
      if (failures.length) throw failures[0];
      const [text, usage, finishReason, response] = await Promise.all([
        result.text,
        result.usage,
        result.finishReason,
        result.response,
      ]);
      const source = chunks.map((chunk) => chunk.text).join('');
      if (finishReason !== 'stop')
        throw new Error(`Response ${index} finished with ${finishReason}`);
      if (!chunks.length || source !== text)
        throw new Error(`Response ${index}: deltas differ from the final text`);
      const firstArrivalMs = chunks[0].dueMs;
      for (const chunk of chunks) chunk.dueMs -= firstArrivalMs;
      return {
        schemaVersion: 1,
        createdAt: new Date().toISOString(),
        client,
        prompt,
        model: response.modelId ?? model,
        request: {
          model,
          reasoningEffort,
          maxOutputTokens: MAX_OUTPUT_TOKENS,
          concurrentRequests: PROMPTS.length,
        },
        firstArrivalMs,
        chunks,
        source,
        sha256: sha(source),
        usage,
        responseStatus: finishReason,
      };
    })
  );
  mkdirSync(outputDir, { recursive: true });
  recordings.forEach((recording, index) => {
    writeFileSync(
      join(outputDir, `response-${index}.json`),
      JSON.stringify(recording, null, 2) + '\n',
      { flag: 'wx' }
    );
    log(
      `response-${index}: ${Buffer.byteLength(recording.source)} bytes, ${recording.chunks.length} deltas, ${(recording.chunks.at(-1).dueMs / 1000).toFixed(1)} s`
    );
  });
}

// ---------------------------------------------------------------------------
// build: release APK with only the benchmark entry and no credentials

async function build(profile, outputDir) {
  log(`Building the ${profile} APK`);
  const packagePath = join(EXAMPLE, 'package.json');
  const originalPackage = readFileSync(packagePath, 'utf8');
  try {
    // Enriched Markdown compiles its highlighter in or out at build time.
    const pkg = JSON.parse(originalPackage);
    pkg['enriched-markdown'] = {
      enableCodeHighlight: profileConfig(profile).syntaxHighlighting,
      enableMath: true,
      codeHighlightLanguages: ['javascript', 'typescript', 'python', 'json'],
    };
    writeFileSync(packagePath, JSON.stringify(pkg, null, 2) + '\n');
    runVisible(
      'node',
      ['example/node_modules/react-native-enriched-markdown/postinstall.mjs'],
      {
        env: { ...process.env, INIT_CWD: EXAMPLE },
      }
    );
    const env = {
      ...process.env,
      EXPO_NO_DOTENV: '1',
      EXPO_PUBLIC_PHONE_BENCHMARK: '1',
      EXPO_PUBLIC_DOWNSTREAM_REVISION: spawnSync('git', ['rev-parse', 'HEAD'], {
        cwd: ROOT,
      })
        .stdout.toString()
        .trim(),
    };
    delete env.EXPO_PUBLIC_OPENAI_API_KEY;
    delete env.OPENAI_API_KEY;
    // The example's own expo binary, so the build never waits on Corepack.
    runVisible(
      join(EXAMPLE, 'node_modules/.bin/expo'),
      ['prebuild', '--platform', 'android', '--no-install'],
      { cwd: EXAMPLE, env }
    );
    // Cleartext only to the phone's own loopback, which adb reverse maps to this Mac.
    const manifestPath = join(
      EXAMPLE,
      'android/app/src/main/AndroidManifest.xml'
    );
    const manifest = readFileSync(manifestPath, 'utf8');
    if (!manifest.includes('phone_benchmark_network_security')) {
      writeFileSync(
        manifestPath,
        manifest.replace(
          '<application ',
          '<application android:networkSecurityConfig="@xml/phone_benchmark_network_security" '
        )
      );
    }
    const xml = join(
      EXAMPLE,
      'android/app/src/main/res/xml/phone_benchmark_network_security.xml'
    );
    mkdirSync(dirname(xml), { recursive: true });
    writeFileSync(
      xml,
      '<network-security-config><base-config cleartextTrafficPermitted="false"/><domain-config cleartextTrafficPermitted="true"><domain includeSubdomains="false">127.0.0.1</domain><domain includeSubdomains="false">localhost</domain></domain-config></network-security-config>'
    );
    runVisible(
      './gradlew',
      [
        ':app:assembleRelease',
        '-PreactNativeArchitectures=arm64-v8a',
        '--console=plain',
        '-q',
      ],
      {
        cwd: join(EXAMPLE, 'android'),
        env,
      }
    );
  } finally {
    writeFileSync(packagePath, originalPackage);
  }
  // The APK must never carry the API key the chat example uses.
  try {
    process.loadEnvFile(join(EXAMPLE, '.env'));
  } catch {}
  const key = process.env.EXPO_PUBLIC_OPENAI_API_KEY;
  const bundle = spawnSync(
    'unzip',
    ['-p', APK, 'assets/index.android.bundle'],
    { maxBuffer: 1024 * 1024 * 1024 }
  ).stdout;
  if (!bundle?.length)
    throw new Error('Could not read the JS bundle from the APK');
  if (key && bundle.includes(key))
    throw new Error('Credential found in the benchmark APK');
  if (!bundle.includes('phone-benchmark-root'))
    throw new Error('APK does not contain the benchmark entry');

  const git = (...args) =>
    spawnSync('git', args, { cwd: ROOT }).stdout.toString();
  const versions = Object.fromEntries(
    [
      'react-native',
      'react',
      'react-native-streamdown',
      'react-native-enriched-markdown',
      'react-native-worklets',
      'react-native-markdown-display',
      'react-native-marked',
      'react-native-markdown-renderer',
      'remend',
      ...(benchmarkSuite
        ? [
            'react-native-downstream',
            'react-native-downstream-code',
            'react-native-downstream-math',
          ]
        : []),
    ].map((name) => [name, exampleVersion(name)])
  );
  const apk = readFileSync(APK);
  const record = {
    profile,
    builtAt: new Date().toISOString(),
    revision: git('rev-parse', 'HEAD').trim(),
    dirty: git('status', '--porcelain').trim().length > 0,
    diffSha256: sha(git('diff', 'HEAD')),
    apkSha256: sha(apk),
    apkBytes: apk.length,
    packages: versions,
    credentialsAbsent: true,
    ...(benchmarkSuite && {
      benchmarkVersion: benchmarkSuite.id,
      suiteFingerprint: suiteFingerprint(),
      profileConfig: profileConfig(profile),
      lockfileSha256: sha(readFileSync(join(ROOT, 'pnpm-lock.yaml'))),
      nodeVersion: process.version,
    }),
  };
  mkdirSync(join(outputDir, 'builds'), { recursive: true });
  copyFileSync(APK, join(outputDir, 'builds', `${profile}.apk`));
  writeFileSync(
    join(outputDir, 'builds', `${profile}.json`),
    JSON.stringify(record, null, 2) + '\n'
  );
  log(
    `Built ${profile}: ${record.apkSha256.slice(0, 16)} (${(apk.length / 1048576).toFixed(1)} MiB)`
  );
  return record;
}

// ---------------------------------------------------------------------------
// Perfetto

const PERFETTO_CONFIG = `
buffers { size_kb: 98304 fill_policy: DISCARD }
buffers { size_kb: 16384 fill_policy: DISCARD }
data_sources { config { name: "android.surfaceflinger.frametimeline" target_buffer: 1 } }
data_sources { config { name: "linux.process_stats" target_buffer: 1 process_stats_config {
  scan_all_processes_on_start: true proc_stats_poll_ms: 100 } } }
data_sources { config { name: "linux.ftrace" target_buffer: 0 ftrace_config {
  ftrace_events: "sched/sched_switch" ftrace_events: "sched/sched_process_exit"
  ftrace_events: "sched/sched_process_free" ftrace_events: "task/task_newtask"
  ftrace_events: "task/task_rename" ftrace_events: "power/suspend_resume"
  ftrace_events: "power/cpu_frequency" } } }
duration_ms: 900000
write_into_file: true
file_write_period_ms: 1000
`;

async function ensureTraceProcessor() {
  if (
    existsSync(TRACE_PROCESSOR.path) &&
    sha(readFileSync(TRACE_PROCESSOR.path)) === TRACE_PROCESSOR.sha256
  )
    return;
  if (process.platform !== 'darwin' || process.arch !== 'arm64') {
    throw new Error(
      `Place Perfetto ${TRACE_PROCESSOR.version} trace_processor_shell at ${TRACE_PROCESSOR.path}`
    );
  }
  log(`Downloading Perfetto trace_processor ${TRACE_PROCESSOR.version}`);
  const response = await fetch(TRACE_PROCESSOR.url);
  if (!response.ok)
    throw new Error(`trace_processor download failed: HTTP ${response.status}`);
  const binary = Buffer.from(await response.arrayBuffer());
  if (sha(binary) !== TRACE_PROCESSOR.sha256)
    throw new Error('trace_processor checksum mismatch');
  mkdirSync(dirname(TRACE_PROCESSOR.path), { recursive: true });
  writeFileSync(TRACE_PROCESSOR.path, binary, { mode: 0o755 });
}

async function startTrace(device) {
  await device.shell(`rm -f ${DEVICE_TRACE}`);
  // Configs must arrive on stdin: SELinux stops perfetto reading app-writable files.
  const output = await device.shell(
    `perfetto --txt -c - -o ${DEVICE_TRACE} --background-wait`,
    {
      input: PERFETTO_CONFIG,
    }
  );
  const pid = output.trim().split(/\s+/).pop();
  if (!/^\d+$/.test(pid)) throw new Error(`perfetto did not start: ${output}`);
  return pid;
}

async function stopTrace(device, pid, localPath) {
  await device.shell(`kill -TERM ${pid}`, { allowFailure: true });
  for (let attempt = 0; attempt < 100; attempt += 1) {
    const alive = await device.adb(['shell', `kill -0 ${pid}`], {
      allowFailure: true,
    });
    if (alive.code !== 0) break;
    await sleep(100);
  }
  await device.adb(['pull', DEVICE_TRACE, localPath]);
  await device.shell(`rm -f ${DEVICE_TRACE}`);
}

/** One trace_processor pass that returns every raw row the metrics need. */
export async function queryTrace(tracePath, pid) {
  const query = `
    INCLUDE PERFETTO MODULE android.frames.timeline;
    SELECT '@@JSON@@' || json_object(
      -- Microseconds keep the wall-clock offset exact in a JS number.
      'clockOffsetUs', (SELECT (r.clock_value - b.clock_value) / 1000 FROM clock_snapshot r
                        JOIN clock_snapshot b ON r.snapshot_id = b.snapshot_id
                        WHERE r.clock_name = 'REALTIME' AND b.clock_name = 'BOOTTIME' ORDER BY r.ts LIMIT 1),
      'upid', (SELECT upid FROM process WHERE pid = ${pid} ORDER BY start_ts DESC LIMIT 1),
      'threads', (SELECT json_group_array(json_object('utid', utid, 'tid', tid, 'name', name)) FROM thread
                  WHERE upid = (SELECT upid FROM process WHERE pid = ${pid} ORDER BY start_ts DESC LIMIT 1)),
      'sched', (SELECT json_group_array(json_array(s.utid, s.ts, s.dur)) FROM sched_slice s JOIN thread t USING (utid)
                WHERE t.upid = (SELECT upid FROM process WHERE pid = ${pid} ORDER BY start_ts DESC LIMIT 1) AND s.dur > 0),
      'frames', (SELECT json_group_array(json_array(a.ts, a.dur, a.jank_type, a.present_type, e.ts, e.dur, a.layer_name))
                 FROM actual_frame_timeline_slice a
                 LEFT JOIN expected_frame_timeline_slice e ON e.name = a.name AND e.upid = a.upid
                 WHERE a.upid = (SELECT upid FROM process WHERE pid = ${pid} ORDER BY start_ts DESC LIMIT 1)
                 ORDER BY a.ts),
      'memory', (SELECT json_group_array(json_array(t.name, c.ts, c.value)) FROM counter c
                 JOIN process_counter_track t ON c.track_id = t.id
                 WHERE t.upid = (SELECT upid FROM process WHERE pid = ${pid} ORDER BY start_ts DESC LIMIT 1)
                   AND t.name IN ('mem.rss.anon', 'mem.swap', 'mem.rss.file', 'mem.rss')
                 ORDER BY c.ts)
    ) || '@@END@@' AS result;`;
  const queryFile = `${tracePath}.sql`;
  writeFileSync(queryFile, query);
  const { stdout } = await runCommand(
    TRACE_PROCESSOR.path,
    ['--query-file', queryFile, tracePath],
    { timeout: 120000 }
  );
  rmSync(queryFile);
  // The shell prints cells without escaping their quotes, so the JSON is cut
  // out between markers rather than parsed as CSV.
  const begin = stdout.indexOf('@@JSON@@');
  const finish = stdout.lastIndexOf('@@END@@');
  if (begin < 0 || finish < begin)
    throw new Error(
      `trace_processor returned no result: ${stdout.slice(0, 500)}`
    );
  const raw = JSON.parse(stdout.slice(begin + '@@JSON@@'.length, finish));
  for (const key of ['threads', 'sched', 'frames', 'memory']) {
    raw[key] =
      typeof raw[key] === 'string' ? JSON.parse(raw[key]) : (raw[key] ?? []);
  }
  raw.pid = Number(pid);
  return raw;
}

/** Wall-clock time of the app's last delivered chunk. */
export function lastDeliveryWall(result, startWall) {
  const exact = Math.max(
    ...(result.deliveries ?? [])
      .map((delivery) => delivery.wallTimeMs)
      .filter(Number.isFinite)
  );
  if (Number.isFinite(exact)) return exact;
  const last = Math.max(
    ...(result.deliveries ?? []).map((delivery) => delivery.actualMs)
  );
  return Number.isFinite(last) && Number.isFinite(startWall)
    ? startWall + last
    : null;
}

/** Only a complete set of app finish receipts gives an exact finish boundary. */
export function lastFinishWall(result) {
  if (!Number.isInteger(result.threadCount) || result.threadCount < 1)
    return null;
  const finishes = (result.finishes ?? []).filter(
    (finish) =>
      Number.isFinite(finish.wallTimeMs) &&
      Number.isInteger(finish.thread) &&
      finish.thread >= 0 &&
      finish.thread < result.threadCount
  );
  const threads = new Set(finishes.map((finish) => finish.thread));
  if (threads.size !== result.threadCount) return null;
  return Math.max(...finishes.map((finish) => finish.wallTimeMs));
}

export function shouldKeepTrace(options, item, metrics, failed = false) {
  return Boolean(
    options.keepTraces ||
    options.diagnosticTrace ||
    failed ||
    metrics.settleMs > 500 ||
    metrics.tailWindowCensored ||
    (item.block === 0 && WORKLOADS[item.workload].repeats == null)
  );
}

/** Older saved APKs must not silently ignore newly requested run options. */
export function appOptionErrors(config, result) {
  const errors = [];
  if (config.animated && result.animationEnabled !== true)
    errors.push('animated profile needs an updated benchmark APK; rebuild it');
  if (result.settleMs !== config.settleMs)
    errors.push(
      'app settle budget differs from the session; rebuild the benchmark APK'
    );
  if (
    (config.finishDelayMs ?? 0) > 0 &&
    result.finishDelayAppliedMs !== config.finishDelayMs
  )
    errors.push(
      'app ignored the replay finish delay; rebuild the benchmark APK'
    );
  if (config.trace && !result.trace?.enabled)
    errors.push('app did not collect requested diagnostic native tracing');
  return [
    ...errors,
    ...(benchmarkSuite?.appOptionErrors?.(config, result) ?? []),
  ];
}

/**
 * Converts the app's wall-clock milliseconds to trace (BOOTTIME) nanoseconds.
 * `offsetUs` is REALTIME minus BOOTTIME from the trace's clock snapshot.
 */
export function wallToTrace(offsetUs, wallMs) {
  if (!Number.isFinite(offsetUs))
    throw new Error('Trace has no REALTIME/BOOTTIME clock snapshot');
  return (Math.round(wallMs * 1000) - offsetUs) * 1000;
}

// ---------------------------------------------------------------------------
// run: the measurement session

export function loadResumeState(output) {
  const session = JSON.parse(
    readFileSync(join(output, 'session.json'), 'utf8')
  );
  assertSessionSuite(session);
  if (session.schemaVersion !== 2 || !Array.isArray(session.order))
    throw new Error(
      'Resume needs a saved schemaVersion 2 session with its run order'
    );
  validateFinishDelay(
    session.finishDelayMs ?? 0,
    session.input?.mode === 'replay'
  );
  const savedRuns = readdirSync(join(output, 'runs'))
    .filter((name) => name.endsWith('.json'))
    .map((name) => ({
      name,
      record: JSON.parse(readFileSync(join(output, 'runs', name), 'utf8')),
    }));
  const completed = new Set();
  for (const { name, record } of savedRuns) {
    const item = session.order.find(
      (scheduled) => scheduled.index === record.config.index
    );
    if (
      !item ||
      ['profile', 'candidate', 'workload', 'block', 'warmup'].some(
        (key) => item[key] !== record.config[key]
      )
    )
      throw new Error(`${name}: saved run does not match the session order`);
    if (record.status === 'ok') {
      benchmarkSuite?.validateSavedRun?.(session, record);
      if (
        (record.config.finishDelayMs ?? 0) !== (session.finishDelayMs ?? 0) ||
        Boolean(record.config.trace) !== Boolean(session.diagnosticTrace)
      )
        throw new Error(
          `${name}: saved run finish delay or tracing differs from the session`
        );
      if (completed.has(item.index))
        throw new Error(`${name}: duplicate successful run ${item.index}`);
      completed.add(item.index);
    }
  }
  const pending = session.order.filter((item) => !completed.has(item.index));
  for (const profile of new Set(pending.map((item) => item.profile))) {
    const buildRecord = session.builds.find(
      (savedBuild) => savedBuild.profile === profile
    );
    // A later profile may not have reached its build step before interruption.
    if (
      !buildRecord &&
      !savedRuns.some(({ record }) => record.config.profile === profile)
    )
      continue;
    const apkPath = join(output, 'builds', `${profile}.apk`);
    if (buildRecord) assertBuildSuite(buildRecord, profile);
    if (
      !buildRecord ||
      !existsSync(apkPath) ||
      sha(readFileSync(apkPath)) !== buildRecord.apkSha256
    )
      throw new Error(
        `Cannot resume: saved ${profile} APK is missing or changed`
      );
  }
  return { session, savedRuns, completed, pending };
}

// Keep failed attempts out of the report while preserving them for inspection.
export function archiveFailedRuns(output, savedRuns, item) {
  for (const { name, record } of savedRuns) {
    if (record.status === 'ok' || record.config.index !== item.index) continue;
    const archive = join(output, 'failed-attempts');
    mkdirSync(archive, { recursive: true });
    let attempt = 1;
    let destination;
    do {
      destination = join(
        archive,
        `${name.slice(0, -5)}-attempt-${attempt++}.json`
      );
    } while (existsSync(destination));
    renameSync(join(output, 'runs', name), destination);
  }
}

export function schedule(candidates, workloads, profiles, repeats, seed) {
  const random = mulberry32(seed);
  const order = [];
  // A workload with a fixed repeat count skips the warmup block and joins only
  // its first blocks.
  const inBlock = (workload, block) => {
    const fixed = WORKLOADS[workload]?.repeats;
    if (fixed == null) return true;
    return block >= 0 && block < fixed;
  };
  for (const profile of profiles) {
    for (let block = -1; block < repeats; block += 1) {
      const cells = candidatesForProfile(candidates, profile).flatMap(
        (candidate) =>
          workloads
            .filter((workload) => inBlock(workload, block))
            .map((workload) => ({ candidate, workload }))
      );
      for (const cell of shuffle(cells, random)) {
        order.push({ ...cell, profile, block, warmup: block < 0 });
      }
    }
  }
  return order;
}

async function prepareDevice(device, enabled) {
  const restore = [];
  await device.shell('input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard', {
    allowFailure: true,
  });
  if (!enabled) return async () => {};
  const remember = async (namespace, key, value) => {
    const previous = await device.setting(namespace, key);
    restore.push(
      `settings ${previous == null ? 'delete' : 'put'} ${namespace} ${key}${previous == null ? '' : ` ${previous}`}`
    );
    await device.shell(`settings put ${namespace} ${key} ${value}`);
  };
  await remember('global', 'stay_on_while_plugged_in', 7);
  await remember('system', 'screen_brightness_mode', 0);
  await remember('system', 'screen_brightness', 128);
  const peak = await device.setting('system', 'peak_refresh_rate');
  if (peak) {
    await remember('system', 'min_refresh_rate', peak);
  }
  return async () => {
    for (const command of restore.reverse())
      await device.shell(command, { allowFailure: true });
  };
}

async function waitUntilCool(device, thermal) {
  const started = Date.now();
  let state = await device.thermal();
  while (
    (state.status > 0 || state.batteryC > thermal.maxBatteryC) &&
    Date.now() - started < thermal.maxWaitMs
  ) {
    await sleep(5000);
    state = await device.thermal();
  }
  return {
    ...state,
    waitedMs: Date.now() - started,
    cool: state.status === 0 && state.batteryC <= thermal.maxBatteryC,
  };
}

class Host {
  constructor() {
    this.current = null;
    this.server = createServer((request, response) =>
      this.handle(request, response)
    );
  }
  listen() {
    return new Promise((done, fail) => {
      this.server.once('error', fail);
      this.server.listen(PORT, '127.0.0.1', done);
    });
  }
  close() {
    this.server.close();
  }
  async handle(request, response) {
    const reply = (status, value) => {
      const body = JSON.stringify(value);
      response.writeHead(status, {
        'Content-Type': 'application/json',
        'Cache-Control': 'no-store',
      });
      response.end(body);
    };
    try {
      const url = new URL(request.url, 'http://host');
      if (request.method === 'GET' && url.pathname === '/config') {
        reply(200, this.current?.config ?? { waiting: true });
        return;
      }
      if (request.method === 'GET' && url.pathname === '/stream') {
        const active = this.current;
        if (
          !active?.onStream ||
          url.searchParams.get('runId') !== active.config.runId
        ) {
          reply(409, { error: 'no live run is active' });
          return;
        }
        await active.onStream(response);
        return;
      }
      const chunks = [];
      for await (const chunk of request) chunks.push(chunk);
      const body = chunks.length
        ? JSON.parse(Buffer.concat(chunks).toString())
        : {};
      const active = this.current;
      if (!active || body.runId !== active.config.runId) {
        reply(409, { error: 'runId is not active' });
        return;
      }
      if (url.pathname === '/event') {
        active.events.push({ ...body, hostMs: Date.now() });
        // The app waits for this reply, so tracing is armed before any text arrives.
        if (body.type === 'mounted') await active.onMounted();
        if (body.type === 'end') active.onEnd();
        reply(200, { ok: true });
        return;
      }
      if (url.pathname === '/result') {
        reply(200, { ok: true });
        active.resolve(body);
        return;
      }
      reply(404, { error: 'not found' });
    } catch (error) {
      this.current?.reject?.(error);
      if (!response.headersSent) reply(500, { error: String(error) });
    }
  }
}

async function measureOne(device, host, item, options, workload, ai) {
  const runId = `${item.profile}-${String(item.index).padStart(4, '0')}-${item.candidate}-${item.workload}${item.warmup ? '-warmup' : ''}`;
  const runDir = join(options.output, 'runs');
  const tracePath = join(options.output, 'traces', `${runId}.pftrace`);
  const before = await waitUntilCool(device, options.thermal);
  await device.shell(`am force-stop ${PACKAGE}; am kill-all`, {
    allowFailure: true,
  });
  await sleep(options.cooldownMs);

  const state = {
    events: [],
    tracePid: null,
    appPid: null,
    meminfoBefore: null,
    meminfoAfter: null,
    live: null,
    liveError: null,
  };
  const prompts = WORKLOADS[item.workload].responses.map(
    (index) => PROMPTS[index]
  );
  const config = {
    runId,
    candidate: item.candidate,
    workload: item.workload,
    mode: 'stream',
    warmup: item.warmup,
    settleMs: options.settleMs,
    finishDelayMs: options.finishDelayMs ?? 0,
    trace: Boolean(options.diagnosticTrace),
    ...profileConfig(item.profile),
    math:
      profileConfig(item.profile).math ??
      WORKLOADS[item.workload].math ??
      false,
    ...(benchmarkSuite && { benchmarkVersion: benchmarkSuite.id }),
    ...(ai
      ? { live: { threads: prompts.length } }
      : { source: workload.source, chunks: workload.chunks }),
  };
  let endWork = Promise.resolve();
  const result = await new Promise((resolveRun, rejectRun) => {
    const timer = setTimeout(
      () => rejectRun(new Error(`${runId}: timed out`)),
      options.timeoutMs
    );
    host.current = {
      config,
      events: state.events,
      onMounted: async () => {
        state.appPid = (await device.shell(`pidof ${PACKAGE}`))
          .trim()
          .split(/\s+/)[0];
        state.meminfoBefore = parseMeminfo(
          await device.shell(`dumpsys meminfo ${state.appPid}`)
        );
        state.tracePid = await startTrace(device);
      },
      onStream: ai
        ? async (response) => {
            try {
              state.live = await relayLive(ai, prompts, response);
            } catch (error) {
              state.liveError = String(error.message ?? error);
            }
          }
        : null,
      onEnd: () => {
        // After the window: reading meminfo now cannot disturb the measurement.
        endWork = device
          .shell(`dumpsys meminfo ${state.appPid}`)
          .then((text) => {
            state.meminfoAfter = parseMeminfo(text);
          });
      },
      resolve: (body) => {
        clearTimeout(timer);
        resolveRun(body);
      },
      reject: (error) => {
        clearTimeout(timer);
        rejectRun(error);
      },
    };
    device.shell(`am start -W -n ${PACKAGE}/.MainActivity`).catch(rejectRun);
  }).finally(() => {
    host.current = null;
  });
  await endWork;
  if (state.tracePid) await stopTrace(device, state.tracePid, tracePath);
  await device.shell(`am force-stop ${PACKAGE}`, { allowFailure: true });
  const after = await device.thermal();

  const errors = [...(result.errors ?? []), ...appOptionErrors(config, result)];
  if (result.failed) errors.push('app reported failure');
  if (ai) {
    if (state.liveError) errors.push(`live model: ${state.liveError}`);
    else if (!state.live) errors.push('the app never opened the live stream');
    else if (
      state.live.some(
        (thread, index) => result.threadTexts?.[index] !== thread.text
      )
    )
      errors.push('the app rendered different text than the model streamed');
  } else if (result.sourceEqualsFixture !== true) {
    errors.push('delivered text differs from the recording');
  }
  const wall = (type) =>
    state.events.find((event) => event.type === type)?.wallTimeMs;
  let metrics = {};
  if (!errors.length) {
    const raw = await queryTrace(tracePath, state.appPid);
    const startWall = result.startWallTimeMs ?? wall('start');
    const finishWall = lastFinishWall(result);
    const window = {
      start: wallToTrace(raw.clockOffsetUs, startWall),
      // Settle counts from the last delivered text, not the later end marker.
      deliveryEnd: wallToTrace(
        raw.clockOffsetUs,
        lastDeliveryWall(result, startWall) ?? wall('delivery-end')
      ),
      finishEnd:
        finishWall == null ? null : wallToTrace(raw.clockOffsetUs, finishWall),
      end: wallToTrace(raw.clockOffsetUs, wall('end')),
    };
    metrics = { ...traceMetrics(raw, window), ...appMetrics(result) };
    if (result.trace) metrics.nativePhases = nativePhaseSummary(result.trace);
    // Live replies differ in length between runs, so CPU is also given per KiB.
    metrics.textKiB =
      (result.threadSources ?? []).reduce((sum, info) => sum + info.bytes, 0) /
      1024;
    metrics.cpuMsPerKiB =
      metrics.textKiB > 0 ? (metrics.cpuTotalS * 1000) / metrics.textKiB : null;
    metrics.pssBeforeMiB = state.meminfoBefore?.pssMiB ?? null;
    metrics.pssEndMiB = state.meminfoAfter?.pssMiB ?? null;
    metrics.javaHeapEndMiB = state.meminfoAfter?.javaHeapMiB ?? null;
    metrics.nativeHeapEndMiB = state.meminfoAfter?.nativeHeapMiB ?? null;
    metrics.graphicsEndMiB = state.meminfoAfter?.graphicsMiB ?? null;
    if (!metrics.frames) errors.push('no frames in the trace window');
    if (metrics.memSamples < 5)
      errors.push('too few memory samples in the window');
  }
  // Keep baseline examples and slow/failed runs, including huge workloads.
  const keepTrace = shouldKeepTrace(options, item, metrics, errors.length > 0);
  if (!keepTrace) rmSync(tracePath, { force: true });
  const record = {
    config: {
      ...config,
      source: undefined,
      chunks: undefined,
      profile: item.profile,
      block: item.block,
      index: item.index,
    },
    status: errors.length ? 'failed' : 'ok',
    errors,
    thermal: { before, after },
    metrics,
    app: {
      deliveryDurationMs: result.deliveryDurationMs,
      totalDurationMs: result.totalDurationMs,
      threadCount: result.threadCount,
      sourceInfo: result.sourceInfo,
      revisionCount: result.revisionCount,
      startWallTimeMs: result.startWallTimeMs,
      deliveries: result.deliveries,
      finishes: result.finishes,
      streamDone: result.streamDone,
      animationEnabled: result.animationEnabled,
      settleMs: result.settleMs,
      finishDelayAppliedMs: result.finishDelayAppliedMs,
      ...(benchmarkSuite && {
        benchmarkVersion: result.benchmarkVersion,
        apiConfig: result.apiConfig,
        threadConfigurations: result.threadConfigurations,
      }),
    },
    ...(result.trace && { nativeTrace: result.trace }),
    ...(state.live && { live: state.live }),
    events: state.events.map(({ type, wallTimeMs, appTimeMs, hostMs }) => ({
      type,
      wallTimeMs,
      appTimeMs,
      hostMs,
    })),
    trace: keepTrace ? relative(options.output, tracePath) : null,
  };
  writeFileSync(
    join(runDir, `${runId}.json`),
    JSON.stringify(record, null, 1) + '\n',
    { flag: 'wx' }
  );
  return record;
}

async function runSession(options) {
  if (
    benchmarkSuite &&
    !options.resume &&
    existsSync(join(options.output, 'session.json'))
  )
    throw new Error(
      'Result directory already has a session; use resume or a new --output.'
    );
  const resumed = options.resume ? loadResumeState(options.output) : null;
  if (resumed) {
    const { session, completed, pending } = resumed;
    if (
      options.finishDelayRequested &&
      options.finishDelayMs !== (session.finishDelayMs ?? 0)
    )
      throw new Error(
        '--finish-delay-ms cannot change during resume; use a new result directory'
      );
    if (options.diagnosticTrace && !session.diagnosticTrace)
      throw new Error(
        'Diagnostic tracing cannot be enabled during resume; use a new result directory'
      );
    options = {
      ...options,
      device: options.device ?? session.device.serial,
      profiles: session.profiles,
      candidates: session.candidates,
      workloads: session.workloadIds,
      repeats: session.repeats,
      seed: session.seed,
      model: session.input.model,
      reasoning: session.input.reasoningEffort,
      replay:
        session.input.mode === 'replay'
          ? resolve(ROOT, session.input.folder)
          : null,
      deviceSetup: session.deviceSetup,
      skipBuild: true,
      finishDelayMs: session.finishDelayMs ?? 0,
      diagnosticTrace: Boolean(session.diagnosticTrace),
    };
    log(
      `Resuming ${completed.size}/${session.order.length} successful runs; ${pending.length} remaining`
    );
    if (!pending.length) return report(options.output, options);
  }
  for (const id of options.workloads) {
    if (!WORKLOADS[id])
      throw new Error(
        `Unknown workload ${id}; choose from ${Object.keys(WORKLOADS).join(', ')}`
      );
  }
  const recordings = options.replay
    ? loadRecordings(resolve(options.replay))
    : null;
  const workloads = recordings
    ? Object.fromEntries(
        options.workloads.map((id) => [
          id,
          buildWorkload(recordings, WORKLOADS[id].responses),
        ])
      )
    : null;
  const ai = recordings ? null : await loadModel(options);
  if (
    benchmarkSuite &&
    resumed &&
    Object.entries(workloads ?? {}).some(
      ([id, workload]) =>
        resumed.session.workloads[id]?.replaySha256 !==
        sha(JSON.stringify(workload.chunks))
    )
  )
    throw new Error(
      'Replay chunks or cadence changed since the saved session; use the original recording.'
    );
  await ensureTraceProcessor();
  const device = await pickDevice(options.device);
  for (const candidate of options.candidates) {
    if (!CANDIDATES[candidate])
      throw new Error(`Unknown candidate ${candidate}`);
  }
  mkdirSync(join(options.output, 'runs'), { recursive: true });
  mkdirSync(join(options.output, 'traces'), { recursive: true });

  const model = (await device.shell('getprop ro.product.model')).trim();
  const release = (
    await device.shell('getprop ro.build.version.release')
  ).trim();
  const fingerprint = (
    await device.shell('getprop ro.build.fingerprint')
  ).trim();
  if (
    resumed &&
    (device.serial !== resumed.session.device.serial ||
      fingerprint !== resumed.session.device.fingerprint)
  )
    throw new Error('Resume requires the original phone and Android build');
  const startThermal = await device.thermal();
  const session = resumed?.session ?? {
    schemaVersion: 2,
    ...(benchmarkSuite && {
      benchmarkVersion: benchmarkSuite.id,
      suiteFingerprint: suiteFingerprint(),
      profileConfigurations: benchmarkSuite.profiles,
    }),
    startedAt: new Date().toISOString(),
    output: options.output,
    device: { serial: device.serial, model, release, fingerprint },
    profiles: options.profiles,
    candidates: options.candidates,
    workloadIds: options.workloads,
    workloads: Object.fromEntries(
      options.workloads.map((id) => [
        id,
        workloads
          ? {
              chunks: workloads[id].chunks.length,
              lastDueMs: workloads[id].chunks.at(-1).dueMs,
              sourceSha256: sha(workloads[id].source),
              ...(benchmarkSuite && {
                replaySha256: sha(JSON.stringify(workloads[id].chunks)),
              }),
              threads: workloads[id].threads,
            }
          : {
              prompts: WORKLOADS[id].responses.length,
              repeats: WORKLOADS[id].repeats ?? options.repeats,
            },
      ])
    ),
    input: recordings
      ? {
          mode: 'replay',
          folder: relative(ROOT, resolve(options.replay)),
          model: recordings[0].model,
          reasoningEffort: recordings[0].request?.reasoningEffort,
          client: recordings[0].client,
        }
      : {
          mode: 'live',
          model: ai.modelId,
          reasoningEffort: ai.reasoningEffort,
          client: ai.client,
        },
    repeats: options.repeats,
    seed: options.seed,
    settleMs: options.settleMs ?? 2000,
    finishDelayMs: options.finishDelayMs ?? 0,
    diagnosticTrace: Boolean(options.diagnosticTrace),
    deviceSetup: options.deviceSetup,
    thermal: {
      maxBatteryC: Math.max(
        options.maxBatteryC ?? startThermal.batteryC + 1.5,
        30
      ),
      maxWaitMs: 240000,
      atStart: startThermal,
    },
    builds: [],
  };
  options.thermal = session.thermal;
  options.settleMs = session.settleMs;
  options.finishDelayMs = session.finishDelayMs ?? 0;
  options.diagnosticTrace = Boolean(session.diagnosticTrace);
  const order =
    resumed?.session.order ??
    schedule(
      options.candidates,
      options.workloads,
      options.profiles,
      options.repeats,
      options.seed
    ).map((item, index) => ({ ...item, index }));
  session.output = options.output;
  session.order = order;

  const host = new Host();
  await host.listen();
  await device.adb(['reverse', `tcp:${PORT}`, `tcp:${PORT}`]);
  const restore = await prepareDevice(device, options.deviceSetup);
  const interrupt = async () => {
    log('Interrupted; restoring phone settings');
    await restore();
    await device.adb(['shell', 'pkill -TERM perfetto'], { allowFailure: true });
    process.exit(130);
  };
  process.once('SIGINT', interrupt);
  const runs = resumed
    ? resumed.savedRuns
        .filter(({ record }) => record.status === 'ok')
        .map(({ record }) => record)
    : [];
  try {
    if (resumed)
      await device.adb(['shell', 'pkill -TERM perfetto'], {
        allowFailure: true,
      });
    for (const profile of options.profiles) {
      if (resumed && !resumed.pending.some((item) => item.profile === profile))
        continue;
      const savedBuild = resumed
        ? session.builds.find((record) => record.profile === profile)
        : null;
      const buildRecord = savedBuild
        ? savedBuild
        : options.skipBuild && !resumed
          ? JSON.parse(
              readFileSync(
                join(options.output, 'builds', `${profile}.json`),
                'utf8'
              )
            )
          : await build(profile, options.output);
      if (!savedBuild) session.builds.push(buildRecord);
      assertBuildSuite(buildRecord, profile);
      log(`Installing ${profile} APK`);
      await device.adb(
        ['install', '-r', join(options.output, 'builds', `${profile}.apk`)],
        { timeout: 300000 }
      );
      if ((await device.installedSha256()) !== buildRecord.apkSha256)
        throw new Error('Installed APK does not match the build');
      // Same ART state for every run: compile the app's Java/Kotlin ahead of time.
      await device.shell(`cmd package compile -m speed -f ${PACKAGE}`);
      session.vsyncHz = null;
      writeFileSync(
        join(options.output, 'session.json'),
        JSON.stringify(session, null, 1) + '\n'
      );

      const items = order.filter((item) => item.profile === profile);
      for (const [position, item] of items.entries()) {
        if (resumed?.completed.has(item.index)) continue;
        if ((await device.installedSha256()) !== buildRecord.apkSha256)
          throw new Error('Installed APK changed mid-session');
        if (resumed) archiveFailedRuns(options.output, resumed.savedRuns, item);
        let record;
        try {
          record = await measureOne(
            device,
            host,
            item,
            options,
            workloads?.[item.workload],
            ai
          );
        } catch (error) {
          record = {
            config: { ...item },
            status: 'failed',
            errors: [String(error.message ?? error)],
            metrics: {},
          };
          writeFileSync(
            join(
              options.output,
              'runs',
              `${item.profile}-${String(item.index).padStart(4, '0')}-failed.json`
            ),
            JSON.stringify(record, null, 1) + '\n'
          );
          await device.adb(['shell', 'pkill -TERM perfetto'], {
            allowFailure: true,
          });
        }
        runs.push(record);
        const m = record.metrics;
        log(
          `${position + 1}/${items.length} ${item.warmup ? 'warmup ' : ''}${item.candidate} ${item.workload}: ` +
            (record.status === 'ok'
              ? `cpu ${fmt(m.cpuTotalS, 2)} s, jank ${fmt(m.jankAppPct, 1)}%, p99 ${fmt(m.frameP99Ms, 1)} ms, mem +${fmt(m.memPeakIncreaseMiB, 1)} MiB, ${fmt(record.thermal?.before?.batteryC, 1)} °C`
              : `FAILED ${record.errors.join('; ')}`)
        );
      }
    }
  } finally {
    process.off('SIGINT', interrupt);
    await restore();
    host.close();
  }
  const vsync = median(
    runs.map((record) => record.metrics.vsyncMs).filter(Number.isFinite)
  );
  session.vsyncHz = vsync ? Math.round(1000 / vsync) : null;
  session.finishedAt = new Date().toISOString();
  writeFileSync(
    join(options.output, 'session.json'),
    JSON.stringify(session, null, 1) + '\n'
  );
  return report(options.output, options);
}

function report(output, options = {}) {
  const session = JSON.parse(
    readFileSync(join(output, 'session.json'), 'utf8')
  );
  assertSessionSuite(session);
  session.output = output;
  const runs = readdirSync(join(output, 'runs'))
    .filter((name) => name.endsWith('.json'))
    .map((name) =>
      JSON.parse(readFileSync(join(output, 'runs', name), 'utf8'))
    );
  const measured = runs.filter(
    (run) => run.status === 'ok' && !run.config.warmup
  );
  for (const run of measured) benchmarkSuite?.validateSavedRun?.(session, run);
  const failed = runs.filter((run) => run.status !== 'ok');
  const stats = summarize(measured, session.seed);
  writeFileSync(
    join(output, 'summary.json'),
    JSON.stringify(
      {
        session: { ...session, order: undefined },
        failed: failed.map((run) => ({
          config: run.config,
          errors: run.errors,
        })),
        ...stats,
      },
      null,
      1
    ) + '\n'
  );
  const markdown = renderReport(
    session,
    runs.filter((run) => run.status === 'ok'),
    stats
  );
  writeFileSync(join(output, 'REPORT.md'), markdown);
  if (options.publish !== false)
    writeFileSync(join(HERE, 'REPORT.md'), markdown);
  log(
    `Report: ${relative(ROOT, join(output, 'REPORT.md'))}${failed.length ? ` (${failed.length} failed runs listed in summary.json)` : ''}`
  );
  return markdown;
}

// ---------------------------------------------------------------------------
// selftest

async function selftest() {
  assert.equal(quantile([1, 2, 3, 4], 0.5), 2.5);
  assert.equal(quantile([5], 0.95), 5);
  assert.equal(quantile([], 0.5), null);
  const random = mulberry32(7);
  const interval = bootstrap([1, 2, 3, 4, 5, 6, 7, 8, 9, 10], median, random);
  assert.ok(interval[0] <= 5.5 && interval[1] >= 5.5);
  assert.deepEqual(
    bootstrap([1, 2, 3], median, mulberry32(3)),
    bootstrap([1, 2, 3], median, mulberry32(3))
  );
  const ratio = ratioInterval(
    [10, 10, 10, 11, 9],
    [20, 21, 19, 20, 20],
    mulberry32(1)
  );
  assert.ok(ratio[0] > 1.6 && ratio[1] < 2.4);
  // Fully separated groups of 10: exact two-sided p is 1.08e-5; the approximation is close.
  const separated = mannWhitney(
    [1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
    [11, 12, 13, 14, 15, 16, 17, 18, 19, 20]
  );
  assert.ok(separated.p < 0.001);
  assert.ok(mannWhitney([1, 2, 3, 4], [1, 2, 3, 4]).p > 0.9);

  const order = schedule(['a', 'b', 'c'], ['x', 'y'], ['highlighted'], 4, 9);
  assert.equal(order.length, 3 * 2 * 5);
  for (let block = -1; block < 4; block += 1) {
    const cells = order
      .filter((item) => item.block === block)
      .map((item) => `${item.candidate}/${item.workload}`);
    assert.equal(new Set(cells).size, 6, 'every block runs each cell once');
  }
  assert.deepEqual(
    order,
    schedule(['a', 'b', 'c'], ['x', 'y'], ['highlighted'], 4, 9)
  );
  const huge = schedule(
    ['a', 'b'],
    ['ai-single', 'ai-huge-single'],
    ['highlighted'],
    4,
    2
  ).filter((item) => item.workload === 'ai-huge-single');
  assert.equal(huge.length, 2, 'one huge run per library');
  assert.ok(huge.every((item) => item.block === 0 && !item.warmup));
  assert.equal(PROMPTS.length, 10);
  assert.equal(FEATURE_ORDERS.length, 16);
  assert.equal(
    new Set(FEATURE_ORDERS.map((sequence) => sequence.join(','))).size,
    16
  );
  for (let mask = 0; mask < 8; mask += 1) {
    const subset = ['math', 'code', 'table'].filter(
      (_, bit) => Math.floor(mask / 2 ** bit) % 2 === 1
    );
    const orders = FEATURE_ORDERS.filter(
      (sequence) =>
        sequence.length === subset.length &&
        subset.every((feature) => sequence.includes(feature))
    );
    assert.equal(
      orders.length,
      [1, 1, 2, 6][subset.length],
      'every subset has all its permutations'
    );
    assert.ok(
      orders.every((sequence) => new Set(sequence).size === sequence.length)
    );
  }
  assert.ok(
    FEATURE_ORDERS.every((sequence) =>
      PROMPTS[3].includes(sequence.join(' then ') || 'prose only')
    )
  );
  assert.deepEqual(WORKLOADS['ai-feature-matrix'].responses, [3]);
  assert.ok(WORKLOADS['ai-feature-matrix'].math);
  assert.ok(WORKLOADS['ai-concurrent-5'].responses.includes(3));
  assert.ok(WORKLOADS['ai-concurrent-5'].math);
  assert.equal(mannWhitney([1], [2]), null);

  const recording = (text, first, step) => {
    const chunks = [...text].map((character, index) => ({
      text: character,
      dueMs: index * step,
    }));
    return {
      source: text,
      chunks,
      firstArrivalMs: first,
      sha256: sha(text),
      model: 'test',
      file: join(ROOT, 'x.json'),
    };
  };
  const recordings = [recording('abc', 100, 10), recording('wxyz', 130, 5)];
  const single = buildWorkload(recordings, [1]);
  assert.equal(single.source, 'wxyz');
  assert.equal(single.chunks[0].dueMs, 0);
  const both = buildWorkload(recordings, [0, 1]);
  const view = (thread) =>
    both.chunks
      .filter((chunk) => chunk.thread === thread)
      .map((chunk) => chunk.text)
      .join('');
  assert.equal(view(0), 'abc');
  assert.equal(view(1), 'wxyz');
  assert.equal(
    both.chunks.find((chunk) => chunk.thread === 1).dueMs,
    30,
    'keeps the real arrival offset'
  );
  assert.ok(
    both.chunks.every(
      (chunk, index) =>
        index === 0 || chunk.dueMs >= both.chunks[index - 1].dueMs
    )
  );

  // Boot 5 s before wall time 1_700_000_000_000 ms.
  assert.equal(
    wallToTrace(1_699_999_995_000_000, 1_700_000_000_001),
    5_001_000_000
  );

  const ms = 1e6;
  const raw = {
    pid: 10,
    threads: [
      { utid: 1, tid: 10, name: 'downstream.exam' },
      { utid: 2, tid: 11, name: 'mqt_v_js' },
      { utid: 3, tid: 12, name: 'RenderThread' },
      { utid: 4, tid: 13, name: 'worker' },
    ],
    sched: [
      [1, 0, 100 * ms], // half inside the window
      [2, 100 * ms, 50 * ms],
      [3, 120 * ms, 10 * ms],
      [4, 900 * ms, 500 * ms], // half inside
    ],
    frames: [
      [100 * ms, 8 * ms, 'None', 'On-time Present', 100 * ms, 8 * ms, 'app'],
      [
        110 * ms,
        30 * ms,
        'App Deadline Missed',
        'Late Present',
        110 * ms,
        8 * ms,
        'app',
      ],
      [
        500 * ms,
        10 * ms,
        'Buffer Stuffing',
        'Late Present',
        500 * ms,
        8 * ms,
        'app',
      ],
      [1200 * ms, 8 * ms, 'None', 'On-time Present', 1200 * ms, 8 * ms, 'app'], // outside
    ],
    memory: [
      ['mem.rss.anon', 0, 100 * 1048576],
      ['mem.rss.anon', 60 * ms, 100 * 1048576],
      ['mem.rss.anon', 300 * ms, 130 * 1048576],
      ['mem.rss.anon', 700 * ms, 120 * 1048576],
      ['mem.swap', 0, 0],
    ],
  };
  const metrics = traceMetrics(raw, {
    start: 50 * ms,
    deliveryEnd: 400 * ms,
    end: 1000 * ms,
  });
  assert.equal(metrics.cpuMainS, 0.05);
  assert.equal(metrics.cpuJsS, 0.05);
  assert.equal(metrics.cpuRenderS, 0.01);
  assert.equal(metrics.cpuOtherS, 0.1);
  assert.equal(metrics.frames, 3);
  assert.equal(metrics.jankAppPct, 100 / 3);
  assert.equal(metrics.jankAnyPct, 100 / 3);
  assert.equal(metrics.frameMaxMs, 30);
  assert.equal(metrics.memPeakIncreaseMiB, 30);
  assert.equal(metrics.settleMs, 110);
  assert.equal(metrics.vsyncMs, 10, 'spacing of expected frame starts');
  assert.equal(
    lastDeliveryWall({ deliveries: [{ actualMs: 5 }, { actualMs: 40 }] }, 1000),
    1040
  );
  assert.equal(lastDeliveryWall({ deliveries: [] }, 1000), null);

  const meminfo = parseMeminfo(
    '  Java Heap:    10240\n  Native Heap:  20480\n  Graphics:  4096\n  TOTAL PSS:   153600   TOTAL RSS: 1\n'
  );
  assert.equal(meminfo.pssMiB, 150);
  assert.equal(meminfo.javaHeapMiB, 10);
  const unicode = { thread: 0, text: 'café 漢字 👩‍💻 "q"\n' };
  const escaped = asciiJson(unicode);
  assert.ok([...escaped].every((character) => character.charCodeAt(0) < 128));
  assert.deepEqual(JSON.parse(escaped), unicode);

  // The relay forwards deltas in arrival order, per thread, then finishes.
  const fakeModel = (deltasByPrompt, failOn) => ({
    model: 'fake',
    reasoningEffort: 'medium',
    streamText: ({ prompt, onError }) => ({
      textStream: (async function* () {
        for (const delta of deltasByPrompt[prompt]) {
          await sleep(1);
          yield delta;
        }
        if (prompt === failOn) onError({ error: new Error('rate limited') });
      })(),
      totalUsage: Promise.resolve({ outputTokens: 3 }),
      finishReason: Promise.resolve('stop'),
    }),
  });
  const fakeResponse = () => {
    const lines = [];
    return {
      lines,
      writeHead: () => {},
      write: (chunk) =>
        lines.push(
          ...chunk
            .split('\n')
            .filter(Boolean)
            .map((line) => JSON.parse(line))
        ),
      end: () => lines.push('end'),
    };
  };
  const ok = fakeResponse();
  const threads = await relayLive(
    fakeModel({ a: ['He', 'llo'], b: ['漢', '字'] }),
    ['a', 'b'],
    ok
  );
  assert.deepEqual(
    threads.map((thread) => thread.text),
    ['Hello', '漢字']
  );
  assert.deepEqual(
    ok.lines
      .filter((line) => line.text)
      .map((line) => `${line.thread}:${line.text}`)
      .sort(),
    ['0:He', '0:llo', '1:字', '1:漢']
  );
  assert.deepEqual(ok.lines.slice(-2), [{ done: true }, 'end']);
  const failing = fakeResponse();
  await assert.rejects(
    relayLive(fakeModel({ a: ['x'] }, 'a'), ['a'], failing),
    /rate limited/
  );
  assert.deepEqual(failing.lines.slice(-2), [{ error: 'rate limited' }, 'end']);
  console.log('selftest passed');
}

// ---------------------------------------------------------------------------
// CLI

async function main(args = process.argv.slice(2)) {
  const { values, positionals } = parseArgs({
    args,
    allowPositionals: true,
    options: {
      'device': { type: 'string' },
      'profiles': {
        type: 'string',
        default:
          benchmarkSuite?.defaultProfiles?.join(',') ??
          'highlighted,highlighted-animated',
      },
      'repeats': { type: 'string', default: '5' },
      'seed': { type: 'string', default: '1' },
      'workloads': {
        type: 'string',
        default:
          benchmarkSuite?.defaultWorkloads?.join(',') ??
          Object.keys(WORKLOADS).join(','),
      },
      'candidates': {
        type: 'string',
        default:
          benchmarkSuite?.defaultCandidates?.join(',') ??
          Object.keys(CANDIDATES).join(','),
      },
      'replay': { type: 'string' },
      'output': { type: 'string' },
      'model': {
        type: 'string',
        default: process.env.EXPO_PUBLIC_OPENAI_MODEL || 'gpt-6-luna',
      },
      'reasoning': { type: 'string', default: 'medium' },
      'max-battery-c': { type: 'string' },
      'cooldown-ms': { type: 'string', default: '3000' },
      'timeout-ms': { type: 'string', default: '600000' },
      'settle-ms': { type: 'string', default: '2000' },
      'finish-delay-ms': { type: 'string' },
      'diagnostic-trace': { type: 'boolean', default: false },
      'keep-traces': { type: 'boolean', default: false },
      'skip-build': { type: 'boolean', default: false },
      'no-device-setup': { type: 'boolean', default: false },
      'no-publish': { type: 'boolean', default: false },
    },
  });
  const [command = 'all', target] = positionals;
  const stamp = new Date().toISOString().replace(/[-:]/g, '').slice(0, 15);
  const options = {
    device: values.device,
    profiles: values.profiles.split(','),
    repeats: Number(values.repeats),
    seed: Number(values.seed),
    workloads: values.workloads.split(','),
    candidates: values.candidates.split(','),
    replay: values.replay,
    output: resolve(
      values.output ??
        join(HERE, 'results', `${benchmarkSuite?.id ?? 'bench'}-${stamp}`)
    ),
    model: values.model,
    reasoning: values.reasoning,
    maxBatteryC: values['max-battery-c']
      ? Number(values['max-battery-c'])
      : undefined,
    cooldownMs: Number(values['cooldown-ms']),
    timeoutMs: Number(values['timeout-ms']),
    settleMs: Number(values['settle-ms']),
    finishDelayMs: Number(values['finish-delay-ms'] ?? 0),
    finishDelayRequested: values['finish-delay-ms'] != null,
    diagnosticTrace: values['diagnostic-trace'],
    keepTraces: values['keep-traces'],
    skipBuild: values['skip-build'] || command === 'run',
    deviceSetup: !values['no-device-setup'],
    publish: benchmarkSuite ? false : !values['no-publish'],
  };
  benchmarkSuite?.validateOptions?.(command, options);
  for (const candidate of options.candidates) {
    if (!CANDIDATES[candidate])
      throw new Error(`Unknown candidate ${candidate}`);
  }
  for (const workload of options.workloads) {
    if (!WORKLOADS[workload]) throw new Error(`Unknown workload ${workload}`);
  }
  for (const profile of options.profiles) {
    profileConfig(profile);
    if (!candidatesForProfile(options.candidates, profile).length)
      throw new Error(
        `${profile}: no selected candidates support streaming animations`
      );
  }
  if (!Number.isSafeInteger(options.repeats) || options.repeats < 2)
    throw new Error('--repeats must be an integer of at least 2');
  if (!Number.isFinite(options.settleMs) || options.settleMs <= 0)
    throw new Error('--settle-ms must be a positive finite number');
  if (command !== 'resume')
    validateFinishDelay(options.finishDelayMs, options.replay);
  if (
    command !== 'resume' &&
    options.diagnosticTrace &&
    options.candidates.some((candidate) => !candidate.startsWith('downstream-'))
  )
    throw new Error('--diagnostic-trace requires Downstream-only --candidates');

  switch (command) {
    case 'plan': {
      const plan = {
        benchmarkVersion: benchmarkSuite?.id ?? null,
        suiteFingerprint: benchmarkSuite ? suiteFingerprint() : null,
        profiles: Object.fromEntries(
          options.profiles.map((id) => [id, profileConfig(id)])
        ),
        candidates: options.candidates,
        workloads: options.workloads,
        order: schedule(
          options.candidates,
          options.workloads,
          options.profiles,
          options.repeats,
          options.seed
        ).map((item, index) => ({ ...item, index })),
      };
      console.log(JSON.stringify(plan, null, 2));
      return plan;
    }
    case 'capture':
      return capture(options);
    case 'build':
      for (const profile of options.profiles)
        await build(profile, options.output);
      return;
    case 'run':
      if (!values.output)
        throw new Error(
          'run needs --output <dir> holding builds/<profile>.apk from `build`'
        );
      return runSession(options);
    case 'resume':
      if (!target && !values.output)
        throw new Error(
          'resume needs <dir> holding session.json, runs/ and builds/'
        );
      return runSession({
        ...options,
        output: resolve(target ?? values.output),
        resume: true,
      });
    case 'all':
      return runSession({ ...options, skipBuild: false });
    case 'report':
      return report(resolve(target ?? options.output), options);
    case 'selftest':
      return selftest();
    default:
      throw new Error(`Unknown command ${command}`);
  }
}

/** Shared transport/build/report infrastructure; each CLI runs one isolated suite. */
export async function runBenchmarkCLI(suite, args = process.argv.slice(2)) {
  if (benchmarkSuite) throw new Error('A benchmark suite is already running.');
  const previous = {
    candidates: CANDIDATES,
    baseline: BASELINE,
  };
  benchmarkSuite = suite;
  if (suite) {
    CANDIDATES = suite.candidates;
    BASELINE = suite.baseline;
  }
  try {
    return await main(args);
  } finally {
    benchmarkSuite = undefined;
    CANDIDATES = previous.candidates;
    BASELINE = previous.baseline;
  }
}

if (
  process.argv[1] &&
  resolve(process.argv[1]) === fileURLToPath(import.meta.url)
) {
  runBenchmarkCLI().catch((error) => {
    console.error(error.stack ?? error);
    process.exit(1);
  });
}

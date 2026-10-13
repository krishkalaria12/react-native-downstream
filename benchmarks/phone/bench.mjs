#!/usr/bin/env node
// Phone benchmark for React Native Markdown renderers, in one file.
//
//   node benchmarks/phone/bench.mjs                 capture (if missing), build, install, run and report
//   node benchmarks/phone/bench.mjs capture         record new AI responses (Vercel AI SDK, costs API calls)
//   node benchmarks/phone/bench.mjs build           build and fingerprint the release APK only
//   node benchmarks/phone/bench.mjs run             run on the phone with an existing build
//   node benchmarks/phone/bench.mjs report <dir>    recompute statistics and REPORT.md from raw runs
//   node benchmarks/phone/bench.mjs selftest        check the statistics and workload code
//
// Options: --device <serial> --profiles highlighted[,plain] --repeats 10 --seed 1
//          --workloads ai-single,ai-code-heavy,ai-concurrent-5 --candidates <ids>
//          --recordings <dir> --output <dir> --keep-traces --skip-build --no-device-setup
//
// Method, in short. Every library receives the same recorded AI deltas at their
// recorded arrival times, inside the same release APK, one cold process per run.
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
  rmSync,
  writeFileSync,
} from 'node:fs';
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';
import assert from 'node:assert/strict';

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

const CANDIDATES = {
  'downstream-default': 'Downstream',
  'streamdown': 'RN Streamdown',
  'enriched': 'Enriched Markdown',
  'markdown-display': 'Markdown Display',
  'marked': 'Marked',
  'markdown-renderer': 'Markdown Renderer',
};
const BASELINE = 'downstream-default';

// ---------------------------------------------------------------------------
// Workloads: real AI responses recorded with `capture`, replayed exactly.

const WORKLOADS = {
  'ai-single': {
    responses: [0],
    label: 'One AI reply (prose, table, TypeScript)',
  },
  'ai-code-heavy': {
    responses: [4],
    label: 'One code-heavy AI reply (14 code blocks)',
  },
  'ai-concurrent-5': {
    responses: [0, 1, 2, 3, 4],
    label: 'Five AI replies streaming at once',
  },
};

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
  if (!n1 || !n2) return null;
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
 * trace timestamps (ns): start, deliveryEnd, end.
 */
export function traceMetrics(raw, window) {
  const { start, deliveryEnd, end } = window;
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
  const frames = raw.frames.filter(([ts]) => ts >= start && ts < end);
  const durations = frames.map(([, dur]) => dur / 1e6);
  const overruns = frames.map(([ts, dur, , , expectedTs, expectedDur]) =>
    expectedTs == null ? null : (ts + dur - (expectedTs + expectedDur)) / 1e6
  );
  const appJank = frames.filter(([, , jank]) =>
    /App Deadline Missed/i.test(jank ?? '')
  ).length;
  const anyJank = frames.filter(
    ([, , jank]) => jank && jank !== 'None' && jank !== 'Buffer Stuffing'
  ).length;
  const vsync = median(
    frames.map(([, , , , , expectedDur]) => (expectedDur ?? NaN) / 1e6)
  );
  const lastFrameEnd = raw.frames.reduce(
    (last, [ts, dur]) => (ts < end ? Math.max(last, ts + dur) : last),
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
    memStartMiB: before / MiB,
    memPeakMiB: peak / MiB,
    memPeakIncreaseMiB: (peak - before) / MiB,
    memEndMiB: used(end) / MiB,
    memSamples: inside.length,
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
    key: 'deliveryP95Ms',
    label: 'JS delivery lateness p95',
    unit: 'ms',
    digits: 1,
    headline: true,
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
  const lines = [];
  const device = session.device;
  const measured = runs.filter((run) => !run.config.warmup);
  lines.push('# Phone streaming benchmark');
  lines.push('');
  lines.push(
    `${device.model} · Android ${device.release} · ${device.vsyncHz ?? '?'} Hz · arm64 Hermes release · ${session.startedAt.slice(0, 10)}. ` +
      `${measured.length} measured runs, ${runs.length - measured.length} warmups excluded. ` +
      `Every number is the median of ${session.repeats} cold-process runs, with its 95% bootstrap confidence interval in brackets. Lower is better everywhere.`
  );
  lines.push('');
  lines.push('## Workloads');
  lines.push('');
  lines.push('| Workload | What streams | Deltas | Duration |');
  lines.push('|---|---|---:|---:|');
  for (const [id, workload] of Object.entries(session.workloads)) {
    lines.push(
      `| ${id} | ${WORKLOADS[id]?.label ?? id} | ${workload.chunks} | ${(workload.lastDueMs / 1000).toFixed(1)} s |`
    );
  }
  lines.push('');
  lines.push(
    `Recorded from \`${session.recordings.model}\` (reasoning ${session.recordings.reasoningEffort}) through the Vercel AI SDK \`streamText\`, five prompts requested at once. Each library gets the same deltas at the same recorded times.`
  );
  for (const profile of session.profiles) {
    lines.push('');
    lines.push(
      `## ${profile === 'highlighted' ? 'Code colors on (library defaults)' : 'Code colors off'}`
    );
    for (const workload of session.workloadIds) {
      lines.push('');
      lines.push(`### ${workload}: ${WORKLOADS[workload]?.label ?? ''}`);
      lines.push('');
      const headline = METRICS.filter((metric) => metric.headline);
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
      '- **Same input.** One release APK holds every library. Each run is a cold process showing one library full screen (five stacked panes for the concurrent workload, all visible). The host serves the recorded deltas; the app delivers them on their recorded schedule and checks that the text it delivered equals the recording.',
      '- **Frames.** Perfetto records SurfaceFlinger FrameTimeline, which classifies every frame the app draws. "Janky (app deadline missed)" is the share of frames Android itself blames on the app; "any cause" adds SurfaceFlinger, display and prediction misses but not buffer stuffing. Frame time is FrameTimeline\'s actual frame duration (CPU start to GPU done or post). Deadline overrun is actual end minus expected end, as in Jetpack Macrobenchmark.',
      '- **CPU.** Scheduler slices for every thread of the app process inside the measured window, split into the UI thread, the JS thread (mqt_js), RenderThread and everything else (native workers, GC, worklets). This is exact on-CPU time, not sampling.',
      '- **Memory.** Anonymous RSS plus swap, polled by Perfetto every 100 ms. "Peak increase" is the highest value during the window minus the value when streaming started. PSS from `dumpsys meminfo` is read after the window so it cannot disturb it.',
      '- **Settle.** Time from the last delivered chunk to the last frame the app drew, from the trace.',
      '- **JS lateness.** How late the app delivered each chunk against its schedule, a proxy for a busy JS thread.',
      `- **Window.** From the moment streaming starts until ${session.settleMs / 1000} s after the last chunk. App start-up and mounting are outside it.`,
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

const PROMPT_RULES =
  'Answer in GitHub-flavored Markdown. Do not use images, HTML, or mathematics.';
const PROMPTS = [
  `Explain how to build a retry-safe background job in about 600 words. Use headings, a numbered list, a bullet list, a GFM table comparing three retry policies, bold and italic emphasis, an inline code span, and a TypeScript code block of about 15 lines. ${PROMPT_RULES}`,
  `Compare PostgreSQL and MongoDB for a chat application's message storage in about 600 words. Include a comparison table with five rows, a Python code block that inserts and pages through messages, a short blockquote, and a recommendation section with a bullet list. ${PROMPT_RULES}`,
  `Write a step-by-step guide to finding and fixing dropped frames in a React Native app, about 600 words. Use nested numbered and bullet lists, a task list checklist, a JSON configuration code block, a bash code block, and links to https://reactnative.dev/docs/performance. ${PROMPT_RULES}`,
  `Plan a three-day trip to Kyoto in about 600 words. Use a heading per day, a table with times and places (include Japanese names such as 清水寺 and 伏見稲荷大社), bullet lists of food to try, bold highlights, a blockquote tip, and a short packing checklist. ${PROMPT_RULES}`,
  `Teach an interactive git rebase workflow for cleaning up a feature branch in about 600 words. Include shell code blocks, a numbered list of steps, a table of common rebase commands, inline code for flags, a warning section, and a reference-style link to https://git-scm.com/docs/git-rebase. ${PROMPT_RULES}`,
];

async function capture(options) {
  const outputDir = resolve(options.recordings);
  if (existsSync(outputDir))
    throw new Error(
      `${outputDir} already exists; pass a new --recordings folder`
    );
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
  const model = options.model;
  const reasoningEffort = options.reasoning;
  const openai = createOpenAI({ apiKey });
  const client = `ai ${requireExample('ai/package.json').version} streamText · @ai-sdk/openai ${requireExample('@ai-sdk/openai/package.json').version}`;
  log(
    `Requesting ${PROMPTS.length} responses at once from ${model} (reasoning ${reasoningEffort})`
  );
  const started = performance.now();
  const recordings = await Promise.all(
    PROMPTS.map(async (prompt, index) => {
      const chunks = [];
      const failures = [];
      const result = streamText({
        model: openai(model),
        prompt,
        maxOutputTokens: 6000,
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
          maxOutputTokens: 6000,
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
      enableCodeHighlight: profile === 'highlighted',
      enableMath: false,
      codeHighlightLanguages: ['javascript', 'typescript', 'python', 'json'],
    };
    writeFileSync(packagePath, JSON.stringify(pkg, null, 2) + '\n');
    if (profile === 'highlighted') {
      runVisible(
        'node',
        ['example/node_modules/react-native-enriched-markdown/postinstall.mjs'],
        {
          env: { ...process.env, INIT_CWD: EXAMPLE },
        }
      );
    }
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
    runVisible(
      'pnpm',
      [
        '--filter',
        'react-native-downstream-example',
        'exec',
        'expo',
        'prebuild',
        '--platform',
        'android',
        '--no-install',
      ],
      { env }
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
  const requireExample = createRequire(join(EXAMPLE, 'package.json'));
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
    ].map((name) => [name, requireExample(`${name}/package.json`).version])
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
duration_ms: 120000
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
async function queryTrace(tracePath, pid) {
  const query = `
    INCLUDE PERFETTO MODULE android.frames.timeline;
    SELECT json_object(
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
    ) AS result;`;
  const queryFile = `${tracePath}.sql`;
  writeFileSync(queryFile, query);
  const { stdout } = await runCommand(
    TRACE_PROCESSOR.path,
    ['--query-file', queryFile, tracePath],
    { timeout: 120000 }
  );
  rmSync(queryFile);
  // Output is a CSV-style column: header line, then the JSON as a quoted cell.
  const cell = stdout.slice(stdout.indexOf('\n') + 1).trim();
  const json = cell.startsWith('"')
    ? cell.slice(1, -1).replace(/""/g, '"')
    : cell;
  const raw = JSON.parse(json);
  for (const key of ['threads', 'sched', 'frames', 'memory']) {
    raw[key] =
      typeof raw[key] === 'string' ? JSON.parse(raw[key]) : (raw[key] ?? []);
  }
  raw.pid = Number(pid);
  return raw;
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

function schedule(candidates, workloads, profiles, repeats, seed) {
  const random = mulberry32(seed);
  const order = [];
  for (const profile of profiles) {
    for (let block = -1; block < repeats; block += 1) {
      const cells = candidates.flatMap((candidate) =>
        workloads.map((workload) => ({ candidate, workload }))
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
      if (request.method === 'GET' && request.url === '/config') {
        reply(200, this.current?.config ?? { waiting: true });
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
      if (request.url === '/event') {
        active.events.push({ ...body, hostMs: Date.now() });
        // The app waits for this reply, so tracing is armed before any text arrives.
        if (body.type === 'mounted') await active.onMounted();
        if (body.type === 'end') active.onEnd();
        reply(200, { ok: true });
        return;
      }
      if (request.url === '/result') {
        reply(200, { ok: true });
        active.resolve(body);
        return;
      }
      reply(404, { error: 'not found' });
    } catch (error) {
      this.current?.reject?.(error);
      reply(500, { error: String(error) });
    }
  }
}

async function measureOne(device, host, item, options, workload) {
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
  };
  const config = {
    runId,
    candidate: item.candidate,
    workload: item.workload,
    mode: 'stream',
    warmup: item.warmup,
    syntaxHighlighting: item.profile === 'highlighted',
    source: workload.source,
    chunks: workload.chunks,
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

  const errors = [...(result.errors ?? [])];
  if (result.failed) errors.push('app reported failure');
  if (result.sourceEqualsFixture !== true)
    errors.push('delivered text differs from the recording');
  const wall = (type) =>
    state.events.find((event) => event.type === type)?.wallTimeMs;
  let metrics = {};
  if (!errors.length) {
    const raw = await queryTrace(tracePath, state.appPid);
    const window = {
      start: wallToTrace(raw.clockOffsetUs, wall('start')),
      deliveryEnd: wallToTrace(raw.clockOffsetUs, wall('delivery-end')),
      end: wallToTrace(raw.clockOffsetUs, wall('end')),
    };
    metrics = { ...traceMetrics(raw, window), ...appMetrics(result) };
    metrics.pssBeforeMiB = state.meminfoBefore?.pssMiB ?? null;
    metrics.pssEndMiB = state.meminfoAfter?.pssMiB ?? null;
    metrics.javaHeapEndMiB = state.meminfoAfter?.javaHeapMiB ?? null;
    metrics.nativeHeapEndMiB = state.meminfoAfter?.nativeHeapMiB ?? null;
    metrics.graphicsEndMiB = state.meminfoAfter?.graphicsMiB ?? null;
    if (!metrics.frames) errors.push('no frames in the trace window');
    if (metrics.memSamples < 5)
      errors.push('too few memory samples in the window');
  }
  const keepTrace = options.keepTraces || item.block === 0;
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
    },
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
  await ensureTraceProcessor();
  const device = await pickDevice(options.device);
  const recordings = loadRecordings(resolve(options.recordings));
  const workloads = Object.fromEntries(
    options.workloads.map((id) => {
      if (!WORKLOADS[id])
        throw new Error(
          `Unknown workload ${id}; choose from ${Object.keys(WORKLOADS).join(', ')}`
        );
      return [id, buildWorkload(recordings, WORKLOADS[id].responses)];
    })
  );
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
  const startThermal = await device.thermal();
  const session = {
    schemaVersion: 2,
    startedAt: new Date().toISOString(),
    output: options.output,
    device: { serial: device.serial, model, release, fingerprint },
    profiles: options.profiles,
    candidates: options.candidates,
    workloadIds: options.workloads,
    workloads: Object.fromEntries(
      Object.entries(workloads).map(([id, workload]) => [
        id,
        {
          chunks: workload.chunks.length,
          lastDueMs: workload.chunks.at(-1).dueMs,
          sourceSha256: sha(workload.source),
          threads: workload.threads,
        },
      ])
    ),
    recordings: {
      folder: relative(ROOT, resolve(options.recordings)),
      model: recordings[0].model,
      reasoningEffort: recordings[0].request?.reasoningEffort,
      client: recordings[0].client,
    },
    repeats: options.repeats,
    seed: options.seed,
    settleMs: 2000,
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
  const order = schedule(
    options.candidates,
    options.workloads,
    options.profiles,
    options.repeats,
    options.seed
  ).map((item, index) => ({ ...item, index }));
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
  const runs = [];
  try {
    for (const profile of options.profiles) {
      const buildRecord = options.skipBuild
        ? JSON.parse(
            readFileSync(
              join(options.output, 'builds', `${profile}.json`),
              'utf8'
            )
          )
        : await build(profile, options.output);
      session.builds.push(buildRecord);
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
        if ((await device.installedSha256()) !== buildRecord.apkSha256)
          throw new Error('Installed APK changed mid-session');
        let record;
        try {
          record = await measureOne(
            device,
            host,
            item,
            options,
            workloads[item.workload]
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
  session.output = output;
  const runs = readdirSync(join(output, 'runs'))
    .filter((name) => name.endsWith('.json'))
    .map((name) =>
      JSON.parse(readFileSync(join(output, 'runs', name), 'utf8'))
    );
  const measured = runs.filter(
    (run) => run.status === 'ok' && !run.config.warmup
  );
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

function selftest() {
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
  assert.equal(metrics.vsyncMs, 8);

  const meminfo = parseMeminfo(
    '  Java Heap:    10240\n  Native Heap:  20480\n  Graphics:  4096\n  TOTAL PSS:   153600   TOTAL RSS: 1\n'
  );
  assert.equal(meminfo.pssMiB, 150);
  assert.equal(meminfo.javaHeapMiB, 10);
  console.log('selftest passed');
}

// ---------------------------------------------------------------------------
// CLI

async function main() {
  const { values, positionals } = parseArgs({
    allowPositionals: true,
    options: {
      'device': { type: 'string' },
      'profiles': { type: 'string', default: 'highlighted' },
      'repeats': { type: 'string', default: '10' },
      'seed': { type: 'string', default: '1' },
      'workloads': {
        type: 'string',
        default: Object.keys(WORKLOADS).join(','),
      },
      'candidates': {
        type: 'string',
        default: Object.keys(CANDIDATES).join(','),
      },
      'recordings': {
        type: 'string',
        default: join(HERE, 'recordings/luna-medium'),
      },
      'output': { type: 'string' },
      'model': {
        type: 'string',
        default: process.env.EXPO_PUBLIC_OPENAI_MODEL || 'gpt-6-luna',
      },
      'reasoning': { type: 'string', default: 'medium' },
      'max-battery-c': { type: 'string' },
      'cooldown-ms': { type: 'string', default: '3000' },
      'timeout-ms': { type: 'string', default: '120000' },
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
    recordings: values.recordings,
    output: resolve(values.output ?? join(HERE, 'results', `bench-${stamp}`)),
    model: values.model,
    reasoning: values.reasoning,
    maxBatteryC: values['max-battery-c']
      ? Number(values['max-battery-c'])
      : undefined,
    cooldownMs: Number(values['cooldown-ms']),
    timeoutMs: Number(values['timeout-ms']),
    keepTraces: values['keep-traces'],
    skipBuild: values['skip-build'] || command === 'run',
    deviceSetup: !values['no-device-setup'],
    publish: !values['no-publish'],
  };
  for (const profile of options.profiles) {
    if (!['highlighted', 'plain'].includes(profile))
      throw new Error(`Unknown profile ${profile}`);
  }
  if (!(options.repeats >= 2)) throw new Error('--repeats must be at least 2');

  switch (command) {
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
    case 'all':
      // A fresh checkout has no recordings yet: capture them first.
      if (!existsSync(resolve(options.recordings))) await capture(options);
      return runSession({ ...options, skipBuild: false });
    case 'report':
      return report(resolve(target ?? options.output), options);
    case 'selftest':
      return selftest();
    default:
      throw new Error(`Unknown command ${command}`);
  }
}

if (
  process.argv[1] &&
  resolve(process.argv[1]) === fileURLToPath(import.meta.url)
) {
  main().catch((error) => {
    console.error(error.stack ?? error);
    process.exit(1);
  });
}

#!/usr/bin/env node
/** Summarize app traces without treating JavaScript callbacks as native paint. */
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const samples = {
  'arrival-to-frame-sample': ['chunkToFrameEstimateMs', 'value'],
  'js-frame-interval': ['jsFrameIntervalMs', 'value'],
  'batch-delivery': ['deliveryLatenessMs', 'latenessMs'],
};

const workloadFields = [
  'renderer', 'parseMode', 'fixture', 'sourceHash', 'workloadSize',
  'workloadShape', 'schedule', 'seed', 'streams', 'runCondition',
];

const finite = (value) => typeof value === 'number' && Number.isFinite(value);
const round = (value, places) => Number(value.toFixed(places));

export function percentile(values, fraction) {
  if (values.length === 0) return null;
  const ordered = [...values].sort((a, b) => a - b);
  const position = (ordered.length - 1) * fraction;
  const low = Math.floor(position);
  const high = Math.ceil(position);
  return round(ordered[low] + (ordered[high] - ordered[low]) * (position - low), 3);
}

export function distribution(values) {
  return {
    samples: values.length,
    p50: percentile(values, 0.5),
    p95: percentile(values, 0.95),
    p99: percentile(values, 0.99),
    min: values.length ? round(Math.min(...values), 3) : null,
    max: values.length ? round(Math.max(...values), 3) : null,
  };
}

function numbers(events, type, field = 'value') {
  return events
    .filter((event) => event?.type === type && finite(event[field]))
    .map((event) => event[field]);
}

function batchKeys(events, type) {
  return new Set(events
    .filter((event) => event?.type === type && Number.isInteger(event.stream) && Number.isInteger(event.batch))
    .map((event) => `${event.stream}:${event.batch}`));
}

export function summarizeRun(run, environment = {}) {
  const events = Array.isArray(run.events) ? run.events : [];
  const summary = {};
  for (const field of [
    'run', 'startedAt', 'renderer', 'parseMode', 'sampleOrder', 'runCondition',
    'deviceId', 'fixture', 'sourceBytes', 'sourceHash', 'workloadSize',
    'workloadShape', 'schedule', 'seed', 'streams', 'traceEnabled',
    'sampleStride', 'batchWindowMs', 'droppedEvents', 'completionStatus',
    'runDurationMs', 'wallDurationMs',
  ]) summary[field] = run[field] ?? null;
  summary.buildRevision = run.buildRevision ?? environment.buildRevision ?? null;

  for (const [type, [name, field]] of Object.entries(samples)) {
    summary[name] = distribution(numbers(events, type, field));
  }

  const flushed = batchKeys(events, 'batch-flush');
  const rendered = batchKeys(events, 'renderer-update');
  const observed = batchKeys(events, 'first-visible-frame-estimate');
  const superseded = batchKeys(events, 'superseded-before-frame');
  summary.sampledBatches = {
    flushed: flushed.size,
    rendererUpdated: rendered.size,
    frameEstimated: observed.size,
    supersededBeforeFrame: superseded.size,
    unobservedAfterFlush: [...flushed].filter((key) => !observed.has(key) && !superseded.has(key)).length,
    supersededRate: rendered.size ? round(superseded.size / rendered.size, 4) : null,
  };
  summary.batchSizeChunks = distribution(numbers(events, 'batch-flush'));
  return summary;
}

export function groupIdentity(environment, summary) {
  const identity = {
    deviceId: summary.deviceId ?? environment.deviceId ?? null,
    deviceModel: environment.deviceModel ?? null,
    platform: environment.platform ?? null,
    platformVersion: environment.platformVersion ?? null,
    screen: environment.screen ?? null,
    buildRevision: summary.buildRevision,
  };
  for (const field of workloadFields) identity[field] = summary[field];
  return identity;
}

export function summarizeGroup(group) {
  const completed = group.filter((run) => run.completionStatus === 'complete');
  const traced = completed.filter((run) => run.traceEnabled === true);
  const untraced = completed.filter((run) => run.traceEnabled === false);
  const result = {
    runs: group.length,
    completedRuns: completed.length,
    traceOnRuns: traced.length,
    traceOffRuns: untraced.length,
    repeatedRunVariation: {},
  };
  for (const metric of ['chunkToFrameEstimateMs', 'jsFrameIntervalMs', 'deliveryLatenessMs']) {
    for (const statistic of ['p50', 'p95', 'p99']) {
      result.repeatedRunVariation[`${metric}.${statistic}`] = distribution(
        traced.map((run) => run[metric][statistic]).filter(finite)
      );
    }
  }
  result.runDurationMs = {
    traceOn: distribution(traced.map((run) => run.runDurationMs).filter(finite)),
    traceOff: distribution(untraced.map((run) => run.runDurationMs).filter(finite)),
  };
  const on = result.runDurationMs.traceOn.p50;
  const off = result.runDurationMs.traceOff.p50;
  result.traceOnMinusOffMedianRunDurationMs = on === null || off === null ? null : round(on - off, 3);
  return result;
}

export function summarizeFiles(paths) {
  const groups = new Map();
  const seen = new Set();
  const rows = [];
  for (const path of paths) {
    const exportData = JSON.parse(readFileSync(path, 'utf8'));
    if (exportData.schemaVersion !== 1 || !Array.isArray(exportData.runs)) {
      throw new Error(`${path}: unsupported app trace export`);
    }
    const environment = exportData.environment ?? {};
    for (const run of exportData.runs) {
      const summary = summarizeRun(run, environment);
      const identity = groupIdentity(environment, summary);
      const groupKey = JSON.stringify(identity);
      const runKey = JSON.stringify([groupKey, summary.startedAt, summary.run]);
      if (seen.has(runKey)) continue;
      seen.add(runKey);
      rows.push({ kind: 'run', file: path, ...summary });
      if (!groups.has(groupKey)) groups.set(groupKey, []);
      groups.get(groupKey).push(summary);
    }
  }
  for (const [key, group] of [...groups.entries()].sort(([a], [b]) => a.localeCompare(b))) {
    rows.push({ kind: 'group', identity: JSON.parse(key), ...summarizeGroup(group) });
  }
  return rows;
}

if (process.argv[1] && fileURLToPath(import.meta.url) === resolve(process.argv[1])) {
  if (process.argv.length < 3) {
    console.error('Usage: node benchmarks/summarize.mjs <trace.json> [trace.json ...]');
    process.exitCode = 2;
  } else {
    try {
      for (const row of summarizeFiles(process.argv.slice(2))) console.log(JSON.stringify(row));
    } catch (error) {
      console.error(error.message);
      process.exitCode = 1;
    }
  }
}

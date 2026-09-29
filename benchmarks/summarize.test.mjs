import assert from 'node:assert/strict';
import { writeFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { groupIdentity, summarizeFiles, summarizeGroup, summarizeRun } from './summarize.mjs';

function run(overrides = {}) {
  return {
    run: 1, startedAt: '2026-09-28T00:00:00.000Z', renderer: 'baseline',
    fixture: 'paragraph', sourceHash: 'abc', workloadSize: 'fixture',
    workloadShape: 'repeat', schedule: 'bursty', seed: 7, streams: 1,
    traceEnabled: true, completionStatus: 'complete', runDurationMs: 100,
    wallDurationMs: 100, events: [], ...overrides,
  };
}

const event = (type, batch, details = {}) => ({ type, stream: 0, batch, ...details });

test('superseded batches are counted outside latency samples', () => {
  const summary = summarizeRun(run({ events: [
    event('batch-flush', 1, { value: 2 }),
    event('renderer-update', 1),
    event('superseded-before-frame', 1),
    event('batch-flush', 2, { value: 1 }),
    event('renderer-update', 2),
    event('first-visible-frame-estimate', 2),
    event('arrival-to-frame-sample', 2, { value: 12.5 }),
    event('batch-delivery', 2, { latenessMs: 3 }),
  ] }));
  assert.equal(summary.chunkToFrameEstimateMs.samples, 1);
  assert.equal(summary.sampledBatches.supersededBeforeFrame, 1);
  assert.equal(summary.sampledBatches.supersededRate, 0.5);
  assert.equal(summary.deliveryLatenessMs.p50, 3);
});

test('group excludes cancelled runs and compares trace toggles', () => {
  const group = summarizeGroup([
    summarizeRun(run()), summarizeRun(run({ runDurationMs: 120 })),
    summarizeRun(run({ traceEnabled: false, runDurationMs: 80 })),
    summarizeRun(run({ completionStatus: 'cancelled', runDurationMs: 1000 })),
  ]);
  assert.equal(group.completedRuns, 3);
  assert.equal(group.runDurationMs.traceOn.p50, 110);
  assert.equal(group.traceOnMinusOffMedianRunDurationMs, 30);
});

test('overlapping exports count each run once', () => {
  const directory = mkdtempSync(join(tmpdir(), 'downstream-bench-'));
  const paths = [join(directory, 'first.json'), join(directory, 'second.json')];
  const first = run();
  const second = run({ run: 2, startedAt: '2026-09-28T00:01:00.000Z' });
  [ [first], [first, second] ].forEach((runs, index) =>
    writeFileSync(paths[index], JSON.stringify({ schemaVersion: 1, environment: {}, runs }))
  );
  const rows = summarizeFiles(paths);
  assert.deepEqual(rows.filter((row) => row.kind === 'run').map((row) => row.run), [1, 2]);
  assert.deepEqual(rows.filter((row) => row.kind === 'group').map((row) => row.runs), [2]);
});

test('candidate, build, device and cold state have separate groups', () => {
  const variants = [
    summarizeRun(run(), { buildRevision: 'a' }),
    summarizeRun(run({ renderer: 'downstream-full', parseMode: 'full' }), { buildRevision: 'a' }),
    summarizeRun(run({ renderer: 'downstream-incremental', parseMode: 'incremental' }), { buildRevision: 'a' }),
    summarizeRun(run(), { buildRevision: 'b' }),
    summarizeRun(run({ runCondition: 'cold' }), { buildRevision: 'a' }),
  ];
  const keys = variants.map((summary) => JSON.stringify(groupIdentity({ deviceModel: 'phone' }, summary)));
  assert.equal(new Set(keys).size, variants.length);
  assert.notDeepEqual(groupIdentity({ deviceModel: 'phone' }, variants[0]),
    groupIdentity({ deviceModel: 'other' }, variants[0]));
});

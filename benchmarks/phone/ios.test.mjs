import assert from 'node:assert/strict';
import test from 'node:test';
import { hitchTimeRatio, iosRaw, traceMetrics, wallToTrace } from './bench.mjs';

const ms = 1e6;
const T = 1_700_000_000_000;

// Samples every 100 ms. Main burns 50 ms per interval, the JS thread 20 ms in
// the second interval only, and a worker exits after the first interval.
const sampler = [
  `s ${T} 0 ${100 * 1048576}`,
  'n 4096224 ',
  'n 6100000000 com.facebook.react.runtime.JavaScript',
  'n 6200000000 worker',
  't 4096224 0',
  't 6100000000 0',
  't 6200000000 0',
  `s ${T + 100} ${80 * ms} ${110 * 1048576}`,
  `t 4096224 ${50 * ms}`,
  `t 6200000000 ${30 * ms}`,
  `s ${T + 200} ${160 * ms} ${130 * 1048576}`,
  `t 4096224 ${100 * ms}`,
  `t 6100000000 ${20 * ms}`,
  `s ${T + 300} ${210 * ms} ${120 * 1048576}`,
  `t 4096224 ${150 * ms}`,
].join('\n');

// Ticks every 10 ms with one 40 ms hitch; work in three of the intervals.
const ticks = [0, 10, 20, 60, 70, 80, 90, 100, 110, 120];
const frames = [
  'm 4096000 100',
  // Mach 50 ms is wall time T, so ticks land 100 ms after the first sample.
  `c ${T} 50`,
  ...ticks.map((tick) => `v ${50 + 100 + tick}`),
  `b ${50 + 100 + 12} ${50 + 100 + 15}`,
  `b ${50 + 100 + 25} ${50 + 100 + 55}`,
  `b ${50 + 100 + 95} ${50 + 100 + 96}`,
].join('\n');
const raw = iosRaw(sampler, frames, 4242);
const at = (wallMs) => wallToTrace(raw.clockOffsetUs, T + wallMs);

test('splits each sample interval evenly and keeps exited threads', () => {
  const metrics = traceMetrics(raw, {
    start: at(50),
    deliveryEnd: at(250),
    end: at(250),
  });
  assert.equal(raw.threads.find((thread) => thread.tid === 4242).name, 'main');
  // Half of the first and last intervals plus the whole second one.
  assert.ok(Math.abs(metrics.cpuMainS - 0.1) < 1e-9);
  assert.ok(Math.abs(metrics.cpuJsS - 0.02) < 1e-9);
  // Half the worker's 30 ms plus 10 ms that no listed thread accounts for.
  assert.ok(Math.abs(metrics.cpuOtherS - 0.025) < 1e-9);
  // Process CPU is the source of truth: 40 + 80 + 25 ms.
  assert.ok(Math.abs(metrics.cpuTotalS - 0.145) < 1e-9);
  assert.equal(metrics.memPeakIncreaseMiB, 30);
});

test('frames are refresh intervals with main-thread work', () => {
  const metrics = traceMetrics(raw, {
    start: at(100),
    deliveryEnd: at(150),
    end: at(300),
  });
  assert.equal(raw.ios.vsyncMs, 10);
  assert.equal(metrics.frames, 3);
  assert.equal(metrics.jankAppPct, 100 / 3);
  assert.equal(metrics.frameMaxMs, 40);
  // The last busy turn ends at 196 ms, inside the 190-200 ms interval.
  assert.equal(metrics.settleMs, 50);
  const ratio = hitchTimeRatio(raw, { start: at(100), end: at(300) });
  assert.ok(Math.abs(ratio - 150) < 1e-9, 'a 30 ms hitch in 0.2 s');
});

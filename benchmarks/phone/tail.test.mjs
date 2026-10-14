import assert from 'node:assert/strict';
import test from 'node:test';
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { replayFinishes } from '../../example/src/bench/replayFinish.ts';
import {
  appOptionErrors,
  candidatesForProfile,
  lastDeliveryWall,
  lastFinishWall,
  loadResumeState,
  nativePhaseSummary,
  profileConfig,
  schedule,
  shouldKeepTrace,
  traceMetrics,
  validateFinishDelay,
} from './bench.mjs';
import { estimateCompletionWait, tailRow } from './tail-compare.mjs';

const ms = 1e6;
function raw(frames, sched = []) {
  return {
    pid: 10,
    threads: [
      { utid: 1, tid: 10, name: 'main' },
      { utid: 2, tid: 11, name: 'mqt_v_js' },
      { utid: 3, tid: 12, name: 'RenderThread' },
    ],
    frames: frames.map(([at, duration, jank = 'None']) => [
      at * ms,
      duration * ms,
      jank,
    ]),
    sched: sched.map(([thread, at, duration]) => [
      thread,
      at * ms,
      duration * ms,
    ]),
    memory: [],
  };
}
const window = {
  start: 0,
  deliveryEnd: 100 * ms,
  finishEnd: 3000 * ms,
  end: 5000 * ms,
};

test('late finish can produce a long last-text tail with little post-finish drawing', () => {
  const metrics = traceMetrics(
    raw(
      [
        [110, 10],
        [3000, 30],
      ],
      [[1, 2990, 30]]
    ),
    window
  );
  assert.equal(metrics.settleMs, 2930);
  assert.equal(metrics.textToFinishMs, 2900);
  assert.equal(metrics.settleAfterFinishMs, 30);
  assert.equal(metrics.tail.beforeFinish.cpuMainS, 0.01);
  assert.equal(metrics.tail.afterFinish.cpuMainS, 0.02);
  assert.equal(metrics.tail.afterFinish.frames, 1);
});

test('provider wait alone does not imply renderer work or a long last-text tail', () => {
  const metrics = traceMetrics(raw([[110, 10]]), window);
  assert.equal(metrics.settleMs, 20);
  assert.equal(metrics.textToFinishMs, 2900);
  assert.equal(metrics.settleAfterFinishMs, 0);
  assert.equal(metrics.tail.afterFinish.frames, 0);
  assert.equal(metrics.tail.afterFinish.cpuTotalS, 0);
});

test('post-finish attribution includes crossing frames and clips CPU at the boundary', () => {
  const metrics = traceMetrics(
    raw(
      [
        [2980, 60, 'App Deadline Missed'],
        [3300, 70],
      ],
      [
        [2, 2990, 30],
        [3, 3300, 70],
      ]
    ),
    window
  );
  assert.equal(metrics.settleAfterFinishMs, 370);
  assert.equal(metrics.tail.afterFinish.frames, 2);
  assert.equal(metrics.tail.afterFinish.slowFrames, 2);
  assert.equal(metrics.tail.afterFinish.appDeadlineMissedFrames, 1);
  assert.equal(metrics.tail.afterFinish.cpuJsS, 0.02);
  assert.equal(metrics.tail.afterFinish.cpuRenderS, 0.07);
  assert.deepEqual(metrics.tail.afterFinish.cpuByThreadS, {
    RenderThread: 0.07,
    mqt_v_js: 0.02,
  });
});

test('missing legacy finish stays unknown; open frames do not alter duration metrics', () => {
  const metrics = traceMetrics(
    raw([
      [110, 10],
      [4999, -1],
    ]),
    { ...window, finishEnd: null }
  );
  assert.equal(metrics.frames, 1);
  assert.equal(metrics.frameMaxMs, 10);
  assert.equal(metrics.textToFinishMs, null);
  assert.equal(metrics.settleAfterFinishMs, null);
  assert.equal(metrics.tail.afterFinish, null);
});

test('measurement boundary crossing is censored and retained', () => {
  const metrics = traceMetrics(raw([[4990, 30]]), window);
  assert.equal(metrics.tailWindowCensored, true);
  assert.equal(
    shouldKeepTrace({}, { block: 3, workload: 'ai-single' }, metrics),
    true
  );
});

test('open and subsequent app frames retain evidence without changing measured completed-frame metrics', () => {
  for (const extraFrame of [
    [4999, -1],
    [5010, 20],
  ]) {
    const metrics = traceMetrics(raw([[110, 10], extraFrame]), window);
    assert.equal(metrics.tailWindowCensored, true);
    assert.equal(metrics.frames, 1);
    assert.equal(metrics.settleMs, 20);
    assert.equal(metrics.frameMaxMs, 10);
    assert.equal(
      shouldKeepTrace({}, { block: 3, workload: 'ai-single' }, metrics),
      true
    );
  }
  const beforeWindow = traceMetrics(
    raw([
      [-20, -1],
      [110, 10],
    ]),
    window
  );
  assert.equal(beforeWindow.tailWindowCensored, false);
});

test('slow and failed traces survive including huge; fast baseline policy is preserved', () => {
  const huge = { block: 0, workload: 'ai-huge-concurrent-5' };
  assert.equal(shouldKeepTrace({}, huge, { settleMs: 2937 }), true);
  assert.equal(shouldKeepTrace({}, huge, { settleMs: 100 }), false);
  assert.equal(shouldKeepTrace({}, huge, {}, true), true);
  assert.equal(shouldKeepTrace({ keepTraces: true }, huge, {}), true);
  assert.equal(
    shouldKeepTrace(
      {},
      { block: 0, workload: 'ai-feature-matrix' },
      { settleMs: 20 }
    ),
    true
  );
  assert.equal(
    shouldKeepTrace(
      {},
      { block: 1, workload: 'ai-feature-matrix' },
      { settleMs: 500 }
    ),
    false
  );
  assert.equal(
    shouldKeepTrace(
      {},
      { block: 1, workload: 'ai-feature-matrix' },
      { settleMs: 501 }
    ),
    true
  );
});

test('exact delivery and concurrent finish receipt boundaries do not use done timing', () => {
  const app = {
    threadCount: 2,
    startWallTimeMs: 1000,
    deliveryDurationMs: 5100,
    deliveries: [
      { actualMs: 5, wallTimeMs: 1005 },
      { actualMs: 40, wallTimeMs: 1041 },
    ],
    finishes: [
      { thread: 0, wallTimeMs: 1030 },
      { thread: 1, wallTimeMs: 1060 },
    ],
    streamDone: { wallTimeMs: 6000 },
  };
  assert.equal(lastDeliveryWall(app, 1000), 1041);
  assert.equal(lastFinishWall(app), 1060);
  assert.equal(
    lastFinishWall({ ...app, finishes: app.finishes.slice(0, 1) }),
    null
  );
  const row = tailRow({ config: {}, app, metrics: {} });
  assert.equal(row.textToFinishMs, 19);
  assert.equal(row.finishToDoneMs, 4940);
  assert.equal(row.estimatedCompletionWaitMs, null);
});

test('legacy host span estimate is explicitly separate from exact app finish timing', () => {
  const record = {
    config: {},
    app: { deliveryDurationMs: 5100 },
    live: [
      { firstDeltaMs: 100, lastDeltaMs: 2200 },
      { firstDeltaMs: 200, lastDeltaMs: 2400 },
    ],
  };
  assert.equal(estimateCompletionWait(record), 2800);
  assert.equal(tailRow(record).textToFinishMs, null);
  assert.equal(
    estimateCompletionWait({
      ...record,
      live: [{ firstDeltaMs: null, lastDeltaMs: 2 }],
    }),
    null
  );
});

test('animation profiles preserve syntax highlighting and exclude unsupported candidates', () => {
  assert.deepEqual(profileConfig('highlighted'), {
    syntaxHighlighting: true,
    animated: false,
  });
  assert.deepEqual(profileConfig('highlighted-animated'), {
    syntaxHighlighting: true,
    animated: true,
  });
  assert.deepEqual(profileConfig('plain-animated'), {
    syntaxHighlighting: false,
    animated: true,
  });
  assert.throws(() => profileConfig('unknown'), /Unknown profile/);
  const candidates = ['marked', 'downstream-default', 'enriched', 'streamdown'];
  assert.deepEqual(candidatesForProfile(candidates, 'plain'), candidates);
  assert.deepEqual(
    candidatesForProfile(candidates, 'plain-animated'),
    candidates.slice(1)
  );
  const profiles = ['highlighted', 'highlighted-animated'];
  const order = schedule(candidates, ['ai-feature-matrix'], profiles, 2, 9);
  assert.deepEqual(
    order,
    schedule(candidates, ['ai-feature-matrix'], profiles, 2, 9)
  );
  assert.equal(
    order.filter((item) => item.profile === 'highlighted').length,
    12
  );
  assert.equal(
    order.filter((item) => item.profile === 'highlighted-animated').length,
    9
  );
  assert.ok(
    order.every(
      (item) =>
        item.profile !== 'highlighted-animated' || item.candidate !== 'marked'
    )
  );
});

test('saved APKs must honor animation and settle options to qualify as measurements', () => {
  assert.deepEqual(
    appOptionErrors(
      { animated: true, settleMs: 2000 },
      { animationEnabled: true, settleMs: 2000 }
    ),
    []
  );
  assert.deepEqual(
    appOptionErrors({ animated: false, settleMs: 2000 }, { settleMs: 2000 }),
    []
  );
  assert.match(
    appOptionErrors({ animated: true, settleMs: 2000 }, { settleMs: 2000 })[0],
    /updated benchmark APK/
  );
  assert.match(
    appOptionErrors({ animated: false, settleMs: 5000 }, { settleMs: 2000 })[0],
    /settle budget differs/
  );
});

test('controlled replay finish delays run independently of other thread input', async () => {
  for (const delayMs of [0, 1000, 3000]) {
    let now = 0;
    const timers = [];
    const completions = [];
    const finish = replayFinishes(
      delayMs,
      (wait) =>
        new Promise((resolve) => {
          timers.push({ due: now + wait, resolve });
        }),
      (thread) => completions.push({ thread, at: now })
    );
    const deliveries = [];
    // Thread zero ends at0; thread one keeps its original delta schedule.
    deliveries.push({ thread: 0, dueMs: 0, at: now });
    finish.schedule(0);
    now = 100;
    deliveries.push({ thread: 1, dueMs: 100, at: now });
    now = 200;
    deliveries.push({ thread: 1, dueMs: 200, at: now });
    finish.schedule(1);
    finish.schedule(1); // Final chunks cannot schedule duplicate finish calls.
    let done = false;
    const all = finish.done().then(() => {
      done = true;
    });
    assert.deepEqual(
      deliveries.map((chunk) => chunk.at - chunk.dueMs),
      [0, 0, 0]
    );
    if (delayMs > 0) {
      assert.deepEqual(completions, []);
      assert.equal(done, false);
      for (const timer of timers) {
        now = timer.due;
        timer.resolve();
        await Promise.resolve();
      }
    }
    await all;
    assert.equal(done, true);
    assert.deepEqual(completions, [
      { thread: 0, at: delayMs },
      { thread: 1, at: delayMs + 200 },
    ]);
  }
});

test('replay-only finish delays are validated and outdated APKs cannot ignore them', () => {
  assert.equal(validateFinishDelay(0, false), 0);
  assert.equal(validateFinishDelay(1000, true), 1000);
  assert.equal(validateFinishDelay(3000, true), 3000);
  for (const invalid of [-1, NaN, Infinity, 2147483648])
    assert.throws(() => validateFinishDelay(invalid, true), /timer duration/);
  assert.throws(() => validateFinishDelay(1000, false), /requires --replay/);
  assert.deepEqual(
    appOptionErrors(
      { finishDelayMs: 1000, settleMs: 2000 },
      { finishDelayAppliedMs: 1000, settleMs: 2000 }
    ),
    []
  );
  assert.match(
    appOptionErrors(
      { finishDelayMs: 1000, settleMs: 2000 },
      { settleMs: 2000 }
    )[0],
    /ignored the replay finish delay/
  );
});

test('delayed finish failures remain observable through aggregate done', async () => {
  const finishes = replayFinishes(
    1000,
    () => Promise.resolve(),
    () => {
      throw new Error('native finish failed');
    }
  );
  finishes.schedule(0);
  await Promise.resolve();
  await Promise.resolve();
  await assert.rejects(finishes.done(), /native finish failed/);
});

test('resume refuses to mix finish delays or trace modes within a saved session', (t) => {
  const directory = mkdtempSync(join(tmpdir(), 'phone-bench-finish-'));
  t.after(() => rmSync(directory, { recursive: true, force: true }));
  mkdirSync(join(directory, 'runs'));
  const config = {
    index: 0,
    profile: 'highlighted',
    candidate: 'downstream-default',
    workload: 'ai-feature-matrix',
    block: 0,
    warmup: false,
  };
  writeFileSync(
    join(directory, 'session.json'),
    JSON.stringify({
      schemaVersion: 2,
      order: [config],
      input: { mode: 'replay' },
      finishDelayMs: 1000,
    })
  );
  const save = (overrides) =>
    writeFileSync(
      join(directory, 'runs', '0.json'),
      JSON.stringify({ status: 'ok', config: { ...config, ...overrides } })
    );
  save({ finishDelayMs: 1000 });
  assert.deepEqual(loadResumeState(directory).pending, []);
  save({ finishDelayMs: 3000 });
  assert.throws(
    () => loadResumeState(directory),
    /finish delay or tracing differs/
  );
  save({ finishDelayMs: 1000, trace: true });
  assert.throws(
    () => loadResumeState(directory),
    /finish delay or tracing differs/
  );
});

function native(
  event,
  atMs,
  sequence = 1,
  sessionId = 'a',
  generation = 0,
  threadId = 'worker'
) {
  return {
    event,
    timeNs: String(BigInt(atMs) * 1000000n),
    clockDomain: 'native-monotonic',
    sequence,
    sessionId,
    generation,
    threadId,
  };
}

test('native diagnostics separate queue, parsing, preparation and publication despite sequence advancement', () => {
  const summary = nativePhaseSummary({
    intervalComplete: true,
    events: [
      native('worker_queued', 0, 1),
      native('worker_start', 20, 2),
      native('parse_start', 20, 2),
      native('parse_end', 30, 2),
      native('prepare_start', 30, 2),
      native('prepare_end', 50, 2),
      native('snapshot_staged', 50, 2),
      native('snapshot_ready', 64, 2),
      {
        event: 'worker_start',
        clockDomain: 'js-performance',
        timeMs: 999999,
        sessionId: 'a',
      },
    ],
  });
  assert.equal(summary.phases.queueWait.all.totalMs, 20);
  assert.equal(summary.phases.parse.all.totalMs, 10);
  assert.equal(summary.phases.prepare.all.totalMs, 20);
  assert.equal(summary.phases.workerPass.all.totalMs, 30);
  assert.equal(summary.phases.publicationWait.all.totalMs, 14);
  assert.equal(summary.unmatchedEvents, 0);
  assert.equal(summary.nativeEvents, 8);
});

test('native diagnostics isolate concurrent sessions, generations and same-sequence finish publication', () => {
  const summary = nativePhaseSummary({
    intervalComplete: true,
    events: [
      native('worker_queued', 0, 1, 'a'),
      native('worker_queued', 1, 1, 'b'),
      native('worker_start', 5, 2, 'b'),
      native('worker_start', 10, 2, 'a'),
      native('finish_received', 12, 2, 'a'),
      native('snapshot_staged', 20, 2, 'a'),
      native('snapshot_ready', 24, 2, 'a'),
      native('worker_queued', 24, 2, 'a'),
      native('worker_start', 28, 2, 'a'),
      native('snapshot_staged', 40, 2, 'a'),
      native('snapshot_ready', 48, 2, 'a'),
      native('snapshot_staged', 18, 2, 'b'),
      native('snapshot_ready', 24, 2, 'b'),
      native('worker_queued', 49, 2, 'a', 0),
      native('worker_start', 50, 1, 'a', 1),
    ].reverse(),
  });
  assert.equal(summary.phases.queueWait.all.count, 3);
  assert.equal(summary.phases.queueWait.all.totalMs, 18);
  assert.equal(summary.phases.workerPass.afterFinish.totalMs, 20);
  assert.equal(summary.phases.publicationWait.afterFinish.totalMs, 12);
  assert.equal(summary.finishToLastObservedReady.maxMs, 36);
  assert.equal(summary.unmatchedEvents, 3); // Reset crosses generation; the final worker has no staged result.
});

test('native retirement remains separate from queue, parse and prepare while overlapping the full worker pass', () => {
  const summary = nativePhaseSummary({
    intervalComplete: true,
    events: [
      native('worker_queued', 0, 1),
      native('worker_start', 10, 2),
      native('retire_start', 10, 1),
      native('finish_received', 15, 2),
      native('retire_end', 20, 1),
      native('parse_start', 20, 2),
      native('parse_end', 30, 2),
      native('prepare_start', 30, 2),
      native('prepare_end', 50, 2),
      native('snapshot_staged', 50, 2),
      native('snapshot_ready', 64, 2),
    ],
  });
  assert.equal(summary.phases.queueWait.all.totalMs, 10);
  assert.equal(summary.phases.retirement.all.totalMs, 10);
  assert.equal(summary.phases.retirement.afterFinish.totalMs, 5);
  assert.equal(summary.phases.parse.all.totalMs, 10);
  assert.equal(summary.phases.prepare.all.totalMs, 20);
  assert.equal(summary.phases.workerPass.all.totalMs, 40);
  assert.equal(summary.phases.publicationWait.all.totalMs, 14);
  assert.equal(summary.unmatchedEvents, 0);
  assert.match(
    summary.notes,
    /UI-held payload references can be released later/
  );
});

test('standalone retirement pairs exact publication sequence and thread without inventing worker passes', () => {
  const summary = nativePhaseSummary({
    intervalComplete: true,
    events: [
      native('retire_start', 10, 5, 'a', 0, 'worker-a'),
      native('retire_start', 11, 5, 'a', 0, 'worker-b'),
      native('retire_end', 14, 5, 'a', 0, 'worker-b'),
      native('retire_end', 20, 5, 'a', 0, 'worker-a'),
      native('retire_start', 21, 6, 'a', 0, 'worker-a'),
      native('retire_end', 25, 7, 'a', 0, 'worker-a'),
    ],
  });
  assert.equal(summary.phases.retirement.all.count, 2);
  assert.equal(summary.phases.retirement.all.totalMs, 13);
  assert.equal(summary.phases.retirement.all.maxMs, 10);
  assert.equal(summary.phases.workerPass.all.count, 0);
  assert.equal(summary.phases.queueWait.all.count, 0);
  assert.equal(summary.unmatchedEvents, 2);
});

test('optional diagnostic data preserves truncation and trace-request validation', () => {
  const trace = {
    intervalComplete: false,
    droppedEvents: 50,
    events: [native('parse_end', 10), native('finish_received', 20)],
  };
  const summary = nativePhaseSummary(trace);
  assert.equal(summary.intervalComplete, false);
  assert.equal(summary.droppedEvents, 50);
  assert.equal(summary.unmatchedEvents, 2);
  assert.equal(summary.finishToLastObservedReady.count, 0);
  assert.match(
    appOptionErrors({ trace: true, settleMs: 2000 }, { settleMs: 2000 })[0],
    /did not collect/
  );
  assert.equal(
    tailRow({ config: {}, app: {}, nativeTrace: trace }).nativePhases
      .droppedEvents,
    50
  );
});

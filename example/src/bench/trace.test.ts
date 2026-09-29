import { TraceRun } from './trace';

test('trace-off runs still report completion and active duration', () => {
  const trace = new TraceRun({
    run: 1,
    renderer: 'baseline',
    parseMode: null,
    sessionIds: [],
    deviceId: 'simulator-a',
    runCondition: 'warm',
    sampleOrder: 1,
    fixture: 'emphasis',
    source: 'emphasis.md',
    sourceBytes: 20,
    sourceHash: 'adler32:12345678',
    workloadSize: 'fixture',
    workloadShape: 'repeat',
    schedule: 'manifest',
    seed: 17,
    streams: 1,
    traceEnabled: false,
    sampleStride: 1,
    batchWindowMs: 16,
  });

  trace.end('complete', 120);
  expect(trace.data.completionStatus).toBe('complete');
  expect(trace.data.runDurationMs).toBe(120);
  expect(trace.data.wallDurationMs).toBeGreaterThanOrEqual(0);
  expect(trace.data.events).toEqual([]);
});

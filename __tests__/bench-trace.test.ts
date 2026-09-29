import { TraceRun } from '../example/src/bench/trace';

test('trace-off runs still report completion and active duration', () => {
  const trace = new TraceRun({
    run: 1,
    renderer: 'downstream-full',
    parseMode: 'full',
    sessionIds: ['session-a'],
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

test('stream trace keeps matching sampled session events', () => {
  const trace = new TraceRun({
    run: 2,
    renderer: 'downstream-full',
    parseMode: 'full',
    sessionIds: ['session-a'],
    deviceId: 'simulator-a',
    runCondition: 'warm',
    sampleOrder: 2,
    fixture: 'emphasis',
    source: 'emphasis.md',
    sourceBytes: 20,
    sourceHash: 'adler32:12345678',
    workloadSize: 'fixture',
    workloadShape: 'repeat',
    schedule: 'range',
    seed: 17,
    streams: 1,
    traceEnabled: true,
    sampleStride: 2,
    batchWindowMs: 16,
  });
  const native = {
    sessionId: 'session-a',
    generation: 0,
    sequence: 2,
    event: 'parse_end',
    clockDomain: 'native-monotonic' as const,
    timeNs: '3000000',
  };
  trace.recordStreamEvent({ ...native, sequence: 1 });
  trace.recordStreamEvent({ ...native, sessionId: 'another-session' });
  trace.recordStreamEvent(native);
  expect(trace.data.streamEvents).toEqual([native]);
});

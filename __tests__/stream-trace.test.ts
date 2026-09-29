/**
 * The trace path, driven by real parses.
 *
 * Tracing exists so a benchmark can join a JavaScript timestamp to the native
 * one. That join is only meaningful if the native markers come from real parse
 * work, so the session TurboModule here is the engine-backed one: every
 * `parse_end` a test sees was produced by parsing a real document.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import {
  createMarkdownStream,
  getStreamState,
} from '../src/createMarkdownStream';
import {
  emitJsStreamTrace,
  setStreamTracing,
  subscribeStreamTrace,
  type StreamTraceEvent,
} from '../src/streamTrace';
import { nativeWork, resetEngine } from './real-engine-module';

vi.mock(
  '../src/NativeDownstreamSession',
  async () => (await import('./real-engine-module')).realEngineModule
);

const streams: { dispose(): void }[] = [];

function stream(): ReturnType<typeof createMarkdownStream> {
  const created = createMarkdownStream();
  streams.push(created);
  return created;
}

/**
 * Appends a real document, which makes the engine emit its native markers.
 * Tracing is per stream and off by default, so it is enabled here.
 */
function parseSomething(): void {
  const session = stream();
  setStreamTracing(session, true);
  session.append('# Title\n\nBody.\n');
  getStreamState(session).flush();
}

beforeEach(() => {
  resetEngine();
});

afterEach(() => {
  for (const created of streams.splice(0)) created.dispose();
});

describe('subscribeStreamTrace', () => {
  it('delivers the markers a real parse produced', () => {
    const received: StreamTraceEvent[] = [];
    const unsubscribe = subscribeStreamTrace((event) => received.push(event));

    parseSomething();

    const events = received.filter(
      (event) => event.clockDomain === 'native-monotonic'
    );
    expect(events.map((event) => event.event)).toContain('parse_end');
    expect(events.map((event) => event.event)).toContain('append_received');
    expect(events[0]?.timeNs).toMatch(/^\d+$/);
    unsubscribe();
  });

  it('pairs each native marker with a JavaScript delivery record', () => {
    const received: StreamTraceEvent[] = [];
    const unsubscribe = subscribeStreamTrace((event) => received.push(event));

    parseSomething();

    // For every native marker there is a JS record naming it, so a JS clock
    // reading can be joined to the native one that produced it.
    const native = received.filter(
      (event) => event.clockDomain === 'native-monotonic'
    );
    const joined = received.filter(
      (event) => event.event === 'native_event_received'
    );
    expect(joined).toHaveLength(native.length);
    for (const event of joined) {
      expect(event).toMatchObject({
        clockDomain: 'js-performance',
        nativeTimeNs: expect.stringMatching(/^\d+$/),
        nativeEvent: expect.any(String),
      });
    }
    unsubscribe();
  });

  it('gives every listener the same events', () => {
    const first = vi.fn();
    const second = vi.fn();
    const unsubscribeFirst = subscribeStreamTrace(first);
    const unsubscribeSecond = subscribeStreamTrace(second);

    parseSomething();

    expect(first).toHaveBeenCalled();
    expect(second.mock.calls.length).toBe(first.mock.calls.length);
    unsubscribeFirst();
    unsubscribeSecond();
  });

  it('stops after the last listener leaves', () => {
    const listener = vi.fn();
    const unsubscribeFirst = subscribeStreamTrace(listener);
    const unsubscribeSecond = subscribeStreamTrace(vi.fn());
    unsubscribeFirst();
    unsubscribeSecond();
    listener.mockClear();

    parseSomething();

    expect(listener).not.toHaveBeenCalled();
  });

  it('resubscribes after every listener has left', () => {
    const unsubscribeFirst = subscribeStreamTrace(vi.fn());
    const unsubscribeSecond = subscribeStreamTrace(vi.fn());
    unsubscribeFirst();
    unsubscribeSecond();

    const listener = vi.fn();
    const unsubscribe = subscribeStreamTrace(listener);
    parseSomething();
    expect(listener).toHaveBeenCalled();
    unsubscribe();
  });
});

describe('emitJsStreamTrace', () => {
  it('does no work when nothing is listening', () => {
    const before = performance.now();
    emitJsStreamTrace('downstream-a', 0, 1, 'receipt');
    // Nothing observable happens, and in particular nothing throws.
    expect(performance.now()).toBeGreaterThanOrEqual(before);
  });
});

describe('setStreamTracing', () => {
  it('records JavaScript receipts once a real parse runs', () => {
    const session = stream();
    const received: StreamTraceEvent[] = [];
    const unsubscribe = subscribeStreamTrace((event) => received.push(event));
    setStreamTracing(session, true);

    session.append('# Title\n');
    getStreamState(session).flush();

    const receipts = received.filter((event) => event.event === 'receipt');
    expect(receipts.length).toBeGreaterThan(0);
    expect(receipts[0]).toMatchObject({
      clockDomain: 'js-performance',
      sequence: 1,
    });
    unsubscribe();
  });

  it('is silent until tracing is enabled', () => {
    const session = stream();
    const received: StreamTraceEvent[] = [];
    const unsubscribe = subscribeStreamTrace((event) => received.push(event));

    session.append('# Title\n');
    getStreamState(session).flush();

    expect(received).toEqual([]);
    unsubscribe();
  });

  it('turns off again', () => {
    const session = stream();
    const received: StreamTraceEvent[] = [];
    const unsubscribe = subscribeStreamTrace((event) => received.push(event));

    setStreamTracing(session, true);
    session.append('a');
    getStreamState(session).flush();
    setStreamTracing(session, false);
    received.length = 0;
    session.append('b');
    getStreamState(session).flush();

    expect(received).toEqual([]);
    unsubscribe();
  });
});

describe('what the trace records', () => {
  it('shows a real parse happening once per append', () => {
    const received: StreamTraceEvent[] = [];
    const unsubscribe = subscribeStreamTrace((event) => received.push(event));
    const before = nativeWork.parses;

    parseSomething();

    expect(nativeWork.parses).toBeGreaterThan(before);
    expect(
      received.filter((event) => event.event === 'parse_end')
    ).toHaveLength(nativeWork.parses - before);
    unsubscribe();
  });
});

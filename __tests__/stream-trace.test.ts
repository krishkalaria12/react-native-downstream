import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { createMarkdownStream } from '../src/createMarkdownStream';
import NativeDownstreamSession from '../src/NativeDownstreamSession';
import * as NativeDownstreamSessionModule from '../src/NativeDownstreamSession';
import {
  emitJsStreamTrace,
  setStreamTracing,
  subscribeStreamTrace,
  type StreamTraceEvent,
} from '../src/streamTrace';

vi.mock('../src/NativeDownstreamSession', () => {
  const listeners = { trace: new Set<Function>() };
  return {
    __esModule: true,
    default: {
      create: vi.fn(),
      append: vi.fn(),
      reset: vi.fn(),
      finish: vi.fn(),
      dispose: vi.fn(),
      setTracing: vi.fn(),
      onSnapshotReady: vi.fn(() => ({ remove: vi.fn() })),
      onSessionError: vi.fn(() => ({ remove: vi.fn() })),
      onTrace: vi.fn((callback: (event: object) => void) => {
        listeners.trace.add(callback);
        return { remove: () => listeners.trace.delete(callback) };
      }),
    },
    emitTrace(event: object) {
      for (const listener of listeners.trace) listener(event);
    },
  };
});

const native = vi.mocked(NativeDownstreamSession);
const events = NativeDownstreamSessionModule as unknown as {
  emitTrace(event: object): void;
};

const streams: { dispose(): void }[] = [];

function stream(): ReturnType<typeof createMarkdownStream> {
  const created = createMarkdownStream();
  streams.push(created);
  return created;
}

beforeEach(() => {
  vi.clearAllMocks();
});

afterEach(() => {
  for (const created of streams.splice(0)) created.dispose();
});

describe('subscribeStreamTrace', () => {
  it('subscribes to native once, no matter how many listeners', () => {
    const first = vi.fn();
    const second = vi.fn();
    const unsubscribeFirst = subscribeStreamTrace(first);
    const unsubscribeSecond = subscribeStreamTrace(second);

    expect(native.onTrace).toHaveBeenCalledTimes(1);

    unsubscribeFirst();
    // The second listener is still live, so the native subscription stays.
    expect(native.onTrace).toHaveBeenCalledTimes(1);
    unsubscribeSecond();
  });

  it('delivers native events to every listener', () => {
    const first = vi.fn();
    const second = vi.fn();
    const unsubscribeFirst = subscribeStreamTrace(first);
    const unsubscribeSecond = subscribeStreamTrace(second);

    events.emitTrace({
      sessionId: 'downstream-a',
      generation: 0,
      sequence: 1,
      event: 'parse_end',
      clockDomain: 'native-monotonic',
      timeNs: '42',
    });

    expect(first).toHaveBeenCalledTimes(2);
    expect(second).toHaveBeenCalledTimes(2);
    unsubscribeFirst();
    unsubscribeSecond();
  });

  it('pairs a native event with a JS delivery record', () => {
    const received: StreamTraceEvent[] = [];
    const unsubscribe = subscribeStreamTrace((event) => received.push(event));

    events.emitTrace({
      sessionId: 'downstream-a',
      generation: 2,
      sequence: 7,
      event: 'parse_end',
      clockDomain: 'native-monotonic',
      timeNs: '99',
    });

    // The native event, then a JS record that points back at it, so a JS
    // timestamp can be joined to the native one.
    expect(received).toHaveLength(2);
    expect(received[0]).toMatchObject({ event: 'parse_end', timeNs: '99' });
    expect(received[1]).toMatchObject({
      event: 'native_event_received',
      sessionId: 'downstream-a',
      generation: 2,
      sequence: 7,
      nativeEvent: 'parse_end',
      nativeTimeNs: '99',
      clockDomain: 'js-performance',
    });
    unsubscribe();
  });

  it('does no work when nothing is listening', () => {
    const before = performance.now();
    emitJsStreamTrace('downstream-a', 0, 1, 'receipt');
    expect(performance.now()).toBeGreaterThanOrEqual(before);
    // Nothing observable happens, and in particular nothing throws.
    expect(native.onTrace).not.toHaveBeenCalled();
  });

  it('stops publishing after the last listener unsubscribes', () => {
    const listener = vi.fn();
    const unsubscribe = subscribeStreamTrace(listener);
    unsubscribe();

    events.emitTrace({
      sessionId: 'downstream-a',
      generation: 0,
      sequence: 1,
      event: 'parse_end',
      clockDomain: 'native-monotonic',
      timeNs: '1',
    });

    expect(listener).not.toHaveBeenCalled();
  });

  it('resubscribes after every listener has left', () => {
    const first = vi.fn();
    const unsubscribeFirst = subscribeStreamTrace(first);
    const unsubscribeSecond = subscribeStreamTrace(vi.fn());
    unsubscribeFirst();
    unsubscribeSecond();
    vi.mocked(native.onTrace).mockClear();

    const listener = vi.fn();
    const unsubscribe = subscribeStreamTrace(listener);
    expect(native.onTrace).toHaveBeenCalledTimes(1);
    unsubscribe();
  });
});

describe('setStreamTracing', () => {
  it('enables tracing on one stream only', () => {
    const traced = stream();
    const quiet = stream();
    setStreamTracing(traced, true);

    expect(native.setTracing).toHaveBeenCalledTimes(1);
    expect(native.setTracing).toHaveBeenCalledWith(
      native.create.mock.calls[0]?.[0],
      true
    );
    void quiet;
  });

  it('emits a JS receipt for each append while tracing', () => {
    const session = stream();
    const received: StreamTraceEvent[] = [];
    const unsubscribe = subscribeStreamTrace((event) => received.push(event));
    setStreamTracing(session, true);

    session.append('hello');

    expect(received).toMatchObject([
      { event: 'receipt', sequence: 1, clockDomain: 'js-performance' },
    ]);
    unsubscribe();
  });

  it('is silent until tracing is enabled', () => {
    const session = stream();
    const received: StreamTraceEvent[] = [];
    const unsubscribe = subscribeStreamTrace((event) => received.push(event));

    session.append('hello');

    expect(received).toEqual([]);
    unsubscribe();
  });
});

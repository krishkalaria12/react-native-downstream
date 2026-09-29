import {
  createMarkdownStream,
  createMarkdownStreamWithMode,
  getStreamState,
} from '../createMarkdownStream';
import NativeDownstreamSession from '../NativeDownstreamSession';
import {
  setStreamTracing,
  subscribeStreamTrace,
  type StreamTraceEvent,
} from '../streamTrace';

jest.mock('../NativeDownstreamSession', () => {
  const listeners = {
    snapshot: new Set(),
    error: new Set(),
    trace: new Set(),
  };
  return {
    __esModule: true,
    default: {
      create: jest.fn(),
      append: jest.fn(),
      reset: jest.fn(),
      finish: jest.fn(),
      dispose: jest.fn(),
      setTracing: jest.fn(),
      onSnapshotReady: jest.fn((callback: (event: object) => void) => {
        listeners.snapshot.add(callback);
        return { remove: () => listeners.snapshot.delete(callback) };
      }),
      onSessionError: jest.fn((callback: (event: object) => void) => {
        listeners.error.add(callback);
        return { remove: () => listeners.error.delete(callback) };
      }),
      onTrace: jest.fn((callback: (event: object) => void) => {
        listeners.trace.add(callback);
        return { remove: () => listeners.trace.delete(callback) };
      }),
    },
    emitSnapshot: (event: object) =>
      listeners.snapshot.forEach((callback) => {
        if (typeof callback === 'function') callback(event);
      }),
    emitError: (event: object) =>
      listeners.error.forEach((callback) => {
        if (typeof callback === 'function') callback(event);
      }),
    emitTrace: (event: object) =>
      listeners.trace.forEach((callback) => {
        if (typeof callback === 'function') callback(event);
      }),
  };
});

const native = jest.mocked(NativeDownstreamSession);
const events = jest.requireMock('../NativeDownstreamSession') as {
  emitSnapshot(event: object): void;
  emitError(event: object): void;
  emitTrace(event: object): void;
};

let frames: Array<Parameters<typeof requestAnimationFrame>[0]>;

beforeEach(() => {
  jest.clearAllMocks();
  frames = [];
  jest
    .spyOn(globalThis, 'requestAnimationFrame')
    .mockImplementation((callback) => {
      frames.push(callback);
      return frames.length;
    });
  jest.spyOn(globalThis, 'cancelAnimationFrame').mockImplementation(() => {});
});

function runFrames(): void {
  const pending = frames;
  frames = [];
  pending.forEach((callback) => callback(0));
}

afterEach(() => {
  jest.restoreAllMocks();
});

describe('createMarkdownStream', () => {
  it('batches ordered appends per frame and flushes before finish', async () => {
    const stream = createMarkdownStream();
    const id = getStreamState(stream).binding().sessionId;
    expect(native.create).toHaveBeenCalledWith(id, 'full');
    stream.append('hello ');
    stream.append('👋');
    expect(native.append).not.toHaveBeenCalled();
    expect(await stream.getSource()).toBe('hello 👋');
    stream.finish();
    expect(native.append).toHaveBeenCalledWith(id, 'hello 👋', 0, 2);
    expect(native.finish).toHaveBeenCalledWith(id, 0, 2);
    stream.finish();
    expect(native.finish).toHaveBeenCalledTimes(1);
    expect(() => stream.append('again')).toThrow(
      expect.objectContaining({ code: 'invalid_state' })
    );
    stream.dispose();
  });

  it('reports a failed finish and permits a retry without losing accepted source', async () => {
    const errors: object[] = [];
    const stream = createMarkdownStream({
      onError: (error) => errors.push(error),
    });
    const id = getStreamState(stream).binding().sessionId;
    jest.mocked(native.append).mockImplementationOnce(() => {
      throw new Error('unavailable');
    });

    stream.append('a');
    expect(() => stream.finish()).toThrow(
      expect.objectContaining({ code: 'render_failed' })
    );
    expect(native.finish).not.toHaveBeenCalled();
    expect(errors).toMatchObject([{ code: 'render_failed', sequence: 1 }]);
    expect(await stream.getSource()).toBe('a');

    stream.append('b');
    stream.finish();
    expect(native.append).toHaveBeenLastCalledWith(id, 'ab', 0, 2);
    expect(native.finish).toHaveBeenCalledWith(id, 0, 2);
    expect(await stream.getSource()).toBe('ab');
    expect(() => stream.append('c')).toThrow(
      expect.objectContaining({ code: 'invalid_state' })
    );
    stream.dispose();
  });

  it('splits large frame batches at 64 KiB and bounds unacknowledged bytes', () => {
    const stream = createMarkdownStream();
    const id = getStreamState(stream).binding().sessionId;
    for (let i = 0; i < 4; i++) stream.append('a'.repeat(64 * 1024));
    expect(() => stream.append('b')).toThrow(
      expect.objectContaining({ code: 'input_limit' })
    );
    frames[0]?.(0);
    expect(native.append).toHaveBeenCalledTimes(4);
    expect(native.append).toHaveBeenLastCalledWith(
      id,
      'a'.repeat(64 * 1024),
      0,
      4
    );
    events.emitSnapshot({ sessionId: id, generation: 0, sequence: 4 });
    stream.append('b');
    stream.dispose();
  });

  it('validates before mutation and reset clears a finished generation', async () => {
    const stream = createMarkdownStream();
    stream.append('ok');
    expect(() => stream.append('\ud800')).toThrow(
      expect.objectContaining({ code: 'invalid_input' })
    );
    expect(await stream.getSource()).toBe('ok');
    stream.finish();
    expect(() => stream.reset('\udfff')).toThrow(
      expect.objectContaining({ code: 'invalid_input' })
    );
    expect(await stream.getSource()).toBe('ok');
    stream.reset('new');
    expect(getStreamState(stream).binding()).toMatchObject({
      generation: 1,
      sequence: 0,
    });
    stream.append('!');
    expect(await stream.getSource()).toBe('new!');
    stream.dispose();
    await expect(stream.getSource()).rejects.toMatchObject({
      code: 'invalid_state',
    });
    expect(() => stream.reset()).toThrow(
      expect.objectContaining({ code: 'invalid_state' })
    );
  });

  it('enforces one view binding and keeps the session alive after unbind', () => {
    const stream = createMarkdownStreamWithMode({}, 'incremental');
    const state = getStreamState(stream);
    expect(native.create).toHaveBeenCalledWith(
      state.binding().sessionId,
      'incremental'
    );
    const unbind = state.bind();
    expect(() => state.bind()).toThrow(
      expect.objectContaining({ code: 'session_bound' })
    );
    unbind();
    expect(native.dispose).not.toHaveBeenCalled();
    state.bind()();
    stream.dispose();
    stream.dispose();
    expect(native.dispose).toHaveBeenCalledTimes(1);
  });

  it('drops stale events and repaints on repeated current snapshots', () => {
    const errors: object[] = [];
    const stream = createMarkdownStream({
      onError: (error) => errors.push(error),
    });
    const state = getStreamState(stream);
    const id = state.binding().sessionId;
    const renders = jest.fn();
    state.subscribe(renders);
    stream.append('a');
    state.flush();
    stream.reset('b');
    const revision = state.binding().revision;
    events.emitSnapshot({ sessionId: id, generation: 0, sequence: 1 });
    events.emitError({
      sessionId: id,
      generation: 0,
      sequence: 1,
      code: 'parse_failed',
      message: 'old',
      recoverable: true,
    });
    expect(state.binding().revision).toBe(revision);
    expect(errors).toHaveLength(0);
    // Snapshots render once per frame; a repeated snapshot in a later frame,
    // as after finish, renders again.
    events.emitSnapshot({ sessionId: id, generation: 1, sequence: 0 });
    events.emitSnapshot({ sessionId: id, generation: 1, sequence: 0 });
    expect(state.binding().revision).toBe(revision);
    runFrames();
    expect(state.binding().revision).toBe(revision + 1);
    events.emitSnapshot({ sessionId: id, generation: 1, sequence: 0 });
    runFrames();
    expect(state.binding().revision).toBe(revision + 2);
    expect(renders).toHaveBeenCalledTimes(3);
    events.emitError({
      sessionId: id,
      generation: 1,
      sequence: 0,
      code: 'parse_failed',
      message: 'current',
      recoverable: true,
    });
    expect(errors).toMatchObject([
      { code: 'parse_failed', message: 'current' },
    ]);
    stream.dispose();
  });

  it('keeps native stage and timestamp with its JS delivery record', () => {
    const records: StreamTraceEvent[] = [];
    const unsubscribe = subscribeStreamTrace((event) => records.push(event));
    const stream = createMarkdownStream();
    const id = getStreamState(stream).binding().sessionId;
    setStreamTracing(stream, true);
    stream.append('a');
    events.emitTrace({
      sessionId: id,
      generation: 0,
      sequence: 1,
      event: 'parse_end',
      clockDomain: 'native-monotonic',
      timeNs: '123456789',
    });
    expect(records).toEqual(
      expect.arrayContaining([
        expect.objectContaining({
          sessionId: id,
          event: 'native_event_received',
          nativeEvent: 'parse_end',
          nativeTimeNs: '123456789',
          clockDomain: 'js-performance',
          timeMs: expect.any(Number),
        }),
      ])
    );
    expect(native.setTracing).toHaveBeenCalledWith(id, true);
    stream.dispose();
    unsubscribe();
  });
});

/**
 * The stream controller, driven against the real C++ engine.
 *
 * `tests/real-engine-module.ts` keeps a real source per session and parses
 * it with the parser that ships in the app, so every assertion here is about
 * what a document becomes. The few remaining call-count checks are the
 * controller's own contract: it must not send more work to native than the
 * batching rules allow.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { unstable_batchedUpdates } from 'react-native';

import {
  createMarkdownStreamWithMode,
  getStreamState,
} from '../src/createMarkdownStream';
import { MAX_SOURCE_BYTES } from '../src/controlledSource';
import { parse, visibleText } from './engine';
import {
  nativeWork,
  resetEngine,
  snapshotOf,
  sourceForSession,
} from './real-engine-module';

vi.mock(
  '../src/NativeDownstreamSession',
  async () => (await import('./real-engine-module')).realEngineModule
);

const streams: { dispose(): void }[] = [];

function stream(): ReturnType<typeof createMarkdownStreamWithMode> {
  const created = createMarkdownStreamWithMode({}, 'incremental');
  streams.push(created);
  return created;
}

let frames = new Map<number, FrameRequestCallback>();
let nextFrame = 0;

function runFrames(): void {
  const pending = frames;
  frames = new Map();
  for (const callback of pending.values()) callback(0);
}

beforeEach(() => {
  resetEngine();
  frames = new Map();
  nextFrame = 0;
  vi.mocked(unstable_batchedUpdates).mockClear();
  vi.spyOn(globalThis, 'requestAnimationFrame').mockImplementation(
    (callback) => {
      const frame = ++nextFrame;
      frames.set(frame, callback);
      return frame;
    }
  );
  vi.spyOn(globalThis, 'cancelAnimationFrame').mockImplementation((frame) => {
    frames.delete(frame);
  });
});

afterEach(() => {
  for (const created of streams.splice(0)) created.dispose();
  vi.restoreAllMocks();
});

describe('the source a stream accumulates', () => {
  it('reuses the incremental parser and settles to the canonical document', () => {
    const session = stream();
    const binding = getStreamState(session).binding();
    expect(binding.mode).toBe('incremental');
    const chunks = [
      'A completed paragraph long enough to prove a stable boundary. '.repeat(
        3
      ) + '\n\n',
      '```typescript\nconst answer = 42;\n',
      'console.log(answer);\n```\n\n',
      'A **finished** answer with [a link](https://example.com).',
    ];
    let source = '';
    for (const part of chunks) {
      source += part;
      session.append(part);
      getStreamState(session).flush();
      const snapshot = snapshotOf(binding.sessionId)!;
      const expected = parse(source);
      expect(snapshot.blocks).toEqual(expected.blocks);
      expect(snapshot.links).toEqual(expected.links);
    }
    session.finish();
    const settled = snapshotOf(binding.sessionId)!;
    expect(settled.blocks).toEqual(parse(source).blocks);
    expect(settled.links).toEqual(parse(source).links);
  });

  it('resets a committed stream and drops provisional styles on finish', () => {
    const session = stream();
    const id = getStreamState(session).binding().sessionId;
    session.append(
      'A completed paragraph with a proven boundary. '.repeat(4) + '\n\n'
    );
    getStreamState(session).flush();
    session.append('```ts\nconst answer = 42;\n```\n\nNext paragraph.');
    getStreamState(session).flush();
    session.setPreview(true);
    session.reset('A **draft');
    expect(snapshotOf(id)!.blocks).toEqual(parse('A **draft**').blocks);
    session.append('** and *tail');
    getStreamState(session).flush();
    expect(snapshotOf(id)!.blocks).toEqual(
      parse('A **draft** and *tail*').blocks
    );
    session.finish();
    const canonical = parse('A **draft** and *tail');
    expect(snapshotOf(id)!.blocks).toEqual(canonical.blocks);
    expect(snapshotOf(id)!.links).toEqual(canonical.links);
  });

  it('is the concatenation of its chunks', async () => {
    const session = stream();
    session.append('hello ');
    session.append('**world**');
    expect(await session.getSource()).toBe('hello **world**');
  });

  it('materializes small chunks before and after reads, flushes, and reset', async () => {
    const session = stream();
    const chunks = Array.from({ length: 1000 }, (_, index) =>
      index % 3 === 0 ? '😀' : index % 3 === 1 ? 'e\u0301' : 'a'
    );
    for (const chunk of chunks) session.append(chunk);
    const expected = chunks.join('');
    expect(await session.getSource()).toBe(expected);
    expect(await session.getSource()).toBe(expected);
    getStreamState(session).flush();
    const id = getStreamState(session).binding().sessionId;
    expect(sourceForSession(id)).toBe(expected);
    session.append(' after read');
    expect(await session.getSource()).toBe(expected + ' after read');
    session.reset('replacement');
    session.append(' tail');
    expect(await session.getSource()).toBe('replacement tail');
    session.finish();
    expect(sourceForSession(id)).toBe('replacement tail');
    session.dispose();
    await expect(session.getSource()).rejects.toMatchObject({
      code: 'invalid_state',
    });
  });

  it('reaches the engine as one document after a flush', () => {
    const session = stream();
    session.append('**bo');
    getStreamState(session).flush();
    session.append('ld** and text');
    getStreamState(session).flush();

    const id = getStreamState(session).binding().sessionId;
    expect(sourceForSession(id)).toBe('**bold** and text');
  });

  it('renders the document a reader would see', () => {
    const session = stream();
    session.append('# Title\n\nBody.\n');
    getStreamState(session).flush();
    const id = getStreamState(session).binding().sessionId;
    expect(visibleText(parse(sourceForSession(id)))).toBe('Title\nBody.');
  });
});

describe('batching', () => {
  it('sends nothing to native until a frame runs', () => {
    const session = stream();
    const before = nativeWork.appends;
    session.append('a');
    session.append('b');
    expect(nativeWork.appends).toBe(before);
  });

  it('sends one append for a frame of chunks', () => {
    const session = stream();
    const before = nativeWork.appends;
    session.append('a');
    session.append('b');
    session.append('c');
    runFrames();
    expect(nativeWork.appends).toBe(before + 1);
  });

  it('flushes before finish', () => {
    const session = stream();
    const before = nativeWork.appends;
    session.append('a');
    session.finish();
    expect(nativeWork.appends).toBe(before + 1);
  });

  it('is idempotent on a second finish', () => {
    const session = stream();
    session.append('a');
    session.finish();
    const before = nativeWork.appends;
    session.finish();
    expect(nativeWork.appends).toBe(before);
  });

  it('refuses an append after finish', () => {
    const session = stream();
    session.append('a');
    session.finish();
    expect(() => session.append('b')).toThrow(
      expect.objectContaining({ code: 'invalid_state' })
    );
  });
});

describe('snapshot publication', () => {
  function ready(state: ReturnType<typeof getStreamState>, sequence?: number) {
    const binding = state.binding();
    state.snapshotReady({
      sessionId: binding.sessionId,
      generation: binding.generation,
      sequence: sequence ?? binding.sequence,
    });
  }

  it('publishes five dirty streams in one frame and one explicit React batch', () => {
    const states = Array.from({ length: 5 }, () => getStreamState(stream()));
    const observed: number[][] = [];
    for (const state of states) {
      state.subscribe(() => {
        observed.push(states.map((other) => other.binding().revision));
      });
      ready(state);
      ready(state);
    }

    expect(frames.size).toBe(1);
    expect(observed).toEqual([]);
    runFrames();
    expect(observed).toEqual(Array.from({ length: 5 }, () => [1, 1, 1, 1, 1]));
    expect(unstable_batchedUpdates).toHaveBeenCalledTimes(1);
    expect(frames.size).toBe(0);
  });

  it('still publishes finish and preview snapshots at the same sequence', () => {
    const session = stream();
    const state = getStreamState(session);
    const revisions: number[] = [];
    state.subscribe(() => revisions.push(state.binding().revision));
    session.append('A **draft');
    state.flush();
    runFrames();
    session.setPreview(true);
    runFrames();
    session.finish();
    runFrames();

    expect(revisions).toEqual([1, 2, 3]);
    expect(state.binding().readySequence).toBe(1);
  });

  it('acknowledges each stream separately and rejects stale sequence events', () => {
    const first = getStreamState(stream());
    const second = getStreamState(stream());
    first.append('a');
    first.append('b');
    second.append('c');
    // Keep input unflushed, so only the supplied events affect readiness.
    ready(first, 2);
    ready(second, 1);
    ready(first, 1);
    ready(second, 2);
    expect(first.binding().readySequence).toBe(2);
    expect(second.binding().readySequence).toBe(1);
  });

  it('reset publishes immediately and leaves other streams queued', () => {
    const first = getStreamState(stream());
    const second = getStreamState(stream());
    const firstNotify = vi.fn();
    const secondNotify = vi.fn();
    first.subscribe(firstNotify);
    second.subscribe(secondNotify);
    ready(first);
    ready(second);
    first.reset('replacement');

    expect(firstNotify).toHaveBeenCalledTimes(1);
    expect(first.binding()).toMatchObject({ generation: 1, revision: 1 });
    expect(frames.size).toBe(1);
    runFrames();
    expect(firstNotify).toHaveBeenCalledTimes(1);
    expect(secondNotify).toHaveBeenCalledTimes(1);
  });

  it('cancels the shared frame only after its last dirty stream is disposed', () => {
    const first = getStreamState(stream());
    const second = getStreamState(stream());
    const notify = vi.fn();
    second.subscribe(notify);
    ready(first);
    ready(second);
    first.dispose();
    expect(frames.size).toBe(1);
    second.dispose();
    expect(frames.size).toBe(0);
    runFrames();
    expect(notify).not.toHaveBeenCalled();
  });

  it('defers snapshots received by a subscriber until the next frame', () => {
    const first = getStreamState(stream());
    const second = getStreamState(stream());
    const secondNotify = vi.fn();
    first.subscribe(() => ready(second));
    second.subscribe(secondNotify);
    ready(first);
    ready(second);
    runFrames();

    expect(secondNotify).toHaveBeenCalledTimes(1);
    expect(second.binding().revision).toBe(1);
    expect(frames.size).toBe(1);
    runFrames();
    expect(secondNotify).toHaveBeenCalledTimes(2);
    expect(second.binding().revision).toBe(2);
  });

  it('drops a queued notification if another subscriber resets that stream', () => {
    const first = getStreamState(stream());
    const second = getStreamState(stream());
    const secondNotify = vi.fn();
    first.subscribe(() => second.reset('replacement'));
    second.subscribe(secondNotify);
    ready(first);
    ready(second);
    runFrames();

    expect(secondNotify).toHaveBeenCalledTimes(1);
    expect(second.binding()).toMatchObject({ generation: 1, revision: 2 });
    expect(frames.size).toBe(0);
  });

  it('drops a queued notification if another subscriber disposes that stream', () => {
    const first = getStreamState(stream());
    const second = getStreamState(stream());
    const secondNotify = vi.fn();
    first.subscribe(() => second.dispose());
    second.subscribe(secondNotify);
    ready(first);
    ready(second);
    runFrames();
    expect(secondNotify).not.toHaveBeenCalled();
  });

  it('stops the old notification when a subscriber resets its own stream', () => {
    const state = getStreamState(stream());
    const revisions: number[] = [];
    let reset = false;
    state.subscribe(() => {
      if (!reset) {
        reset = true;
        state.reset('replacement');
      }
    });
    state.subscribe(() => revisions.push(state.binding().revision));
    ready(state);
    runFrames();
    expect(revisions).toEqual([2]);
    expect(frames.size).toBe(0);
  });
});

describe('input limits', () => {
  it('rejects an unpaired surrogate before the source changes', async () => {
    const session = stream();
    session.append('ok');
    expect(() => session.append('\ud800')).toThrow(
      expect.objectContaining({ code: 'invalid_input' })
    );
    expect(await session.getSource()).toBe('ok');
  });

  it('splits a batch larger than 64 KiB', () => {
    const session = stream();
    const before = nativeWork.appends;
    for (let index = 0; index < 4; index += 1) {
      session.append('a'.repeat(64 * 1024));
    }
    runFrames();
    expect(nativeWork.appends - before).toBe(4);
  });

  it('refuses to queue past 256 KiB', () => {
    const session = stream();
    for (let index = 0; index < 4; index += 1) {
      session.append('a'.repeat(64 * 1024));
    }
    expect(() => session.append('b')).toThrow(
      expect.objectContaining({ code: 'input_limit' })
    );
  });

  it('refuses a source past 1 MiB', () => {
    const session = stream();
    const chunk = 'a'.repeat(64 * 1024);
    const accepted = Math.floor(MAX_SOURCE_BYTES / chunk.length);
    for (let index = 0; index < accepted; index += 1) {
      session.append(chunk);
      runFrames();
    }
    expect(MAX_SOURCE_BYTES).toBe(1024 * 1024);
    // One chunk past the limit is refused, and the source is left intact.
    expect(() => session.append(chunk)).toThrow(
      expect.objectContaining({ code: 'input_limit' })
    );
    expect(
      sourceForSession(getStreamState(session).binding().sessionId).length
    ).toBe(MAX_SOURCE_BYTES);
  });
});

describe('reset', () => {
  it('replaces the source and restarts the sequence', async () => {
    const session = stream();
    session.append('first');
    getStreamState(session).flush();

    getStreamState(session).reset('second');
    runFrames();

    expect(await session.getSource()).toBe('second');
    expect(getStreamState(session).binding()).toMatchObject({
      generation: 1,
      sequence: 0,
    });
    expect(sourceForSession(getStreamState(session).binding().sessionId)).toBe(
      'second'
    );
  });

  it('validates the new source before applying it', async () => {
    const session = stream();
    session.append('keep');
    expect(() => getStreamState(session).reset('\udfff')).toThrow(
      expect.objectContaining({ code: 'invalid_input' })
    );
    expect(await session.getSource()).toBe('keep');
  });

  it('allows appending again after a reset', async () => {
    const session = stream();
    session.append('a');
    getStreamState(session).reset('b');
    session.append('c');
    expect(await session.getSource()).toBe('bc');
  });
});

describe('disposal', () => {
  it('rejects every operation afterwards', async () => {
    const session = stream();
    session.append('a');
    session.dispose();

    await expect(session.getSource()).rejects.toMatchObject({
      code: 'invalid_state',
    });
    expect(() => session.append('b')).toThrow(
      expect.objectContaining({ code: 'invalid_state' })
    );
    expect(() => session.finish()).toThrow(
      expect.objectContaining({ code: 'invalid_state' })
    );
  });

  it('is safe to dispose twice', () => {
    const session = stream();
    session.dispose();
    expect(() => session.dispose()).not.toThrow();
  });
});

describe('bindings', () => {
  it('allows one view and refuses a second while the first holds it', () => {
    const session = stream();
    const state = getStreamState(session);
    const unbind = state.bind();
    expect(() => state.bind()).toThrow(
      expect.objectContaining({ code: 'session_bound' })
    );
    unbind();
    expect(() => state.bind()).not.toThrow();
  });

  it('keeps the session alive after the view unbinds', () => {
    const session = stream();
    const state = getStreamState(session);
    state.bind()();
    expect(() => session.append('a')).not.toThrow();
  });
});

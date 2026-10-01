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

import {
  createMarkdownStream,
  getStreamState,
} from '../src/createMarkdownStream';
import { MAX_SOURCE_BYTES } from '../src/controlledSource';
import { parse, visibleText } from './engine';
import {
  nativeWork,
  resetEngine,
  sourceForSession,
} from './real-engine-module';

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

let frames: FrameRequestCallback[] = [];

function runFrames(): void {
  const pending = frames;
  frames = [];
  for (const callback of pending) callback(0);
}

beforeEach(() => {
  resetEngine();
  frames = [];
  vi.spyOn(globalThis, 'requestAnimationFrame').mockImplementation(
    (callback) => {
      frames.push(callback);
      return frames.length;
    }
  );
  vi.spyOn(globalThis, 'cancelAnimationFrame').mockImplementation(() => {});
});

afterEach(() => {
  for (const created of streams.splice(0)) created.dispose();
  vi.restoreAllMocks();
});

describe('the source a stream accumulates', () => {
  it('is the concatenation of its chunks', async () => {
    const session = stream();
    session.append('hello ');
    session.append('**world**');
    expect(await session.getSource()).toBe('hello **world**');
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

/** @vitest-environment happy-dom */
import { act, StrictMode } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { DownstreamMarkdown } from '../src/DownstreamMarkdown';
import {
  createMarkdownStreamWithMode,
  getStreamState,
} from '../src/createMarkdownStream';
import { MAX_SOURCE_BYTES } from '../src/controlledSource';
import { parse, PREVIEW_ALL, PREVIEW_EMPHASIS } from './engine';
import { createNativeView } from './native-view';
import { mount, type MountedHost } from './native-view-spy';
import {
  realEngineModule,
  resetEngine,
  snapshotOf,
  sourceForSession,
  parseFlagsOf,
} from './real-engine-module';

const nativeView = createNativeView();
vi.mock('../src/DownstreamMarkdownViewNativeComponent', () => ({
  default: (props: Record<string, unknown>) => nativeView.component(props),
}));
vi.mock(
  '../src/NativeDownstreamSession',
  async () => (await import('./real-engine-module')).realEngineModule
);

let view: MountedHost | undefined;
let frames: Map<number, FrameRequestCallback>;
let nextFrame: number;
const native = realEngineModule.default;

function runFrames() {
  for (let pass = 0; frames.size && pass < 10; pass++) {
    act(() => {
      const pending = [...frames.values()];
      frames.clear();
      for (const callback of pending) callback(0);
    });
  }
  expect(frames.size).toBe(0);
}

function sessionId(): string {
  return nativeView.last()!.sessionId as string;
}

beforeEach(() => {
  resetEngine();
  nativeView.reset();
  frames = new Map();
  nextFrame = 0;
  vi.spyOn(globalThis, 'requestAnimationFrame').mockImplementation(
    (callback) => {
      frames.set(++nextFrame, callback);
      return nextFrame;
    }
  );
  vi.spyOn(globalThis, 'cancelAnimationFrame').mockImplementation((id) => {
    frames.delete(id);
  });
});

afterEach(() => {
  view?.unmount();
  view = undefined;
  vi.restoreAllMocks();
});

describe('controlled streaming through the real incremental parser', () => {
  it('keeps the accepted document when activation or option changes coincide with invalid input', () => {
    const onError = vi.fn();
    view = mount(<DownstreamMarkdown markdown="accepted" onError={onError} />);
    view.rerender(
      <DownstreamMarkdown
        markdown={'bad\udc00'}
        isAnimating
        onError={onError}
      />
    );
    runFrames();
    expect(sourceForSession(sessionId())).toBe('accepted');
    expect(onError).toHaveBeenCalledTimes(1);
    view.rerender(
      <DownstreamMarkdown
        markdown={'bad\udc00'}
        isAnimating
        cjk={false}
        onError={onError}
      />
    );
    runFrames();
    expect(sourceForSession(sessionId())).toBe('accepted');
    expect(onError).toHaveBeenCalledTimes(1);
  });
  it('creates no sessions for settled messages and activates when streaming starts', () => {
    const create = vi.spyOn(native, 'create');
    view = mount(
      <>
        {Array.from({ length: 100 }, (_, index) => (
          <DownstreamMarkdown key={index} markdown="history" />
        ))}
      </>
    );
    expect(create).not.toHaveBeenCalled();
    view.rerender(<DownstreamMarkdown markdown="history" isAnimating />);
    runFrames();
    expect(create).toHaveBeenCalledTimes(1);
    expect(create.mock.calls[0]?.[1]).toBe('incremental');
    expect(sourceForSession(sessionId())).toBe('history');
  });

  it('appends only new text and matches a controller at every prefix and finish', () => {
    const append = vi.spyOn(native, 'append');
    const reset = vi.spyOn(native, 'reset');
    const controller = createMarkdownStreamWithMode(
      { preview: true },
      'incremental'
    );
    const state = getStreamState(controller);
    let text = '';
    view = mount(<DownstreamMarkdown markdown={text} isAnimating />);
    const id = sessionId();
    try {
      const chunks = [
        '# Heading\n\n',
        'Some **bo',
        'ld** and 👩‍💻.\n\n',
        '| A | B |\n',
        '| - | - |\n| 1 | 2 |\n\n',
        '```ts\nconst x = 1;\n```\n\n',
        'unfinished **span',
      ];
      for (const chunk of chunks) {
        text += chunk;
        view.rerender(<DownstreamMarkdown markdown={text} isAnimating />);
        controller.append(chunk);
        state.flush();
        runFrames();
        expect(snapshotOf(id)).toEqual(snapshotOf(state.sessionId));
      }
      expect(
        append.mock.calls
          .filter(([session]) => session === id)
          .map(([, chunk]) => chunk)
      ).toEqual(chunks);
      expect(reset).not.toHaveBeenCalled();
      view.rerender(<DownstreamMarkdown markdown={text} isAnimating={false} />);
      controller.finish();
      runFrames();
      expect(snapshotOf(id)).toEqual(snapshotOf(state.sessionId));
      // The same flags as the session are used by the one-shot reference.
      expect(snapshotOf(id)?.blocks).toEqual(
        parse(text, parseFlagsOf(id)).blocks
      );
    } finally {
      controller.dispose();
    }
  });

  it('previews by default, supports a live override, and removes preview at finish', () => {
    const preview = vi.spyOn(native, 'setPreview');
    view = mount(<DownstreamMarkdown markdown="**hello" isAnimating />);
    runFrames();
    const id = sessionId();
    expect(preview).toHaveBeenCalledWith(id, PREVIEW_ALL);
    expect(snapshotOf(id)?.blocks[0]?.text).toBe('hello');
    view.rerender(
      <DownstreamMarkdown markdown="**hello" isAnimating preview={false} />
    );
    runFrames();
    expect(snapshotOf(id)?.blocks[0]?.text).toBe('**hello');
    view.rerender(
      <DownstreamMarkdown
        markdown="**hello"
        isAnimating
        preview={{ emphasis: true }}
      />
    );
    runFrames();
    expect(preview).toHaveBeenLastCalledWith(id, PREVIEW_EMPHASIS);
    expect(snapshotOf(id)?.blocks[0]?.text).toBe('hello');
    view.rerender(
      <DownstreamMarkdown markdown="**hello" isAnimating={false} />
    );
    runFrames();
    expect(snapshotOf(id)?.blocks[0]?.text).toBe('**hello');
  });

  it('resets on edits and on growth after finish, including the final prop update', () => {
    const reset = vi.spyOn(native, 'reset');
    view = mount(<DownstreamMarkdown markdown="first" isAnimating />);
    runFrames();
    const id = sessionId();
    const generation = nativeView.last()!.generation as number;
    view.rerender(<DownstreamMarkdown markdown="replacement" isAnimating />);
    runFrames();
    expect(reset).toHaveBeenLastCalledWith(id, 'replacement', generation + 1);
    view.rerender(
      <DownstreamMarkdown markdown="replacement done" isAnimating={false} />
    );
    runFrames();
    expect(sourceForSession(id)).toBe('replacement done');
    view.rerender(
      <DownstreamMarkdown markdown="replacement done again" isAnimating />
    );
    runFrames();
    expect(reset).toHaveBeenLastCalledWith(
      id,
      'replacement done again',
      generation + 2
    );
    view.rerender(<DownstreamMarkdown markdown="" isAnimating={false} />);
    runFrames();
    expect(sourceForSession(id)).toBe('');
    expect(sessionId()).toBe(id);
  });

  it('holds a split surrogate until its pair arrives and rejects invalid final input once', () => {
    const onError = vi.fn();
    view = mount(
      <DownstreamMarkdown markdown={'a\ud83d'} isAnimating onError={onError} />
    );
    runFrames();
    const id = sessionId();
    expect(sourceForSession(id)).toBe('a');
    expect(onError).not.toHaveBeenCalled();
    view.rerender(
      <DownstreamMarkdown markdown="a😀" isAnimating onError={onError} />
    );
    runFrames();
    expect(sourceForSession(id)).toBe('a😀');
    view.rerender(
      <DownstreamMarkdown
        markdown={'a😀\ud83d'}
        isAnimating={false}
        onError={onError}
      />
    );
    runFrames();
    expect(onError).toHaveBeenCalledTimes(1);
    expect(onError.mock.calls[0]?.[0].code).toBe('invalid_input');
    expect(sourceForSession(id)).toBe('a😀');
  });

  it('accepts large controlled updates without the controller chunk or queue limits', () => {
    const onError = vi.fn();
    const append = vi.spyOn(native, 'append');
    // Put a surrogate pair across the internal 16K-unit chunk boundary.
    const text = 'a'.repeat(16383) + '😀' + '漢'.repeat(90000);
    view = mount(
      <DownstreamMarkdown markdown={text} isAnimating onError={onError} />
    );
    runFrames();
    const id = sessionId();
    expect(sourceForSession(id)).toBe(text);
    for (const [, chunk] of append.mock.calls) {
      expect(new TextEncoder().encode(chunk).length).toBeLessThanOrEqual(65536);
    }
    expect(onError).not.toHaveBeenCalled();
    view.rerender(
      <DownstreamMarkdown
        markdown={'a'.repeat(MAX_SOURCE_BYTES + 1)}
        isAnimating
        onError={onError}
      />
    );
    runFrames();
    expect(onError).toHaveBeenCalledTimes(1);
    expect(onError.mock.calls[0]?.[0].code).toBe('input_limit');
    expect(sourceForSession(id)).toBe(text);
  });

  it('rebuilds from the latest source when parse options change', () => {
    const dispose = vi.spyOn(native, 'dispose');
    view = mount(<DownstreamMarkdown markdown="^up^" isAnimating />);
    runFrames();
    const first = sessionId();
    view.rerender(
      <DownstreamMarkdown
        markdown="^up^ more"
        isAnimating
        syntax={{ superscript: true }}
      />
    );
    runFrames();
    expect(dispose).toHaveBeenCalledWith(first);
    expect(sessionId()).toBe(first);
    expect(sourceForSession(sessionId())).toBe('^up^ more');
    expect(snapshotOf(sessionId())?.blocks[0]?.text).toBe('up more');
  });

  it('disposes every owned session under StrictMode and cancels pending work on unmount', () => {
    const create = vi.spyOn(native, 'create');
    const dispose = vi.spyOn(native, 'dispose');
    view = mount(
      <StrictMode>
        <DownstreamMarkdown markdown="hello" isAnimating />
      </StrictMode>
    );
    runFrames();
    expect(sourceForSession(sessionId())).toBe('hello');
    view.rerender(
      <StrictMode>
        <DownstreamMarkdown markdown="hello pending" isAnimating />
      </StrictMode>
    );
    view.unmount();
    view = undefined;
    runFrames();
    expect(dispose.mock.calls.map(([id]) => id).sort()).toEqual(
      create.mock.calls.map(([id]) => id).sort()
    );
  });
});

/**
 * @vitest-environment happy-dom
 */
import { act } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { createMarkdownStream, getStreamState } from '../src/createMarkdownStream';
import * as NativeDownstreamSessionModule from '../src/NativeDownstreamSession';
import { DownstreamMarkdown } from '../src/DownstreamMarkdown';
import type { MarkdownViewProps } from '../src/types';
import { createNativeViewSpy, mount, type MountedHost } from './native-view-spy';

const nativeView = createNativeViewSpy();

vi.mock('../src/DownstreamMarkdownViewNativeComponent', () => ({
  __esModule: true,
  default: (props: Record<string, unknown>) => nativeView.component(props),
}));

vi.mock('../src/NativeDownstreamSession', () => {
  const listeners = { snapshot: new Set<Function>() };
  return {
    __esModule: true,
    default: {
      create: vi.fn(),
      append: vi.fn(),
      reset: vi.fn(),
      finish: vi.fn(),
      dispose: vi.fn(),
      setTracing: vi.fn(),
      onSnapshotReady: vi.fn((callback: (event: object) => void) => {
        listeners.snapshot.add(callback);
        return { remove: () => listeners.snapshot.delete(callback) };
      }),
      onSessionError: vi.fn(() => ({ remove: () => {} })),
      onTrace: vi.fn(() => ({ remove: () => {} })),
    },
    emitSnapshot(event: object) {
      for (const listener of listeners.snapshot) listener(event);
    },
  };
});

const events = NativeDownstreamSessionModule as unknown as {
  emitSnapshot(event: object): void;
};

const streams: { dispose(): void }[] = [];

function stream(): ReturnType<typeof createMarkdownStream> {
  const created = createMarkdownStream();
  streams.push(created);
  return created;
}

let host: MountedHost | undefined;

let frames: FrameRequestCallback[] = [];

beforeEach(() => {
  nativeView.reset();
  vi.clearAllMocks();
  frames = [];
  vi.spyOn(globalThis, 'requestAnimationFrame').mockImplementation((callback) => {
    frames.push(callback);
    return frames.length;
  });
  vi.spyOn(globalThis, 'cancelAnimationFrame').mockImplementation(() => {});
});

afterEach(() => {
  host?.unmount();
  host = undefined;
  for (const created of streams.splice(0)) created.dispose();
  vi.restoreAllMocks();
});

/** Snapshots publish on the next frame, so the view needs a frame to react. */
function runFrames(): void {
  act(() => {
    const pending = frames;
    frames = [];
    for (const callback of pending) callback(0);
  });
}

describe('DownstreamMarkdown', () => {
  it('requires a source', () => {
    // The guard runs during render, so it is called directly here: a mounted
    // root would capture the error instead of rethrowing it to the caller.
    expect(() =>
      (DownstreamMarkdown as unknown as (props: object) => unknown)({})
    ).toThrow(/requires exactly one of markdown or stream/);
  });

  it('prefers the stream when both sources are given', () => {
    // The type forbids both, but a JavaScript caller can pass them. The stream
    // wins, so a stray `markdown` prop cannot redirect a bound session.
    const session = stream();
    host = mount(
      <DownstreamMarkdown
        {...({ markdown: 'ignored', stream: session } as unknown as MarkdownViewProps)}
      />
    );
    expect(nativeView.last()).toMatchObject({
      sessionId: getStreamState(session).binding().sessionId,
    });
  });

  it('hands the controlled source to native and starts at generation 1', () => {
    host = mount(<DownstreamMarkdown markdown="# hi" />);
    expect(nativeView.last()).toMatchObject({
      markdown: '# hi',
      generation: 1,
      selectable: false,
    });
  });

  it('keeps the native props stable when the source is unchanged', () => {
    host = mount(<DownstreamMarkdown markdown="# hi" />);
    const first = nativeView.last();

    host.rerender(<DownstreamMarkdown markdown="# hi" />);
    host.rerender(<DownstreamMarkdown markdown="# hi" />);

    // An unchanged source is a new element but not a new generation, so native
    // must not be told the text changed. React may still commit the host; what
    // matters is that the props it receives are identical.
    expect(nativeView.last()).toEqual(first);
    expect(first).toMatchObject({ markdown: '# hi', generation: 1 });
  });

  it('starts a new generation when the source changes', () => {
    host = mount(<DownstreamMarkdown markdown="# hi" />);
    nativeView.reset();

    host.rerender(<DownstreamMarkdown markdown="# bye" />);

    expect(nativeView.last()).toMatchObject({
      markdown: '# bye',
      generation: 2,
    });
  });

  it('keeps the last valid source after a rejected one', () => {
    host = mount(<DownstreamMarkdown markdown="ok" />);
    nativeView.reset();

    host.rerender(<DownstreamMarkdown markdown={'bad \ud800'} />);

    // Native never sees a rejected value, so it keeps showing the last good one.
    expect(nativeView.last()).toMatchObject({
      markdown: 'ok',
      generation: 1,
    });
  });

  it('reports a rejected source once through onError', () => {
    const onError = vi.fn();
    host = mount(
      <DownstreamMarkdown markdown={'bad \ud800'} onError={onError} />
    );
    expect(onError).toHaveBeenCalledTimes(1);
    expect(onError.mock.calls[0]?.[0]).toMatchObject({
      code: 'invalid_input',
    });

    host.rerender(
      <DownstreamMarkdown markdown={'bad \ud800'} onError={onError} />
    );
    expect(onError).toHaveBeenCalledTimes(1);
  });

  it('recovers when an accepted source returns', () => {
    const onError = vi.fn();
    host = mount(<DownstreamMarkdown markdown="ok" onError={onError} />);
    host.rerender(<DownstreamMarkdown markdown={'bad \ud800'} onError={onError} />);
    nativeView.reset();

    host.rerender(<DownstreamMarkdown markdown="ok" onError={onError} />);

    expect(nativeView.last()).toMatchObject({ markdown: 'ok' });
  });

  it('stops reporting an error once the source is accepted again', () => {
    const onError = vi.fn();
    host = mount(<DownstreamMarkdown markdown="ok" onError={onError} />);
    host.rerender(<DownstreamMarkdown markdown={'bad \ud800'} onError={onError} />);
    expect(onError).toHaveBeenCalledTimes(1);

    host.rerender(<DownstreamMarkdown markdown="ok" onError={onError} />);
    expect(onError).toHaveBeenCalledTimes(1);
  });
});

describe('DownstreamMarkdown with a stream', () => {
  it('binds the session and reports its revision to native', () => {
    const session = stream();
    host = mount(<DownstreamMarkdown stream={session} />);

    expect(nativeView.last()).toMatchObject({
      markdown: '',
      sessionId: getStreamState(session).binding().sessionId,
      sessionRevision: 0,
    });
  });

  it('keeps the native props stable for a stable stream', () => {
    const session = stream();
    host = mount(<DownstreamMarkdown stream={session} />);
    const first = nativeView.last();

    host.rerender(<DownstreamMarkdown stream={session} />);
    host.rerender(<DownstreamMarkdown stream={session} />);

    // useSyncExternalStore only bumps the revision when a snapshot lands, so an
    // idle re-render must not tell native to re-read the session.
    expect(nativeView.last()).toEqual(first);
    expect(first).toMatchObject({ sessionRevision: 0 });
  });

  it('refuses to bind one stream to two views', () => {
    const session = stream();
    host = mount(<DownstreamMarkdown stream={session} />);
    expect(() => mount(<DownstreamMarkdown stream={session} />)).toThrow(
      /only one view/
    );
  });

  it('accepts a new stream after the previous view unmounts', () => {
    const first = stream();
    host = mount(<DownstreamMarkdown stream={first} />);
    host.unmount();

    const second = stream();
    const next = mount(<DownstreamMarkdown stream={second} />);
    expect(nativeView.last()?.sessionId).toBe(
      getStreamState(second).binding().sessionId
    );
    next.unmount();
  });

  it('publishes a new revision when a snapshot arrives', () => {
    const session = stream();
    host = mount(<DownstreamMarkdown stream={session} />);
    nativeView.reset();

    session.append('hello');
    getStreamState(session).flush();
    const { sessionId } = getStreamState(session).binding();
    events.emitSnapshot({ sessionId, generation: 0, sequence: 1 });
    runFrames();

    expect(nativeView.last()).toMatchObject({ sessionRevision: 1 });
  });

  it('drops a snapshot from a stale generation', () => {
    const session = stream();
    host = mount(<DownstreamMarkdown stream={session} />);
    session.append('hello');
    getStreamState(session).flush();
    const { sessionId } = getStreamState(session).binding();
    nativeView.reset();

    // An old session's event must not repaint the view.
    events.emitSnapshot({ sessionId, generation: 0, sequence: 99 });
    runFrames();

    expect(nativeView.count()).toBe(0);
  });
});

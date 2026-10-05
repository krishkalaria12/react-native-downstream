/**
 * @vitest-environment happy-dom
 *
 * The `animated`, `isAnimating`, and `caret` props, and the callbacks that
 * report when text starts and stops arriving.
 *
 * The props are asserted on what reaches the native view, because that is the
 * whole contract: JavaScript resolves the options and hands native a name, a
 * split, a flat array of numbers, a caret shape, and whether text is arriving.
 * Nothing else crosses, and a frame of animation costs no React render.
 */
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import {
  resolveAnimate,
  resolveEasing,
  resetAnimationWarnings,
} from '../src/animation';
import {
  createMarkdownStream,
  getStreamState,
} from '../src/createMarkdownStream';
import { DownstreamMarkdown } from '../src/DownstreamMarkdown';
import { createNativeView, type NativeView } from './native-view';
import { resetEngine } from './real-engine-module';

const nativeView: NativeView = createNativeView();

vi.mock('../src/DownstreamMarkdownViewNativeComponent', () => ({
  __esModule: true,
  default: (props: Record<string, unknown>) => nativeView.component(props),
}));

// The session TurboModule is backed by the real C++ engine, so a stream in these
// tests is a real parse.
vi.mock(
  '../src/NativeDownstreamSession',
  async () => (await import('./real-engine-module')).realEngineModule
);

let host: HTMLDivElement | undefined;
let root: Root | undefined;
let committed: React.ReactElement | undefined;
let frames: FrameRequestCallback[] = [];
const streams: { dispose(): void }[] = [];

function stream() {
  const created = createMarkdownStream();
  streams.push(created);
  return created;
}

function mount(element: React.ReactElement): void {
  host ??= document.body.appendChild(document.createElement('div'));
  root ??= createRoot(host);
  committed = element;
  act(() => root!.render(element));
}

/**
 * Renders the committed element again.
 *
 * `render` with the same element object is a no-op in React, so a test that
 * wants a real second render rebuilds the element first. That is what a parent
 * re-render does, and it is the case the stable-by-value props exist for.
 */
function rerender(): void {
  act(() => root!.render(committed!));
}

function unmount(): void {
  if (!root || !committed) return;
  act(() => root!.unmount());
  root = undefined;
  committed = undefined;
}

/** Runs the frames the stream and the fade asked for. */
function runFrames(): void {
  act(() => {
    for (const callback of frames.splice(0)) callback(0);
  });
}

beforeEach(() => {
  if (host) host.replaceChildren();
  nativeView.reset();
  resetEngine();
  resetAnimationWarnings();
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
  unmount();
  for (const created of streams.splice(0)) created.dispose();
  vi.restoreAllMocks();
});

/** The animation props the last commit handed the native view. */
function sent() {
  return {
    animation: nativeView.last()?.animation,
    animationSplit: nativeView.last()?.animationSplit,
    animationTiming: nativeView.last()?.animationTiming,
    animating: nativeView.last()?.animating,
    caret: nativeView.last()?.caret,
  };
}

describe('resolveAnimate', () => {
  it('is off unless asked for', () => {
    expect(resolveAnimate(undefined).animation).toBe('none');
    expect(resolveAnimate(false).animation).toBe('none');
    // Nothing to send at all, so an unchanged configuration costs native nothing.
    expect(resolveAnimate(false).timing).toEqual([]);
  });

  it('gives every default for `true`', () => {
    expect(resolveAnimate(true)).toEqual({
      animation: 'fadeIn',
      split: 'word',
      timing: [150, 40, 320, 0.25, 0.1, 0.25, 1],
    });
  });

  it('reads each option', () => {
    expect(
      resolveAnimate({
        animation: 'slideUp',
        duration: 200,
        sep: 'char',
        stagger: 10,
        maxBacklogMs: 500,
        easing: 'linear',
      })
    ).toEqual({
      animation: 'slideUp',
      split: 'char',
      timing: [200, 10, 500, 0, 0, 1, 1],
    });
  });

  it('keeps a zero stagger, which turns the cascade off', () => {
    expect(resolveAnimate({ stagger: 0 }).timing[1]).toBe(0);
  });

  it('draws blurIn as a fade and says so once', () => {
    const warnings: string[] = [];
    expect(
      resolveAnimate({ animation: 'blurIn' }, (m) => warnings.push(m)).animation
    ).toBe('fadeIn');
    expect(warnings).toHaveLength(1);
    expect(warnings[0]).toMatch(/blurIn/);
  });

  it('warns once for an unknown name', () => {
    const warnings: string[] = [];
    const warn = (m: string) => warnings.push(m);
    resolveAnimate({ animation: 'sparkle' }, warn);
    expect(warnings).toHaveLength(1);
    expect(warnings[0]).toMatch(/sparkle/);
    // A second resolve with no handler goes through the module's own warning,
    // which remembers the first.
    resolveAnimate({ animation: 'sparkle' });
    resolveAnimate({ animation: 'sparkle' });
  });

  it('falls back to the default for a number that is not one', () => {
    const warnings: string[] = [];
    expect(
      resolveAnimate(
        { duration: -1, stagger: NaN, maxBacklogMs: Infinity },
        (m) => warnings.push(m)
      ).timing
    ).toEqual([150, 40, 320, 0.25, 0.1, 0.25, 1]);
    expect(warnings).toHaveLength(3);
    expect(warnings[0]).toMatch(/duration/);
  });

  it('treats an unrecognized shape as off', () => {
    const warnings: string[] = [];
    expect(
      resolveAnimate('yes' as unknown as boolean, (m) => warnings.push(m))
        .animation
    ).toBe('none');
    expect(warnings[0]).toMatch(/animated/);
  });
});

describe('resolveEasing', () => {
  it('reads every CSS keyword', () => {
    expect(resolveEasing('linear')).toEqual([0, 0, 1, 1]);
    expect(resolveEasing('ease')).toEqual([0.25, 0.1, 0.25, 1]);
    expect(resolveEasing('ease-in')).toEqual([0.42, 0, 1, 1]);
    expect(resolveEasing('ease-out')).toEqual([0, 0, 0.58, 1]);
    expect(resolveEasing('ease-in-out')).toEqual([0.42, 0, 0.58, 1]);
  });

  it('reads a cubic-bezier, clamping x into range', () => {
    expect(resolveEasing('cubic-bezier(0.1, 0.2, 0.3, 0.4)')).toEqual([
      0.1, 0.2, 0.3, 0.4,
    ]);
    // A curve whose x doubles back has no single y for an x, which a fade needs.
    expect(resolveEasing('cubic-bezier(-1, 2, 3, 4)')).toEqual([0, 2, 1, 4]);
  });

  it('falls back to ease for what a draw pass cannot express', () => {
    const warnings: string[] = [];
    expect(resolveEasing('steps(4, end)', (m) => warnings.push(m))).toEqual([
      0.25, 0.1, 0.25, 1,
    ]);
    expect(warnings[0]).toMatch(/steps/);
  });
});

describe('the animation props a controlled view sends', () => {
  it('sends nothing when animated is off', () => {
    mount(<DownstreamMarkdown markdown="hello" />);
    expect(sent()).toEqual({
      animation: 'none',
      animationSplit: 'word',
      animationTiming: undefined,
      animating: 'off',
      caret: 'none',
    });
  });

  it('sends the resolved animation when it is on', () => {
    mount(<DownstreamMarkdown markdown="hello" animated isAnimating />);
    expect(sent()).toEqual({
      animation: 'fadeIn',
      animationSplit: 'word',
      animationTiming: [150, 40, 320, 0.25, 0.1, 0.25, 1],
      animating: 'on',
      caret: 'none',
    });
  });

  it('sends the caret shape', () => {
    mount(<DownstreamMarkdown markdown="hello" caret="circle" isAnimating />);
    expect(nativeView.last()?.caret).toBe('circle');
  });

  it('follows isAnimating for both a fade and a caret', () => {
    mount(<DownstreamMarkdown markdown="hello" animated caret="block" />);
    expect(nativeView.last()?.animating).toBe('off');
    rerender();
    expect(nativeView.last()?.animating).toBe('off');
  });

  it('sends nothing new for an inline literal that has not changed', () => {
    // A fresh literal on each render, which is how an app normally writes it.
    const element = () => (
      <DownstreamMarkdown
        markdown="hello"
        animated={{ animation: 'slideUp' }}
        isAnimating
      />
    );
    mount(element());
    const first = nativeView.last()?.animationTiming;
    expect(first).toEqual([150, 40, 320, 0.25, 0.1, 0.25, 1]);
    committed = element();
    rerender();
    // Comparing the resolved values rather than the identity is what keeps this
    // from sending a new array and re-rendering the document behind it.
    expect(nativeView.last()?.animationTiming).toBe(first);
  });

  it('sends a new array when an option actually changes', () => {
    mount(
      <DownstreamMarkdown
        markdown="hello"
        animated={{ stagger: 10 }}
        isAnimating
      />
    );
    expect(nativeView.last()?.animationTiming).toEqual([
      150, 10, 320, 0.25, 0.1, 0.25, 1,
    ]);
    committed = (
      <DownstreamMarkdown
        markdown="hello"
        animated={{ stagger: 20 }}
        isAnimating
      />
    );
    rerender();
    expect(nativeView.last()?.animationTiming).toEqual([
      150, 20, 320, 0.25, 0.1, 0.25, 1,
    ]);
  });
});

describe('the animation props a stream view sends', () => {
  it('follows the stream rather than a prop', () => {
    const session = stream();
    mount(<DownstreamMarkdown stream={session} animated />);
    // `auto` is what makes the platform follow the session's own flag.
    expect(nativeView.last()?.animating).toBe('auto');
  });

  it('takes an explicit isAnimating as an override', () => {
    const session = stream();
    mount(<DownstreamMarkdown stream={session} animated isAnimating={false} />);
    expect(nativeView.last()?.animating).toBe('off');
    committed = <DownstreamMarkdown stream={session} animated isAnimating />;
    rerender();
    expect(nativeView.last()?.animating).toBe('on');
  });
});

describe('the animation callbacks', () => {
  it.each(['controlled', 'stream'] as const)(
    'reports transitions once with inline callbacks in a %s view',
    (mode) => {
      const session = mode === 'stream' ? stream() : undefined;
      const starts: number[] = [];
      const ends: number[] = [];
      function render(revision: number, animating: boolean) {
        const source = session
          ? { stream: session }
          : { markdown: `text ${revision}` };
        mount(
          <DownstreamMarkdown
            {...source}
            isAnimating={animating}
            onAnimationStart={() => starts.push(revision)}
            onAnimationEnd={() => ends.push(revision)}
          />
        );
      }

      render(0, false);
      for (let revision = 1; revision <= 20; revision++) render(revision, true);
      expect(starts).toEqual([1]);
      expect(ends).toEqual([]);
      render(21, false);
      for (let revision = 22; revision <= 40; revision++)
        render(revision, false);
      expect(ends).toEqual([21]);
      render(41, true);
      expect(starts).toEqual([1, 41]);
    }
  );

  it('fires start on mount when a controlled view is already animating', () => {
    const onStart = vi.fn();
    mount(
      <DownstreamMarkdown
        markdown="hi"
        isAnimating
        onAnimationStart={onStart}
      />
    );
    expect(onStart).toHaveBeenCalledTimes(1);
  });

  it('does not fire end on mount, because nothing was animating', () => {
    const onEnd = vi.fn();
    mount(
      <DownstreamMarkdown
        markdown="hi"
        isAnimating={false}
        onAnimationEnd={onEnd}
      />
    );
    expect(onEnd).not.toHaveBeenCalled();
  });

  it('fires on each transition', () => {
    const onStart = vi.fn();
    const onEnd = vi.fn();
    mount(
      <DownstreamMarkdown
        markdown="hi"
        isAnimating={false}
        onAnimationStart={onStart}
        onAnimationEnd={onEnd}
      />
    );
    committed = (
      <DownstreamMarkdown
        markdown="hi"
        isAnimating
        onAnimationStart={onStart}
        onAnimationEnd={onEnd}
      />
    );
    rerender();
    expect(onStart).toHaveBeenCalledTimes(1);
    expect(onEnd).not.toHaveBeenCalled();
    committed = (
      <DownstreamMarkdown
        markdown="hi"
        isAnimating={false}
        onAnimationStart={onStart}
        onAnimationEnd={onEnd}
      />
    );
    rerender();
    expect(onEnd).toHaveBeenCalledTimes(1);
    expect(onStart).toHaveBeenCalledTimes(1);
  });

  it('fires start on the first append of a stream generation', () => {
    const onStart = vi.fn();
    const onEnd = vi.fn();
    const session = stream();
    mount(
      <DownstreamMarkdown
        stream={session}
        onAnimationStart={onStart}
        onAnimationEnd={onEnd}
      />
    );
    // A stream that has been created but has not been written to is not
    // animating, and nothing fires.
    expect(onStart).not.toHaveBeenCalled();

    session.append('hello');
    getStreamState(session).flush();
    runFrames();
    expect(onStart).toHaveBeenCalledTimes(1);

    // A second append continues the same cycle.
    session.append(' world');
    getStreamState(session).flush();
    runFrames();
    expect(onStart).toHaveBeenCalledTimes(1);
    expect(onEnd).not.toHaveBeenCalled();
  });

  it('fires end when a stream finishes', () => {
    const onStart = vi.fn();
    const onEnd = vi.fn();
    const session = stream();
    mount(
      <DownstreamMarkdown
        stream={session}
        onAnimationStart={onStart}
        onAnimationEnd={onEnd}
      />
    );
    session.append('hello');
    getStreamState(session).flush();
    runFrames();
    expect(onStart).toHaveBeenCalledTimes(1);
    session.finish();
    expect(onEnd).toHaveBeenCalledTimes(1);
  });

  it('starts a new cycle after a reset', () => {
    const onStart = vi.fn();
    const onEnd = vi.fn();
    const session = stream();
    mount(
      <DownstreamMarkdown
        stream={session}
        onAnimationStart={onStart}
        onAnimationEnd={onEnd}
      />
    );
    session.append('first');
    getStreamState(session).flush();
    runFrames();
    session.reset('second');
    expect(onEnd).toHaveBeenCalledTimes(1);
    session.append(' more');
    getStreamState(session).flush();
    runFrames();
    expect(onStart).toHaveBeenCalledTimes(2);
  });

  it('does not fire on a re-render that changes nothing', () => {
    const onStart = vi.fn();
    const session = stream();
    mount(
      <DownstreamMarkdown
        stream={session}
        animated
        onAnimationStart={onStart}
      />
    );
    session.append('hello');
    getStreamState(session).flush();
    runFrames();
    rerender();
    expect(onStart).toHaveBeenCalledTimes(1);
  });

  it('fires start on mount when a stream is already arriving', () => {
    const onStart = vi.fn();
    const onEnd = vi.fn();
    const session = stream();
    session.append('hello');
    getStreamState(session).flush();
    mount(
      <DownstreamMarkdown
        stream={session}
        onAnimationStart={onStart}
        onAnimationEnd={onEnd}
      />
    );
    expect(onStart).toHaveBeenCalledTimes(1);
    session.finish();
    expect(onEnd).toHaveBeenCalledTimes(1);
  });

  it('follows an explicit isAnimating on a stream view rather than the controller', () => {
    const onStart = vi.fn();
    const onEnd = vi.fn();
    const session = stream();
    mount(
      <DownstreamMarkdown
        stream={session}
        isAnimating={false}
        onAnimationStart={onStart}
        onAnimationEnd={onEnd}
      />
    );
    session.append('hello');
    getStreamState(session).flush();
    runFrames();
    session.finish();
    expect(onStart).not.toHaveBeenCalled();
    expect(onEnd).not.toHaveBeenCalled();

    committed = (
      <DownstreamMarkdown
        stream={session}
        isAnimating
        onAnimationStart={onStart}
        onAnimationEnd={onEnd}
      />
    );
    rerender();
    expect(onStart).toHaveBeenCalledTimes(1);
  });
});

describe('animation frames cost no React render', () => {
  it('renders once per snapshot, not once per frame', () => {
    const session = stream();
    mount(<DownstreamMarkdown stream={session} animated caret="block" />);
    const before = nativeView.count();
    session.append('hello');
    getStreamState(session).flush();
    runFrames();
    expect(nativeView.count()).toBe(before + 1);
    // The frames the fade would run are native's; a re-render of the parent is
    // the only thing that asks React for another commit, and one render of the
    // parent is one commit whatever the fade is doing.
    committed = <DownstreamMarkdown stream={session} animated caret="block" />;
    rerender();
    expect(nativeView.count()).toBe(before + 2);
  });
});

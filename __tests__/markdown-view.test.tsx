/**
 * @vitest-environment happy-dom
 */
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import {
  createMarkdownStream,
  getStreamState,
} from '../src/createMarkdownStream';
import { DownstreamMarkdown } from '../src/DownstreamMarkdown';
import type { MarkdownViewProps } from '../src/types';
import { parse, visibleText } from './engine';
import { createNativeView, type NativeView } from './native-view';
import { resetEngine } from './real-engine-module';

const nativeView: NativeView = createNativeView();

vi.mock('../src/DownstreamMarkdownViewNativeComponent', () => ({
  __esModule: true,
  default: (props: Record<string, unknown>) => nativeView.component(props),
}));

// The session TurboModule is backed by the real C++ engine, so a snapshot in
// these tests is a real parse and the text below is what the engine produced.
// `vi.mock` is hoisted, so the module is imported inside the factory.
vi.mock(
  '../src/NativeDownstreamSession',
  async () => (await import('./real-engine-module')).realEngineModule
);

let host: HTMLDivElement | undefined;
let root: Root | undefined;
let committed: React.ReactElement | undefined;
const streams: { dispose(): void }[] = [];

function stream(): ReturnType<typeof createMarkdownStream> {
  const created = createMarkdownStream();
  streams.push(created);
  return created;
}

function mount(element: React.ReactElement): void {
  host ??= document.body.appendChild(document.createElement('div'));
  root ??= createRoot(host);
  committed = element;
  act(() => {
    root!.render(element);
  });
}

function rerender(next?: React.ReactElement): void {
  committed = next ?? committed;
  act(() => {
    root!.render(committed!);
  });
}

/** The DOM a reader would see. */
function rendered(): ParentNode {
  return host!;
}

function renderedText(): string {
  // The engine's block text keeps a trailing newline; the DOM does not.
  return (rendered().textContent ?? '').replace(/\n+$/, '');
}

/** The engine's visible text, with its block separators, for comparison. */
function expectedText(source: string): string {
  return visibleText(parse(source)).replace(/\n/g, '');
}

function blockTags(): string[] {
  return [...rendered().querySelectorAll('[data-native-view] > *')].map(
    (element) => element.tagName
  );
}

function unmount(): void {
  act(() => {
    root?.unmount();
  });
  root = undefined;
  host?.remove();
  host = undefined;
  committed = undefined;
}

/** Runs the pending animation frames, which is when a snapshot publishes. */
function runFrames(): void {
  act(() => {
    for (const callback of frames.splice(0)) callback(0);
  });
}

let frames: FrameRequestCallback[] = [];

beforeEach(() => {
  // The mount point is reused across tests, so clear the previous document.
  if (host) host.replaceChildren();
  nativeView.reset();
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
  unmount();
  for (const created of streams.splice(0)) created.dispose();
  vi.restoreAllMocks();
});

describe('DownstreamMarkdown with a controlled source', () => {
  it('renders a heading and a paragraph', () => {
    mount(<DownstreamMarkdown markdown={'# Title\n\nBody text.\n'} />);

    // The reader sees the parsed blocks, not the source.
    expect(renderedText()).toBe(expectedText('# Title\n\nBody text.\n'));
    expect(blockTags()[0]).toBe('H1');
    expect(blockTags()[1]).toBe('P');
  });

  it('renders a table as a table', () => {
    const source = '| A | B |\n| --- | --- |\n| one | two |\n';
    mount(<DownstreamMarkdown markdown={source} />);

    expect(blockTags()[0]).toBe('TABLE');
    expect(renderedText()).toContain('one');
    expect(renderedText()).toContain('two');
  });

  it('renders emphasis as a strong element', () => {
    mount(<DownstreamMarkdown markdown={'a **b** c\n'} />);
    expect(renderedText()).toBe(expectedText('a **b** c\n'));
    expect(rendered().querySelector('strong')?.textContent).toBe('b');
  });

  it('renders a link as an anchor with the allowed target', () => {
    mount(<DownstreamMarkdown markdown={'[docs](https://example.com)\n'} />);
    expect(rendered().querySelector('a')?.getAttribute('href')).toBe(
      'https://example.com'
    );
  });

  it('renders no anchor for a refused target', () => {
    mount(<DownstreamMarkdown markdown={'[x](javascript:alert(1))\n'} />);
    expect(rendered().querySelector('a')).toBeNull();
  });

  it('shows the previous document when a new one is refused', () => {
    mount(<DownstreamMarkdown markdown={'first\n'} />);
    expect(renderedText()).toBe(expectedText('first\n'));

    rerender(<DownstreamMarkdown markdown={'bad \ud800'} />);

    expect(renderedText()).toBe(expectedText('first\n'));
  });

  it('reports a refused source through onError', () => {
    const onError = vi.fn();
    mount(<DownstreamMarkdown markdown={'bad \ud800'} onError={onError} />);
    expect(onError).toHaveBeenCalledTimes(1);
    expect(onError.mock.calls[0]?.[0]).toMatchObject({ code: 'invalid_input' });
  });

  it('replaces the rendered text when the source changes', () => {
    mount(<DownstreamMarkdown markdown={'first\n'} />);
    rerender(<DownstreamMarkdown markdown={'second\n'} />);
    expect(renderedText()).toBe(expectedText('second\n'));
  });

  it('shows the same text when an unchanged source is re-rendered', () => {
    const element = <DownstreamMarkdown markdown={'stable\n'} />;
    mount(element);
    const before = renderedText();

    rerender(element);
    rerender(element);

    expect(renderedText()).toBe(before);
  });
});

describe('DownstreamMarkdown with a stream', () => {
  it('renders the document as chunks arrive', () => {
    const session = stream();
    mount(<DownstreamMarkdown stream={session} />);
    expect(renderedText()).toBe('');

    session.append('**bo');
    getStreamState(session).flush();
    runFrames();

    session.append('ld** and text');
    getStreamState(session).flush();
    runFrames();

    expect(renderedText()).toBe(expectedText('**bold** and text'));
    expect(rendered().querySelector('strong')?.textContent).toBe('bold');
  });

  it('renders a table once its separator row arrives', () => {
    const session = stream();
    mount(<DownstreamMarkdown stream={session} />);

    session.append('| A | B |\n');
    getStreamState(session).flush();
    runFrames();
    expect(blockTags()[0]).toBe('P');

    session.append('| --- | --- |\n| one | two |');
    getStreamState(session).flush();
    runFrames();
    expect(blockTags()[0]).toBe('TABLE');
    expect(renderedText()).toContain('two');
  });

  it('matches the finished document when the whole source is streamed', () => {
    const source = '# Title\n\nA paragraph with **bold**.\n\n- [x] done\n';
    const chunks = source.match(/[\s\S]{1,7}/gu) ?? [];
    const session = stream();
    mount(<DownstreamMarkdown stream={session} />);

    for (const chunk of chunks) {
      session.append(chunk);
      getStreamState(session).flush();
      runFrames();
    }

    // The same document, parsed complete, is the reference.
    expect(renderedText()).toBe(expectedText(source));
  });

  it('shows the same text when the stream is idle and the parent re-renders', () => {
    const session = stream();
    session.append('hello');
    getStreamState(session).flush();
    const element = <DownstreamMarkdown stream={session} />;
    mount(element);
    runFrames();
    const before = renderedText();

    rerender(element);
    rerender(element);

    expect(renderedText()).toBe(before);
    expect(before).toBe(expectedText('hello\n'));
  });

  it('refuses to bind one session to two views', () => {
    const session = stream();
    const state = getStreamState(session);

    // The one-view-per-session rule lives in the controller, and the view
    // enforces it in a layout effect. React captures that throw, so the rule is
    // asserted where it is enforced.
    const unbind = state.bind();
    expect(() => state.bind()).toThrow(
      expect.objectContaining({ code: 'session_bound' })
    );
    unbind();
    expect(() => state.bind()).not.toThrow();
  });

  it('renders again after the first view unmounts', () => {
    const first = stream();
    first.append('first document');
    getStreamState(first).flush();
    mount(<DownstreamMarkdown stream={first} />);
    runFrames();
    expect(renderedText()).toBe(expectedText('first document\n'));
    unmount();

    const second = stream();
    second.append('second document');
    getStreamState(second).flush();
    mount(<DownstreamMarkdown stream={second} />);
    runFrames();

    expect(renderedText()).toBe(expectedText('second document\n'));
  });
});

describe('DownstreamMarkdown props', () => {
  it('requires a source', () => {
    expect(() =>
      (DownstreamMarkdown as unknown as (props: object) => unknown)({})
    ).toThrow(/requires exactly one of markdown or stream/);
  });

  it('passes selectable through to the native view', () => {
    mount(<DownstreamMarkdown markdown="x" selectable />);
    expect(nativeView.last()?.selectable).toBe(true);
  });

  it('starts a controlled source at generation 1', () => {
    mount(<DownstreamMarkdown markdown="x" />);
    expect(nativeView.last()?.generation).toBe(1);
  });

  it('accepts both sources at runtime, preferring the stream', () => {
    const session = stream();
    mount(
      <DownstreamMarkdown
        {...({
          markdown: 'ignored',
          stream: session,
        } as unknown as MarkdownViewProps)}
      />
    );
    expect(nativeView.last()?.sessionId).toBe(
      getStreamState(session).binding().sessionId
    );
  });
});

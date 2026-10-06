/* eslint-disable no-bitwise -- the native `controls` prop is a bitmask */
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
import {
  CONTROL_ALL,
  CONTROL_CODE_DOWNLOAD,
  CONTROL_LABEL_KEYS,
} from '../src/controls';
import { presets } from '../src/presets';
import { themeSlot } from '../src/themeSlots';
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

  it('highlights code by default', () => {
    mount(<DownstreamMarkdown markdown="x" />);
    expect(nativeView.last()?.syntaxHighlighting).toBe(true);
  });

  it('passes syntaxHighlighting through to the native view', () => {
    mount(<DownstreamMarkdown markdown="x" syntaxHighlighting={false} />);
    expect(nativeView.last()?.syntaxHighlighting).toBe(false);
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

describe('DownstreamMarkdown controls', () => {
  const SOURCE =
    '```ts title="app.ts"\nconst a = 1;\n```\n\n| a | b |\n| --- | --- |\n| 1 | 2 |\n';

  /**
   * The event the native view would send, with everything a control sends. The
   * generation is the mounted document's, so the press is recognised; the tests
   * that care about a superseded document override it.
   */
  function controlEvent(
    overrides: Partial<{
      block: string;
      action: string;
      format: string;
      language: string;
      filename: string;
      wrapped: boolean;
      generation: number;
    }> = {}
  ) {
    return {
      nativeEvent: {
        block: 'code',
        action: 'copy',
        format: 'text',
        language: 'ts',
        filename: '',
        wrapped: false,
        // The mounted document's own generation, so a press is recognised; the
        // tests about a superseded document override it.
        generation: Number(nativeView.last()?.generation ?? 0),
        ...overrides,
      },
    };
  }

  /** A `control_failed` error, tagged the way native tags it. */
  function controlError(
    block: string,
    action: string,
    message: string,
    overrides: { generation?: number; code?: string } = {}
  ) {
    return {
      nativeEvent: {
        code: overrides.code ?? 'control_failed',
        message: `${block} ${action} failed: ${message}`,
        generation:
          overrides.generation ?? Number(nativeView.last()?.generation ?? 0),
        sequence: 0,
        recoverable: true,
        block,
        action,
      },
    };
  }

  /** Calls the native view's event handler as Fabric would. */
  function fire(
    name: 'onControlAction' | 'onMarkdownError',
    event: unknown
  ): void {
    act(() => {
      (nativeView.last()?.[name] as (event: unknown) => void)?.(event);
    });
  }

  it('sends every control on by default', () => {
    mount(<DownstreamMarkdown markdown={SOURCE} />);
    expect(nativeView.last()?.controls).toBe(CONTROL_ALL);
  });

  it('sends the resolved bitmask rather than the prop', () => {
    mount(
      <DownstreamMarkdown
        markdown={SOURCE}
        controls={{ code: { download: false } }}
      />
    );
    expect(nativeView.last()?.controls).toBe(
      CONTROL_ALL & ~CONTROL_CODE_DOWNLOAD
    );
  });

  it('sends the file names, the separator, and the labels', () => {
    mount(
      <DownstreamMarkdown
        markdown={SOURCE}
        controls={{
          code: { download: { filename: 'snippet' } },
          table: { csvSeparator: ';' },
        }}
        translations={{ copyCode: 'Copiar' }}
      />
    );
    const props = nativeView.last();
    expect(props?.codeFilename).toBe('snippet');
    expect(props?.tableFilename).toBe('table');
    expect(props?.csvSeparator).toBe(';');
    const labels = props?.controlLabels as string[];
    expect(labels[0]).toBe('Copiar');
    expect(labels).toHaveLength(CONTROL_LABEL_KEYS.length);
  });

  it('does not wrap code by default', () => {
    mount(<DownstreamMarkdown markdown={SOURCE} />);
    expect(nativeView.last()?.codeBlockWrap).toBe(false);
  });

  it('passes codeBlockWrap through', () => {
    mount(<DownstreamMarkdown markdown={SOURCE} codeBlockWrap />);
    expect(nativeView.last()?.codeBlockWrap).toBe(true);
  });

  it('passes lineNumbers and codeBlockMaxHeight through, normalized', () => {
    // Numbers are on and the cap is 400 by default, as in Streamdown, and every
    // "off" spelling of the cap reaches native as the single 0 it reads.
    mount(<DownstreamMarkdown markdown={SOURCE} />);
    expect(nativeView.last()?.lineNumbers).toBe(true);
    expect(nativeView.last()?.codeBlockMaxHeight).toBe(400);

    mount(
      <DownstreamMarkdown
        markdown={SOURCE}
        lineNumbers={false}
        codeBlockMaxHeight="none"
      />
    );
    expect(nativeView.last()?.lineNumbers).toBe(false);
    expect(nativeView.last()?.codeBlockMaxHeight).toBe(0);

    // A value that is not a height falls back rather than reaching native.
    mount(
      <DownstreamMarkdown
        markdown={SOURCE}
        codeBlockMaxHeight={'50vh' as unknown as number}
      />
    );
    expect(nativeView.last()?.codeBlockMaxHeight).toBe(400);
  });

  it('selects by default', () => {
    mount(<DownstreamMarkdown markdown="x" />);
    expect(nativeView.last()?.selectable).toBe(true);
  });

  it('selects by default for a stream too', () => {
    const session = stream();
    mount(<DownstreamMarkdown stream={session} />);
    expect(nativeView.last()?.selectable).toBe(true);
  });

  it('routes a code copy to the code handler', () => {
    const onCopy = vi.fn();
    const tableCopy = vi.fn();
    mount(
      <DownstreamMarkdown
        markdown={SOURCE}
        controls={{
          code: { copy: { onCopy } },
          table: { copy: { onCopy: tableCopy } },
        }}
      />
    );
    fire('onControlAction', controlEvent());
    expect(onCopy).toHaveBeenCalledWith({
      block: 'code',
      action: 'copy',
      format: 'text',
      language: 'ts',
    });
    expect(tableCopy).not.toHaveBeenCalled();
  });

  it('routes a table copy to the table handler with its format', () => {
    const onCopy = vi.fn();
    mount(
      <DownstreamMarkdown
        markdown={SOURCE}
        controls={{ table: { copy: { onCopy } } }}
      />
    );
    fire(
      'onControlAction',
      controlEvent({
        block: 'table',
        action: 'copy',
        format: 'csv',
        language: '',
      })
    );
    expect(onCopy).toHaveBeenCalledWith({
      block: 'table',
      action: 'copy',
      format: 'csv',
      language: '',
    });
  });

  it('routes a download with the file name the native side chose', () => {
    const onDownload = vi.fn();
    mount(
      <DownstreamMarkdown
        markdown={SOURCE}
        controls={{ code: { download: { onDownload } } }}
      />
    );
    fire(
      'onControlAction',
      controlEvent({ action: 'download', filename: 'app.ts' })
    );
    expect(onDownload).toHaveBeenCalledWith({
      block: 'code',
      action: 'download',
      format: 'text',
      language: 'ts',
      filename: 'app.ts',
    });
  });

  it('routes a wrap toggle with the layout the press left behind', () => {
    const onToggle = vi.fn();
    mount(
      <DownstreamMarkdown
        markdown={SOURCE}
        controls={{ code: { wrap: { onToggle } } }}
      />
    );
    fire(
      'onControlAction',
      controlEvent({ action: 'wrap', format: '', wrapped: true })
    );
    expect(onToggle).toHaveBeenCalledWith({
      block: 'code',
      action: 'wrap',
      wrapped: true,
      language: 'ts',
    });
  });

  it('drops an event that names a control this build does not have', () => {
    const onCopy = vi.fn();
    mount(
      <DownstreamMarkdown
        markdown={SOURCE}
        controls={{ code: { copy: { onCopy } } }}
      />
    );
    fire(
      'onControlAction',
      controlEvent({ block: 'mermaid', action: 'download' })
    );
    fire('onControlAction', controlEvent({ action: 'fullscreen' }));
    expect(onCopy).not.toHaveBeenCalled();
  });

  it('drops a format native does not know rather than passing it on', () => {
    const onCopy = vi.fn();
    mount(
      <DownstreamMarkdown
        markdown={SOURCE}
        controls={{ table: { copy: { onCopy } } }}
      />
    );
    fire(
      'onControlAction',
      controlEvent({ block: 'table', format: 'pdf', language: '' })
    );
    expect(onCopy).toHaveBeenCalledTimes(1);
    expect(onCopy.mock.calls[0]?.[0]).not.toHaveProperty('format');
  });

  it('routes a control failure to that control onError', () => {
    const onError = vi.fn();
    const viewError = vi.fn();
    mount(
      <DownstreamMarkdown
        markdown={SOURCE}
        onError={viewError}
        controls={{ code: { download: { onError } } }}
      />
    );
    fire(
      'onMarkdownError',
      controlError(
        'code',
        'download',
        'the clipboard could not take that much text.'
      )
    );
    expect(onError).toHaveBeenCalledTimes(1);
    // The block and action ride on the event, so the app is told which control
    // failed without parsing the message.
    expect(onError.mock.calls[0]?.[0]).toMatchObject({
      code: 'control_failed',
      block: 'code',
      action: 'download',
    });
    expect(viewError).not.toHaveBeenCalled();
  });

  it('routes a control failure to the view onError when the control has none', () => {
    const onError = vi.fn();
    mount(<DownstreamMarkdown markdown={SOURCE} onError={onError} />);
    fire(
      'onMarkdownError',
      controlError('table', 'copy', 'the document is gone.')
    );
    expect(onError).toHaveBeenCalledTimes(1);
  });

  it('routes a control failure by its fields, not by how the message reads', () => {
    const copyError = vi.fn();
    const onError = vi.fn();
    mount(
      <DownstreamMarkdown
        markdown={SOURCE}
        onError={onError}
        controls={{ code: { copy: { onError: copyError } } }}
      />
    );
    // A message that names the control in prose must not be mistaken for one:
    // only the fields route a failure.
    fire(
      'onMarkdownError',
      controlError('code', 'copy', 'the clipboard said table copy was refused')
    );
    expect(copyError).toHaveBeenCalledTimes(1);
    expect(onError).not.toHaveBeenCalled();
  });

  it('routes a non-control failure to the view onError', () => {
    const copyError = vi.fn();
    const onError = vi.fn();
    mount(
      <DownstreamMarkdown
        markdown={SOURCE}
        onError={onError}
        controls={{ code: { copy: { onError: copyError } } }}
      />
    );
    fire('onMarkdownError', {
      nativeEvent: {
        code: 'parse_failed',
        message: 'The Markdown parser could not complete.',
        generation: 1,
        sequence: 0,
        recoverable: true,
        block: '',
        action: '',
      },
    });
    expect(onError).toHaveBeenCalledTimes(1);
    expect(copyError).not.toHaveBeenCalled();
  });

  it('drops a control failure from a generation that is gone', () => {
    const onError = vi.fn();
    mount(<DownstreamMarkdown markdown={SOURCE} onError={onError} />);
    fire(
      'onMarkdownError',
      controlError('code', 'copy', 'too large.', { generation: 99 })
    );
    expect(onError).not.toHaveBeenCalled();
  });

  it('reports nothing when a control is turned off', () => {
    const onCopy = vi.fn();
    mount(
      <DownstreamMarkdown
        markdown={SOURCE}
        controls={{ code: { copy: { onCopy } } }}
      />
    );
    rerender(
      <DownstreamMarkdown
        markdown={SOURCE}
        controls={{ code: { copy: false } }}
      />
    );
    fire('onControlAction', controlEvent());
    expect(onCopy).not.toHaveBeenCalled();
  });

  it('drops a control action from a document that has been replaced', () => {
    const onCopy = vi.fn();
    mount(
      <DownstreamMarkdown
        markdown={SOURCE}
        controls={{ code: { copy: { onCopy } } }}
      />
    );
    const stale = Number(nativeView.last()?.generation ?? 0);
    rerender(
      <DownstreamMarkdown
        markdown={`${SOURCE}\n\nA later paragraph.\n`}
        controls={{ code: { copy: { onCopy } } }}
      />
    );
    // The press happened on the document before this one, so it describes a
    // block the reader is no longer looking at.
    fire('onControlAction', controlEvent({ generation: stale }));
    expect(onCopy).not.toHaveBeenCalled();
    fire('onControlAction', controlEvent());
    expect(onCopy).toHaveBeenCalledTimes(1);
  });

  it('sends nothing new to native when the controls are rebuilt unchanged', () => {
    mount(<DownstreamMarkdown markdown={SOURCE} controls={{ code: {} }} />);
    const first = nativeView.last();
    const firstLabels = first?.controlLabels;
    const firstHandler = first?.onControlAction;
    for (let index = 0; index < 3; index++) {
      // A fresh object literal each render, which is how this prop is written in
      // practice. Nothing about it changed, so native should be told nothing.
      rerender(
        <DownstreamMarkdown markdown={SOURCE} controls={{ code: {} }} />
      );
    }
    expect(nativeView.last()?.controlLabels).toBe(firstLabels);
    expect(nativeView.last()?.onControlAction).toBe(firstHandler);
  });

  it('sends a changed label through to native', () => {
    mount(
      <DownstreamMarkdown
        markdown={SOURCE}
        translations={{ copyCode: 'Copiar' }}
      />
    );
    expect(nativeView.last()?.controlLabels).toContain('Copiar');
    rerender(
      <DownstreamMarkdown
        markdown={SOURCE}
        translations={{ copyCode: 'Kopieren' }}
      />
    );
    expect(nativeView.last()?.controlLabels).toContain('Kopieren');
  });

  it('keeps the handlers when the control value is rebuilt each render', () => {
    const onCopy = vi.fn();
    for (let index = 0; index < 3; index++) {
      mount(
        <DownstreamMarkdown
          markdown={SOURCE}
          controls={{ code: { copy: { onCopy } } }}
        />
      );
      rerender(
        <DownstreamMarkdown
          markdown={SOURCE}
          controls={{ code: { copy: { onCopy } } }}
        />
      );
    }
    fire('onControlAction', controlEvent());
    expect(onCopy).toHaveBeenCalledTimes(1);
  });
});

describe('DownstreamMarkdown math and image callbacks', () => {
  for (const mode of ['controlled', 'stream'] as const) {
    it(`keeps ${mode} native callbacks stable and calls the newest handlers`, () => {
      const source =
        mode === 'controlled' ? { markdown: 'x' } : { stream: stream() };
      const oldMath = vi.fn();
      const oldImage = vi.fn();
      const newMath = vi.fn();
      const newImage = vi.fn();
      mount(
        <DownstreamMarkdown
          {...source}
          onMathError={oldMath}
          onImagePress={oldImage}
        />
      );
      const first = nativeView.last() as Record<string, unknown>;
      rerender(
        <DownstreamMarkdown
          {...source}
          onMathError={newMath}
          onImagePress={newImage}
        />
      );
      const next = nativeView.last() as Record<string, unknown>;
      expect(next.onMathError).toBe(first.onMathError);
      expect(next.onImagePress).toBe(first.onImagePress);
      const mathError = next.onMathError as (event: object) => void;
      const imagePress = next.onImagePress as (event: object) => void;
      mathError({
        nativeEvent: {
          source: '\\bad',
          message: 'Invalid formula',
          display: false,
          generation: next.generation,
        },
      });
      imagePress({
        nativeEvent: {
          url: 'https://example.com/image.png',
          alt: 'Image',
          title: '',
          start: 0,
          end: 5,
        },
      });
      expect(oldMath).not.toHaveBeenCalled();
      expect(oldImage).not.toHaveBeenCalled();
      expect(newMath).toHaveBeenCalledTimes(1);
      expect(newImage).toHaveBeenCalledWith({
        url: 'https://example.com/image.png',
        alt: 'Image',
        range: { start: 0, end: 5 },
      });
      mathError({
        nativeEvent: {
          source: '\\stale',
          message: 'Old formula',
          display: true,
          generation: Number(next.generation) + 1,
        },
      });
      expect(newMath).toHaveBeenCalledTimes(1);
    });
  }

  it('keeps stream math callbacks stable across snapshots and rejects pre-reset errors', () => {
    const session = stream();
    const onMathError = vi.fn();
    mount(<DownstreamMarkdown stream={session} onMathError={onMathError} />);
    const first = nativeView.last() as Record<string, unknown>;
    session.append('Next');
    getStreamState(session).flush();
    runFrames();
    expect(nativeView.last()?.onMathError).toBe(first.onMathError);
    act(() => session.reset('Reset'));
    const mathError = first.onMathError as (event: object) => void;
    mathError({
      nativeEvent: {
        source: '\\bad',
        message: 'Invalid formula',
        display: true,
        generation: first.generation,
      },
    });
    expect(onMathError).not.toHaveBeenCalled();
    runFrames();
    const next = nativeView.last() as Record<string, unknown>;
    expect(next.onMathError).toBe(first.onMathError);
    mathError({
      nativeEvent: {
        source: '\\bad',
        message: 'Invalid formula',
        display: true,
        generation: next.generation,
      },
    });
    expect(onMathError).toHaveBeenCalledTimes(1);
  });
});

describe('DownstreamMarkdown theme', () => {
  /**
   * The theme props are usually inline literals, so their identity changes on
   * every render of the parent. What native must not see is a fresh array for a
   * configuration that has not changed: it would re-intern and rebuild the
   * rendering behind every message in a chat.
   */
  function themeProps(): {
    themeSlots?: readonly number[];
    themeValues?: readonly number[];
    themeFonts?: readonly string[];
  } {
    const props = nativeView.last() as Record<string, unknown>;
    return {
      themeSlots: props.themeSlots as readonly number[] | undefined,
      themeValues: props.themeValues as readonly number[] | undefined,
      themeFonts: props.themeFonts as readonly string[] | undefined,
    };
  }

  it('sends nothing at all for a view with no theme', () => {
    mount(<DownstreamMarkdown markdown="x" />);
    // Three empty arrays rather than three absent ones: the empty case is the
    // shape the default theme is, so "no theme" is not a path native takes.
    expect(themeProps().themeSlots).toEqual([]);
    expect(themeProps().themeValues).toEqual([]);
    expect(themeProps().themeFonts).toEqual([]);
  });

  it('sends the resolved slots for a token', () => {
    mount(
      <DownstreamMarkdown
        markdown="x"
        theme={{ radius: 12, colors: { primary: '#112233' } }}
      />
    );
    const { themeSlots, themeValues } = themeProps();
    expect(themeSlots?.length).toBeGreaterThan(0);
    expect(themeValues?.length).toBe(themeSlots?.length);
    // Sorted, so the shadow node can compare them as vectors.
    expect([...(themeSlots ?? [])].sort((a, b) => a - b)).toEqual(themeSlots);
  });

  it('sends nothing new when an inline theme is equal by value', () => {
    const theme = { colors: { foreground: '#111111' } };
    mount(<DownstreamMarkdown markdown="x" theme={theme} />);
    const first = themeProps();
    expect(first.themeSlots?.length).toBeGreaterThan(0);

    // A fresh literal with the same contents: the common case, since a theme is
    // usually written inline.
    rerender(
      <DownstreamMarkdown
        markdown="x"
        theme={{ colors: { foreground: '#111111' } }}
      />
    );
    expect(themeProps().themeSlots).toBe(first.themeSlots);
    expect(themeProps().themeValues).toBe(first.themeValues);
    expect(themeProps().themeFonts).toBe(first.themeFonts);
    expect(nativeView.count()).toBe(2);
  });

  for (const mode of ['controlled', 'stream'] as const) {
    function sourceProps(): MarkdownViewProps {
      return mode === 'controlled' ? { markdown: 'x' } : { stream: stream() };
    }

    it(`updates ${mode} slots when equal values move to a different element`, () => {
      const source = sourceProps();
      mount(
        <DownstreamMarkdown
          {...source}
          markdownStyle={{ paragraph: { fontSize: 20 } }}
        />
      );
      const first = themeProps();
      rerender(
        <DownstreamMarkdown
          {...source}
          markdownStyle={{ h1: { fontSize: 20 } }}
        />
      );
      expect(themeProps().themeValues).toEqual(first.themeValues);
      expect(themeProps().themeSlots).toEqual([
        themeSlot('heading.1.fontSize')!.id,
      ]);
      expect(themeProps().themeSlots).not.toBe(first.themeSlots);
    });

    it(`updates changed and removed ${mode} fonts`, () => {
      const source = sourceProps();
      mount(
        <DownstreamMarkdown
          {...source}
          markdownStyle={{ paragraph: { fontFamily: 'Inter' } }}
        />
      );
      const first = themeProps();
      rerender(
        <DownstreamMarkdown
          {...source}
          markdownStyle={{ paragraph: { fontFamily: 'System' } }}
        />
      );
      expect(themeProps().themeFonts).toEqual(['System']);
      expect(themeProps().themeValues).toEqual(first.themeValues);
      rerender(<DownstreamMarkdown {...source} />);
      expect(themeProps().themeSlots).toEqual([]);
      expect(themeProps().themeValues).toEqual([]);
      expect(themeProps().themeFonts).toEqual([]);
    });

    it(`compares the complete ${mode} font table when slot values stay equal`, () => {
      const source = sourceProps();
      mount(
        <DownstreamMarkdown
          {...source}
          markdownStyle={{ paragraph: { fontFamily: 'Inter' } }}
        />
      );
      const first = themeProps();
      rerender(
        <DownstreamMarkdown
          {...source}
          markdownStyle={[
            { paragraph: { fontFamily: 'Inter' } },
            { paragraph: { fontFamily: 'System' } },
            { paragraph: { fontFamily: 'Inter' } },
          ]}
        />
      );
      expect(themeProps().themeSlots).toEqual(first.themeSlots);
      expect(themeProps().themeValues).toEqual(first.themeValues);
      expect(themeProps().themeFonts).toEqual(['Inter', 'System']);
    });

    it(`preserves ${mode} array identities for equivalent inline styles and fonts`, () => {
      const source = sourceProps();
      mount(
        <DownstreamMarkdown
          {...source}
          markdownStyle={{ paragraph: { fontSize: 20, fontFamily: 'Inter' } }}
        />
      );
      const first = themeProps();
      rerender(
        <DownstreamMarkdown
          {...source}
          markdownStyle={{ paragraph: { fontFamily: 'Inter', fontSize: 20 } }}
        />
      );
      expect(themeProps().themeSlots).toBe(first.themeSlots);
      expect(themeProps().themeValues).toBe(first.themeValues);
      expect(themeProps().themeFonts).toBe(first.themeFonts);
    });
  }

  it('sends one update when the theme changes', () => {
    mount(<DownstreamMarkdown markdown="x" theme={{ radius: 12 }} />);
    const first = themeProps();

    rerender(<DownstreamMarkdown markdown="x" theme={{ radius: 16 }} />);
    expect(themeProps().themeValues).not.toEqual(first.themeValues);
    expect(nativeView.count()).toBe(2);
  });

  it('keeps the theme across a stream rebind', () => {
    // A stream rebinds its session; the theme has nothing to do with that, so it
    // must not be re-resolved or re-sent as a new value.
    const session = stream();
    session.append('hello');
    getStreamState(session).flush();
    mount(
      <DownstreamMarkdown
        stream={session}
        theme={{ colors: { primary: '#445566' } }}
      />
    );
    runFrames();
    const first = themeProps();
    expect(first.themeSlots?.length).toBeGreaterThan(0);

    session.append(' world');
    getStreamState(session).flush();
    runFrames();
    expect(themeProps().themeSlots).toEqual(first.themeSlots);
    expect(themeProps().themeValues).toEqual(first.themeValues);
  });

  it('carries a light and dark pair as two values', () => {
    mount(
      <DownstreamMarkdown
        markdown="x"
        theme={{
          colors: { foreground: { light: '#ffffff', dark: '#000000' } },
        }}
      />
    );
    const { themeSlots, themeValues } = themeProps();
    expect(themeSlots?.length).toBe(themeValues?.length);
    // A pair is two entries, so more values than a single color would send.
    expect(themeValues?.some((value) => value === 0xffffffff)).toBe(true);
    expect(themeValues?.some((value) => value === 0xff000000)).toBe(true);
  });

  it('sends both halves of a composed preset and override', () => {
    mount(
      <DownstreamMarkdown
        markdown="x"
        theme={[presets.github.theme, { radius: 12 }]}
        markdownStyle={[
          presets.github.markdownStyle,
          { link: { color: '#ffffff' } },
        ]}
      />
    );
    const { themeSlots, themeValues, themeFonts } = themeProps();
    expect(themeSlots?.length).toBeGreaterThan(10);
    // Every preset color plus the override, so a preset is a starting point
    // rather than a whole replacement.
    expect(themeValues).toContain(0xffffffff);
    // The GitHub preset names no font, so an override is what puts one on the wire.
    expect(themeFonts).toEqual([]);
  });

  it('sends an element style over a token from the same theme', () => {
    mount(
      <DownstreamMarkdown
        markdown="x"
        theme={{ colors: { foreground: '#111111' } }}
        markdownStyle={{ paragraph: { fontSize: 20 } }}
      />
    );
    const { themeSlots, themeValues } = themeProps();
    // The element style is a size, so it arrives as a metric rather than as a
    // color, and the token's colors are still there beside it.
    expect(themeValues).toContain(20);
    expect(themeValues?.some((value) => value === 0xff111111)).toBe(true);
    expect(themeSlots?.length).toBe(themeValues?.length);
  });
});

/* eslint-disable no-bitwise -- native parser flags are a bitmask */
/** @vitest-environment happy-dom */
import { act } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { Downstream } from '../src/Downstream';
import { DownstreamProvider } from '../src/DownstreamProvider';
import {
  createMarkdownStream,
  getStreamState,
} from '../src/createMarkdownStream';
import { math } from '../math/index.js';
import { code } from '../code/index.js';
import type { DownstreamPlugin } from '../src/plugins';
import {
  PARSE_FLAG_ENABLED,
  PARSE_FLAG_NO_CODE_HIGHLIGHT,
  resetMathEngineWarning,
} from '../src/parseFlags';
import { mount, type MountedHost } from './native-view-spy';
import { createNativeView } from './native-view';
import {
  realEngineModule,
  resetEngine,
  snapshotOf,
  parseFlagsOf,
} from './real-engine-module';
import {
  parse,
  PARSE_CJK,
  PARSE_INLINE_HTML,
  PARSE_ENABLED,
  PARSE_SINGLE_DOLLAR,
  PARSE_BRACKETS,
} from './engine';

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
let frameId: number;
const streams: ReturnType<typeof createMarkdownStream>[] = [];
function stream(source: string) {
  const controller = createMarkdownStream();
  controller.reset(source);
  controller.finish();
  streams.push(controller);
  return controller;
}
function runFrames() {
  for (let pass = 0; frames.size && pass < 10; pass++) {
    act(() => {
      const callbacks = [...frames.values()];
      frames.clear();
      callbacks.forEach((callback) => callback(0));
    });
  }
  expect(frames.size).toBe(0);
}
beforeEach(() => {
  resetEngine();
  resetMathEngineWarning();
  nativeView.reset();
  frames = new Map();
  frameId = 0;
  vi.stubGlobal('__DEV__', true);
  vi.spyOn(console, 'warn').mockImplementation(() => {});
  vi.spyOn(globalThis, 'requestAnimationFrame').mockImplementation(
    (callback) => {
      frames.set(++frameId, callback);
      return frameId;
    }
  );
  vi.spyOn(globalThis, 'cancelAnimationFrame').mockImplementation((id) =>
    frames.delete(id)
  );
});
afterEach(() => {
  view?.unmount();
  view = undefined;
  streams.splice(0).forEach((controller) => controller.dispose());
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
});

describe('explicit native plugins through the real parser', () => {
  it('keeps math literal and skips code tokens without a plugin, even with engines linked', () => {
    const controller = stream('$$x^2$$\n\n```ts\nconst answer = 42;\n```');
    view = mount(<Downstream stream={controller} />);
    runFrames();
    const id = getStreamState(controller).sessionId;
    expect(parseFlagsOf(id) & PARSE_FLAG_ENABLED).toBe(0);
    expect(parseFlagsOf(id) & PARSE_FLAG_NO_CODE_HIGHLIGHT).not.toBe(0);
    const blocks = snapshotOf(id)!.blocks;
    expect(blocks[0]?.text).toBe('$$x^2$$');
    expect(blocks.find((block) => block.language === 'ts')?.tokens).toEqual([]);
    expect(nativeView.last()?.syntaxHighlighting).toBe(false);
    expect(console.warn).toHaveBeenCalledOnce();
    view.rerender(<Downstream stream={controller} />);
    expect(console.warn).toHaveBeenCalledOnce();
  });

  it('code() matches existing highlighted output and can be removed from a finished controller', () => {
    const source = '```ts\nconst answer: number = 42;\n```';
    const controller = stream(source);
    const id = getStreamState(controller).sessionId;
    view = mount(<Downstream stream={controller} plugins={[code()]} />);
    runFrames();
    const expected = parse(source, PARSE_CJK | PARSE_INLINE_HTML);
    expect(snapshotOf(id)?.blocks).toEqual(expected.blocks);
    expect(snapshotOf(id)?.blocks[0]?.tokens.length).toBeGreaterThan(0);
    expect(nativeView.last()?.syntaxHighlighting).toBe(true);
    view.rerender(<Downstream stream={controller} plugins={[]} />);
    runFrames();
    expect(snapshotOf(id)?.blocks[0]?.tokens).toEqual([]);
    expect(snapshotOf(id)?.blocks[0]?.text).toBe('const answer: number = 42;');
    expect(nativeView.last()?.syntaxHighlighting).toBe(false);
  });

  it('math(options) matches the existing math parser output', () => {
    const source = 'A $x+1$ formula and \\(y^2\\).\n\n$$z^2$$';
    const controller = stream(source);
    view = mount(
      <Downstream
        stream={controller}
        plugins={[math({ singleDollarTextMath: true })]}
      />
    );
    runFrames();
    const id = getStreamState(controller).sessionId;
    const flags =
      PARSE_CJK |
      PARSE_INLINE_HTML |
      PARSE_ENABLED |
      PARSE_SINGLE_DOLLAR |
      PARSE_BRACKETS |
      PARSE_FLAG_NO_CODE_HIGHLIGHT;
    expect(snapshotOf(id)?.blocks).toEqual(parse(source, flags).blocks);
    expect(parseFlagsOf(id) & PARSE_FLAG_ENABLED).not.toBe(0);
    expect(console.warn).not.toHaveBeenCalled();
  });

  it('inherits provider plugins and permits an explicit empty list per view', () => {
    view = mount(
      <DownstreamProvider plugins={[code(), math()]}>
        <Downstream>$$x$$</Downstream>
        <Downstream plugins={[]}>$$literal$$</Downstream>
      </DownstreamProvider>
    );
    const records = nativeView.records;
    expect(records[0]?.syntaxHighlighting).toBe(true);
    expect((records[0]?.parseFlags as number) & PARSE_FLAG_ENABLED).not.toBe(0);
    expect(records.at(-1)?.syntaxHighlighting).toBe(false);
    expect((records.at(-1)?.parseFlags as number) & PARSE_FLAG_ENABLED).toBe(0);
  });

  it('applies plugin styles beneath provider and view styles, and preserves explicit parse overrides', () => {
    const plugin: DownstreamPlugin = {
      name: 'custom',
      parse: { syntax: { underline: true }, cjk: false },
      style: { markdownStyle: { tags: { user: { color: '#ff0000' } } } },
    };
    view = mount(
      <DownstreamProvider plugins={[plugin]} cjk>
        <Downstream
          syntax={{ superscript: true }}
          markdownStyle={{ tags: { user: { fontWeight: 'bold' } } }}
        >
          ^up^
        </Downstream>
      </DownstreamProvider>
    );
    const props = nativeView.last()!;
    expect((props.parseFlags as number) & PARSE_CJK).not.toBe(0);
    expect(props.tagStyleNames).toEqual(['user']);
    expect(props.tagStyleColors).not.toEqual([-1]);
    expect((props.tagStyleFlags as number[])[0]).toBeGreaterThan(0);
  });

  it('rejects duplicate plugin names in development and removed public feature props', () => {
    expect(() =>
      mount(<Downstream plugins={[code(), code()]}>text</Downstream>)
    ).toThrow(/Duplicate/);
    // @ts-expect-error Highlighting is selected by code().
    const oldCode = <Downstream syntaxHighlighting>text</Downstream>;
    expect(() => mount(oldCode)).toThrow(/feature plugin/);
    // @ts-expect-error Math is selected by math().
    const oldMath = <Downstream math>text</Downstream>;
    expect(() => mount(oldMath)).toThrow(/feature plugin/);
  });

  it('lets a view revoke tag allowlists inherited from plugins and providers', () => {
    const plugin: DownstreamPlugin = {
      name: 'tags',
      parse: { allowedTags: { chart: true } },
    };
    view = mount(
      <DownstreamProvider plugins={[plugin]} allowedTags={{ user: true }}>
        <Downstream allowedTags={{}}>{'<user>Bob</user>'}</Downstream>
      </DownstreamProvider>
    );
    expect(nativeView.last()?.allowedTags).toEqual([]);
  });

  it('does not warn about installed-but-unused math in production', () => {
    vi.stubGlobal('__DEV__', false);
    const available = vi.spyOn(realEngineModule.default, 'mathAvailable');
    view = mount(<Downstream>literal</Downstream>);
    const reads = available.mock.calls.length;
    view.rerender(<Downstream>updated literal</Downstream>);
    expect(available).toHaveBeenCalledTimes(reads);
    expect(console.warn).not.toHaveBeenCalled();
  });
});

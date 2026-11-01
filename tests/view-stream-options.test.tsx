/* eslint-disable no-bitwise -- native inline styles are a bitmask */
/** @vitest-environment happy-dom */
import { act, StrictMode } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { Downstream } from '../src/Downstream';
import { DownstreamProvider } from '../src/DownstreamProvider';
import {
  createMarkdownStream,
  getStreamState,
} from '../src/createMarkdownStream';
import { parse, STYLE } from './engine';
import { createNativeView } from './native-view';
import { mount, type MountedHost } from './native-view-spy';
import {
  realEngineModule,
  resetEngine,
  snapshotOf,
  sourceForSession,
  parseFlagsOf,
} from './real-engine-module';
import { resolveTagStyles } from '../src/tagStyles';
import { resolveLinkVariants } from '../src/linkVariants';

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
const streams: ReturnType<typeof createMarkdownStream>[] = [];

function stream() {
  const controller = createMarkdownStream();
  streams.push(controller);
  return controller;
}

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
  vi.spyOn(globalThis, 'cancelAnimationFrame').mockImplementation((id) =>
    frames.delete(id)
  );
});

afterEach(() => {
  view?.unmount();
  view = undefined;
  streams.splice(0).forEach((controller) => controller.dispose());
  vi.restoreAllMocks();
});

describe('view-owned controller parsing', () => {
  it('buffers and finishes before mount without creating native sessions', async () => {
    const create = vi.spyOn(native, 'create');
    const controller = stream();
    controller.append('discarded');
    controller.reset('^up^');
    controller.append(' and a [page](/docs)');
    controller.finish();
    expect(await controller.getSource()).toBe('^up^ and a [page](/docs)');
    expect(create).not.toHaveBeenCalled();
    view = mount(
      <Downstream
        stream={controller}
        syntax={{ superscript: true }}
        links={{ relative: true }}
      />
    );
    runFrames();
    const id = getStreamState(controller).sessionId;
    expect(create).toHaveBeenCalledTimes(1);
    expect(snapshotOf(id)?.blocks[0]?.text).toBe('up and a page');
    expect(snapshotOf(id)?.links[0]?.url).toBe('/docs');
    expect(snapshotOf(id)?.blocks).toEqual(
      parse(await controller.getSource(), parseFlagsOf(id)).blocks
    );
  });

  it('rebuilds with pending text, ignores old publications, and keeps equivalent options stable', async () => {
    const controller = stream();
    const create = vi.spyOn(native, 'create');
    const dispose = vi.spyOn(native, 'dispose');
    view = mount(<Downstream stream={controller} />);
    const state = getStreamState(controller);
    const id = state.sessionId;
    act(() => controller.append('^one^'));
    runFrames();
    const old = state.binding();
    act(() => controller.append(' and ~two~'));
    view.rerender(
      <Downstream
        stream={controller}
        syntax={{ superscript: true, subscript: true }}
      />
    );
    runFrames();
    expect(sourceForSession(id)).toBe('^one^ and ~two~');
    expect(snapshotOf(id)?.blocks[0]?.text).toBe('one and two');
    expect(state.binding().generation).toBeGreaterThan(old.generation);
    const current = state.binding();
    state.snapshotReady({
      sessionId: id,
      generation: old.generation,
      sequence: old.sequence,
    });
    runFrames();
    expect(state.binding()).toEqual(current);
    view.rerender(
      <Downstream
        stream={controller}
        syntax={{ superscript: true, subscript: true }}
      />
    );
    expect(create).toHaveBeenCalledTimes(2);
    expect(dispose).toHaveBeenCalledTimes(1);
    expect(await controller.getSource()).toBe('^one^ and ~two~');
  });

  it('preserves finish while options change and supports a later reset', () => {
    const controller = stream();
    controller.append('^up^ **literal');
    controller.finish();
    view = mount(<Downstream stream={controller} />);
    runFrames();
    const id = getStreamState(controller).sessionId;
    view.rerender(
      <Downstream stream={controller} syntax={{ superscript: true }} preview />
    );
    runFrames();
    expect(snapshotOf(id)?.blocks[0]?.text).toBe('up **literal');
    expect(() => controller.append('no')).toThrow(/Reset the stream/);
    act(() => controller.reset('^new^'));
    runFrames();
    expect(snapshotOf(id)?.blocks[0]?.text).toBe('new');
  });

  it('keeps parsing after unmount and applies new view options on remount', () => {
    const controller = stream();
    view = mount(<Downstream stream={controller} preview={false} />);
    const id = getStreamState(controller).sessionId;
    act(() => controller.append('^first^'));
    runFrames();
    view.unmount();
    view = undefined;
    controller.append(' ^second^');
    runFrames();
    expect(sourceForSession(id)).toBe('^first^ ^second^');
    view = mount(
      <Downstream stream={controller} syntax={{ superscript: true }} />
    );
    runFrames();
    expect(snapshotOf(id)?.blocks[0]?.text).toBe('first second');
  });

  it('rebuilds allowed-tag and literal-content policies through the real parser', () => {
    const controller = stream();
    controller.reset('<user>**Bob**</user>');
    controller.finish();
    view = mount(<Downstream stream={controller} />);
    runFrames();
    const id = getStreamState(controller).sessionId;
    expect(snapshotOf(id)?.blocks[0]?.text).toContain('<user>');
    view.rerender(
      <Downstream stream={controller} allowedTags={{ user: true }} />
    );
    runFrames();
    expect(snapshotOf(id)?.blocks[0]?.text).toBe('Bob');
    expect(
      snapshotOf(id)?.blocks[0]?.runs.some(
        (run) => (run[2] & STYLE.strong) !== 0
      )
    ).toBe(true);
    view.rerender(
      <Downstream
        stream={controller}
        allowedTags={{ user: true }}
        literalTagContent
      />
    );
    runFrames();
    expect(snapshotOf(id)?.blocks[0]?.text).toBe('Bob');
    expect(
      snapshotOf(id)?.blocks[0]?.runs.some(
        (run) => (run[2] & STYLE.strong) !== 0
      )
    ).toBe(false);
    view.rerender(<Downstream stream={controller} allowedTags={{}} />);
    runFrames();
    expect(snapshotOf(id)?.blocks[0]?.text).toContain('<user>');
  });

  it('applies provider parse options to both text and controller sources', () => {
    const controller = stream();
    controller.reset('^same^');
    controller.finish();
    view = mount(
      <DownstreamProvider syntax={{ superscript: true }}>
        <Downstream>^same^</Downstream>
        <Downstream stream={controller} />
      </DownstreamProvider>
    );
    runFrames();
    expect(
      snapshotOf(getStreamState(controller).sessionId)?.blocks[0]?.text
    ).toBe('same');
    expect(
      nativeView.records.every(
        (props) =>
          props.parseFlags ===
          parseFlagsOf(getStreamState(controller).sessionId)
      )
    ).toBe(true);
  });

  it('retains one native session across StrictMode effect replay', () => {
    const controller = stream();
    const create = vi.spyOn(native, 'create');
    controller.reset('hello');
    view = mount(
      <StrictMode>
        <Downstream stream={controller} />
      </StrictMode>
    );
    runFrames();
    expect(create).toHaveBeenCalledTimes(1);
    expect(sourceForSession(getStreamState(controller).sessionId)).toBe(
      'hello'
    );
  });

  it('routes session errors to the latest bound view callback', () => {
    const controller = stream();
    const first = vi.fn();
    const second = vi.fn();
    view = mount(<Downstream stream={controller} onError={first} />);
    view.rerender(<Downstream stream={controller} onError={second} />);
    const state = getStreamState(controller);
    state.sessionError({
      ...state.binding(),
      code: 'parse_failed',
      message: 'failure',
      recoverable: true,
    });
    expect(first).not.toHaveBeenCalled();
    expect(second).toHaveBeenCalledOnce();
  });

  it('rejects removed controller options before creating a native session', () => {
    const create = vi.spyOn(native, 'create');
    // @ts-expect-error Parse options now belong to the view.
    expect(() => createMarkdownStream({ math: true })).toThrow(
      /belongs on Downstream/
    );
    // @ts-expect-error Preview options now belong to the view.
    expect(() => createMarkdownStream({ preview: true })).toThrow(
      /belongs on Downstream/
    );
    expect(create).not.toHaveBeenCalled();
  });
});

describe('provider style composition reaches native', () => {
  it('inherits tags and link variants when a view sets an unrelated element style', () => {
    const inherited = {
      tags: { user: { color: '#ff0000' } },
      linkVariants: [{ match: 'https://example.com', color: '#00ff00' }],
    };
    const local = {
      paragraph: { fontSize: 18 },
      tags: { USER: { fontWeight: 'bold' as const } },
    };
    view = mount(
      <DownstreamProvider markdownStyle={inherited}>
        <Downstream markdownStyle={local}>hello</Downstream>
      </DownstreamProvider>
    );
    const tags = resolveTagStyles([inherited, local]);
    const variants = resolveLinkVariants([inherited, local]);
    expect(nativeView.last()?.tagStyleNames).toEqual(['user']);
    expect(nativeView.last()?.tagStyleColors).toEqual(
      tags.map((tag) => tag.color)
    );
    expect(nativeView.last()?.tagStyleFlags).toEqual(
      tags.map((tag) => tag.flags)
    );
    expect(tags[0]?.flags).toBeGreaterThan(0);
    expect(nativeView.last()?.linkVariantPrefixes).toEqual(
      variants.flatMap((variant) => variant.match)
    );
    expect(nativeView.last()?.linkVariantColors).toEqual(
      variants.map((variant) => variant.color)
    );
  });
});

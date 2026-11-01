/** @vitest-environment happy-dom */
import React, { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { DownstreamMarkdown } from '../src/DownstreamMarkdown';
import {
  resolveTagRenderers,
  TagSlots,
  useStableTagRenderers,
  useTagSlots,
  type TagQuery,
} from '../src/tagRenderers';
import type { MarkdownPlugins, TagRendererProps } from '../src/types';
import { resetEngine } from './real-engine-module';

const queries = vi.hoisted(() => ({
  controlled: vi.fn(),
  stream: vi.fn(),
  delta: vi.fn(),
}));
vi.mock('../src/NativeDownstreamSession', async () => {
  const { realEngineModule } = await import('./real-engine-module');
  return {
    ...realEngineModule,
    default: {
      ...realEngineModule.default,
      setParseTags: vi.fn(),
      tagSlots: queries.controlled,
      sessionTagSlots: queries.stream,
      sessionTagSlotChanges: queries.delta,
    },
  };
});
vi.mock('../src/DownstreamMarkdownViewNativeComponent', () => ({
  default: ({ children }: { children?: React.ReactNode }) => (
    <div>{children}</div>
  ),
}));
vi.mock('../src/DownstreamFenceSlotNativeComponent', () => ({
  default: ({ children }: { children?: React.ReactNode }) => (
    <div>{children}</div>
  ),
}));

(
  globalThis as unknown as { IS_REACT_ACT_ENVIRONMENT: boolean }
).IS_REACT_ACT_ENVIRONMENT = true;
let host: HTMLDivElement;
let root: Root;
const Chart = (props: TagRendererProps) => <span>{props.markdown}</span>;
const source = '<CHART>first</CHART>';
const slots = [
  {
    container: 1,
    name: 'chart',
    attributes: [],
    markdown: 'first',
    incomplete: false,
  },
];

function mount(element: React.ReactElement) {
  act(() => root.render(element));
}

function Harness({
  plugins,
  ...query
}: Omit<TagQuery, 'resolved'> & { plugins?: MarkdownPlugins }) {
  const resolved = useStableTagRenderers(resolveTagRenderers(plugins));
  const current = useTagSlots({ ...query, resolved });
  return <TagSlots slots={current} scope="test" />;
}

beforeEach(() => {
  resetEngine();
  queries.controlled.mockReset().mockReturnValue(slots);
  queries.stream.mockReset().mockReturnValue(slots);
  queries.delta.mockReset().mockImplementation((...args) => {
    const answer = queries.stream(...args.slice(0, 4));
    return {
      token: 1,
      order: answer.map((slot: { container: number }) => slot.container),
      changed: answer,
    };
  });
  host = document.body.appendChild(document.createElement('div'));
  root = createRoot(host);
});
afterEach(() => {
  act(() => root.unmount());
  host.remove();
  vi.restoreAllMocks();
});

describe('tag renderer query reuse', () => {
  it('delta subscribers retain unchanged slot props and remove obsolete hosted IDs', () => {
    const snapshots: Array<ReturnType<typeof useTagSlots>> = [];
    const resolved = resolveTagRenderers({
      tags: [{ name: 'chart', component: Chart }],
    });
    function Capture({
      revision,
      subscriber,
    }: {
      revision: number;
      subscriber: string;
    }) {
      const current = useTagSlots({
        sessionId: subscriber,
        generation: 0,
        sequence: 1,
        revision,
        parseFlags: 0,
        allowedTags: [],
        resolved,
      });
      snapshots.push(current);
      return <TagSlots slots={current} scope={subscriber} />;
    }
    const deltaCalls: number[] = [];
    queries.delta.mockImplementation(
      (_id, _generation, _sequence, _names, baseToken: number) => {
        deltaCalls.push(baseToken);
        return baseToken === 0
          ? { token: 11, order: [1], changed: slots }
          : { token: 12, order: [1], changed: [] };
      }
    );
    mount(
      <>
        <Capture revision={1} subscriber="a" />
        <Capture revision={1} subscriber="b" />
      </>
    );
    const first = snapshots.slice();
    mount(
      <>
        <Capture revision={2} subscriber="a" />
        <Capture revision={2} subscriber="b" />
      </>
    );
    expect(deltaCalls).toEqual([0, 0, 11, 11]);
    expect(snapshots[2]?.[0]).toBe(first[0]?.[0]);
    expect(snapshots[3]?.[0]?.props).toBe(first[1]?.[0]?.props);
    expect(host.textContent).toBe('firstfirst');
    queries.delta.mockReturnValue({ token: 13, order: [], changed: [] });
    mount(
      <>
        <Capture revision={3} subscriber="a" />
        <Capture revision={3} subscriber="b" />
      </>
    );
    expect(snapshots.at(-1)).toEqual([]);
    expect(host.textContent).toBe('');
  });

  it('same-sequence closure and generation replacement receive fresh renderer props', () => {
    const snapshots: Array<ReturnType<typeof useTagSlots>> = [];
    const resolved = resolveTagRenderers({
      tags: [{ name: 'chart', component: Chart }],
    });
    function Capture({
      revision,
      generation,
    }: {
      revision: number;
      generation: number;
    }) {
      const current = useTagSlots({
        sessionId: 'a',
        generation,
        sequence: 1,
        revision,
        parseFlags: 0,
        allowedTags: [],
        resolved,
      });
      snapshots.push(current);
      return <TagSlots slots={current} scope={`a:${generation}`} />;
    }
    queries.delta.mockReturnValueOnce({
      token: 1,
      order: [1],
      changed: [{ ...slots[0], incomplete: true }],
    });
    mount(<Capture revision={1} generation={0} />);
    queries.delta.mockReturnValueOnce({
      token: 2,
      order: [1],
      changed: [{ ...slots[0], incomplete: false }],
    });
    mount(<Capture revision={2} generation={0} />);
    expect(snapshots[0]?.[0]?.props.isIncomplete).toBe(true);
    expect(snapshots[1]?.[0]?.props.isIncomplete).toBe(false);
    expect(snapshots[1]?.[0]?.props).not.toBe(snapshots[0]?.[0]?.props);
    queries.delta.mockReturnValueOnce({
      token: 3,
      order: [1],
      changed: [{ ...slots[0], markdown: 'replacement' }],
    });
    mount(<Capture revision={3} generation={1} />);
    expect(queries.delta.mock.calls.at(-1)?.[4]).toBe(0);
    expect(host.textContent).toBe('replacement');
  });

  it('growing controlled strings use session tag slots', () => {
    const frames: FrameRequestCallback[] = [];
    vi.spyOn(globalThis, 'requestAnimationFrame').mockImplementation(
      (callback) => {
        frames.push(callback);
        return frames.length;
      }
    );
    vi.spyOn(globalThis, 'cancelAnimationFrame').mockImplementation(() => {});
    const plugins = { tags: [{ name: 'chart', component: Chart }] };
    mount(
      <DownstreamMarkdown
        markdown={source}
        isAnimating
        allowedTags={{ chart: true }}
        plugins={plugins}
      />
    );
    act(() => {
      for (const callback of frames.splice(0)) callback(0);
    });
    act(() => {
      for (const callback of frames.splice(0)) callback(0);
    });
    expect(queries.controlled).not.toHaveBeenCalled();
    expect(queries.stream).toHaveBeenCalled();
    expect(host.textContent).toBe('first');
    mount(
      <DownstreamMarkdown
        markdown={source}
        isAnimating={false}
        allowedTags={{ chart: true }}
        plugins={plugins}
      />
    );
    act(() => {
      for (const callback of frames.splice(0)) callback(0);
    });
    expect(queries.controlled).not.toHaveBeenCalled();
  });

  it('equivalent inline plugins and policies do not query again on parent updates', () => {
    const seen = vi.fn(Chart);
    function Parent({ tick }: { tick: number }) {
      return (
        <div data-tick={tick}>
          <DownstreamMarkdown
            markdown={source}
            allowedTags={{ chart: true }}
            plugins={{ tags: [{ name: ['Chart', 'CHART'], component: seen }] }}
          />
        </div>
      );
    }
    mount(<Parent tick={0} />);
    expect(queries.controlled).toHaveBeenCalledTimes(1);
    expect(seen).toHaveBeenCalledTimes(1);
    mount(<Parent tick={1} />);
    mount(<Parent tick={2} />);
    expect(queries.controlled).toHaveBeenCalledTimes(1);
    expect(seen).toHaveBeenCalledTimes(1);
    expect(host.textContent).toBe('first');
  });

  it('component and fallback changes invalidate the query and mount their new output', () => {
    const Second = () => <span>second</span>;
    const Fallback = () => <span>fallback</span>;
    const NextFallback = () => <span>next fallback</span>;
    const render = (plugins: MarkdownPlugins) =>
      mount(
        <DownstreamMarkdown
          markdown={source}
          allowedTags={{ chart: true }}
          plugins={plugins}
        />
      );
    render({ tags: [{ name: 'chart', component: Chart }] });
    render({ tags: [{ name: 'CHART', component: Second }] });
    expect(queries.controlled).toHaveBeenCalledTimes(2);
    expect(host.textContent).toBe('second');
    render({ fallbackComponent: Fallback });
    expect(host.textContent).toBe('fallback');
    render({ fallbackComponent: Fallback });
    expect(queries.controlled).toHaveBeenCalledTimes(3);
    render({ fallbackComponent: NextFallback });
    expect(queries.controlled).toHaveBeenCalledTimes(4);
    expect(host.textContent).toBe('next fallback');
    render({});
    expect(host.textContent).toBe('');
    expect(queries.controlled).toHaveBeenCalledTimes(4);
  });

  it('source and attribute policy changes query with the latest document options', () => {
    const render = (markdown: string, attributes: readonly string[]) =>
      mount(
        <DownstreamMarkdown
          markdown={markdown}
          allowedTags={{ chart: attributes }}
          plugins={{ tags: [{ name: 'chart', component: Chart }] }}
        />
      );
    render(source, ['id']);
    const initialFlags = queries.controlled.mock.calls[0]![1];
    render(source, ['ID']);
    expect(queries.controlled).toHaveBeenCalledTimes(1);
    render(source, ['id', 'title']);
    expect(queries.controlled).toHaveBeenLastCalledWith(
      source,
      initialFlags,
      ['chart'],
      ['-1', 'chart', 'id', 'title']
    );
    render('<chart>second</chart>', ['title', 'id']);
    expect(queries.controlled).toHaveBeenCalledTimes(3);
    expect(queries.controlled.mock.calls[2]![0]).toBe('<chart>second</chart>');
  });

  it('fallback queries use allowed tag names without policy attributes or sentinels', () => {
    const Fallback = () => <span>fallback</span>;
    const render = (markdown: string, attributes: readonly string[]) =>
      mount(
        <DownstreamMarkdown
          markdown={markdown}
          allowedTags={{ chart: attributes, note: true }}
          plugins={{ fallbackComponent: Fallback }}
        />
      );
    render(source, ['id', 'title']);
    expect(queries.controlled).toHaveBeenLastCalledWith(
      source,
      expect.any(Number),
      ['chart', 'note'],
      ['-1', 'chart', 'id', 'title', '-1', 'note']
    );
    render(source, ['TITLE', 'ID']);
    expect(queries.controlled).toHaveBeenCalledTimes(1);
    render('<id>attribute name is not an allowed tag</id>', ['id', 'title']);
    expect(queries.controlled).toHaveBeenCalledTimes(1);
    expect(host.textContent).toBe('');
  });

  it('stream sequence, settled revision, session, generation and parse flags invalidate queries', () => {
    const allowedTags = ['-1', 'chart'];
    let query = {
      sessionId: 'first',
      generation: 0,
      sequence: 1,
      revision: 1,
      parseFlags: 0,
      allowedTags,
    };
    const render = () =>
      mount(
        <Harness
          {...query}
          plugins={{ tags: [{ name: 'CHART', component: Chart }] }}
        />
      );
    render();
    render();
    expect(queries.stream).toHaveBeenCalledTimes(1);
    for (const change of [
      { sequence: 2 },
      { revision: 2 },
      { sessionId: 'second' },
      { generation: 1 },
      { parseFlags: 1 },
    ]) {
      query = { ...query, ...change };
      render();
    }
    expect(queries.stream).toHaveBeenCalledTimes(6);
    expect(queries.controlled).not.toHaveBeenCalled();
    expect(queries.stream).toHaveBeenLastCalledWith('second', 1, 2, ['chart']);
  });

  it('lowercases a controlled source once even when several candidates are absent', () => {
    const lower = vi.spyOn(String.prototype, 'toLowerCase');
    const markdown = 'A long paragraph without any registered tags.';
    mount(
      <Harness
        source={markdown}
        generation={0}
        parseFlags={0}
        allowedTags={[]}
        plugins={{
          tags: [{ name: ['one', 'two', 'three'], component: Chart }],
        }}
      />
    );
    expect(
      lower.mock.contexts.filter((value) => value === markdown)
    ).toHaveLength(1);
    expect(queries.controlled).not.toHaveBeenCalled();
  });
});

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

const queries = vi.hoisted(() => ({ controlled: vi.fn(), stream: vi.fn() }));
vi.mock('../src/NativeDownstreamSession', async () => {
  const { realEngineModule } = await import('./real-engine-module');
  return {
    ...realEngineModule,
    default: {
      ...realEngineModule.default,
      tagSlots: queries.controlled,
      sessionTagSlots: queries.stream,
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
  host = document.body.appendChild(document.createElement('div'));
  root = createRoot(host);
});
afterEach(() => {
  act(() => root.unmount());
  host.remove();
  vi.restoreAllMocks();
});

describe('tag renderer query reuse', () => {
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

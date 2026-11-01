/** @vitest-environment happy-dom */
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { Downstream, DownstreamMarkdown } from '../src/Downstream';
import {
  DownstreamProvider,
  useDownstreamDefaults,
} from '../src/DownstreamProvider';
import { resetDeprecationWarnings } from '../src/deprecations';
import {
  mergeDownstreamOptions,
  resolveDownstreamOptions,
  type DownstreamOptions,
} from '../src/downstreamOptions';
import type { MarkdownStreamController } from '../src/types';
import { mount, type MountedHost } from './native-view-spy';

const core = vi.hoisted(() => vi.fn((_props: Record<string, unknown>) => null));
vi.mock('../src/DownstreamMarkdown', () => ({ DownstreamMarkdown: core }));

let view: MountedHost | undefined;
const Press = vi.fn();
const Fullscreen = () => null;
const Fence = () => null;
const OtherFence = () => null;
const Tag = () => null;
const Fallback = () => null;

function lastProps(): Record<string, unknown> {
  return core.mock.calls.at(-1)![0];
}

beforeEach(() => {
  resetDeprecationWarnings();
  core.mockClear();
  vi.stubGlobal('__DEV__', true);
  vi.spyOn(console, 'warn').mockImplementation(() => {});
});

afterEach(() => {
  view?.unmount();
  view = undefined;
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
});

describe('Downstream sources', () => {
  it('accepts string children, including empty text, and the markdown prop', () => {
    view = mount(<Downstream>{'# title'}</Downstream>);
    expect(lastProps().markdown).toBe('# title');
    expect(lastProps()).not.toHaveProperty('children');
    view.rerender(<Downstream>{''}</Downstream>);
    expect(lastProps().markdown).toBe('');
    view.rerender(<Downstream markdown="legacy source" />);
    expect(lastProps().markdown).toBe('legacy source');
    expect(console.warn).not.toHaveBeenCalled();
  });

  it('passes an external stream through without treating it as a string', () => {
    const stream = {} as MarkdownStreamController;
    view = mount(<Downstream stream={stream} preview />);
    expect(lastProps().stream).toBe(stream);
    expect(lastProps().preview).toBe(true);
    expect(lastProps()).not.toHaveProperty('markdown');
  });

  it('rejects missing, multiple and non-string sources', () => {
    // @ts-expect-error A source is required.
    expect(() => renderToStaticMarkup(<Downstream />)).toThrow(/exactly one/);
    // @ts-expect-error A view cannot have two sources.
    const duplicate = <Downstream markdown="a">b</Downstream>;
    expect(() => renderToStaticMarkup(duplicate)).toThrow(/exactly one/);
    // @ts-expect-error Children must be a string.
    expect(() => renderToStaticMarkup(<Downstream>{12}</Downstream>)).toThrow(
      /string children/
    );
  });

  it('the deprecated component has the same behavior and warns once', () => {
    view = mount(<DownstreamMarkdown markdown="text" />);
    expect(lastProps().markdown).toBe('text');
    view.rerender(<DownstreamMarkdown markdown="next" />);
    expect(lastProps().markdown).toBe('next');
    expect(console.warn).toHaveBeenCalledTimes(1);
    expect(console.warn).toHaveBeenCalledWith(
      expect.stringContaining('DownstreamMarkdown is deprecated')
    );
  });
});

const legacyCases: ReadonlyArray<{
  name: string;
  old: DownstreamOptions;
  modern: DownstreamOptions;
}> = [
  {
    name: 'codeBlockWrap',
    old: { codeBlockWrap: false },
    modern: { codeBlock: { wrap: false } },
  },
  {
    name: 'lineNumbers',
    old: { lineNumbers: false },
    modern: { codeBlock: { lineNumbers: false } },
  },
  {
    name: 'codeBlockMaxHeight',
    old: { codeBlockMaxHeight: 0 },
    modern: { codeBlock: { maxHeight: 0 } },
  },
  {
    name: 'tableMaxHeight',
    old: { tableMaxHeight: 250 },
    modern: { table: { maxHeight: 250 } },
  },
  {
    name: 'onTableFullscreen',
    old: { onTableFullscreen: Press },
    modern: { table: { onFullscreen: Press } },
  },
  {
    name: 'renderTableFullscreen',
    old: { renderTableFullscreen: Fullscreen },
    modern: { table: { renderFullscreen: Fullscreen } },
  },
  {
    name: 'allowedImagePrefixes',
    old: { allowedImagePrefixes: ['https://images.example/'] },
    modern: { images: { allowedPrefixes: ['https://images.example/'] } },
  },
  {
    name: 'onImagePress',
    old: { onImagePress: Press },
    modern: { images: { onPress: Press } },
  },
  {
    name: 'linkSafety',
    old: { linkSafety: false },
    modern: { links: { safety: false } },
  },
  {
    name: 'onLinkPress',
    old: { onLinkPress: Press },
    modern: { links: { onPress: Press } },
  },
  {
    name: 'onLinkLongPress',
    old: { onLinkLongPress: Press },
    modern: { links: { onLongPress: Press } },
  },
  {
    name: 'linkPreview',
    old: { linkPreview: false },
    modern: { links: { preview: false } },
  },
  {
    name: 'disableAutolinkProtocols',
    old: { disableAutolinkProtocols: ['mailto'] },
    modern: { links: { disableAutolinkProtocols: ['mailto'] } },
  },
  {
    name: 'plugins.renderers',
    old: { plugins: { renderers: [{ language: 'chart', component: Fence }] } },
    modern: { components: { fences: { chart: Fence } } },
  },
  {
    name: 'plugins.tags',
    old: { plugins: { tags: [{ name: 'person', component: Tag }] } },
    modern: { components: { tags: { person: Tag } } },
  },
  {
    name: 'plugins.fallbackComponent',
    old: { plugins: { fallbackComponent: Fallback } },
    modern: { components: { fallback: Fallback } },
  },
];

describe('grouped options and compatibility aliases', () => {
  it.each(legacyCases)(
    '$name behaves like its replacement and warns once',
    ({ name, old, modern }) => {
      const expected = resolveDownstreamOptions(modern);
      expect(console.warn).not.toHaveBeenCalled();
      const actual = resolveDownstreamOptions(old);
      // The core defaults images on. An options object makes that default explicit.
      if (modern.images !== undefined)
        expect({ images: true, ...actual }).toEqual(expected);
      else if (modern.links !== undefined)
        expect({ links: {}, ...actual }).toEqual(expected);
      else expect(actual).toEqual(expected);
      expect(console.warn).toHaveBeenCalledTimes(1);
      expect(console.warn).toHaveBeenCalledWith(
        expect.stringContaining(`${name} is deprecated`)
      );
      resolveDownstreamOptions(old);
      expect(console.warn).toHaveBeenCalledTimes(1);
    }
  );

  it('modern subfields win over aliases within the same layer', () => {
    const current = resolveDownstreamOptions({
      codeBlockWrap: true,
      codeBlock: { wrap: false },
      tableMaxHeight: 300,
      table: { maxHeight: 0 },
      linkSafety: true,
      links: {
        safety: false,
        relative: true,
        defaultOrigin: 'https://example.com',
      },
      allowedImagePrefixes: ['https://old/'],
      images: { allowedPrefixes: ['https://new/'] },
    });
    expect(current).toMatchObject({
      codeBlockWrap: false,
      tableMaxHeight: 0,
      linkSafety: false,
      links: { relative: true, defaultOrigin: 'https://example.com' },
      allowedImagePrefixes: ['https://new/'],
    });
  });

  it('image booleans still disable and enable image loading', () => {
    expect(resolveDownstreamOptions({ images: false }).images).toBe(false);
    expect(resolveDownstreamOptions({ images: true }).images).toBe(true);
  });

  it('component maps override legacy registrations before reaching the slot resolver', () => {
    const current = resolveDownstreamOptions({
      components: {
        fences: { chart: Fence },
        tags: { person: Tag },
        fallback: Fallback,
      },
      plugins: {
        renderers: [{ language: 'chart', component: OtherFence }],
        tags: [{ name: 'person', component: OtherFence }],
        fallbackComponent: OtherFence,
      },
    });
    expect(current.plugins?.renderers?.[0]).toEqual({
      language: 'chart',
      component: Fence,
    });
    expect(current.plugins?.tags?.[0]).toEqual({
      name: 'person',
      component: Tag,
    });
    expect(current.plugins?.fallbackComponent).toBe(Fallback);
  });

  it('aliases are silent in production and warn if subsequently used in development', () => {
    vi.stubGlobal('__DEV__', false);
    resolveDownstreamOptions({ codeBlockWrap: true });
    renderToStaticMarkup(<DownstreamMarkdown markdown="x" />);
    expect(console.warn).not.toHaveBeenCalled();
    vi.stubGlobal('__DEV__', true);
    resolveDownstreamOptions({ codeBlockWrap: true });
    expect(console.warn).toHaveBeenCalledTimes(1);
  });
});

describe('provider defaults', () => {
  it('does not notify consumers when only the provider parent re-renders', () => {
    let renders = 0;
    function Consumer() {
      useDownstreamDefaults();
      renders++;
      return null;
    }
    const child = <Consumer />;
    view = mount(
      <DownstreamProvider selectable={false}>{child}</DownstreamProvider>
    );
    expect(renders).toBe(1);
    view.rerender(
      <DownstreamProvider selectable={false}>{child}</DownstreamProvider>
    );
    expect(renders).toBe(1);
    view.rerender(<DownstreamProvider selectable>{child}</DownstreamProvider>);
    expect(renders).toBe(2);
  });
  it('a provider renders the same props as a view configured directly', () => {
    const options: DownstreamOptions = {
      codeBlock: { wrap: true, lineNumbers: false },
      table: { maxHeight: 140 },
      images: false,
      links: { relative: true, safety: false },
      plugins: [],
      selectable: false,
      components: { fences: { chart: Fence } },
    };
    view = mount(<Downstream {...options}>source</Downstream>);
    const direct = lastProps();
    view.rerender(
      <DownstreamProvider {...options}>
        <Downstream>source</Downstream>
      </DownstreamProvider>
    );
    expect(lastProps()).toEqual(direct);
  });

  it('merges theme and markdownStyle arrays provider-first and preserves unspecified grouped subfields', () => {
    const inheritedTheme = { radius: 9 };
    const ownTheme = { colors: { foreground: '#123456' } };
    const inheritedStyle = { paragraph: { fontSize: 16 } };
    const ownStyle = { h1: { fontSize: 28 } };
    view = mount(
      <DownstreamProvider
        theme={[inheritedTheme]}
        markdownStyle={inheritedStyle}
        codeBlock={{ wrap: true, lineNumbers: false }}
        links={{ relative: true, defaultOrigin: 'https://example.com' }}
      >
        <Downstream
          theme={[ownTheme]}
          markdownStyle={[ownStyle]}
          codeBlock={{ lineNumbers: true }}
          links={{ defaultOrigin: 'https://local.example' }}
        >
          x
        </Downstream>
      </DownstreamProvider>
    );
    expect(lastProps()).toMatchObject({
      theme: [inheritedTheme, ownTheme],
      markdownStyle: [inheritedStyle, ownStyle],
      codeBlockWrap: true,
      lineNumbers: true,
      links: { relative: true, defaultOrigin: 'https://local.example' },
    });
  });

  it('local aliases beat provider groups, and local groups beat provider aliases', () => {
    view = mount(
      <DownstreamProvider codeBlock={{ wrap: true }} table={{ maxHeight: 300 }}>
        <Downstream codeBlockWrap={false} tableMaxHeight={0}>
          x
        </Downstream>
      </DownstreamProvider>
    );
    expect(lastProps()).toMatchObject({
      codeBlockWrap: false,
      tableMaxHeight: 0,
    });
    view.rerender(
      <DownstreamProvider codeBlockWrap={true} tableMaxHeight={300}>
        <Downstream codeBlock={{ wrap: false }} table={{ maxHeight: 0 }}>
          x
        </Downstream>
      </DownstreamProvider>
    );
    expect(lastProps()).toMatchObject({
      codeBlockWrap: false,
      tableMaxHeight: 0,
    });
  });

  it('nested providers inherit outer defaults and local components have precedence', () => {
    view = mount(
      <DownstreamProvider
        codeBlock={{ wrap: true }}
        components={{
          fences: { chart: Fence, other: OtherFence },
          tags: { person: Tag },
          fallback: Fallback,
        }}
      >
        <DownstreamProvider
          codeBlock={{ lineNumbers: false }}
          components={{ fences: { chart: OtherFence } }}
        >
          <Downstream components={{ fences: { chart: Fence } }}>x</Downstream>
        </DownstreamProvider>
      </DownstreamProvider>
    );
    expect(lastProps()).toMatchObject({
      codeBlockWrap: true,
      lineNumbers: false,
    });
    const expected = mergeDownstreamOptions(
      resolveDownstreamOptions({
        components: {
          fences: { chart: Fence, other: OtherFence },
          tags: { person: Tag },
          fallback: Fallback,
        },
      }),
      resolveDownstreamOptions({
        components: { fences: { chart: OtherFence } },
      })
    );
    expect(expected.plugins?.renderers?.[0]).toEqual({
      language: 'chart',
      component: OtherFence,
    });
    const plugins = lastProps().plugins as NonNullable<
      ReturnType<typeof resolveDownstreamOptions>['plugins']
    >;
    expect(plugins.renderers?.[0]).toEqual({
      language: 'chart',
      component: Fence,
    });
    expect(plugins.renderers).toContainEqual({
      language: 'other',
      component: OtherFence,
    });
    expect(plugins.tags).toContainEqual({ name: 'person', component: Tag });
    expect(plugins.fallbackComponent).toBe(Fallback);
  });
});

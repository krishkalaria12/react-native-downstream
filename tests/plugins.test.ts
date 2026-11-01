import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { math } from '../math';
import { resolvePlugins, type DownstreamPlugin } from '../src/plugins';
import { resolveRenderers } from '../src/fenceRenderers';
import { resolveTagRenderers } from '../src/tagRenderers';
import { resolveTheme } from '../src/theme';
import { themeSlot } from '../src/themeSlots';

const FirstFence = () => null;
const LastFence = () => null;
const FirstTag = () => null;
const LastTag = () => null;

beforeEach(() => vi.stubGlobal('__DEV__', true));
afterEach(() => vi.unstubAllGlobals());

describe('resolvePlugins', () => {
  it('enables no features without a plugin', () => {
    expect(resolvePlugins([])).toEqual({
      names: [],
      native: { code: false, math: false },
      options: {},
    });
  });

  it('keeps array order and merges parse options with later entries winning', () => {
    const plugins: readonly DownstreamPlugin[] = [
      {
        name: 'first',
        parse: {
          inlineHtml: true,
          syntax: { superscript: true, subscript: true },
          links: { relative: true },
          allowedTags: { user: ['id'] },
          cjk: false,
        },
        native: { code: true },
      },
      {
        name: 'second',
        parse: {
          inlineHtml: false,
          syntax: { superscript: false, highlight: true },
          links: { defaultOrigin: 'https://example.com' },
          allowedTags: { user: true, chart: true },
          cjk: undefined,
        },
        native: { math: true },
      },
    ];
    const result = resolvePlugins(plugins);
    expect(result.names).toEqual(['first', 'second']);
    expect(result.native).toEqual({ code: true, math: true });
    expect(result.options).toMatchObject({
      inlineHtml: false,
      syntax: { superscript: false, subscript: true, highlight: true },
      links: { relative: true, defaultOrigin: 'https://example.com' },
      allowedTags: { user: true, chart: true },
      cjk: false,
    });
    expect(plugins[0]?.parse?.syntax?.superscript).toBe(true);
  });

  it('places later fence and tag registrations before earlier matches', () => {
    const result = resolvePlugins([
      {
        name: 'first',
        fences: { chart: FirstFence },
        tags: { user: FirstTag },
      },
      { name: 'last', fences: { CHART: LastFence }, tags: { USER: LastTag } },
    ]);
    expect(
      resolveRenderers(result.options.plugins).components.get('chart')
    ).toBe(LastFence);
    expect(
      resolveTagRenderers(result.options.plugins).components.get('user')
    ).toBe(LastTag);
  });

  it('composes styles in plugin order, including conditional style entries', () => {
    const result = resolvePlugins([
      {
        name: 'first',
        style: {
          theme: { radius: 4 },
          markdownStyle: { paragraph: { fontSize: 16 } },
        },
      },
      {
        name: 'last',
        style: {
          theme: [false, { radius: 12 }],
          markdownStyle: [{ paragraph: { fontSize: 20 } }, null],
        },
      },
    ]);
    const theme = resolveTheme(
      result.options.theme,
      result.options.markdownStyle
    );
    const values = new Map(
      theme.slots.map((slot, index) => [slot, theme.values[index]])
    );
    expect(values.get(themeSlot('radius')!.id)).toBe(12);
    expect(values.get(themeSlot('paragraph.fontSize')!.id)).toBe(20);
  });

  it('allows later entries to disable a native capability', () => {
    const result = resolvePlugins([
      { name: 'code', native: { code: true } },
      { name: 'plain', native: { code: false } },
    ]);
    expect(result.native.code).toBe(false);
  });

  it('rejects duplicate names in development', () => {
    expect(() => resolvePlugins([{ name: 'same' }, { name: 'same' }])).toThrow(
      /Duplicate.*same/
    );
  });

  it('keeps the first duplicate in production without applying its later defaults', () => {
    vi.stubGlobal('__DEV__', false);
    const result = resolvePlugins([
      { name: 'same', native: { code: true }, parse: { cjk: true } },
      { name: 'same', native: { code: false }, parse: { cjk: false } },
    ]);
    expect(result.names).toEqual(['same']);
    expect(result.native.code).toBe(true);
    expect(result.options.cjk).toBe(true);
  });

  it('rejects unnamed plugins and invalid native capability values', () => {
    for (const name of ['', '   ']) {
      expect(() => resolvePlugins([{ name }])).toThrow(/nonempty name/);
    }
    expect(() =>
      resolvePlugins([
        // @ts-expect-error A plugin must have a string name.
        { name: 12 },
      ])
    ).toThrow(/nonempty name/);
    expect(() =>
      resolvePlugins([
        // @ts-expect-error Native capabilities must be booleans.
        { name: 'invalid', native: { code: 'yes' } },
      ])
    ).toThrow(/native.code.*boolean/);
  });
});

describe('math factory', () => {
  it('enables math explicitly and preserves delimiter options without importing core at runtime', () => {
    expect(math()).toEqual({
      name: 'math',
      native: { math: true },
      parse: { math: {} },
    });
    const options = { singleDollarTextMath: true, brackets: false };
    const plugin = math(options);
    expect(plugin).toEqual({
      name: 'math',
      native: { math: true },
      parse: { math: options },
    });
    options.brackets = true;
    expect(plugin.parse?.math).toEqual({
      singleDollarTextMath: true,
      brackets: false,
    });
    expect(resolvePlugins([plugin]).native.math).toBe(true);
  });

  it('rejects non-object options', () => {
    // @ts-expect-error The factory accepts delimiter options, not a boolean.
    expect(() => math(true)).toThrow(/must be an object/);
  });
});

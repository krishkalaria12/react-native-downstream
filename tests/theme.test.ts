/**
 * The `theme` and `markdownStyle` props, resolved to the three arrays native
 * reads, and the tokens and elements that produce them.
 *
 * Resolution is pure and lives in `src/theme.ts`, so most of this is a table of
 * the cases the contract names: the precedence order, the token fan-out, every
 * color form `processColor` accepts, the composition rules, and the warnings a
 * rejected value produces. The view tests at the bottom drive the real component
 * with a stand-in for the native component, so they prove an inline literal
 * equal by value sends nothing rather than proving the arrays by eye.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { processColor } from 'react-native';

import { presets } from '../src/presets';
import { resetThemeWarnings, resolveTheme } from '../src/theme';
import { THEME_SLOTS, themeSlot } from '../src/themeSlots';
import type { MarkdownStyle, MarkdownTheme } from '../src/types';

vi.mock('react-native', async (importOriginal) => {
  const actual = await importOriginal<typeof import('react-native')>();
  return { ...actual, processColor: vi.fn(actual.processColor) };
});

/**
 * A style written for another renderer, handed to this one as it stands.
 *
 * The tests that use this are about the contract for a copied value: it
 * resolves and reports rather than throwing. They cannot be written as typed
 * literals, because the keys this renderer does not draw are exactly what they
 * are testing, and TypeScript refuses a literal it knows to be wrong.
 */
function fromPeer(value: unknown): MarkdownStyle {
  return value as MarkdownStyle;
}

/** The slot id for a name, so a test reads as the value it expects. */
function slot(name: string): number {
  const found = themeSlot(name);
  if (found === undefined) throw new Error(`no slot named ${name}`);
  return found.id;
}

/** The value at a slot, or undefined when the theme does not set it. */
function valueOf(
  resolved: ReturnType<typeof resolveTheme>,
  name: string
): number | undefined {
  const id = slot(name);
  const index = resolved.slots.indexOf(id);
  return index < 0 ? undefined : resolved.values[index];
}

/** Whether the theme sets the slot at all. A name with no slot is false. */
function sets(
  resolved: ReturnType<typeof resolveTheme>,
  name: string
): boolean {
  const found = themeSlot(name);
  return found !== undefined && resolved.slots.includes(found.id);
}

beforeEach(() => {
  resetThemeWarnings();
  vi.mocked(processColor).mockClear();
  vi.spyOn(console, 'warn').mockImplementation(() => {});
});

afterEach(() => {
  vi.restoreAllMocks();
  resetThemeWarnings();
});

describe('resolveTheme', () => {
  it('converts each palette color once before writing all its slots', () => {
    const resolved = resolveTheme({ colors: { foreground: '#010203' } });
    expect(processColor).toHaveBeenCalledExactlyOnceWith('#010203');
    expect(resolved.slots).toHaveLength(20);
    expect(resolved.values).toEqual(Array(20).fill(0xff010203));
  });

  it('converts each appearance once while preserving later half overrides', () => {
    const resolved = resolveTheme([
      { colors: { foreground: { light: '#111111', dark: '#222222' } } },
      { colors: { foreground: { dark: '#333333' } } },
    ]);
    expect(processColor).toHaveBeenCalledTimes(3);
    expect(resolved.slots).toHaveLength(20);
    expect(resolved.values).toEqual(
      Array.from({ length: 20 }, (_, index) =>
        index % 2 === 0 ? 0xff111111 : 0xff333333
      )
    );
  });

  it('sends three empty arrays for no theme at all', () => {
    // "No theme means no change" is structural: the native side sees the same
    // empty list an app would send, and its static default theme stands.
    expect(resolveTheme()).toEqual({ slots: [], values: [], fonts: [] });
    expect(resolveTheme(undefined, undefined)).toEqual({
      slots: [],
      values: [],
      fonts: [],
    });
    expect(resolveTheme({}, {})).toEqual({ slots: [], values: [], fonts: [] });
  });

  it('sorts slots ascending and never repeats one', () => {
    const resolved = resolveTheme(
      { radius: 12 },
      { paragraph: { fontSize: 20 }, codeBlock: { padding: 4 } }
    );
    expect([...resolved.slots].sort((a, b) => a - b)).toEqual(resolved.slots);
    expect(new Set(resolved.slots).size).toBe(resolved.slots.length);
    // One value per slot, always.
    expect(resolved.values.length).toBe(resolved.slots.length);
  });

  it('sends values in slot order, so a platform reads them positionally', () => {
    const resolved = resolveTheme(
      { radius: 12 },
      { paragraph: { fontSize: 20, fontWeight: 600 } }
    );
    for (let index = 1; index < resolved.slots.length; index++) {
      expect(resolved.slots[index]).toBeGreaterThan(
        resolved.slots[index - 1] ?? 0
      );
    }
    expect(valueOf(resolved, 'radius')).toBe(12);
    expect(valueOf(resolved, 'paragraph.fontSize')).toBe(20);
    expect(valueOf(resolved, 'paragraph.fontWeight')).toBe(600);
  });
});

describe('precedence', () => {
  it('prefers an element style over a token', () => {
    const resolved = resolveTheme(
      { colors: { foreground: '#111111' } },
      { paragraph: { color: '#222222' } }
    );
    expect(valueOf(resolved, 'paragraph.color')).toBe(0xff222222);
  });

  it('prefers a token over the default', () => {
    const resolved = resolveTheme({ colors: { foreground: '#111111' } });
    expect(valueOf(resolved, 'paragraph.color')).toBe(0xff111111);
    expect(sets(resolveTheme(), 'paragraph.color')).toBe(false);
  });

  it('leaves the rest of a token in place when an element overrides one of its slots', () => {
    // A token fans out over many slots and an element names one, which is why
    // the resolver runs tokens first rather than merging the two layers.
    const resolved = resolveTheme(
      { colors: { foreground: '#111111' } },
      { h1: { color: '#222222' } }
    );
    expect(valueOf(resolved, 'heading.1.color')).toBe(0xff222222);
    expect(valueOf(resolved, 'paragraph.color')).toBe(0xff111111);
    expect(valueOf(resolved, 'table.color')).toBe(0xff111111);
  });

  it('lets a later array entry win over an earlier one', () => {
    const resolved = resolveTheme([
      { colors: { primary: '#111111' } },
      { colors: { primary: '#222222' } },
    ]);
    expect(valueOf(resolved, 'link.color')).toBe(0xff222222);
  });
});

describe('token fan-out', () => {
  it('foreground reaches body, headings one to five, list and table text', () => {
    const resolved = resolveTheme({ colors: { foreground: '#010203' } });
    for (const name of [
      'paragraph.color',
      'heading.1.color',
      'heading.5.color',
      'list.color',
      'table.color',
      'code.color',
      'codeBlock.color',
    ]) {
      expect(valueOf(resolved, name), name).toBe(0xff010203);
    }
    // An h6 is already de-emphasized today, so the token does not reach it.
    expect(sets(resolved, 'heading.6.color')).toBe(false);
  });

  it('mutedForeground reaches quotes, an h6, markers, alt text and the chrome', () => {
    const resolved = resolveTheme({ colors: { mutedForeground: '#040506' } });
    for (const name of [
      'blockquote.color',
      'heading.6.color',
      'list.markerColor',
      'list.bulletColor',
      'imageAlt.color',
      'codeBlock.header.color',
      'codeBlock.header.buttonColor',
      'table.toolbar.buttonColor',
    ]) {
      expect(valueOf(resolved, name), name).toBe(0xff040506);
    }
  });

  it('primary reaches links, footnote references and a checked task', () => {
    const resolved = resolveTheme({ colors: { primary: '#070809' } });
    expect(valueOf(resolved, 'link.color')).toBe(0xff070809);
    expect(valueOf(resolved, 'footnote.color')).toBe(0xff070809);
    expect(valueOf(resolved, 'taskList.checkedColor')).toBe(0xff070809);
  });

  it('muted reaches the code card, inline code and the table header', () => {
    const resolved = resolveTheme({ colors: { muted: '#0a0b0c' } });
    expect(valueOf(resolved, 'codeBlock.backgroundColor')).toBe(0xff0a0b0c);
    expect(valueOf(resolved, 'code.backgroundColor')).toBe(0xff0a0b0c);
    expect(valueOf(resolved, 'table.headerBackgroundColor')).toBe(0xff0a0b0c);
  });

  it('border reaches the grid, rules, quote bars and the code card', () => {
    const resolved = resolveTheme({ colors: { border: '#0d0e0f' } });
    for (const name of [
      'table.borderColor',
      'thematicBreak.color',
      'blockquote.borderColor',
      'codeBlock.borderColor',
    ]) {
      expect(valueOf(resolved, name), name).toBe(0xff0d0e0f);
    }
  });

  it('selection reaches the highlight only', () => {
    const resolved = resolveTheme({ colors: { selection: '#101112' } });
    expect(valueOf(resolved, 'selection.color')).toBe(0xff101112);
    expect(sets(resolved, 'paragraph.color')).toBe(false);
  });

  it('radius and the font scale cap are metrics, not colors', () => {
    const resolved = resolveTheme({ radius: 14, maxFontSizeMultiplier: 1.3 });
    expect(valueOf(resolved, 'radius')).toBe(14);
    expect(valueOf(resolved, 'maxFontSizeMultiplier')).toBe(1.3);
  });

  it('fonts reach the elements that draw body, heading and code text', () => {
    const resolved = resolveTheme({
      fonts: { sans: 'Inter', heading: 'Sora', mono: 'JetBrainsMono-Regular' },
    });
    expect(resolved.fonts).toEqual(['Inter', 'Sora', 'JetBrainsMono-Regular']);
    // A font slot's value is an index into that list, not the family itself.
    expect(valueOf(resolved, 'paragraph.fontFamily')).toBe(0);
    expect(valueOf(resolved, 'table.fontFamily')).toBe(0);
    expect(valueOf(resolved, 'heading.1.fontFamily')).toBe(1);
    expect(valueOf(resolved, 'heading.6.fontFamily')).toBe(1);
    expect(valueOf(resolved, 'code.fontFamily')).toBe(2);
    expect(valueOf(resolved, 'codeBlock.fontFamily')).toBe(2);
  });

  it('syntax reaches every token class it names', () => {
    const resolved = resolveTheme({
      syntax: { keyword: '#aa0000', comment: '#00aa00' },
    });
    expect(valueOf(resolved, 'syntax.keyword')).toBe(0xffaa0000);
    expect(valueOf(resolved, 'syntax.comment')).toBe(0xff00aa00);
    expect(sets(resolved, 'syntax.string')).toBe(false);
  });
});

describe('colors', () => {
  it('accepts a hex string, a short hex, a named color and a number', () => {
    const resolved = resolveTheme({
      colors: {
        foreground: '#112233',
        primary: '#abc',
        muted: 'rebeccapurple',
        // A number is read as 0xrrggbbaa and processed into 0xaarrggbb, which is
        // what React Native's own processColor does with it.
        border: 0x445566ff,
      },
    });
    expect(valueOf(resolved, 'paragraph.color')).toBe(0xff112233);
    expect(valueOf(resolved, 'link.color')).toBe(0xffaabbcc);
    expect(valueOf(resolved, 'code.backgroundColor')).toBe(0xff663399);
    expect(valueOf(resolved, 'table.borderColor')).toBe(0xff445566);
  });

  it('sends both halves for a color that is the same in both appearances', () => {
    // A light id on its own means "only light mode", so a plain color names
    // both halves rather than relying on the engine to copy one.
    const resolved = resolveTheme({ colors: { primary: '#112233' } });
    const light = slot('link.color');
    expect(resetsLightOnly(resolved, light)).toBe(false);
    expect(resolved.values[resolved.slots.indexOf(light)]).toBe(0xff112233);
    expect(resolved.values[resolved.slots.indexOf(light + 1)]).toBe(0xff112233);
  });

  it('sends two slots for a light and dark pair', () => {
    const resolved = resolveTheme({
      colors: { foreground: { light: '#ffffff', dark: '#000000' } },
    });
    const light = slot('paragraph.color');
    // The dark half is always the id after the light one, which is what makes a
    // pair a positional thing rather than a second lookup.
    expect(themeSlot('paragraph.color')?.darkId).toBe(light + 1);
    const lightIndex = resolved.slots.indexOf(light);
    const darkIndex = resolved.slots.indexOf(light + 1);
    expect(lightIndex).toBeGreaterThanOrEqual(0);
    expect(darkIndex).toBe(lightIndex + 1);
    expect(resolved.values[lightIndex]).toBe(0xffffffff);
    expect(resolved.values[darkIndex]).toBe(0xff000000);
  });

  it('sends a pair whose halves match the way it sends one color', () => {
    const pair = resolveTheme({
      colors: { primary: { light: '#112233', dark: '#112233' } },
    });
    const plain = resolveTheme({ colors: { primary: '#112233' } });
    expect(pair).toEqual(plain);
  });

  it('sends only the light half of a light-only pair', () => {
    // The dark half is left to the default for dark mode, as the type promises,
    // rather than frozen to the light color.
    const resolved = resolveTheme({
      colors: { primary: { light: '#112233' } },
    });
    expect(resetsLightOnly(resolved, slot('link.color'))).toBe(true);
    expect(valueOf(resolved, 'link.color')).toBe(0xff112233);
  });

  it('sends only the dark half of a dark-only pair', () => {
    // Light mode keeps its default. Sending the dark color for both would draw
    // a dark theme's near-white text on a light background.
    const resolved = resolveTheme({
      colors: { foreground: { dark: '#e6e1e5' } },
    });
    const light = slot('paragraph.color');
    expect(resolved.slots).not.toContain(light);
    expect(resolved.values[resolved.slots.indexOf(light + 1)]).toBe(0xffe6e1e5);
  });

  it('keeps the other half when a later value names one', () => {
    // Merged per leaf: a dark-only override on top of a preset changes dark mode
    // and leaves the preset's light color where it was.
    const resolved = resolveTheme([
      { colors: { foreground: { light: '#111111', dark: '#222222' } } },
      { colors: { foreground: { dark: '#333333' } } },
    ]);
    const light = slot('paragraph.color');
    expect(resolved.values[resolved.slots.indexOf(light)]).toBe(0xff111111);
    expect(resolved.values[resolved.slots.indexOf(light + 1)]).toBe(0xff333333);
  });

  it('warns once about a value it cannot draw, and keeps the default', () => {
    const resolved = resolveTheme({ colors: { primary: 'not-a-color' } });
    expect(sets(resolved, 'link.color')).toBe(false);
    expect(console.warn).toHaveBeenCalledTimes(1);
    // A second render with the same mistake does not repeat it, or a long chat
    // would bury the first warning under a thousand copies of itself.
    resolveTheme({ colors: { primary: 'not-a-color' } });
    expect(console.warn).toHaveBeenCalledTimes(1);
  });

  it('names the token in the warning, so a mistake is findable', () => {
    resolveTheme({ colors: { primary: 'nonsense' } });
    expect(console.warn).toHaveBeenCalledWith(
      expect.stringContaining('theme.colors.primary')
    );
  });

  it('warns about a PlatformColor rather than drawing it in one appearance', () => {
    // Android resolves one to a fixed integer when props are parsed, so it could
    // not follow a configuration change the way an unset default does.
    const platformColor = { semantic: ['label'] };
    const resolved = resolveTheme({
      colors: { primary: platformColor as never },
    });
    expect(sets(resolved, 'link.color')).toBe(false);
    expect(console.warn).toHaveBeenCalledWith(
      expect.stringContaining('PlatformColor')
    );
  });

  it('accepts a processed DynamicColorIOS pair as a pair', () => {
    // What processColor produces for DynamicColorIOS, which is what a value that
    // went through the platform's own pipeline arrives as.
    const dynamic = {
      dynamic: {
        light: 0xffffffff,
        dark: 0xff000000,
        highContrastLight: 0xffffffff,
        highContrastDark: 0xff000000,
      },
    };
    const resolved = resolveTheme({ colors: { foreground: dynamic as never } });
    const lightIndex = resolved.slots.indexOf(slot('paragraph.color'));
    expect(resolved.values[lightIndex]).toBe(0xffffffff);
    expect(resetsLightOnly(resolved, slot('paragraph.color'))).toBe(false);
  });
});

/** True when the slot at `id` was sent alone, with no dark half after it. */
function resetsLightOnly(
  resolved: ReturnType<typeof resolveTheme>,
  id: number
): boolean {
  const index = resolved.slots.indexOf(id);
  return index >= 0 && resolved.slots[index + 1] !== id + 1;
}

describe('markdownStyle', () => {
  it('takes a light and dark pair on an element color, over the token', () => {
    // An element style wins over a token, and a pair is how an element color
    // follows the appearance, so dropping it would leave the token showing.
    const resolved = resolveTheme(
      { colors: { muted: '#1e293b' } },
      {
        codeBlock: { backgroundColor: { light: '#ffffff', dark: '#020817' } },
      }
    );
    const light = slot('codeBlock.backgroundColor');
    expect(resolved.values[resolved.slots.indexOf(light)]).toBe(0xffffffff);
    expect(resolved.values[resolved.slots.indexOf(light + 1)]).toBe(0xff020817);
  });

  it('maps every element key this renderer draws onto its slots', () => {
    const resolved = resolveTheme(
      {},
      {
        paragraph: {
          fontSize: 18,
          marginTop: 4,
          lineHeight: 24,
          fontStyle: 'italic',
        },
        h3: { fontSize: 21, fontWeight: '500' },
        blockquote: { borderWidth: 5, gapWidth: 20, borderColor: '#111111' },
        list: {
          marginLeft: 10,
          gapWidth: 2,
          itemSpacing: 8,
          markerColor: '#222222',
        },
        taskList: { checkedColor: '#333333', borderColor: '#444444' },
        code: { fontSize: 12, backgroundColor: '#555555' },
        codeBlock: {
          padding: 2,
          borderRadius: 4,
          borderWidth: 1,
          backgroundColor: '#666666',
          header: { color: '#777777', buttonColor: '#888888' },
        },
        link: { color: '#999999', underline: false },
        strong: { fontWeight: 'normal' },
        em: { fontStyle: 'normal' },
        strikethrough: { color: '#aaaaaa' },
        thematicBreak: { height: 2, marginTop: 20 },
        table: {
          cellPaddingHorizontal: 6,
          minColumnWidth: 40,
          toolbar: { buttonColor: '#bbbbbb' },
        },
        admonition: { note: '#cccccc' },
        footnote: { color: '#dddddd' },
        imageAlt: { color: '#eeeeee' },
      }
    );
    expect(valueOf(resolved, 'paragraph.fontSize')).toBe(18);
    expect(valueOf(resolved, 'paragraph.marginTop')).toBe(4);
    expect(valueOf(resolved, 'paragraph.lineHeight')).toBe(24);
    expect(valueOf(resolved, 'paragraph.fontStyle')).toBe(1);
    expect(valueOf(resolved, 'heading.3.fontSize')).toBe(21);
    expect(valueOf(resolved, 'heading.3.fontWeight')).toBe(500);
    expect(valueOf(resolved, 'blockquote.borderWidth')).toBe(5);
    expect(valueOf(resolved, 'blockquote.gapWidth')).toBe(20);
    expect(valueOf(resolved, 'blockquote.borderColor')).toBe(0xff111111);
    expect(valueOf(resolved, 'list.marginLeft')).toBe(10);
    expect(valueOf(resolved, 'list.gapWidth')).toBe(2);
    expect(valueOf(resolved, 'list.itemSpacing')).toBe(8);
    expect(valueOf(resolved, 'list.markerColor')).toBe(0xff222222);
    expect(valueOf(resolved, 'taskList.checkedColor')).toBe(0xff333333);
    expect(valueOf(resolved, 'code.fontSize')).toBe(12);
    expect(valueOf(resolved, 'code.backgroundColor')).toBe(0xff555555);
    expect(valueOf(resolved, 'codeBlock.padding')).toBe(2);
    expect(valueOf(resolved, 'codeBlock.borderRadius')).toBe(4);
    expect(valueOf(resolved, 'codeBlock.borderWidth')).toBe(1);
    expect(valueOf(resolved, 'codeBlock.backgroundColor')).toBe(0xff666666);
    expect(valueOf(resolved, 'codeBlock.header.color')).toBe(0xff777777);
    expect(valueOf(resolved, 'codeBlock.header.buttonColor')).toBe(0xff888888);
    expect(valueOf(resolved, 'link.color')).toBe(0xff999999);
    expect(valueOf(resolved, 'link.underline')).toBe(0);
    expect(valueOf(resolved, 'strong.fontWeight')).toBe(400);
    expect(valueOf(resolved, 'em.fontStyle')).toBe(0);
    expect(valueOf(resolved, 'strikethrough.color')).toBe(0xffaaaaaa);
    expect(valueOf(resolved, 'thematicBreak.height')).toBe(2);
    expect(valueOf(resolved, 'thematicBreak.marginTop')).toBe(20);
    expect(valueOf(resolved, 'table.cellPaddingHorizontal')).toBe(6);
    expect(valueOf(resolved, 'table.minColumnWidth')).toBe(40);
    expect(valueOf(resolved, 'table.toolbar.buttonColor')).toBe(0xffbbbbbb);
    expect(valueOf(resolved, 'admonition.note')).toBe(0xffcccccc);
    expect(valueOf(resolved, 'footnote.color')).toBe(0xffdddddd);
    expect(valueOf(resolved, 'imageAlt.color')).toBe(0xffeeeeee);
  });

  it('maps every admonition alert type this renderer draws', () => {
    const resolved = resolveTheme(
      {},
      {
        admonition: {
          note: '#111111',
          tip: '#222222',
          important: '#333333',
          warning: '#444444',
          caution: '#555555',
        },
      }
    );
    for (const [type, color] of [
      ['note', 0xff111111],
      ['tip', 0xff222222],
      ['important', 0xff333333],
      ['warning', 0xff444444],
      ['caution', 0xff555555],
    ] as const) {
      expect(valueOf(resolved, `admonition.${type}`), type).toBe(color);
    }
  });

  it('accepts enriched-markdown syntax keys and maps them onto engine classes', () => {
    const resolved = resolveTheme(
      {},
      {
        codeBlock: {
          syntaxColors: {
            keyword: '#aa0000',
            punctuation: '#00bb00',
            variable: '#0000cc',
            embedded: '#00dd00',
          },
        },
      }
    );
    expect(valueOf(resolved, 'syntax.keyword')).toBe(0xffaa0000);
    // enriched's `punctuation` is the engine's `operator`, which is what GitHub's
    // themes color for punctuation anyway.
    expect(valueOf(resolved, 'syntax.operator')).toBe(0xff00bb00);
    // `variable` and `embedded` are both `plain`, by enriched's own rule that they
    // inherit the base color, so the later of the two is the one that lands.
    expect(valueOf(resolved, 'syntax.plain')).toBe(0xff00dd00);
  });

  it('reports a key it does not draw, once, rather than ignoring it silently', () => {
    // A whole enriched-markdown style for a feature this renderer has not built
    // is one thing to say, not one per property. `spoiler` is the example: no
    // renderer draws it.
    resolveTheme({}, fromPeer({ spoiler: { color: '#6B7280' } }));
    expect(console.warn).toHaveBeenCalledTimes(1);
    expect(console.warn).toHaveBeenCalledWith(
      expect.stringContaining('markdownStyle.spoiler')
    );
    resolveTheme({}, fromPeer({ spoiler: { color: '#6B7280' } }));
    expect(console.warn).toHaveBeenCalledTimes(1);
  });

  it('draws the math and image keys, and reports the rest of their peer keys', () => {
    // `image` and `math` were the two keys this renderer did not draw. It draws
    // both now, so enriched-markdown's values for them land — and the three
    // properties of `image` it still does not draw are what gets reported, as
    // the plan's own rule for keys a renderer ignores.
    const resolved = resolveTheme(
      {},
      fromPeer({
        image: { height: 40, aspectRatio: 1.5, resizeMode: 'cover' },
        math: { fontSize: 20, color: '#1F2937', textAlign: 'center' },
      })
    );
    expect(valueOf(resolved, 'math.fontSize')).toBe(20);
    expect(valueOf(resolved, 'math.color')).toBe(0xff1f2937);
    // `center` is the public name; one is what the slot holds.
    expect(valueOf(resolved, 'math.textAlign')).toBe(1);
    // And an alignment the renderer does not have is reported rather than
    // guessed at, because a formula aligned nowhere is a silent failure.
    resolveTheme({}, { math: { textAlign: 'justify' as never } });
    expect(console.warn).toHaveBeenCalledWith(
      expect.stringContaining('markdownStyle.math.textAlign')
    );
  });

  it('accepts a value written for enriched-markdown as it is', () => {
    // The shape from enriched-markdown's own docs, pasted unchanged. It resolves
    // without throwing, honors the keys this renderer draws, and reports the
    // rest, which is what the contract promises for a copied value.
    const enriched: MarkdownStyle = fromPeer({
      paragraph: { fontSize: 16, color: '#333', lineHeight: 24 },
      h1: {
        fontSize: 32,
        fontWeight: 'bold',
        marginBottom: 16,
        textAlign: 'center',
      },
      h2: { fontSize: 24, fontWeight: '600', marginBottom: 12 },
      strong: { fontFamily: 'Montserrat-Bold', color: '#000' },
      em: { fontFamily: 'Montserrat-Italic', fontStyle: 'italic' },
      strikethrough: { color: '#999' },
      link: { color: '#007AFF', underline: true },
      code: {
        fontFamily: 'CutiveMono-Regular',
        fontSize: 16,
        color: '#E91E63',
      },
      codeBlock: {
        fontSize: 14,
        padding: 16,
        borderRadius: 8,
        marginBottom: 16,
      },
      blockquote: {
        borderColor: '#007AFF',
        borderWidth: 3,
        backgroundColor: '#F0F8FF',
        borderRadius: 8,
        padding: 12,
        marginBottom: 12,
      },
      list: {
        fontSize: 16,
        bulletColor: '#007AFF',
        bulletSize: 6,
        markerColor: '#007AFF',
        gapWidth: 8,
        marginLeft: 20,
      },
      image: { borderRadius: 8, marginBottom: 12 },
      taskList: { checkedColor: '#2196F3', borderColor: '#9E9E9E' },
      math: { fontSize: 20, color: '#1F2937' },
      spoiler: { color: '#6B7280' },
    });
    const resolved = resolveTheme({}, enriched);
    expect(valueOf(resolved, 'paragraph.fontSize')).toBe(16);
    expect(valueOf(resolved, 'paragraph.color')).toBe(0xff333333);
    expect(valueOf(resolved, 'paragraph.lineHeight')).toBe(24);
    expect(valueOf(resolved, 'heading.1.fontSize')).toBe(32);
    expect(valueOf(resolved, 'heading.1.fontWeight')).toBe(700);
    // A property it does not draw is dropped without a warning of its own; the
    // key it belongs to is what gets reported.
    expect(sets(resolved, 'blockquote.backgroundColor')).toBe(false);
    expect(sets(resolved, 'blockquote.borderRadius')).toBe(false);
    expect(sets(resolved, 'list.bulletSize')).toBe(false);
    expect(valueOf(resolved, 'list.bulletColor')).toBe(0xff007aff);
    // The fixture names a family for strong rather than a weight, so the weight
    // slot stays at its default and the bold trait is still added on top.
    expect(sets(resolved, 'strong.fontWeight')).toBe(false);
    expect(resolved.fonts).toContain('Montserrat-Bold');
    expect(resolved.fonts).toContain('CutiveMono-Regular');
    // `image` and `math` are drawn, so nothing is reported for them; only
    // `spoiler` is a key this renderer has no renderer for.
    expect(valueOf(resolved, 'image.borderRadius')).toBe(8);
    expect(valueOf(resolved, 'image.marginBottom')).toBe(12);
    expect(valueOf(resolved, 'math.fontSize')).toBe(20);
    expect(valueOf(resolved, 'math.color')).toBe(0xff1f2937);
    expect(console.warn).not.toHaveBeenCalledWith(
      expect.stringContaining('markdownStyle.image')
    );
    expect(console.warn).not.toHaveBeenCalledWith(
      expect.stringContaining('markdownStyle.math')
    );
    expect(console.warn).toHaveBeenCalledWith(
      expect.stringContaining('markdownStyle.spoiler')
    );
  });

  it('accepts a font weight written as a string, a number, bold or normal', () => {
    const resolved = resolveTheme(
      {},
      {
        h1: { fontWeight: 'bold' },
        h2: { fontWeight: 250 },
        h3: { fontWeight: '300' },
        h4: { fontWeight: 'normal' },
      }
    );
    expect(valueOf(resolved, 'heading.1.fontWeight')).toBe(700);
    expect(valueOf(resolved, 'heading.2.fontWeight')).toBe(250);
    expect(valueOf(resolved, 'heading.3.fontWeight')).toBe(300);
    expect(valueOf(resolved, 'heading.4.fontWeight')).toBe(400);
  });

  it('warns about a font weight it cannot use, and falls back to regular', () => {
    const resolved = resolveTheme(
      {},
      { h1: { fontWeight: 'heavy-ish' as never } }
    );
    expect(console.warn).toHaveBeenCalledWith(
      expect.stringContaining('fontWeight')
    );
    expect(valueOf(resolved, 'heading.1.fontWeight')).toBe(400);
  });

  it('treats a null or false entry in an array as absent', () => {
    const resolved = resolveTheme([null, { radius: 9 }, false, undefined]);
    expect(valueOf(resolved, 'radius')).toBe(9);
    expect(resolveTheme([null, false, undefined])).toEqual({
      slots: [],
      values: [],
      fonts: [],
    });
  });
});

describe('composition', () => {
  it('resolves an array the same as the equivalent merged object', () => {
    // The point of accepting an array: no merge helper at the call site.
    const merged: MarkdownTheme = {
      colors: { foreground: '#111111', primary: '#222222' },
    };
    const composed = resolveTheme([
      { colors: { foreground: '#111111' } },
      { colors: { primary: '#222222' } },
    ]);
    expect(composed).toEqual(resolveTheme(merged));
  });

  it('keeps the properties a later entry does not name inside an element', () => {
    // `[{ h1: { fontSize: 22 } }, { h1: { color: x } }]` has to keep both, which
    // is what merging one level deeper means.
    const resolved = resolveTheme({}, [
      { h1: { fontSize: 22 } },
      { h1: { color: '#123456' } },
    ]);
    expect(valueOf(resolved, 'heading.1.fontSize')).toBe(22);
    expect(valueOf(resolved, 'heading.1.color')).toBe(0xff123456);
  });

  it('lets a later element entry win on one property and keep the rest', () => {
    const resolved = resolveTheme({}, [
      { codeBlock: { padding: 2, fontSize: 12 } },
      { codeBlock: { padding: 8 } },
    ]);
    expect(valueOf(resolved, 'codeBlock.padding')).toBe(8);
    expect(valueOf(resolved, 'codeBlock.fontSize')).toBe(12);
  });

  it('composes the two props independently', () => {
    const resolved = resolveTheme(
      [{ colors: { foreground: '#111111' } }, { radius: 10 }],
      [{ paragraph: { color: '#222222' } }, { h1: { fontSize: 30 } }]
    );
    expect(valueOf(resolved, 'paragraph.color')).toBe(0xff222222);
    expect(valueOf(resolved, 'table.color')).toBe(0xff111111);
    expect(valueOf(resolved, 'radius')).toBe(10);
    expect(valueOf(resolved, 'heading.1.fontSize')).toBe(30);
  });
});

describe('presets', () => {
  it('resolves both with no warning at all', () => {
    for (const [name, value] of Object.entries(presets)) {
      const resolved = resolveTheme(value.theme, value.markdownStyle);
      expect(resolved.slots.length, name).toBeGreaterThan(0);
      expect(console.warn, name).not.toHaveBeenCalled();
    }
  });

  it('draws the GitHub preset with concrete colors in both appearances', () => {
    const resolved = resolveTheme(presets.github.theme);
    const light = resolved.slots.indexOf(slot('paragraph.color'));
    expect(resolved.values[light]).toBe(0xff1f2328);
    expect(resolved.values[light + 1]).toBe(0xffe6edf3);
  });

  it('draws the Streamdown preset with the sizes its Tailwind classes describe', () => {
    const resolved = resolveTheme(
      presets.streamdown.theme,
      presets.streamdown.markdownStyle
    );
    expect(valueOf(resolved, 'heading.1.fontSize')).toBe(30);
    expect(valueOf(resolved, 'heading.6.fontSize')).toBe(14);
    expect(valueOf(resolved, 'heading.1.marginTop')).toBe(24);
    expect(valueOf(resolved, 'heading.1.marginBottom')).toBe(8);
    expect(valueOf(resolved, 'blockquote.borderWidth')).toBe(4);
    expect(valueOf(resolved, 'blockquote.gapWidth')).toBe(16);
    expect(valueOf(resolved, 'blockquote.fontStyle')).toBe(1);
    expect(valueOf(resolved, 'codeBlock.padding')).toBe(24);
    expect(valueOf(resolved, 'table.cellPaddingHorizontal')).toBe(16);
    expect(valueOf(resolved, 'table.cellPaddingVertical')).toBe(8);
    expect(valueOf(resolved, 'radius')).toBe(8);
  });

  it('is frozen, so spreading a preset cannot mutate the shared value', () => {
    expect(Object.isFrozen(presets)).toBe(true);
    expect(Object.isFrozen(presets.github)).toBe(true);
    expect(Object.isFrozen(presets.github.theme)).toBe(true);
  });

  it('starts from a preset and overrides one value without a merge helper', () => {
    const resolved = resolveTheme([presets.github.theme, { radius: 12 }]);
    expect(valueOf(resolved, 'radius')).toBe(12);
    // Everything the preset set is still there.
    expect(sets(resolved, 'paragraph.color')).toBe(true);
  });
});

describe('the slot table', () => {
  it('holds only ids this build can send', () => {
    for (const id of Object.values(THEME_SLOTS)) {
      expect(id.id).toBeGreaterThan(0);
      expect(id.id).toBeLessThan(Object.keys(THEME_SLOTS).length * 3);
    }
  });

  it('gives every color slot a dark half and every other slot none', () => {
    for (const [name, entry] of Object.entries(THEME_SLOTS)) {
      if (entry.kind === 'color') {
        expect(entry.darkId, name).toBe(entry.id + 1);
      } else {
        expect(entry.darkId, name).toBeUndefined();
      }
    }
  });

  it('marks no color as changing layout, so a palette change invalidates nothing', () => {
    for (const [name, entry] of Object.entries(THEME_SLOTS)) {
      if (entry.kind === 'color') expect(entry.layout, name).toBe(false);
    }
  });

  it('marks every metric, weight and font as changing layout', () => {
    for (const [name, entry] of Object.entries(THEME_SLOTS)) {
      if (entry.kind !== 'color' && entry.kind !== 'flag') {
        expect(entry.layout, name).toBe(true);
      }
    }
  });

  it('names every slot the resolver writes', () => {
    // A slot name the resolver spells that the table does not have would silently
    // do nothing, which is the one failure mode a generated table must not have.
    for (const name of [
      'radius',
      'maxFontSizeMultiplier',
      'selection.color',
      'paragraph.color',
      'heading.6.color',
      'blockquote.borderColor',
      'list.markerColor',
      'list.bulletColor',
      'list.markerFontSize',
      'list.markerFontWeight',
      'taskList.checkedColor',
      'code.backgroundColor',
      'codeBlock.header.color',
      'codeBlock.header.backgroundColor',
      'link.underline',
      'link.fontWeight',
      'strong.fontWeight',
      'em.fontStyle',
      'strikethrough.color',
      'thematicBreak.height',
      'table.headerTextColor',
      'table.rowOddBackgroundColor',
      'table.toolbar.buttonColor',
      'admonition.note',
      'admonition.caution',
      'footnote.color',
      'imageAlt.color',
      'syntax.plain',
      'syntax.heading',
    ]) {
      expect(themeSlot(name), name).toBeDefined();
    }
  });
});

describe('warnings', () => {
  it('says nothing for a value that is entirely well formed', () => {
    resolveTheme(
      {
        colors: { foreground: { light: '#fff', dark: '#000' } },
        fonts: { sans: 'Inter' },
      },
      {
        paragraph: { fontSize: 18 },
        codeBlock: { padding: 8, syntaxColors: { keyword: '#f00' } },
      }
    );
    expect(console.warn).not.toHaveBeenCalled();
  });

  it('reports a token this version does not have', () => {
    resolveTheme({ colors: { accent: '#123456' } } as never);
    expect(console.warn).toHaveBeenCalledWith(
      expect.stringContaining('theme.colors.accent')
    );
  });

  it('reports a token class this version does not have', () => {
    resolveTheme({ syntax: { comment2: '#123456' } } as never);
    expect(console.warn).toHaveBeenCalledWith(
      expect.stringContaining('theme.syntax.comment2')
    );
  });

  it('reports an alert type it does not draw', () => {
    resolveTheme({}, { admonition: { danger: '#123456' } } as never);
    expect(console.warn).toHaveBeenCalledWith(
      expect.stringContaining('admonition.danger')
    );
  });
});

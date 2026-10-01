/**
 * Theme resolution cost, and the cost of the wire arrays it produces.
 *
 * This is the JavaScript half only. It measures what a `theme` or
 * `markdownStyle` prop costs on the render path: normalizing a value, merging
 * arrays, and building the three positional arrays the native side reads. It
 * says nothing about what the platforms do with them, which is why it is
 * separate from the engine benchmarks in `native-highlight.mjs` and needs a
 * device for the rest. React Native's color conversion is a test stub here;
 * compare revisions using the same Node version and machine.
 *
 * Run: pnpm exec vitest bench benchmarks/theme-resolve.bench.ts --run --silent=false
 */
import { describe, test, type BenchRegistration } from 'vitest';

import { presets } from '../src/presets';
import { syntaxColorsFromTheme as convertSyntaxTheme } from '../src/syntaxTheme';
import { resolveTheme as resolveThemeProp } from '../src/theme';
import type { MarkdownStyle, MarkdownTheme } from '../src/types';

// Bind outside timed callbacks to exclude Vitest's module export getters.
const resolveTheme = resolveThemeProp;
const syntaxColorsFromTheme = convertSyntaxTheme;
const github = presets.github;

/** A theme naming something at every altitude, which is the expensive shape. */
const FULL_THEME: MarkdownTheme = {
  colors: {
    foreground: { light: '#101418', dark: '#e6e1e5' },
    mutedForeground: { light: '#5b5b66', dark: '#938f99' },
    primary: { light: '#7a3e9d', dark: '#c39be0' },
    muted: { light: '#f2f0f4', dark: '#2b2930' },
    border: { light: '#d8d4dd', dark: '#47454c' },
    selection: { light: '#c39be0' },
  },
  fonts: {
    sans: 'Inter',
    heading: 'Inter Display',
    mono: 'JetBrainsMono-Regular',
  },
  radius: 8,
  maxFontSizeMultiplier: 1.3,
  syntax: {
    comment: { light: '#6e7781', dark: '#8b949e' },
    keyword: { light: '#cf222e', dark: '#ff7b72' },
    string: { light: '#0a3069', dark: '#a5d6ff' },
    number: { light: '#0550ae', dark: '#79c0ff' },
    type: { light: '#8250df', dark: '#d2a8ff' },
    function: { light: '#8250df', dark: '#d2a8ff' },
    tag: { light: '#116329', dark: '#7ee787' },
    attribute: { light: '#0550ae', dark: '#79c0ff' },
  },
};

/** A style naming every element, so the walk has all its work to do. */
const FULL_STYLE: MarkdownStyle = {
  paragraph: { fontSize: 17, lineHeight: 26, color: { light: '#101418' } },
  h1: { fontSize: 32, fontWeight: 700, marginTop: 24, marginBottom: 12 },
  h2: { fontSize: 24, fontWeight: 600, marginTop: 20, marginBottom: 8 },
  h3: { fontSize: 20, fontWeight: 600 },
  h4: { fontSize: 18, fontWeight: 600 },
  h5: { fontSize: 16, fontWeight: 600 },
  h6: { fontSize: 15, fontWeight: 600 },
  blockquote: {
    fontStyle: 'italic',
    color: { light: '#5b5b66' },
    borderColor: '#7a3e9d',
    borderWidth: 3,
    gapWidth: 16,
    marginTop: 16,
    marginBottom: 16,
  },
  list: {
    markerColor: '#5b5b66',
    bulletColor: '#7a3e9d',
    marginLeft: 20,
    gapWidth: 6,
    itemSpacing: 4,
    markerFontWeight: 600,
  },
  taskList: { checkedColor: '#116329', borderColor: '#9e9e9e' },
  code: { fontSize: 15, color: '#0a3069', backgroundColor: '#f2f0f4' },
  codeBlock: {
    fontSize: 14,
    padding: 16,
    borderRadius: 8,
    borderWidth: 1,
    borderColor: '#d8d4dd',
    backgroundColor: { light: '#f2f0f4', dark: '#2b2930' },
    header: {
      color: '#5b5b66',
      buttonColor: '#5b5b66',
      backgroundColor: '#eae7ee',
      borderColor: '#d8d4dd',
    },
    syntaxColors: {
      keyword: { light: '#cf222e' },
      string: { light: '#0a3069' },
      comment: '#6e7781',
    },
  },
  link: { color: '#7a3e9d', underline: true, fontWeight: 500 },
  strong: { fontWeight: 700, color: '#101418' },
  em: { fontStyle: 'italic' },
  strikethrough: { color: '#5b5b66' },
  thematicBreak: {
    height: 1,
    color: '#d8d4dd',
    marginTop: 20,
    marginBottom: 20,
  },
  table: {
    fontSize: 15,
    borderColor: '#d8d4dd',
    borderWidth: 1,
    borderRadius: 8,
    cellPaddingHorizontal: 12,
    cellPaddingVertical: 8,
    minColumnWidth: 56,
    maxColumnWidth: 280,
    headerTextColor: '#101418',
    headerBackgroundColor: '#eae7ee',
    rowEvenBackgroundColor: '#f7f6f9',
    rowOddBackgroundColor: { light: '#ffffff', dark: '#232128' },
    toolbar: { buttonColor: '#5b5b66' },
  },
};

/** The editor themes a converter is asked about, by size. */
const DRACULA = {
  name: 'Dracula',
  type: 'dark',
  colors: { 'editor.background': '#282a36', 'editor.foreground': '#f8f8f2' },
  tokenColors: [
    { scope: ['comment'], settings: { foreground: '#6272a4' } },
    {
      scope: ['keyword', 'storage.type', 'storage.modifier'],
      settings: { foreground: '#ff79c6' },
    },
    {
      scope: ['entity.name.function', 'support.function'],
      settings: { foreground: '#50fa7b' },
    },
    {
      scope: ['entity.name.type', 'support.type'],
      settings: { foreground: '#8be9fd' },
    },
    { scope: ['string', 'string.quoted'], settings: { foreground: '#f1fa8c' } },
    {
      scope: ['constant.numeric', 'constant.language'],
      settings: { foreground: '#bd93f9' },
    },
    {
      scope: ['variable', 'variable.other'],
      settings: { foreground: '#f8f8f2' },
    },
    { scope: ['entity.name.tag'], settings: { foreground: '#ff79c6' } },
    {
      scope: ['entity.other.attribute-name'],
      settings: { foreground: '#50fa7b' },
    },
    {
      scope: ['punctuation', 'keyword.operator'],
      settings: { foreground: '#ff79c6' },
    },
  ],
} as const;

/** Print metrics explicitly: Vitest 5 runs these as context benchmarks. */
async function report(registration: BenchRegistration<string>): Promise<void> {
  const result = await registration.run({ time: 500, warmupTime: 250 });
  console.log(
    `${result.name}: mean ${(result.latency.mean * 1000).toFixed(3)} µs, ` +
      `p99 ${(result.latency.p99 * 1000).toFixed(3)} µs, ` +
      `relative margin ±${result.latency.rme.toFixed(2)}%`
  );
}

describe('theme resolution', () => {
  test('empty theme', async ({ bench }) => {
    await report(
      bench('no overrides', () => {
        resolveTheme({}, {});
      })
    );
  });

  test('foreground fan-out', async ({ bench }) => {
    await report(
      bench('one color, 10 destinations', () => {
        resolveTheme({ colors: { foreground: '#101418' } });
      })
    );
  });

  test('color-only theme', async ({ bench }) => {
    // The common case: a reader picks a palette and nothing else moves.
    await report(
      bench(`${Object.keys(FULL_THEME.colors ?? {}).length} colors`, () => {
        resolveTheme({ colors: FULL_THEME.colors }, {});
      })
    );
  });

  test('full theme', async ({ bench }) => {
    await report(
      bench(`every slot at both altitudes`, () => {
        resolveTheme(FULL_THEME, FULL_STYLE);
      })
    );
  });

  test('preset', async ({ bench }) => {
    await report(
      bench('presets.github', () => {
        resolveTheme(github.theme, github.markdownStyle);
      })
    );
  });

  test('merged arrays', async ({ bench }) => {
    // Three layers deep, which is what an app composes from a preset, a brand
    // override, and one screen's own choice.
    const brand: MarkdownTheme = {
      colors: { foreground: '#101418' },
      radius: 6,
    };
    await report(
      bench('3 layers merged', () => {
        resolveTheme(
          [github.theme, brand, FULL_THEME],
          [github.markdownStyle, FULL_STYLE]
        );
      })
    );
  });
});

describe('wire shape', () => {
  test('a resolved theme sorts and packs', async ({ bench }) => {
    // The three arrays the native side reads. A full theme produces one entry
    // per named slot plus one per color's dark half, so this is what a themed
    // view allocates per commit.
    await report(
      bench('full theme, sorted ids', () => {
        const resolved = resolveTheme(FULL_THEME, FULL_STYLE);
        // Read out of the result so the pack cannot be optimized away, and so a
        // regression in how many entries it produces shows up as a shape change.
        return (
          resolved.slots.length + resolved.values.length + resolved.fonts.length
        );
      })
    );
  });
});

describe('syntax conversion', () => {
  test('a VS Code theme', async ({ bench }) => {
    await report(
      bench(`${DRACULA.tokenColors.length} scopes`, () => {
        syntaxColorsFromTheme(DRACULA);
      })
    );
  });

  test('an unrecognised theme', async ({ bench }) => {
    // Every scope misses, which is the worst case: the converter has to walk the
    // whole list and produce nothing. A scope list that resolves to nothing must
    // not cost more than one that resolves to everything.
    await report(
      bench('no scopes match', () => {
        syntaxColorsFromTheme({
          name: 'empty',
          type: 'dark',
          tokenColors: [
            {
              scope: ['nonexistent.scope'],
              settings: { foreground: '#ffffff' },
            },
          ],
        } as never);
      })
    );
  });
});

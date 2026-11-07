import { useMemo, useState } from 'react';
import {
  ScrollView,
  StyleSheet,
  Switch,
  Text,
  View,
  useColorScheme,
} from 'react-native';
import {
  Downstream,
  presets,
  syntaxColorsFromTheme,
  type MarkdownPreset,
  type MarkdownTheme,
  type VSCodeThemeJson,
} from '@react-native-downstream/core';

import { Badge, ChipRow, Group, Notice } from '../ui/kit';
import {
  Hero,
  Screen,
  SectionLabel,
  useBottomInset,
  useScreenScroll,
} from '../ui/screen';
import { radius, space, useTheme } from '../ui/theme';

/**
 * One document with something for every kind of themeable value: headings at two
 * levels, a quote, an alert, a list with a task, inline and block code, a table,
 * a link, and a rule.
 */
const SOURCE = `# Theming

A theme sets **slots**, not styles. Both renderers read one table, so a theme
written here draws the same on each platform.

## Colors

> A quote is a color of its own, so a themed quote can restyle the text inside it
> rather than only tinting the bar.

> [!NOTE]
> An alert takes its own color, one slot per GitHub alert kind.

- a bullet
- a second item
- [x] a done item

## Type and code

Inline \`code\` sits beside body text, and \`strong\` and *emphasis* ride on top of
whatever the theme set for the block.

\`\`\`ts
export function theme(id: string): Theme {
  return intern({ id, colors: { label: '#101418' } });
}
\`\`\`

## A table

| Slot | Kind | Changes layout |
| --- | --- | --- |
| \`paragraph.fontSize\` | metric | yes |
| \`paragraph.color\` | color | no |
| \`maxFontSizeMultiplier\` | metric | yes |

---

A [link](https://example.com) and a rule.
`;

/**
 * A theme written by hand rather than taken from a preset, to show that the
 * precise layer reaches the same slots the presets do.
 *
 * The values are deliberately lopsided: one appearance is left alone, so a
 * reader can see that a theme changes only what it names.
 */
const CUSTOM = {
  theme: {
    colors: {
      foreground: { light: '#1b1b1f' },
      mutedForeground: { light: '#5b5b66' },
      primary: { light: '#7a3e9d', dark: '#c39be0' },
      border: { light: '#d8d4dd' },
    },
    fonts: { heading: 'Georgia' },
  } satisfies MarkdownTheme,
  markdownStyle: {
    paragraph: { fontSize: 17, lineHeight: 26 },
    h1: { fontWeight: 700, marginBottom: 8 },
    h2: { fontWeight: 600, marginTop: 24 },
    blockquote: { fontStyle: 'italic' },
    codeBlock: { padding: 14, borderRadius: 8 },
  },
} satisfies MarkdownPreset;

/** Only the dark half is named, so the light one keeps the platform's look. */
const DARK_ONLY = {
  theme: {
    colors: {
      foreground: { dark: '#e6e1e5' },
      mutedForeground: { dark: '#938f99' },
    },
  } satisfies MarkdownTheme,
  markdownStyle: {
    codeBlock: {
      backgroundColor: { dark: '#2b2930' },
      syntaxColors: {
        keyword: { dark: '#ff7b72' },
        string: { dark: '#a5d6ff' },
      },
    },
  },
} satisfies MarkdownPreset;

/**
 * A VS Code theme read from its own JSON, to show the syntax converter.
 *
 * Small on purpose: the point is the round trip through Shiki's format, not the
 * coverage of a real theme file.
 */
const VSCODE_JSON = {
  name: 'Downstream demo',
  type: 'dark',
  colors: {
    'editor.background': '#1b1b1f',
    'editor.foreground': '#e6e1e5',
  },
  tokenColors: [
    {
      scope: ['comment', 'punctuation.definition.comment'],
      settings: { foreground: '#8b949e' },
    },
    {
      scope: ['keyword', 'storage', 'keyword.control'],
      settings: { foreground: '#ff7b72' },
    },
    {
      scope: ['entity.name.type', 'support.type', 'storage.type'],
      settings: { foreground: '#d2a8ff' },
    },
    { scope: ['string', 'string.quoted'], settings: { foreground: '#a5d6ff' } },
    {
      scope: ['constant.numeric', 'constant.language'],
      settings: { foreground: '#79c0ff' },
    },
    {
      scope: ['entity.name.function', 'support.function'],
      settings: { foreground: '#d2a8ff' },
    },
  ],
} as unknown as VSCodeThemeJson;

/** A code block drawn with nothing but the converter's output. */
const CODE_SAMPLE = `\`\`\`ts
// The converter maps TextMate scopes onto token classes.
export function convert(theme: Theme): number {
  const name = 'demo';
  return theme.tokenColors.length + 42;
}
\`\`\``;

/** What the screen switches between, in the order it offers them. */
const MODES = [
  { value: 'none', label: 'No theme' },
  { value: 'github', label: 'github' },
  { value: 'streamdown', label: 'streamdown' },
  { value: 'serif', label: 'Custom' },
  { value: 'dark-only', label: 'Dark only' },
] as const;

type Mode = (typeof MODES)[number]['value'];

export function ThemeScreen() {
  const t = useTheme();
  const scroll = useScreenScroll();
  const bottom = useBottomInset();
  const scheme = useColorScheme();
  const [mode, setMode] = useState<Mode>('github');
  const [capScale, setCapScale] = useState(false);

  const preset = useMemo<MarkdownPreset | undefined>(() => {
    switch (mode) {
      case 'none':
        return undefined;
      case 'github':
        return presets.github;
      case 'streamdown':
        return presets.streamdown;
      case 'serif':
        return CUSTOM;
      case 'dark-only':
        return DARK_ONLY;
    }
  }, [mode]);

  // The one thing a theme reaches outside its own slots: a cap on the platform's
  // font scale. It belongs inside `theme` because that is the only place the
  // resolver reads it from, and it can cap scaling without being able to turn it
  // off.
  const capped = useMemo<MarkdownPreset | undefined>(
    () =>
      capScale && preset
        ? { ...preset, theme: { ...preset.theme, maxFontSizeMultiplier: 1.15 } }
        : preset,
    [capScale, preset]
  );

  // The converter's own output, so the screen shows what it produced rather than
  // only what the renderer drew with it.
  const converted = useMemo(() => syntaxColorsFromTheme(VSCODE_JSON), []);

  return (
    <Screen title="Theme" scroll={scroll}>
      <ScrollView
        onScroll={scroll.onScroll}
        scrollEventThrottle={16}
        contentContainerStyle={[styles.content, { paddingBottom: bottom }]}
      >
        <Hero
          title="Theme"
          summary="Slot-based theming, both presets, and a VS Code theme read in."
        />

        <SectionLabel>Theme</SectionLabel>
        <ChipRow options={MODES} value={mode} onChange={setMode} />

        <View style={styles.controls}>
          <Group title="Appearance">
            <Text style={[t.s.caption, styles.groupBody]}>
              The device is in {scheme === 'dark' ? 'dark' : 'light'} right now.
              A theme follows it without a render, because every color it names
              is a pair and the platform resolves that pair when it draws.
            </Text>
          </Group>

          <Group title="Font scale">
            <View style={styles.row}>
              <Text style={styles.rowLabel}>Cap at 1.15</Text>
              <Switch
                accessibilityLabel="Cap font scale at 1.15"
                value={capScale}
                onValueChange={setCapScale}
              />
            </View>
            <Text style={[t.s.caption, styles.groupBody]}>
              Caps the platform&apos;s font scale, as{' '}
              <Text style={t.s.mono}>maxFontSizeMultiplier</Text> does.
            </Text>
          </Group>

          {mode !== 'none' && (
            <Notice tone="accent" title="Only what it names">
              A theme changes only the slots it sets. Everything else keeps the
              platform&apos;s own color, so it still follows the appearance and
              increased contrast.
            </Notice>
          )}
          {mode === 'dark-only' && (
            <Badge
              label="Only the dark half is set here, on purpose."
              tone="warn"
            />
          )}
        </View>

        <SectionLabel>Rendered</SectionLabel>
        <View style={[t.s.card, styles.card]}>
          <Downstream {...capped}>{SOURCE}</Downstream>
        </View>

        <SectionLabel>From a VS Code theme</SectionLabel>
        <View style={styles.controls}>
          <Text style={t.s.caption}>
            <Text style={t.s.mono}>syntaxColorsFromTheme</Text> reads
            Shiki&apos;s own format and answers with the engine&apos;s token
            classes. A scope it cannot resolve is listed as unresolved rather
            than quietly drawn as something else.
          </Text>
          <Group title="Converted token colors">
            <View style={styles.groupBody}>
              {(
                [
                  'comment',
                  'keyword',
                  'string',
                  'type',
                  'number',
                  'function',
                ] as const
              ).map((token) => (
                <View key={token} style={styles.swatchLine}>
                  <Text style={styles.rowLabel}>{token}</Text>
                  <Swatch color={converted.syntax[token]} />
                </View>
              ))}
            </View>
          </Group>
        </View>
        <View style={[t.s.card, styles.card]}>
          <Downstream
            markdown={CODE_SAMPLE}
            theme={{ syntax: converted.syntax }}
            markdownStyle={{ codeBlock: converted.codeBlock }}
          />
        </View>
      </ScrollView>
    </Screen>
  );
}

/** One converted token color, or a dash where the converter found none. */
function Swatch({ color }: { color: string | undefined }) {
  const t = useTheme();
  return (
    <View style={styles.swatchRow}>
      <View
        style={[
          styles.swatch,
          { backgroundColor: color ?? t.c.sunken, borderColor: t.c.lineSoft },
        ]}
      />
      <Text style={t.s.mono}>{color ?? 'unresolved'}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  content: { paddingTop: space.sm },
  controls: {
    paddingHorizontal: space.xl,
    paddingTop: space.md,
    paddingBottom: space.md,
    gap: space.md,
  },
  groupBody: {
    paddingHorizontal: space.lg,
    paddingBottom: space.lg,
  },
  row: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: space.md,
    minHeight: 44,
    paddingHorizontal: space.lg,
  },
  swatchLine: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: space.md,
    minHeight: 36,
  },
  rowLabel: {
    flexShrink: 1,
  },
  swatchRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: space.sm,
  },
  swatch: {
    width: 20,
    height: 20,
    borderRadius: radius.pill,
    borderWidth: StyleSheet.hairlineWidth,
  },
  card: {
    marginHorizontal: space.xl,
    marginTop: space.md,
    padding: space.lg,
    borderRadius: radius.card,
  },
});

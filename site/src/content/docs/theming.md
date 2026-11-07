---
title: Theming
description: Start from a preset, then override colors, fonts, radius, and individual Markdown elements.
group: Guide
order: 9
---

Start with `presets.github` or `presets.streamdown`, then override what you need:

```tsx
import { Downstream, presets } from '@react-native-downstream/core';

<Downstream
  theme={[
    presets.github.theme,
    {
      colors: {
        foreground: { light: '#18202b', dark: '#edf2f7' },
        primary: { light: '#4f46e5', dark: '#a5b4fc' },
      },
      radius: 12,
    },
  ]}
  markdownStyle={[
    presets.github.markdownStyle,
    { h1: { fontSize: 28 }, paragraph: { fontSize: 16 } },
  ]}
>
  {text}
</Downstream>;
```

`theme` controls document colors, fonts, corner radius, syntax colors, and the font scale cap. `markdownStyle` controls individual elements, including headings, paragraphs, lists, blockquotes, code, tables, links, alerts, images, and math.

Both accept a value or an array merged left to right. Element styles override theme tokens.

## Theme tokens

| Token                    | Controls                                          |
| ------------------------ | ------------------------------------------------- |
| `colors.foreground`      | Body and heading text.                            |
| `colors.mutedForeground` | Quotes, h6, markers, alt text, control chrome.    |
| `colors.primary`         | Links and footnote references.                    |
| `colors.muted`           | The code card, inline code, and table header row. |
| `colors.border`          | Table grid, rules, and quote bars.                |
| `colors.selection`       | Text selection highlight.                         |
| `radius`                 | Corner radius for code cards and tables.          |
| `fonts.sans`             | Body font family.                                 |
| `fonts.heading`          | Heading font family.                              |
| `fonts.mono`             | Code font family.                                 |
| `syntax`                 | Token colors by engine class.                     |
| `maxFontSizeMultiplier`  | Caps Dynamic Type and Android font scaling.       |

Colors accept strings or `{ light, dark }` pairs. Defaults follow the platform appearance, and a pair is drawn as a dynamic color, so switching appearance needs no new theme and no React render.

Custom fonts must already be available in your app.

## Per-element styles

`markdownStyle` uses `react-native-enriched-markdown`'s key and property names for every key this renderer draws, so a value written for the closest native peer works as it is.

```tsx
<Downstream
  markdownStyle={{
    h1: { fontSize: 30, fontWeight: 800, marginBottom: 12 },
    paragraph: { fontSize: 16, lineHeight: 26 },
    blockquote: { borderColor: '#3654d9', borderWidth: 4, gapWidth: 12 },
    codeBlock: {
      backgroundColor: '#171a24',
      borderRadius: 14,
      header: { backgroundColor: '#10131a', buttonColor: '#f5d94f' },
    },
    table: { borderColor: '#1c1b19', headerBackgroundColor: '#3654d9' },
  }}
>
  {text}
</Downstream>
```

A key or property this renderer does not draw is reported once in development rather than silently ignored.

## Syntax colors from a VS Code theme

Syntax colors style code highlighted by the [`code()` plugin](/docs/code-blocks). To start from a VS Code or Shiki JSON theme, convert its token colors with `syntaxColorsFromTheme`:

```tsx
import { syntaxColorsFromTheme } from '@react-native-downstream/core';
import myTheme from './my-theme.json';

const converted = syntaxColorsFromTheme(myTheme);

<Downstream
  theme={{ syntax: converted.syntax }}
  markdownStyle={{ codeBlock: { syntaxColors: converted.codeBlock } }}
>
  {text}
</Downstream>;
```

## Icons and labels

Replace control glyphs with platform icon names or bundled image sources, and customize labels and accessibility announcements:

```tsx
<Downstream
  icons={{ copy: 'doc.on.doc', check: 'checkmark' }}
  translations={{
    copyCode: 'Copy code',
    copied: 'Copied',
    wrapCode: 'Wrap lines',
  }}
>
  {text}
</Downstream>
```

See the [API reference](/docs/api) for the full control and translation surface.

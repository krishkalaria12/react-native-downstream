---
title: Links & interaction
description: Link safety, relative URLs, selection, task lists, direction, CJK parsing, and the options that change text behavior.
group: Guide
order: 8
---

## Links

Link presses show a confirmation by default. `links.safety` can supply an asynchronous `onLinkCheck` or a custom `renderModal`. `links={{ safety: false }}` opens links directly.

With `links.onPress`, your app handles navigation and the built-in confirmation does not run:

```tsx
import { Linking } from 'react-native';

<Downstream
  links={{
    defaultOrigin: 'https://docs.example.com',
    onPress: ({ url }) => {
      Linking.openURL(url).catch(console.warn);
    },
  }}
>
  {text}
</Downstream>;
```

Events include the resolved `url`, the original `href`, and the source range.

| Option                           | Behavior                                                                      |
| -------------------------------- | ----------------------------------------------------------------------------- |
| `links.defaultOrigin`            | Enables and resolves relative links against a base URL.                       |
| `links.relative`                 | Enables relative links for your own router without a base URL.                |
| `markdownStyle.linkVariants`     | Styles links by URL prefix.                                                   |
| `links.onLongPress`              | Handles a hold on a link. The library does nothing with it by default.        |
| `links.preview`                  | iOS link previews. Off by default.                                            |
| `links.disableAutolinkProtocols` | Disables automatic links for selected `http`, `https`, or `mailto` protocols. |

## Selection and task lists

`selectable` enables native selection for settled text. It is on by default, and unfinished text is never selectable. Set it to `false` when your app owns the message menu.

`onTaskPress` reports a task checkbox press. The document does not change: update your Markdown source to flip the marker.

```tsx
<Downstream
  onTaskPress={({ checked }) => {
    // Update your source and re-render.
  }}
>
  {text}
</Downstream>
```

## Direction and language

`dir` is `auto`, `ltr`, or `rtl`. `auto` is the default and is decided per block by the native engine: a block reads the way the majority of its strong characters read, and a block with no strong character inherits the block before it.

`cjk` enables CJK-friendly emphasis, strikethrough, and autolink boundaries. It is on by default. A document with no CJK parses identically either way.

## Inline HTML and extra syntax

`inlineHtml` supports `<br>`, `<sub>`, and `<sup>`. It is on by default.

`syntax` turns on optional md4c syntaxes, all off by default:

```tsx
<Downstream
  syntax={{
    superscript: true,
    subscript: true,
    highlight: true,
    underline: true,
    spoiler: true,
  }}
>
  {text}
</Downstream>
```

`normalizeHtmlIndentation` normalizes indentation for documents that start with an HTML block. It is off by default.

## Accessibility

- Settled text is exposed to screen readers with its Markdown structure.
- Code line numbers are never read aloud.
- The streaming caret is never copied, selected, exported, or read aloud.
- Animation respects Reduce Motion and Android's animator scale.
- `theme.maxFontSizeMultiplier` caps Dynamic Type and Android font scaling.

## Full option table

| Option                           | Behavior and default                                                                                 |
| -------------------------------- | ---------------------------------------------------------------------------------------------------- |
| `selectable`                     | Native selection for settled text. On by default.                                                    |
| `onTaskPress`                    | Reports a task checkbox press.                                                                       |
| `dir`                            | `auto`, `ltr`, or `rtl`. Defaults to automatic direction per block.                                  |
| `cjk`                            | CJK-friendly emphasis, strikethrough, and autolinks. On by default.                                  |
| `inlineHtml`                     | Supports `<br>`, `<sub>`, and `<sup>`. On by default.                                                |
| `syntax`                         | Optional superscript, subscript, highlight, underline, and spoiler. All off.                         |
| `links.disableAutolinkProtocols` | Disables automatic links for selected protocols.                                                     |
| `normalizeHtmlIndentation`       | Normalizes indentation for documents that start with an HTML block. Off.                             |
| `androidParagraphLayout`         | `canonical` by default. `incremental` reuses completed lines in eligible streaming ASCII paragraphs. |

---
title: API reference
description: Every prop, stream option, controller method, callback, and exported type.
group: Reference
order: 9
---

## Component

```tsx
import { DownstreamMarkdown } from '@react-native-downstream/core';
```

`DownstreamMarkdown` takes either a complete `markdown` string or a `stream` controller, never both.

```tsx
type DownstreamMarkdownProps =
  | ({ markdown: string; stream?: never } & ViewProps)
  | ({ stream: MarkdownStreamController; markdown?: never } & ViewProps);
```

### Content and animation

| Prop               | Type                                | Default | What it does                                                                                        |
| ------------------ | ----------------------------------- | ------- | --------------------------------------------------------------------------------------------------- |
| `markdown`         | `string`                            | —       | A complete document. Re-renders when the string changes.                                            |
| `stream`           | `MarkdownStreamController`          | —       | A live controller from `createMarkdownStream`.                                                      |
| `isAnimating`      | `boolean`                           | —       | Whether text is still arriving. A stream follows its controller; pass this for a controlled string. |
| `animated`         | `boolean \| MarkdownAnimateOptions` | `false` | Fades new units in while animating.                                                                 |
| `caret`            | `'block' \| 'circle'`               | —       | Draws a caret after the newest text while animating.                                                |
| `onAnimationStart` | `() => void`                        | —       | `isAnimating` turned on, including on mount.                                                        |
| `onAnimationEnd`   | `() => void`                        | —       | `isAnimating` turned off, before the last fade finishes.                                            |

`animated` options: `animation` (`fadeIn`, `slideUp`, `blurIn`, `none`), `duration` (150), `easing` (`ease`), `sep` (`word` or `char`), `stagger` (40), and `maxBacklogMs` (320).

### Parsing

| Prop                       | Type                                      | Default           | What it does                                                               |
| -------------------------- | ----------------------------------------- | ----------------- | -------------------------------------------------------------------------- |
| `math`                     | `boolean \| MarkdownMathOptions`          | on when installed | Typesets formulas. `singleDollarTextMath` is off; `brackets` is on.        |
| `onMathError`              | `(event: MarkdownMathErrorEvent) => void` | —                 | A formula the engine could not typeset, once per distinct source per view. |
| `inlineHtml`               | `boolean`                                 | `true`            | Renders `<br>`, `<sub>`, and `<sup>`. Other tags stay text.                |
| `syntax`                   | `MarkdownSyntaxOptions`                   | all off           | `superscript`, `subscript`, `highlight`, `underline`, `spoiler`.           |
| `cjk`                      | `boolean`                                 | `true`            | CJK-friendly emphasis, strikethrough, and autolink boundaries.             |
| `links`                    | `MarkdownLinksOptions`                    | off               | `relative` and `defaultOrigin` for relative destinations.                  |
| `disableAutolinkProtocols` | `readonly string[]`                       | —                 | Protocols a bare address may not autolink to.                              |
| `allowedTags`              | `MarkdownAllowedTags`                     | —                 | Extra tags and the attributes each may carry.                              |
| `literalTagContent`        | `boolean`                                 | `false`           | Leaves allowed tag content unstyled instead of parsing Markdown inside it. |
| `normalizeHtmlIndentation` | `boolean`                                 | `false`           | Moves leading indentation after a document-leading HTML block.             |
| `dir`                      | `'ltr' \| 'rtl' \| 'auto'`                | `auto`            | Layout direction. `auto` decides per block from its strong characters.     |

### Layout and text

| Prop                     | Type                                    | Default     | What it does                                                              |
| ------------------------ | --------------------------------------- | ----------- | ------------------------------------------------------------------------- |
| `style`                  | `StyleProp<ViewStyle>`                  | —           | Outer view style.                                                         |
| `selectable`             | `boolean`                               | `true`      | Native text selection, applied per settled segment.                       |
| `androidParagraphLayout` | `'canonical' \| 'incremental'`          | `canonical` | Reuses completed lines in an unfinished plain ASCII paragraph on Android. |
| `tableMaxHeight`         | `number \| '0' \| 'none' \| 'Infinity'` | `300`       | Height before a table scrolls.                                            |

### Code blocks

| Prop                 | Type                                    | Default | What it does                                                                       |
| -------------------- | --------------------------------------- | ------- | ---------------------------------------------------------------------------------- |
| `syntaxHighlighting` | `boolean`                               | `true`  | Colors fences by info string. Unknown languages and blocks over 64 KiB stay plain. |
| `lineNumbers`        | `boolean`                               | `true`  | Numbers beside fenced and indented code. Never copied or read aloud.               |
| `codeBlockWrap`      | `boolean`                               | `false` | Wraps long lines instead of scrolling them.                                        |
| `codeBlockMaxHeight` | `number \| '0' \| 'none' \| 'Infinity'` | `400`   | Height before a code block scrolls inside its card.                                |

### Controls and labels

| Prop           | Type                                                       | Default  | What it does                                                    |
| -------------- | ---------------------------------------------------------- | -------- | --------------------------------------------------------------- |
| `controls`     | `boolean \| MarkdownControls`                              | on       | Copy, download, wrap, expand, full-screen, and details toggles. |
| `translations` | `Partial<MarkdownTranslations>`                            | English  | Button labels and spoken announcements.                         |
| `icons`        | `Partial<Record<MarkdownControlIcon, MarkdownIconSource>>` | platform | Replace control glyphs with icon names or image sources.        |

A control key accepts `true`, `false`, or an object with its own callbacks. Missing keys mean on.

```tsx
<DownstreamMarkdown
  markdown={text}
  controls={{
    code: {
      copy: true,
      download: { filename: 'snippet' },
      wrap: true,
      expand: true,
    },
    table: {
      copy: true,
      download: { filename: 'data' },
      csvSeparator: ',',
      expand: true,
      fullscreen: true,
    },
    image: { download: true },
    details: { expand: true },
  }}
/>
```

### Styling

| Prop            | Type                        | What it does                                                  |
| --------------- | --------------------------- | ------------------------------------------------------------- |
| `theme`         | `Composable<MarkdownTheme>` | Colors, fonts, radius, syntax colors, and the font scale cap. |
| `markdownStyle` | `Composable<MarkdownStyle>` | Per-element styles. Wins over `theme`.                        |
| `plugins`       | `MarkdownPlugins`           | Fence renderers, tag renderers, and a fallback component.     |

Both `theme` and `markdownStyle` accept one value or an array merged left to right. See [Theming](/docs/theming).

### Images

| Prop                   | Type                                  | Default | What it does                                              |
| ---------------------- | ------------------------------------- | ------- | --------------------------------------------------------- |
| `images`               | `boolean`                             | `true`  | Load images. `false` shows alt text and makes no request. |
| `allowedImagePrefixes` | `readonly string[]`                   | `['*']` | URL prefixes an image may load from.                      |
| `onImagePress`         | `(event: MarkdownImageEvent) => void` | —       | A press on a loaded image that is not inside a link.      |

### Links and interaction

| Prop                    | Type                                                 | Default | What it does                                                                                |
| ----------------------- | ---------------------------------------------------- | ------- | ------------------------------------------------------------------------------------------- |
| `onLinkPress`           | `(event: MarkdownLinkPressEvent) => void`            | —       | A press on a link. Receives the resolved `url`, the written `href`, and the source `range`. |
| `onLinkLongPress`       | `(event: MarkdownLinkPressEvent) => void`            | —       | A hold on a link. The library does nothing else with it.                                    |
| `linkSafety`            | `boolean \| MarkdownLinkSafetyOptions`               | on      | Confirms an external link before opening when there is no `onLinkPress`.                    |
| `linkPreview`           | `boolean`                                            | `false` | iOS peek-and-pop on a held link.                                                            |
| `onTagPress`            | `(event: MarkdownTagEvent) => void`                  | —       | A press on a tag `allowedTags` allows.                                                      |
| `onTaskPress`           | `(event: MarkdownTaskPressEvent) => void`            | —       | A press on a task list checkbox. The app owns the source.                                   |
| `onTableFullscreen`     | `(event: MarkdownTableFullscreenEvent) => void`      | —       | A table's full-screen button.                                                               |
| `renderTableFullscreen` | `(event: MarkdownTableFullscreenEvent) => ReactNode` | —       | Replaces the built-in full-screen modal.                                                    |
| `onError`               | `(error: MarkdownErrorEvent) => void`                | —       | Parse, render, control, and renderer failures.                                              |

## createMarkdownStream

```tsx
import { createMarkdownStream } from '@react-native-downstream/core';

const stream = createMarkdownStream({
  preview: true,
  math: { singleDollarTextMath: true },
  onError: (error) => console.warn(error.code, error.message),
});
```

### Options

| Option                     | Type                                  | Default           | What it does                                         |
| -------------------------- | ------------------------------------- | ----------------- | ---------------------------------------------------- |
| `onError`                  | `(error: MarkdownErrorEvent) => void` | —                 | Failures for this controller.                        |
| `preview`                  | `boolean \| MarkdownPreviewOptions`   | off               | Provisional styling for the unstable tail.           |
| `math`                     | `boolean \| MarkdownMathOptions`      | on when installed | Math parsing for this stream, read once at creation. |
| `inlineHtml`               | `boolean`                             | `true`            | Render `<br>`, `<sub>`, and `<sup>`.                 |
| `syntax`                   | `MarkdownSyntaxOptions`               | all off           | Inline syntax extensions.                            |
| `cjk`                      | `boolean`                             | `true`            | CJK-friendly boundaries.                             |
| `links`                    | `MarkdownLinksOptions`                | off               | Relative link handling.                              |
| `disableAutolinkProtocols` | `readonly string[]`                   | —                 | Protocols a bare address may not autolink to.        |
| `allowedTags`              | `MarkdownAllowedTags`                 | —                 | Extra allowed tags.                                  |
| `literalTagContent`        | `boolean`                             | `false`           | Leaves tag content unstyled.                         |

Parse options are part of a source's identity, so a stream takes them when it is created. `reset()` keeps them. The view's `math` prop only decides whether parsed formulas are typeset.

### Controller

| Method       | Signature                                              | What it does                                                         |
| ------------ | ------------------------------------------------------ | -------------------------------------------------------------------- |
| `append`     | `(chunk: string) => void`                              | Adds the next chunk. Updates are batched by animation frame.         |
| `finish`     | `() => void`                                           | Marks the response complete.                                         |
| `reset`      | `(source?: string) => void`                            | Starts a new response, optionally with initial text.                 |
| `getSource`  | `() => Promise<string>`                                | Resolves with the exact source supplied.                             |
| `setPreview` | `(options: boolean \| MarkdownPreviewOptions) => void` | Changes provisional styling for the rest of the stream.              |
| `dispose`    | `() => void`                                           | Releases the controller. A disposed controller publishes no updates. |

A controller binds to one view. Create it per message, outside the render path, and dispose it when the message is removed.

## Errors

A rejected synchronous stream operation throws `MarkdownOperationError` with a `code`. `onError` receives a `MarkdownErrorEvent`.

| Code              | Meaning                                                            |
| ----------------- | ------------------------------------------------------------------ |
| `invalid_input`   | The source or an option was rejected.                              |
| `input_limit`     | The document exceeded a size limit.                                |
| `invalid_state`   | The operation is not valid in the controller's current state.      |
| `session_bound`   | The controller is already bound to another view.                   |
| `parse_failed`    | The native parser failed.                                          |
| `render_failed`   | The native renderer failed.                                        |
| `control_failed`  | A control action failed. The event names the `block` and `action`. |
| `renderer_failed` | A custom fence renderer failed. The event names the `language`.    |

```tsx
onError={(error) => {
  if (error.code === 'control_failed') {
    console.warn(error.block, error.action, error.message);
  }
}}
```

## Exports

```tsx
import {
  DownstreamMarkdown,
  createMarkdownStream,
  presets,
  syntaxColorsFromTheme,
  resolveAllowedTags,
  resolveParseFlags,
  mathAvailable,
  openLinkSafely,
  resolveLinkOptions,
  resolveLinkUrl,
  defaultTranslations,
  defaultTableMaxHeight,
  resolveMaxHeight,
  defaultDuration,
  defaultStagger,
  defaultMaxBacklogMs,
  resolveAnimate,
  resolveEasing,
} from '@react-native-downstream/core';
```

Every public type is exported from the package root, including `DownstreamMarkdownProps`, `MarkdownStreamOptions`, `MarkdownStreamController`, `MarkdownTheme`, `MarkdownStyle`, `MarkdownControls`, `MarkdownErrorEvent`, `MarkdownLinkPressEvent`, `MarkdownTagEvent`, `MarkdownImageEvent`, `MarkdownMathErrorEvent`, `CustomRenderer`, and `CustomRendererProps`.

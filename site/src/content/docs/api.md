---
title: API reference
description: Every prop, grouped option, plugin, controller method, callback, and exported type.
group: Reference
order: 10
---

## Component

```tsx
import { Downstream } from '@react-native-downstream/core';
```

`Downstream` takes one source: string children, a `markdown` string, or a `stream` controller.

```tsx
type DownstreamProps = DownstreamOptions &
  (
    | { children: string }
    | { markdown: string }
    | { stream: MarkdownStreamController }
  );
```

### Source and animation

| Prop               | Type                                | Default | What it does                                                                                    |
| ------------------ | ----------------------------------- | ------- | ----------------------------------------------------------------------------------------------- |
| `children`         | `string`                            | —       | The document. Growth appends to the session; any other change replaces it.                      |
| `markdown`         | `string`                            | —       | The same as string children, as a prop.                                                         |
| `stream`           | `MarkdownStreamController`          | —       | A live controller from `createMarkdownStream`.                                                  |
| `isAnimating`      | `boolean`                           | —       | Whether text is still arriving. A stream follows its controller; pass this for a string source. |
| `animated`         | `boolean \| MarkdownAnimateOptions` | `false` | Fades new units in while animating.                                                             |
| `caret`            | `'block' \| 'circle'`               | —       | Draws a caret after the newest text while animating.                                            |
| `preview`          | `boolean \| MarkdownPreviewOptions` | `true`  | Provisional styling for unfinished Markdown while streaming.                                    |
| `onAnimationStart` | `() => void`                        | —       | `isAnimating` turned on, including on mount.                                                    |
| `onAnimationEnd`   | `() => void`                        | —       | `isAnimating` turned off, before the last fade finishes.                                        |

`animated` options: `animation` (`fadeIn`, `slideUp`, `blurIn`, `none`), `duration` (150), `easing` (`ease`), `sep` (`word` or `char`), `stagger` (40), and `maxBacklogMs` (320). `preview` keys: `emphasis`, `inlineCode`, `links`, `images`, `math`, `htmlTags`, and `tables`.

### Plugins and components

| Prop         | Type                          | What it does                                                                     |
| ------------ | ----------------------------- | -------------------------------------------------------------------------------- |
| `plugins`    | `readonly DownstreamPlugin[]` | Feature plugins such as `code()` and `math()`. Replaces the inherited selection. |
| `components` | `DownstreamComponents`        | App components for fences, block tags, and a fallback.                           |

`components` fields:

| Field      | Type                                                 | What it does                                     |
| ---------- | ---------------------------------------------------- | ------------------------------------------------ |
| `fences`   | `Record<string, ComponentType<CustomRendererProps>>` | Renders a fenced language with your component.   |
| `tags`     | `Record<string, ComponentType<TagRendererProps>>`    | Replaces an allowed block tag.                   |
| `fallback` | `ComponentType<FallbackRendererProps>`               | Shared replacement for unmatched fences or tags. |

### Parsing

| Prop                       | Type                                      | Default | What it does                                                               |
| -------------------------- | ----------------------------------------- | ------- | -------------------------------------------------------------------------- |
| `onMathError`              | `(event: MarkdownMathErrorEvent) => void` | —       | A formula the engine could not typeset, once per distinct source per view. |
| `inlineHtml`               | `boolean`                                 | `true`  | Renders `<br>`, `<sub>`, and `<sup>`. Other tags stay text.                |
| `syntax`                   | `MarkdownSyntaxOptions`                   | all off | `superscript`, `subscript`, `highlight`, `underline`, `spoiler`.           |
| `cjk`                      | `boolean`                                 | `true`  | CJK-friendly emphasis, strikethrough, and autolink boundaries.             |
| `allowedTags`              | `MarkdownAllowedTags`                     | —       | Extra tags and the attributes each may carry.                              |
| `literalTagContent`        | `boolean`                                 | `false` | Leaves allowed tag content unstyled instead of parsing Markdown inside it. |
| `normalizeHtmlIndentation` | `boolean`                                 | `false` | Moves leading indentation after a document-leading HTML block.             |
| `dir`                      | `'ltr' \| 'rtl' \| 'auto'`                | `auto`  | Layout direction. `auto` decides per block from its strong characters.     |

Math delimiters are options of the `math()` plugin: `singleDollarTextMath` (off) and `brackets` (on).

### Layout and text

| Prop                     | Type                           | Default     | What it does                                                              |
| ------------------------ | ------------------------------ | ----------- | ------------------------------------------------------------------------- |
| `style`                  | `StyleProp<ViewStyle>`         | —           | Outer view style.                                                         |
| `selectable`             | `boolean`                      | `true`      | Native text selection, applied per settled segment.                       |
| `androidParagraphLayout` | `'canonical' \| 'incremental'` | `canonical` | Reuses completed lines in an unfinished plain ASCII paragraph on Android. |

### Code blocks

`codeBlock` groups the layout of fenced and indented code. Highlighting comes from the `code()` plugin.

| Field         | Type                                    | Default | What it does                                                         |
| ------------- | --------------------------------------- | ------- | -------------------------------------------------------------------- |
| `lineNumbers` | `boolean`                               | `true`  | Numbers beside fenced and indented code. Never copied or read aloud. |
| `wrap`        | `boolean`                               | `false` | Wraps long lines instead of scrolling them.                          |
| `maxHeight`   | `number \| '0' \| 'none' \| 'Infinity'` | `400`   | Height before a code block scrolls inside its card.                  |

### Tables

| Field              | Type                                                 | Default | What it does                             |
| ------------------ | ---------------------------------------------------- | ------- | ---------------------------------------- |
| `maxHeight`        | `number \| '0' \| 'none' \| 'Infinity'`              | `300`   | Height before a table scrolls.           |
| `onFullscreen`     | `(event: MarkdownTableFullscreenEvent) => void`      | —       | A table's full-screen button.            |
| `renderFullscreen` | `(event: MarkdownTableFullscreenEvent) => ReactNode` | —       | Replaces the built-in full-screen modal. |

### Images

`images` is `true` by default. `false` shows alt text and makes no request. An object enables images with these fields:

| Field             | Type                                  | Default | What it does                                         |
| ----------------- | ------------------------------------- | ------- | ---------------------------------------------------- |
| `allowedPrefixes` | `readonly string[]`                   | `['*']` | URL prefixes an image may load from.                 |
| `onPress`         | `(event: MarkdownImageEvent) => void` | —       | A press on a loaded image that is not inside a link. |

### Links

| Field                      | Type                                      | Default | What it does                                                                                |
| -------------------------- | ----------------------------------------- | ------- | ------------------------------------------------------------------------------------------- |
| `onPress`                  | `(event: MarkdownLinkPressEvent) => void` | —       | A press on a link. Receives the resolved `url`, the written `href`, and the source `range`. |
| `onLongPress`              | `(event: MarkdownLinkPressEvent) => void` | —       | A hold on a link. The library does nothing else with it.                                    |
| `safety`                   | `boolean \| MarkdownLinkSafetyOptions`    | on      | Confirms an external link before opening when there is no `onPress`.                        |
| `preview`                  | `boolean`                                 | `false` | iOS peek-and-pop on a held link.                                                            |
| `defaultOrigin`            | `string`                                  | —       | Enables and resolves relative links against a base URL.                                     |
| `relative`                 | `boolean`                                 | `false` | Enables relative links for your own router without a base URL.                              |
| `disableAutolinkProtocols` | `readonly string[]`                       | —       | Protocols a bare address may not autolink to.                                               |

### Controls and labels

| Prop           | Type                                                       | Default  | What it does                                                    |
| -------------- | ---------------------------------------------------------- | -------- | --------------------------------------------------------------- |
| `controls`     | `boolean \| MarkdownControls`                              | on       | Copy, download, wrap, expand, full-screen, and details toggles. |
| `translations` | `Partial<MarkdownTranslations>`                            | English  | Button labels and spoken announcements.                         |
| `icons`        | `Partial<Record<MarkdownControlIcon, MarkdownIconSource>>` | platform | Replace control glyphs with icon names or image sources.        |

A control key accepts `true`, `false`, or an object with its own callbacks. Missing keys mean on.

```tsx
<Downstream
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
>
  {text}
</Downstream>
```

### Styling

| Prop            | Type                        | What it does                                                  |
| --------------- | --------------------------- | ------------------------------------------------------------- |
| `theme`         | `Composable<MarkdownTheme>` | Colors, fonts, radius, syntax colors, and the font scale cap. |
| `markdownStyle` | `Composable<MarkdownStyle>` | Per-element styles. Wins over `theme`.                        |

Both accept one value or an array merged left to right. See [Theming](/docs/theming).

### Events

| Prop          | Type                                      | What it does                                              |
| ------------- | ----------------------------------------- | --------------------------------------------------------- |
| `onTagPress`  | `(event: MarkdownTagEvent) => void`       | A press on a tag `allowedTags` allows.                    |
| `onTaskPress` | `(event: MarkdownTaskPressEvent) => void` | A press on a task list checkbox. The app owns the source. |
| `onError`     | `(error: MarkdownErrorEvent) => void`     | Parse, render, control, and renderer failures.            |

## DownstreamProvider

```tsx
import { DownstreamProvider } from '@react-native-downstream/core';
```

Takes every view option except the source, and applies it to the views inside it. Views override providers, and nested providers override outer ones. See [Plugins & defaults](/docs/plugins).

## Feature plugins

| Import                                                 | Options                                | Requires                    |
| ------------------------------------------------------ | -------------------------------------- | --------------------------- |
| `import { code } from '@react-native-downstream/code'` | —                                      | The installed code package. |
| `import { math } from '@react-native-downstream/math'` | `{ singleDollarTextMath?, brackets? }` | The installed math package. |

`codeAvailable()` and `mathAvailable()` from the core package report whether each native engine is in the current binary.

A custom plugin is a `DownstreamPlugin`: a `name` and optional `parse`, `fences`, `tags`, and `style` fields.

## createMarkdownStream

```tsx
import { createMarkdownStream } from '@react-native-downstream/core';

const stream = createMarkdownStream({
  onError: (error) => console.warn(error.code, error.message),
});
```

| Option    | Type                                  | What it does                  |
| --------- | ------------------------------------- | ----------------------------- |
| `onError` | `(error: MarkdownErrorEvent) => void` | Failures for this controller. |

A controller carries only text. Parsing, preview, and plugin options belong to the view it binds to.

### Controller

| Method      | Signature                   | What it does                                                         |
| ----------- | --------------------------- | -------------------------------------------------------------------- |
| `append`    | `(chunk: string) => void`   | Adds the next chunk. Updates are batched by animation frame.         |
| `finish`    | `() => void`                | Marks the response complete.                                         |
| `reset`     | `(source?: string) => void` | Starts a new response, optionally with initial text.                 |
| `getSource` | `() => Promise<string>`     | Resolves with the exact source supplied.                             |
| `dispose`   | `() => void`                | Releases the controller. A disposed controller publishes no updates. |

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
  Downstream,
  DownstreamProvider,
  createMarkdownStream,
  presets,
  syntaxColorsFromTheme,
  codeAvailable,
  mathAvailable,
  resolveAllowedTags,
  resolveParseFlags,
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

Every public type is exported from the package root, including `DownstreamProps`, `DownstreamOptions`, `DownstreamProviderProps`, `DownstreamPlugin`, `DownstreamComponents`, `DownstreamCodeBlockOptions`, `DownstreamTableOptions`, `DownstreamImagesOptions`, `DownstreamLinksOptions`, `MarkdownStreamOptions`, `MarkdownStreamController`, `MarkdownTheme`, `MarkdownStyle`, `MarkdownControls`, `MarkdownErrorEvent`, `MarkdownLinkPressEvent`, `MarkdownTagEvent`, `MarkdownImageEvent`, `MarkdownMathErrorEvent`, `CustomRenderer`, and `CustomRendererProps`.

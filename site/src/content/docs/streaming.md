---
title: Streaming
description: Append chunks, preview unfinished Markdown, animate the reveal, and manage the controller lifecycle.
group: Guide
order: 2
---

A streaming response is an append-only document. `createMarkdownStream` gives you a controller that accepts chunks, batches updates by animation frame, and keeps completed blocks stable while the tail changes.

## Create a controller

```tsx
import { createMarkdownStream } from 'react-native-downstream';

const stream = createMarkdownStream({
  preview: true,
  onError: (error) => console.warn(error.code, error.message),
});
```

Create one controller per message, outside the component's render path, and keep it for that message's lifetime. Each controller binds to one view.

```tsx
<DownstreamMarkdown stream={stream} animated caret="block" />
```

## Controller methods

| Method                | What it does                                                                                      |
| --------------------- | ------------------------------------------------------------------------------------------------- |
| `append(chunk)`       | Adds the next text chunk. Updates are batched by animation frame.                                 |
| `finish()`            | Marks the response complete and renders the final Markdown.                                       |
| `reset(source?)`      | Starts a new response, optionally with initial text. Use before appending again after `finish()`. |
| `getSource()`         | Returns a promise with the exact source you supplied.                                             |
| `setPreview(options)` | Changes how incomplete Markdown is displayed.                                                     |
| `dispose()`           | Releases the controller when the message is no longer needed.                                     |

Call `dispose()` when the message is removed. A disposed controller no longer publishes updates.

## Make incomplete Markdown readable

By default, an unfinished span such as `**hello` shows its literal markers. `preview: true` applies provisional formatting while closing markers are still arriving. Enable only the previews you want:

```tsx
const stream = createMarkdownStream({
  preview: {
    emphasis: true,
    inlineCode: true,
    links: true,
    math: true,
  },
});
```

| Key          | Effect                                                                                                         |
| ------------ | -------------------------------------------------------------------------------------------------------------- |
| `emphasis`   | Closes an open `*`, `_` or `~~` run so its content shows styled.                                               |
| `inlineCode` | Closes an unclosed backtick run.                                                                               |
| `links`      | Shows an unfinished link as its text. The partial target is dropped, so nothing is tappable until `)` arrives. |
| `images`     | Shows an unfinished image as its alt text.                                                                     |
| `math`       | Closes an unfinished inline `$$…$$` span once the prefix lays out.                                             |
| `htmlTags`   | Hides a half-arrived allowlisted tag at the stream tail.                                                       |
| `tables`     | Hides a table until every row is complete.                                                                     |

Previews never change your source. `finish()` removes provisional formatting and renders the Markdown you actually sent.

## Animate arriving text

`animated` fades in new words. Pass an options object for character-by-character reveals, timing, or a slide animation:

```tsx
<DownstreamMarkdown
  stream={stream}
  animated={{
    animation: 'slideUp',
    sep: 'char',
    duration: 200,
    stagger: 20,
    maxBacklogMs: 320,
  }}
  caret="circle"
/>
```

| Option         | Default  | Notes                                                                 |
| -------------- | -------- | --------------------------------------------------------------------- |
| `animation`    | `fadeIn` | Also `slideUp`, `blurIn`, or `none`. `blurIn` falls back to `fadeIn`. |
| `sep`          | `word`   | `char` reveals one character at a time.                               |
| `duration`     | `150`    | Milliseconds for one unit to reach full opacity.                      |
| `stagger`      | `40`     | Delay between units, in milliseconds.                                 |
| `maxBacklogMs` | `320`    | Caps how far a burst of text can delay the reveal.                    |
| `easing`       | `ease`   | Any supported easing name.                                            |

Animation changes drawing without reflowing text. A copy, a selection, an export, and a screen reader all see the whole message. Reduce Motion disables the reveal.

A stream tracks its own animation state. For a controlled `markdown` string, pass `isAnimating={isStreaming}`. `onAnimationStart` and `onAnimationEnd` report changes; the end callback fires when streaming stops, before any remaining fades finish.

## Caret

`caret="block"` or `caret="circle"` draws a caret after the newest text while animating. It takes the color of the last character, sits after the last prose block or inside an open fence, and moves to its own line after a table, closed fence, or rule. The caret never appears in copied or selected text.

## Parse options belong to the controller

For streams, set parsing options such as `math`, `syntax`, `links`, and `inlineHtml` when creating the controller. The view's `math` prop only decides whether formulas the stream already parsed are typeset.

```tsx
const stream = createMarkdownStream({
  math: { singleDollarTextMath: true },
  syntax: { superscript: true },
  links: { defaultOrigin: 'https://docs.example.com' },
});
```

## Lifecycle rules

- Call `finish()` once the response is complete.
- Call `reset()` before appending again after `finish()`.
- Call `dispose()` when the message is removed from the screen.
- A controller binds to one view; do not render the same controller in two places.
- A rejected synchronous operation throws a `MarkdownOperationError` with a `code`.

<div class="callout callout-blue">
  <strong>Frame coalescing.</strong>
  <p>Updates from all streams share one animation frame and one React Native batch. Native publication is capped at one snapshot per session per display tick, and the cadence backs off under pressure.</p>
</div>

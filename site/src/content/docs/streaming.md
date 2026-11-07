---
title: Streaming
description: Stream a growing string or feed a controller, preview unfinished Markdown, animate the reveal, and manage the controller lifecycle.
group: Guide
order: 2
---

A streaming response is an append-only document. Downstream keeps completed blocks stable while the tail changes, and batches updates by animation frame. Give a view the growing string, or feed it chunks through a controller.

## Stream a growing string

Pass the text as children and set `isAnimating` while the response is arriving. Downstream owns the incremental session and appends only the added text. With the AI SDK's `useChat`:

```tsx
import { useChat } from '@ai-sdk/react';
import { Downstream } from '@react-native-downstream/core';

export function Chat() {
  const { messages, status } = useChat();
  return messages.map((message) =>
    message.parts.map((part, index) =>
      part.type === 'text' ? (
        <Downstream
          key={`${message.id}-${index}`}
          isAnimating={
            status === 'streaming' &&
            message.role === 'assistant' &&
            message.id === messages.at(-1)?.id
          }
          animated
          caret="block"
        >
          {part.text}
        </Downstream>
      ) : null
    )
  );
}
```

Setting `isAnimating` to `false` finishes the response and removes provisional formatting. A replaced string resets the session, and growth after finishing starts a new generation. Sessions are disposed when their views unmount. A message that mounts already settled is parsed once, without a session.

## Feed chunks through a controller

Create a controller when your transport emits chunks directly, or when a reply must keep arriving while its list row is unmounted:

```tsx
import {
  createMarkdownStream,
  Downstream,
} from '@react-native-downstream/core';

const stream = createMarkdownStream({
  onError: (error) => console.warn(error.code, error.message),
});

<Downstream stream={stream} animated caret="block" />;
```

Create one controller per message, outside the component's render path, and keep it for that message's lifetime. Each controller binds to one view. Text keeps accumulating while the view is unmounted.

## Controller methods

| Method           | What it does                                                                                      |
| ---------------- | ------------------------------------------------------------------------------------------------- |
| `append(chunk)`  | Adds the next text chunk. Updates are batched by animation frame.                                 |
| `finish()`       | Marks the response complete and renders the final Markdown.                                       |
| `reset(source?)` | Starts a new response, optionally with initial text. Use before appending again after `finish()`. |
| `getSource()`    | Returns a promise with the exact source you supplied.                                             |
| `dispose()`      | Releases the controller when the message is removed.                                              |

A disposed controller publishes no updates.

## Make incomplete Markdown readable

While a response is streaming, an unfinished span such as `**hello` is previewed with provisional formatting by default, whether the source is string children or a controller. Set `preview={false}` on the view to show literal markers, or enable only the previews you want:

```tsx
<Downstream
  stream={stream}
  preview={{
    emphasis: true,
    inlineCode: true,
    links: true,
    math: true,
  }}
/>
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

Previews never change your source. Finishing the response removes provisional formatting and renders the Markdown you actually sent. To change the policy mid-stream, update the `preview` prop.

## Animate arriving text

`animated` fades in new words. Pass an options object for character-by-character reveals, timing, or a slide animation:

```tsx
<Downstream
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

A stream tracks its own animation state. For string children, pass `isAnimating={isStreaming}`. `onAnimationStart` and `onAnimationEnd` report changes; the end callback fires when streaming stops, before any remaining fades finish.

## Caret

`caret="block"` or `caret="circle"` draws a caret after the newest text while animating. It takes the color of the last character, sits after the last prose block or inside an open fence, and moves to its own line after a table, closed fence, or rule. The caret never appears in copied or selected text.

## Parse options belong to the view

A controller carries only text. Parsing options such as `syntax`, `links`, and `inlineHtml` go on the view or on a [`DownstreamProvider`](/docs/plugins), and math delimiters go to the `math()` plugin:

```tsx
import { math } from '@react-native-downstream/math';

<Downstream
  stream={stream}
  plugins={[math({ singleDollarTextMath: true })]}
  syntax={{ superscript: true }}
  links={{ defaultOrigin: 'https://docs.example.com' }}
/>;
```

Changing these options on a bound stream rebuilds its accepted source under the new policy without losing text or its finished state.

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

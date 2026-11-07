---
title: Getting started
description: Install Downstream, rebuild your app natively, and render your first Markdown message.
group: Start here
order: 1
---

Downstream renders Markdown with native text on iOS and Android. Parsing and text preparation run on native worker threads, and a streaming response reuses its completed blocks instead of rebuilding the whole document for every chunk.

<div class="callout">
  <strong>Native build required.</strong>
  <p>Downstream uses React Native's New Architecture. Expo Go cannot load the native module — use a development build or a bare React Native app.</p>
</div>

## Requirements

- React Native with the New Architecture enabled.
- iOS 15+ or Android with the current React Native minimum SDK.
- A native rebuild after installing. JavaScript changes alone are not enough the first time.

## Install

```sh
pnpm add @react-native-downstream/core
```

Or with your app's package manager:

```sh
npm install @react-native-downstream/core
# or
yarn add @react-native-downstream/core
```

For a bare iOS app, install the pods and rebuild:

```sh
npx pod-install
```

For Expo, create a development build:

```sh
npx expo run:ios
# or
npx expo run:android
```

## Render a complete message

Pass a string when you already have the whole document:

```tsx
import { DownstreamMarkdown } from '@react-native-downstream/core';

export function Message({ text }: { text: string }) {
  return (
    <DownstreamMarkdown
      markdown={text}
      onError={(error) => console.warn(error.code, error.message)}
    />
  );
}
```

Links open only after a confirmation by default. Pass `onLinkPress` to handle navigation yourself.

## Stream a response

For an append-only response, create one controller per message and keep it for that message's lifetime. Create it outside the render path — in the action that starts the reply.

```tsx
import {
  createMarkdownStream,
  DownstreamMarkdown,
} from '@react-native-downstream/core';

const stream = createMarkdownStream({
  preview: true,
  onError: (error) => console.warn(error.code, error.message),
});

// Render this view while chunks arrive.
<DownstreamMarkdown stream={stream} animated caret="block" />;

// In your response handler:
stream.append('Hello ');
stream.append('**world**');
stream.finish();

// When the message is removed:
stream.dispose();
```

Each controller binds to one view. Pass either `markdown` or `stream`, never both.

## Where to go next

- [Streaming](/docs/streaming) — previews, animation, carets, and the controller lifecycle.
- [Code blocks](/docs/code-blocks) — highlighting, filenames, line numbers, and controls.
- [Theming](/docs/theming) — presets, tokens, and per-element styles.
- [Benchmarks](/benchmarks) — what the native path buys you on a real phone.

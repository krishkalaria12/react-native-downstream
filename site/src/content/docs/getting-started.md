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

## Render a message

Pass the Markdown as a string child. Update the string as text arrives, and Downstream appends only what was added:

```tsx
import { Downstream } from '@react-native-downstream/core';

export function Message({ text }: { text: string }) {
  return (
    <Downstream onError={(error) => console.warn(error.code, error.message)}>
      {text}
    </Downstream>
  );
}
```

Links open only after a confirmation by default. Pass `links={{ onPress }}` to handle navigation yourself.

## Stream a response

Pass the growing text as children and set `isAnimating` while the response is arriving. Setting it to `false` finishes the response and removes provisional formatting:

```tsx
<Downstream isAnimating={isStreaming} animated caret="block">
  {text}
</Downstream>
```

When your transport emits chunks directly, or a reply must keep arriving while its list row is unmounted, feed a controller instead. Create one per message, outside the render path, in the action that starts the reply:

```tsx
import {
  createMarkdownStream,
  Downstream,
} from '@react-native-downstream/core';

const stream = createMarkdownStream({
  onError: (error) => console.warn(error.code, error.message),
});

// Render this view while chunks arrive.
<Downstream stream={stream} animated caret="block" />;

// In your response handler:
stream.append('Hello ');
stream.append('**world**');
stream.finish();

// When the message is removed:
stream.dispose();
```

Each controller binds to one view. A view takes string children, a `markdown` string, or a `stream`, one at a time.

## Add code highlighting and math

Native syntax highlighting and math typesetting are optional packages. Install them, rebuild, and select them on a view or on a provider for the whole app:

```sh
pnpm add @react-native-downstream/code @react-native-downstream/math
npx pod-install
```

```tsx
import { DownstreamProvider } from '@react-native-downstream/core';
import { code } from '@react-native-downstream/code';
import { math } from '@react-native-downstream/math';

const plugins = [code(), math()];

<DownstreamProvider plugins={plugins}>
  <App />
</DownstreamProvider>;
```

## Where to go next

- [Streaming](/docs/streaming) — previews, animation, carets, and the controller lifecycle.
- [Code blocks](/docs/code-blocks) — highlighting, filenames, line numbers, and controls.
- [Plugins & defaults](/docs/plugins) — feature plugins and app-wide options with `DownstreamProvider`.
- [Theming](/docs/theming) — presets, tokens, and per-element styles.
- [Benchmarks](/benchmarks) — what the native path buys you on a real phone.

---
title: Troubleshooting
description: Fix the common native build, streaming, rendering, and performance problems.
group: Reference
order: 12
---

## The view renders nothing

Check the source first. Pass one of string children, `markdown`, or `stream`:

```tsx
// A string that may still be growing
<Downstream isAnimating={isStreaming}>{text}</Downstream>

// Chunks fed through a controller
<Downstream stream={stream} />
```

A stream renders nothing until the first `append()`. If you created the controller in a component body, it may be recreated on every render; move it into the action that starts the reply, or into a ref.

## The native module cannot be found

Downstream requires a native rebuild. JavaScript-only reloads are not enough after installing or upgrading.

```sh
# Bare React Native
npx pod-install
pnpm react-native run-ios
pnpm react-native run-android

# Expo
npx expo run:ios
npx expo run:android
```

Expo Go cannot load the module. Use a development build. Confirm the New Architecture is enabled in your app configuration.

If the build still fails, clear the native caches and rebuild:

```sh
rm -rf ios/Pods ios/build android/app/build android/build
pnpm install
npx pod-install
```

## Stream text never appears

A controller publishes on animation frames. Confirm three things:

1. The controller was created with `createMarkdownStream()` and is not disposed.
2. The same controller is not rendered in two views. A controller binds to one view.
3. You call `finish()` when the response ends.

```tsx
stream.append('Hello');
stream.append(' world');
stream.finish();
```

After `finish()`, call `reset()` before appending again. A disposed controller publishes no updates.

## Incomplete Markdown shows its markers

Streaming views preview unfinished spans such as `**hello` by default. Markers show when the view sets `preview={false}`, or passes a `preview` object that leaves that feature out:

```tsx
<Downstream
  stream={stream}
  preview={{
    emphasis: true,
    inlineCode: true,
    links: true,
  }}
/>
```

Previews never change your source. If a preview is enabled but the marker still shows, the repair was not proven safe for that block, or the block had already ended.

## Links do nothing

A press with no `links.onPress` opens the link after a confirmation. If nothing happens at all:

- `links={{ safety: false }}` without `onPress` means the library still opens it, without asking. If it does not, your app's URL handling may be intercepting the request.
- `links.onPress` replaces the built-in behavior entirely. Make sure your handler actually opens the URL.
- Relative links are off by default. Pass `links={{ relative: true }}` or `links={{ defaultOrigin: 'https://example.com' }}`.

```tsx
<Downstream
  links={{
    defaultOrigin: 'https://docs.example.com',
    onPress: ({ url }) => router.push(url),
  }}
>
  {text}
</Downstream>
```

## Math shows as source

Formulas are typeset only when `@react-native-downstream/math` is installed, the app has been rebuilt, and the view or its provider selects `math()`:

```sh
pnpm add @react-native-downstream/math
npx pod-install
```

```tsx
import { math } from '@react-native-downstream/math';

<Downstream plugins={[math()]}>{text}</Downstream>;
```

Development builds warn when the engine is installed but a view does not select `math()`. A view's own `plugins` array replaces its provider's, so include `math()` there too.

If formulas still show as source, check the delimiter. `$…$` is off by default; enable it explicitly with `math({ singleDollarTextMath: true })`.

`onMathError` reports formulas the engine could not typeset, once per distinct source per view.

## Images do not load

Images require HTTPS and must match `images.allowedPrefixes`. The default is `['*']`, which allows any HTTPS host.

```tsx
<Downstream images={{ allowedPrefixes: ['https://cdn.example.com/'] }}>
  {text}
</Downstream>
```

An image inside a paragraph with other content renders as text rather than a block image. Only an image-only paragraph becomes a native image. `images={false}` shows alt text and makes no request.

## A code block is plain

Highlighting comes from `@react-native-downstream/code` and covers 84 languages. A block stays plain when:

- The code package is not installed, or the app was not rebuilt after installing it. `codeAvailable()` reports whether it is in the binary.
- The view or its provider does not select `code()`. A view's own `plugins` array replaces its provider's.
- Its info string is unknown, missing, or misspelled.
- The block exceeds 64 KiB of code.

Identifiers are case-insensitive, but only the first word of the info string counts. See [Code blocks](/docs/code-blocks) for the accepted metadata forms.

## Code or tables are cut off

Long code scrolls inside its card at `codeBlock.maxHeight` (400 points by default), and tables scroll at `table.maxHeight` (300). The expand control lifts the cap for one block.

```tsx
<Downstream codeBlock={{ maxHeight: 0 }} table={{ maxHeight: 'none' }}>
  {text}
</Downstream>
```

`0`, `'0'`, `'none'`, `Infinity`, and `'Infinity'` all remove a cap.

## Custom fences fall back to the native card

A renderer matches the normalized fence language, case-insensitively. Check that:

- `components.fences` has a key for the language.
- The component is defined outside the render path, or is stable across renders.
- The fence's first info word matches, before any `:` or metadata.

A component that throws reports `renderer_failed` and falls back to the native code card. Use `isIncomplete` to render a placeholder until the payload is complete.

## Selection, copy, or accessibility looks wrong

Selection is applied per settled segment, so text that is still streaming is not selectable. Pass `selectable={false}` if your app opens its own message menu on a long press.

Line numbers, carets, and control chrome are never copied, selected, exported, or read aloud. If copied text includes something extra, check that you are not copying the rendered string yourself instead of the source.

## Animation does not run

`animated` requires `isAnimating` for string children. A stream tracks its own state. Reduce Motion disables the reveal, and Android's animator scale of 0 does the same.

`blurIn` falls back to `fadeIn` on platforms that cannot draw a blur. This is expected and reported once in development.

## The app uses more memory than expected

Views that take string children dispose their sessions when they unmount. A stream controller holds its own document state until you dispose it, so dispose it where the message is removed:

```tsx
function removeMessage(message: Message) {
  message.reply.dispose();
  setMessages((messages) => messages.filter((m) => m !== message));
}
```

Peak memory also depends on images, code highlighting, and how many documents are mounted at once. Compare against the [benchmarks](/benchmarks) before assuming a regression.

## Errors and codes

`onError` receives a `MarkdownErrorEvent`. A rejected synchronous stream operation throws `MarkdownOperationError` with a `code`.

| Code              | Likely cause                                                             |
| ----------------- | ------------------------------------------------------------------------ |
| `invalid_input`   | Empty or malformed source, or an option the engine rejected.             |
| `input_limit`     | The document is too large for one view.                                  |
| `invalid_state`   | `append()` after `finish()` without `reset()`, or use after `dispose()`. |
| `session_bound`   | The same controller is rendered in a second view.                        |
| `parse_failed`    | Native parse failure. Capture the source and report it.                  |
| `render_failed`   | Native render failure.                                                   |
| `control_failed`  | A copy, download, wrap, expand, or full-screen action failed.            |
| `renderer_failed` | A custom fence renderer threw.                                           |

```tsx
onError={(error) => {
  console.warn(error.code, error.message, error.recoverable);
}}
```

Include the code, the React Native version, the platform, and a minimal source string in a bug report.

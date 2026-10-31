---
title: Troubleshooting
description: Fix the common native build, streaming, rendering, and performance problems.
group: Reference
order: 11
---

## The view renders nothing

Check the content prop first. Pass either `markdown` or `stream`, never both:

```tsx
// Complete document
<DownstreamMarkdown markdown={text} />

// Live response
<DownstreamMarkdown stream={stream} />
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

This is the default. An unfinished span such as `**hello` renders literally until its closing marker arrives. Enable provisional styling when you want it repaired:

```tsx
const stream = createMarkdownStream({
  preview: {
    emphasis: true,
    inlineCode: true,
    links: true,
  },
});
```

Previews never change your source. If a preview is enabled but the marker still shows, the repair was not proven safe for that block, or the block had already ended.

## Links do nothing

A press with no `onLinkPress` opens the link after a confirmation. If nothing happens at all:

- `linkSafety={false}` without an `onLinkPress` means the library still opens it, without asking. If it does not, your app's URL handling may be intercepting the request.
- An `onLinkPress` replaces the built-in behavior entirely. Make sure your handler actually opens the URL.
- Relative links are off by default. Pass `links={{ relative: true }}` or `links={{ defaultOrigin: 'https://example.com' }}`.

```tsx
<DownstreamMarkdown
  markdown={text}
  links={{ defaultOrigin: 'https://docs.example.com' }}
  onLinkPress={({ url }) => router.push(url)}
/>
```

## Math shows as source

Math is enabled only when `react-native-downstream-math` is installed and the app has been rebuilt.

```sh
pnpm add react-native-downstream-math
npx pod-install
```

If formulas still show as source, check the delimiter. `$…$` is off by default; enable it explicitly:

```tsx
<DownstreamMarkdown markdown={text} math={{ singleDollarTextMath: true }} />
```

For a stream, set math options when creating the controller. The view's `math` prop only decides whether already-parsed formulas are typeset.

`onMathError` reports formulas the engine could not typeset, once per distinct source per view.

## Images do not load

Images require HTTPS and must match `allowedImagePrefixes`. The default is `['*']`, which allows any HTTPS host.

```tsx
<DownstreamMarkdown
  markdown={text}
  allowedImagePrefixes={['https://cdn.example.com/']}
/>
```

An image inside a paragraph with other content renders as text rather than a block image. Only an image-only paragraph becomes a native image. `images={false}` shows alt text and makes no request.

## A code block is plain

Highlighting covers 84 languages. A block stays plain when:

- Its info string is unknown, missing, or misspelled.
- The block exceeds 64 KiB of code.
- `syntaxHighlighting={false}` is set.

Identifiers are case-insensitive, but only the first word of the info string counts. See [Code blocks](/docs/code-blocks) for the accepted metadata forms.

## Code or tables are cut off

Long code scrolls inside its card at `codeBlockMaxHeight` (400 points by default), and tables scroll at `tableMaxHeight` (300). The expand control lifts the cap for one block.

```tsx
<DownstreamMarkdown
  markdown={text}
  codeBlockMaxHeight={0}
  tableMaxHeight="none"
/>
```

`0`, `'0'`, `'none'`, `Infinity`, and `'Infinity'` all remove a cap.

## Custom fences fall back to the native card

A renderer matches the normalized fence language, case-insensitively. Check that:

- `plugins.renderers` includes the language.
- The component is defined outside the render path, or is stable across renders.
- The fence's first info word matches, before any `:` or metadata.

A component that throws reports `renderer_failed` and falls back to the native code card. Use `isIncomplete` to render a placeholder until the payload is complete.

## Selection, copy, or accessibility looks wrong

Selection is applied per settled segment, so text that is still streaming is not selectable. Pass `selectable={false}` if your app opens its own message menu on a long press.

Line numbers, carets, and control chrome are never copied, selected, exported, or read aloud. If copied text includes something extra, check that you are not copying the rendered string yourself instead of the source.

## Animation does not run

`animated` requires `isAnimating` for a controlled string. A stream tracks its own state. Reduce Motion disables the reveal, and Android's animator scale of 0 does the same.

`blurIn` falls back to `fadeIn` on platforms that cannot draw a blur. This is expected and reported once in development.

## The app uses more memory than expected

Each stream controller holds its own document state. Dispose controllers when messages leave the screen:

```tsx
useEffect(() => {
  return () => stream.dispose();
}, [stream]);
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

---
title: Code blocks
description: Syntax highlighting, filenames, line numbers, wrapping, copy, download, and expansion.
group: Guide
order: 4
---

Fenced code has a header with copy, download, and wrap controls. Long blocks scroll within their card and can be expanded. Add the `code()` plugin for native syntax highlighting:

```sh
pnpm add @react-native-downstream/code
# For a bare iOS app:
npx pod-install
```

```tsx
import { Downstream } from '@react-native-downstream/core';
import { code } from '@react-native-downstream/code';

<Downstream
  plugins={[code()]}
  codeBlock={{ lineNumbers: true, wrap: false, maxHeight: 400 }}
>
  {text}
</Downstream>;
```

Rebuild your native app after installing the package. Without it, or without `code()` in the view's plugins, blocks render plain with the same layout and controls.

Line numbers are on by default. Wrapping is off, and the default height cap is 400 points. Set `codeBlock={{ maxHeight: 0 }}` for full-height code.

Line numbers are drawn beside the code and never become part of copied or selected text.

## Fence metadata

Name a file and choose the first line number in the fence metadata:

````md
```ts title="src/api.ts" startLine=10
export async function load(url: string) {
  const response = await fetch(url);
  return response.json();
}
```
````

Downstream also accepts:

| Form                 | Example                         |
| -------------------- | ------------------------------- |
| `filename=`          | ` ```ts filename="src/app.ts" ` |
| `lang:path`          | ` ```js:src/app.js `            |
| `showLineNumbers=N`  | ` ```ts showLineNumbers=10 `    |
| `showLineNumbers{N}` | ` ```ts showLineNumbers{10} `   |
| `noLineNumbers`      | ` ```ts noLineNumbers `         |

Only the first word of the info string counts, so `TypeScript title="app.ts"` and `js:src/app.js` both resolve.

## Languages

The code plugin colors 84 languages natively, including languages embedded in others: script and style blocks in HTML, Vue, Svelte, and Astro, and fenced code inside Markdown and MDX.

Identifiers are case-insensitive. Blocks with an unknown language, no language, or more than 64 KiB of code render as plain code.

| Language   | Identifiers                             |
| ---------- | --------------------------------------- |
| Bash       | `bash`, `console`, `sh`, `shell`, `zsh` |
| C          | `c`, `h`                                |
| C++        | `c++`, `cc`, `cpp`, `cxx`, `hpp`        |
| CSS        | `css`                                   |
| Dart       | `dart`                                  |
| Go         | `go`, `golang`                          |
| HTML       | `htm`, `html`                           |
| Java       | `java`                                  |
| JavaScript | `js`, `jsx`, `mjs`, `cjs`               |
| JSON       | `json`, `jsonc`                         |
| Kotlin     | `kotlin`, `kt`, `kts`                   |
| Markdown   | `markdown`, `md`                        |
| PHP        | `php`                                   |
| Python     | `py`, `python`, `python3`               |
| Ruby       | `rb`, `ruby`                            |
| Rust       | `rs`, `rust`                            |
| SQL        | `sql`, `mysql`, `postgres`, `sqlite`    |
| Swift      | `swift`                                 |
| TSX        | `tsx`                                   |
| TypeScript | `ts`, `typescript`, `mts`, `cts`        |
| Vue        | `vue`                                   |
| YAML       | `yaml`, `yml`                           |

Remove `code()` from a view's plugins to render its code plain. `codeAvailable()` reports whether the native highlighter is installed in the current binary.

## Controls

Configure code controls individually and listen for completed actions:

```tsx
<Downstream
  controls={{
    code: {
      copy: { onCopy: (event) => console.log('Copied', event.language) },
      download: { filename: 'snippet' },
      wrap: true,
      expand: true,
    },
  }}
>
  {text}
</Downstream>
```

Omitted controls stay enabled. Use `controls={false}` to hide them all, or `controls={{ code: false }}` to hide only code controls.

Buttons become available when their own block settles, even if the rest of the response is still streaming. Downloads use the platform's save dialog, with a language-appropriate file extension.

## Custom fence renderers

To replace the code card for a language, map it to a component. See [Custom renderers](/docs/custom-renderers).

```tsx
<Downstream components={{ fences: { 'answer-card': AnswerCard } }}>
  {text}
</Downstream>
```

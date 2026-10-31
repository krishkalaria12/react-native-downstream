---
title: Code blocks
description: Syntax highlighting, filenames, line numbers, wrapping, copy, download, and expansion.
group: Guide
order: 3
---

Fenced code gets syntax highlighting and a header with copy, download, and wrap controls. Long blocks scroll within their card and can be expanded.

```tsx
<DownstreamMarkdown
  markdown={text}
  syntaxHighlighting
  lineNumbers
  codeBlockWrap={false}
  codeBlockMaxHeight={400}
/>
```

Highlighting and line numbers are on by default. Wrapping is off, and the default height cap is 400 points. Set `codeBlockMaxHeight={0}` for full-height code.

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

| Form | Example |
| --- | --- |
| `filename=` | ` ```ts filename="src/app.ts" ` |
| `lang:path` | ` ```js:src/app.js ` |
| `showLineNumbers=N` | ` ```ts showLineNumbers=10 ` |
| `showLineNumbers{N}` | ` ```ts showLineNumbers{10} ` |
| `noLineNumbers` | ` ```ts noLineNumbers ` |

Only the first word of the info string counts, so `TypeScript title="app.ts"` and `js:src/app.js` both resolve.

## Languages

Code is colored natively in 84 languages, including languages embedded in others: script and style blocks in HTML, Vue, Svelte, and Astro, and fenced code inside Markdown and MDX.

Identifiers are case-insensitive. Blocks with an unknown language, no language, or more than 64 KiB of code render as plain code.

| Language | Identifiers |
| --- | --- |
| Bash | `bash`, `console`, `sh`, `shell`, `zsh` |
| C | `c`, `h` |
| C++ | `c++`, `cc`, `cpp`, `cxx`, `hpp` |
| CSS | `css` |
| Dart | `dart` |
| Go | `go`, `golang` |
| HTML | `htm`, `html` |
| Java | `java` |
| JavaScript | `js`, `jsx`, `mjs`, `cjs` |
| JSON | `json`, `jsonc` |
| Kotlin | `kotlin`, `kt`, `kts` |
| Markdown | `markdown`, `md` |
| PHP | `php` |
| Python | `py`, `python`, `python3` |
| Ruby | `rb`, `ruby` |
| Rust | `rs`, `rust` |
| SQL | `sql`, `mysql`, `postgres`, `sqlite` |
| Swift | `swift` |
| TSX | `tsx` |
| TypeScript | `ts`, `typescript`, `mts`, `cts` |
| Vue | `vue` |
| YAML | `yaml`, `yml` |

Set `syntaxHighlighting={false}` to disable coloring throughout the view.

## Controls

Configure code controls individually and listen for completed actions:

```tsx
<DownstreamMarkdown
  markdown={text}
  controls={{
    code: {
      copy: { onCopy: (event) => console.log('Copied', event.language) },
      download: { filename: 'snippet' },
      wrap: true,
      expand: true,
    },
  }}
/>
```

Omitted controls stay enabled. Use `controls={false}` to hide them all, or `controls={{ code: false }}` to hide only code controls.

Buttons become available when their own block settles, even if the rest of the response is still streaming. Downloads use the platform's save dialog, with a language-appropriate file extension.

## Custom fence renderers

To replace the code card for a language, register a renderer. See [Custom renderers](/docs/custom-renderers).

```tsx
<DownstreamMarkdown
  markdown={text}
  plugins={{
    renderers: [{ language: 'answer-card', component: AnswerCard }],
  }}
/>
```

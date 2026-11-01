# react-native-downstream

Native Markdown for React Native, built for streaming responses.

Downstream renders Markdown on iOS and Android with native text, code cards, tables, images, and optional math. Use it for an AI chat reply that arrives token by token, a documentation screen, or a complete Markdown message.

Parsing and text preparation run on native worker threads. Streaming updates reuse completed blocks and update the unfinished tail, so your app can keep receiving text without rebuilding the entire document for every chunk.

## What you get

- GitHub-flavored Markdown with headings, nested lists, task lists, tables, blockquotes, strikethrough, footnotes, and alerts.
- Incremental streaming from a growing string, with previews for incomplete Markdown and an optional stream controller.
- Code with line numbers, filenames, copy, download, wrap, and expand controls, plus optional native syntax highlighting.
- Scrollable tables with copy and export formats, an expand button, and a full-screen view.
- Light and dark themes, ready-made presets, custom fonts, and styles for individual Markdown elements.
- Word or character reveal animations and a streaming caret that respect Reduce Motion.
- HTTPS images, optional native math typesetting, collapsible details, and your own React components inside fenced blocks or custom tags.
- Native text selection, accessibility support, CJK-friendly parsing, and automatic or explicit text direction.

## Video and real-device benchmarks

The [phone benchmark report](benchmarks/phone/REPORT.md) compares six streaming Markdown libraries on a OnePlus Nord CE 3 running Android 15. The [project site](site/README.md) turns the same numbers into charts, alongside the landing page and documentation.

Use the [API and plugin benchmark](benchmarks/phone/API_BENCHMARK.md) to compare the growing-string and controller APIs with explicit code/math plugin profiles on an Android phone.

To work on the site locally:

```sh
pnpm --filter react-native-downstream-site dev
```

## Install

```sh
pnpm add react-native-downstream
```

Or use your app's package manager:

```sh
npm install react-native-downstream
# or
yarn add react-native-downstream
```

Downstream requires React Native's New Architecture and a native build on iOS or Android. For a bare React Native app, install iOS pods after adding the package, then rebuild your app:

```sh
npx pod-install
```

For Expo, use a development build:

```sh
npx expo run:ios
# or
npx expo run:android
```

Expo Go cannot load the native module.

## Render Markdown

Pass a complete string, or update the string as your app receives text:

```tsx
import { Downstream } from 'react-native-downstream';

export function Message({ text }: { text: string }) {
  return (
    <Downstream onError={(error) => console.warn(error.code, error.message)}>
      {text}
    </Downstream>
  );
}
```

Links work with a confirmation before opening by default. Pass `links={{ onPress }}` to handle navigation yourself. The `markdown={text}` prop remains supported as an alternative to string children.

## Stream a response

Pass the growing text as children and set `isAnimating` while the response is arriving. Downstream owns the incremental session and appends only the added text. With the AI SDK's `useChat`:

```tsx
import { useChat } from '@ai-sdk/react';
import { Downstream } from 'react-native-downstream';

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

Incomplete Markdown is previewed by default while streaming. Set `preview={false}` to show literal markers, or pass an object to enable selected preview features. Setting `isAnimating` to false finishes the response and removes provisional formatting. Replacements reset the session; growth after finish starts a new generation. Sessions are disposed when their views unmount. Messages that mount already settled use a one-shot parse without creating sessions.

### Advanced: feed chunks without React updates

Create one controller per message when your transport emits chunks directly, or when a reply must continue while its list row is unmounted:

```tsx
import { createMarkdownStream, Downstream } from 'react-native-downstream';

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

Create the controller outside the component's render path, such as in the action that starts a reply. Each controller binds to one view. A controller carries text; parse and preview options belong on the view or provider. The view creates the native session when it binds, and changing its parse options rebuilds the same source under the new policy. Text can continue accumulating while a list row is unmounted.

| Method           | What it does                                                                                      |
| ---------------- | ------------------------------------------------------------------------------------------------- |
| `append(chunk)`  | Adds the next text chunk. Updates are batched by animation frame.                                 |
| `finish()`       | Marks the response complete and renders the final Markdown.                                       |
| `reset(source?)` | Starts a new response, optionally with initial text. Use before appending again after `finish()`. |
| `getSource()`    | Returns a promise with the exact source you supplied.                                             |
| `dispose()`      | Releases the controller when the message is no longer needed.                                     |

### Make incomplete Markdown readable

A streaming view previews unfinished spans such as `**hello` by default, whether its source is string children or a controller. Set `preview={false}` for literal markers, or enable selected features on the view:

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

The available preview keys are `emphasis`, `inlineCode`, `links`, `images`, `math`, `htmlTags`, and `tables`. Table preview hides an unfinished table until it is complete. An incomplete link can display its label, but becomes tappable only when its destination is complete.

Previews never change your source. `finish()` removes provisional formatting and renders the Markdown you actually sent.

### Animate arriving text

`animated` fades in new words. Use an options object for character-by-character reveals, timing, or a slide animation:

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

The defaults are `fadeIn`, word separation, a 150 ms duration, a 40 ms stagger, and `ease` easing. `maxBacklogMs` limits how far a burst of text can delay the reveal. `blurIn` falls back to `fadeIn`.

A stream tracks its own animation state. For string children or a `markdown` prop, pass `isAnimating={isStreaming}`. `onAnimationStart` and `onAnimationEnd` report changes to that state. The end callback fires when streaming stops, before any remaining fades finish.

Animation changes drawing without reflowing the text. Reduce Motion disables the reveal, and the caret never appears in copied or selected text.

## Feature plugins

The core renders plain code blocks and includes tables, images, controls, link safety, CJK parsing, and the extra inline syntaxes. Native code highlighting and math typesetting are optional packages:

```sh
pnpm add react-native-downstream-code react-native-downstream-math
# For a bare iOS app:
npx pod-install
```

Rebuild the native app after installing either package. Select the features on a view or provider:

```tsx
import { Downstream } from 'react-native-downstream';
import { code } from 'react-native-downstream-code';
import { math } from 'react-native-downstream-math';

const plugins = [code(), math({ singleDollarTextMath: true })];

<Downstream plugins={plugins} isAnimating={streaming}>
  {text}
</Downstream>;
```

Installing a package links its engine but does not activate it on a view. Omit a view's `plugins` to inherit the provider selection. An explicit array replaces that selection, so `plugins={[]}` opts out of all feature plugins and `plugins={[math()]}` selects only math.

Plugins are named objects with optional `parse`, `native`, `fences`, `tags`, and `style` fields. The resolver applies them in array order; later entries override earlier defaults. Duplicate names are an error in development. App component maps and provider/view styles override plugin defaults. A plugin can supply `style.theme` and `style.markdownStyle`, or register a fence/tag component without adding a native engine. See [`DownstreamPlugin`](src/plugins.ts) for the complete contract.

## Code blocks

Fenced code has a header with copy, download, and wrap controls. Long blocks scroll within their card and can be expanded. Add `code()` for native syntax highlighting:

```tsx
import { code } from 'react-native-downstream-code';

<Downstream
  plugins={[code()]}
  codeBlock={{ lineNumbers: true, wrap: false, maxHeight: 400 }}
>
  {text}
</Downstream>;
```

Without the code package or its plugin, blocks render plain with the same layout and controls. Line numbers are on by default. Wrapping is off, and the default height cap is 400 points. Set `codeBlock={{ maxHeight: 0 }}` for full-height code. Line numbers never become part of copied code.

Name a file and choose the first line number in the fence metadata:

````md
```ts title="src/api.ts" startLine=10
export async function load(url: string) {
  const response = await fetch(url);
  return response.json();
}
```
````

Downstream also accepts `filename=`, `lang:path`, `showLineNumbers=N`, `showLineNumbers{N}`, and `noLineNumbers` for individual blocks.

The code plugin colors 84 languages natively, including languages embedded in others: script and style blocks in HTML, Vue, Svelte, and Astro, and fenced code inside Markdown and MDX. Identifiers are case-insensitive, and only the first word of the info string counts, so the info strings `TypeScript title="app.ts"` and `js:src/app.js` both resolve. Blocks with an unknown language, no language, or more than 64 KiB of code render as plain code. Remove `code()` from the selected plugin array to disable coloring. `codeAvailable()` reports whether its native engine is installed in the current binary.

<details>
<summary>Languages and identifiers</summary>

| Language             | Identifiers                                                                           |
| -------------------- | ------------------------------------------------------------------------------------- |
| Angular HTML         | `angular-html`                                                                        |
| Assembly             | `asm`, `assembly`, `s`                                                                |
| Astro                | `astro`                                                                               |
| Bash                 | `bash`, `console`, `sh`, `shell`, `shellscript`, `shellsession`, `zsh`                |
| Batch                | `bat`, `batch`, `batchfile`, `cmd`, `dos`                                             |
| C                    | `c`, `h`                                                                              |
| C#                   | `c#`, `cs`, `csharp`                                                                  |
| C++                  | `c++`, `cc`, `cpp`, `cxx`, `h++`, `hh`, `hpp`, `hxx`                                  |
| C3                   | `c3`                                                                                  |
| Clojure              | `clj`, `cljc`, `cljs`, `clojure`, `edn`                                               |
| CMake                | `cmake`                                                                               |
| CSS                  | `css`                                                                                 |
| CSV                  | `csv`                                                                                 |
| CUDA                 | `cu`, `cuda`, `cuh`                                                                   |
| Dart                 | `dart`                                                                                |
| Diff                 | `diff`, `git-commit`, `git-rebase`, `patch`                                           |
| Dockerfile           | `containerfile`, `docker`, `dockerfile`                                               |
| Elixir               | `elixir`, `ex`, `exs`                                                                 |
| Elm                  | `elm`                                                                                 |
| Erlang               | `erl`, `erlang`, `hrl`                                                                |
| F#                   | `f#`, `fs`, `fsharp`, `fsi`, `fsx`                                                    |
| Fortran              | `f03`, `f08`, `f90`, `f95`, `fortran`, `fortran-free-form`                            |
| Fortran (fixed form) | `f`, `f77`, `for`, `fortran-fixed-form`                                               |
| GDScript             | `gd`, `gdscript`                                                                      |
| Gleam                | `gleam`                                                                               |
| GLSL                 | `comp`, `frag`, `geom`, `glsl`, `vert`                                                |
| Go                   | `go`, `golang`                                                                        |
| Godot resource       | `gdresource`, `godot-resource`, `tres`, `tscn`                                        |
| Godot shader         | `gdshader`, `gdshaderinc`, `godot-shader`                                             |
| GraphQL              | `gql`, `graphql`                                                                      |
| Groovy               | `gradle`, `groovy`, `gsh`, `gvy`, `gy`                                                |
| GROQ                 | `groq`                                                                                |
| Haskell              | `haskell`, `hs`                                                                       |
| HLSL                 | `hlsl`                                                                                |
| HTML                 | `htm`, `html`                                                                         |
| Java                 | `java`                                                                                |
| JavaScript           | `cjs`, `javascript`, `js`, `mjs`                                                      |
| JSON                 | `json`, `jsonc`                                                                       |
| JSON Lines           | `jsonl`                                                                               |
| JSON5                | `json5`                                                                               |
| JSX                  | `jsx`                                                                                 |
| Julia                | `jl`, `julia`                                                                         |
| Kotlin               | `kotlin`, `kt`, `kts`                                                                 |
| LaTeX                | `latex`, `tex`                                                                        |
| Less                 | `less`                                                                                |
| Lisp                 | `cl`, `el`, `elisp`, `emacs-lisp`, `lisp`, `lsp`, `scheme`, `scm`                     |
| Lua                  | `lua`                                                                                 |
| Makefile             | `make`, `makefile`, `mk`                                                              |
| Markdown             | `markdown`, `md`                                                                      |
| MATLAB               | `matlab`, `octave`                                                                    |
| MDX                  | `mdx`                                                                                 |
| Nim                  | `nim`                                                                                 |
| Nix                  | `nix`                                                                                 |
| Objective-C          | `m`, `mm`, `obj-c`, `objc`, `objcpp`, `objective-c`, `objective-cpp`, `objectivec`    |
| OCaml                | `ml`, `mli`, `ocaml`                                                                  |
| Odin                 | `odin`                                                                                |
| Pascal               | `delphi`, `dpk`, `dpr`, `lpr`, `object-pascal`, `objectpascal`, `pas`, `pascal`, `pp` |
| Perl                 | `perl`, `pl`, `pm`                                                                    |
| PHP                  | `php`                                                                                 |
| PowerShell           | `powershell`, `ps`, `ps1`, `psd1`, `psm1`, `pwsh`                                     |
| Protocol Buffers     | `proto`, `protobuf`                                                                   |
| Python               | `py`, `python`, `python3`                                                             |
| R                    | `r`, `rscript`                                                                        |
| Ruby                 | `rb`, `ruby`                                                                          |
| Rust                 | `rs`, `rust`                                                                          |
| Sass                 | `sass`                                                                                |
| Scala                | `sbt`, `sc`, `scala`                                                                  |
| SCSS                 | `scss`                                                                                |
| Solidity             | `sol`, `solidity`                                                                     |
| SQL                  | `mysql`, `postgres`, `postgresql`, `sql`, `sqlite`                                    |
| Svelte               | `svelte`                                                                              |
| Swift                | `swift`                                                                               |
| Terraform            | `hcl`, `terraform`, `tf`, `tfvars`                                                    |
| TOML                 | `toml`                                                                                |
| TSRX                 | `tsrx`                                                                                |
| TSV                  | `tsv`                                                                                 |
| TSX                  | `tsx`                                                                                 |
| TypeScript           | `angular-ts`, `cts`, `mts`, `ts`, `typescript`                                        |
| Vue                  | `vue`                                                                                 |
| WebAssembly text     | `wasm`, `wat`                                                                         |
| WGSL                 | `wgsl`                                                                                |
| XML                  | `svg`, `xml`, `xsd`                                                                   |
| YAML                 | `yaml`, `yml`                                                                         |
| Zig                  | `zig`, `zon`                                                                          |

</details>

## Tables, copy, and downloads

Tables scroll when they exceed their available width or height. Their toolbar lets readers copy, download, expand, or open a full-screen view. The default height cap is 300 points; `table={{ maxHeight: 0 }}` removes it.

Table copy offers Markdown, CSV, and TSV. It also includes HTML clipboard data so compatible spreadsheet apps can preserve the grid. Table downloads offer CSV and Markdown. Code copies as plain text and downloads with a language-appropriate extension. Downloads use the platform's save dialog.

Configure controls individually and listen for completed actions:

```tsx
<Downstream
  markdown={text}
  controls={{
    code: {
      copy: { onCopy: (event) => console.log('Copied', event.language) },
      download: { filename: 'snippet' },
      wrap: true,
      expand: true,
    },
    table: {
      csvSeparator: ';',
      fullscreen: true,
      download: false,
    },
    image: { download: false },
  }}
/>
```

Omitted controls stay enabled. Use `controls={false}` to hide them all, or `controls={{ code: false }}` to hide only code controls. Buttons become available when their own block settles, even if the rest of the response is still streaming.

`table.onFullscreen` reports a full-screen request. Use `table.renderFullscreen` to replace the default modal with your own React content. Customize button labels and accessibility announcements with `translations`, and replace glyphs with `icons` using platform icon names or bundled image sources.

## App-wide defaults

Set shared options once with `DownstreamProvider`:

```tsx
import { DownstreamProvider, presets } from 'react-native-downstream';
import { code } from 'react-native-downstream-code';
import { math } from 'react-native-downstream-math';

const plugins = [code(), math()];

<DownstreamProvider
  plugins={plugins}
  theme={presets.github.theme}
  markdownStyle={presets.github.markdownStyle}
  codeBlock={{ wrap: true }}
  links={{ defaultOrigin: 'https://docs.example.com' }}
>
  <App />
</DownstreamProvider>;
```

Props on an individual view override provider defaults. Nested providers override outer providers. Grouped options inherit unspecified subfields, and component maps inherit entries that the view does not replace. `theme` and `markdownStyle` merge provider-first, then view styles, in the same left-to-right order as their array forms. Plugin defaults go before both. A view or nested provider's explicit `plugins` array replaces the inherited selection.

## Match your app's design

Start with `presets.github` or `presets.streamdown`, then override what you need:

```tsx
import { Downstream, presets } from 'react-native-downstream';

<Downstream
  markdown={text}
  theme={[
    presets.github.theme,
    {
      colors: {
        foreground: { light: '#18202b', dark: '#edf2f7' },
        primary: { light: '#4f46e5', dark: '#a5b4fc' },
      },
      radius: 12,
    },
  ]}
  markdownStyle={[
    presets.github.markdownStyle,
    { h1: { fontSize: 28 }, paragraph: { fontSize: 16 } },
  ]}
/>;
```

`theme` controls document colors, fonts, corner radius, syntax colors, and the font scale cap. `markdownStyle` controls individual elements, including headings, paragraphs, lists, blockquotes, code, tables, links, alerts, images, and math.

Both accept a value or an array merged left to right. Element styles override theme tokens. Colors accept strings or `{ light, dark }` pairs; defaults follow the platform appearance. Custom fonts must already be available in your app. `theme.maxFontSizeMultiplier` caps Dynamic Type and Android font scaling.

For a VS Code or Shiki JSON theme, convert its token colors with `syntaxColorsFromTheme` and apply the result's `syntax` to `theme.syntax` and `codeBlock` to `markdownStyle.codeBlock`.

## Images and math

### Images

An image-only paragraph renders as a native image with a loading placeholder and alt text on failure. Images are enabled by default and load over HTTPS. Restrict their sources with `images.allowedPrefixes`, or use `images={false}` to show alt text without fetching images.

```tsx
<Downstream
  markdown={text}
  images={{
    allowedPrefixes: ['https://cdn.example.com/'],
    onPress: ({ url }) => console.log('Image pressed', url),
  }}
/>
```

The default prefix list is `['*']`, allowing any HTTPS host. `markdownStyle.image` controls height, radius, margins, and placeholder color. Loaded images can also have a download control.

### Math

Install the optional typesetting package and rebuild your native app:

```sh
pnpm add react-native-downstream-math
# For a bare iOS app:
npx pod-install
```

Select `math()` on a view or provider to enable formulas. Installing the package alone keeps them as source and produces a development warning when a view does not select the plugin. Configure delimiters through the factory:

```tsx
import { math } from 'react-native-downstream-math';

<Downstream
  markdown={text}
  plugins={[math({ singleDollarTextMath: true, brackets: true })]}
  onMathError={({ source, message }) => console.warn(source, message)}
/>;
```

Supported forms include `$$...$$`, display blocks delimited by `$$`, fenced `math` blocks, `\(...\)`, and `\[...\]`. Single-dollar `$...$` math is off by default so currency stays text. `brackets: false` disables the backslash forms.

Unsupported formulas, or formulas requested without an installed engine, display their source and report `onMathError`. During streaming, a formula keeps its last valid layout while more input arrives.

For both strings and controllers, select math through the plugin array and set other parsing options such as `syntax`, `links`, and `inlineHtml` on the view or provider. Changing them on a bound stream rebuilds its accepted source without losing text or its finished state. To keep code coloring when replacing a provider's plugin selection, include both `code()` and `math()` in the view's array.

## Custom renderers and tags

### Render a fence with your own component

Use `components.fences` to turn a fenced language into an app component. A chart, diagram, or interactive card can sit inside the same Markdown document.

```tsx
import { Text } from 'react-native';
import { Downstream, type CustomRendererProps } from 'react-native-downstream';

function AnswerCard({ code, isIncomplete }: CustomRendererProps) {
  return <Text>{isIncomplete ? 'Receiving answer...' : code}</Text>;
}

<Downstream
  markdown={text}
  components={{
    fences: { 'answer-card': AnswerCard },
  }}
/>;
```

A fence beginning with ` ```answer-card ` now renders `AnswerCard`. Each renderer receives `code`, `language`, `meta`, and `isIncomplete`. Language matching is case-insensitive. To register aliases, map each language key to the same component.

Use `isIncomplete` to show a placeholder until a structured payload is complete. An unloaded or failed renderer falls back to the native code card. A settled renderer failure reports `renderer_failed` through `onError`. You supply the rendering logic; registering a language does not execute its contents.

### Style inline tags or replace block tags

Allow your app's tags and choose the attributes they can carry:

```tsx
<Downstream
  markdown={'Hello <user id="42">Sam</user>.'}
  allowedTags={{ user: ['id'] }}
  markdownStyle={{ tags: { user: { color: '#4f46e5', fontWeight: 700 } } }}
  onTagPress={({ attributes }) => console.log('User', attributes.id)}
/>
```

Inline tags retain their text and use `markdownStyle.tags`. For a block tag placed on its own lines with blank lines around it, register `components={{ tags: { user: UserCard } }}`. Its component receives `name`, allowed `attributes`, inner `markdown`, and `isIncomplete`.

Without a custom block renderer, Downstream draws the tag's children natively. `components.fallback` provides a shared replacement for unmatched fences or allowed block tags. `literalTagContent` keeps tag contents as unstyled text.

Built-in `<details>` and `<summary>` provide collapsible sections. `controls.details.expand` configures their toggle.

## Links, interaction, and text options

Link presses show a confirmation by default. `links.safety` can supply an asynchronous `onLinkCheck` or a custom `renderModal`. `links={{ safety: false }}` opens links directly. With `links.onPress`, your app handles navigation and the built-in confirmation does not run:

```tsx
import { Linking } from 'react-native';

<Downstream
  markdown={text}
  links={{
    defaultOrigin: 'https://docs.example.com',
    onPress: ({ url }) => {
      Linking.openURL(url).catch(console.warn);
    },
  }}
/>;
```

`links.defaultOrigin` enables and resolves relative links. `links.relative` enables them for your own router without a base URL. Events include the resolved `url`, original `href`, and source range. Style links by URL prefix with `markdownStyle.linkVariants`, and handle holds with `links.onLongPress`. iOS link previews are off by default; `links={{ preview: true }}` enables them.

| Option                           | Behavior and default                                                                                                                   |
| -------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- |
| `selectable`                     | Native selection for settled text. On by default; unfinished text is not selectable. Set `false` for an app-owned message menu.        |
| `onTaskPress`                    | Reports a task checkbox press. Update your Markdown source to change its checked state.                                                |
| `dir`                            | `auto`, `ltr`, or `rtl`. Defaults to automatic direction per block.                                                                    |
| `cjk`                            | CJK-friendly emphasis, strikethrough, and autolinks. On by default.                                                                    |
| `inlineHtml`                     | Supports `<br>`, `<sub>`, and `<sup>`. On by default.                                                                                  |
| `syntax`                         | Optional superscript, subscript, highlight, underline, and spoiler syntax. All off by default.                                         |
| `links.disableAutolinkProtocols` | Disables automatic links for selected `http`, `https`, or `mailto` protocols.                                                          |
| `normalizeHtmlIndentation`       | Normalizes indentation for documents that start with an HTML block. Off by default.                                                    |
| `androidParagraphLayout`         | Defaults to `canonical`. `incremental` reuses completed lines in eligible streaming ASCII paragraphs; text can reflow when it settles. |

For the complete TypeScript definitions, see [`src/downstreamOptions.ts`](src/downstreamOptions.ts) and [`src/types.ts`](src/types.ts).

## API migration

The API below is intended for 0.2. Flat view props and the former component name remain compatible aliases during 0.2.x, with one development warning per alias. `markdown` remains supported without a warning. The controller now carries only text and an optional `onError` handler. The former `math` and `syntaxHighlighting` props have been removed in favor of explicit feature plugins.

| Previous API                                                                                                               | Current API                                                                                                               |
| -------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------- |
| `DownstreamMarkdown`                                                                                                       | `Downstream`                                                                                                              |
| `markdown={text}`                                                                                                          | `{text}` as children, or keep `markdown={text}`                                                                           |
| `codeBlockWrap`, `lineNumbers`, `codeBlockMaxHeight`                                                                       | `codeBlock={{ wrap, lineNumbers, maxHeight }}`                                                                            |
| `tableMaxHeight`, `onTableFullscreen`, `renderTableFullscreen`                                                             | `table={{ maxHeight, onFullscreen, renderFullscreen }}`                                                                   |
| `allowedImagePrefixes`, `onImagePress`                                                                                     | `images={{ allowedPrefixes, onPress }}`; boolean `images` still works                                                     |
| `linkSafety`, `onLinkPress`, `onLinkLongPress`, `linkPreview`, `disableAutolinkProtocols`                                  | `links={{ safety, onPress, onLongPress, preview, disableAutolinkProtocols }}`                                             |
| `plugins.renderers: [{ language, component }]`                                                                             | `components.fences: { [language]: component }`                                                                            |
| `plugins.tags: [{ name, component }]`                                                                                      | `components.tags: { [name]: component }`                                                                                  |
| `plugins.fallbackComponent`                                                                                                | `components.fallback`                                                                                                     |
| `createMarkdownStream({ math, syntax, links, inlineHtml, cjk, allowedTags, literalTagContent, disableAutolinkProtocols })` | Math options go to `math(options)`; other parse options go on the view or provider, with autolink protocols under `links` |
| `createMarkdownStream({ preview })`, `stream.setPreview(preview)`                                                          | The view or provider's `preview` prop; update the prop to change policy                                                   |
| Repeated options on every view                                                                                             | Shared defaults on `DownstreamProvider`                                                                                   |
| Built-in highlighting or `syntaxHighlighting`                                                                              | Install `react-native-downstream-code` and select `plugins={[code()]}`                                                    |
| Math enabled by installation or `math={options}`                                                                           | Select `plugins={[math(options)]}` from `react-native-downstream-math`                                                    |

Other view props keep their names: `style`, `selectable`, `androidParagraphLayout`, `controls`, `translations`, `theme`, `markdownStyle`, `onMathError`, `inlineHtml`, `syntax`, `cjk`, `links.relative`, `links.defaultOrigin`, `allowedTags`, `literalTagContent`, `normalizeHtmlIndentation`, `dir`, `onTagPress`, `onTaskPress`, `icons`, `animated`, `isAnimating`, `preview`, `caret`, `onAnimationStart`, `onAnimationEnd`, and `onError`. The controller's `append`, `reset`, `finish`, `getSource`, and `dispose` methods remain available.

`theme.syntax` and `syntaxColorsFromTheme` remain in core, so existing palettes continue to style code selected by `code()`. Plain code-block layout and controls do not require a plugin.

## Fork and run locally

Fork this repository on GitHub, then clone your fork. Use Node.js from [`.nvmrc`](.nvmrc) and the pnpm version declared in [`package.json`](package.json).

```sh
git clone https://github.com/YOUR_USERNAME/react-native-downstream.git
cd react-native-downstream
corepack enable
pnpm install

# Prepare the example app's optional math framework and fonts.
node math/scripts/vendor-ratex.mjs
```

The example app uses the workspace library directly. Run it with Xcode and CocoaPods on iOS, or JDK 17 and the Android SDK/NDK on Android:

```sh
pnpm example ios
# or
pnpm example android
```

To restart Metro for an existing development build:

```sh
pnpm example start
```

The example includes live chat, controls, custom renderers, animation, comparison, benchmark, and stress screens. Only the live chat screen needs an API key:

```sh
cp example/.env.example example/.env
# Set EXPO_PUBLIC_OPENAI_API_KEY in example/.env.
pnpm example start --clear
```

The example embeds that key in its bundle. Use a development key and do not distribute a build containing it.

JavaScript changes appear through Metro; native changes require rebuilding the app. Before sending a change, run the relevant checks from the repository root:

```sh
pnpm lint
pnpm typecheck
pnpm test
pnpm test:native
pnpm prepare
```

### Use your fork in another app

Build the library in your fork:

```sh
pnpm prepare
```

Then install it from your app directory using the path to your local checkout:

```sh
pnpm add /absolute/path/to/react-native-downstream
# For a bare iOS app:
npx pod-install
```

Rebuild your native app after installing it. Depending on how your package manager links local dependencies, reinstall the package after updating the checkout. For source development, the workspace example provides the direct feedback loop.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the development workflow and pull request guidelines, and the [code of conduct](CODE_OF_CONDUCT.md) for community expectations.

## License

[MIT](LICENSE). Vendored dependencies retain their own licenses:

- [md4c](src/cpp/md4c/LICENSE.md) uses the BSD license.
- The optional code package's [syntax highlighting lexers](code/cpp/wasm/LICENSE.md) and [wasm2c runtime](code/cpp/wasm/wasm-rt/LICENSE) use Apache-2.0, and [SIMDe](code/cpp/wasm/simde/COPYING) uses MIT; their notices must accompany copies.
- The [language extension map](src/cpp/downstream/export-extensions.json) includes attribution to Streamdown under Apache-2.0.
- The optional math package includes the [RaTeX license](math/licenses/RaTeX-LICENSE.txt) and [font license notices](math/licenses/THIRD_PARTY_NOTICES.txt).

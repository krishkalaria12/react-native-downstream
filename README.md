# react-native-downstream

React Native Downstream

## Installation

```sh
pnpm add react-native-downstream
```

## Usage

```tsx
import { DownstreamMarkdown } from 'react-native-downstream';

<DownstreamMarkdown
  markdown={message}
  onLinkPress={({ url }) => Linking.openURL(url)}
  onError={(error) => console.warn(error.code, error.message)}
/>;
```

For an append-only response, keep one controller for the message and dispose it when the message is removed:

```tsx
import { createMarkdownStream, DownstreamMarkdown } from 'react-native-downstream';

const stream = createMarkdownStream();
stream.append('Hello ');
stream.append('**world**');
stream.finish();

<DownstreamMarkdown stream={stream} />;
// Later: stream.dispose();
```

The view parses with md4c and lays text out natively (TextKit on iOS, `StaticLayout` on Android). Parsing and text preparation run on a small shared pool of worker threads below the UI and JavaScript threads, so several messages render in parallel without blocking the app; see [threading](docs/api.md#threading). It needs the New Architecture and a development build; Expo Go cannot load it. The library never opens links itself.

An open span renders as the literal text the writer sent, so a half-arrived `**bold wor` shows its markers. Turn on provisional styling to show the content styled while the closer is still on its way:

```tsx
const stream = createMarkdownStream({ preview: true });
// Or one feature at a time: { preview: { emphasis: true, links: true } }
```

The pass only ever repairs the stream's unstable tail, and only when reparsing proves the repair changed nothing but the block it aimed at. It never touches `getSource()` or source ranges, and `finish()` drops it, so the settled document is always a parse of the bytes you appended; see [provisional styling](docs/streaming-semantics.md#provisional-styling).

Fenced code is colored by the language in its info string, on the same worker that parses it, from the vendored [Lexilla](https://github.com/ScintillaOrg/lexilla) lexers:

```tsx
<DownstreamMarkdown markdown={message} syntaxHighlighting={false} />
```

`ts`, `tsx`, `js`, `java`, `c`, `cpp`, `cs`, `go`, `swift`, `kt`, `dart`, `scala`, `objc`, `py`, `sh`, `json`, `html`, `xml`, `css`, `sql`, `rs`, `yaml`, `toml`, `rb`, `lua`, `diff`, and `md` are colored; anything else renders plain, as it does in Streamdown. The palette follows GitHub's light and dark code themes and is the same on both platforms. A block with no language, an unknown language, and a block over 64 KiB of code stay plain, and a document with no fenced code never loads a lexer. Highlighting only sets colors, so selection, copy, and accessibility text are unchanged; see [the plan](docs/syntax-highlighting-plan.html) for how a streaming block stays cheap.

### Theming

`theme` sets colors, fonts and metrics; `markdownStyle` sets one element at a time. Both take one value or an array merged left to right, so a preset is an override you extend rather than replace:

```tsx
import { DownstreamMarkdown, presets } from 'react-native-downstream';

// A preset is `{ theme, markdownStyle }`, so it spreads.
<DownstreamMarkdown markdown={message} {...presets.github} />

// One array element overrides the one before it.
<DownstreamMarkdown
  markdown={message}
  theme={[{ colors: { foreground: '#101418' } }, presets.streamdown.theme]}
  markdownStyle={{ h1: { fontWeight: 700 } }}
/>
```

A color is a string or `{ light, dark }`, and either half may be left out to keep the default for that appearance. What is left unset keeps the platform's own color, so it still follows the appearance and increased contrast without a render:

```tsx
theme={{
  colors: { foreground: { light: '#1b1b1f' }, primary: '#7a3e9d' },
  fonts: { heading: 'Georgia', mono: 'JetBrainsMono-Regular' },
  radius: 8,
}}
```

Both renderers read one slot table, so a theme draws the same on each platform. Only metrics change layout, and only those are measured again: a colors-only change redraws without a single remeasure. `maxFontSizeMultiplier` caps the platform font scale, as `<Text>`'s does; it cannot turn scaling off. Any other key is reported once in development rather than ignored, so a value written for another renderer does not look like it took effect.

`theme.syntax` takes the engine's token classes. For a full editor theme, read Shiki's own JSON and convert it:

```tsx
import { syntaxColorsFromTheme } from 'react-native-downstream';

const converted = syntaxColorsFromTheme(await import('./dracula.json'));
<DownstreamMarkdown
  markdown={message}
  theme={{
    syntax: converted.syntax,
    colors: { foreground: converted.codeBlock.color, muted: converted.codeBlock.backgroundColor },
  }}
/>
```

`presets.github` and `presets.streamdown` are the two themes the renderers are checked against. See [theming](docs/api.md#theming) for the whole surface.

### Copy and download controls

Every code block gets a header and every table gets a toolbar, with no setup. `controls` takes Streamdown's shape, so a value written for it works as it is:

```tsx
<DownstreamMarkdown
  markdown={message}
  controls={{
    code: {
      copy: { onCopy: (event) => track('code copied', event.format) },
      download: { filename: 'snippet', onDownload: save },
    },
    table: { csvSeparator: ';' },
  }}
/>
```

Code copies as plain text; a table copies as `text/html` first, so pasting into a spreadsheet keeps the rows, columns, and alignment, and falls back to plain text for targets that do not take HTML. A code block's header also has a per-block wrap button, and `codeBlockWrap` sets the starting state for all of them. `controls={false}` removes the header and the toolbar without changing any block's layout.

A download opens the platform's own save dialog and the user picks the location, so the library never writes to a path it chose. `filename` is a base name; the extension comes from the block, not from the name. Every press reaches the matching callback, and a failure reports `control_failed` to that control's `onError` or to the view's. A control turns on as soon as its own block is settled, so a finished code block can be copied while the rest of the answer is still streaming. See [controls](docs/api.md#controls) for the whole shape.

`selectable` is on by default: a long press on settled text starts a text selection, and text that is still streaming is never selectable. An app that opens its own message menu on a long press — react, reply, delete — passes `selectable={false}`.

## Contributing

- [Development workflow](CONTRIBUTING.md#development-workflow)
- [Sending a pull request](CONTRIBUTING.md#sending-a-pull-request)
- [Code of conduct](CODE_OF_CONDUCT.md)

## Example app

`example/` has five screens behind a drawer, opened from the app bar or by
dragging in from the left edge. Example streams a live Markdown chat reply from
OpenAI. Controls exercises copy, download, and wrap on code blocks and tables,
and logs what every press reports. Testing replays one source through
Streamdown and then Downstream and shows the comparison once both runs finish.
Bench replays a source through Downstream alone and exports a trace for the
[performance method](docs/performance-methodology.md). Stress streams several
answers at once to exercise the worker pool. Every screen stays mounted, so a
run configured on one of them survives a trip through the drawer.

The shared look lives in `example/src/ui`: a light and dark palette, a type and
spacing scale, and the controls the screens are built from.

The Example screen calls OpenAI from the app, so it needs a key in
`example/.env`:

```sh
cp example/.env.example example/.env
# then set EXPO_PUBLIC_OPENAI_API_KEY
```

Expo compiles `EXPO_PUBLIC_` variables into the bundle, so use a key you can
revoke and never ship that build. Clear the bundler cache after changing the
value. The other four screens need no key.

## Markdown implementation plan

Phase 0 defines the [API contract](docs/api.md) and [streaming behavior](docs/streaming-semantics.md). The native display path, stream controller, and conservative incremental-tail parser are implemented. Streams use incremental parsing by default: completed blocks share their parsed content and rendering, while new input updates the unstable tail. Unproven boundaries fall back to a full parse, and `finish()` always parses the complete canonical source. Copy and download controls, per-block wrap, and the exports behind them are implemented on both platforms; the plan is [copy and download controls](docs/copy-download-controls-plan.html).

## Tests and benchmarks

`pnpm test` runs the TypeScript suites with vitest. They run the real engine:
`tests/engine.ts` compiles `src/cpp` and the session TurboModule in the
tests is backed by that parser, so a test asserts on the text a reader sees
rather than on a mock's call log. `tests/native-view.ts` stands in for the
native view by rendering the real parse as DOM.

`pnpm test:native` builds the C++ engine and runs its own suites, including the
`vendored md4c` digest pins that keep the vendored parser unmodified, the
golden token fixtures that pin the vendored lexers, the streaming
equivalence check that a grown code block ends with the tokens a one-shot pass
gives it, and the copy and download suites that pin every export format and
check a Markdown export re-parses to the table it came from.

`pnpm bench:native` builds a release-mode host engine and measures complete
highlighting, streaming highlighting, and full versus incremental parsing with
one parser retained for the stream. See the [PR 15 performance analysis](docs/pr-15-performance.md)
for workloads, measurements, and the Streamdown source comparison.

`pnpm bench` measures the Vitest adapters. The Downstream adapter starts a fresh
subprocess and replays earlier operations at every update; the Streamdown RN
adapter uses React DOM. Their different overhead prevents these timings from
ranking the native renderers. The example app's Bench and Testing screens
collect device traces.

## License

MIT, except for the vendored third-party sources, which keep their own terms:
`src/cpp/md4c` is BSD (see its `LICENSE.md`) and `src/cpp/lexilla` is HPND
(see `src/cpp/lexilla/License.txt`, © 1998-2021 Neil Hodgson), which requires
that notice to travel with every copy. Both notices ship in the package.
`src/cpp/downstream/export-extensions.json` is Streamdown's language extension
map, taken under Apache-2.0; see the attribution in that file.

---

Made with [create-react-native-library](https://github.com/callstack/react-native-builder-bob)

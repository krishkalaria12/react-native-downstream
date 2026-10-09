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
import {
  createMarkdownStream,
  DownstreamMarkdown,
} from 'react-native-downstream';

const stream = createMarkdownStream();
stream.append('Hello ');
stream.append('**world**');
stream.finish();

<DownstreamMarkdown stream={stream} />;
// Later: stream.dispose();
```

The view parses with md4c and lays text out natively (TextKit on iOS, `StaticLayout` on Android). Parsing and text preparation run on a small shared pool of worker threads below the UI and JavaScript threads, so several messages render in parallel without blocking the app. It needs the New Architecture and a development build; Expo Go cannot load it. The library never opens links itself.

An open span renders as the literal text the writer sent, so a half-arrived `**bold wor` shows its markers. Turn on provisional styling to show the content styled while the closer is still on its way:

```tsx
const stream = createMarkdownStream({ preview: true });
// Or one feature at a time: { preview: { emphasis: true, links: true, math: true } }
```

The pass only ever repairs the stream's unstable tail, and only when reparsing proves the repair changed nothing but the block it aimed at. It never touches `getSource()` or source ranges, and `finish()` drops it, so the settled document is always a parse of the bytes you appended.

Fenced code is colored by the language in its info string, on the same worker that parses it, from the vendored [Lexilla](https://github.com/ScintillaOrg/lexilla) lexers:

```tsx
<DownstreamMarkdown markdown={message} syntaxHighlighting={false} />
```

`ts`, `tsx`, `js`, `java`, `c`, `cpp`, `cs`, `go`, `swift`, `kt`, `dart`, `scala`, `objc`, `py`, `sh`, `json`, `html`, `xml`, `css`, `sql`, `rs`, `yaml`, `toml`, `rb`, `lua`, `diff`, and `md` are colored; anything else renders plain, as it does in Streamdown. The palette follows GitHub's light and dark code themes and is the same on both platforms. A block with no language, an unknown language, and a block over 64 KiB of code stay plain, and a document with no fenced code never loads a lexer. Highlighting only sets colors, so selection, copy, and accessibility text are unchanged.

While streaming, an unfinished code line longer than 4 KiB refreshes its colors as its length doubles. New text can stay plain and earlier colors provisional between refreshes, including while the stream pauses. A newline, fence closure or `finish()` refreshes the colors immediately on the next parse.

### Images and math

An image-only paragraph becomes a picture, fetched through the platform's own image pipeline so your HTTP stack, cache and URL handlers apply. A picture that has not arrived shows a placeholder, one that fails shows its alt text, and `allowedImagePrefixes` decides what may be fetched at all. A press on a picture reports `onImagePress`; the package opens nothing.

```tsx
<DownstreamMarkdown
  markdown={message}
  images
  allowedImagePrefixes={['https://cdn.example.com/']}
/>
```

Math is off by default, so a document without it parses exactly as it did before and every formula stays readable source. Turn it on for `$$…$$`, a `$$` line fence, a ` ```math ` fence, and — unlike Streamdown — `\(…\)` and `\[…\]`, which `math={{ brackets: false }}` turns back off:

```tsx
<DownstreamMarkdown markdown={message} math />
<DownstreamMarkdown markdown={message} math={{ singleDollarTextMath: true }} />
```

Typesetting a formula needs an engine, which ships as the separate opt-in package. Without it a formula keeps its literal and delimiters — which is also what copy returns and what a screen reader reads — is drawn in the theme's error colour, and reports `onMathError`. A formula that is laid out keeps its last good layout while it streams, so it does not re-shape on every token.

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
  theme={{ syntax: converted.syntax }}
  markdownStyle={{ codeBlock: converted.codeBlock }}
/>;
```

`presets.github` and `presets.streamdown` are the two themes the renderers are checked against.

### Line numbers, titles, and a capped height

Code blocks get the chrome a documentation site's do, and it all comes from the fence's own info string:

````tsx
```ts title="src/app/load.ts" startLine=10
export async function load(id: string) {
  const res = await fetch(`/api/${id}`);
  return res.json();
}
```
````

The header shows `src/app/load.ts` instead of `ts`, and the gutter numbers from 10. The same `title=`, `filename=`, and `lang:path` spellings the Docusaurus, rehype-pretty code, and Expressive Code docs use are all accepted, along with `showLineNumbers=N`, `showLineNumbers{N}`, and `noLineNumbers`.

```tsx
<DownstreamMarkdown markdown={message} lineNumbers={false} />
<DownstreamMarkdown markdown={message} codeBlockMaxHeight={0} />
```

`lineNumbers` is on by default, as in Streamdown, and a fence's `noLineNumbers` turns them off for one block. Numbers are drawn in a gutter beside the code rather than typed into it, so they are never selected, copied, or read aloud: copied text is byte-identical with numbers on or off. The gutter stays still while the code scrolls sideways, and widens by one digit as a block crosses 99. It draws in the theme's own code color, dimmed, in the theme's code font at the theme's code size, and the card's padding and border are the theme's too, so a theme restyles a code block and its numbers together.

`codeBlockMaxHeight` is 400 points, as in Streamdown, and `0` removes it. A taller block scrolls inside its own card rather than filling the screen, and while it is still streaming it follows its newest line until the reader scrolls away. A vertical drag at the card's edge scrolls the surrounding list instead, so a scroll view inside a chat list does not trap the gesture. The header's expand button lifts the cap for one block; it appears only on a block past the cap.

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

Code copies as plain text; a table copies as `text/html` first, so pasting into a spreadsheet keeps the rows, columns, and alignment, and falls back to plain text for targets that do not take HTML. A code block's header has a per-block wrap button and, on a block past the cap, an expand button; `codeBlockWrap` and `codeBlockMaxHeight` set the starting state for all of them. `controls={false}` removes the header and the toolbar without changing any block's layout.

A download opens the platform's own save dialog and the user picks the location, so the library never writes to a path it chose. `filename` is a base name; the extension comes from the block, not from the name. Every press reaches the matching callback, and a failure reports `control_failed` to that control's `onError` or to the view's. A control turns on as soon as its own block is settled, so a finished code block can be copied while the rest of the answer is still streaming.

`selectable` is on by default: a long press on settled text starts a text selection, and text that is still streaming is never selectable. An app that opens its own message menu on a long press — react, reply, delete — passes `selectable={false}`.

On Android, `androidParagraphLayout="incremental"` lets eligible live paragraphs reuse completed line layouts and shape only their unfinished suffix:

```tsx
<DownstreamMarkdown stream={stream} androidParagraphLayout="incremental" />
```

The default is `"canonical"`. Incremental mode uses greedy wrapping while an ordinary ASCII paragraph is streaming, then restores high-quality wrapping when it settles. Text can reflow at settlement. Styled text, contextual scripts, custom fonts, right-to-left text, carets and oversized unfinished suffixes use the canonical path. Selection becomes available after settlement as usual, and live accessibility and reveal remain supported. iOS keeps its existing layout. See the [Android paragraph measurements](docs/android-paragraph-layout.md) for coverage and limits.

With `selectable={false}`, long documents can recycle offscreen plain-text views using the actual ancestor viewport and cached segment frames. Scrolling reconciles the previous and next visible segment ranges. Interactive text, nested details, hosted renderers, code, tables, images and math keep their native views. Selection and accessibility retain the complete document. Android disables recycling while accessibility services are enabled; iOS restores all text views when VoiceOver, Switch Control or explicit accessibility enumeration requires them. iOS shares one weakly registered display link among eligible attached documents to detect ancestor clipping changes. Source data, export and full document measurement remain available; this reduces mounted text-view work rather than replacing layout measurement.

### Fence renderers

A fenced block whose language you register is drawn by your own React component instead of the code card, inside the same native view and in the same commit. `plugins.renderers` is Streamdown's shape, so a configuration written for the web needs only its components rewritten:

```tsx
<DownstreamMarkdown
  markdown={message}
  plugins={{
    renderers: [
      { language: ['vega', 'vega-lite'], component: VegaLiteChart },
      { language: 'mermaid', component: MermaidDiagram },
    ],
  }}
/>
```

A renderer receives four props: `code` (exactly what the block's copy button copies), `language` as the writer wrote it, `meta` from the rest of the info string, and `isIncomplete` while the fence is still streaming. Guard `isIncomplete` and show a placeholder: a component that renders half a chart looks broken, and one that renders a whole chart for a spec that is still arriving flickers as it grows.

The code card is the fallback, and it is the reason this is safe to ship: a lazy component that has not loaded yet, one that throws, and any fence you did not register all draw the card you already have, with copy and download. A throw is reported once as `renderer_failed` on `onError`, only after the fence settles, and the fence draws its card. Nothing executes a fence: a renderer that embeds HTML must sandbox it itself, because the text it receives is model output.

Matching is case-insensitive, which differs from Streamdown on purpose.

### Every React surface, and what the native answer is

Each of these is a place an app can take over. Where a surface exists on the web
and not here, the table says what replaces it rather than leaving a gap to find.

| Surface                                      | On the web                                          | Here                                                                                                                                                                                                                                   |
| -------------------------------------------- | --------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| A component for one block (`BlockComponent`) | Replace any block kind                              | `plugins.renderers` draws a **fenced** block by its language. For a block that is not a fence, a `<details>` body is drawn from the document and an allowed block tag is a container — see below.                                      |
| `mode="static"`                              | Renders from a `markdown` prop rather than a stream | The `markdown` prop, which every example in this README uses. A stream reads its `source`; a `markdown` string is the same thing without a session.                                                                                    |
| A fence that is still arriving               | `isIncomplete` on the renderer                      | The same `isIncomplete`, and the same rule: show a placeholder rather than a half-drawn chart.                                                                                                                                         |
| A table's full screen                        | A route or a modal the app owns                     | `controls.table.fullscreen.onToggle` reports the press, and `renderTableFullscreen` draws whatever the app wants — a route, a sheet, or nothing at all. The library's own modal is the default.                                        |
| `<details>`                                  | A browser widget with its own summary               | A container the engine pairs, with `controls.details.expand` as the toggle and `expanded` on the event. The summary is announced as an expandable control, so it is reachable without sight.                                           |
| An app's own HTML tag                        | `allowedTags` plus a renderer                       | `allowedTags` names it; a **multi-line** tag becomes a container whose `plugins.tags[name]` component draws it in place, with the inner Markdown and `isIncomplete`. An inline tag needs no renderer — `markdownStyle.tags` styles it. |
| A fenced block with no renderer              | The code card                                       | The code card, with copy and download. `fallbackComponent` replaces it for a fence you would rather not draw at all.                                                                                                                   |
| `onTagPress`                                 | A press on a custom tag                             | The same event, with the tag's name, its attributes, its text, and the source range.                                                                                                                                                   |
| `linkVariants`, per-URL link styles          | Style a link by its href                            | `markdownStyle.linkVariants` styles a link by URL prefix — first prefix wins — with its own color and underline, drawn on both platforms. `onLinkPress` still gets the resolved URL and the range, so an app can route by either.      |

Two of these are worth spelling out because they replace something the web has
and this does not.

**A block that is not a fence.** `BlockComponent` on the web replaces any block.
Here the unit of layout is a native segment and only four kinds have their own
drawing — prose, a table, code, and a formula or picture — so the surface is
`plugins.renderers` and it reaches fenced blocks. Everything else is reached
through the document: `allowedTags` for the app's own container, `controls` for a
button, and the callbacks for a press.

**A tag the app allowed.** `allowedTags` takes Streamdown's shape,
`Record<tag, attributes[]>`. An **inline** tag — `<user>` inside a paragraph —
keeps its text and is styled by `markdownStyle.tags`, and a press reports
`onTagPress`. A **block** tag, used with blank lines around it, becomes the same
kind of container a `<details>` is: the blocks between the pair are inside it, and
`plugins.tags[name]` draws it in place of them, receiving the inner Markdown and
`isIncomplete`. Without a renderer it draws its children natively, and
`fallbackComponent` is what an allowed tag with no renderer of its own uses.

### Streaming animation and the caret

A message can fade its new words in as they arrive, with a caret at the writing point. Both are off by default and take Streamdown's names and defaults:

```tsx
// A stream works `isAnimating` out from its own controller.
<DownstreamMarkdown stream={stream} animated caret="block" />;

// A controlled view says whether text is still arriving, as Streamdown requires.
<DownstreamMarkdown
  markdown={message}
  animated
  isAnimating={isStreaming}
  onAnimationEnd={() => hideSpinner()}
/>;

<DownstreamMarkdown
  stream={stream}
  animated={{ animation: 'slideUp', sep: 'char', stagger: 20, duration: 200 }}
  caret="circle"
/>;
```

`animation` is `fadeIn` or `slideUp`; `blurIn` draws as `fadeIn` and warns once, because neither platform can blur one run of glyphs cheaply inside a text draw pass. `duration` is 150 ms, `stagger` 40 ms, `easing` `ease`, `sep` `word`, and `maxBacklogMs` 320. A burst is compressed into the backlog rather than queued, so forty words fade across about a third of a second instead of leaving a second and a half of invisible text. CJK is split per syllable in either mode, and a rule, a table row, and an empty fence are one unit each.

What fades is decided by aligning the new document's visible text against the one the view last showed, in the engine, so a character already on screen never fades again when a `**` closes, a link resolves, or a list continues. A word split across two snapshots joins the fade already running and keeps its timing.

`caret` is `'none'`, `'block'`, or `'circle'`. It is drawn rather than typed, so it looks the same in every font and cannot change a line's height, and it is never copied, selected, exported, or read aloud. It sits after the last character of prose, a heading, a list item, or a still-open fence, and takes a line of its own in an empty document or after a table, a closed fence, or a rule. While a fade runs it follows the last word that has _started_, so it never sits after a gap.

A frame of animation costs a lookup and a redraw. There is no React render, no Fabric state update, no relayout, and no change to any cached text, which matters because a segment is shared and immutable and alpha can only be applied in a draw pass. Reduce Motion turns the fade off on both platforms, and Android's animator duration scale multiplies every duration and turns it off at zero. With `animated` and `caret` off the output is identical to a build without them.

## Contributing

- [Development workflow](CONTRIBUTING.md#development-workflow)
- [Sending a pull request](CONTRIBUTING.md#sending-a-pull-request)
- [Code of conduct](CODE_OF_CONDUCT.md)

## Example app

`example/` has seven screens behind a drawer, opened from the app bar or by
dragging in from the left edge. Example streams a live Markdown chat reply from
OpenAI, with a chart renderer registered. Controls exercises copy, download, and
wrap on code blocks and tables, and logs what every press reports. Renderers
draws fences with app components, including one that throws and one that loads
lazily, in controlled and streamed modes. Animation replays a source with the
per-word fade-in and the caret. Testing replays one source through
Streamdown and then Downstream and shows the comparison once both runs finish.
Bench replays a source through Downstream alone and exports a trace for the
performance method. Stress streams several
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
value. The other six screens need no key.

## Markdown implementation plan

The native display path, stream controller, and conservative incremental-tail parser are implemented. Streams use incremental parsing by default: completed blocks share their parsed content and rendering, while new input updates the unstable tail. Unproven boundaries fall back to a full parse, and `finish()` always parses the complete canonical source. Copy and download controls, per-block wrap, and the exports behind them are implemented on both platforms, as is the streaming fade-in and the caret.

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
one parser retained for the stream.

`pnpm bench` measures the Vitest adapters. The Downstream adapter starts a fresh
subprocess and replays earlier operations at every update; the Streamdown RN
adapter uses React DOM. Their different overhead prevents these timings from
ranking the native renderers. The example app's Bench and Testing screens
collect device traces.

## Running CI locally

CI runs three workflows. `ci.yml` runs on every push. `ios.yml` and
`android.yml` run only when native code or the native specs change (`src/cpp`,
`src/ios`, `src/android`, `src/specs`, the `Native*.ts` files, and the `math/`
workspace package). The commands below reproduce the CI checks. Run them from
the repository root after `pnpm install`.

```sh
pnpm lint && pnpm typecheck   # lint
pnpm test                     # test
pnpm test:native              # test-native: builds and tests the C++ engine
pnpm test:kotlin              # test-kotlin: needs kotlinc (brew install kotlin)
pnpm prepare                  # build-library
pnpm example exec expo export --platform web   # build-web
```

### iOS

Needs Xcode and CocoaPods. Vendor the pinned RaTeX framework and KaTeX fonts
before installing pods; the script verifies both downloads against their
SHA-256 pins. The `DownstreamMath` scheme compiles the math installer, the
`Downstream` pod, and their dependencies for the simulator without building or
signing the app:

```sh
node math/scripts/vendor-ratex.mjs
pnpm example exec expo prebuild --platform ios --no-install
(cd example/ios && pod install)
xcodebuild -project example/ios/Pods/Pods.xcodeproj -scheme DownstreamMath \
  -configuration Debug -sdk iphonesimulator \
  -destination 'generic/platform=iOS Simulator' \
  -derivedDataPath /tmp/downstream-ios-build \
  CODE_SIGNING_ALLOWED=NO COMPILER_INDEX_STORE_ENABLE=NO build
```

If `xcode-select` points at the Command Line Tools, prefix the command with
`DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`. Newer Xcode releases
reject a dependency's iOS 6.0 deployment target. Add
`IPHONEOS_DEPLOYMENT_TARGET=15.1` to the `xcodebuild` line to work around it.

### Android

Needs JDK 17, the Android SDK, and NDK 27.1, with `ANDROID_HOME` set. This
compiles the Kotlin sources and the JNI and Fabric C++ for one ABI, without an
emulator or an APK:

```sh
pnpm example exec expo prebuild --platform android --no-install
(cd example/android && ./gradlew :react-native-downstream:compileDebugKotlin \
  :react-native-downstream-math:compileDebugKotlin \
  :app:externalNativeBuildDebug -PreactNativeArchitectures=x86_64)
```

An additional local check compiles the Android source set and the JNI without
Gradle:

```sh
pnpm typecheck:android
```

`typecheck:android` needs the Android SDK, an NDK, kotlinc, and a populated
Gradle cache, and skips with a message when one is missing. It resolves
`react-android` out of that cache and runs React Native's own codegen, so the view
manager interface it checks against is the generated one.

### The C++ React Native builds

Three C++ files reach a device through React Native's own build rather than
through this repository's, so no test here compiled them until this check:

```sh
pnpm test:fabric
```

It runs the compiler in syntax-only mode over the fabric shadow node, the layout
manager it calls, and the session TurboModule, against React Native's real
headers and its real codegen output from `node_modules`. It needs a C++ compiler
and nothing else.

The Android build compiles C++ with `-Werror`, so a deprecation warning that iOS
lets through fails here. `example/ios` and `example/android` are generated and
ignored by git. Delete them and prebuild again when a native dependency changes.

## License

MIT, except for the vendored third-party sources, which keep their own terms:
`src/cpp/md4c` is BSD (see its `LICENSE.md`) and `src/cpp/lexilla` is HPND
(see `src/cpp/lexilla/License.txt`, © 1998-2021 Neil Hodgson), which requires
that notice to travel with every copy. Both notices ship in the package.
`src/cpp/downstream/export-extensions.json` is Streamdown's language extension
map, taken under Apache-2.0; see the attribution in that file.

---

Made with [create-react-native-library](https://github.com/callstack/react-native-builder-bob)

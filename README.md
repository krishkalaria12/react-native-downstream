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

## Contributing

- [Development workflow](CONTRIBUTING.md#development-workflow)
- [Sending a pull request](CONTRIBUTING.md#sending-a-pull-request)
- [Code of conduct](CODE_OF_CONDUCT.md)

## Example app

`example/` has four screens behind a drawer, opened from the app bar or by
dragging in from the left edge. Example streams a live Markdown chat reply from
OpenAI. Testing replays one source through Streamdown and then Downstream and
shows the comparison once both runs finish. Bench replays a source through
Downstream alone and exports a trace for the
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
value. The other three screens need no key.

## Markdown implementation plan

Phase 0 defines the [API contract](docs/api.md) and [streaming behavior](docs/streaming-semantics.md). The native display path, stream controller, and conservative incremental-tail parser are implemented. Full parsing remains the default mode while the two modes await device measurements.

## Tests and benchmarks

`pnpm test` runs the TypeScript suites with vitest. They run the real engine:
`tests/engine.ts` compiles `src/cpp` and the session TurboModule in the
tests is backed by that parser, so a test asserts on the text a reader sees
rather than on a mock's call log. `tests/native-view.ts` stands in for the
native view by rendering the real parse as DOM.

`pnpm test:native` builds the C++ engine and runs its own suites, including the
`vendored md4c` digest pins that keep the vendored parser unmodified, the
golden token fixtures that pin the vendored lexers, and the streaming
equivalence check that a grown code block ends with the tokens a one-shot pass
gives it.

`pnpm bench` runs the vitest benchmarks, including a Downstream against
Streamdown RN comparison. Those numbers describe the harness: the engine runs in
a subprocess there, where it is in-process on a device. The example app's Bench
and Testing screens measure a real device.

## License

MIT, except for the vendored third-party sources, which keep their own terms:
`src/cpp/md4c` is BSD (see its `LICENSE.md`) and `src/cpp/lexilla` is HPND
(see `src/cpp/lexilla/License.txt`, © 1998-2021 Neil Hodgson), which requires
that notice to travel with every copy. Both notices ship in the package.

---

Made with [create-react-native-library](https://github.com/callstack/react-native-builder-bob)

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

Phase 0 defines the [API contract](docs/api.md), [streaming behavior](docs/streaming-semantics.md), and [parser fixtures](src/fixtures/README.md). The native display path, stream controller, and conservative incremental-tail parser are implemented. Full parsing remains the default mode while the two modes await device measurements.

Run the shared C++ engine tests with `pnpm test:native`.

## License

MIT

---

Made with [create-react-native-library](https://github.com/callstack/react-native-builder-bob)

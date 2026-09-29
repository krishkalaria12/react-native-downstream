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

## Markdown implementation plan

Phase 0 defines the [API contract](docs/api.md), [streaming behavior](docs/streaming-semantics.md), and [parser fixtures](fixtures/README.md). The native display path, stream controller, and conservative incremental-tail parser are implemented. Full parsing remains the default mode while the two modes await device measurements.

Run the shared C++ engine tests with `python3 cpp/tests/test_engine.py`.

## License

MIT

---

Made with [create-react-native-library](https://github.com/callstack/react-native-builder-bob)

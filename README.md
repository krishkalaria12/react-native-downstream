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

The view parses with md4c and lays text out natively (TextKit on iOS, `StaticLayout` on Android). It needs the New Architecture and a development build; Expo Go cannot load it. The library never opens links itself.

## Contributing

- [Development workflow](CONTRIBUTING.md#development-workflow)
- [Sending a pull request](CONTRIBUTING.md#sending-a-pull-request)
- [Code of conduct](CODE_OF_CONDUCT.md)

## Markdown implementation plan

Phase 0 defines the [API contract](docs/api.md), [streaming behavior](docs/streaming-semantics.md), and [parser fixtures](fixtures/README.md). The static native display path is implemented; the stream controller is planned for the native session phase.

Run the shared C++ engine tests with `python3 cpp/tests/test_engine.py`.

## License

MIT

---

Made with [create-react-native-library](https://github.com/callstack/react-native-builder-bob)

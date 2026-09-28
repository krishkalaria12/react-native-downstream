# react-native-downstream

React Native Downstream

## Installation

```sh
pnpm add react-native-downstream
```

## Usage

```js
import { multiply } from 'react-native-downstream';

// ...

const result = await multiply(3, 7);
```

## Contributing

- [Development workflow](CONTRIBUTING.md#development-workflow)
- [Sending a pull request](CONTRIBUTING.md#sending-a-pull-request)
- [Code of conduct](CODE_OF_CONDUCT.md)

## Markdown implementation plan

Phase 0 defines the proposed [API contract](docs/api.md), [streaming behavior](docs/streaming-semantics.md), and [parser fixtures](fixtures/README.md). The Markdown view and stream controller are planned runtime APIs; this package still exports only `multiply`.

## License

MIT

---

Made with [create-react-native-library](https://github.com/callstack/react-native-builder-bob)

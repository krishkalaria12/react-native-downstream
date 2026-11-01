# react-native-downstream-math

Optional native math typesetting for React Native Downstream, using RaTeX and
the KaTeX fonts. Installing the package links the engine; select `math()` on a
view or provider to enable it.

## Install

```sh
pnpm add react-native-downstream react-native-downstream-math
# Bare iOS apps:
npx pod-install
```

Rebuild your native app after installation. Expo projects need a development
build; Expo Go cannot load this engine.

## Use

```tsx
import { Downstream, DownstreamProvider } from 'react-native-downstream';
import { math } from 'react-native-downstream-math';

const plugins = [math({ singleDollarTextMath: true })];

<DownstreamProvider plugins={plugins}>
  <Downstream>{String.raw`The answer is \(x^2\).`}</Downstream>
</DownstreamProvider>;
```

`math()` recognizes `$$...$$`, display blocks delimited by `$$`, fenced `math`
blocks, `\(...\)`, and `\[...\]`. Single-dollar `$...$` math is off by default,
so currency remains text. Pass `singleDollarTextMath: true` to enable it, or
`brackets: false` to disable the backslash forms.

Both string sources and external stream controllers take their math policy
from the view's plugin selection. Changing the selection rebuilds the retained
source without dropping text or its finished state. Unsupported formulas keep
their source and report the view's `onMathError` callback.

An explicit `plugins` array replaces the provider's selection. Use
`plugins={[]}` to disable all feature plugins for one view. If the provider also
selects `code()`, include `code()` when replacing its list with a local math
configuration.

`mathAvailable()` from the core package reports whether the native engine is
installed, independently of a view's selected plugins. An installed engine no
longer enables math automatically. Development builds warn when a view has an
installed engine without a math plugin.

## From a source checkout

The iOS XCFramework and fonts are pinned by `ratex-version.json`. Fetch them
before installing pods if the vendored files are absent:

```sh
pnpm --filter react-native-downstream-math vendor
```

Android resolves the matching RaTeX release through the package's Gradle
dependency. See the [core README](../README.md) for styles, streaming, and the
complete migration guide.

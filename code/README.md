# @react-native-downstream/code

Optional native syntax highlighting for React Native Downstream. The core
renders plain code blocks, line numbers, and copy/download controls without
this package. Installing it links the lexer; selecting `code()` enables it for
a view or provider.

## Install

```sh
pnpm add @react-native-downstream/core @react-native-downstream/code
# Bare iOS apps:
npx pod-install
```

Rebuild your native app after installation. Expo projects need a development
build; Expo Go cannot load the native lexer.

## Use

```tsx
import { Downstream, DownstreamProvider } from '@react-native-downstream/core';
import { code } from '@react-native-downstream/code';

const plugins = [code()];

<DownstreamProvider plugins={plugins}>
  <Downstream codeBlock={{ wrap: true }}>{text}</Downstream>
</DownstreamProvider>;
```

The plugin supplies highlighting. Layout is set in core through
`codeBlock={{ wrap, lineNumbers, maxHeight }}`. Colors come from `theme.syntax`,
and `syntaxColorsFromTheme` converts VS Code/Shiki themes.

An explicit view `plugins` array replaces its provider's selection. Use
`plugins={[]}` for plain code in one view. If the provider also selects
`math()`, include it when replacing the list for a local configuration.

`codeAvailable()` from the core package reports whether the native lexer is
installed, independently of plugin selection. Without the linked native
package, code renders plain. With the package installed, views that omit the
plugin skip lexer work.

The package owns the generated lexer, WASM-to-C runtime, and identifier tables.
It installs a versioned C function table into core. iOS installs at application
load; Android resolves the shared library on first use. Neither path uses a
JavaScript highlighter or a WebView.

## Validation and licenses

From a source checkout, `node scripts/test-core-without-code.mjs` builds and
checks the core without linking this package. Native CI builds both package
configurations. `DOWNSTREAM_DISABLE_CODE_PACKAGE=1` excludes this package from
native autolinking for those builds.

The [lexer](cpp/wasm/LICENSE.md) and [wasm2c runtime](cpp/wasm/wasm-rt/LICENSE)
use Apache-2.0; [SIMDe](cpp/wasm/simde/COPYING) uses MIT. Their notices ship with
the package. See the [core README](../README.md) for language support.

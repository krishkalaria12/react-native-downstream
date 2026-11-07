---
title: Plugins & defaults
description: Add native code highlighting and math with feature plugins, and share options across your app with DownstreamProvider.
group: Guide
order: 3
---

The core package renders plain code blocks and includes tables, images, controls, link safety, CJK parsing, and the extra inline syntaxes. Native code highlighting and math typesetting are separate packages, so an app only ships the engines it uses.

## Install feature plugins

```sh
pnpm add @react-native-downstream/code @react-native-downstream/math
# For a bare iOS app:
npx pod-install
```

Rebuild the native app after installing either package. Expo projects need a development build.

| Package                         | Plugin          | Adds                                                    |
| ------------------------------- | --------------- | ------------------------------------------------------- |
| `@react-native-downstream/code` | `code()`        | Native syntax highlighting for 84 languages.            |
| `@react-native-downstream/math` | `math(options)` | Native math typesetting with RaTeX and the KaTeX fonts. |

## Select plugins

Installing a package links its engine but does not turn it on. Select plugins on a view:

```tsx
import { Downstream } from '@react-native-downstream/core';
import { code } from '@react-native-downstream/code';
import { math } from '@react-native-downstream/math';

const plugins = [code(), math({ singleDollarTextMath: true })];

<Downstream plugins={plugins} isAnimating={streaming}>
  {text}
</Downstream>;
```

Define the array once, outside render, so every message shares it. Plugins apply in array order, and later entries override earlier defaults. Duplicate names are an error in development.

`codeAvailable()` and `mathAvailable()` report whether each native engine is installed in the current binary.

## App-wide defaults

Set shared options once with `DownstreamProvider`. Every view inside it inherits the plugins and any other option:

```tsx
import { DownstreamProvider, presets } from '@react-native-downstream/core';
import { code } from '@react-native-downstream/code';
import { math } from '@react-native-downstream/math';

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

The provider accepts every view option except the source.

## How options combine

- Props on a view override provider defaults, and nested providers override outer ones.
- Grouped options such as `codeBlock`, `table`, `images`, and `links` inherit the subfields a view leaves out.
- `components` maps inherit entries that the view does not replace.
- `theme` and `markdownStyle` merge plugin defaults first, then the provider, then the view, left to right like their array forms.
- An explicit `plugins` array replaces the inherited selection. `plugins={[]}` opts one view out of every feature plugin, and `plugins={[math()]}` selects only math.

When you replace a provider's selection on one view, include every plugin that view still needs, for example `plugins={[code(), math({ brackets: false })]}`.

## Write your own plugin

A plugin is a named object with optional `parse`, `fences`, `tags`, and `style` fields. Use one to package a fence or tag component together with its styles and parse options, so several screens can select it by name:

```tsx
import type { DownstreamPlugin } from '@react-native-downstream/core';

export const answerCards: DownstreamPlugin = {
  name: 'answer-cards',
  fences: { 'answer-card': AnswerCard },
  style: {
    markdownStyle: { codeBlock: { borderRadius: 16 } },
  },
};

<Downstream plugins={[code(), answerCards]}>{text}</Downstream>;
```

App components and provider or view styles override plugin defaults. See [Custom renderers](/docs/custom-renderers) for the props a fence or tag component receives.

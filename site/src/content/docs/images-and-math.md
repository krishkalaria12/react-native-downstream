---
title: Images & math
description: HTTPS images with an allowlist, and optional native math typesetting that stays stable while streaming.
group: Guide
order: 5
---

## Images

An image-only paragraph renders as a native image with a loading placeholder and alt text on failure. Images are enabled by default and load over HTTPS.

```tsx
<DownstreamMarkdown
  markdown={text}
  allowedImagePrefixes={['https://cdn.example.com/']}
  onImagePress={({ url }) => console.log('Image pressed', url)}
/>
```

The default prefix list is `['*']`, which allows any HTTPS host. Every image URL comes from model output, and a model that reads untrusted text can be made to fetch any URL. An app whose models read web pages, tool output, or email should pass its own CDN prefixes.

Use `images={false}` to show alt text without fetching images.

`markdownStyle.image` controls height, radius, margins, and placeholder color. Loaded images can also have a download control:

```tsx
<DownstreamMarkdown
  markdown={text}
  controls={{ image: { download: true } }}
/>
```

Images are loaded at most four at a time and cancelled when no view needs them.

## Math

Math is optional. Install the typesetting package and rebuild your native app:

```sh
pnpm add react-native-downstream-math
# For a bare iOS app:
npx pod-install
```

Math is enabled by default when the native math engine is installed. Use `math={false}` to keep formulas as source, or configure the delimiters:

```tsx
<DownstreamMarkdown
  markdown={text}
  math={{ singleDollarTextMath: true, brackets: true }}
  onMathError={({ source, message }) => console.warn(source, message)}
/>
```

### Supported forms

- `$$...$$`
- Display blocks delimited by `$$`
- Fenced `math` blocks
- `\(...\)`
- `\[...\]`

Single-dollar `$...$` math is off by default so currency stays text. `brackets: false` disables the backslash forms.

### Errors and streaming

Unsupported formulas, or formulas requested without an installed engine, display their source and report `onMathError`. The event includes the TeX source, the engine message, whether the formula is display math, and the source generation.

During streaming, a formula keeps its last valid layout while more input arrives. A formula that is still growing does not report an error for every token.

For streams, set math options when creating the controller. The view's `math` prop controls typesetting of formulas the stream has already parsed:

```tsx
const stream = createMarkdownStream({
  math: { singleDollarTextMath: true },
});

<DownstreamMarkdown stream={stream} math />;
```

### Styling

`markdownStyle.math` and `markdownStyle.inlineMath` control spacing, color, and sizing. Theme font scale caps apply to math as well.

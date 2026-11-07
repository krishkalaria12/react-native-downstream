---
title: Custom renderers
description: Turn fenced languages into your own React components, and allow your app's inline and block tags.
group: Guide
order: 6
---

## Render a fence with your own component

Use `plugins.renderers` to turn a fenced language into an app component. A chart, diagram, or interactive card can sit inside the same Markdown document.

```tsx
import { Text } from 'react-native';
import {
  DownstreamMarkdown,
  type CustomRendererProps,
} from '@react-native-downstream/core';

function AnswerCard({ code, isIncomplete }: CustomRendererProps) {
  return <Text>{isIncomplete ? 'Receiving answer...' : code}</Text>;
}

<DownstreamMarkdown
  markdown={text}
  plugins={{
    renderers: [{ language: 'answer-card', component: AnswerCard }],
  }}
/>;
```

A fence beginning with ` ```answer-card ` now renders `AnswerCard`.

Each renderer receives:

| Prop           | Meaning                                                                    |
| -------------- | -------------------------------------------------------------------------- |
| `code`         | The fence's text, exactly what the code block's copy button copies.        |
| `language`     | The info string's first word as written, cut at `:`.                       |
| `meta`         | The info string after the language, trimmed. Undefined when there is none. |
| `isIncomplete` | True until the fence is settled.                                           |

Language matching is case-insensitive, and `language` can be an array of aliases.

Use `isIncomplete` to show a placeholder until a structured payload is complete. An unloaded or failed renderer falls back to the native code card. A settled renderer failure reports `renderer_failed` through `onError`. You supply the rendering logic; registering a language does not execute its contents.

## Style inline tags

Allow your app's tags and choose the attributes they can carry:

```tsx
<DownstreamMarkdown
  markdown={'Hello <user id="42">Sam</user>.'}
  allowedTags={{ user: ['id'] }}
  markdownStyle={{ tags: { user: { color: '#4f46e5', fontWeight: 700 } } }}
  onTagPress={({ attributes }) => console.log('User', attributes.id)}
/>
```

Inline tags retain their text and use `markdownStyle.tags`.

## Replace block tags

For a block tag placed on its own lines with blank lines around it, register `plugins.tags`:

```tsx
<DownstreamMarkdown
  markdown={text}
  allowedTags={{ callout: ['tone'] }}
  plugins={{
    tags: [{ name: 'callout', component: CalloutCard }],
  }}
/>
```

A tag component receives `name`, allowed `attributes`, inner `markdown`, and `isIncomplete`.

Without a custom block renderer, Downstream draws the tag's children natively. `plugins.fallbackComponent` provides a shared replacement for unmatched fences or allowed block tags.

## Keep tag contents literal

`literalTagContent` keeps tag contents as unstyled text:

```tsx
<DownstreamMarkdown
  markdown={'<user>Bob</user>'}
  allowedTags={{ user: [] }}
  literalTagContent
/>
```

This is what an app wants when `<user>Bob</user>` is a name, not a paragraph.

## Built-in details

`<details>` and `<summary>` are built in and provide collapsible sections. `controls.details.expand` configures their toggle.

```tsx
<DownstreamMarkdown markdown={text} controls={{ details: { expand: true } }} />
```

---
title: Custom renderers
description: Turn fenced languages into your own React components, and allow your app's inline and block tags.
group: Guide
order: 7
---

## Render a fence with your own component

Use `components.fences` to turn a fenced language into an app component. A chart, diagram, or interactive card can sit inside the same Markdown document.

```tsx
import { Text } from 'react-native';
import {
  Downstream,
  type CustomRendererProps,
} from '@react-native-downstream/core';

function AnswerCard({ code, isIncomplete }: CustomRendererProps) {
  return <Text>{isIncomplete ? 'Receiving answer...' : code}</Text>;
}

<Downstream components={{ fences: { 'answer-card': AnswerCard } }}>
  {text}
</Downstream>;
```

A fence beginning with ` ```answer-card ` now renders `AnswerCard`.

Each renderer receives:

| Prop           | Meaning                                                                    |
| -------------- | -------------------------------------------------------------------------- |
| `code`         | The fence's text, exactly what the code block's copy button copies.        |
| `language`     | The info string's first word as written, cut at `:`.                       |
| `meta`         | The info string after the language, trimmed. Undefined when there is none. |
| `isIncomplete` | True until the fence is settled.                                           |

Language matching is case-insensitive. To register aliases, map each language key to the same component.

Use `isIncomplete` to show a placeholder until a structured payload is complete. An unloaded or failed renderer falls back to the native code card. A settled renderer failure reports `renderer_failed` through `onError`. You supply the rendering logic; registering a language does not execute its contents.

## Style inline tags

Allow your app's tags and choose the attributes they can carry:

```tsx
<Downstream
  allowedTags={{ user: ['id'] }}
  markdownStyle={{ tags: { user: { color: '#4f46e5', fontWeight: 700 } } }}
  onTagPress={({ attributes }) => console.log('User', attributes.id)}
>
  {'Hello <user id="42">Sam</user>.'}
</Downstream>
```

Inline tags retain their text and use `markdownStyle.tags`.

## Replace block tags

For a block tag placed on its own lines with blank lines around it, register a component under `components.tags`:

```tsx
<Downstream
  allowedTags={{ callout: ['tone'] }}
  components={{ tags: { callout: CalloutCard } }}
>
  {text}
</Downstream>
```

A tag component receives `name`, allowed `attributes`, inner `markdown`, and `isIncomplete`.

Without a custom block renderer, Downstream draws the tag's children natively. `components.fallback` provides a shared replacement for unmatched fences or allowed block tags.

## Keep tag contents literal

`literalTagContent` keeps tag contents as unstyled text:

```tsx
<Downstream allowedTags={{ user: [] }} literalTagContent>
  {'<user>Bob</user>'}
</Downstream>
```

This is what an app wants when `<user>Bob</user>` is a name, not a paragraph.

## Built-in details

`<details>` and `<summary>` are built in and provide collapsible sections. `controls.details.expand` configures their toggle.

```tsx
<Downstream controls={{ details: { expand: true } }}>{text}</Downstream>
```

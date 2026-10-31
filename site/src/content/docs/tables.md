---
title: Tables
description: Scrollable tables with copy formats, export, expand, and a full-screen view.
group: Guide
order: 4
---

Tables scroll when they exceed their available width or height. Their toolbar lets readers copy, download, expand, or open a full-screen view.

```tsx
<DownstreamMarkdown
  markdown={text}
  tableMaxHeight={300}
  controls={{
    table: {
      csvSeparator: ';',
      fullscreen: true,
      download: false,
    },
  }}
/>
```

The default height cap is 300 points. `tableMaxHeight={0}` removes it.

## Copy formats

Table copy offers:

- Markdown
- CSV
- TSV
- HTML clipboard data, so compatible spreadsheet apps preserve the grid

Table downloads offer CSV and Markdown. Downloads use the platform's save dialog.

## Full-screen view

`onTableFullscreen` reports a full-screen request. Use `renderTableFullscreen` to replace the default modal with your own React content:

```tsx
<DownstreamMarkdown
  markdown={text}
  onTableFullscreen={(event) => console.log(event.markdown)}
  renderTableFullscreen={(event) => <MyTableScreen markdown={event.markdown} />}
/>
```

The callback receives the table's own Markdown and nothing else about the view, so an app that pushes a route has everything it needs.

## Styling

Table tokens live on the theme, and individual elements can be styled with `markdownStyle`:

```tsx
<DownstreamMarkdown
  markdown={text}
  markdownStyle={{
    table: {
      borderColor: '#1c1b19',
      headerBackgroundColor: '#3654d9',
      headerTextColor: '#ffffff',
    },
  }}
/>
```

See [Theming](/docs/theming) for the full token list.

## During streaming

A table that is still arriving can be hidden until it is complete. Enable the table preview when you create the controller:

```tsx
const stream = createMarkdownStream({
  preview: { tables: true },
});
```

Not even the rows that have settled are shown; when the table completes, its rows arrive together. This prevents a half-drawn grid from flashing on screen.

Buttons for a table become available when that table settles, even while later blocks are still streaming.

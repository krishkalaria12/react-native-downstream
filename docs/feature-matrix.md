# First-release feature behavior

This matrix applies to the controlled view and the native stream session on iOS and Android. The release scope comes from [the API contract](api.md#feature-scope). The example app has four screens: Example streams a live Markdown chat reply, Testing replays one source through Streamdown and Downstream, Bench replays a source through Downstream alone, and Stress streams several answers at once.

| Feature | iOS and Android behavior | Streaming behavior |
| --- | --- | --- |
| CommonMark and GFM text | Paragraphs, headings, lists, quotes, rules, emphasis, strikethrough, inline code, fenced and indented code, and task markers render as native text. | The active tail can change classification; completed snapshots match a fresh full parse. |
| Tables | A valid separator classifies a table. Columns use the widest current cell, clamped to 56–280 points plus padding. Wide tables scroll horizontally; inline styles and validated links remain in cells. | Rows appear as their source completes. The visible table updates without discarding the user's horizontal scroll position. Column widths can grow when a longer cell arrives. |
| Code | Code uses plain monospace text. No highlighter is loaded. | Open fences show incoming lines before the closing fence arrives. |
| Images | The renderer shows alt text and makes no image request. | A partial image destination remains non-actionable, including when the URL inside it is already complete. |
| Math and raw HTML | Math source and delimiters remain literal. Raw HTML is visible text, never executed. | Partial syntax remains text until a valid Markdown construct changes its classification. |
| Links | Complete absolute `http:`, `https:`, and `mailto:` targets can emit `onLinkPress` with the full Markdown source range. The package does not open URLs. | Incomplete and disallowed destinations cannot emit an event. A bare URL inside an unfinished `](` is left as text, because md4c's permissive autolinks would otherwise offer a target the next chunk can move or remove. A bare URL in prose still becomes a link. |
| Selection and copy | The `selectable` prop enables native text selection in text segments. Copy returns visible text. The controlled prop or `stream.getSource()` provides the complete canonical source. | Completed text segments retain their selection when their native view is reused. Selection cannot cross a table or segment boundary. |
| Accessibility | Native reading order includes headings, individual link-role elements, and spoken checked or unchecked task state. Table rows are available in reading order. | Accessibility content follows the current snapshot and does not expose superseded text. |
| Font scale, RTL, colors | Text follows system font scale and natural writing direction. List and quote decorations follow the paragraph direction. Colors follow the system light or dark appearance. | Layout updates when the view's font scale, width, or appearance changes. |

Network images, syntax highlighting, native math layout, link long press actions, and per-block copy controls belong to a later release. Their absence keeps plain prose independent of those heavier paths.

## Device checks

Run the example on both platforms with a development build. The Example screen needs an `EXPO_PUBLIC_OPENAI_API_KEY` in `example/.env`; without one it shows a setup note instead of calling the API. The Testing and Bench screens replay the documents in `__benchmarks__/samples.ts` and need no key. The Example screen exercises the first-release forms as live text, and the Testing screen replays the same forms in full and incremental parse modes.

1. With VoiceOver or TalkBack, traverse headings, task items, links, and each table row. Check link activation and task state announcements.
2. Select and copy text, then compare it with the visible text. Compare `getSource()` with the exact appended Markdown after a stream finishes.
3. Scroll a wide table horizontally while a new row streams in. Its position should stay put. Replay or reset the stream and check that a different table starts at the leading edge.
4. Rotate the device, enlarge system text, switch light and dark appearance, and revisit the RTL case. Check wrapping, mirrored decorations, and table overflow.
5. Scroll between active and completed messages. Check that old content does not reappear after a cell is recycled or a session resets.

Automated parser and controller tests cover source preservation, URL policy, prefix classification, and final parse equality. Native layout and accessibility checks require a device or simulator.

# Markdown display API contract

This is the Phase 0 contract for the native Markdown component. `src/types.ts` holds the public types. `DownstreamMarkdown` accepts a controlled `markdown` prop or a controller from `createMarkdownStream`. See [implementation status](#implementation-status) for what the current build covers. Both iOS and Android follow this contract. The first supported app target is the repository's React Native 0.83.2 and Expo 55 example, using a development build.

## Feature scope

| Feature                             | First release                                                                  | Later release                                                          | Unsupported                                                |
| ----------------------------------- | ------------------------------------------------------------------------------ | ---------------------------------------------------------------------- | ---------------------------------------------------------- |
| CommonMark blocks and inline syntax | Paragraphs, headings, lists, quotes, rules, emphasis, code, and escapes        | None planned                                                           | Raw HTML execution                                         |
| GFM tables                          | Progressive rows after a valid separator; horizontal overflow                  | Rich cell content if native layout cannot preserve it at launch        | Raw HTML in cells                                          |
| GFM task lists                      | Readable checkbox state and accessibility label; display only                  | Interactive toggling                                                   | Mutation of Markdown by tapping                            |
| Raw HTML                            | Escaped, visible source text                                                   | None planned                                                           | HTML rendering, scripts, embedded content                  |
| Images                              | Alt text remains readable; no network request                                  | HTTPS images after complete URL validation, placeholders, cancellation | `data:`, `file:`, and custom schemes                       |
| Code                                | Inline code, progressively visible fenced/indented blocks, and syntax highlighting by the fence's language | Code block header and copy button                                     | Executing code, and highlighting above 64 KiB of code       |
| Math                                | Literal delimiter/source text                                                  | Native math layout after separate review                               | Executing macros or loading remote math content            |
| Links                               | Valid complete `https:`, `http:`, and `mailto:` links; press callback          | Long press actions                                                     | Automatic opening of custom schemes                        |
| Custom renderers                    | None                                                                           | None planned                                                           | Per-node React render callbacks in the native display path |
| Selection                           | Select/copy visible text and complete canonical source                         | Block-specific copy controls                                           | Editing inside the display view                            |
| Accessibility                       | Text reading order, heading and link roles, task state, font scaling, RTL text | Rich table navigation if needed                                        | Platform-specific semantics that change source meaning     |

The same feature matrix applies on iOS and Android. See the [platform behavior matrix](feature-matrix.md) for rendering and device checks. The first release is not complete until the first-release cells work on both. A native renderer may show unsupported syntax as literal text; it must never silently execute it. For oracle fixtures, md4c's parsed HTML records syntax interpretation. The display policy above is a separate projection of that parse.

## Input modes

```tsx
<DownstreamMarkdown markdown={message} onLinkPress={handleLink} />;

const stream = createMarkdownStream({ onError: handleStreamError });
<DownstreamMarkdown stream={stream} />;
stream.append('hello ');
stream.append('**world**');
stream.finish();

// An open span previews as bold instead of showing its markers.
const previewed = createMarkdownStream({ preview: true });
previewed.append('hello **wor');

// Fenced code is colored by its language. `false` keeps it plain monospace.
<DownstreamMarkdown markdown={message} syntaxHighlighting={false} />;
```

The examples describe the runtime API. A view receives exactly one of `markdown` or `stream`. TypeScript rejects both or neither. A controlled string replacement starts a new generation when its value changes. An unchanged value, including a React rerender, does not reset work. `stream` identity changes unbind the previous controller and bind the new one; they do not copy source between controllers.

`syntaxHighlighting` is on by default and decides only whether the platform applies the colors the engine computed. A fenced block is colored when its info string names a language the engine has a lexer for; the info string is trimmed, lower-cased, and cut at the first space and at a `:`, so ```TypeScript and ```ts title="a.ts" are TypeScript. A block with no language, an unknown language, and a block over 64 KiB of code stay plain either way. `syntaxHighlighting` only changes colors, never a character, so selection, copy, accessibility text, and link ranges are the same with it on or off. The default palette follows GitHub's light and dark code themes on both platforms; overriding individual token colors is not part of this release. An indented code block has no language and stays plain. A document with no fenced code never loads a lexer, and a fence with a language but no lexer is not auto-detected.

A block that is still growing may be colored only in part while it streams, and the rest is colored as the stream continues. A block that is complete is colored whole: the engine keeps working on it within the pass that colors it, so a block too large for the per-pass time budget ends up fully colored rather than part-colored. The per-pass budget bounds what one snapshot does, not how complete a finished block is.

`createMarkdownStream(options?)` creates an active, empty controller. `append(chunk)` adds a nonempty chunk in call order and increments its sequence. An empty chunk is a no-op. `reset(source = '')` starts a new generation, clears pending work and preview state, and allows appends even after `finish()`. It validates the replacement before discarding the previous source. `finish()` flushes queued chunks, marks the generation complete, and requests an ordinary full-source parse with no synthetic preview. Repeated `finish()` is a no-op. `append()` after finish throws `invalid_state`; the caller must reset first. `dispose()` is idempotent and terminal. Other methods after disposal throw `invalid_state`. `getSource()` returns the canonical raw string on demand; it does not subscribe to every update. It rejects after disposal.

A stream controller owns its source and native session even when its view unmounts. Remounting with the same live controller rebinds and shows the newest snapshot. At most one view may bind a controller at a time; a second binding reports `session_bound`. Unmount does not call `finish()` or `dispose()`; the app disposes when the message is no longer needed. Completed sessions remain bindable until disposal. Controlled views own no stream controller.

`preview` is off by default, so an open span renders as the literal text the writer sent. `true` enables every repair and `{ emphasis, inlineCode, links, images }` enables the ones named, as remend's options do. An unfinished link or image previews as its text with the partial target dropped, so it is never tappable before its own `)` arrives. The policy applies only to the stream's unstable tail, is proven by reparsing before it takes effect, never touches `getSource()` or source ranges, and is dropped entirely by `finish()`. `setPreview(options)` changes it mid-stream and restyles the current tail without waiting for new input. The settled document is always a parse of the canonical source; see [provisional styling](streaming-semantics.md#provisional-styling).

## Events and failures

`onLinkPress` receives the complete validated URL and the range of its Markdown occurrence, including delimiters. A reference link's range covers its use, not its definition. The library does not open a URL itself. Only absolute `http:`, `https:`, and `mailto:` targets without control characters are actionable. Scheme matching is case-insensitive. Relative, incomplete, and disallowed targets are plain text and never emit link events. `onError` on the controller receives session failures even while no view is mounted. `onError` on the view receives controlled-source or view/layout failures. An event carries `code`, `message`, `generation`, `sequence`, and `recoverable`; errors from an old generation are discarded. Invalid synchronous calls throw a typed error with the same code. Failed append or reset calls leave source, generation, and last valid snapshot unchanged. Asynchronous parse/render failures retain the last valid snapshot and report one error for the failed sequence. A later append or reset may recover.

No parser model or AST is sent to JavaScript for display. The stream controller is a source/session handle, and the view binds by session identity. Native parse and view application both reject stale generation or sequence results.

## Source ranges and limits

All public offsets are zero-based UTF-16 code units into the canonical raw JavaScript string, with a half-open `[start, end)` convention. Native storage is UTF-8; conversion happens at the boundary. Ranges must satisfy `0 <= start <= end <= source.length` and must not start or end between a high and low surrogate. Invalid ranges report `invalid_input`; they are never rounded. The first release exposes ranges for link events only. Copying all source uses `getSource()` or the controlled prop.

Inputs must be well-formed Unicode. An isolated surrogate in a chunk or `reset` source is rejected rather than replaced; a pair split across calls is therefore invalid. Split a stream only at Unicode code point boundaries. CRLF, tabs, NUL, combining marks, emoji, and RTL characters are retained exactly in the canonical source. The parser may normalize line endings for layout, but ranges and copied source always refer to the original string. NUL is displayed as a replacement glyph and is never passed to C string APIs as a terminator.

The initial limits are 1 MiB UTF-8 per source, 64 KiB UTF-8 per append, 256 KiB queued before native acknowledgement, and 128 KiB of speculative preview input. Limit violations report `input_limit` and preserve the last accepted source. If the uncertain tail exceeds 128 KiB, parse the full source without speculative repair; it is not a reason to discard accepted input. Parser nesting and output allocation require measured limits during native implementation. The renderer will cap concurrent image requests at four and skip highlighting above 64 KiB of code when those later features exist. These are contract defaults, not measured device results.

## Platform target

The first release targets iOS and Android with the New Architecture enabled. The first supported app is the repository's React Native 0.83.2 and Expo 55 example, using a development build.

## Implementation status

The native display path renders complete documents on iOS and Android. Parsing, measurement, and drawing are native.

A document is a vertical stack of segments. A text segment is a run of non-table blocks in one platform text view. A table segment is one table in a horizontal scroll view. In a stream, completed blocks are grouped into text segments of about 16 blocks that later snapshots reuse, so only the active tail is rebuilt. Incremental mode commits blocks once md4c proves them closed. Full mode reparses the whole source but gives a block that is unchanged since the previous snapshot a stable identity, so both modes rebuild and lay out only the tail.

### Threading

Parsing and platform text preparation run on one shared pool of worker threads: two fewer than the device's cores, between one and four. The workers run below the UI and JavaScript threads' priority (user-initiated QoS on iOS, background niceness on Android), so under load they yield to input, animation, and app code.

- **Streams.** A session has no thread of its own. While it has new input, one job for it is queued on the pool. The job parses, builds the platform rendering at the width the view was last measured at, and then publishes the snapshot. Fabric's measurement on the JavaScript thread and the view's mount on the UI thread find the rendering cached. Several streams share the pool in turn. A pass publishes even if more input arrived while it parsed, so a stream that outpaces parsing still shows progress.
- **Controlled `markdown`.** A new value starts preparing when its shadow node is created. Every Markdown view in a commit, such as a conversation's history, is therefore prepared in parallel while React finishes the commit. Layout waits for sources up to 16 KiB; if no worker has started one yet, the JavaScript thread runs it itself. A larger source shows the view's previous content until preparation finishes, then relayouts through a Fabric state update without a React render. Equal sources share one parse, so a recycled message is not parsed or laid out again.
- **Android.** Text segments are built as `PrecomputedText` on the worker (Android 9 and later). A non-selectable text view shows it with a static layout, so the UI thread breaks lines without measuring glyphs again.

| Area | Current behavior | Planned |
| --- | --- | --- |
| Input | Controlled `markdown` and `createMarkdownStream` with a `stream` prop. The stream buffers up to 64 KiB per native append and 256 KiB of unacknowledged input. Lone surrogates and sources over 1 MiB are rejected in JavaScript. | Device traces will choose the default full or incremental parse mode. |
| Line breaks | Soft line breaks display as line breaks, as the fixture contract requires. | None |
| Tables | Columns take their widest cell, clamped to 56–280 points plus padding, with md4c's column alignment. Wide tables scroll horizontally. Cells keep inline styles and links. Completed rows appear during streaming without resetting the scroll position. | None |
| Admonitions and footnotes | Admonitions render as a quote with a colored bar and title. Footnote references are superscript numbers; definitions follow a rule at the end. | None |
| Links | Tap hit testing emits `onLinkPress` with a validated URL and the UTF-16 range of its Markdown occurrence, in text and table cells. VoiceOver and TalkBack expose each link as a focusable link role. | Long press actions in a later release. |
| Selection | `selectable` enables native selection and copy in text segments. Copy is plain visible text; `getSource()` returns the complete canonical stream source. Selection cannot span a table or completed segment boundary. | Per-block copy controls. |
| Accessibility | VoiceOver and TalkBack expose reading stops for text blocks, table rows, and links, with heading and link roles and spoken task state. | Rich table navigation if needed. |
| RTL | Text uses natural writing direction. List markers, quote bars, code backgrounds, rules, and table columns mirror for RTL content. | None |
| Code blocks | Fenced blocks are colored from a shared token palette modeled on GitHub's light and dark code themes, so iOS and Android agree. A streaming block is colored from the text it has, and its settled tokens are the same whether it arrived in one piece or a thousand. Selection and copy return the plain code text. | The code block header and copy control, and a theme that can override token colors. |
| Colors | System colors follow light and dark appearance on both platforms. | Style customization after the feature set is fixed. |

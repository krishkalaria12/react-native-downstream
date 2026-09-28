# Markdown display API contract

This is the Phase 0 contract for the native Markdown component. `src/types.ts` holds the public types. `DownstreamMarkdown` with the controlled `markdown` prop is implemented; the stream factory is not a runtime export yet. See [implementation status](#implementation-status) for what the current build covers. Both iOS and Android follow this contract. The first supported app target is the repository's React Native 0.83.2 and Expo 55 example, using a development build.

## Feature scope

| Feature                             | First release                                                                  | Later release                                                          | Unsupported                                                |
| ----------------------------------- | ------------------------------------------------------------------------------ | ---------------------------------------------------------------------- | ---------------------------------------------------------- |
| CommonMark blocks and inline syntax | Paragraphs, headings, lists, quotes, rules, emphasis, code, and escapes        | None planned                                                           | Raw HTML execution                                         |
| GFM tables                          | Progressive rows after a valid separator; horizontal overflow                  | Rich cell content if native layout cannot preserve it at launch        | Raw HTML in cells                                          |
| GFM task lists                      | Readable checkbox state and accessibility label; display only                  | Interactive toggling                                                   | Mutation of Markdown by tapping                            |
| Raw HTML                            | Escaped, visible source text                                                   | None planned                                                           | HTML rendering, scripts, embedded content                  |
| Images                              | Alt text remains readable; no network request                                  | HTTPS images after complete URL validation, placeholders, cancellation | `data:`, `file:`, and custom schemes                       |
| Code                                | Inline code and progressively visible fenced/indented blocks, plain monospace  | Syntax highlighting and copy button                                    | Executing code                                             |
| Math                                | Literal delimiter/source text                                                  | Native math layout after separate review                               | Executing macros or loading remote math content            |
| Links                               | Valid complete `https:`, `http:`, and `mailto:` links; press callback          | Long press actions                                                     | Automatic opening of custom schemes                        |
| Custom renderers                    | None                                                                           | None planned                                                           | Per-node React render callbacks in the native display path |
| Selection                           | Select/copy visible text and complete canonical source                         | Block-specific copy controls                                           | Editing inside the display view                            |
| Accessibility                       | Text reading order, heading and link roles, task state, font scaling, RTL text | Rich table navigation if needed                                        | Platform-specific semantics that change source meaning     |

The same feature matrix applies on iOS and Android. The first release is not complete until the first-release cells work on both. A native renderer may show unsupported syntax as literal text; it must never silently execute it. For oracle fixtures, md4c's parsed HTML records syntax interpretation. The display policy above is a separate projection of that parse.

## Input modes

```tsx
<DownstreamMarkdown markdown={message} onLinkPress={handleLink} />;

const stream = createMarkdownStream({ onError: handleStreamError });
<DownstreamMarkdown stream={stream} />;
stream.append('hello ');
stream.append('**world**');
stream.finish();
```

The examples describe the future runtime API. A view receives exactly one of `markdown` or `stream`. TypeScript rejects both or neither. Native code also rejects both at runtime with `invalid_input` and retains the last valid display. A controlled string replacement starts a new generation when its value changes. An unchanged value, including a React rerender, does not reset work. `stream` identity changes unbind the previous controller and bind the new one; they do not copy source between controllers.

`createMarkdownStream(options?)` creates an active, empty controller. `append(chunk)` adds a nonempty chunk in call order and increments its sequence. An empty chunk is a no-op. `reset(source = '')` starts a new generation, clears pending work and preview state, and allows appends even after `finish()`. It validates the replacement before discarding the previous source. `finish()` flushes queued chunks, marks the generation complete, and requests an ordinary full-source parse with no synthetic preview. Repeated `finish()` is a no-op. `append()` after finish throws `invalid_state`; the caller must reset first. `dispose()` is idempotent and terminal. Other methods after disposal throw `invalid_state`. `getSource()` returns the canonical raw string on demand; it does not subscribe to every update. It rejects after disposal.

A stream controller owns its source and native session even when its view unmounts. Remounting with the same live controller rebinds and shows the newest snapshot. At most one view may bind a controller at a time; a second binding reports `session_bound`. Unmount does not call `finish()` or `dispose()`; the app disposes when the message is no longer needed. Completed sessions remain bindable until disposal. Controlled views own no stream controller.

## Events and failures

`onLinkPress` receives the complete validated URL and the range of its Markdown occurrence, including delimiters. A reference link's range covers its use, not its definition. The library does not open a URL itself. Only absolute `http:`, `https:`, and `mailto:` targets without control characters are actionable. Scheme matching is case-insensitive. Relative, incomplete, and disallowed targets are plain text and never emit link events. `onError` on the controller receives session failures even while no view is mounted. `onError` on the view receives controlled-source or view/layout failures. An event carries `code`, `message`, `generation`, `sequence`, and `recoverable`; errors from an old generation are discarded. Invalid synchronous calls throw a typed error with the same code. Failed append or reset calls leave source, generation, and last valid snapshot unchanged. Asynchronous parse/render failures retain the last valid snapshot and report one error for the failed sequence. A later append or reset may recover.

No parser model or AST is sent to JavaScript for display. The stream controller is a source/session handle, and the view binds by session identity. Native parse and view application both reject stale generation or sequence results.

## Source ranges and limits

All public offsets are zero-based UTF-16 code units into the canonical raw JavaScript string, with a half-open `[start, end)` convention. Native storage is UTF-8; conversion happens at the boundary. Ranges must satisfy `0 <= start <= end <= source.length` and must not start or end between a high and low surrogate. Invalid ranges report `invalid_input`; they are never rounded. The first release exposes ranges for link events only. Copying all source uses `getSource()` or the controlled prop.

Inputs must be well-formed Unicode. An isolated surrogate in a chunk or `reset` source is rejected rather than replaced; a pair split across calls is therefore invalid. Split a stream only at Unicode code point boundaries. CRLF, tabs, NUL, combining marks, emoji, and RTL characters are retained exactly in the canonical source. The parser may normalize line endings for layout, but ranges and copied source always refer to the original string. NUL is displayed as a replacement glyph and is never passed to C string APIs as a terminator.

The initial limits are 1 MiB UTF-8 per source, 64 KiB UTF-8 per append, 256 KiB queued before native acknowledgement, and 128 KiB of speculative preview input. Limit violations report `input_limit` and preserve the last accepted source. If the uncertain tail exceeds 128 KiB, parse the full source without speculative repair; it is not a reason to discard accepted input. Parser nesting and output allocation require measured limits during native implementation. The renderer will cap concurrent image requests at four and skip highlighting above 64 KiB of code when those later features exist. These are contract defaults, not measured device results.

## Platform and baseline target

The first release targets iOS and Android with the New Architecture enabled. Phase 1's fixed baseline device targets are a physical iPhone 13 and Pixel 6a, both measured in release builds at their native refresh rates. Record exact OS versions, thermal state, and hardware identifiers with each trace. Device availability and any substitutions must be recorded before comparison; no physical device or runtime compatibility has been verified in Phase 0.

## Implementation status

The static native display path renders complete documents on iOS and Android. Parsing, measurement, and drawing are native; see [the renderer decision](decisions/renderer-ownership.md).

| Area | Current behavior | Planned |
| --- | --- | --- |
| Input | `markdown` prop only. Lone surrogates and sources over 1 MiB are rejected in JavaScript with `invalid_input` or `input_limit`, and the last valid source stays visible. | `createMarkdownStream` and the `stream` prop in the session phase |
| Line breaks | Soft line breaks display as line breaks, as the fixture contract requires. | None |
| Tables | Rows render as tab-separated lines with a bold header and a header rule. Column alignment is parsed but not applied. | Stable column sizing and horizontal overflow in the feature phase |
| Admonitions and footnotes | md4c's GFM dialect enables both. Admonitions render as a titled quote with a colored bar. Footnote references are superscript numbers; definitions follow a rule at the end. | None |
| Links | Tap hit-testing emits `onLinkPress` with the validated URL and the UTF-16 range of the whole Markdown occurrence. VoiceOver exposes links as custom actions on each block; TalkBack exposes them as custom actions on the view. | Long press in the feature phase |
| Accessibility | iOS exposes one element per block, with the header trait on headings and task state in the label. Android exposes the whole view with a readable description; headings have no separate role yet. | Per-block Android nodes and heading roles in the feature phase |
| Selection | `selectable` is accepted and ignored. | Selection and copy in the feature phase |
| RTL | Text uses natural writing direction, but indents, quote bars, and markers are drawn on the left. | Mirrored decorations in the feature phase |
| Colors | System colors follow light and dark mode on both platforms. There is no style prop yet. | Style customization after the feature set is fixed |


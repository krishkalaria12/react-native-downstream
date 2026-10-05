# Markdown display API contract

This is the Phase 0 contract for the native Markdown component. `src/types.ts` holds the public types. `DownstreamMarkdown` accepts a controlled `markdown` prop or a controller from `createMarkdownStream`. See [implementation status](#implementation-status) for what the current build covers. Both iOS and Android follow this contract. The first supported app target is the repository's React Native 0.83.2 and Expo 55 example, using a development build.

## Feature scope

| Feature                             | First release                                                                  | Later release                                                          | Unsupported                                                |
| ----------------------------------- | ------------------------------------------------------------------------------ | ---------------------------------------------------------------------- | ---------------------------------------------------------- |
| CommonMark blocks and inline syntax | Paragraphs, headings, lists, quotes, rules, emphasis, code, and escapes        | None planned                                                           | Raw HTML execution                                         |
| GFM tables                          | Progressive rows after a valid separator; horizontal overflow                  | Rich cell content if native layout cannot preserve it at launch        | Raw HTML in cells                                          |
| GFM task lists                      | Readable checkbox state and accessibility label; display only                  | Interactive toggling                                                   | Mutation of Markdown by tapping                            |
| Raw HTML                            | Escaped, visible source text                                                   | None planned                                                           | HTML rendering, scripts, embedded content                  |
| Images                              | HTTPS images after complete URL validation, with placeholders, alt-text fallback, cancellation, a save control, and `onImagePress` | Images inside text, headings and table cells            | `data:`, `file:`, relative, and custom schemes; SVG; over 8 KiB destinations |
| Code                                | Inline code, progressively visible fenced/indented blocks, syntax highlighting by the fence's language, line numbers, fence titles, a header with copy, download, wrap, and expand controls, and a capped height that scrolls inside the card | Drawing line marks; a theme that can override token colors | Executing code, and highlighting above 64 KiB of code       |
| Math                                | `math` is off by default, so a document without it is byte-for-byte unchanged. With it on: `$$…$$`, a `$$` line fence and a ` ```math ` fence are display math, `singleDollarTextMath` is off as in Streamdown, `\(…\)` and `\[…\]` are on and can be turned off, and a formula that cannot be laid out keeps its literal and reports `onMathError`. Typesetting itself needs the opt-in engine package. | Macros, images inside a formula, and a display formula wider than its column | Executing macros or loading remote math content            |
| Links                               | Valid complete `https:`, `http:`, and `mailto:` links; press callback          | Long press actions                                                     | Automatic opening of custom schemes                        |
| Custom renderers                    | None                                                                           | None planned                                                           | Per-node React render callbacks in the native display path |
| Theming                             | `theme` and `markdownStyle`, per-element precision, both presets, and a VS Code converter | Per-node themes inside one document                             | Style objects, stylesheet context, and cascading selectors  |
| Selection                           | Select/copy visible text and complete canonical source, plus per-block copy and download on code and table blocks | Long press actions                                            | Editing inside the display view                            |
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

// Controls are on by default. A callback replaces the default silently.
<DownstreamMarkdown
  markdown={message}
  controls={{
    code: {
      copy: { onCopy: (event) => track('code', event.format) },
      download: { filename: 'snippet', onDownload: save },
    },
    table: { csvSeparator: ';' },
  }}
/>;

// `false` removes the code header and table toolbar. Layout is unchanged.
<DownstreamMarkdown markdown={message} controls={false} />;

// Every long line wraps, and the code header gains a per-block wrap button.
<DownstreamMarkdown markdown={message} codeBlockWrap />;

// Line numbers are on by default. `false` hides them for every block.
<DownstreamMarkdown markdown={message} lineNumbers={false} />;

// A code block's body stops at 400 points and scrolls inside its card.
// `0` removes the cap, and the header's expand button lifts it per block.
<DownstreamMarkdown markdown={message} codeBlockMaxHeight={0} />;

// `selectable` is on by default. An app that opens its own message menu on a
// long press turns it off, or the platform's selection menu wins the gesture.
<DownstreamMarkdown markdown={message} selectable={false} />;

// Text fades in as it arrives, with a caret at the writing point. A stream works
// `isAnimating` out for itself; a controlled view says so.
<DownstreamMarkdown stream={stream} animated caret="block" />;

<DownstreamMarkdown
  markdown={message}
  animated
  isAnimating={isStreaming}
  caret="circle"
/>
```

The examples describe the runtime API. A view receives exactly one of `markdown` or `stream`. TypeScript rejects both or neither. A controlled string replacement starts a new generation when its value changes. An unchanged value, including a React rerender, does not reset work. `stream` identity changes unbind the previous controller and bind the new one; they do not copy source between controllers.

`syntaxHighlighting` is on by default and decides only whether the platform applies the colors the engine computed. A fenced block is colored when its info string names a language the engine has a lexer for; the info string is trimmed, lower-cased, and cut at the first space and at a `:`, so ```TypeScript and ```ts title="a.ts" are TypeScript. A block with no language, an unknown language, and a block over 64 KiB of code stay plain either way. `syntaxHighlighting` only changes colors, never a character, so selection, copy, accessibility text, and link ranges are the same with it on or off. The default palette follows GitHub's light and dark code themes on both platforms; overriding individual token colors is not part of this release. An indented code block has no language and stays plain. A document with no fenced code never loads a lexer, and a fence with a language but no lexer is not auto-detected.

A block that is still growing may be colored only in part while it streams, and the rest is colored as the stream continues. A block that is complete is colored whole: the engine keeps working on it within the pass that colors it, so a block too large for the per-pass time budget ends up fully colored rather than part-colored. The per-pass budget bounds what one snapshot does, not how complete a finished block is.

`createMarkdownStream(options?)` creates an active, empty controller. `append(chunk)` adds a nonempty chunk in call order and increments its sequence. An empty chunk is a no-op. `reset(source = '')` starts a new generation, clears pending work and preview state, and allows appends even after `finish()`. It validates the replacement before discarding the previous source. `finish()` flushes queued chunks, marks the generation complete, and requests an ordinary full-source parse with no synthetic preview. Repeated `finish()` is a no-op. `append()` after finish throws `invalid_state`; the caller must reset first. `dispose()` is idempotent and terminal. Other methods after disposal throw `invalid_state`. `getSource()` returns the canonical raw string on demand; it does not subscribe to every update. It rejects after disposal.

A stream controller owns its source and native session even when its view unmounts. Remounting with the same live controller rebinds and shows the newest snapshot. At most one view may bind a controller at a time; a second binding reports `session_bound`. Unmount does not call `finish()` or `dispose()`; the app disposes when the message is no longer needed. Completed sessions remain bindable until disposal. Controlled views own no stream controller.

`preview` is off by default, so an open span renders as the literal text the writer sent. `true` enables every repair and `{ emphasis, inlineCode, links, images, math }` enables the ones named, as remend's options do. An unfinished link or image previews as its text with the partial target dropped, so it is never tappable before its own `)` arrives. The policy applies only to the stream's unstable tail, is proven by reparsing before it takes effect, never touches `getSource()` or source ranges, and is dropped entirely by `finish()`. `setPreview(options)` changes it mid-stream and restyles the current tail without waiting for new input. The settled document is always a parse of the canonical source; see [provisional styling](streaming-semantics.md#provisional-styling).

## Controls

`controls` takes Streamdown's shape, so a value written for it works as it is. It is `true` by default. Each key is a boolean or an object, and a missing key means on, so `controls={{ table: false }}` keeps the code header and drops the table toolbar without changing any block's layout. `controls={false}` removes both.

```ts
type MarkdownControls =
  | boolean
  | {
      code?:
        | boolean
        | {
            copy?: boolean | { onCopy?: (e) => void; onError?: (e) => void };
            download?:
              | boolean
              | {
                  filename?: string;
                  onDownload?: (e) => void;
                  onError?: (e) => void;
                };
            wrap?: boolean | { onToggle?: (e) => void };
            expand?: boolean | { onToggle?: (e) => void };
          };
      table?:
        | boolean
        | {
            copy?: boolean | { onCopy?: (e) => void; onError?: (e) => void };
            download?:
              | boolean
              | {
                  filename?: string;
                  onDownload?: (e) => void;
                  onError?: (e) => void;
                };
            csvSeparator?: ',' | ';' | '\t' | 'auto';
          };
    };
```

A code block's header shows copy, download, wrap, and expand. Expand appears only on a block taller than `codeBlockMaxHeight`, because that is the block it can do anything for. A table shows a toolbar whose copy and download buttons open a format menu: Markdown, CSV, and TSV for copy, CSV and Markdown for download. A control turns on as soon as its own block is settled, so a finished code block can be copied while the rest of the message is still streaming. A block that is still being written shows its controls disabled, because its content can still change.

Copying is native and needs no permission. A code block copies its plain text, with a zero-width placeholder removed, so an empty fence copies as an empty string. A table copies as a `text/html` flavor first, so pasting into a spreadsheet or a document keeps the rows, columns, and alignment, and falls back to plain text for targets that do not take HTML. A value is quoted in CSV only when it holds the separator, a quote, or a line break, and an inner quote is doubled, which is what Streamdown's web component does; a value in TSV is never quoted, because a tab inside a cell would read as a new column, so a tab, a newline, and a carriage return become the two-character escapes `\t`, `\n`, and `\r` that a spreadsheet shows as text.

`csvSeparator` applies to a download. `csvSeparator: 'auto'` picks the separator the current locale's numbers suggest: `;` where the locale writes decimals with a comma, and `,` everywhere else. The engine is not given a locale database, so the platform resolves this before the call and the two platforms resolve it the same way.

`filename` is a base name. The extension comes from the block, not from the name: a fenced block uses the language's extension from a vendored map, matched without regard to case so `TS` names a `.ts` file, an unknown or absent language gets `.txt`, and a table gets `.csv` or `.md`. A table's download menu is CSV and Markdown, matching the formats the plan defines.

Downloading opens the platform's own save dialog: `UIDocumentPickerViewController` for exporting on iOS, and a `CreateDocument` `ActivityResultContract` on Android. The user chooses the location, and the library never picks one. A cancelled dialog is not an error and reports nothing.

`translations` overrides the button labels, the menu items, and the spoken announcements, using Streamdown's key names (`copyCode`, `downloadFile`, `copied`, `wrapCode`, `unwrapCode`, `copyTable`, `copyTableAsCsv`, and the rest) plus `expandCode` and `collapseCode` for the expand button. `defaultTranslations` holds the English defaults. Unknown keys are ignored.

Every press reaches the matching callback as one `MarkdownControlEvent`:

```ts
type MarkdownControlEvent = {
  block: 'code' | 'table';
  action: 'copy' | 'download' | 'wrap' | 'expand';
  format?: 'text' | 'markdown' | 'csv' | 'tsv';
  wrapped?: boolean; // wrap only
  expanded?: boolean; // expand only
  language: string; // fence language as written; '' for tables
  filename?: string; // downloads
};
```

`language` is the fence's language in the case the writer used: `ts`, `TS`, and `Ts` all come back as written, so compare it case-insensitively. The download name is the one that is case-insensitive, not this field.

`onCopy` and `onDownload` observe a press the library has already carried out, so an app can log, toast, or undo. `onToggle` on `wrap` reports the block's layout after the press, and `onToggle` on `expand` reports whether the block's cap is lifted after it; neither takes over anything: the button still changes the block, and the event is a report. `codeBlockWrap` sets the starting state for every code block and the button changes one block without changing the prop; `codeBlockMaxHeight` is the cap for all of them and expand lifts it for one block. A press on a document that has since been replaced is dropped, so a handler never sees an event for content the reader is no longer looking at.

A control that fails reports `control_failed` to that control's own `onError`, and to the view's `onError` when the control has none, so an app can watch every failure in one place without giving up per-control handling. The error carries `block` and `action` naming the control that failed, so nothing has to be parsed out of the message.

## Streaming animation and the caret

A message can fade its new text in as it arrives, with a caret at the writing point. Both are off by default, and both are drawing only: they never change the canonical source, the settled parse, a source range, a selection, or the accessibility text. See [streaming semantics](streaming-semantics.md#streaming-animation-and-the-caret) for that boundary.

```tsx
// A controlled view says whether text is still arriving, as Streamdown requires.
<DownstreamMarkdown
  markdown={message}
  animated
  isAnimating={isStreaming}
  caret="block"
  onAnimationStart={() => setBusy(true)}
  onAnimationEnd={() => setBusy(false)}
/>;

// A stream works `isAnimating` out from its own controller, so it needs no prop.
const stream = createMarkdownStream();
<DownstreamMarkdown stream={stream} animated caret="circle" />;

// The whole option object, with Streamdown's names and defaults.
<DownstreamMarkdown
  stream={stream}
  animated={{ animation: 'slideUp', sep: 'char', duration: 200, stagger: 20 }}
/>;
```

```ts
type MarkdownAnimateOptions = Readonly<{
  /** 'fadeIn' by default. 'blurIn' and other names draw as 'fadeIn' and warn once. */
  animation?: 'fadeIn' | 'slideUp' | 'blurIn' | (string & {});
  /** Per unit, in ms. 150 by default. */
  duration?: number;
  /** A CSS timing keyword or cubic-bezier(). 'ease' by default. */
  easing?: string;
  /** 'word' by default. CJK text is split per character in either mode. */
  sep?: 'word' | 'char';
  /** Delay between units, in ms. 40 by default; 0 turns the cascade off. */
  stagger?: number;
  /** How far ahead of now a burst may be scheduled, in ms. 320 by default. */
  maxBacklogMs?: number;
}>;
```

`animated` is `false` by default, or the option object. `animation` is `fadeIn` (alpha) or `slideUp` (alpha and a four-point rise). `blurIn` draws as `fadeIn` and warns once in development: neither platform can blur one run of glyphs cheaply inside a text draw pass, and a CSS keyframe name means nothing natively. An unknown name does the same.

A unit is a word by default, and takes its trailing whitespace with it, so a link's underline never shows before its word. CJK is split per syllable in either mode, because a CJK paragraph has no spaces and would otherwise fade as one block. `sep: 'char'` splits per grapheme, keeping a surrogate pair, combining marks, and a ZWJ emoji sequence whole. A rule, a table row, and an empty fence are one unit each.

`stagger` is the delay between units, and a burst is compressed to fit `maxBacklogMs` rather than queued: forty words on a 40 ms stagger are scheduled across about 320 ms, not 1.6 s of invisible text. `stagger: 0` has no floor and turns the cascade off. Every time value is multiplied by Android's animator duration scale, and a scale of zero turns the fade off.

`isAnimating` is required for a controlled `markdown` view, as in Streamdown: the library cannot know whether a string prop is still growing. A `stream` view works it out from its own controller — on from the first append until `finish()` — and an explicit `isAnimating` overrides that. `onAnimationStart` fires when it turns true, including on mount; `onAnimationEnd` fires when it turns false, but not on mount. Neither fires while the view is not animating at all.

`caret` is `'none'` by default, or `'block'` (a bar) or `'circle'` (a dot). It is drawn, not typeset, so it looks the same in every font and cannot change a line's height. It is shown while `isAnimating` is true, so the two props are needed together.

The caret sits after the last character when the last block is a paragraph, a heading, a list item at any depth, or a fence that is still open — that is where text is being written. It takes a line of its own when the document is empty, or ends with a table, a closed fence, or a rule, because that is where the next block's first line will appear. While a fade is running it is drawn at the reveal frontier, the end of the last word that has started, so it never sits after a gap of words that have not appeared yet.

The caret is never copied, selected, exported, or read aloud. Copying a selection that touches it writes the selection without it. The placeholder it reserves is measured with the text, so measured height equals drawn height, and a caret that wraps to a new line moves the document by that line. The final snapshot removes it, so a view shrinks by a line when a stream finishes, as Streamdown's `::after` does.

No fade is drawn when the system asks for reduced motion: `UIAccessibilityIsReduceMotionEnabled()` on iOS, and Android's Reduce Motion and animator duration scale as well. The caret is unaffected, because it does not move. With `animated` and `caret` off, a view's output is byte-identical to one built without this feature.

## Events and failures

`onLinkPress` receives the complete validated URL and the range of its Markdown occurrence, including delimiters. A reference link's range covers its use, not its definition. The library does not open a URL itself. Only absolute `http:`, `https:`, and `mailto:` targets without control characters are actionable. Scheme matching is case-insensitive. Relative, incomplete, and disallowed targets are plain text and never emit link events. `onError` on the controller receives session failures even while no view is mounted. `onError` on the view receives controlled-source or view/layout failures. An event carries `code`, `message`, `generation`, `sequence`, and `recoverable`; errors from an old generation are discarded. Invalid synchronous calls throw a typed error with the same code. Failed append or reset calls leave source, generation, and last valid snapshot unchanged. Asynchronous parse/render failures retain the last valid snapshot and report one error for the failed sequence. A later append or reset may recover.

No parser model or AST is sent to JavaScript for display. The stream controller is a source/session handle, and the view binds by session identity. Native parse and view application both reject stale generation or sequence results.

## Theming

Two props, at two altitudes. `theme` sets what the document is made of: colors, fonts, and metrics. `markdownStyle` sets one element at a time. Both take a single value or an array merged left to right, so a preset is an override you extend rather than a starting point you replace:

```tsx
<DownstreamMarkdown
  markdown={message}
  theme={[{ colors: { foreground: '#101418' } }, presets.streamdown.theme]}
  markdownStyle={[presets.github.markdownStyle, { h1: { fontWeight: 700 } }]}
/>
```

An array is applied left to right and merged per leaf, so a later element sets only what it names. It is the same merge a single value uses internally, so `{...a, ...b}` on two objects is a different and worse thing.

### Colors

A color is a `ColorValue` or `{ light, dark }`, and either half may be left out to keep the default for that appearance:

```tsx
theme={{
  colors: {
    foreground: { light: '#1b1b1f' },          // only the light half
    primary: { light: '#7a3e9d', dark: '#c39be0' },
  },
}}
```

A color slot the theme does not set draws its platform's own semantic color, so it keeps following the appearance and the reader's increased-contrast setting with no React render. That is the whole reason an unset slot is a role rather than a value: `label` on iOS means the system's label color, not one frozen integer. A `PlatformColor` is refused with one warning, because its resolved value depends on the view it is attached to and the engine resolves slots rather than colors.

Both renderers read one generated slot table, so a theme draws the same on each platform and a token cannot drift between them.

### Metrics and fonts

`theme` also carries the layout numbers: font sizes, weights, families, line heights, margins, indents, gaps, paddings, radii, and border widths. Only metrics change layout, and only those are measured again: a colors-only change redraws without a single remeasure, because the shadow node keys its measurement on a hash of the size-changing slots alone.

`fonts` names three families, `sans`, `heading`, and `mono`, and an element's own `fontFamily` overrides the role. A family that is not registered falls back to the platform font and reports one warning per family, so a font `expo-font` has not loaded yet is visible rather than silently substituted.

A weight is a CSS weight, so any number from 1 to 1000 is accepted and the platform resolves it to the nearest face it has. A weight of 400 is the family itself.

A slot the theme sets and a slot it leaves alone are the same number and mean different things, and the difference is visible. A `strong` that named a weight means exactly that weight; one that named only a family raises the block's own weight instead, so bold inside a bold heading still reads as bolder rather than as the same weight. A `codeBlock.borderRadius` the theme named is final; one it did not falls back to the shared `radius`. An `em` that named no slant keeps the block's, so a quote that is already italic does not lose it to an `em` that never ran.

A quote and a list are elements in their own right. A theme that sets `blockquote.fontSize` restyles the text inside every quote without naming a single block, and `list.fontSize` does the same for list items.

`maxFontSizeMultiplier` caps the platform font scale, as `<Text>`'s does. It can cap Dynamic Type and Android's font scale and cannot turn scaling off; below 1 is the unset value and is ignored, because capping below the current scale would shrink text the reader asked to enlarge.

### markdownStyle

`markdownStyle` uses `react-native-enriched-markdown`'s key and property names for every key this renderer draws, so a value written for the closest native peer works as it is:

```tsx
markdownStyle={{
  h1: { fontSize: 32, marginBottom: 16 },
  blockquote: { borderColor: '#7a3e9d', borderWidth: 3 },
  list: { bulletColor: '#7a3e9d', marginLeft: 20 },
  codeBlock: { padding: 16, borderRadius: 8 },
}}
```

A key this renderer does not draw is reported once in development rather than ignored, so nobody believes a value took effect. A property it does not draw is dropped without a warning of its own, because the key it belongs to is the useful part of the message.

### Syntax colors

`theme.syntax` takes the engine's token classes, and `markdownStyle.codeBlock.syntaxColors` takes those plus enriched-markdown's own keys. For a full editor theme, read Shiki's own JSON and convert it:

```tsx
import { syntaxColorsFromTheme } from 'react-native-downstream';

const converted = syntaxColorsFromTheme(vscodeThemeJson);
<DownstreamMarkdown
  markdown={message}
  theme={{ syntax: converted.syntax }}
  markdownStyle={{ codeBlock: converted.codeBlock }}
/>
```

The converter maps TextMate scopes onto the engine's classes and returns the code block's own two colors from `editor.background` and `editor.foreground`, for `markdownStyle.codeBlock` rather than `theme.colors`, which would recolor prose and tables too. `punctuation` maps to `operator`, and `variable` and `embedded` map to `plain`, which is the rule enriched applies to them. The goldens are the GitHub light and dark code themes plus Dracula, checked against the same table the renderers use.

### Presets

`presets.github` and `presets.streamdown` are the two themes both renderers are checked against. A preset is `{ theme, markdownStyle }`, so it spreads onto the view.

## Source ranges and limits

All public offsets are zero-based UTF-16 code units into the canonical raw JavaScript string, with a half-open `[start, end)` convention. Native storage is UTF-8; conversion happens at the boundary. Ranges must satisfy `0 <= start <= end <= source.length` and must not start or end between a high and low surrogate. Invalid ranges report `invalid_input`; they are never rounded. The first release exposes ranges for link events only. Copying all source uses `getSource()` or the controlled prop.

Inputs must be well-formed Unicode. An isolated surrogate in a chunk or `reset` source is rejected rather than replaced; a pair split across calls is therefore invalid. Split a stream only at Unicode code point boundaries. CRLF, tabs, NUL, combining marks, emoji, and RTL characters are retained exactly in the canonical source. The parser may normalize line endings for layout, but ranges and copied source always refer to the original string. NUL is displayed as a replacement glyph and is never passed to C string APIs as a terminator.

The initial limits are 1 MiB UTF-8 per source, 64 KiB UTF-8 per append, 256 KiB queued before native acknowledgement, and 128 KiB of speculative preview input. Limit violations report `input_limit` and preserve the last accepted source. If the uncertain tail exceeds 128 KiB, parse the full source without speculative repair; it is not a reason to discard accepted input. Parser nesting and output allocation require measured limits during native implementation. The renderer caps concurrent image requests at four and skips highlighting above 64 KiB of code. These are contract defaults, not measured device results.

## Platform target

The first release targets iOS and Android with the New Architecture enabled. The first supported app is the repository's React Native 0.83.2 and Expo 55 example, using a development build.

## Implementation status

The native display path renders complete documents on iOS and Android. Parsing, measurement, and drawing are native.

A document is a vertical stack of segments. A text segment is a run of non-table, non-code blocks in one platform text view. A code segment is one fenced or indented block with its own header. A table segment is one table in a horizontal scroll view. In a stream, completed blocks are grouped into text segments of about 16 blocks that later snapshots reuse, so only the active tail is rebuilt. Incremental mode commits blocks once md4c proves them closed. Full mode reparses the whole source but gives a block that is unchanged since the previous snapshot a stable identity, so both modes rebuild and lay out only the tail.

Theming is on both platforms. A theme is interned by content in the shadow node, so equal themes are one object and a theme change costs one intern rather than one per view. The interned theme travels with the mounting state as its id, the way the parsed document does, so nothing copies 232 slot values across the platform boundary on every update. A color-only change rebuilds the drawing objects and measures nothing; a size change is measured again like any other layout change.

Streaming animation works the same way. What is new, where the unit boundaries fall, when each unit starts, and where the caret goes are all decided in the engine, in `src/cpp/downstream/Reveal.h` and `Caret.h`, and both renderers call those. A per-view tracker holds the document that view last showed, so a message scrolled back into view does not fade all of itself again. None of that lives in a rendering: a segment is shared and immutable, so alpha can only be applied in a draw pass. iOS splits `DSLayoutManager`'s glyph drawing into runs of equal alpha and redraws only the lines holding an active unit. Android draws a text segment in clipped passes, one outside the fading band and one per alpha bucket inside it, capped at eight layers a frame. The caret is the exception, because it takes space: its placeholder is a real character in the measured text, so it is part of the rendering and of the measurement, and of both platforms' rendering cache keys.

The rules that decide what a block draws with are written once, in the engine, and both renderers call them. A document read resolves each block's typography, its element, and its two margins on the C++ side and hands the result to the platform, so a theme means one thing rather than one thing per platform. A theme slot that nothing reads is a value that resolves, interns, redraws, and never appears, which is why `pnpm test:native` fails when one is added.

### Line numbers, titles, and the height cap

`lineNumbers` is on by default, as in Streamdown. Numbers are drawn beside the code in a gutter that does not scroll sideways with it, so a long line stays readable while the code scrolls. They are drawn rather than typed: they are never selected, never copied, and never read aloud. The gutter draws in the theme's own `codeBlock.color`, dimmed, in the theme's code font and size, and the card's padding, background, radius, and border are the theme's, so a theme restyles a code block and its numbers together. There is no separate gutter slot: the numbers annotate the code, so they follow it. `lineNumbers={false}` hides them for every block; a fence's own `noLineNumbers` hides them for that block only, and the prop wins over it.

A fence's info string is parsed once by the engine, and the header label, the gutter, and the download name all read that one result. The grammar, and the spellings each renderer uses, are:

| Token | Effect |
| --- | --- |
| `startLine=N` | Numbers begin at N, from 1 to 1000000. Below 1 or above that bound is ignored, so the gutter stays a readable width. |
| `showLineNumbers=N`, `showLineNumbers{N}` | The same, as Docusaurus and rehype-pretty code write it. `startLine` wins if both appear. |
| `noLineNumbers` | Hides numbers for that block. An exact token. |
| `title="…"`, `title='…'`, `title=bare`, `filename="…"` | The header's label, replacing the language. The first one wins. Docusaurus, rehype-pretty code, Expressive Code, and Nextra spell it this way. |
| `lang:path` in the first word | The label, when no `title=` is present. |
| `{1,3-5}` | Parsed and kept, so a mark list can never reach a title. Drawing them is not part of this release. |

Anything else in the info string is ignored and stays available to a custom renderer. A title loses control characters and the bidi embedding, override, and isolate characters, and is capped at 96 UTF-16 units, so a model cannot write a label that reads as though it named something else. While a fence is still being written, a title with no closing quote yet shows the language instead of a wrong name.

`codeBlockMaxHeight` is the tallest a code block's body grows before it scrolls inside its own card, in points. It is 400 by default, as in Streamdown. `0`, `'0'`, `'none'`, `Infinity`, and `'Infinity'` remove the cap. A number is points, as React Native's own layout props are; a CSS length has no meaning here and falls back to the default with a development warning, as do a negative and a `NaN`.

A capped block is never shorter than five lines, whatever the text size, because a 400-point cap at the largest Dynamic Type size would otherwise show two. While a capped block is still streaming it follows its newest line, so a reader watching a long fence arrive sees its end rather than its beginning; scrolling away stops that, scrolling back resumes it, and the block keeps its position once the stream settles. A vertical drag at a capped block's edge scrolls the surrounding list instead, so a scroll view inside a chat list does not trap the gesture.

The expand button lifts the cap for one block, and is the way out of scrolling inside a scrolling chat on a phone. It appears only on a block taller than the cap, and clears on a new generation.


A control turns on as soon as its own block is settled. A controlled document settles every block; a stream settles the blocks it has proven closed, and settles the rest when it finishes. So a finished code block in a still-streaming answer can be copied immediately, and a block that is still being written shows its controls disabled. Settling is a property of the parse, not of the controls, and no control button ever changes whether a block is settled.

### Threading

Parsing and platform text preparation run on one shared pool of worker threads: two fewer than the device's cores, between one and four. The workers run below the UI and JavaScript threads' priority (user-initiated QoS on iOS, background niceness on Android), so under load they yield to input, animation, and app code.

- **Streams.** A session has no thread of its own. While it has new input, one job for it is queued on the pool. The job parses, builds the platform rendering at the width the view was last measured at, and then publishes the snapshot. Fabric's measurement on the JavaScript thread and the view's mount on the UI thread find the rendering cached. Several streams share the pool in turn. A pass publishes even if more input arrived while it parsed, so a stream that outpaces parsing still shows progress.
- **Controlled `markdown`.** A new value starts preparing when its shadow node is created. Every Markdown view in a commit, such as a conversation's history, is therefore prepared in parallel while React finishes the commit. Layout waits for sources up to 16 KiB; if no worker has started one yet, the JavaScript thread runs it itself. A larger source shows the view's previous content until preparation finishes, then relayouts through a Fabric state update without a React render. Equal sources share one parse, so a recycled message is not parsed or laid out again.
- **Android.** Text segments are built as `PrecomputedText` on the worker (Android 9 and later). A non-selectable text view shows it with a static layout, so the UI thread breaks lines without measuring glyphs again.

| Area | Current behavior | Planned |
| --- | --- | --- |
| Input | Controlled `markdown` and `createMarkdownStream` with a `stream` prop. Streams use incremental parsing, share completed blocks, and fall back to a full parse at unproven boundaries. The stream buffers up to 64 KiB per native append and 256 KiB of unacknowledged input. Lone surrogates and sources over 1 MiB are rejected in JavaScript. | Real-device performance measurements. |
| Streaming animation | `animated`, `isAnimating`, `caret`, `onAnimationStart`, and `onAnimationEnd`, on both platforms. The engine decides what is new and when each unit starts; each platform's existing draw pass applies the alpha, so a frame costs no React render, no Fabric state update, no relayout, and no change to a shared segment. | `smooth` pacing for bursty providers, and effects beyond fade and rise. |
| Line breaks | Soft line breaks display as line breaks, as the fixture contract requires. | None |
| Tables | Columns take their widest cell, clamped to 56–280 points plus padding, with md4c's column alignment. Wide tables scroll horizontally. Cells keep inline styles and links. Completed rows appear during streaming without resetting the scroll position. A toolbar copies the table as HTML, Markdown, CSV, or TSV and saves it as CSV or Markdown. | None |
| Admonitions and footnotes | Admonitions render as a quote with a colored bar and title. Footnote references are superscript numbers; definitions follow a rule at the end. | None |
| Links | Tap hit testing emits `onLinkPress` with a validated URL and the UTF-16 range of its Markdown occurrence, in text and table cells. VoiceOver and TalkBack expose each link as a focusable link role. | Long press actions in a later release. |
| Selection | `selectable` enables native selection and copy in text segments. **It is on by default**, applied per settled segment, so text that is still streaming is never selectable. Copy is plain visible text; `getSource()` returns the complete canonical stream source. Selection cannot span a table or completed segment boundary. `controls` adds a per-block copy and download, described under [controls](#controls). | An app that opens its own message menu on a long press must pass `selectable={false}`. |
| Accessibility | VoiceOver and TalkBack expose reading stops for text blocks, table rows, and links, with heading and link roles and spoken task state. | Rich table navigation if needed. |
| RTL | Text uses natural writing direction. List markers, quote bars, code backgrounds, rules, and table columns mirror for RTL content. | None |
| Code blocks | Fenced blocks are colored from a shared token palette modeled on GitHub's light and dark code themes, so iOS and Android agree. A streaming block is colored from the text it has, and its settled tokens are the same whether it arrived in one piece or a thousand. Selection and copy return the plain code text. Each fenced block gets a header with copy, download, a per-block wrap button, and an expand button that appears only on a block past the cap; `codeBlockWrap` sets the starting state for all of them. `lineNumbers` is on by default and draws a gutter that stays put while the code scrolls sideways, numbered from the fence's `startLine` and widened when the last number gains a digit. `codeBlockMaxHeight` caps a block's body at 400 points and lets it scroll inside its card, which follows its newest line while it streams. A fence's `title=` replaces the language in the header, truncating in the middle so a path keeps its file name. None of this changes copied text, selection, or what a screen reader reads. | Drawing `meta.marks` as line backgrounds. A theme that can override token colors. |
| Colors | System colors follow light and dark appearance on both platforms. | Style customization after the feature set is fixed. |

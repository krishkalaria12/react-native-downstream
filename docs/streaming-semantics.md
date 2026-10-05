# Streaming Markdown semantics

The canonical source is the exact concatenation of accepted chunks since the last reset. Preview rendering may use a temporary repaired copy, but never changes canonical source, `getSource()`, source ranges, or the final parse. `finish()` removes all preview repair and renders the raw source through the same complete-document parser used for a controlled view.

## Prefix policy

| Input prefix             | Visible preview before completion                                                                      | Completion rule                                                                                    |
| ------------------------ | ------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------- | --- | ---------------------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| `hello **wor`            | Provisional bold for `wor` only if the `**` opener is unambiguous. If ambiguous, show literal markers. | Closing `**` or `finish()` settles the parse.                                                      |
| `[docs](https://exam`    | Show `docs` as plain text, with no tappable target. Never open or prefetch a partial URL.              | Closing `)` and allowed scheme make it a link. A rejected scheme remains plain text.               |
| `![alt](https://partial` | Show `alt` as text. Do not request an image.                                                           | A complete allowed HTTPS URL may become an image in a later release; first release keeps alt text. |
| `[docs](https://example.com/guide` | A bare URL inside an unfinished `](` is text, never a target, even when the URL is already complete. The next chunk can extend it or close the construct. | The closing `)` settles it. A complete allowed target then becomes a link. |
| ` ```ts\nconst x `       | Show code text as plain monospace. No highlighting or completion-dependent controls.                   | A matching closing fence or `finish()` settles the block.                                          |
| `\| a \| b \|` then `\| --- \|` | Show text, not a table. A following valid separator may reclassify it.      | Valid separator classifies the table; a row stays provisional until its line ends.                 |
| `$x +` or `$$\nx +`      | Show literal math source and delimiters. No remote renderer or macro evaluation.                       | Math stays literal in the first release. Later math support requires a separate policy.            |

Temporary closing markers, when used for emphasis or other preview syntax, exist only in the parser's preview buffer. They have no source offsets and cannot enter copy or error events. A preview is allowed to restyle the active tail when new input arrives. Text order is preserved, and accepted characters never disappear from the canonical source. Completed output must match a fresh full parse of canonical source under the chosen md4c options.

## Provisional styling

The prefix policy above allows a provisional style when an opener is unambiguous. The engine implements that branch in `src/cpp/remend/`, ported from Vercel's `remend`, and **leaves it off by default**: with no options set, every open span renders literally.

When enabled, the pass sees only the unstable tail. In incremental mode that is `source[committedBytes_ .. end]`, and committed blocks live in shared immutable storage the pass cannot reach, so no repair can restyle a block a reader has already seen. Before any commit boundary exists the whole document is the tail, and in full mode the whole document is the tail on every update. A reset or a tail past the retained limit falls back to a full parse, which still previews, with the committed blocks frozen. `finish()` never repairs, in either mode.

A block md4c has already closed is never repaired, because no later input can close a span inside it: a paragraph after a blank line, and a heading or table row after its line ends. A styled preview there would be retracted when the next block starts. A span whose chunk ended on whitespace still previews, with the closer placed before the whitespace.

A candidate repair is kept only when reparsing proves it changed nothing but the block it aimed at: the block count is unchanged, every earlier block is identical down to run ranges and container identity, the last block's text is the original text minus exactly the marker run that was closed, and inline style coverage strictly increased. A repair that cannot be proven is discarded, so the literal reading is always the fallback. A repair that would style nothing is also discarded, which is what keeps a bare `**` or `text ***` literal.

Every open run closes, innermost first, so `**bold *ital` reads as bold and italic and the closers nest the way the writer's will. Each closer is proven on its own; one that fails, such as an escaped star, is skipped rather than hiding the opener before it. Math delimiters, raw HTML, and list and thematic-break markers are never openers: a repair there would invent syntax the writer did not type.

A single tilde never opens a span. md4c's GitHub dialect reads `~x~` as strikethrough, so closing one would invent a span the writer did not ask for.

An unfinished link or image previews as its text, the way Streamdown shows it. `[text` and `[text](https://exa` read as `text`, and `![alt](https://exa` as `alt`, which is how a finished image renders. The partial target is dropped rather than followed, so nothing is tappable until the link's own `)` arrives, and a stream that finishes first shows the syntax as typed. `[note](which is long` stays literal: a space ends a bare destination, so it is prose that will never be a link.

## Streaming animation and the caret

Animation and the caret are drawing only. They never change the canonical source, the settled parse, a source range, the visible text a reader can select or copy, or the accessibility tree. A document with `animated` and `caret` off is byte-identical to one built without them.

What fades is decided against the document the view last showed, not against the source. Two texts are compared after the parser has removed the markers, so a character keeps its identity when a `**` closes around it: `hello **wor` followed by `hello world` makes only `ld` new, where a count of the visible length would have shown the closing word at once. A word split across two snapshots joins the unit already fading, and keeps its start time.

Unit timing lives in a per-view tracker and not in any rendered object, so a snapshot that changes a block can cut a fade short or leave it running without anything having to be retracted from the screen. A unit whose characters still align keeps its start time across snapshots; a unit that loses them is dropped. A character drawn at full alpha is never scheduled again, which is the invariant the tracker exists to keep and the one a re-render must not break.

`finish()` does not stop the last fades. The final snapshot is still scheduled, every fade runs to its end, and a caret that wrapped to its own line takes that line with it. `reset()` clears the tracker, so the new source animates from its first word rather than resuming the old one's timing. A view that binds to a live stream is seeded from the session's own record of what it has already put on screen, so a message scrolled out of a virtualized list and back does not fade all of itself again; a controlled view has no session, and a fresh one shows its first document without a fade.

Hidden text is still in the layout and still readable by a screen reader, for at most `maxBacklogMs + duration` — about half a second. The caret is drawn at the reveal frontier rather than at the end of the layout so the writing point stays visible, and the caret placeholder is outside every block's range, so it is never selected, copied, exported, or read aloud.

## Classification and ordering

A newline alone never commits a block. A following setext underline, table separator, list continuation, reference definition, quote continuation, or fence close may change earlier classification. Until the native implementation proves a safe boundary, it reparses the full source. A later tail optimization must fall back to full parse when context is uncertain and pass every prefix case in the parser oracle.

Chunks are applied in call order to one session. A coalesced batch preserves the same order. Each reset changes generation; each accepted nonempty append advances sequence within that generation. `finish()` makes the final state observable after all earlier appends. Results and measurements carry generation and sequence, and the view only applies the current pair. Old worker results after reset, unmount, or rebinding cannot repaint the view. Width or style changes also invalidate stale measurements.

The parser oracle in `src/cpp/tests/oracle.mjs` holds each document with its exact chunk strings and the visible text, links, and block shape expected at every chunk boundary. The renderer's safe projection, such as escaped raw HTML, alt text instead of images, and literal math, is governed by [the API matrix](api.md#feature-scope). The boundaries are asserted against the visible text a reader would see, so a checkpoint describes behaviour rather than a serialised artifact. The documents themselves are in `benchmarks/samples.ts`, which the benchmarks and the example app replay.

## Unresolved implementation details

- The exact syntax-highlighting and image APIs belong to a later release. Their first-release fallback is fixed above.
- Native parser nesting and rendered-output limits need device measurements. The accepted source and queue limits are fixed in the API contract.

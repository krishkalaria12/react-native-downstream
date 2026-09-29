# Streaming Markdown semantics

The canonical source is the exact concatenation of accepted chunks since the last reset. Preview rendering may use a temporary repaired copy, but never changes canonical source, `getSource()`, source ranges, or the final parse. `finish()` removes all preview repair and renders the raw source through the same complete-document parser used for a controlled view.

## Prefix policy

| Input prefix             | Visible preview before completion                                                                      | Completion rule                                                                                    |
| ------------------------ | ------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------- | --- | ---------------------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| `hello **wor`            | Provisional bold for `wor` only if the `**` opener is unambiguous. If ambiguous, show literal markers. | Closing `**` or `finish()` settles the parse.                                                      |
| `[docs](https://exam`    | Show `docs` as plain text, with no tappable target. Never open or prefetch a partial URL.              | Closing `)` and allowed scheme make it a link. A rejected scheme remains plain text.               |
| `![alt](https://partial` | Show `alt` as text. Do not request an image.                                                           | A complete allowed HTTPS URL may become an image in a later release; first release keeps alt text. |
| ` ```ts\nconst x `       | Show code text as plain monospace. No highlighting or completion-dependent controls.                   | A matching closing fence or `finish()` settles the block.                                          |
| `                        | A                                                                                                      | B                                                                                                  | `   | Show text, not a table. A following valid separator may reclassify it. | Valid separator classifies the table; a row stays provisional until its line ends. |
| `$x +` or `$$\nx +`      | Show literal math source and delimiters. No remote renderer or macro evaluation.                       | Math stays literal in the first release. Later math support requires a separate policy.            |

Temporary closing markers, when used for emphasis or other preview syntax, exist only in the parser's preview buffer. They have no source offsets and cannot enter copy or error events. A preview is allowed to restyle the active tail when new input arrives. Text order is preserved, and accepted characters never disappear from the canonical source. Completed output must match a fresh full parse of canonical source under the chosen md4c options.

## Classification and ordering

A newline alone never commits a block. A following setext underline, table separator, list continuation, reference definition, quote continuation, or fence close may change earlier classification. Until the native implementation proves a safe boundary, it reparses the full source. A later tail optimization must fall back to full parse when context is uncertain and pass every prefix case in the parser oracle.

Chunks are applied in call order to one session. A coalesced batch preserves the same order. Each reset changes generation; each accepted nonempty append advances sequence within that generation. `finish()` makes the final state observable after all earlier appends. Results and measurements carry generation and sequence, and the view only applies the current pair. Old worker results after reset, unmount, or rebinding cannot repaint the view. Width or style changes also invalidate stale measurements.

The parser oracle in `src/cpp/tests/oracle.mjs` holds each document with its exact chunk strings and the visible text, links, and block shape expected at every chunk boundary. The renderer's safe projection, such as escaped raw HTML, alt text instead of images, and literal math, is governed by [the API matrix](api.md#feature-scope). The boundaries are asserted against the visible text a reader would see, so a checkpoint describes behaviour rather than a serialised artifact. The documents themselves are in `__benchmarks__/samples.ts`, which the benchmarks and the example app replay.

## Unresolved implementation details

- The exact syntax-highlighting and image APIs belong to a later release. Their first-release fallback is fixed above.
- Native parser nesting and rendered-output limits need device measurements. The accepted source and queue limits are fixed in the API contract.

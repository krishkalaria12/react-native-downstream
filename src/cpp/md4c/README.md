# Vendored md4c

These files come from [md4c](https://github.com/mity/md4c) at commit
`902b1bc9172ab45a7ea2539c9db4496ead77daa8` (0.6.0). md4c is MIT licensed; see
`LICENSE.md`.

| File | Used by |
| --- | --- |
| `md4c.c`, `md4c.h` | The native parser on iOS and Android |
| `entity.c`, `entity.h` | HTML entity decoding in text and link targets |

`md4c-html.c` and `md4c-html.h` are not vendored. They produced the HTML oracle
that the host tests used to compare against; the tests now assert the engine's
own render model, so the HTML renderer had no remaining caller.

## Unmodified

`pnpm test:native` pins these four files by SHA-256. The digests live in
`src/cpp/tests/engine.test.mjs` under `vendored md4c`.

A local edit is either a deliberate patch, which should be recorded there on
purpose, or an accident that changes parsing behaviour everywhere. The pin makes
that decision explicit instead of letting it happen quietly.

## Local patches

Two flags and one helper, all guarded by a flag that is off by default, so a
build that sets neither flag parses exactly as 0.6.0 did. Every one is marked
`LOCAL PATCH` in the source.

### `MD_FLAG_LATEXMATHBLOCKS` — a `$$` line is a fence

`md_is_opening_code_fence` and the line classifier that calls it. A line that is
exactly `$$`, after up to three spaces of indentation, with nothing but
whitespace after it, opens or closes a fence reported as `MD_BLOCK_CODE` with
`fence_char = '$'`.

Without it, `$$` alone on a line is only a span opener, so a display formula
containing a blank line, a line starting `- `, a `|`, or four spaces of
indentation is split into several Markdown blocks and the formula is not a
formula. Routing it through the existing fence machinery rather than writing a
new block type is what makes container nesting, lazy continuation, and an
unclosed fence running to the end of its container all work: those are
properties of a fence in md4c, not of math.

`$$` still works as an inline span with `MD_FLAG_LATEXMATHSPANS` alone, which is
what remark-math does and what Streamdown does, so the two cases are
distinguishable and the builder can tell a lone `$$…$$` paragraph from a real
fence.

This is upstream issue [#270](https://github.com/mity/md4c/issues/270), so the
patch is offered upstream and the goal is to drop it once it merges.

### `MD_FLAG_LATEXMATHNOSINGLE` — a single `$` is never math

The dollar-run collector in the inline mark analysis. md4c has no switch between
`$` and `$$`, and a span that pairs `$5 and $10` is a span that later fails to
lay out, at which point the reader sees source that has already lost the
emphasis md4c skipped while resolving the span. Skipping the one-dollar run in
the collector is two lines; the alternative is a second pass over the source
after the fact.

Downstream enables this and lets an app opt back into `$…$` through
`math={{ singleDollarTextMath: true }}`, which is the same name and the same
default as Streamdown's.

## Symbol prefixing

Builds rename the public symbols at compile time: `md_parse` becomes
`downstream_md_parse` and `entity_lookup` becomes `downstream_entity_lookup`.
Apps can link another md4c, and Enriched Markdown vendors an older one; without
the prefix the linker can bind our calls to that copy.
`MarkdownParser.cpp` fails to compile if the prefix is missing. The definitions
live in `Downstream.podspec`, `src/android/src/main/jni/CMakeLists.txt`, and
`src/cpp/tests/engine.test.mjs`.

## Known issue

The check that a list item cannot begin with two blank lines reads the last
line record in md4c's block buffer as a block record, so whether a list ends
after two blank lines depends on a line's byte offset modulo 256.
`src/cpp/session/IncrementalParser.cpp` pads partial parses to full-parse
offsets so incremental output still matches a full parse.

## Updating

Copy the same files from the new revision, update the digests in
`src/cpp/tests/engine.test.mjs`, and run `pnpm test:native`.

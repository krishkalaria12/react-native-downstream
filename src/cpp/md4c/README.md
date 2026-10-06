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

Three flags and two helpers, all guarded by a flag that is off by default, so a
build that sets no flag parses exactly as 0.6.0 did. Every one is marked
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

### `MD_FLAG_CJKFRIENDLY` — the markdown-cjk-friendly amendment

The amendment at
<https://github.com/tats-u/markdown-cjk-friendly/blob/main/specification.md>,
which CommonMark issue [#650](https://github.com/commonmark/commonmark-spec/issues/650)
is tracking. Three sites:

**The delimiter analysis** (`md_collect_marks`, the `*`/`_` branch). CommonMark
decides whether a delimiter run may open or close from three levels of what
surrounds it — whitespace, punctuation, other — and md4c computes them here. The
amendment leaves that test alone and changes what counts as punctuation: a run
next to a CJK character is flanked as though that character were punctuation, so
a CJK neighbour is reclassified from level 2 to level 1.

Three consequences, all of them the amendment's and none of them a departure
from it:

| Source | Before | After |
| --- | --- | --- |
| `これは**「強調」**です` | literal | bold |
| `**[リンク](https://x.com)**です` | literal `**` around the link | bold |
| `a**太字` | bold | literal, as CommonMark already requires |

The `_` intraword rule runs **before** the relaxation and on the unrelaxed
levels, because the amendment leaves that rule alone. That is what keeps
`__テスト__です` literal: its closer is flanked by two "other" characters, the
intraword rule removes it, and nothing puts it back.

`~~` gets the same treatment in the strikethrough branch, which is how
`remark-cjk-friendly-gfm-strikethrough` applies it: a CJK neighbour stops the
run being refused for being flanked by two words. `~~del~~ok` stays literal,
which is GFM's own answer — the run is followed by a Latin letter, and neither
CommonMark nor the amendment lets it close.

**The permissive autolink** (`md_analyze_permissive_autolink`). A CJK character
immediately before the start or after the end ends the autolink, because neither
is a character a URL may contain. Without this a bare URL written as prose is
not a link at all: `請訪問 https://example.com。謝謝` has no link anywhere in it,
which is a worse failure than Streamdown's own bug of running the link past the
fullwidth stop.

**The CJK range table.** 61 ranges, the ones the amendment's `ranges.md`
publishes and the same table comrak uses, Unicode 18. It is a sorted array
searched by bisection after a `codepoint < 0x1100` early exit, so ASCII and
Latin prose — the overwhelming majority of a chat — never touch it. The neighbour
tests are reached only when neither side is already whitespace, so the table is
not consulted for `** ` or ` **` at all.

Downstream enables this by default and lets an app opt out with `cjk: false`.
The default is safe because a document with no CJK in it parses identically
either way: every change above is gated on a neighbour being a CJK character,
and `tests/cjk.test.ts` runs every corpus the repository holds — the remend
inputs, the intraword emphasis families, the unterminated spans — both ways and
asserts the render models are identical. `src/cpp/tests/engine.test.mjs` asserts
the same over the parse oracle's own cases.

This is upstream issue #650, so the patch is offered upstream and the goal is to
drop it once the amendment merges.

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

# remend

Provisional styling for the unstable tail of a stream.

A model that stops after `**bold wor` left a span open. The reader has two
options: show the markers literally, or show the content as bold and wait for
the closer to arrive. This module takes the second, but only when it can prove
the choice was safe.

It is a port of [`remend`](https://github.com/vercel/streamdown/tree/main/packages/remend)
(Apache-2.0), the streaming markdown repair pass Vercel ships as part of
Streamdown. It is not a port of remend's approach.

## What it does

`Preview::apply` parses the unstable tail, finds the marker runs md4c left as
literal text in its last block, and tries them innermost first: append a
matching closer, reparse, and keep the result only if the reparse proves the
closer changed nothing but that span. Each kept closer becomes the baseline for
the next, so `**bold *ital` closes both spans. A candidate that fails its proof
is skipped, and a tail costs at most four probes.

The proof is the design. A closer is accepted when:

- the block count is unchanged;
- every block before the last is identical, down to run ranges, cell alignment,
  and container identity;
- the last block's text is exactly the previous text with the claimed marker
  run removed and nothing else moved;
- the last block's inline style coverage, counted once per style a unit
  carries, strictly increased.

A closer that fails any of those is discarded and the literal text stands. A
closer that changes nothing visible is also discarded, because there was
nothing to style.

Where the closer goes matters as much as whether it is proven:

- **Before trailing whitespace.** A closer after a space is not
  right-flanking, and a chunk boundary often lands on one. Without this the
  span would flicker literal between two styled frames. remend gets the same
  effect by stripping one trailing space.
- **Never into a closed block.** A blank line ends a paragraph, and a heading
  or table row ends with its line. No later chunk can close a span there, so a
  preview would show something the settled document never will and vanish when
  the next block starts. The pass leaves those literal. remend closes them.

Because the check is a parse rather than a model of the delimiter rules, there
is no second implementation of CommonMark flanking rules to keep in step with
md4c's.

## Where it runs

Only on the tail, and only while the stream is unfinished.

- **Incremental mode.** After a commit boundary exists, the pass sees
  `source[committedBytes_ .. end]` and nothing else. Committed blocks live in
  shared immutable storage that the pass cannot reach, so no repair can restyle
  a block a reader has already seen.
- **Before any commit.** A stream shorter than the commit threshold has no
  boundary, so the whole document is the tail. The same argument holds.
- **Full mode.** Every update is a full parse, so the whole document is the
  tail on every update. This is the mode `createMarkdownStream` uses.
- **After a reset, or past the tail limit.** Both fall back to a full parse of
  a stream that is still open, so both still preview. Blocks already committed
  lead that parse and are passed as frozen, so a repair cannot land on one.
- **`finished`.** Never, in either mode. `docs/streaming-semantics.md` requires
  the settled document to be a parse of the bytes the writer sent, and
  `src/cpp/tests/test_preview.cpp` asserts that a previewed session and a plain
  one produce the same document once the stream finishes. A policy set after
  finish waits for the next reset rather than repainting a settled stream.

The pass does the parse it replaces, so an inert policy costs one parse, and
each probe is one more parse of the tail. `IncrementalCounters::bytesParsed`
counts both.

The canonical source, `getSource()`, and source ranges are never repaired. A
closer appended for a preview has no source offset and cannot reach a copy or
error event.

## It is off by default

```ts
// At construction.
const stream = createMarkdownStream({ preview: true });
// Or one feature at a time.
const emphasisOnly = createMarkdownStream({ preview: { emphasis: true } });
// Or mid-stream, which restyles the current tail without new input.
stream.setPreview({ inlineCode: true });
```

`PreviewOptions{}` has no features, which makes the pass inert: every open span
renders literally, exactly as the engine did before this existed. That default
is what `__tests__/streaming-preview.test.ts` pins, so this module could be
merged without changing a single currently-passing test.

`preview` accepts `true` for every feature, `false` for none, or an object
naming the ones to enable. Anything else throws rather than falling back to a
default, because a typo would otherwise silently change what a reader sees
mid-stream. The object mirrors remend's option-per-feature shape, so a caller
moving from Streamdown finds the same granularity; only the features listed
above exist.

Crossing the bridge is a bitmask, so the feature list has to be kept in step
with `PreviewFeature` in `Preview.h`. Adding a bit is a change on both sides,
and the native side masks off anything it does not define, so a stale caller
cannot ask for a feature that no longer exists and appear to succeed.

## What was not ported

remend has fourteen handlers. The emphasis, inline-code, and strikethrough ones
are ported in spirit through the one pass above. Links and images are not yet.
Most of the rest are workarounds for a parser that is not CommonMark-complete.
md4c is, so the same inputs reach the same answers without rewriting the source.

| remend handler | Why it is not ported |
| --- | --- |
| `links`, `images` | Not yet, and this is the most visible gap. The engine already refuses to make an unfinished target tappable: `isUnfinishedDestination` suppresses it, so `[docs](https://exa` can never open a half-typed URL. But the reader sees that raw syntax, where remend rewrites the target to `](streamdown:incomplete-link)` so Streamdown shows `docs` as link text, and drops a half-typed image. Doing the same here needs a proof of a different shape, since the visible text changes by more than a marker run, and a link the renderer knows not to open. It is left for its own change. |
| `singleTilde` | md4c's GitHub dialect reads `~x~` as strikethrough, so remend escapes the tilde to stop it. The pass refuses to treat a single tilde as an opener at all, which is the same answer without touching the source. `__tests__/streaming-preview.test.ts` pins `~hello` as literal. |
| `comparisonOperators` | remend rewrites `- > 25` to `- \> 25`. CommonMark says the `>` is a quote, so the item really is a list item containing a blockquote, and the test at `streaming-preview.test.ts:351` pins that. Escaping it would break the document. |
| `setextHeadings` | remend injects U+200B after a partial `---` underline because it cannot tell a heading from a rule until both arrive. md4c waits for the line, which is the same answer without an invisible character. |
| `htmlTags` | remend strips a partial tag off the end of the text. This engine already renders raw HTML as visible text, never executed, so a partial tag is text and stays text. Stripping it would delete characters the writer sent. |
| `katex`, `inlineKatex` | Math is literal by policy, and a closer appended to `$$x` would invent a delimiter. |
| `emphasis-handlers` | 813 lines implementing five marker-counting functions and their flanking rules. md4c already implements CommonMark delimiters, reached through the parser's span callbacks. The pass proposes a candidate run and lets a reparse arbitrate. |
| `strikethrough` | Ported through the same pass as emphasis. The half-complete case differs: remend turns `~~xxx~` into `~~xxx~~`, which deletes the tilde the writer already sent. The pass leaves it literal and lets the next chunk settle it. |
| `code-block-utils` | `isInsideCodeBlock` reimplements fence tracking that `Scan.h` already does, and does it worse: it knows only ` ``` `, where the scanner knows `~~~`, indentation, and info strings. The pass does not need it at all, because a completed code span is already identified by the parser's own `InlineStyle::Code` runs, which is an exact answer rather than a re-scan. |

One ordering difference also worth naming. remend runs its handlers in a fixed
priority order, so it closes `**` before a code span and writes
`**bold with \`code**\``, which puts the bold closer inside the code. This pass
closes open runs innermost first, each proven before the next is tried, so
`**bold with \`code` reads as bold text ending in a code span, the way the
writer's own closers will nest. A candidate that fails its proof, such as a
star the writer escaped, is skipped rather than ending the pass, and one tail
costs at most four probes.

## What lives alongside the policy

`src/cpp/downstream/Scan.h` and `src/cpp/downstream/Compare.h` are the shared
primitives the policy depends on. They are not remend's; they are answers the engine already gave
three times in three languages, consolidated into one place.

- `isEscaped`, `findUnescaped`, `skipInlineDestination`, and
  `isUnfinishedDestination` moved out of `DocumentBuilder` in
  `src/cpp/downstream/MarkdownParser.cpp`, which had the only test suite
  behind them, into `Scan.h`.
- `isBlankLine`, `afterIndent`, `startsWith`, `fenceOpener`, `closesFence`, and
  `isFenceLine` moved out of the anonymous namespace in
  `src/cpp/session/IncrementalParser.cpp`.
- `sameBlock` moved out of that same anonymous namespace into `Compare.h`,
  joined by `sameDocument`, because the preview pass needs the same structural
  comparison the commit path already relied on.
- `parseAt` became `parseMarkdownAt` in `src/cpp/downstream/MarkdownParser.h`,
  since the preview pass parses at a source offset too.

They live in `src/cpp/downstream` because the parser and the incremental parser
use them whether or not a session previews, so the engine core never depends on
the preview pass. The namespace is `downstream::scan`: these are the engine's
scanners, and `downstream::remend` is the part that is remend's.

## Tests

`src/cpp/tests/test_preview.cpp` and `__tests__/preview.test.ts`, built and run
by `pnpm test:native` and `pnpm test`.

The inputs are ported from remend's own suite — several hundred shapes of
markdown a model can stop emitting mid-token. The expectations are not, for the
reasons in the table above. For every input exactly one of two things is true:
the open span is styled, or nothing happened. Losing a block, inventing one, or
rewriting a block the reader has already seen is never an outcome, and
`corpusInvariants` asserts that over the whole ported corpus.

The C++ file also carries a suite for the shared scanner, since those primitives
replaced code that had tests and the replacement has to earn them back, and a
`sessionIntegration` case that drives a real `IncrementalParser`.

The TypeScript file covers the other half: the option a caller passes, the
bitmask that reaches the session, the mid-stream change, and the same stream in
both parse modes. `__tests__/real-engine-module.ts` records what the native
worker would apply to its parser and replays it through a real
`IncrementalParser` with `dump_session`, so the finish rule and the preview
policy under test are the engine's, not a restatement of them in JavaScript.

`src/cpp/tests/dump_model.h` is the shared JSON emitter, so `dump_document` and
`dump_session` describe a document identically and a test can compare them
directly.

## Provenance

github.com/vercel/streamdown, `packages/remend` and `packages/remend/__tests__`.
remend is Apache-2.0. The ported inputs are markdown syntax, which is not
copyrightable, and are reproduced under Apache-2.0 with that notice. No remend
source was copied; the behaviour was reimplemented against the engine's own
parser.

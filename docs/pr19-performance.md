# PR #19 performance analysis

The changes target the native code-card work added by [PR #19](https://github.com/krishkalaria12/react-native-downstream/pull/19).
The baseline is its final commit, `e680e62a1b601a361339085cbcc5902c6104d558`.
GitHub's merged main is `c353de85e4bccb0bfcd16b2aad13cf865d35963e`.
Their source trees are identical, verified after fetching main.

## What was expensive

| Path                    | Previous work                                                                                                                                                        | Change                                                                                                                                                                        |
| ----------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Android gutter lookup   | Each draw walked every logical line preceding the viewport, calling Android `Layout` methods along the way. Deep scroll cost grew with scroll depth.                 | Ask `Layout` for the top visual line, then binary search its UTF-16 offset in the logical line index. Lookup is logarithmic; drawing still visits only visible logical lines. |
| iOS blank-line gutter   | Each visible blank line could search the entire remaining blank suffix for a text baseline. An all-blank block could do quadratic work when drawing its full height. | Prepare a sorted nonblank-line index once. A draw searches it once for the initial baseline and preserves each visible nonblank glyph's actual baseline.                      |
| iOS measurement         | One cached width meant alternating wrapped/unwrapped measurements discarded each other. Every miss allocated a TextKit stack and laid out the complete segment.      | Retain three recently used height/width results, under the existing lock. No layout manager is shared between threads.                                                        |
| iOS mounting            | Mounting each streaming snapshot copied the complete line-start vector into `NSData`.                                                                                | The gutter retains the immutable segment owning the vector and reads it directly.                                                                                             |
| Android line indexing   | `ArrayList<Int>` allocated boxed offsets, then copied them into an `IntArray`.                                                                                       | Build a growable primitive `IntArray` in one text scan.                                                                                                                       |
| Android repeated layout | Each gutter measurement shaped a digit and fetched font metrics again; identical gutter updates requested another redraw.                                            | Cache immutable metrics when the segment is prepared and skip unchanged updates, including checking the body's actual `Layout` identity.                                      |

The capped viewport still requires native text layout for the complete code block.
These changes remove repeated work around that layout. They do not virtualize the
code text or replace platform glyph measurement with guessed line heights.

## Measured Kotlin results

Run `pnpm bench:code-layout`. This invokes the production Kotlin helpers and
compares them with the previous linear search and boxed line-index builder.
Both implementations must return identical results. There are 50 warm-up rounds
and seven samples, with sample order alternating.

Apple M5, arm64 macOS, Kotlin 2.4.20, Temurin Java 17.0.20.1. Medians in milliseconds:

| Workload                                  | Before |  After |  Ratio |
| ----------------------------------------- | -----: | -----: | -----: |
| 5,000 lookups across 2,000 lines          |  0.801 |  0.087 |   9.3x |
| 5,000 lookups across 20,000 lines         |  7.534 |  0.280 |  26.9x |
| 5,000 lookups across 100,000 lines        | 38.890 |  0.386 | 100.8x |
| Index 20,000-line code, 20 repetitions    | 12.897 | 12.383 |  1.04x |
| Index 100,000 blank lines, 20 repetitions |  7.648 |  2.536 |  3.02x |

These are host microbenchmarks, excluding Android `Layout`, text shaping, painting,
and frame scheduling. The lookup comparison gives both methods the first visible
visual line's offset; it also excludes the old implementation's repeated Android
layout-method calls. The line-index improvement for ordinary code is small. Its
clear benefit is removing boxed offsets and improving dense newline input.

[Raw Kotlin samples](../benchmarks/results/2026-10-02/pr19-code-layout-kotlin.jsonl)
are retained. [Native engine benchmark output](../benchmarks/results/2026-10-02/pr19-native-highlight.txt)
is a current-build measurement, not a before/after comparison. `bench:native` also
needed a repair: PR #19 added `parseFenceMeta`, but its compile list omitted
`FenceMeta.cpp`, causing a linker failure.

## Measured iOS work

`pnpm test:ios:layout` links the production rendering files to UIKit in an iOS
27 simulator using Xcode 27.0 and an arm64 target. The same harness also compiles
against the original PR source. It counts calls to real layout and string APIs.

| Workload                                                                         |                 Original PR |               Optimized |
| -------------------------------------------------------------------------------- | --------------------------: | ----------------------: |
| 12 warmed measurements alternating wrapped/unbounded widths, 4,100 logical lines |     12 TextKit layout calls |  0 TextKit layout calls |
| Draw a 200-point visible gutter over 4,096 blank lines                           | 53,170 text character reads | 14 text character reads |

Both builds produced the same measured text widths: 519.3 points unbounded and
233.7 points at a 240-point container. The original alternating-width sequence
took 444.631 ms in this run. The optimized sequence reused its measurements;
operation counts are the useful comparison here, rather than a speed ratio for
sub-microsecond cache hits.

Additional checks verify concurrent reads of the three warmed widths, bounded
LRU eviction, gutter ownership after the card/document are released, blank-line
baseline lookup, emoji line numbering, and expand-only layout changes.
[Original output](../benchmarks/results/2026-10-02/pr19-ios-baseline.txt) and
[optimized output](../benchmarks/results/2026-10-02/pr19-ios-optimized.txt) are saved.

The standalone harness substitutes system UIKit fonts for React Native's font
resolver. It constructs native documents directly and does not measure parsing,
React Native commits, live streaming, touch interaction, or physical-device frame
time. [Harness instructions](../benchmarks/code-layout-ios.md) explain how to
reproduce both builds.

## Reference-library findings

[Streamdown's code highlighter](https://github.com/vercel/streamdown/blob/08da224d303e1ef2516e2be4ce04d0c55fbd68ea/packages/streamdown-code/index.ts#L111)
retains completed-line tokens and grammar state, and bounds completed-result
caching by source/token/row cost. Its
[pin-scroll hook](https://github.com/vercel/streamdown/blob/08da224d303e1ef2516e2be4ce04d0c55fbd68ea/packages/streamdown/lib/use-pinned-scroll.ts#L75)
coalesces scroll requests. Its
[code body](https://github.com/vercel/streamdown/blob/08da224d303e1ef2516e2be4ce04d0c55fbd68ea/packages/streamdown/lib/code-block/body.tsx#L107)
still renders every highlighted row; capping height does not make initial rendering
independent of code length.

[Enriched Markdown's iOS code view](https://github.com/software-mansion/react-native-enriched-markdown/blob/1a4d2f857b0240a589c7c409c50ed8f214699d6c/packages/react-native-enriched-markdown/ios/segments/ENRMCodeBlockContainerView.m#L79)
retains TextKit layout and paints dirty glyphs. It analytically measures unwrapped
height. Downstream supports wrapping, custom line heights, and fallback glyphs,
so this patch retains real TextKit measurement rather than copying that formula.
Enriched Markdown also
[defers Android highlighting while a fence is pending](https://github.com/software-mansion/react-native-enriched-markdown/blob/1a4d2f857b0240a589c7c409c50ed8f214699d6c/packages/react-native-enriched-markdown/android/src/main/java/com/swmansion/enriched/markdown/segments/CodeBlockContainerView.kt#L194).
Changing that behavior here would remove an existing streaming feature, so the
optimizations retain incremental syntax highlighting.

## Validation and remaining costs

- Existing JS tests: 372 passed.
- Native engine tests: 257 passed.
- Kotlin checks cover UTF-16 surrogate pairs, CRLF, exact line boundaries, wrapped
  continuations, trailing empty lines, and 100,000 consecutive newlines.
- Android `:react-native-downstream:compileDebugKotlin` succeeds.
- The production-linked iOS layout checks pass against both source trees.
- Typecheck, lint, and library build pass. Lint retains five existing warnings.
- The Android compiled-document regression fixture used the old 13-field block
  stride. PR #19 expanded it to 16 fields; the fixture now exercises that format.
- iOS wrap/expand state comparison now includes `expanded`, so changing from an
  uncapped card to an expanded card refreshes the collapse control even when
  the two states have equal body height.

The largest remaining streaming cost is rebuilding the growing active code
segment's attributed text or spans and native layout. Completed segments already
reuse identities and caches. Improving the active segment further requires
append-aware native layout and invalidation that preserves selection, fallback
fonts, wraps, and lexer changes. Large-table layout and reference-heavy parse
fallbacks are separate costs. Physical-device release traces are still needed to
quantify scrolling frame time, memory, and end-to-end streaming latency; the host
lookup ratios do not establish those results.

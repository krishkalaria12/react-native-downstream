# PR #22 performance review

This pass optimizes PR #22 at `a813d28326447b34cb1c7f3b66d3940dea0683dc`.
The largest measured gains are in long paragraphs with styled line breaks and
preview over completed inline-code spans. It also reduces native script styling
work and fixes a streaming-preview crash. Physical-device frame latency has not
been measured.

## Findings and implementation

`MarkdownParser.cpp::trimTrailingSpaces` visited every earlier styled run for
every `<br>` with preceding spaces. A paragraph containing repeated scripts,
emphasis, and breaks therefore accumulated quadratic work. Runs are sorted and
disjoint, so trimming now removes only wholly trimmed trailing runs and clips
the last surviving run. Earlier runs remain unchanged.

`Preview.cpp::findOpenSpans` searched every styled run for each literal emphasis
marker, including markers inside completed inline-code spans. Preview now walks
the run list in source order alongside the text. A marker's code membership costs
constant work after advancing the cursor, making the complete walk linear in
text length plus run count. The existing parser-backed repair proofs remain.

The parser previously allocated a temporary UTF-16 string for every ordinary
text or literal-HTML callback, then copied it into the block. It now decodes
directly into the block with the existing Unicode decoder and records the same
merged runs. Whitespace following `<br>` is skipped in the UTF-8 view before
decoding. Math, entities, NUL replacement, and their special semantics remain
covered by the engine suites.

The allowlisted tag matcher compares ASCII names directly and rejects a fourth
name letter immediately. A long unsupported tag name no longer requires a full
name scan and lowercase string construction. Partial-tag matching also compares
views without a temporary lowercase string.

Preview uses its required output string as the repair buffer, avoiding a second
copy of the complete eligible region. After a successful partial-tag removal it
recomputes trailing whitespace and insertion offsets. Previously `**bold<su`
removed `<su` and then inserted the bold closer at the original, longer string's
offset, terminating with an uncaught `std::out_of_range`. A regression test
reproduced that failure before the fix and now checks emphasis and link repair
after partial-tag removal.

iOS `applyRuns` retains up to 16 immutable attribute dictionaries within one
call, keyed by style bits and link index. Repeated script/style combinations
reuse font, color, baseline, and link resolution. Block font, theme, font scale,
and clamp are fixed for the call, so the cache cannot carry attributes between
different blocks or themes. Calls with no overlapping styled runs return before
allocating the cache or resolving colors.

Android resolves block script metrics once per block. It uses the same integer
pixel size written into the block's `AbsoluteSizeSpan`, preserving themed
headings, density, and font scale. Table rows retain the original base paint.
This removes per-run span-tree searches, lazy wrappers, paint copies, and font
metric queries. Android span instances remain distinct for each range. Inline
code with script styling also avoids a second identical held-line span.

## Comparison with the reference libraries

The current Streamdown source was inspected at
[`1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc`](https://github.com/vercel/streamdown/tree/1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc).
Its [block parser](https://github.com/vercel/streamdown/blob/1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc/packages/streamdown/lib/parse-blocks.tsx#L255)
checks append-only input and lexes the changing tail. Its
[memoized blocks](https://github.com/vercel/streamdown/blob/1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc/packages/streamdown/index.tsx#L481)
skip unchanged rendering. Downstream already shares committed C++ blocks and
reuses native segments. Those caches do not remove work inside one growing
paragraph, which is the case targeted here.

Streamdown invokes remend before block splitting. Its
[incomplete-tag handler](https://github.com/vercel/streamdown/blob/1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc/packages/remend/src/html-tag-handler.ts#L19)
uses scans to propose trailing-tag removal. Downstream's reparse proves that
removal preserves block structure, prior content, and code/math text. The changes
reduce allocation and scanning costs while retaining that proof.

Enriched Markdown was inspected at
[`181bfcc345116b3dfc26fea6fd7973b2de355902`](https://github.com/software-mansion/react-native-enriched-markdown/tree/181bfcc345116b3dfc26fea6fd7973b2de355902).
The installed comparison package is `0.6.0`; the refreshed default-branch source
is research evidence, not a measurement of that installed release. Its
[Android measurement store](https://github.com/software-mansion/react-native-enriched-markdown/blob/181bfcc345116b3dfc26fea6fd7973b2de355902/packages/react-native-enriched-markdown/android/src/main/java/com/swmansion/enriched/markdown/MeasurementStore.kt#L289)
reuses spannables when only width changes. Its
[iOS strong renderer](https://github.com/software-mansion/react-native-enriched-markdown/blob/181bfcc345116b3dfc26fea6fd7973b2de355902/packages/react-native-enriched-markdown/ios/renderer/StrongRenderer.m#L32)
avoids attribute writes when the effective font or color already matches.
Downstream already caches measurements and segments; this pass reuses repeated
attribute resolution within a new segment.

## Measurements

[Raw C++ samples](../benchmarks/results/2026-10-05/pr22/inline-html-paired.json)
use Apple M5 arm64, Apple clang 21, C++20, `-O3 -DNDEBUG`. Both source roots compile
with the same harness before timing. Execution alternates baseline, optimized,
optimized, baseline. Each workload has a warm-up and nine samples. The harness
checks that text, run, and cell fingerprints match between all candidate runs.
Timing includes fingerprinting. It excludes native layout, paint, highlighting,
and typesetting.

The growing-paragraph workloads preview each 64-byte prefix of a 10 KiB source.
They exercise the real preview engine repeatedly but do not include session
scheduling, React, JNI, native rendering, or display latency.

| Work | Baseline median ms | Optimized median ms |
| --- | ---: | ---: |
| Parse 100 KiB scripts with styled breaks | 4.371–4.414 | 0.631–0.638 |
| Preview 100 KiB completed code spans containing markers | 10.876–10.906 | 0.680–0.682 |
| Preview growing 10 KiB code-marker paragraph, 64-byte chunks | 17.799–17.935 | 7.747–7.762 |
| Parse 100 KiB scripts | 0.657–0.680 | 0.478–0.488 |
| Parse 100 KiB plain prose | 0.353–0.496 | 0.347–0.401 |
| Preview 50 KiB plain prose | 0.250–0.472 | 0.243–0.322 |

Plain-prose results vary between runs and do not establish a reliable general
speedup. All samples remain in the JSON. An earlier pair that overlapped native
test execution is retained separately as
[`inline-html-validation-overlap.json`](../benchmarks/results/2026-10-05/pr22/inline-html-validation-overlap.json);
the table uses the final run without intentionally concurrent validation.
Rejecting a 64 KiB
unsupported tag name also improves in the isolated scanner workload, but that
microbenchmark is not an overall parser speed ratio.

iOS simulator attributed-string construction uses the real renderer with the
standalone harness's font shim, `-O2 -DNDEBUG`, 11 cold document builds per fixture,
and discards the first sample. Two pairs use opposite candidate order. Medians
were 0.039–0.041 to 0.029 ms for 32 runs, 0.450–0.465 to 0.265–0.278 ms for 512
runs, and 3.501–3.589 to 1.971–2.003 ms for 4,096 runs, about 44% less construction
time. The [baseline](../benchmarks/results/2026-10-05/pr22/ios-inline-baseline.txt) and
[optimized](../benchmarks/results/2026-10-05/pr22/ios-inline-optimized.txt) logs
retain the first pair's min/max and command. The reverse pair is retained in
[baseline-2](../benchmarks/results/2026-10-05/pr22/ios-inline-baseline-2.txt) and
[optimized-2](../benchmarks/results/2026-10-05/pr22/ios-inline-optimized-2.txt).
These timings exclude TextKit layout
and drawing. Android has source/JNI validation but no runtime timing.

## Reproduction and validation

```sh
git worktree add --detach /tmp/downstream-pr22-baseline a813d283
DOWNSTREAM_BENCH_COMPARE_ROOT=/tmp/downstream-pr22-baseline pnpm bench:inline-html

DOWNSTREAM_IOS_SOURCE_ROOT=/tmp/downstream-pr22-baseline bash benchmarks/native-inline-runs-ios.sh
bash benchmarks/native-inline-runs-ios.sh
pnpm test:ios:layout
```

Passed `pnpm test` with 562 tests, `pnpm test:native` with 317 tests,
`pnpm test:kotlin`, `pnpm typecheck`, `pnpm typecheck:android`, and the real iOS
TextKit and inline-run harnesses. Inline checks cover 24 distinct links beyond
the cache capacity, custom script themes, font scale, code scripts, footnotes,
line heights, and unchanged text. A Unicode break-trimming test checks surrogate
pairs, combining characters, 257 styled runs, and whitespace-only trailing runs.
Lint has zero errors and five existing bitwise warnings in test helpers.
Independent review found no actionable regression.
[Validation summaries](../benchmarks/results/2026-10-05/pr22/validation.txt)
retain the test and source-check output.

The remaining end-to-end measurements are physical-device Release first-glyph
latency, scrolling, UI frame times, CPU, memory, and energy. A long single
paragraph still needs parsing on each delivered update, and a proven preview
repair can still reparse it. The full-mode identity scan and controlled-document
cache source comparisons are candidates for later profiling. Session callbacks
currently run under the lock to guarantee no event follows `stop()`; changing
that publication contract requires ordering tests. This pass makes no claim that
every path is now faster or that the device budgets in
[the performance methodology](performance-methodology.md) have been met.

# PR #21 performance review

The changes target repeated work introduced by native math in PR #21, against
`abac4ef8ccd2aee740e3a5b678f5b110fedb562f`. Complete parsing of the 100 KiB
mixed-code-and-bracket-math fixture is about 1.7 times faster on this host.
Android redraws reuse prepared geometry and text, iOS inline attachments reuse
their last raster, and repeated invalid formulas reuse their error. These results
do not establish physical-device frame latency or an overall renderer speedup.

## Findings and changes

`MathScan.cpp` previously built code-region lists even for sources without bracket
math. It also accumulated inline-code regions and searched the entire list for
each later formula. It now checks for a possible bracket opener first, skips
repeated fence checks on the same line, and walks fenced regions in source order.
Completed inline spans advance directly to their end. The source remains the
same length and every delimiter offset remains unchanged. Both implementation
and independent review matched the original scanner over 100,000 deterministic
generated inputs containing prose, code, escapes, and bracket delimiters.

`MathEngine.cpp` cached successful layouts but reran RaTeX for identical failures.
The same 2,000-entry LRU now holds successful layouts and failure messages. TeX and
display mode remain the key, so a growing formula gets a new attempt. Installation
and clearing invalidate the cache; a generation check prevents an old in-flight
call from repopulating it. Errors are copied before the engine frees its output.
Engine calls still execute outside the mutex. Concurrent initial misses can still
execute independently.

Android's `MathRenderer` rebuilt every path and encoded every glyph during every
draw. `MathDisplayList` now prepares each path and glyph string once and drops the
raw path-command buffer after conversion. Size and color still apply during
drawing. `MathSegmentView` retains its background drawable, skips equal styles,
and reuses its child array during measurement and layout. Actual text-size changes
use `TextView.setTextSize` so fallback text updates Android's cached layout.
Speech-only changes update accessibility without forcing geometry changes.

iOS's `DSMathAttachment.imageForBounds` previously rasterized an unchanged formula
on every request. Each attachment now retains only its latest image. Bounds,
derived size, resolved color, display scale, appearance, gamut, and font-generation
changes invalidate it. A per-attachment mutex protects concurrent requests. This
saves warm redraw work; it does not reduce the first raster's cost. One image per
attachment is a retention bound, not a global memory budget.

The React math-error callback depended on an inline generation getter, creating a
new native callback on every render and stream snapshot. Math and image callbacks
now retain their identity and read current handlers and generation through a ref.
Tests verify current handlers receive events and errors from before a stream reset
are dropped.

## Comparison with the reference libraries

Streamdown was inspected at `1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc`.
Its [block parser](https://github.com/vercel/streamdown/blob/1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc/packages/streamdown/lib/parse-blocks.tsx#L112)
reuses a conservatively proven append-only prefix and preserves exact-input array
identity. Its [memoized block](https://github.com/vercel/streamdown/blob/1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc/packages/streamdown/index.tsx#L481)
avoids repeating transformation for unchanged content. Downstream already shares
committed blocks and caches prepared native segments. Preserving those identities
and eliminating work around them is the relevant transfer. Streamdown's browser
KaTeX and image rendering do not measure native TextKit, Android, or RaTeX costs.

Enriched Markdown was inspected at
`c10ab56837c6ca3fca2f143aacc85ca7f5020dcb`. Its
[measurement cache](https://github.com/software-mansion/enriched-markdown/blob/c10ab56837c6ca3fca2f143aacc85ca7f5020dcb/packages/react-native-enriched-markdown/ios/internals/MeasurementCache.h#L189)
keys bounded reuse on content, width, style, and font-related inputs. Its
[segment reconciler](https://github.com/software-mansion/enriched-markdown/blob/c10ab56837c6ca3fca2f143aacc85ca7f5020dcb/packages/react-native-enriched-markdown/ios/segments/SegmentReconciler.m#L28)
skips updates for identical signatures. Those approaches support retaining math
artifacts and skipping unchanged setters, with complete invalidation keys.

Enriched Markdown's
[async coordinator](https://github.com/software-mansion/enriched-markdown/blob/c10ab56837c6ca3fca2f143aacc85ca7f5020dcb/packages/react-native-enriched-markdown/ios/utils/ENRMAsyncRenderCoordinator.m#L16)
executes queued rendering jobs and suppresses obsolete results during application.
Downstream already coalesces queued session appends before parsing. Copying that
coordinator would not improve its queue behavior.

Both Downstream image loaders already deduplicate requests and limit concurrency
to four. The image-state, download, and committed-image retention paths remain
covered by the existing suites. Image loading was not identified as a new measured
bottleneck in this pass.

## Measurements

[Paired raw samples](../benchmarks/results/2026-10-02/pr21/math-paired.json)
use Apple M5 arm64, Apple clang 21, C++20, `-O3 -DNDEBUG`, and vendored RaTeX
0.1.14. Both candidates compile before measurement. Execution order is baseline,
optimized, optimized, baseline. Each row below gives the range of the two run
medians; each run has nine timed samples and a warm-up. Scan and parse times are
per complete 100 KiB input. The repeated-engine cases measure 5,000 calls.

| Work | Baseline ms | Optimized ms | Interpretation |
| --- | ---: | ---: | --- |
| Bracket scan, mixed code and math | 2.035–2.045 | 0.105–0.108 | About 19 times faster |
| Complete parse, mixed code and math | 4.791–4.806 | 2.816–2.825 | About 1.7 times faster |
| Complete parse, inline code | 1.064–1.064 | 0.760–0.804 | Lower parsing cost |
| Complete parse, bracket math only | 4.732–4.746 | 4.791–4.815 | No gain; slightly slower samples |
| Repeated invalid formula | 101.836–103.248 | 0.169–0.170 | 5,000 engine calls become one |
| Repeated valid formula | 0.102–0.103 | 0.100–0.106 | One engine call in both; similar timing |

The large parser fixtures intentionally stress parsing and include more formulas
than the document typesetting cap. They do not run document layout or native
painting. Host load and all samples are retained in the JSON. Earlier independently
compiled runs are also retained as `math-baseline-{1,2}.json` and
`math-optimized-{1,2}.json`; they overlap test/build activity and are noisier.

The existing native highlight/stream benchmark now includes the math sources
needed by PR #21 and accepts the same source-root override. Two sample pairs
are retained as `stream-*.jsonl`. Sample order reverses in the second pair.
Variation between pairs is larger than most candidate differences. This pass
does not claim a general syntax-highlighting or streaming speedup from those runs.

On the iOS 27 iPhone 18 Pro simulator, seven samples of 200 warmed inline-attachment
requests with 120 system glyphs had a median of
[150.719 ms before](../benchmarks/results/2026-10-02/pr21/ios-math-baseline.txt)
and [0.141 ms after](../benchmarks/results/2026-10-02/pr21/ios-math-optimized.txt).
The original performed 1,400 rasterizations; the optimized version performed zero
after warm-up. This artificial repeated-request workload demonstrates reuse,
not an app-wide speed ratio.

Android host operation tests show zero additional paths across 100 redraws,
stable glyph-string identities, and no additional layout, redraw, or background
allocation across 100 identical style/snapshot updates. They also check changed
color, size, padding, alignment, metrics, speech, Unicode, error fallback, and
detach/reattach. These are operation assertions rather than Android frame timings.

## Reproduction and validation

```sh
pnpm bench:math

baseline_dir=$(mktemp -d)
git archive abac4ef src | tar -x -C "$baseline_dir"
DOWNSTREAM_BENCH_COMPARE_ROOT="$baseline_dir" pnpm bench:math

DOWNSTREAM_IOS_SOURCE_ROOT="$baseline_dir" pnpm test:ios:math
pnpm test:ios:math
```

Passed `pnpm test` with 381 tests, `pnpm test:native` with 263 tests including
the real RaTeX tests, `pnpm test:kotlin`, `pnpm typecheck`, and
`pnpm typecheck:android`. The portable cache suite also passed AddressSanitizer
and UndefinedBehaviorSanitizer. The iOS math harness checks actual raster pixels
for transparency, inherited/explicit colors, dark mode and increased contrast,
plus bounds, display scale, gamut, font invalidation, and concurrent reuse.
Full `pnpm lint`, focused native-benchmark `.mjs` lint, and Node script syntax
checks passed. Independent review found no actionable regression.

Both example app builds passed:

```sh
cd example/android
./gradlew :app:assembleDebug -PreactNativeArchitectures=arm64-v8a
```

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcodebuild \
  -workspace example/ios/DownstreamExample.xcworkspace \
  -scheme DownstreamExample -configuration Debug -sdk iphonesimulator \
  -destination 'generic/platform=iOS Simulator' \
  -derivedDataPath /tmp/downstream-pr21-ios-build CODE_SIGNING_ALLOWED=NO build
```

These builds check compilation and linking, not Release performance.

The older `test:ios:layout` harness originally failed to include
`React/RCTImageLoaderProtocol.h`. React-specific imports now stay in the image
loader's implementation, and the harness links the real math renderer, box views,
exporter, and their C++ dependencies. Its standalone image transport and React
presentation entry points fail immediately if a fixture invokes them. The real
TextKit checks now pass, including emoji gutter coverage, blank-suffix scanning,
width-cache reuse, concurrent measurement, and eviction.
[The repaired harness output](../benchmarks/results/2026-10-02/pr21/ios-layout-optimized.txt)
is retained.

The ESLint configuration inherited an ES2015 parser setting for `.mjs` scripts,
which rejected `import.meta` and top-level await. A module-specific override now
allows current ECMAScript syntax. Both native benchmark runners pass ESLint.

Physical-device Release traces are still needed for first visible glyph, p95 UI
work, dropped frames, scrolling, CPU, memory, and energy. The remaining profiling
candidates are cold math font/glyph rendering, JNI display-list serialization for
new segments, and repeated accessibility speech generation. No claim is made
that those costs have been eliminated or that the device budgets in
[the performance methodology](performance-methodology.md) are met.

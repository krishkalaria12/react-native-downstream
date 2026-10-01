# PR #18 performance review

Baseline: `f38f7c3909a2cfb240530849a51be6b8e7701540`, branch
`feat/theme-and-style`.

## Verified problems and fixes

| Path | Problem | Change |
| --- | --- | --- |
| JavaScript palette resolution | `foreground` converts the same input color once for each of ten destination slots. | Convert each input token once, then write its resolved appearance halves to every destination. |
| Stable native theme props | Comparing values without slot IDs loses an update when the same number moves between elements. Font table lengths were also unchecked. | Compare lengths and contents of slots, values and fonts. Equal inline themes retain their native array identities. |
| Native font identity | Font slots hash their numeric array indices, so `Inter` and `Georgia` at index zero collide and reuse the previous font and size. | Hash ordered font family names with their boundaries into drawing and measurement identities. |
| Palette measurement reuse | A theme with only colors has a nonzero layout key, while the default theme has zero. Switching from defaults to a palette measures again. | Use the default layout key when no metric slot is overridden. Drawing IDs still distinguish colors and appearances. |
| Controlled source preparation | A cloned shadow node requests source preparation before resolving its theme. Unknown-width hints also inherit unrelated views' controls and add their themes to the work. | Resolve the current theme first. Borrow only recent widths and scales; retain requested theme, chrome and wrap choices, and deduplicate layouts. |
| Android segment boundaries | Each new noninitial segment flattens the entire preceding block over JNI just to read its bottom margin. A large code block copies text and tokens again. | Append the preceding margin to the existing layout array. Each new segment needs one document read. Margin slots come from shared C++ rules. |
| Android run ownership | `blockOf` returns the first block for every valid offset, so later runs inherit the wrong font. | Find the owning block with allocation-free binary search. |
| Inline font work | Color-only runs resolve and apply a font identical to the enclosing block, allocating attributes or metric spans. | Apply a face override only when effective font metrics differ. Android table runs retain their face spans because table cell paint is a separate inheritance path. |
| iOS styled tables | Every cell scans every styled run in its row. | Binary-search the overlapping run range. The shared selector also serves table exports and is checked against a full scan across every boundary. |

Preparation is best effort. A theme change arriving after the worker has
collected its layout hints uses normal on-demand rendering. The optimization
does not restart parsing or highlighting. A cold first view with no known width
still builds its native layout when measured.

## Streamdown and Enriched Markdown comparison

Reviewed Vercel Streamdown at
[`08da224d303e1ef2516e2be4ce04d0c55fbd68ea`](https://github.com/vercel/streamdown/tree/08da224d303e1ef2516e2be4ce04d0c55fbd68ea)
and Enriched Markdown at
[`609971c336754eca276ca122ba03755b63651856`](https://github.com/software-mansion/enriched-markdown/tree/609971c336754eca276ca122ba03755b63651856).

Streamdown [reuses a proven block prefix and reparses the active tail](https://github.com/vercel/streamdown/blob/08da224d303e1ef2516e2be4ce04d0c55fbd68ea/packages/streamdown/lib/parse-blocks.tsx#L240-L346).
Its [memoized block comparator](https://github.com/vercel/streamdown/blob/08da224d303e1ef2516e2be4ce04d0c55fbd68ea/packages/streamdown/index.tsx#L481-L585)
keeps unchanged blocks out of React rendering. Downstream already carries native
committed block identities, shared prefix storage, incremental tail parsing,
resumable code highlighting, and bounded caches. Those mechanisms are the right
foundation for streaming performance.

Enriched Markdown [builds immutable render results asynchronously and rejects superseded results](https://github.com/software-mansion/enriched-markdown/blob/609971c336754eca276ca122ba03755b63651856/packages/react-native-enriched-markdown/ios/EnrichedMarkdown.mm#L698-L756).
Its [segment reconciler reuses existing native views by signature](https://github.com/software-mansion/enriched-markdown/blob/609971c336754eca276ca122ba03755b63651856/packages/react-native-enriched-markdown/ios/segments/SegmentReconciler.m#L28-L116).
Downstream already prepares native renderings on its worker pool and reuses
settled segments. This review fixes theme inputs and extra work at those
boundaries so the native presenters can use the existing caches correctly.

## Measurements

These host measurements cover individual operations. They exclude UIKit and
Android layout, view mounting, visible paint latency, and device frame rates.

The inline traversal benchmark runs the previous full scan and current range
selection in the same C++20 release binary with Apple clang 21, `-O3`, on an
Apple M5. Seven samples alternate execution order after warmup. Each cell's
selected runs are checked for equal results before timing.

| Workload | Median full scan | Median range selection | Reduction |
| --- | ---: | ---: | ---: |
| 128 columns, 256 runs, 100 passes | 0.815 ms | 0.217 ms | 73% |
| 2,048 columns, 4,096 runs, 20 passes | 42.014 ms | 1.214 ms | 97%, 34.6x |

The wide-table number measures run selection only. It is not a 34.6x renderer
speedup. Complexity falls from `cells * runs` to `cells * log(runs) + overlaps`.
The following reductions are deterministic operation counts from regression
tests, independent of host timing noise.

| Operation | Before | After |
| --- | ---: | ---: |
| Foreground token color conversions | 10 | 1 |
| Light/dark token plus later dark override conversions | 30 | 3 |
| Requested unknown-width theme with two unrelated recent layouts | 4 preparations | 2 preparations |
| Duplicate recent geometry with different themes/controls | 4 preparations | 1 preparation |
| Requested known width with two unrelated recent layouts | 3 preparations | 1 preparation |
| Document JNI reads for a new noninitial segment | 2 | 1 |

Theme timing with the React Native color test stub varied while native builds
were running. Those samples are retained for inspection, and no theme timing
percentage is claimed. `benchmarks/theme-resolve.bench.ts` now awaits all runs,
prints mean/p99/relative margin, excludes module export getters from callbacks,
and measures the actual foreground conversion path.

Raw samples and metadata are in
[`benchmarks/results/2026-10-01/host-pr18`](../benchmarks/results/2026-10-01/host-pr18/).
Reproduce the traversal benchmark with
`node benchmarks/native-inline-runs.mjs`, and theme measurements with
`pnpm exec vitest bench benchmarks/theme-resolve.bench.ts --run --silent=false`.

## Validation

- JavaScript suite: 359 tests passed.
- Shared C++ suite: 256 tests passed. Font identity, palette measurement keys,
  requested preparation hints, and inline range boundaries have regressions.
- `pnpm typecheck` passed. `pnpm lint` passed with five existing bitwise warnings.
- Android Kotlin compilation and native builds passed for arm64-v8a,
  armeabi-v7a, x86 and x86_64. `python3 src/android/tests/check_native_document.py`
  checks the actual compiled `NativeDocument` bytecode against offset and
  font inheritance cases, a 4,096-block oracle, and the margin array contract.
- iOS simulator Debug build passed for arm64 and x86_64 using Xcode 27.
  The command selects Xcode with `DEVELOPER_DIR` and overrides
  `IPHONEOS_DEPLOYMENT_TARGET=15.1`, because an installed dependency's font
  resource target still names iOS 6, which this SDK refuses. These are build
  arguments; no native project or dependency configuration was edited.

Platform validation commands:

```sh
cd example/android
./gradlew :react-native-downstream:compileDebugKotlin :app:buildCMakeDebug --console=plain
```

From the repository root:

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcodebuild \
  -workspace example/ios/DownstreamExample.xcworkspace \
  -scheme DownstreamExample -configuration Debug -sdk iphonesimulator \
  -destination 'generic/platform=iOS Simulator' \
  -derivedDataPath /tmp/downstream-pr18-ios -jobs 4 \
  CODE_SIGNING_ALLOWED=NO COMPILER_INDEX_STORE_ENABLE=NO \
  IPHONEOS_DEPLOYMENT_TARGET=15.1 build
```

Native visual rendering and physical-device frame times have not been measured
for this patch. Follow [the device measurement procedure](performance-methodology.md)
to assess scrolling, concurrent streams, long open fences and tail updates at
60/120 Hz. Subprocess replay benchmarks include historical parsing and process
startup, so their durations do not establish production streaming latency.

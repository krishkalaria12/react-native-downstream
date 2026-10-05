# PR #23 performance review

This pass optimizes PR #23 at `0dab488b96b7a9dbb24cac23999593d5e6afd10d`.
The largest repeatable gains are in React reconciliation around settled fences,
cached iOS measurement, and repeated controlled-source slot queries. Bare parsing
is mostly unchanged. Physical-device frame latency has not been measured.

## Findings and changes

`src/fenceRenderers.ts` previously memoized only the app component. Every later
snapshot still reconstructed its error boundary, Suspense boundary, and native
slot. `MountedFence` now memoizes that complete subtree against ordinal, renderer
component, code, language, metadata, completeness, document scope, and the error
callback. The inner renderer memo remains, so changing an error callback updates
the boundary without rerendering an unchanged chart. React still processes local
component state and lazy resolution. A regression test proves that 24 settled
fences receive no additional native-wrapper renders across 20 later snapshots,
removing 480 wrapper renders. Generation callbacks in `DownstreamMarkdown.tsx`
are stable; their previous inline identity defeated the outer memo on every token.

Renderer identity was missing from the old comparator. Replacing a registered
component with the same fence input could keep the previous component on screen.
The comparator now includes that identity, and a failed boundary also retries
when its renderer changes.

Async results now carry a small query token and are accepted only by that query.
Previously a source replacement could commit the previous source's chart before
the effect cleared its slots. The token excludes stale results during render.
It also removes the source-containing serialized key, clearing effect, and
code-containing boundary reset string. Query dependencies still include the
publication revision because finishing a stream can settle fences without
changing the last append's sequence.

`ControlledSlots.cpp` cached unresolved large-source misses and derived pending
status from an empty answer. That hid later completed preparations and treated a
valid zero-match result as unresolved. The cache now contains resolved answers
only, including empty answers. Worker queries check the cache and completed
preparation before parsing. The LRU retains at most 16 answers and 4 MiB of
accounted source, language, and slot storage; oversized answers are not retained.
Bare extraction passes its settled-prefix count into `fenceSlots`, avoiding a
copy of the whole parsed document merely to change that count. The 16 KiB limit
on fresh synchronous parsing remains. Repeated-query gains primarily benefit
remounted messages and multiple controlled views of one source.

Fabric previously searched document blocks separately for every hosted child,
rebuilt theme inputs for every child, searched the children again when mounting,
and linearly searched hosted rectangles. `measureSlots` now indexes code blocks
in one traversal, stopping at the largest requested ordinal, and uses the node's
already resolved theme. Indented code blocks still consume ordinals. Children
carry their original index, and rectangle lookup uses binary search. Column
discovery changes from O(blocks × slots) to O(blocks + slots); rectangle lookup
changes from O(slots²) to O(slots log slots). With no children, the indexing pass
does not run. Child measurement remains necessary because a chart can change
height independently of markdown publication.

iOS stored frame rectangles in boxed Objective-C objects and converted every
frame back into C++ even for a cached size-only query. It now caches native
geometry and copies only the result a caller needs. Measurement returns size and
hosted rectangles from one stack result. Previously it obtained size and then
read shared cached rectangles under another lock, allowing concurrent widths or
host heights to mix in one result. The harness now checks 1,024 concurrent
measurements with different inputs, plus zero-height hosts, missing ordinals,
changed widths, and changed list indentation.

Android image lookup was linear despite its sorted ordinal table. It now uses
binary search, removing quadratic lookup growth in image-heavy stacking. Hosted
height resolution uses one search rather than separate presence and height
searches. Host rectangles use a primitive `IntArray` instead of boxed integers,
and JNI omits the host field for an empty layout. Sparse 8,192-entry lookup tests
verify results and a logarithmic bound on table reads. Android runtime timing
was not collected.

## Reference libraries

The refreshed Streamdown source was inspected at
[`1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc`](https://github.com/vercel/streamdown/tree/1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc).
Its [block parser](https://github.com/vercel/streamdown/blob/1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc/packages/streamdown/lib/parse-blocks.tsx#L240)
preserves stable strings and parses a conservative changing tail. Its
[component comparison](https://github.com/vercel/streamdown/blob/1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc/packages/streamdown/index.tsx#L550)
includes component functions. These principles apply to settled fence wrappers
and component replacement here. Downstream already has native incremental
parsing, and its normalized renderer map already avoids Streamdown's
[linear renderer search](https://github.com/vercel/streamdown/blob/1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc/packages/streamdown/lib/plugin-context.tsx#L51).

Enriched Markdown was inspected at
[`181bfcc345116b3dfc26fea6fd7973b2de355902`](https://github.com/software-mansion/react-native-enriched-markdown/tree/181bfcc345116b3dfc26fea6fd7973b2de355902).
Its [bounded iOS measurement cache](https://github.com/software-mansion/react-native-enriched-markdown/blob/181bfcc345116b3dfc26fea6fd7973b2de355902/packages/react-native-enriched-markdown/ios/internals/MeasurementCache.h#L188)
and [Android measurement reuse](https://github.com/software-mansion/react-native-enriched-markdown/blob/181bfcc345116b3dfc26fea6fd7973b2de355902/packages/react-native-enriched-markdown/android/src/main/java/com/swmansion/enriched/markdown/MeasurementStore.kt#L462)
support retaining results with complete validity inputs. Downstream continues
to include host heights, width, font scale, images, code layout, caret, and theme
layout values in measurement validity. A settled ordinal alone cannot justify
skipping a child's layout. The installed comparison packages remain enriched
markdown `0.6.0` and React Native Streamdown `0.2.0`; inspecting refreshed source
is research, not a speed comparison against those installed packages.

## Measurements

All candidates use the exact original PR head as baseline and the same current
harness. Host runs use Apple M5 arm64. Native and React runs alternate baseline,
optimized, optimized, baseline, with warm-up, nine samples per workload, and
matching output fingerprints. Timings run separately from validation suites.

The [React samples](../benchmarks/results/2026-10-05/pr23/fence-react-paired.json)
use production React DOM with fresh synthetic native slot objects and an
unchanged renderer configuration. Each sample contains 128 commits in batches
of 16. Task yields between batches prevent an artificial synchronous-update
depth limit in the original implementation; timings sum commit-batch durations
and exclude the waits. The fixture allocates its slot objects inside the timing.
Parsing, JSI serialization, Hermes, Fabric layout, paint, and app chart computation
are excluded. These numbers cannot rank native renderers.

| React work, 128 commits | Baseline median ms | Optimized median ms |
| --- | ---: | ---: |
| Changing tail after 64 settled fences | 14.407 to 14.657 | 4.128 to 4.241 |
| Changing tail after 256 settled fences | 36.636 to 36.737 | 13.721 to 14.205 |
| Unchanged rerenders, 64 settled fences | 12.693 to 13.784 | 2.782 to 3.496 |
| Unchanged rerenders, 256 settled fences | 34.140 to 34.790 | 9.748 to 12.108 |

Both candidates execute the changing renderer exactly 1,152 times across their
nine samples. At 256 settled fences, native-wrapper calls fall from 296,064 to
1,152. The one-settled-fence changing-tail workload varies around parity; there
is no demonstrated general gain for tiny messages.

The [C++ samples](../benchmarks/results/2026-10-05/pr23/fence-slots-paired.json)
use Apple clang 21, C++20, `-O3 -DNDEBUG`. They execute real controlled-slot,
extraction, parsing, and answer-cache code. The prepared-document lookup is
stubbed identically to miss in both candidates, isolating the fallback paths.
The baseline worker shim calls `parsedFenceSlots`, exactly as its TurboModule
worker did. Fingerprinting is outside timing; a result sink remains inside.
The table divides sample totals by their recorded iteration count.

| Native query | Baseline median ms/query | Optimized median ms/query |
| --- | ---: | ---: |
| Repeated worker query, 100 KiB prose with one fence | 0.978 to 0.993 | 0.001858 to 0.001862 |
| Repeated worker query, 100 KiB open code fence | 0.331 to 0.334 | 0.004367 to 0.004575 |
| Repeated worker query, 256 fences | 0.175 to 0.176 | 0.000904 to 0.000909 |
| Cold controlled query, 10 KiB prose | 0.104 to 0.110 | 0.098 to 0.099 |

Bare 100 KiB prose parsing is 0.977 to 0.990 ms versus 0.935 to 1.002 ms and does
not establish a general parser speedup. Repeated worker results are cache hits,
not cold parse ratios. Source comparisons and answer copies still cost work.
The full JSON retains all 40 workloads, including pending synchronous answers,
cold worker queries, neutral results, and variation.

iOS uses an iPhone 18 Pro simulator on iOS 27 with the standalone real native
renderer, `-O2 -DNDEBUG`, and the harness font shim. Two pairs use opposite
candidate order. Each reported mean contains 5,000 calls over 512 hosted fences
after initial preparation. Geometry checksums match across all four runs.

| iOS document measurement | Baseline mean µs/call | Optimized mean µs/call |
| --- | ---: | ---: |
| Cached size only | 34.010 to 35.558 | 0.180 to 0.185 |
| Cached size and host rectangles | 34.835 to 35.406 | 1.484 to 1.513 |
| Alternating-width restack and rectangles | 92.155 to 95.480 | 26.588 to 27.069 |

Raw logs retain the first [baseline](../benchmarks/results/2026-10-05/pr23/ios-host-baseline.txt)
and [optimized](../benchmarks/results/2026-10-05/pr23/ios-host-optimized.txt)
runs and the reversed [optimized](../benchmarks/results/2026-10-05/pr23/ios-host-optimized-2.txt)
and [baseline](../benchmarks/results/2026-10-05/pr23/ios-host-baseline-2.txt)
runs. This is document geometry cost, excluding Fabric child measurement, React,
drawing, and first-glyph latency. Four aggregate observations do not establish
device p95 or p99 latency.

## Reproduction and validation

```sh
git worktree add --detach /tmp/downstream-pr23-baseline 0dab488
DOWNSTREAM_BENCH_COMPARE_ROOT=/tmp/downstream-pr23-baseline pnpm bench:fence-slots
DOWNSTREAM_BENCH_COMPARE_ROOT=/tmp/downstream-pr23-baseline pnpm bench:fences

SIMCTL_CHILD_DOWNSTREAM_IOS_HOST_BENCH_ONLY=1 \
  DOWNSTREAM_IOS_SOURCE_ROOT=/tmp/downstream-pr23-baseline bash tests/ios/run-native-harness.sh
SIMCTL_CHILD_DOWNSTREAM_IOS_HOST_BENCH_ONLY=1 bash tests/ios/run-native-harness.sh
```

Run timed commands sequentially without concurrent build or test work. Native
and React runners also accept `--check` for output validation without samples.

Passed `pnpm test` with 595 tests, `pnpm test:native` with 329 tests, library and
example typechecks, `pnpm test:kotlin`, `pnpm typecheck:android`,
`pnpm test:fabric`, and `pnpm test:ios:layout`. Lint has zero errors and five
existing bitwise warnings. Fabric checks compile against real RN codegen and
both platform header sets and run hosted rectangle lookup tests with both
`Float` types. Independent review found no actionable regression.
[Validation records](../benchmarks/results/2026-10-05/pr23/validation.md)
describe coverage and its limits.

## Remaining costs

Every streaming slot query still walks document blocks, exports matching fence
text, converts it to UTF-8, constructs JSI values, and joins fresh slot objects
in JavaScript. Its work therefore scales with document blocks plus matching
fence text per publication even when settled React trees skip rendering. That
is the next profiling target for messages with many large completed charts.
The current benchmarks do not measure that complete bridge path or prove it
dominates frame time. A future cache must include snapshot identity and
completeness, because sequence alone misses settlement; wire block identities
also pass through JavaScript doubles and must not be trusted as exact 64-bit keys.

Release builds on physical iOS and Android devices still need first-glyph,
scrolling, frame-time, CPU, memory, and energy traces. Actual Yoga relayout after
an app renderer's own state changes its height is outside these runtime
harnesses. This pass does not claim the physical-device budgets in
[the performance methodology](performance-methodology.md) have been met.

# PR #20 performance review

This pass targets the animation and caret added by PR #20, against
`6d2aa70fbe162557a4839e4a1cad9cfdcb0dd33e`. The workspace changes reduce
repeated reveal scheduling, drawing, and invalidation work. Measurements below
cover specific host and simulator workloads; they do not establish an overall
renderer speed ratio or physical-device frame budgets.

## Findings

The original C++ reveal pass allocated alignment arrays for every document block
and revisited settled ranges across the entire document. Skipping text alignment
inside those loops did not make the pass proportional to the changing tail.
Fresh units also searched every carried unit before a full sort. Unchanged blocks
had no alignment runs, which caused their pending fades to disappear on a later
snapshot. The optimized pass preserves unchanged units and limits alignment work
to the tail, using actual shared-storage provenance to establish the prefix.

iOS repeatedly searched all segments and reveal units, sorted already ordered
runs, and converted fully hidden character ranges into glyph ranges before
discarding their drawing. The changes use ordered block lookup, combine
invalidations within a block, and skip hidden drawing work. Table mapping includes
all rows. Settled text between and after active fades must remain visible, and
finishing a fade must invalidate the affected text children.

Android rebuilt clipping paths during segment drawing, searched all reveal units
for each segment and gutter query, and invalidated the entire child tree each
frame. The changes retain drawing scratch storage, reuse segment bands within a
frame, and target the affected segment roots. Alpha and rise share one easing
evaluation. Completing units remain part of the invalidation set. A reusable
frame callback can be canceled on detach and disposal, avoiding duplicate loops.

The React animation effect depended on inline callback identity. Twenty renders
with an unchanged true flag produced twenty start notifications. The new
regression tests cover controlled views and stream views with an explicit flag.
They require one notification per transition and the current handler on the next
transition. Callback identity no longer reruns the effect.

## Reference-library comparison

Streamdown was inspected at `1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc`.
Its [block parser](https://github.com/vercel/streamdown/blob/1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc/packages/streamdown/lib/parse-blocks.tsx#L240)
retains stable blocks and lexes a conservative changing tail. The relevant
transfer is that consumers of Downstream's shared committed blocks should also
avoid allocating and traversing document-wide state on each append.
Its [animation timeline](https://github.com/vercel/streamdown/blob/1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc/packages/streamdown/lib/animate.ts#L62)
compresses stagger under load and bounds scheduling debt. Downstream already
implements those rules. Its
[stable per-block animation configuration](https://github.com/vercel/streamdown/blob/1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc/packages/streamdown/index.tsx#L748)
has the same purpose as Downstream's existing stable native animation props.
DOM spans and browser CSS animation do not provide native TextKit or Android
drawing measurements.

Enriched Markdown was inspected at
`c10ab56837c6ca3fca2f143aacc85ca7f5020dcb`. Its
[segment reconciler](https://github.com/software-mansion/enriched-markdown/blob/c10ab56837c6ca3fca2f143aacc85ca7f5020dcb/packages/react-native-enriched-markdown/ios/segments/SegmentReconciler.m#L28)
keeps matching segment views and avoids redundant content updates. Its
[measurement cache](https://github.com/software-mansion/enriched-markdown/blob/c10ab56837c6ca3fca2f143aacc85ca7f5020dcb/packages/react-native-enriched-markdown/ios/internals/MeasurementCache.h#L205)
includes content, width, style, font scale, and rendering options in a bounded
cache. These support retaining unchanged drawing artifacts with complete
invalidation inputs. Caching every complete growing document would mostly miss
and retain redundant prefixes.

Enriched Markdown's
[iOS fade animator](https://github.com/software-mansion/enriched-markdown/blob/c10ab56837c6ca3fca2f143aacc85ca7f5020dcb/packages/react-native-enriched-markdown/ios/utils/ENRMTailFadeInAnimator.m#L97)
edits text-storage colors every tick. Downstream's draw-time fade preserves cached
text and layout, so this pass retains that architecture. Its
[async coordinator](https://github.com/software-mansion/enriched-markdown/blob/c10ab56837c6ca3fca2f143aacc85ca7f5020dcb/packages/react-native-enriched-markdown/ios/utils/ENRMAsyncRenderCoordinator.m#L16)
suppresses stale results when applying them, while Downstream already coalesces
queued session appends before parsing.

## Measurements

[Paired raw C++ samples](../benchmarks/results/2026-10-02/pr20/reveal-paired.json)
use Apple M5 arm64, Apple clang 21, C++20, and `-O3 -DNDEBUG`. Each row is the
median of seven alternating runs. Times are totals for the stated repeated
snapshot applications, not individual frames or app latency.

| Reveal workload | Applications per run | Before ms | After ms | Speedup |
| --- | ---: | ---: | ---: | ---: |
| Short tail after 2,048 committed blocks | 2,580 | 14.256 | 0.497 | 28.7x |
| Appends to a 48,000-unit paragraph | 520 | 18.876 | 6.711 | 2.81x |
| 3,000-word burst followed by appends | 260 | 11.493 | 4.774 | 2.41x |
| 8,000-character burst followed by appends | 99 | 9.930 | 4.159 | 2.39x |

All units, ranges, and start times produce identical semantic checksums in these
paired fixtures. They avoid the unchanged-active-block bug so that correcting
that behavior does not artificially change the measured amount of work.
Separate regressions exercise that correction.

The iOS simulator harness uses real UIKit/TextKit drawing at `-O2`. One paired
sample of five draws with 8,192 hidden units took
[81.590 ms before](../benchmarks/results/2026-10-02/pr20/ios-reveal-baseline.txt)
and [0.237 ms after](../benchmarks/results/2026-10-02/pr20/ios-reveal-optimized.txt).
Glyph conversions fell from 81,920 to zero. Twenty thousand last-block lookups in
128 code segments took 286.974 ms and 5.999 ms, with protocol checks falling from
2,560,000 to 20,000. These single pairs demonstrate eliminated work, with less
timing evidence than the repeated C++ samples. The harness also checks foreground
and background range coverage for settled gaps, caret lookup, table row ownership,
and one invalidation for 4,096 units in a block. These checks assert drawing
operations and invalidation, rather than final screen pixels. Its provider is
mocked; the React Native
view's provider implementation is checked by source review and app compilation.

[Android host samples](../benchmarks/results/2026-10-02/pr20/android-reveal.json)
measure production `RevealFrame` building on OpenJDK 17 with a stub interpolator
that calls the production curve solver. They exclude Android layout and painting.
Each workload has 10,000 warm-up frames and seven samples of 20,000 frames.

| 512-unit frame workload | Before ns/frame | After ns/frame |
| --- | ---: | ---: |
| Simultaneous rise | 10,870 | 871 |
| Staggered rise | 9,114 | 5,543 |
| Staggered fade | 5,583 | 5,419 |
| Mostly completed queue | 1,648 | 618 |

The simultaneous rise case reduces curve evaluations from 1,024 to one.
Staggered rise uses 512 rather than 1,024. Fade-only work is similar. Host
operation checks verify no new paths across 100 redraws, exact clips preserving
settled gaps and suffixes, RTL/LTR marker margins, and completion invalidation.
A tail fade among 10,000 segments targets one root. Actual callback lifecycle,
platform interpolator cost, clipped drawing, and GPU layers remain device checks.

## Reproduction

```sh
pnpm bench:reveal
pnpm test:ios:reveal
pnpm test:kotlin
pnpm bench:reveal-frame -- --baseline 6d2aa70
pnpm bench:reveal-frame
```

The reveal benchmark compiles the original and current reveal implementation
before timed samples, warms each executable, alternates execution order, and
retains every sample. Snapshot construction happens outside timing. Pruning and
per-pass tracker construction and cleanup are included. The benchmark
checks output checksums and reports its compiler, flags, source revision, host,
and harness digest. Correctness tests cover behavior independently of timing.

## Validation

The full JavaScript suite passes 501 tests. After restoring the pinned RaTeX
framework, the native suite passes all 264 tests, including the real math-engine
test, 2,000 deterministic alignment/scheduling comparisons, and
regressions for retained fades, independently constructed prefixes, markup
reconciliation, Unicode, placeholders, and zero-duration rise. The old replay
ordering assertion flattened successive snapshots. It now checks each snapshot
independently, because ongoing units correctly appear in more than one snapshot.

Kotlin operation and curve suites, Android source/JNI typechecking, TypeScript
typechecking, the package build, and lint pass. Lint reports five existing
bitwise-operation warnings. The iOS TextKit harness also passes its Unicode,
gutter, width-cache, and concurrent measurement checks. Independent diff review
found no remaining actionable regression.

The Android example Debug build passes with
`./gradlew :app:assembleDebug -PreactNativeArchitectures=arm64-v8a` from
`example/android`. The full iOS example Debug build also passes for both simulator
architectures, including the real view provider and caret code that the standalone
harness does not compile. This verifies compilation and linking, rather than
Release performance.

The local CocoaPods project initially lacked the math header directory already
listed in `Downstream.podspec`, and RaTeX was absent locally. Restoring the pinned
framework with `node math/scripts/vendor-ratex.mjs` and running `pod install` in
`example/ios` regenerated the correct project. Xcode 27 also rejects a dependency
resource target configured for iOS 6. The validation command supplies a deployment
target override; no dependency versions or generated projects are part of the
optimization diff.

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcodebuild \
  -workspace example/ios/DownstreamExample.xcworkspace \
  -scheme DownstreamExample -configuration Debug -sdk iphonesimulator \
  -destination 'generic/platform=iOS Simulator' \
  -derivedDataPath /tmp/downstream-pr20-ios-build \
  CODE_SIGNING_ALLOWED=NO IPHONEOS_DEPLOYMENT_TARGET=15.1 build
```

Physical-device Release traces remain necessary for arrival-to-visible-glyph
latency, UI frame time, scrolling, CPU, memory, and energy. The proposed budgets
in [the performance methodology](performance-methodology.md) remain unproven.

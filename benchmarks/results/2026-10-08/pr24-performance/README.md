# PR #24 performance analysis

Baseline: `373ead5f5405490a23f5269e3cfd5250b6cb0047`, the head of PR #24 when this audit started. The optimizations target `feat/streamdown-gap-plan`.

## Measured improvements

Host: Apple M5, macOS arm64, Apple clang 21, release builds with `-O3 -DNDEBUG`. These measure host parser CPU and deterministic layout/query work. They do not measure device frame rates or end-to-end streaming latency.

| Workload | Before | After | Result |
| --- | --- | --- | --- |
| 100 sibling one-block details elements with Unicode and links | 1.130 ms | 0.310 ms | 72.6% less parser CPU |
| 500 sibling details elements | 20.693 ms | 1.595 ms | 92.3% less parser CPU |
| 1,000 sibling details elements | 76.428 ms | 3.168 ms | 95.9% less parser CPU |
| Closed paragraph with 4,096 inline-code bracket runs | 10.032 ms | 0.558 ms | 94.4% less parser CPU |
| Same paragraph streamed in 256-byte chunks | 67.040 ms | 56.416 ms | 15.8% less cumulative parser CPU |
| Initial tag render plus two unrelated parent updates | 3 native queries | 1 native query | Equivalent inline plugins reuse the query |
| Fixed-width Android details, 1,000 repeats and 3,000 visibility toggles | Rebuilds layout on every closed measurement or visibility change | 1 layout build | Reuses shaping and scalar line geometry |

Details timings average the two per-execution medians in an ABBA comparison. Each execution warms up, then records nine samples, with three parses per sample. [Raw output](inline-html-paired.json) includes fingerprints and every sample. Baseline and optimized model fingerprints agree across all workloads, including text, inline runs, cells, container IDs/markers and link ranges. Unrelated inline-HTML workloads had no slowdown above 10% in this final run; small timing variation is not a throughput guarantee. An earlier run overlapped other compilation and was discarded.

Bracket timings take the median of three execution medians, each with nine measured samples following a warmup. The baseline replaces only `IncrementalParser.cpp`; all other engine sources are identical. [Raw output](incremental-brackets-paired.txt) records counters. Parsed bytes remain 57,346 for the closed paragraph and 6,508,546 for streaming, with one commit attempt each. The smaller streaming gain reflects the remaining reparsing of a growing unfinished paragraph.

JavaScript and Android counts come from the new regression tests. Tag APIs are mocked in the React test; it verifies query invalidation and component invocation rather than C++ parse duration. Android uses production `Segments.kt` and `DetailsLayout.kt` with host layout stubs; it verifies shaping-call counts and geometry arithmetic rather than Android text-shaping speed.

## Changes and causes

- `src/cpp/downstream/MarkdownParser.cpp`: each details body formerly converted the whole preceding source to UTF-16 just to get its length. Successive sibling elements repeatedly traversed growing prefixes. It now uses the existing source offset cursor. Container prefixes are inserted as one range instead of shifting the vector once per ancestor. Unicode link-range regression coverage checks 40 sibling bodies after earlier linked Unicode prose.
- `src/cpp/session/IncrementalParser.cpp`: the reference-dependency check searched every inline run for each bracket. Ordered, disjoint parser runs allow one forward cursor, reducing the check from brackets × runs to text + runs. Tests cover code combined with emphasis, unresolved brackets before/between/after code, byte-by-byte late definitions and retained snapshot immutability.
- `src/tagRenderers.ts` and `src/DownstreamMarkdown.tsx`: effective renderer names, component identities and fallback identity stabilize query dependencies. Unrelated parent updates no longer repeat synchronous tag discovery. Lowercasing the controlled source happens once per query. Fallback candidates decode tag names from the policy wire format rather than querying attribute names and sentinels. Source, policy, renderer, fallback, session, generation, sequence and revision changes still invalidate.
- `src/android/src/main/java/com/downstream/Segments.kt`: hidden ranges formerly compared by object identity, so an equal rebuilt list missed the height cache. Ranges now compare by value. Width caches the full height and line geometry; visibility changes recompute only the subtraction. Plain segments keep scalar height only. Details segments retain approximately eight bytes per line plus one top entry and array overhead, without retaining `StaticLayout`.
- `src/android/src/main/java/com/downstream/DetailsLayout.kt`: merged hidden intervals and binary searches replace checking every range against every line. The original per-line rule matches across 2,000 randomized tests with unordered, overlapping, reversed and empty intervals and unequal line heights. A 100,000-line/1,000-range fixture takes 33,377 line-start probes and 2,000 top probes instead of up to 100 million range checks.
- `src/cpp/fabric/DownstreamMarkdownShadowNode.cpp`: mounted tag measurement now indexes the first block of each requested custom container once, then uses direct lookups. The previous per-child search grew with tags × blocks × nesting. The new index also removes a premature stop based on block count, which could omit later containers when an earlier container held many blocks. Nested frames retain the same first matching block and its content indent. This path passed Fabric compilation and independent code review; direct runtime measurement coverage remains outstanding.

## Upstream comparison

Repositories were refreshed and inspected at these revisions:

- Streamdown: `8edc64857a175720ad549e4d8c3cb1b9b4fca1ca`.
- Enriched Markdown: `fd66e31f2e63b1c112ed85eb880dcc8643a8ff49`.

Streamdown's [incremental block parser](https://github.com/vercel/streamdown/blob/8edc64857a175720ad549e4d8c3cb1b9b4fca1ca/packages/streamdown/lib/parse-blocks.tsx#L108), [memoized block rendering](https://github.com/vercel/streamdown/blob/8edc64857a175720ad549e4d8c3cb1b9b4fca1ca/packages/streamdown/index.tsx#L481), and [incremental highlighting/cache](https://github.com/vercel/streamdown/blob/8edc64857a175720ad549e4d8c3cb1b9b4fca1ca/packages/streamdown-code/index.ts#L111) preserve settled blocks and grammar state. Downstream already has equivalent committed-prefix and highlighter reuse. The changes above address new work that bypassed those existing savings. Promise-based JavaScript highlighting is not evidence of native worker-thread performance.

Enriched Markdown [caches span styles](https://github.com/software-mansion-labs/react-native-enriched-markdown/blob/fd66e31f2e63b1c112ed85eb880dcc8643a8ff49/packages/react-native-enriched-markdown/android/src/main/java/com/swmansion/enriched/markdown/renderer/SpanStyleCache.kt#L30) and [avoids Android span copies](https://github.com/software-mansion-labs/react-native-enriched-markdown/blob/fd66e31f2e63b1c112ed85eb880dcc8643a8ff49/packages/react-native-enriched-markdown/android/src/main/java/com/swmansion/enriched/markdown/utils/text/view/TextViewSetup.kt#L14). Its owned mutable text buffers differ from Downstream's shared immutable renderings, so adopting its no-copy factory without changing ownership would be unsafe. Its [exact iOS measurement reuse](https://github.com/software-mansion-labs/react-native-enriched-markdown/blob/fd66e31f2e63b1c112ed85eb880dcc8643a8ff49/packages/react-native-enriched-markdown/ios/internals/ShadowMeasurementUtils.h#L102) supports separating geometry reuse from visibility changes, as the Android fix does here.

## Remaining bottlenecks

These are code-traced opportunities, not measured improvements from this patch:

1. `SessionRegistry.cpp` copies canonical source into each custom-tag snapshot, and `TagSlots.cpp` rebuilds structural ranges and materializes body strings per query. Append-shared immutable source storage and snapshot slot deltas would require careful ownership and bounded retention. This patch only eliminates redundant queries for unchanged inputs.
2. An unfinished paragraph still reparses its growing tail, as the bracket benchmark's unchanged parsed-byte counter demonstrates. Long open fences also retain text/token snapshot copies. Grammar-specific suffix parsing and immutable append storage need separate correctness work.
3. iOS tag-style matching and both platforms' link-variant matching can repeat name/prefix lookups per run. Resolve by tag/link index within immutable render state if profiling shows a material cost.
4. Details sub-parses still recurse through nested bodies. Reusing the offset cursor improves sibling scaling; it does not make arbitrary deep nesting or body copying free.
5. Physical-device frame percentiles, peak heap retention, selectable-text copy costs and real Android shaping timings remain unmeasured. Fabric tag index behavior has syntax checks and code review, but no new mounted runtime harness.

## Reproduction and validation

Export the original source into an isolated directory:

```sh
mkdir -p /tmp/downstream-pr24-baseline
git archive 373ead5f5405490a23f5269e3cfd5250b6cb0047 | tar -x -C /tmp/downstream-pr24-baseline
DOWNSTREAM_BENCH_COMPARE_ROOT=/tmp/downstream-pr24-baseline node benchmarks/native-inline-html.mjs
node benchmarks/native-incremental-brackets.mjs --baseline /tmp/downstream-pr24-baseline
```

The inline runner was updated for PR #24's current parser dependencies and tag scanner API. The incremental runner shares the engine's content-addressed build cache with tests, using an opt-in release flag; default test flags remain unchanged.

Passed:

- `pnpm test`: 744 tests across 29 files.
- `pnpm test:native`: 362 tests, no skips.
- `pnpm test:kotlin`, including the new production text-height cache and randomized interval checks.
- `pnpm typecheck` and `pnpm typecheck:android`, including production Kotlin and both JNI translation units.
- `pnpm test:fabric`: iOS/Android syntax checks and host rectangle lookup.
- `pnpm test:ios:layout`: native TextKit harness.
- `pnpm lint`, `pnpm format:check`, and `git diff --check`. Existing lint/native-harness warnings remain.

Independent review found no supported correctness regression and confirmed the paint/theme ownership, run ordering and snapshot invariants.

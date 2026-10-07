# Streaming performance audit

Audited October 7, 2026 at revision `144fd321f253dad53d0efe33eacbde6922c6dcfb`. The priority is streaming chat responses. This pass contains analysis and reproduction artifacts; it changes no library behavior.

The largest opportunities are reducing whole-response fallback parsing, reusing native layout for growing tails, and avoiding repeated copying of growing code fences. The existing architecture already does useful work: frame batching, worker scheduling, immutable committed blocks, and native segment caches. Preserve those mechanisms.

The [first implementation pass](../benchmarks/results/2026-10-07/performance-fixes/README.md) now fixes trace pairing, batches Android state application, adds native metadata/tail cache budgets, and repairs the iOS harness declarations. Android host checks show synchronizations reduced from four to one for the targeted state change. The original iOS fixture now retains 15 tail renderings instead of 128. The analysis below records the original baseline; larger parser and layout changes remain follow-up work.

The [parser and native layout follow-up](../benchmarks/results/2026-10-07/performance-followups/README.md) adds revocable reference-prefix reuse, bounded iOS measurement workspaces and mounted suffix edits, code-free preparation reuse, and Android plain-table-cell caches. The original 96 KiB reference fixture now parses 142,046 bytes instead of 38,273,853. The paired iOS fixture reduces 32 KiB paragraph measurement from 10.386 ms to 0.476 ms per update. The findings below remain the original audit baseline.

## Evidence and limits

C++ measurements use the real parser and highlighter, Apple M5 ARM64, Apple clang 21.0.0, `-O3 -DNDEBUG`, and Node 24.15.0. Each experiment feeds a whole response synchronously and excludes `finish()`, platform rendering, worker scheduling, and React Native delivery. It intentionally parses after every chunk. Production frame batching and worker coalescing reduce the number of passes, so these totals are workload scaling evidence, not response latency.

iOS measurements use the current `DSRenderedDocument` and real TextKit, with the existing system-font shim, `-O2 -DNDEBUG`, and an iPhone 18 Pro simulator running iOS 27.0. They isolate native rendering and measurement. They do not measure physical-device FPS or the complete React Native pipeline.

Android findings primarily establish repeated work through code. The reveal experiment measures target counts using production Kotlin and host stubs. Heap usage and milliseconds saved on Android remain unmeasured.

Sources, results, and commands are in [the reproduction directory](../benchmarks/results/2026-10-07/performance-audit/README.md). No physical-device release traces are available in the repository. JavaScript dependencies were absent, so the JS tests and React benchmark suite were not run.

## Findings ranked by streaming impact

| Priority    | Workload                                                        | Verified repeated work                                            | Proposed optimization                                      | Scope                                       |
| ----------- | --------------------------------------------------------------- | ----------------------------------------------------------------- | ---------------------------------------------------------- | ------------------------------------------- |
| High        | An unresolved reference early in a response                     | Every subsequent update reparses the entire response              | Track reference dependencies and retain unaffected blocks  | Parser design change                        |
| High        | Long growing paragraphs, fences, and containers                 | Native tail text and layout are rebuilt                           | Update changed suffixes and preserve valid layout work     | Native renderer change                      |
| High        | iOS prepared width differs from mounted width                   | A second full TextKit layout can run during mounting              | Prepare the actual mounted width or both required widths   | Investigate device frequency first          |
| High        | Long code fences arriving in small updates                      | Tail snapshot copy, token copy, and prefix comparison             | Avoid redundant copies; provide trusted append provenance  | Small first patch, deeper storage follow-up |
| Medium      | Many snapshots of a large open tail                             | Up to 128 document renderings retained without a byte-cost budget | Add approximate byte-cost budgets                          | Bounded cache change                        |
| Medium      | Android state changes with new caret, images, or hosted heights | Setters synchronize the old document before applying the new one  | Apply the complete incoming state once                     | Focused Android change                      |
| Medium      | Streaming tables                                                | Existing cells are rebuilt and measured again                     | Reuse cell content and layouts; invalidate changed columns | Native table change                         |
| Conditional | Registered custom fence renderers                               | Every revision queries and exports all matching code              | Cache settled slot results and deliver changes             | API and lifecycle change                    |
| Lower       | Thousands of individually committed blocks                      | Every prefix extension copies the accumulated pointer index       | Use structurally shared indexed pages                      | Storage design change                       |
| Lower       | Multi-block animated bursts                                     | Finished roots remain redraw targets                              | Retire targets after their final opaque redraw             | Focused animation change                    |

### 1. An early unresolved reference defeats incremental parsing

[`hasUnresolvedBracket`](../src/cpp/session/IncrementalParser.cpp#L66) conservatively rejects rendered `[` or `]` outside inline code, excluding code, HTML, and math blocks. [`provesClosed`](../src/cpp/session/IncrementalParser.cpp#L90) also rejects regions containing `]:`. Commit attempts start at the existing committed boundary. An unresolved reference at the start therefore prevents all subsequent regions from committing. With no committed prefix, [`parse`](../src/cpp/session/IncrementalParser.cpp#L856) selects whole-source parsing. Above 64 KiB, the tail limit explicitly selects that fallback.

The experiment changes the first paragraph from `plain` to `[ref]` and appends ordinary paragraphs in 128-byte chunks. Three timed samples follow one warmup; values are cumulative median CPU time.

| Final source size | Ordinary prose | Starts with `[ref]` | Bytes parsed with `[ref]` |
| ----------------: | -------------: | ------------------: | ------------------------: |
|            16 KiB |       1.327 ms |           17.658 ms |                 1,128,798 |
|            32 KiB |       3.767 ms |           91.322 ms |                 4,378,794 |
|            64 KiB |       8.201 ms |          326.810 ms |                17,125,231 |
|            96 KiB |      11.870 ms |          749.179 ms |                38,273,853 |

The reference workload committed zero bytes and performed one full parse per update. The ordinary workload performed one full parse, then tail parses. Commit-probe backoff helps failed probes but does not avoid the main parse.

The fix needs reference-definition context and dependencies, with invalidation of affected earlier blocks when a definition arrives. Removing the bracket guard would freeze incorrect output. Retaining later independent blocks requires changing the current contiguous-prefix model. Keep final full parsing and prove parity at every streamed prefix, including definitions before and after use, shortcut and collapsed links, duplicate definitions, images, escapes, footnotes, and containers.

### 2. Native renderers rebuild the growing tail

iOS [`DSRenderedDocument`](../src/ios/DSRenderedDocument.mm#L1635) rebuilds uncommitted segments. It copies complete text and attributes, creates a fresh measurement stack, and lays out the full segment. [`DSTextSegmentView`](../src/ios/DSTextSegmentView.mm#L48) replaces its mounted text storage with the complete attributed string.

Twenty snapshots at each initial size append six characters per update. These are arithmetic means from one simulator process, not medians.

| Initial paragraph characters | Build per update | Measure per update | Full measurement calls |
| ---------------------------: | ---------------: | -----------------: | ---------------------: |
|                        2,048 |         0.030 ms |           1.249 ms |                     20 |
|                        8,192 |         0.077 ms |           5.521 ms |                     20 |
|                       32,768 |         0.236 ms |          21.376 ms |                     20 |

Worker preparation can absorb measurement when the prepared variant and width match. Mounted text replacement still occurs synchronously. Investigate updating suffix text and attributes with conservative invalidation when closing markdown delimiters restyles earlier content. Keep old snapshots immutable and preserve selection and accessibility ranges.

Android has the same uncommitted-segment cache bypass at [`RenderedDocument.kt:386`](../src/android/src/main/java/com/downstream/RenderedDocument.kt#L386). It reconstructs text and spans, and creates `PrecomputedText` on supported Android versions. The Android device cost has not been measured.

### 3. iOS width and highlighting variants can miss prepared layout

Worker preparation measures a hinted width. [`DSDocumentView.layoutSubviews`](../src/ios/DSSegmentViews.mm#L694) asks for frames at the mounted bounds width. TextKit measurement caches require exact width equality, while [`layoutSlots`](../src/cpp/fabric/DownstreamMarkdownShadowNode.cpp#L549) explicitly accounts for Yoga rounding the measured constraint into a different frame width.

For a 32,768-character paragraph prepared at width 320.2, requesting mounted frames at width 320 caused one additional full TextKit layout taking 20.582 ms in the simulator. The mechanism is verified; its frequency in a real chat remains unknown. Prepare geometry consistent with the actual mounted width, or warm both required widths. Do not merge distinct widths with an epsilon: a small width change can alter line wrapping.

There is another preparation mismatch. Both iOS worker preparation and Fabric measurement request [`syntaxHighlighting:YES`](../src/ios/DownstreamTextLayoutManager.mm#L107), while mounting uses the actual prop. Cache keys include that flag. Requesting the same code-free 8,192-character paragraph first with YES and then NO produced a second rendering, second segment, and another full layout. Normalize irrelevant highlighting values for code-free content first; code needs shared metric preparation or preparation of the requested paint variant. Disabling this view prop currently does not disable engine tokenization.

### 4. Open-fence parsing still copies and compares the whole fence

[`appendOpenFence`](../src/cpp/session/IncrementalParser.cpp#L807) copies the document's owned tail, including the growing UTF-16 string and token vector. Committed payloads remain shared. [`HighlightState::applies`](../src/cpp/highlight/CodeHighlighter.cpp#L368) compares the entire old prefix, and the highlighter copies accumulated token runs into its output.

Seven timed samples follow one warmup. These medians feed one open fence in 16-byte chunks.

| Fence content size | Plain fence | TypeScript fence |
| -----------------: | ----------: | ---------------: |
|              8 KiB |    0.421 ms |         1.904 ms |
|             16 KiB |    1.523 ms |         6.502 ms |
|             32 KiB |    5.397 ms |        22.677 ms |
|             60 KiB |   20.231 ms |        66.114 ms |

The plain fence's parser counter stayed at nine parsed bytes, and TypeScript's at thirteen. Every append took the fast path. Parser-byte counters therefore miss the growing copy/comparison cost.

A narrow first patch can avoid copying old tokens immediately before replacing them and supply verified append provenance to the highlighter. Bind that trust to the exact block/state lineage and previous consumed length. Reset it on replacement, reset, fallback, and language changes; preserve the surrogate-boundary checks and checked path for arbitrary callers.

This alone will not make the complete path linear. Snapshot copies remain, and one unfinished long code line can require repeated scanning and lexing. Immutable text/token chunks are a separate storage change; consumers must not flatten them on every update or the cost merely moves into native rendering.

### 5. Native document caches retain historical open tails

Android's [`document cache`](../src/android/src/main/java/com/downstream/RenderedDocument.kt#L279) has 128 entries with no `sizeOf` override. iOS's [`document cache`](../src/ios/DSRenderedDocument.mm#L1415) has `countLimit=128` and no cost limit. Both keys include the snapshot document ID. Open tails get new native text rather than committed-segment reuse.

The iOS experiment created 128 successive roughly 32K-character paragraph snapshots, drained their autorelease pools, and checked weak references. All 128 renderings survived, holding 4,243,072 text characters. Multiplying by two gives logical UTF-16 content size, not measured resident memory. Attributes, layouts, and other allocations are additional; `NSCache` can evict under pressure.

Add approximate byte-cost budgets for document metadata and independently retained tail payloads. Avoid charging shared committed content repeatedly where practical. Mounted views can retain objects outside the cache, so this is a cache budget rather than a strict process-memory cap. Preferential eviction by stream revision needs separate lifecycle work to preserve worker-to-measure-to-mount reuse.

### 6. Android applies one state update through several synchronizations

[`updateState`](../src/android/src/main/java/com/downstream/DownstreamMarkdownViewManager.kt#L243) sets image layout, host layout, and caret before the incoming document. Changed image and host setters immediately synchronize views. A changed caret also obtains a rendering for the old document. [`setDocument`](../src/android/src/main/java/com/downstream/DownstreamMarkdownView.kt#L296) then obtains and synchronizes the incoming document.

Apply the full incoming state atomically, resolve the final rendering once, and synchronize once. Simply moving `setDocument` earlier still builds under old options. Preserve same-document updates, reveal tracking, tracing, recycling, and host layout requests. Equality guards already avoid this cost when options are unchanged, so it does not occur on every ordinary token.

### 7. Open tables and custom fences have reusable work

Android [`buildTable`](../src/android/src/main/java/com/downstream/RenderedDocument.kt#L1248) rebuilds cell text, desired widths, and `StaticLayout` for every cell in the open table. Cache styled cell content and layouts keyed by effective width, alignment, header status, theme, and font scale. A widened column invalidates its cell layouts and affected row heights; replacements that shrink a maximum-width cell need recomputation too.

With registered custom languages, [`useFenceSlots`](../src/fenceRenderers.ts#L399) synchronously queries every stream revision. [`fenceSlots`](../src/cpp/downstream/FenceSlots.cpp#L75) walks all blocks and exports complete code for every matching fence, including settled fences. React memoization skips settled renderer execution but does not avoid these exports and transfers. Cache settled results and return changes, with removals and finalization. Keys need generation, actual document revision, and renderer configuration. Sequence alone cannot distinguish finish or preview updates.

Android also prepares a complete native code body before [`setHosted`](../src/android/src/main/java/com/downstream/CodeSegmentView.kt#L527) hides it. A lazy native fallback could avoid shaping large hosted bodies until needed, while retaining the existing fallback for renderer errors and suspension.

### 8. Lower-priority cumulative work

[`SharedList::Prefix::extend`](../src/cpp/downstream/Document.h#L316) copies all previous chunk handles and item pointers. A nine-sample isolated microbenchmark with one item per extension measured 1.886/8.703/44.903/173.436 ms for 1,024/2,048/4,096/8,192 extensions. Real commits can batch multiple blocks. Structurally shared pages could reduce growth, but must preserve cheap indexing, stable references, iteration, and bounded destruction depth.

Android reveal targets also outlive individual completed fades. A synthetic 64-root staggered schedule retained 64 invalidation targets at 1,100 ms while only four roots remained in the frame. That frame includes future scheduled units at zero alpha. Invalidate active roots plus roots needing their final opaque redraw, then retire completed roots. This finding applies only with animation enabled and measures target counts rather than frame rate.

JavaScript `source += chunk` is a possible rope-flattening cost, not an established bottleneck. Measure it in Hermes before replacing source storage. Chunk validation already scans only the accepted chunk, and snapshot publication already coalesces to one render per frame. Generic memoization is not the first useful change here.

## Fix measurement validity before evaluating improvements

[`nativeEvents`](../benchmarks/summarize.mjs#L104) validates `sequence` but drops it from the returned record. [`nativeDurations`](../benchmarks/summarize.mjs#L130) groups on that missing field. Different snapshots in a session generation can therefore be paired.

Two reproductions confirmed the defect:

- Sequence 1 starts parsing at 1 ms; sequence 2 ends at 100 ms. Expected zero paired samples, actual one sample of 99 ms.
- Sequence 1 measures from 1 to 11 ms; sequence 2 from 2 to 3 ms. Expected durations 10 and 1 ms, actual 2 and 9 ms.

Retain the sequence and test mismatched and interleaved snapshots before relying on trace percentiles. Complete, strictly ordered pairs can still be correct; this does not invalidate every historic duration. The existing eight summary tests pass and miss these cases. This is measurement correctness, not a runtime speed improvement.

The full iOS native harness currently fails compilation with eight missing property/selector declaration errors in `DSSegmentViews.mm`. [The log is retained](../benchmarks/results/2026-10-07/performance-audit/ios-native-harness-failure.txt). The narrower current-source TextKit experiment passes. Repair the existing harness build before using it as an optimization gate; no product-source patch was made during this audit.

## Reference projects

Current upstream source was checked using refreshed local clones. These versions are distinct from the pinned dependencies in this repository and from `react-native-streamdown`.

- Vercel Streamdown at `1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc` already uses [verified unstable-tail parsing](https://github.com/vercel/streamdown/blob/1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc/packages/streamdown/lib/parse-blocks.tsx#L101) and [memoized blocks](https://github.com/vercel/streamdown/blob/1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc/packages/streamdown/index.tsx#L481). Downstream implements those ideas natively. Its [deferred rendering](https://github.com/vercel/streamdown/blob/1ddd8f4accd87dfc0e933f257c9d6ffe0fa85ccc/packages/streamdown/hooks/use-deferred-render.ts#L107) is a useful reference for expensive custom diagrams: wait for visibility and retain only the latest pending request.
- Enriched Markdown at `559f5164ce00b77ddfb3135770641edc32553abc` uses [view-free native measurement](https://github.com/software-mansion/react-native-enriched-markdown/blob/559f5164ce00b77ddfb3135770641edc32553abc/packages/react-native-enriched-markdown/ios/internals/ENRMViewFreeMeasurement.h#L25). Downstream already measures unattached native text stacks and prepares layouts on workers. Preserve this separation.

Neither comparison establishes which renderer is faster on a phone. The repository's DOM/subprocess comparison benchmark explicitly measures different adapters and cannot rank shipping native renderers.

## Implementation order and proof

1. Repair trace pairing and the existing iOS harness build. Add per-update prepare timing and cache hit/miss counts to the comparison report. Keep JS frame callbacks labeled as estimates.
2. Apply Android state atomically and add byte-cost budgets to both native rendering caches. Measure synchronization counts and retained tails while checking same-document changes and active-view ownership.
3. Measure width mismatch frequency in release builds, then remove avoidable iOS mount-time preparation misses. Normalize highlighting variants for code-free content. Preserve exact geometry and appearance.
4. Avoid redundant fence token copying and add a trusted append-only highlighter path. Re-run size/chunk scaling and existing retained-snapshot, Unicode, replacement, and highlighting parity tests.
5. Design reference-aware parsing and changed-tail native rendering separately. Add cell reuse and slot deltas where the application's fixtures use them. Defer persistent-prefix storage unless many-block workloads show it dominates.

Use the same fixture, chunk boundaries, arrival schedule, device, release revision, and feature settings for before/after runs. Alternate candidate order and collect cold and warm runs. Measure one and several simultaneous streams, ordinary prose, long paragraphs, code with and without newlines, unresolved references, tables, and optional custom renderers.

Acceptance requires unchanged output at every prefix and on finish; valid old snapshots after append/reset; correct links, copying, selection, caret, and accessibility; reduced work in the targeted counters and CPU measurements; and no regression in device p95/p99 frame responsiveness, delivery latency, or peak memory. Set speed targets from device baselines rather than host timings. No runtime improvement is claimed by this analysis-only pass.

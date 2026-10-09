# Performance follow-up

This follow-up starts from the earlier uncommitted performance implementation. The earlier benchmark-results directory was removed from the workspace; this work preserves that cleanup. New measurements compare a source snapshot of the preceding implementation with the follow-up changes, rather than comparing everything with the original Git commit.

## Android paragraph preparation

Plain non-table inline runs inherit the block's existing typography spans. They now skip redundant block, font and theme lookups when their style bits are zero and both link and tag indices are negative. Tables, links, tags and styled runs keep the full path. The production-code check uses 4,096 plain runs and confirms zero inline typography walks, with fallback cases for tables and every nonzero style bit.

Canonical mode still uses Android's `PrecomputedText` shaping and high-quality line breaking for changed paragraphs. In the installed Android 36 source, `PrecomputedText.create` only reuses a matching existing `PrecomputedText` with unchanged contents. `createMeasuredParagraphs` builds each newline-delimited paragraph with a null measurement hint; the constructor and reusable paragraph measurement data are private or hidden. `StaticLayout` consumes this cache only from an actual `PrecomputedText`. Appending through a spannable builder cannot preserve it. `DynamicLayout` expands edits to the entire changed paragraph, so an editable suffix does not make an unbroken growing paragraph cheap and would move that work onto the UI thread.

The subsequent [Android paragraph implementation](android-paragraph-layout.md) adds an explicit `androidParagraphLayout="incremental"` option with a separate live layout backend. Eligible plain paragraphs reuse completed greedy line layouts and shape a bounded suffix. Settlement restores canonical high-quality layout. The default remains canonical because the two wrapping policies can produce different line breaks.

## Shared fence snapshots

`Block.text` now uses `SnapshotText`. Ordinary parser construction uses a copy-on-write builder; verified streaming fence appends use immutable 1,024-unit pages and a persistent index. Each exact snapshot can cache a contiguous string under a mutex when a consumer needs one. Later appends cannot mutate an older snapshot's pages or cached text.

Trusted highlighting receives the appended text directly. Empty, unknown and oversized languages avoid unnecessary materialization. The parser also retains its confirmed raw fence-line position, so checking an append no longer rescans a growing unfinished line. Token snapshots use immutable shared vectors: document copies and deferred updates share them, while a real highlight refresh still copies its complete output vector once. This does not implement persistent stable-prefix token storage.

Proof includes 5,000 retained text roots across page/index boundaries, eight concurrent readers, builder detachment and compatibility, token ownership, Unicode/newline/closure/reset cases and skipped highlighting. A 60 KiB fence fed in 16-byte chunks copies 9,498,160 logical page/index bytes, scans 61,440 appended source bytes, and leaves its parser-only text unflattened. These counters do not measure allocator traffic or peak heap. UIKit/JNI still need a contiguous string for each snapshot they actually render or export.

## Paired measurements

The [raw samples](performance-followup-data/stream-scaling.json) use the same release C++ fixture for the saved preceding implementation and this follow-up. Apple M5, ARM64, Apple clang 21, `-O3 -DNDEBUG`. Execution order is baseline/current/current/baseline; each invocation has one warmup and five measured samples. These medians pool ten samples per variant.

| Workload                                  |   Size | Chunk | Preceding implementation ms | Follow-up ms |
| ----------------------------------------- | -----: | ----: | --------------------------: | -----------: |
| TypeScript fence with newlines            | 60 KiB |  16 B |                      26.147 |        8.593 |
| TypeScript fence, unfinished single line  | 60 KiB |  16 B |                      39.943 |        9.456 |
| Plain fence, unfinished single line       | 60 KiB |  16 B |                      28.053 |        1.253 |
| Separated paragraphs                      | 96 KiB | 128 B |                       5.087 |        5.262 |
| Early reference definition and paragraphs | 96 KiB | 128 B |                       5.164 |        5.245 |
| One unbroken plain paragraph              | 96 KiB | 128 B |                       3.333 |        3.325 |

The fixture measures cumulative synchronous parser/highlighter work after every chunk, excluding finish, scheduling, JS delivery, native preparation and presentation. Both variants pass live/final parity checks before timing; only explicitly incomplete live colors may differ. Prose shows little change and a small measured cost increase in separated paragraphs. The substantial fence gains do not establish device-frame performance.

The comparison snapshot is `/tmp/downstream-followup-baseline`. Reproduce in the current workspace with:

```sh
DOWNSTREAM_BENCH_COMPARE_ROOT=/tmp/downstream-followup-baseline node benchmarks/native-stream-scaling.mjs
```

The separate [retained-snapshot probe](../benchmarks/stream-snapshot-retention.cpp) uses fresh processes and captures whole-process high-water RSS before contiguous consumers or finish. Each mode runs in baseline/current/current/baseline order. For a 60 KiB plain fence in 16-byte updates, the median of two fresh processes per variant was:

| Retention policy                     | Preceding implementation peak RSS | Follow-up peak RSS |
| ------------------------------------ | --------------------------------: | -----------------: |
| Keep only the latest native snapshot |                         4.477 MiB |          3.313 MiB |
| Retain all 3,840 native snapshots    |                       263.609 MiB |         20.188 MiB |

[Raw RSS values](performance-followup-data/snapshot-retention.json) include process overhead and allocator retention. Indexed old text is checked after more appends, with final text checked after closure. This synthetic result demonstrates immutable storage retention; it is not an isolated live-heap measurement or an application/device memory prediction. Rendering every retained snapshot would also materialize its native strings.

```sh
DOWNSTREAM_BENCH_COMPARE_ROOT=/tmp/downstream-followup-baseline node benchmarks/native-snapshot-retention.mjs
```

## Viewport virtualization

Long documents with `selectable={false}` can now recycle eligible offscreen native text views. Exact cached frames and the actual ancestor clip determine the visible range with overscan. For unchanged geometry, scrolling reconciles the old and new visible intervals rather than walking every segment. Nonmonotonic frames, including those produced by negative margins, use the full correctness fallback. Unknown viewports also retain full mounting.

Interactive, hosted, decorated or otherwise unsupported content keeps its existing views. Selection and accessibility retain access to the complete document. Android disables recycling when any accessibility service is enabled. iOS restores all text views for VoiceOver, Switch Control or explicit accessibility enumeration. Eligibility differs slightly between platforms according to their native text/decorations models.

Android's production-method test keeps at most 13 text views for 10,000 eligible slots, with fewer than 100 segment reads per scroll. UIKit's actual view test uses 200 segments, one initial text assignment, at most three mounted text views and at most 14 segment reads per scroll. Across the platform checks, coverage includes cold geometry and text parity, replacement, recycled content, hidden/re-exposed views, padding, selection/accessibility restoration and observer detachment. The padding fixture runs on Android.

iOS shares one display link with weak registration among attached eligible documents, because ancestor clipping can change without laying out the document itself. Unchanged clips return immediately, and documents without deferrable text do not register. This carries a per-display-frame clip-check cost. Full document measurement and mixed-content views remain; this implementation virtualizes guarded text views rather than all rendering and shaping work.

## Physical-device measurements

The device inventory found no attached Android or iOS device. Android has an available Android 36 arm64 emulator, and iOS has an iPhone 18 Pro simulator running iOS 27. Simulator results cannot establish physical-device FPS, heap behavior or Hermes GC.

Release measurements on hardware remain dependent on connecting a device. The existing Bench screen reports JS callback timing; native frame presentation and memory require platform profilers such as Android `gfxinfo`/`meminfo` and Xcode Instruments.

## Validation

[Verification output](performance-followup-data/verification.txt) records 749 JavaScript tests, 365 native tests, TypeScript checks, full Kotlin checks, Android Kotlin/JNI typechecking, Fabric syntax checks and actual UIKit layout/reveal/math checks. The Downstream pod built for both simulator architectures, including the new viewport module. Local pod source registration was regenerated without upgrading dependencies or changing the lockfile. Lint, formatting and whitespace checks passed.

Independent review covered storage ownership, concurrent reads, skipped-highlighter paths, viewport coordinates, frame ordering, cache immutability, accessibility and observer lifecycles. The coordinate, ordering and frame-cache defects found during review were fixed and tested. Changes remain uncommitted.

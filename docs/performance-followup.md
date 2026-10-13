# Performance follow-up

The parser, Android paragraph and viewport work is committed in `72e8ff7`. The next changes add persistent token prefixes and paged native text bridges. Earlier raw benchmark and report directories were removed from the workspace; this document preserves that cleanup and links to executable fixtures instead.

## Android paragraph preparation

Plain non-table inline runs inherit the block's existing typography spans. They now skip redundant block, font and theme lookups when their style bits are zero and both link and tag indices are negative. Tables, links, tags and styled runs keep the full path. The production-code check uses 4,096 plain runs and confirms zero inline typography walks, with fallback cases for tables and every nonzero style bit.

Canonical mode still uses Android's `PrecomputedText` shaping and high-quality line breaking for changed paragraphs. In the installed Android 36 source, `PrecomputedText.create` only reuses a matching existing `PrecomputedText` with unchanged contents. `createMeasuredParagraphs` builds each newline-delimited paragraph with a null measurement hint; the constructor and reusable paragraph measurement data are private or hidden. `StaticLayout` consumes this cache only from an actual `PrecomputedText`. Appending through a spannable builder cannot preserve it. `DynamicLayout` expands edits to the entire changed paragraph, so an editable suffix does not make an unbroken growing paragraph cheap and would move that work onto the UI thread.

The `androidParagraphLayout="incremental"` option adds a separate live layout backend. Eligible plain paragraphs reuse completed greedy line layouts and shape a bounded suffix. Settlement restores canonical high-quality layout. The default remains canonical because the two wrapping policies can produce different line breaks. Styled text, contextual scripts, custom fonts, right-to-left text, carets and oversized unfinished suffixes keep the canonical path. The [Android runtime fixture](../tests/android/paragraph-runtime/README.md) checks actual framework layout, settlement, accessibility and rendering.

## Shared fence snapshots

`Block.text` now uses `SnapshotText`. Ordinary parser construction uses a copy-on-write builder; verified streaming fence appends use immutable 1,024-unit pages and a persistent index. Each exact snapshot can cache a contiguous string under a mutex when a consumer needs one. Later appends cannot mutate an older snapshot's pages or cached text.

Trusted highlighting receives the appended text directly. Empty, unknown and oversized languages avoid unnecessary materialization. The parser also retains its confirmed raw fence-line position, so checking an append no longer rescans a growing unfinished line.

Token snapshots now use immutable 64-entry pages with a persistent index. A trusted highlight refresh shares completed prefix pages and republishes the dirty suffix, including the boundary token that can merge with it. A full rewrite with no stable prefix publishes one immutable contiguous vector, avoiding unnecessary page-index allocation. Document copies and deferred updates share the exact publication. General vector access remains compatible through a lazy contiguous cache; indexed access and iteration read pages directly. `highlightTokenBytesCopied` counts copied publication payload, excluding index-node allocation.

Proof includes 5,000 retained text roots across page/index boundaries, eight concurrent readers, builder detachment and compatibility, 6,000 token suffix updates, token ownership, Unicode/newline/closure/reset cases and skipped highlighting. A 60 KiB fence fed in 16-byte chunks copies 9,498,160 logical text-page/index bytes, scans 61,440 appended source bytes, and leaves its parser-only text unflattened. These counters do not measure allocator traffic or peak heap.

## Paged native text bridges

Android exports a single nonempty code block as immutable Java string pages. `PagedText` implements `CharSequence` and `GetChars`, so Android can copy requested ranges without first joining the C++ source. A bounded weak cache reuses unchanged Java pages while both the native page and Java string remain alive. The rendered code segment retains its source pages through garbage collection; its cache cost includes their UTF-16 payload, and React Native's existing `DoNotStrip` rules preserve the ownership field in release builds. Replaced tail pages, dead owners and collected Java strings become misses. Mutable builder snapshots bypass cache reuse. General paragraphs, tables, math and multi-block ranges keep their existing string export.

On Android 36 ART, the [paged-text runtime fixture](../tests/android/paragraph-runtime/PagedTextChecks.kt) retained 257 append snapshots. Full-prefix transfer would copy 1,507,784 UTF-16 units; the production page bridge copied 132,552, reused 1,347 pages and recorded zero native flattened bytes. Retained text, surrogate seams, replacement/reset, concurrent reads, garbage collection and cache eviction passed. An actual `CodeSegment` retained page identity through GC with no additional JNI character transfer, then released those pages when discarded. Canonical body-view geometry, pixels, accessibility and selection matched an ordinary string. The fixture uses a test-only snapshot registry and minimal rendered-document collaborator, so it checks the production page bridge, model ownership and body view rather than the entire metadata exporter or code-card chrome.

iOS uses an immutable `DSSnapshotString` backed by the exact `SnapshotText`. Foundation reads characters and ranges without a C++ contiguous cache; substring views retain the same immutable snapshot. Small cached table cells own only their copied range. Code line indexing and UTF-8 conversion also read pages directly. Actual UIKit tests verify code, styled prose and table preparation with zero native flattened bytes, plus layout, pixels, selection, retained snapshots and concurrent reads.

These changes remove C++ flattening and repeated Android prefix-character transfer. Android still creates span/precomputed-text buffers, and Foundation/TextKit still create attributed-string and text-storage buffers. Code token metadata, line indexing, shaping and document measurement can still visit the full block. This is not an end-to-end linear-time renderer or a guarantee of zero platform copies.

## Paired measurements

The following historical measurements were recorded for the text-page implementation now in `72e8ff7`, against its preceding source snapshot. Their raw sample files and the benchmark fixtures were removed. The stream-scaling fixture used identical release C++ sources for both variants. Apple M5, ARM64, Apple clang 21, `-O3 -DNDEBUG`. Execution order was baseline/current/current/baseline; each invocation had one warmup and five measured samples. These medians pooled ten samples per variant.

| Workload                                  |   Size | Chunk | Preceding implementation ms | Follow-up ms |
| ----------------------------------------- | -----: | ----: | --------------------------: | -----------: |
| TypeScript fence with newlines            | 60 KiB |  16 B |                      26.147 |        8.593 |
| TypeScript fence, unfinished single line  | 60 KiB |  16 B |                      39.943 |        9.456 |
| Plain fence, unfinished single line       | 60 KiB |  16 B |                      28.053 |        1.253 |
| Separated paragraphs                      | 96 KiB | 128 B |                       5.087 |        5.262 |
| Early reference definition and paragraphs | 96 KiB | 128 B |                       5.164 |        5.245 |
| One unbroken plain paragraph              | 96 KiB | 128 B |                       3.333 |        3.325 |

The fixture measures cumulative synchronous parser/highlighter work after every chunk, excluding finish, scheduling, JS delivery, native preparation and presentation. Both variants pass live/final parity checks before timing; only explicitly incomplete live colors may differ. Prose shows little change and a small measured cost increase in separated paragraphs. The substantial fence gains do not establish device-frame performance.

The separate retained-snapshot probe used fresh processes and captures whole-process high-water RSS before contiguous consumers or finish. Each mode runs in baseline/current/current/baseline order. For a 60 KiB plain fence in 16-byte updates, the median of two fresh processes per variant was:

| Retention policy                     | Preceding implementation peak RSS | Follow-up peak RSS |
| ------------------------------------ | --------------------------------: | -----------------: |
| Keep only the latest native snapshot |                         4.477 MiB |          3.313 MiB |
| Retain all 3,840 native snapshots    |                       263.609 MiB |         20.188 MiB |

These historical RSS values include process overhead and allocator retention. Their raw files were removed. Indexed old text is checked after more appends, with final text checked after closure. This synthetic result demonstrates immutable storage retention; it is not an isolated live-heap measurement or an application/device memory prediction. Rendering retained snapshots also creates platform text buffers.

## Token-prefix measurements against `72e8ff7`

The new measurements compare an immutable export of `72e8ff7` with the current sources on the same Apple M5 release configuration. The stream-scaling runner performed ABBA execution with one warmup and five samples per invocation, pooling ten samples per variant. Live/final correctness checks run before timing.

| Workload                                  |   Size | Chunk | `72e8ff7` ms | Current ms |
| ----------------------------------------- | -----: | ----: | -----------: | ---------: |
| TypeScript fence with newlines            | 60 KiB |  16 B |        8.852 |      6.221 |
| TypeScript fence, unfinished single line  | 60 KiB |  16 B |        9.191 |      9.209 |
| Plain fence, unfinished single line       | 60 KiB |  16 B |        1.222 |      1.204 |
| Separated paragraphs                      | 96 KiB | 128 B |        5.061 |      5.167 |
| Early reference definition and paragraphs | 96 KiB | 128 B |        5.135 |      5.186 |
| One unbroken plain paragraph              | 96 KiB | 128 B |        3.248 |      3.304 |

The completed-line fence copied 283,194,780 token payload bytes before and 1,905,564 after. The single-line fence has no completed prefix to share and copied the same 1,746,888 bytes in both variants. Its initial paged full-rewrite implementation measured about 9% slower; the contiguous full-rewrite path above removed that measured overhead. Prose medians were about 1–2% higher, and these changes do not establish a prose speedup.

The token-retention fixture fed 46,800 TypeScript bytes as 5,200 complete 9-byte lines. Its runner used two ABBA rounds per retention policy, with each sample in a fresh process. Correctness checks cover final tokens and retained roots after further append and settlement, outside timing and RSS capture. Medians use four samples per variant.

| Metric                           |     `72e8ff7` |     Current |
| -------------------------------- | ------------: | ----------: |
| Token publication payload copied | 649,084,800 B | 2,370,432 B |
| Latest-snapshot streaming time   |     14.516 ms |    7.379 ms |
| Latest-snapshot peak process RSS |     5.805 MiB |   5.133 MiB |
| Retain-all streaming time        |     45.127 ms |    8.182 ms |
| Retain-all peak process RSS      |   686.133 MiB |  41.883 MiB |

Token payload copying fell about 274 times in this fixture. The retained case used about 16.4 times less peak process RSS. RSS includes allocator retention and process overhead; timing excludes finish, rendering, JS delivery and frame presentation. Fresh-process timings varied, including a slower first current latest-only sample. Earlier exploratory samples were also variable and are separate from the two reported ABBA rounds. These host results are not application/device memory or FPS predictions.

These host benchmarks have since been removed from the repository.

## Viewport virtualization

Long documents with `selectable={false}` can now recycle eligible offscreen native text views. Exact cached frames and the actual ancestor clip determine the visible range with overscan. For unchanged geometry, scrolling reconciles the old and new visible intervals rather than walking every segment. Nonmonotonic frames, including those produced by negative margins, use the full correctness fallback. Unknown viewports also retain full mounting.

Interactive, hosted, decorated or otherwise unsupported content keeps its existing views. Selection and accessibility retain access to the complete document. Android disables recycling when any accessibility service is enabled. iOS restores all text views for VoiceOver, Switch Control or explicit accessibility enumeration. Eligibility differs slightly between platforms according to their native text/decorations models.

Android's production-method test keeps at most 13 text views for 10,000 eligible slots, with fewer than 100 segment reads per scroll. UIKit's actual view test uses 200 segments, one initial text assignment, at most three mounted text views and at most 14 segment reads per scroll. Across the platform checks, coverage includes cold geometry and text parity, replacement, recycled content, hidden/re-exposed views, padding, selection/accessibility restoration and observer detachment. The padding fixture runs on Android.

iOS shares one display link with weak registration among attached eligible documents, because ancestor clipping can change without laying out the document itself. Unchanged clips return immediately, and documents without deferrable text do not register. This carries a per-display-frame clip-check cost. Full document measurement and mixed-content views remain; this implementation virtualizes guarded text views rather than all rendering and shaping work.

## Physical-device measurements

The device inventory found no attached Android or iOS device. Android has an available Android 36 arm64 emulator, and iOS has an iPhone 18 Pro simulator running iOS 27. Simulator results cannot establish physical-device FPS, heap behavior or Hermes GC.

Release measurements on hardware remain dependent on connecting a device. The existing Bench screen reports JS callback timing; native frame presentation and memory require platform profilers such as Android `gfxinfo`/`meminfo` and Xcode Instruments.

## Validation

The earlier implementation passed 749 JavaScript tests, 365 native tests, TypeScript checks, full Kotlin checks, Android Kotlin/JNI typechecking, Fabric syntax checks and actual UIKit layout/reveal/math checks. Its Downstream pod built for both simulator architectures. The old verification log was removed with the report data.

The new implementation passed 752 JavaScript tests, 366 native tests, root and example TypeScript checks, full Kotlin checks, Android Kotlin/JNI compilation and Fabric syntax checks. Actual UIKit layout, reveal and math checks passed. The Downstream pod built for ARM64 and x86_64 simulators, including `DSSnapshotString`. Pod source registration was regenerated without changing the dependency lockfile.

Android's JNI/paged-text checks passed on actual ART with `DOWNSTREAM_PAGED_ONLY=1`; that mode explicitly skips paragraph timings. The existing paragraph checks had already passed before the separate paged checks during full fixture runs. The host `PagedText` range/ownership/bounds suite also passed. Lint exits successfully with existing repository warnings.

Independent review of the new token and bridge changes found no correctness issues. A separate 10,000-update randomized token-suffix check passed AddressSanitizer and UBSan while checking retained snapshots.

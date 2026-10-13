# Android memory and jank follow-up

This pass starts from `ac8b5c4cf93dd1a97206fe0e810e2b4d215d1669`. The physical phone was unavailable. Changes target verified retention and main-thread allocation; their effect on phone PSS and frame deadlines remains unmeasured.

## Existing phone evidence

The local, git-ignored `benchmarks/phone/REPORT.md` records a OnePlus CPH2569 running an Android 15 arm64 Hermes release build. The original colored first-AI replay used 6.35 CPU seconds, versus 7.77 for RN Streamdown and 7.09 for Enriched. Its HWUI jank was 31.86%, versus 11.11% and 12.69%. Downstream's rich-AI replay used 9.67 CPU seconds with 36.33% jank. These results precede later optimization experiments; they are not measurements of this patch.

The later `benchmarks/phone/OPTIMIZATIONS.md` records two rejected code-card experiments and rejected reductions of the document cache budget. Removing local work did not reliably improve whole-app results. Diagnostic traces placed instrumented span/precomputed-text preparation on workers. They did not measure TextView's internal measurement, layout or drawing. Rich-AI pilot memory included 42.63 MiB of graphics and 46.02 MiB of mapped code, alongside Java/native heaps. This evidence does not identify one root cause for the remaining frame misses or justify replacing canonical text layout without more measurement.

## Removed paragraph retention

`RenderedDocument.plainParagraphs` has a 2 MiB estimated budget. Its separate `plainParagraphLanes` index strongly owned complete `PlainParagraphKey` objects even after budget eviction. Each key can contain 32,768 characters, and the index holds 128 lanes.

An extracted-production regression creates 128 distinct maximum-sized lane keys. Before the fix, all 128 survive garbage collection although only two entries fit in the paragraph cache. They contain 4,194,304 characters, equivalent to 8 MiB of logical UTF-16 payload. This is a reachability test, not a measured Android heap or process-memory saving; JVM string storage and object overhead differ.

Lane values now weakly reference keys. The real cache still owns live keys, so append-prefix retirement and cache hits work as before. After eviction, lane bookkeeping no longer owns the paragraph text. The same regression now collects 126 keys and retains the two cache-owned keys. Existing checks cover repeated-content reuse, growing-prefix retirement, settings isolation and retained segment validity.

## Reading metadata on demand

Text assignment previously rebuilt accessibility reading stops for every streamed revision. Table assignment likewise rebuilt the shared header/body reading list, including row strings and link pieces. Those results were created even when no accessibility provider requested them.

Text now invalidates its current reading cache on assignment and builds it on the first query. Tables share one demand-built holder for the current segment. Providers still answer explicit queries even when accessibility services are disabled. Content-change notifications remain immediate. An existing accessibility focus still causes the count to be checked so an invalid focus is cleared; an unfocused update does not need that count.

Production-method checks verify zero reading builds over 100 unqueried text or table revisions, one build on first query, shared table entries, current content after subsequent updates, focus shrink/reset, headings, links, Unicode, details metadata and selection/canonical fallback. This removes work from ordinary streaming; active accessibility can still require a build on each revision. Layout policy, text assignment and displayed content are unchanged.

## TextView diagnostics

Tracing now includes `android_text_measure`, `android_text_layout` and `android_text_draw` start/end markers around the production view calls. The view keeps only a document ID for tracing. Unchanged shared segments refresh that ID; clearing content clears it. Boundaries balance through early returns and exceptions, and disabled tracing avoids marker strings and event delivery.

The local phone trace analyzer additionally aggregates paired durations by session, generation, thread and stage. It reports call counts, total/max wall time and calls over 8 or 16 ms. Stages can nest, so their totals must not be summed as CPU time. A long callback is not proof of a missed presentation deadline. Diagnostic samples remain excluded from ordinary performance comparisons.

## Validation and phone retest

Passed `pnpm test:kotlin`, `pnpm typecheck:android`, all 30 phone collector/analyzer unit tests, and `git diff --check`. The Android paragraph/runtime fixture passed on the `downstream-test` emulator using `DOWNSTREAM_PARAGRAPH_AVD=downstream-test node tests/android/paragraph-runtime/run.mjs`. It uses actual Android text, accessibility, layout and raster APIs with test collaborators around the React host. It does not establish phone frame rate or PSS.

For the next phone session, build matching highlighted release baseline/candidate APKs from the starting revision and this patch, retaining source manifests and APK hashes. Alternate their order for five pairs of `ai-replay`, `ai-rich-text-replay` and `longParagraph10KiB`. Preserve exact source, chunks, schedules, settling interval and feature settings. Record untraced CPU, PSS/RSS, HWUI jank and thermal conditions. Collect traced runs separately to inspect the new TextView stages alongside mount and worker preparation.

The lane-retention regression concerns multiple distinct paragraphs, so also check a long conversation and repeated source replacement within one process. A fresh-process single-paragraph replay may not expose this ownership bug. Include TalkBack queries, selection, code colors, table header/body navigation and scrolling in functional device checks. No general jank or PSS improvement should be claimed until those measurements pass.

# Parser and native layout improvements

This follows the [first performance fixes](../performance-fixes/README.md).

## Verified changes

| Workload | Before | After | Evidence |
| --- | --- | --- | --- |
| 96 KiB response beginning with unresolved `[ref]`, 128-byte chunks | 38,273,853 parsed bytes, 768 full parses | 142,046 parsed bytes, 1 full parse | Real C++ parser, original streaming fixture |
| 32 KiB growing iOS paragraph, 20 snapshots | 10.386 ms mean measurement per update | 0.476 ms | Paired simulator fixture with only workspace reuse disabled in the control |
| 100 growing two-column Android table rows | 10,300 cell copies, desired-width calls and layouts | 202 of each | Actual production table builder with host work counters |
| 60 KiB highlighted fence, 16-byte updates | 23.120 ms cumulative CPU | 21.658 ms, 6.3% less | Paired immutable release binaries, ABBA order repeated three times |
| Code-free iOS content prepared with highlighting and mounted without it | Another rendering, segment and layout | Same rendering, zero extra layout calls | Native TextKit regression and original fixture |

Reference-bearing closed regions are shared provisionally, with identity zero and no additional settled blocks. A definition marker, relevant math opener, preview activation, replacement, reset, or parse-option change invalidates that reuse. A late definition therefore reparses the affected source before publication. Defined-reference and preview workloads retain conservative fallback behavior. The strict committed prefix and retained old snapshots remain valid.

iOS measurements use two private TextKit workspaces per executing thread, indexed by exact width. They retain at most 65,536 UTF-16 code units each; code cards, oversized text and unconstrained widths use fresh stacks. Text and attribute comparison identifies the unchanged prefix, and edits include the preceding composed character. Immutable snapshots keep scalar measurements, while workspaces own the mutable layout objects. Mounted views use the same suffix edit and preserve the existing selection-reset behavior. Closing Markdown that restyles earlier text invalidates from that earlier position.

The workspace cap is per thread and counts text characters, not total heap bytes. Attributes and TextKit allocations add memory. The measured iOS gain is a native simulator measurement result, not phone FPS or end-to-end response latency. Exact widths remain distinct; the patch does not round constraints or guarantee that every mount-width miss disappears.

Android caches only source cells without inline spans. Cached text is immutable. Keys cover content, theme, scale, density, appearance, header, alignment, locale and actual cell-layout width. Geometry is recomputed for every snapshot, including column growth and shrink. Text and layout caches each have an approximate 2 MiB budget and a 1,024-entry cap. Formatted, linked and formula cells retain the existing path. The host fixture proves reuse and invalidation; it does not measure Android shaping time.

The fence highlighter uses exact previous-snapshot identity and consumed length to skip repeated whole-prefix verification after verified appends. General callers retain the checked path, including surrogate-boundary handling. Snapshot and token-vector copies still remain; this does not make the complete fence path linear.

## Reproductions

- `node benchmarks/results/2026-10-07/performance-audit/stream-scaling.mjs` runs the real parser/highlighter size and chunk fixture. [stream-before.txt](stream-before.txt) records the original source; [stream-after.txt](stream-after.txt) records the reference-reuse path. These separate runs establish scaling and work counts, not controlled timing percentiles.
- `bash benchmarks/results/2026-10-07/performance-followups/ios-tail-workspace.sh` compares identical current sources with workspace reuse disabled and enabled. [ios-tail-fresh.txt](ios-tail-fresh.txt) and [ios-tail-workspace.txt](ios-tail-workspace.txt) retain both outputs.
- `node tests/android/table-streaming.mjs` compares the production builder with cell-cache eligibility disabled and enabled. [android-table.txt](android-table.txt) retains its counters.

The token-vector suffix-update experiment was removed after it increased allocation work. Trusted append verification is retained separately. [fence-paired-raw.txt](fence-paired-raw.txt) and [fence-paired-summary.json](fence-paired-summary.json) record six executions per variant, each reporting the median of seven timed samples after a warmup. The control keeps reference reuse but restores checked highlighting. Highlighted 8/16/32/60 KiB fences reduced cumulative CPU by 10.9/13.1/10.9/6.3%. Plain-fence timings varied within roughly 2%; no plain-fence improvement is claimed.

## Validation

- `pnpm test`: 595 tests in 24 files passed.
- `pnpm test:native`: 329 passed; one existing RaTeX vendor-dependent test skipped. The new invariant entry point runs reference, work-bound and retained-fence-snapshot checks that the previous batch-only runner missed.
- `pnpm test:kotlin`: passed, including table geometry/invalidation and the earlier state/cache suites.
- `pnpm typecheck:android`: production Kotlin, generated interfaces and both JNI translation units passed.
- `pnpm test:fabric`: both platforms passed syntax and host-rectangle checks.
- `pnpm test:ios:layout`: passed, including fresh TextKit geometry for font scale, heading/quote metrics, caret/attachment changes, exact widths, old/interleaved snapshots, Unicode, RTL and concurrent measurement. [ios-layout-final.txt](ios-layout-final.txt) retains the output. Mounted tail edits total 140 characters versus 656,620 for full replacement over 20 updates.
- `pnpm test:ios:reveal` and `pnpm test:ios:math`: passed. [Reveal](ios-reveal-final.txt) and [math](ios-math-final.txt) outputs are retained.
- `pnpm lint`, `pnpm typecheck`, `pnpm format:check` and `git diff --check`: passed. Lint reports existing warnings.

Physical-device frame latency and peak heap measurements remain outstanding. Reference-definition dependency indexing, immutable fence storage, Android paragraph suffix layout and custom-renderer slot deltas remain separate follow-up work.

Independent review found no supported runtime correctness issue. It confirmed exact-width matching, snapshot ownership, reference revocation, cell invalidation and attachment restyling; final metric/caret/attachment checks passed afterward.

# First streaming performance fixes

Implemented against audit baseline `144fd321f253dad53d0efe33eacbde6922c6dcfb`.

## Changes and measured proof

| Change | Before | After | Evidence scope |
| --- | --- | --- | --- |
| Android applies incoming state atomically | 4 segment synchronizations, 2 rendering lookups | 1 synchronization, 1 lookup | Host counters execute production state methods for a document/caret/image/host change |
| Native snapshot cache estimates include changing-tail cost | Count cap only | 8 MiB approximate metadata/tail budget, count cap 128 | Production cache logic and eviction/lifetime tests |
| iOS retained tail renderings in the original 128-snapshot fixture | 128 renderings, 4,243,072 characters | 15 renderings, 502,320 characters | Real native renderer, iPhone 18 Pro simulator, iOS 27.0 |
| Native trace interval pairing | Mismatched and overlapping sequences can pair incorrectly | Intervals stay within their snapshot sequence | Two new regression cases failed before the fix and pass after |
| iOS native layout harness | 8 declaration errors | Compiles and passes | Existing harness with original production sources |

The before-cache observation is in [the audit output](../performance-audit/ios-tail-results.txt); the rerun is in [ios-tail-after.txt](ios-tail-after.txt). Use the original [reproduction command](../performance-audit/README.md#ios-tail-layout) to repeat it. Only retention counts are compared. The reported layout timings come from separate runs and are not a controlled measurement of CPU gains from this patch.

The 8 MiB estimate covers snapshot metadata and independently built tails. Shared committed segments are excluded from that estimate, and renderings can keep them alive after the separate segment cache evicts them. Both platforms keep one latest oversized rendering outside the ordinary budget until the next insertion. Live views also hold references outside the cache. This is not an 8 MiB process-memory bound; iOS NSCache limits are advisory.

Android's state regression executes extracted production methods with host counters. It preserves independent same-document code/image/host/chrome changes, caret changes, no-op wrapper replacement, tracing, and theme fallback. It does not execute Android drawing, JNI, or the full Fabric lifecycle. The Android source set and JNI were checked against the real SDK and generated React Native interfaces separately.

## Validation

- `node --test benchmarks/summarize.test.mjs`: 10 passed. Output is in [trace-tests.txt](trace-tests.txt).
- `pnpm test`: 595 tests in 24 files passed.
- `pnpm test:kotlin`: cache eviction/concurrency, atomic state, reveal, code/image/math/host layout, and math rendering checks passed.
- `pnpm test:ios:layout`: native cache, held-value lifetime, selection/glyph mapping, gutter, text/table, and hosted-fence layout checks passed.
- `pnpm test:ios:reveal`: native reveal checks passed.
- `pnpm test:ios:math`: native math attachment and repeated-request checks passed.
- `pnpm typecheck:android`: Kotlin source set, real codegen, and both JNI translation units passed.
- `pnpm test:fabric`: iOS and Android Fabric/session syntax and host rectangle checks passed.
- `pnpm lint`, `pnpm typecheck`, `pnpm format:check`, and `git diff --check`: passed. Lint reports existing warnings.

An independent review found no supported runtime correctness regression. Its two wording findings were corrected to qualify shared-payload accounting and oversized-slot lifetime.

Reference-aware parsing, immutable fence storage, changed-suffix native text layout, and measurement/mount width alignment remain separate follow-up work. Physical-device frame and heap improvements are not yet measured.

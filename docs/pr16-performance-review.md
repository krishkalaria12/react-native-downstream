# PR #16 performance and CI review

Baseline: `fece47f4bbc53453aea5c0f10e5da08a4393193f`, branch
`feat/copy-download-controls`.

## Findings and changes

| Path | Verified problem | Change |
| --- | --- | --- |
| Android controls | Two Kotlin calls to the iOS-only `setNeedsLayout()` prevent compilation. | Use `requestLayout()` and apply per-block wrap choices even when the aggregate chrome flags do not change. |
| Android settled segments | `TextSegmentView.setContent()` clears the identity that `setSegment()` just stored. Settled text is therefore rebound on every streaming update. Code bodies also rebind unchanged segments. | Store text identity after binding and skip identical code segment binding. Selection and layout survive unchanged snapshots. |
| Android unwrapped code | `StaticLayout.width` is the supplied maximum constraint, giving code a roughly two-billion-pixel scroll width. | Cache actual shaped line extents, including tabs, trailing whitespace, RTL text and spans. |
| Android controls state | Applying controls rereads the immutable segment plan through JNI. | Keep the plan already obtained for the rendered snapshot. |
| Table geometry | Cell coordinates repeatedly sum preceding columns on Android and preceding rows and columns on iOS. | Cache prefix offsets on segment creation. Coordinates cost O(1) per cell. iOS also skips text outside the dirty rectangle. |
| iOS measurement | Finite-width layout computes unused unconstrained widths, then allocates frame objects twice. | Skip unused width queries and construct constrained frames once. |
| iOS exports | The documented worker threshold was unused; large table serialization and scratch-file writes run on main. | Serialize large exports and write files on the bounded worker pool. Retain the pressed immutable snapshot, guard reset/recycle, and prevent delayed copies overwriting newer clipboard contents. |
| Android exports | Large text and HTML exports are serialized through JNI on main; moving the lookup to a worker would lose the weakly registered snapshot. | Capture a strong document handle before queuing serialization, then deliver results to main with source, lifecycle and copy-order checks. Large download encoding also runs on the worker. |
| Android file delivery | React Native supplies a `ThemedReactContext`, so the direct activity cast rejects normal downloads. Completion uses an API28 method despite minSDK24, and a null output stream reports success. | Resolve the current activity, dispatch through a main handler, and report null-stream failures. Share one writer and use unique picker keys; accepted downloads still finish after view recycling. |
| Syntax highlighting | Full parses and uncommitted incremental tails lex unchanged code again. | Reuse completed tokens only when block kind, language and exact text match. Budget-cut results retry. Growing code retains its resumable lexer state. |
| Styled table exports | Every cell scans all row runs and builds temporary piece vectors. | Find the first overlapping run by binary search and visit pieces directly without temporary vectors. |
| JavaScript source validation | Appends and resets scan text once to validate Unicode and again to count bytes. | Return the byte count from the existing validation pass, preserving input limits and surrogate errors. |
| Local lint | Generated native projects and bundled JavaScript are included in the broad lint glob. | Ignore generated native projects, build output and web exports. |

## What transfers from Streamdown

Reviewed Vercel Streamdown at
[`08da224d303e1ef2516e2be4ce04d0c55fbd68ea`](https://github.com/vercel/streamdown/tree/08da224d303e1ef2516e2be4ce04d0c55fbd68ea).

Its [parser keeps completed blocks and parses the active tail](https://github.com/vercel/streamdown/blob/08da224d303e1ef2516e2be4ce04d0c55fbd68ea/packages/streamdown/lib/parse-blocks.tsx#L240-L305),
and its [block comparator skips unchanged content](https://github.com/vercel/streamdown/blob/08da224d303e1ef2516e2be4ce04d0c55fbd68ea/packages/streamdown/index.tsx#L481-L585).
Downstream already has native shared-prefix storage and incremental tail parsing.
The Android identity fix makes the view layer honor those savings. Replacing the
native parser with a JavaScript block parser would discard that architecture.

Streamdown [renders plain code while its highlighter loads and rejects stale async results](https://github.com/vercel/streamdown/blob/08da224d303e1ef2516e2be4ce04d0c55fbd68ea/packages/streamdown/lib/code-block/index.tsx#L35-L122).
Downstream keeps budgeted native highlighting and resumes growing code, while
reusing completed tokens. Async native exports follow the same requirement to
reject stale results before changing the UI or clipboard.

Existing append batching, native worker coalescing and snapshot publication once
per frame remain in place. Another debounce would add latency without removing
the work found in this review.

## Measurements

Host measurements do not include native UI layout, paint, bridge latency or
physical-device responsiveness. Compare the same machine, compiler and workload.

JavaScript validation and byte accounting, Node 24.15.0 on an Apple M5 host, median of
nine runs with 2,000 validations per run after warmup:

| Input | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| 64 KiB ASCII | 0.1847 ms | 0.0954 ms | 48% |
| 64 KiB mixed Unicode | 0.0824 ms | 0.0472 ms | 43% |

The before measurement calls the original validator plus the original separate
byte counter. The after measurement calls `checkedUtf8Bytes`. This isolates the
removed duplicate scan. `benchmarks/source-validation.bench.ts` measures the
current validator without subprocess or native parser overhead.

Native release-mode benchmarks used Apple M5, Apple clang 21, C++20 and `-O3`.
Both revisions ran the same benchmark harness, with seven samples per workload:

| Workload | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Styled table, 2,048 columns, 20 Markdown exports | 43.829 ms | 2.731 ms | 94%, 16.05× speedup |
| Same table, 20 HTML exports | 44.058 ms | 2.339 ms | 95%, 18.84× speedup |
| Mixed document, full parsing, 32-byte appends | 69.007 ms | 44.798 ms | 35% |
| Many fences, full parsing, 32-byte appends | 63.360 ms | 30.995 ms | 51% |
| Reference context, incremental, 32-byte appends | 63.744 ms | 31.239 ms | 51% |
| Reference context, incremental, 128-byte appends | 24.677 ms | 16.203 ms | 34% |

The reference workload holds completed code in an uncommitted tail. It exercises
the default incremental mode where token reuse matters despite incomplete
reference resolution. The ordinary mixed/fence incremental cases improved 8–14%
in this run, but variation across host runs was large enough that those smaller
differences should not be treated as a general speedup. Open-fence parsing was
effectively unchanged, 23.362 ms before and 23.423 ms after. An unchanged
`CodeDocument` append workload rose 6.9% despite no changes to its implementation.
Treat smaller differences cautiously and check any repeated regression under
controlled device measurements.

Raw samples, environment details, harness hash and baseline method are saved in
[the host results directory](../benchmarks/results/2026-09-30/host-pr16/).
Baseline runtime sources came from the baseline commit in a separate temporary
directory. The working tree was never swapped during other checks.

Native test build measurements on the same host:

| Run | Time | Object compilations |
| --- | ---: | ---: |
| Original builder | 18.59 s | Not instrumented |
| New builder, cold cache | 18.69 s | 57 |
| New builder, warm cache | 7.79 s | 0 |

The cold run has similar cost. The warm run removes compilation and linking.
These are local measurements, not predicted GitHub job durations. Cache
validation also checked source changes, header changes, unchanged compiler paths
despite unrelated PATH changes, and concurrent atomic publication.

## CI coverage and build work

The latest remote check run at the baseline was green, but it built only the
JavaScript package and web example. It missed the Android compilation errors and
an iOS block/nil conditional compilation error in `DSFileExporter.mm`.

- Run PR checks for stacked branches as well as `main`.
- Run the Node-only native engine suite independently from Vitest, avoiding a
  dependency install for the host suite and serial waits between the suites.
- Share content-addressed C++ objects and binaries between both test builders.
  Keys cover source, headers, compiler identity, flags and relevant include
  environment. Atomic publication prevents partial cache files under concurrency.
- Persist native test builds in Actions caches. Compiler cache installation is
  no longer needed.
- Compile Android Kotlin, generated bindings, JNI and Fabric for one ABI.
- Compile the iOS pod for the simulator. Native workflows use relevant path
  filters and Gradle and CocoaPods caches to bound unnecessary work. The iOS
  compile disables the index store rather than uploading a large build cache.

New workflow durations require an actual GitHub run after push. Adding platform
compilation improves coverage and can lengthen a cold complete run; the existing
fast checks finish independently. No publishing or deployment configuration is
changed.

## Validation

- `pnpm test`: 273 passed across 16 files.
- `pnpm test:native`: all 253 passed, including prefix parity, export goldens,
  late-reference resolution and incomplete-highlight retry checks.
- `pnpm lint`, `pnpm typecheck` and `pnpm prepare`: passed.
- Web example export: passed.
- `actionlint` for all three CI workflows and `git diff --check`: passed.
- iOS Downstream pod: compiled for arm64 and x86_64 Simulator.
- Android production `assembleDebug`: passed after removing temporary test
  fixture code. Kotlin, codegen, JNI and Fabric compilation are included.
- API36 Android emulator: settled text/code identity, preserved selection,
  changed content/style, unwrapped line extents and RTL/LTR table coordinates
  passed. Async checks verified callback thread choice, immutable snapshot
  retention and release, empty-table threshold, copy ordering across views,
  selection/external clipboard protection, stale generation/reset/release
  suppression, and independent download failure events. The fixture deliberately
  blocks workers to make completion-order checks deterministic.
- API36 exporter instrumentation also passed with an actual
  `ThemedReactContext`: activity resolution, `ACTION_CREATE_DOCUMENT` launcher
  registration, injected successful picker result after exporter retirement,
  exact written bytes, completion on main, and a null-stream provider error.
  This tests delivery without manually navigating the system picker. An API24
  device run remains unperformed.

The [raw Android async result](../benchmarks/results/2026-09-30/host-pr16/android-async-validation.log)
retains the emitted behavior summary. Temporary fixture code is absent from
production sources. The [exporter result](../benchmarks/results/2026-09-30/host-pr16/android-exporter-validation.log)
records the additional file-delivery checks.

## Remaining measurement limits

Physical-device release traces are still needed to measure frame times, memory,
layout and paint. Host benchmark improvements cannot establish an FPS increase.
The benchmark adapters that replay operations through subprocesses also cannot
measure a warm production streaming session. Follow
[the existing performance methodology](performance-methodology.md) for device
comparisons. Interactive iOS clipboard and document-picker testing remains a
manual check.

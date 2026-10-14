# Frame coalescing and the settle tail

The phone report identifies two different problems: repeated native/UI work as streams grow, and a late last frame after text delivery. This branch implements frame coalescing, cheaper canonical finalization, exact finish render reuse, broader Android viewport realization, a hard reveal backlog bound, and experiments that distinguish completion waiting from renderer drain. The missing historical slow traces still prevent exact attribution of the original tails.

## What changed

`src/cpp/session/FrameScheduler.h` coordinates one-shot native display callbacks. Android uses Choreographer through `SessionFrameClock.kt`, `NativeDocument.kt` and `DownstreamTextLayoutManager.cpp`. iOS uses a paused CADisplayLink in `DownstreamSessionFrameClock.mm`, installed by `DownstreamSessionProvider.mm`. Quiet sessions request no frames. Paused mobile hosts use asynchronous turns so explicit flush/finish still progress without display ticks. Foreground hosts return to vsync. Headless engine consumers retain asynchronous WorkerPool scheduling.

`SessionRegistry.cpp` now accumulates deltas until a frame permits worker dispatch. The worker parses and prepares an immutable result, then stages it separately from the published snapshot. The following tick installs the document and its source/tag metadata and emits readiness together. It can dispatch the next accumulated batch on that same tick. Worker completion alone cannot publish another version between ticks.

The foreground invariant is at most one native snapshot publication per session per display tick. All changed JS stream stores then share one requestAnimationFrame and one explicit React Native batch in `createMarkdownStream.ts`. Store revisions advance together before subscriber notifications. React Native still controls Fabric commits across surfaces and unrelated application updates; this is not an unconditional cap on every Fabric commit in the app.

Native cadence starts at one tick and is capped at four ticks. Preparation cost above 70% of the frame interval and late frame callbacks increase the stride. Eight healthy publications reduce it by one step. Finish and reset bypass the cooldown. At 60 Hz this permits deliberate batching down to about 15 updates per second under pressure. This policy needs device measurement, especially because larger batches can themselves cost more to prepare.

The worker remains off the main thread, and every completed prefix makes progress even while more input arrives. Staging preserves exact snapshot-version queries until publication. Reset invalidates old generations; disposal suppresses queued events; finish can publish settled content at the same generation and sequence as the live snapshot.

Trace phases include `finish_received`, `worker_queued`, `worker_start`, `parse_start/end`, `prepare_start/end`, `snapshot_staged`, and `snapshot_ready`. Together with existing Fabric measure and Android apply events these distinguish preparation from publication delay.

## Additional improvements implemented

### Canonical finish without reparsing a proven prefix

`IncrementalParser::canFinishTail` permits finalizing only the raw uncommitted tail when its prefix is proven canonical. Finish completes code highlighting and math layout, settles every block, and retains immutable prefix storage and identities. Open fences and plain paragraphs are no longer probed again after finalization, since no future append can use those probes. Repeating an unchanged finish returns the same document without parsing, highlighting or math work.

Global dependencies retain full canonical parsing: references and definitions, footnotes, bracket math, unresolved tags/containers, preview policies and reference budget limits. Formula overflow also requires a whole-document pass because the cap changes prefix rendering. That fallback now discards stale committed formula layouts. Final malformed math receives its canonical error instead of remaining provisionally incomplete.

Integration with the newer list-prefix parser retains full canonical finish for provisional list items and their synthetic list lead. Ordered and unordered list regressions prove that provisional reuse occurs before finish and still produces the complete canonical document. Render-cache integration retains superseded chunk retirement, including the finish-alias path, with location, caret and ordinal isolation in its cache lanes.

The native regression streams a 120-paragraph prefix with links, images, custom tags and list context, then finalizes twelve different tails. It checks exact parsed-byte growth against remaining tail bytes, shared prefix storage, final canonical model and math parity, settled state and repeat-finish work counters. It also exercises dependency and global formula-cap fallbacks.

### Preserve unchanged render payload through finish

`RenderReuse.h` proves exact payload equivalence, including resolved link/image/tag records, formula layouts and errors, highlighting/fence state, and structural segment boundaries. A finish-only `Document::renderReuseFrom` ID lets Android and iOS share the preceding immutable segments while building a fresh document wrapper and applying final settlement/control state. It does not change block identity semantics, retain a document chain or use content hashes.

The parent lookup includes the rendering options, caret reservation, theme and scale. Missing/evicted parents fall back normally. Android's incremental paragraph policy still performs canonical final layout. Both platforms retain the payload's memory charge when the parent wrapper retires; Android tests explicitly reject the formerly possible uncharged final tail.

The same resolved-render proof now guards existing block-identity transfer and committed-prefix confirmation. Equal local reference indexes or TeX cannot keep a native cache identity after a link destination or formula answer changes. A full-mode regression proves that crossing the global formula cap invalidates earlier identities rather than retaining a cached pre-cap formula/error.

The production Android factory test proves unchanged identity-zero tails need zero additional JNI block reads or segment builds at finish. New IDs and settled metadata remain correct. Caret, scale, theme, link variants, paragraph policy and a missing parent defeat reuse. Committed segment keys also now include kind, location and ordinals explicitly; the iOS committed key includes caret reservation.

### Realize mixed Android content only near the viewport

`DownstreamMarkdownView` now defers formatted text with links, tags, tasks and inline math, plus unhosted code, tables and display math. It uses the canonical measured frames rather than estimating placeholder geometry. Code/table/math reader offsets and pin state live independently of mounted views, survive deferred remount and reset with a new source. Recycling pools remain bounded. Stable slot replacement removes document-sized array shifting on each scroll.

The production-method harness realizes 10,000 mixed slots with at most 13 mounted views and fewer than 120 segment reads per scroll. It verifies visible geometry/content, horizontal and vertical offsets, RTL table leading distance, reader unpin, latest snapshots, source reset, zero slot-array shifts and bounded recycling. Selection and accessibility retain eager native reading order. Spoilers, details, custom hosts, carets and asynchronous images keep their stateful fallback.

This viewport extension targets the Android phone workload. The existing iOS viewport policy remains unchanged.

### Bound reveal backlog

`RevealTimeline` no longer applies a 4 ms minimum stagger beyond its available budget. New unit starts and the cursor stay within `now + maxBacklogMs`. Tightening options compresses future carried starts while preserving already begun fades and opaque text. Zero backlog, stagger or duration removes future scheduling debt.

Tests first reproduced the old 600-unit overrun, then prove bounds for 600-word and 1,200-character bursts, multiple blocks/passes, partly settled cascades, tightened options and platform animation scaling. Default reveal completion is bounded by 320 ms backlog plus 150 ms fade duration. `maxBacklogMs: 0` permits a 150 ms fade tail. Animation-off historical results cannot improve from this fix alone.

### Measure queueing and release old owners away from the frame callback

The worker bound remains one to four threads. Optional native diagnostics now distinguish queue wait, worker pass, parser work, preparation, staged publication wait, Fabric measurement and native application without mixing clock domains. Queue sequence advancement and same-sequence final events are covered by tests. Increasing threads before a device measurement could worsen UI contention.

Native ticks swap superseded snapshot/source/tag owners out and release them on the worker pool, rather than destroying those potentially large owners inline. `retire_start/end` measure that phase, including standalone retirement jobs. This measures session ownership only; references still held by platform views can release later. Inline retirement overlaps worker-pass elapsed time, so phase totals must not be summed blindly.

`--finish-delay-ms` schedules each replay thread's finish independently at 0, 1,000 or 3,000 ms after its actual last delivery, without delaying other input. `--diagnostic-trace` supports all concurrent Downstream sessions, keeps the latest 40,000 events so huge finalization tails survive, and reports truncation/unmatched phases. Resume and APK checks reject mixed or ignored options.

## Historical slow-versus-fast comparison

The original data is in the original checkout's `benchmarks/phone/results/bench-20261013T102344`. Its build records revision `5264202a673f31c8b6056e1f90e2be3283bd2965` with local changes. A dirty build record cannot establish that every audited file exactly matches the APK. Benchmark files and REPORT.md were copied unchanged into this worktree before adding diagnostics; the original checkout remains untouched.

The benchmark used `animated={false}`. Caret defaults to none. Reveal animation therefore does not explain these particular tails.

The estimate below is `app.deliveryDurationMs - (max(live.lastDeltaMs) - min(live.firstDeltaMs))`. It compares the phone's delivery-loop duration with the host's text-delta span. It includes terminal waiting, transport/scheduling differences, and usage/finish-reason waits. It is not an exact phone finish timestamp or native finalization duration.

| Workload/run         | Reported settle, ms | Completion-gap estimate, ms | UI CPU, s | Other CPU, s | App frames | React commits |
| -------------------- | ------------------: | --------------------------: | --------: | -----------: | ---------: | ------------: |
| matrix 0040, fast    |                17.2 |                        97.9 |      3.03 |         2.79 |        239 |           461 |
| matrix 0079          |               978.7 |                       926.1 |      3.49 |         3.03 |        260 |           437 |
| matrix 0101          |              1693.0 |                      1635.4 |      3.26 |         2.66 |        241 |           396 |
| matrix 0117          |              1450.6 |                      1278.9 |      3.26 |         3.00 |        235 |           663 |
| matrix 0145          |              1049.0 |                      1015.8 |      3.04 |         2.73 |        224 |           407 |
| matrix 0168, fast    |                44.4 |                       625.7 |      3.06 |         2.48 |        162 |           365 |
| matrix 0199, fast    |                30.1 |                       103.1 |      2.40 |         1.90 |        206 |           385 |
| matrix 0227          |               883.6 |                       873.7 |      3.76 |         3.52 |        267 |           584 |
| matrix 0234, fast    |                20.9 |                      1100.4 |      2.76 |         2.15 |        159 |           330 |
| matrix 0262          |               573.4 |                       539.5 |      3.77 |         3.56 |        252 |           634 |
| huge concurrent 0051 |              2937.3 |                      2882.5 |     20.26 |        33.34 |       1244 |          1945 |

Rows come from `runs/highlighted-<run>-downstream-default-ai-<workload>.json`. CPU, frame, and commit totals cover each entire run, not just its tail. For matrix 0101, settle is at JSON line 84, app duration at 103, and host delta boundaries at 120–121. For huge 0051 those fields are at 82, 101, and 118–215. Fast 0040 records them at 79, 98, and 115–116.

All six slow matrix samples have completion-gap estimates near their settle times. Huge 0051 differs by about 55 ms. That supports a quiet terminal wait followed by a final update as the leading hypothesis. It does not prove that native finalization took 55 ms.

Counterexamples limit the conclusion. Matrix 0234 waited an estimated 1100 ms but settled in 21 ms; 0168 waited about 626 ms but settled in 44 ms. Delayed completion does not always produce another app frame.

## What is verified about the tail

The live relay sends `finished` after `result.textStream` closes, then waits for usage and finish reason before its aggregate `done` marker. The phone calls the controller's `finish()` on the thread's finished message. Its old `deliveryDurationMs` extends to aggregate done, so old durations conflate several boundaries.

The audited historical `IncrementalParser::parse(true)` unconditionally used `fullParse(ParseFallback::Finished, false, true)`. It reparsed canonical source, completed highlighting, laid out math, validated the committed prefix, and produced settled state. Completed code tokens, math layouts, and confirmed block storage could be reused. This proved repeated whole-source work, not seconds of native CPU. This branch now uses the safe tail path and exact render reuse described above, keeping canonical fallback when needed.

Only the fast matrix 0040 trace survived. The harness deleted every slow matrix trace and huge 0051's trace. Their JSON records say `trace: null`, and old records omit exact per-thread finish receipt and individual delivery boundaries. The retained fast trace has no app frames after its delivery-end marker. There is no corresponding slow trace to identify which function was still drawing.

Code inspection did not establish an autonomous redraw loop. The viewport pre-draw handler exits when viewport/accessibility values are unchanged. Table clipping invalidates only on rectangle changes. Host requestLayout coalesces its posted reconciliation. Math preparation is synchronous in parser work. Queued preparation or mounts remain possible causes that require retained traces.

## Benchmarks to run

Keep the unanimated highlighted/plain profiles as the comparison baseline. Add animated profiles separately. Downstream, RN Streamdown, and Enriched support reveal; the JS renderers do not. Compare animated results only among supported libraries, and label each library's default effect because the effects and progressive rendering behavior differ.

Use captured input and identical delta timing for before/after comparisons. Fresh model output remains useful for coverage but cannot isolate a scheduling change. Measure feature matrix and huge concurrent five with immediate finish, and with controlled one-second and three-second terminal delays. A terminal delay followed by one isolated final frame must be distinguishable from continued renderer activity.

Persist exact phone last-text, per-thread finish, and aggregate-done boundaries. Report original text-to-last-frame settle alongside finish-to-last-frame drain, CPU/frame work before and after finish, and window censoring. Preserve slow traces automatically. The roughly 150 ms target applies to unanimated renderer drain; reveal duration plus stagger intentionally adds animation time.

No Android phone was connected during implementation. Historical rows are baseline measurements; they do not demonstrate a performance gain from this branch. Android/JNI and Fabric syntax checks cover their type boundaries. Xcode was available under `/Applications/Xcode.app`; selecting its developer directory for test commands enabled the real iOS simulator checks below.

Run a paired animation comparison from this worktree:

```sh
node benchmarks/phone/bench.mjs all --replay <recording-dir> --output <new-result-dir> \
  --profiles highlighted,highlighted-animated \
  --candidates downstream-default,streamdown,enriched \
  --workloads ai-feature-matrix,ai-huge-concurrent-5 --keep-traces
node benchmarks/phone/tail-compare.mjs <new-result-dir>
```

Use `--settle-ms 5000` for both profiles if the common default 2000 ms window is censored. The harness rejects saved APKs that do not honor the requested animation or observation budget. An animated profile is additional evidence, not a replacement for the unanimated report.

Repeat the captured, animation-off Downstream workload with a controlled terminal wait in separate result directories:

```sh
node benchmarks/phone/bench.mjs all --replay <recording-dir> --output <delay-result-dir> \
  --profiles highlighted --candidates downstream-default \
  --workloads ai-feature-matrix,ai-huge-concurrent-5 \
  --finish-delay-ms 1000 --diagnostic-trace --keep-traces
```

Use delays 0, 1000 and 3000 with the same recording. Diagnostic tracing is opt-in and adds overhead, so run clean performance comparisons separately from phase-attribution captures. Compare native `afterFinish` summaries and exact phone boundaries, rather than inferring native work from provider completion delay.

## Verification

- All 29 JS suites passed, covering 761 tests, including shared publications, coherent multi-stream reads, cumulative acknowledgements, reentrant reset/dispose and same-sequence finish events.
- Native engine tests passed with 367 tests and one optional RaTeX host test skipped. Fake-frame tests cover bursts, five simultaneous sessions, worker completion between ticks, adaptive pressure/recovery, finish, reset, disposal, inactive progress and worker-only retirement. Canonical finish, render equivalence and bounded reveal regressions are included.
- Android Kotlin/JNI type checks, both Fabric platform syntax checks and Android host rendering checks passed.
- The isolated Android text harness also passed on the API 36 arm64 emulator, using production text/layout and JNI page code. It completed 319 seam checks, 1,826 prefix checks, 76 view checks, 512 interleaved-stream checks and 45 layout benchmark samples. Pixel, selection, accessibility and page-retention assertions passed. This harness does not execute the React Native frame clock or mixed viewport lifecycle.
- Native iOS TextKit, reveal and math harnesses passed on the iOS 27 simulator. New coverage executes the actual CADisplayLink clock, next-tick reentrancy, inactive notifications/resume, final controls, render reuse, independent geometry and option/caret mismatches. It also verifies missing/retired parents fall back.
- Benchmark tests passed with 26 tests, plus the existing statistics selftest and app TypeScript check. They cover exact boundaries, independently delayed concurrent finish, queue/publication/retirement phases, stale APKs, resume consistency and incomplete/out-of-window trace retention.
- Source review identified and corrected missing mobile inactive progress, Android strict-mode clock registration, incomplete-frame censoring, alias memory accounting and stale formula identities. Physical-phone gains, Fabric commit frequency and Android lifecycle/scroll execution remain device checks. The UIKit lifecycle harness drives notifications rather than a real operating-system suspension.

## Remaining device validation

The requested implementation work is complete. A connected Android phone is still needed to establish CPU/frame improvements and the roughly 150 ms unanimated drain target on identical matrix and huge-concurrent replay. The new measurements cannot retroactively recover deleted slow traces. Simulator UIKit checks establish implementation behavior, not physical-device performance or a complete React Native application run.

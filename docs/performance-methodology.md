# Phase 5 performance method

The example app splits the work across three measurement screens. Bench measures Downstream
alone: select full or incremental tail parsing, then replay one source across
fixed chunk schedules. Testing replays the same source through React Native
Streamdown and then Downstream, and shows the comparison once both runs finish.
The Testing screen reuses the same seeded schedules as Bench, so a workload in
one screen means the same workload in the other. Schedules and summaries can be
checked without a device; performance claims require Release builds on physical
iOS and Android devices.

## Testing versus Bench

Use Testing to answer "is Downstream faster than Streamdown on this device?".
It runs one candidate at a time over an identical schedule, keeps both panes
mounted in the same place in the view tree, and reports time to first frame,
time to the final frame, the longest frame gap, the worst delivery delay, and
whether both candidates received the same source. Streamdown runs first so
Downstream never inherits a warm renderer. Those numbers come from JavaScript
animation frame callbacks and bound JS-thread work only; they are not evidence
that a glyph reached the display.

Use Bench to answer "which Downstream parse mode should ship?". It keeps the
full trace export, the native session markers, and the JSON the summarizer reads.

## Build and run

1. Install with `pnpm install`. The example pins React Native 0.83.2 and Expo 55.
   Generate native projects with
   `pnpm --filter react-native-downstream-example exec expo prebuild --clean`.
2. Build a Release app on each physical device. Use
   `pnpm --filter react-native-downstream-example exec expo run:ios --configuration Release --device`
   and
   `pnpm --filter react-native-downstream-example exec expo run:android --variant release --device`.
   Record the exact commands, Git revision, package versions, and any signing or
   variant change. A successful dependency install or Metro export alone does not
   prove that the baseline runs on either device.
3. On Bench, run each parse mode with the same fixture, size, shape, seeded
   schedule, and 1, 4, then 8 streams. Cover one-character, 1–20-character,
   bursty, and manifest chunking; 10, 50, and 100 KiB sources; prose, long open
   code, and many completed blocks. Exercise pause/resume, reset during pending
   work, finish, scrolling, rotation, font scale, and theme changes. Compare the
   final source to the fixture and inspect screen recordings for flashes, missing
   text, or stale updates.
4. On Testing, run each workload for both candidates and capture at least two
   independent pairs per workload and device, including cold and warm runs. Keep
   all samples in order, including pairs where the run failed.
5. Alternate candidate order, parse mode, and tracing on/off. Keep native traces
   and screen recordings for each Bench run next to its JSON in
   `benchmarks/results/`. The Testing screen has no JSON export; transcribe its
   table into the run notes.

## Metadata and clocks

Record device model and ID, OS, refresh rate, thermal state, battery mode, viewport,
font scale, build revision, RN/Expo and renderer versions, fixture hash, seed,
schedule, stream count, renderer, parse mode, sample order, cold/warm state, and
trace toggle. Bench writes one JSON export per run; keep native trace and video
paths next to it. Streamdown has no Downstream native markers, so compare its
CPU, memory, allocations, and UI frames with system tools rather than implying
matching parser timings. The Testing screen records the fixture, seed, schedule,
and parse mode on screen but exports nothing, so copy its table into the same
notes.

The app records JavaScript receipt, delivery, append, and frame callbacks. The
Downstream opt-in trace records native append, parse, measurement, and apply
markers with session ID, generation, and sequence. Join records by those IDs.
JavaScript `performance.now()` and C++ monotonic timestamps have separate origins;
only subtract values within the same clock domain unless a measured clock offset
is available. A JS animation-frame callback is a frame estimate, not proof that
a glyph reached the display. Verify arrival-to-paint latency with native frame
traces and video. Capture JS and UI frame durations, missed frames during scroll,
CPU, resident memory, allocations/GC, and energy when the platform tools expose
them reliably. Mark any unavailable measurement as unavailable.

## Analysis and decision

Run `node benchmarks/summarize.mjs <export-1.json> <export-2.json> ...` on Bench
exports. Keep one device, build revision, and cold/warm cohort together. The tool
prints per-run p50/p95/p99 and repeat-run variation for sampled JS estimates
and paired native queue, parse, measurement, and apply intervals. It counts
superseded frame samples and compares trace-on/off run duration. Read
`queueDelayMsUnmatchedAppends` and `frameUnavailable` next to their intervals:
appends still queued at export and snapshots superseded before a frame each lose
a sample, so a healthy run reports few of both. The native worker coalesces every
append that lands while it is parsing, so `queueDelayMs` pairs each append with
the next parse in the same session generation rather than only the append that
shares a snapshot with it; the intervals after `parseMs` stay snapshot-scoped.
The native apply-to-frame interval still ends at a frame callback estimate. Add
native UI frame, allocation, memory, and CPU results from Instruments or Perfetto.
Report variability, not a best run. Keep before/after traces if tuning changes
batching, parsing, measurement, rich content, or list behavior.

Use the Testing comparison to say which renderer met those budgets on a given
device, and the Bench traces to say why. A single Testing pair is one sample: take
at least two per workload before drawing a conclusion, and record a loss for
Downstream as plainly as a win.

The proposed review budgets are p95 chunk arrival to visible glyph below 50 ms,
p95 Markdown UI-thread work below 4 ms on a 60 Hz target, and p95 JS append work
below 2 ms. Apply them only to measured physical-device data. If full parse meets
the budgets at 100 KiB and concurrent streams, prefer it and remove the tail
unless a harder workload proves a repeatable end-to-end gain. Choose incremental
tail only when it improves end-to-end results without correctness regressions.
If Downstream misses a budget that the baseline meets, profile the dominant cost
before changing the renderer. Record any unmet budget as a known limit.

Streams now default to incremental parsing based on host engine benchmarks and
prefix parity tests. This is a runtime setting, not evidence that these device
budgets have been met. No physical-device result has been recorded yet.

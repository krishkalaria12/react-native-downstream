# Phase 5 benchmark results

No physical-device release traces have been collected yet. The benchmark screen and
Node summary script can be exercised with simulated playback, but simulated timestamps
are not performance results. Streams now default to `incremental`, which reuses
completed blocks and updates the tail. Host engine benchmarks and prefix parity
tests support that choice; physical-device comparisons are still required to
measure layout, paint, memory, and responsiveness.

Save raw exports under `<date>/<device>/<sample-order>-<renderer>.json`. Keep failed
and cancelled runs, native traces, screen recordings, build revision, fixture hash,
and environment notes before drawing a mode decision.

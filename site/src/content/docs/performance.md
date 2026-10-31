---
title: Performance
description: What the native path measures on a real phone, why streaming reuses completed blocks, and how to read the benchmark numbers.
group: Reference
order: 10
---

Every number on this site comes from a measured run on a physical Android phone, not a simulator or a JavaScript microbenchmark. The full tables, confidence intervals, and relative multipliers are on the [benchmarks page](/benchmarks).

## The setup

| | |
| --- | --- |
| Device | OnePlus Nord CE 3 (CPH2569), Android 15, 120 Hz, arm64 |
| Build | Hermes release, New Architecture |
| Model | `gpt-6-luna`, reasoning medium, streamed through the Vercel AI SDK |
| Transport | Each text delta sent over USB the moment it arrived |
| Runs | 198 measured, 36 warmups excluded |
| Statistics | Median of 5 cold-process runs, 95% bootstrap confidence interval |

Libraries compared: Downstream, RN Streamdown, Enriched Markdown, Markdown Display, Marked, and Markdown Renderer. Each library renders the same replies with its own defaults.

## One AI reply

Prose, a table, and a TypeScript block streaming into one view:

| Library | App CPU | UI thread CPU | Peak memory | Janky frames | Frame p99 |
| --- | ---: | ---: | ---: | ---: | ---: |
| **Downstream** | **6.56 s** | **1.65 s** | **10.6 MiB** | 7.3% | **16.6 ms** |
| RN Streamdown | 11.83 s | 2.74 s | 29.0 MiB | 2.1% | 11.3 ms |
| Enriched Markdown | 9.60 s | 2.84 s | 16.6 MiB | 3.4% | 11.4 ms |
| Markdown Display | 11.10 s | 4.14 s | 37.4 MiB | 53.1% | 40.9 ms |
| Marked | 13.81 s | 6.29 s | 53.5 MiB | 47.6% | 79.3 ms |
| Markdown Renderer | 13.32 s | 6.24 s | 45.6 MiB | 58.9% | 79.1 ms |

Downstream uses roughly 1.5× less app CPU and 1.6× less peak memory than the next closest library. The UI thread stays comparatively free because parsing, highlighting, and text preparation run on native worker threads.

<div class="callout callout-blue">
  <strong>Lower is better everywhere.</strong>
  <p>App CPU is total process CPU during the stream. UI thread CPU is main-thread time. Peak memory is the increase over the app's baseline. A janky frame is one where the app missed its deadline.</p>
</div>

## Five replies at once

Concurrency is where the gap widens, because every library is asked to keep five documents live at the same time:

| Library | App CPU | Peak memory | Janky frames | Frame p99 |
| --- | ---: | ---: | ---: | ---: |
| **Downstream** | **23.33 s** | **23.4 MiB** | **2.3%** | **14.0 ms** |
| RN Streamdown | 48.86 s | 72.4 MiB | 2.2% | 14.1 ms |
| Enriched Markdown | 44.76 s | 37.7 MiB | 4.4% | 16.7 ms |
| Markdown Display | 53.17 s | 84.5 MiB | 64.1% | 159.1 ms |
| Marked | 62.87 s | 100.6 MiB | 76.5% | 196.3 ms |
| Markdown Renderer | 63.43 s | 167.5 MiB | 54.0% | 1002.3 ms |

The JavaScript renderers fall behind badly under concurrency. Their p99 frame times climb into hundreds of milliseconds while Downstream stays near one frame.

## Why the native path is cheaper

Downstream does not parse Markdown in JavaScript and rebuild a React tree for every chunk. The pipeline is:

1. A JavaScript chunk reaches the controller.
2. Native code parses and prepares an immutable snapshot off the main thread.
3. Completed blocks keep their identity and storage.
4. Only the unstable tail is re-prepared.
5. One snapshot is published per session per display tick.

The same completed prefix is reused across appends, so a 60-point reply does not become 60 full parses. At `finish()`, the parser finalizes only the uncommitted tail when the prefix is already canonical, and the renderer reuses the previous payload where it is provably unchanged.

## Frame coalescing

Updates from every stream share one animation frame and one React Native batch. Native publication is capped at one snapshot per session per display tick.

The cadence starts at one tick and backs off to at most four ticks. Preparation above 70% of the frame interval, or a late frame callback, increases the stride; eight healthy publications reduce it by one step. `finish()` and `reset()` bypass the cooldown. At 60 Hz this allows deliberate batching down to roughly 15 updates per second under pressure.

Quiet sessions request no frames. A paused host still flushes explicitly. This is why CPU scales with text rather than with token count.

## Reading the numbers

- **CPU per KiB** normalizes CPU by the text each run actually streamed, since live replies differ in length.
- **Settle after last chunk** is how long the final frame took after the last text arrived. Values near zero mean the view was already settled when the stream ended.
- **Huge workloads** run once per library, so their rows have no confidence interval or significance test. Treat them as directional.
- **Relative multipliers** compare each library's median to Downstream's. An asterisk marks a Mann-Whitney `p < 0.05` difference.

Some runs have unfinished frames at the observation boundary. Those settle values cannot prove completion, which is why the report publishes the raw traces and confidence intervals instead of a single headline number.

## Reproduce it

The harness and workloads live in `benchmarks/phone/` in the repository:

```sh
node benchmarks/phone/bench.mjs all --replay <recording-dir> --output <result-dir> \
  --profiles highlighted --candidates downstream-default,streamdown,enriched \
  --workloads ai-single,ai-feature-matrix --keep-traces
```

See [Benchmarks](/benchmarks) for every workload, including code-heavy, feature-matrix, and huge concurrent runs.

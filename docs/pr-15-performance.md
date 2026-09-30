# PR 15 performance analysis

## Findings and changes

The public stream factory selected full parsing on every update. That reparsed
the complete source and highlighted every earlier code block again. The native
incremental parser already had a conservative boundary proof, shared committed
blocks, a mutable tail, and an open-fence append path. Streams now select that
parser by default. Unproven boundaries and oversized tails still use a full
parse. Replacement and completion also take a full parse; `finish()` removes
provisional styling and parses the canonical source.

The highlighter resumed lexing near the last completed line, but
`CodeDocument::append` rebuilt the newline index over the complete code block
on every chunk. Appends now scan only new bytes and preserve existing line
offsets, styles, and lexer states. This removes repeated indexing of the prefix.
The maximum admitted highlighted block remains 64 KiB.

Token construction scanned the generated language style table for every byte.
Each lexer now prepares a 64-entry lookup table once, matching the existing
style mask. Token construction uses a direct lookup. Regression tests compare
token ranges with whole tokenization.

A UTF-16 prefix ending in a high surrogate cannot reuse its UTF-8 cache when
the next append supplies the low surrogate. Previously this left two replacement
characters in the cache. That boundary now rebuilds the cache, with a regression
that checks both UTF-8 content and tokens against a whole pass.

## Streamdown reference

The research used Vercel Streamdown at
[`0c2efd78`](https://github.com/vercel/streamdown/tree/0c2efd78a3b09e9d7e135b2cf8b5a0544202c8d9).
Its transferable mechanisms are:

- [Stable block reuse and incremental tail parsing](https://github.com/vercel/streamdown/blob/0c2efd78a3b09e9d7e135b2cf8b5a0544202c8d9/packages/streamdown/lib/parse-blocks.tsx#L240).
- [Memoized block rendering](https://github.com/vercel/streamdown/blob/0c2efd78a3b09e9d7e135b2cf8b5a0544202c8d9/packages/streamdown/index.tsx#L480).
- [Highlighting that retains grammar state and completed token rows](https://github.com/vercel/streamdown/blob/0c2efd78a3b09e9d7e135b2cf8b5a0544202c8d9/packages/streamdown-code/index.ts#L109).

Downstream's equivalents are immutable committed blocks, cached native segments,
and a per-block Lexilla resume state. Enabling incremental mode makes those
existing mechanisms available to normal streams. Native text preparation and
measurement already run on the shared worker pool; JavaScript appends and
snapshot notifications already coalesce by animation frame. Adding another
scheduler or a JavaScript syntax highlighter would duplicate those mechanisms.

## Host engine measurements

Apple M5, Apple Clang 21, `-O3 -DNDEBUG`, one warmup followed by the median of
seven samples. Before uses the original PR highlighter; after uses the new line
index and style lookup. Each pair uses the same benchmark source and compiler.
These are elapsed engine times, excluding React Native layout and paint.

| Workload | Before | After | Improvement |
| --- | ---: | ---: | ---: |
| Line indexing, twenty 60 KiB streams in 16-byte chunks | 1097.983 ms | 1.609 ms | 99.9% less time |
| Whole highlighting, twenty 60 KiB TypeScript blocks | 23.001 ms | 17.289 ms | 24.8% less time |
| Highlighting one 60 KiB TypeScript stream in 16-unit chunks | 63.277 ms | 11.000 ms | 5.8x faster |
| Incremental parser, one 60 KiB open fence in 16-byte chunks | 79.761 ms | 23.593 ms | 3.4x faster |

The indexing row isolates the removed scan. It is not a whole-renderer speedup.
The open-fence row measures streaming updates before completion.

The default-mode comparison uses the optimized engine, approximately 16 KiB
documents, 32-byte chunks, one parse per chunk, and a final canonical parse:

| Workload | Full mode | Incremental mode | Improvement |
| --- | ---: | ---: | ---: |
| Headings, prose, links, and TypeScript fences | 69.690 ms | 3.323 ms | 21.0x faster |
| Repeated TypeScript fences | 64.345 ms | 3.597 ms | 17.9x faster |

Run `pnpm bench:native` to compile a separate release-mode engine and repeat the
workloads, including 128-byte chunk variants. Compilation is outside the timed
regions. Counts accompany each result so omitted work can be detected. Repeated
runs vary; compare on the same machine and compiler with other builds stopped.

## CI failure and build cost

The failing Ubuntu job could not compile the highlighter because
`CatalogueModules.h` calls `strcmp` without its declaration. `<cstring>` is now
included before that vendored header, preserving the pinned vendor contents.

The standalone native suite compiled shared engine sources for each executable
and launched every lexer compiler at once. It now compiles 53 unique translation
units once, instead of 97 compilations, and limits compiler concurrency to four
or the host CPU count, whichever is smaller.

Parallel Vitest workers also independently built the native engine. Global setup
now prepares the objects and three binaries once before workers start. Shared
objects are reused across binaries. Atomic publishing protects concurrent test
runs. Cache keys include source and header contents, compiler identity, flags,
and relevant compiler environment, avoiding stale objects after header changes.

The same focused 59-test suite, on a fresh temporary cache, took 24.37 seconds
before and 8.46 seconds after. CPU time fell from 50.31 to 20.03 seconds. A warm
run took 2.18 seconds. These are local timings, not GitHub runner measurements.

The CI test job now runs native tests first, so compiler failures stop once
before unit-test workers begin. Both harnesses support ccache, with matching
compile flags. The workflow persists a bounded compiler cache and reports hits.
Existing required job names and test coverage are preserved.

With the same stable C++ sources and cold builds, the standalone native suite
took 22.23 seconds using the original harness and 14.09 seconds after, a 36.6%
reduction. CPU time fell from 47.57 to 26.00 seconds. GCC and Clang both passed
the native suite. The GitHub cache hit rate still needs a workflow run.

The complete unit gate initially exposed a load-sensitive test oracle. Its raw
parse used one budgeted highlight pass, while production settles complete code
blocks. A truncated oracle was reported as an identity failure. Both test
drivers now settle their tokens; an 80-line zero-budget regression proves the
old mismatch and the corrected result without depending on host speed.

## Verification

- `pnpm test`: 220 tests pass.
- `pnpm test:native`: 237 tests pass with Clang and GCC 16.
- `pnpm lint`, `pnpm typecheck`, and `pnpm prepare` pass.
- The Expo example exports for Web successfully.
- `actionlint`, `git diff --check`, native benchmark execution, and focused
  Vitest adapter benchmarks pass.
- Isolated build checks confirm that header, compiler, and flag changes
  invalidate the cache, and compiler failures stop new work and drain active
  tasks before reporting the original error.

## Benchmark corrections and remaining measurements

The React Native Streamdown comparison previously batched all Downstream chunks
into one final parse while rendering every Streamdown prefix. Both now process
each update. React roots are unmounted after each iteration, and document labels
show actual sizes. Whole-source replacement has its own reset workload.

The Node comparison still includes subprocess transport and replays earlier
session operations in its test adapter. Use it to inspect that harness, not to
rank native application latency. `pnpm bench:native` keeps a real parser in the
process for its entire stream.

Long unbroken paragraphs, unresolved references, container-heavy input, and
unproven boundaries can still require full parsing. Every finished stream pays
one canonical full parse. Host timings do not establish UI latency, memory use,
or frame budgets on a phone. Physical-device release traces remain necessary;
see [the measurement method](performance-methodology.md).

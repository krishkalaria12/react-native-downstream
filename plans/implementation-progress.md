# API and plugins implementation

Work follows [api-and-plugins.html](api-and-plugins.html) on `feat/api-and-plugins`.
The plan was committed before implementation in `b8872d2`.

## Phase 1

Implemented growing `markdown` strings with owned incremental sessions, enabled
when `isAnimating` first becomes true. Settled history creates no sessions.
Growing sources append only their suffix; edits and growth after finish reset
the generation. Preview defaults on and is removed at finish. UTF-8 validation
counts appended bytes and holds back a trailing high surrogate while streaming.

The owned path uses the existing controller's batching, subscriptions, fence and
tag slots, and unmount cleanup. Large controlled updates are split to fit the
native chunk limit, while keeping the 1 MiB source limit. The public controller's
64 KiB chunk and 256 KiB queue limits are unchanged. Parse-option changes rebuild
the owned session with its accepted source. Rejected updates preserve that source,
including during activation and session rebuilds.

Regression coverage compares the owned path with a controller at every prefix
and finish through the real C++ `IncrementalParser`. It also covers preview
policies, Unicode boundaries, limits, renderer slots, option changes, and
StrictMode cleanup. The README leads streaming with a `useChat` string example.

Validation: `pnpm test --maxWorkers=4` passed all 775 tests; `pnpm test:native`
passed all 467 tests. `pnpm typecheck`, `pnpm exec bob build`, and lint and
formatting checks for changed files passed. A default-worker full test rerun
timed out in the existing inline-HTML corpus test; the four-worker run passed
without changing the test or its timeout.

Still pending: the plan's long-response phone benchmark comparing controlled
strings with controllers on both platforms. An Android emulator is connected;
no physical phone is attached, and `xcrun simctl` is unavailable. No performance
parity claim has been established.

## Phase 2

Implemented `Downstream` with string children, optional `markdown` input, and
external controllers. `DownstreamMarkdown` remains a deprecated alias. Added
`DownstreamProvider` defaults, provider-first theme and element styles, grouped
code block, table, image, and link options, and fence/tag component maps.
Former flat props and renderer arrays retain compatibility with development
warnings once per alias.

Public controllers carry text and optional error handling. Their bound view
owns parse and preview configuration, and a live option change rebuilds the
accepted source under the new policy. Controllers can accumulate text before
mount and while their rows are unmounted. The README documents the new API and
migration, and example screens use the new component and grouped options.
Benchmark views retain their previous preview and math policies explicitly;
recorded benchmark results have not been changed.

Validation: `pnpm test --maxWorkers=4` passed all 814 tests, and
`pnpm test:native` passed all 467 tests. Root and example typechecks,
`pnpm exec bob build`, lint, formatting, and `git diff --check` passed.
Review found no remaining issues. Real-engine regressions cover buffering before
mount, live policy changes with pending text, finished responses, remounts,
allowed tags, literal tag styles, stale publications, and provider styles.

## Next

Phase 3 remains unimplemented: explicit plugin objects and math activation,
the native highlighter package extraction, plugin migration guidance, and
binary-size measurements. The phase 1 phone performance gate also remains open.

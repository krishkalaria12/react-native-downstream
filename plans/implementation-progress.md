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

## Next

Phase 2 remains unimplemented: `Downstream`, children input, provider defaults,
grouped props, component maps, and view-owned options for external controllers.

Phase 3 remains unimplemented: explicit plugin objects and math activation,
the native highlighter package extraction, and the 0.2 migration documentation.

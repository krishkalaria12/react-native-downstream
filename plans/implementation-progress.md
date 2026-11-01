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
strings with controllers on both platforms. No physical phone is attached.
During phase 3, selecting the installed Xcode with `DEVELOPER_DIR` restored
SDK and simulator access; the earlier `simctl` failure came from the active
command-line-tools selection. No phone performance parity claim has been established.

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

## Phase 3

Implemented feature-plugin resolution, the math factory, and the optional code
package. The public `math` and `syntaxHighlighting` props
are replaced by `math(options)` and `code()` in the selected plugin array.
Plugin defaults apply before app component maps and provider/view styles.
An omitted array inherits the provider selection; an explicit array replaces it.

The README and examples now use explicit plugins. The example app supplies
`code()` and `math()` through its provider, while benchmark views select the
same code/math features their profiles requested before the migration.
Recorded benchmark results have not been changed. The math package now has its
own install and usage guide.

The core retains its small incremental policy and token adapters. The optional
code package owns the lexer/runtime and installs a versioned C function table.
Views without `code()` skip lexer work, even when the package is linked for
another view. Core-only tests preserve plain-code copy and line-number layout.
Development builds warn once when math is installed without a selected plugin.
Explicit view tag allowlists replace inherited ones, including an empty list.

Validation: 832 JavaScript tests and 467 native tests passed, along with the
core-only smoke test, Fabric checks, root/example typechecks, package build,
lint, and formatting for changed files. iOS simulator and Android arm64 native
builds passed both with and without the code package. CI now builds both
configurations. Package dry runs confirm the core omits the lexer/runtime and
the code package contains them and their licenses.

Repository-wide formatting now passes after formatting the plan HTML and site stylesheet.

Matched unstripped engine archive measurements are recorded in the
[size report](../benchmarks/phone/CODE_PACKAGE_SIZE.md). Core archive size fell
by 20.26% on iOS arm64 and 22.37% on Android arm64-v8a. These are archive sizes,
not APK/IPA download sizes or loaded-memory measurements.

The phase 1 phone performance gate remains open. No packages were published.

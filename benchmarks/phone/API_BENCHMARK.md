# API and plugin phone benchmark

Use `bench-api.mjs` for the public string/controller APIs and explicit plugin configuration added by the API and plugins plan. `bench.mjs` remains the entrypoint for the historical six-library report. The new suite reuses its release build, replay transport, cold-process runs, thermal checks, Perfetto collection and statistics.

This runner targets a **physical arm64 Android phone**. It does not run the iOS simulator benchmark. Build from the workspace with the example's dependencies installed, Android SDK/NDK and Java configured, `adb` on your path, and USB debugging enabled. The pinned Perfetto trace processor downloads on first run; its current binary targets an Apple Silicon Mac. No model API key is needed for replay.

## Start here

Run these commands from the repository root:

```sh
# Generate deterministic fixtures; the destination must not already exist.
node benchmarks/phone/bench-api.mjs fixtures benchmarks/phone/results/api-fixtures

# Inspect the candidate/profile matrix and seeded order without a phone or build.
node benchmarks/phone/bench-api.mjs plan --profiles core,all --workloads ai-single

# Build fresh release APKs, install, run and write results.
node benchmarks/phone/bench-api.mjs all \
  --replay benchmarks/phone/results/api-fixtures \
  --output benchmarks/phone/results/api-smoke \
  --profiles core,all --workloads ai-single --repeats 2
```

The smoke run performs 12 cold-process runs: two APIs × two profiles × (one warmup + two measurements). It checks the harness; use the default five measured repeats for a comparison. A new result directory is required for each session. The runner temporarily adjusts the example's Enriched native build settings and restores its `package.json` after building.

For the full default matrix:

```sh
node benchmarks/phone/bench-api.mjs all \
  --replay benchmarks/phone/results/api-fixtures \
  --output benchmarks/phone/results/api-full --keep-traces
```

Defaults are two Downstream APIs, four plugin profiles, `ai-single`, `ai-feature-matrix`, `ai-concurrent-5`, and `ai-huge-single`. Regular workloads have one warmup and five measured repeats. Huge workloads have one measured run and no warmup, following the original scheduler. The default matrix contains 152 runs and builds four APKs; start with the smaller command above.

## What is measured

| Candidate               | Public API used                                              | Updates                                                                              |
| ----------------------- | ------------------------------------------------------------ | ------------------------------------------------------------------------------------ |
| `downstream-string`     | `<Downstream isAnimating={active}>{text}</Downstream>`       | Cumulative strings through React; each row becomes inactive at its own finish marker |
| `downstream-controller` | `createMarkdownStream()` and `<Downstream stream={reply} />` | `append()` per delta, then `finish()`; no parent React source update for each delta  |

Both use the same replay chunks, terminal delays, styles, canonical Android paragraph layout, controls setting and plugin objects. Native engine availability is checked before the measurement window. Each row reports the source mode, selected plugin names, preview setting and engine availability from its mounted renderer. Missing receipts or required engines fail the run, so an old APK cannot silently stand in for the new benchmark.

| Profile        | Plugins        | Preview | Animation |
| -------------- | -------------- | ------- | --------- |
| `core`         | none           | on      | off       |
| `code`         | `code`         | on      | off       |
| `math`         | `math`         | on      | off       |
| `all`          | `code`, `math` | on      | off       |
| `all-animated` | `code`, `math` | on      | on        |
| `all-literal`  | `code`, `math` | off     | off       |

The last two are opt in. For example, use `--profiles all,all-animated,all-literal` to examine animation and preview. `preview: false` disables incomplete-markup repair. Code remains visible without the code plugin, with highlighting disabled; math syntax remains literal without the math plugin. All optional native packages remain linked in every APK: these profiles measure **runtime plugin selection**, not the binary-size benefit of omitting a package. See [CODE_PACKAGE_SIZE.md](CODE_PACKAGE_SIZE.md) for the separate size checks.

Optional peer IDs remain available via `--candidates`: `streamdown`, `enriched`, `markdown-display`, `marked`, `markdown-renderer`. Only Streamdown and Enriched join animated profiles. Peers retain their own rendering and repair behavior; Downstream plugin and preview profiles do not imply feature equivalence across libraries.

The report includes app and JS CPU, memory increase, frame p95 and settle after finish. `summary.json` retains all original metrics, bootstrap intervals and ratios relative to the controller. A ratio below 1 means lower cost for that metric; a single huge-workload sample does not establish a performance difference. Source equality verifies delivered text, not visual output. Inspect screenshots/accessibility output before making rendering-equivalence claims. Check `tailWindowCensored` in raw results before interpreting settle times; extend `--settle-ms` in a new session when necessary.

## Replay inputs

The fixture generator writes ten source-hashed recordings, covering prose, Unicode, code, tables, math, unfinished delimiters and concurrent replies. Huge replies exceed 64 KiB. It sends 128 Unicode code points every 16 ms, preserving complete surrogate pairs. These are **synthetic inputs**, labeled as such in recorded metadata; they are useful for controlled comparisons and are not model-paced streams. Workload IDs retain the `ai-` prefix for compatibility with the shared workload registry.

You can point `--replay` at an existing ten-response capture instead. To intentionally make a new live capture, use:

```sh
node benchmarks/phone/bench-api.mjs capture \
  --replay benchmarks/phone/results/api-sdk-capture
```

This reuses the original Vercel AI SDK path: `streamText` with `@ai-sdk/openai`. It requires `OPENAI_API_KEY` in the environment or `EXPO_PUBLIC_OPENAI_API_KEY` in `example/.env`, and makes ten model requests. Choose a model with `--model <id>`. Then pass that recording folder to `all --replay`. `bench-api.mjs all` and `run` require replay and never initiate model requests.

## Build, resume and inspect

```sh
# Separate build and run, with the same profiles in each command.
node benchmarks/phone/bench-api.mjs build \
  --profiles all --output benchmarks/phone/results/api-separated
node benchmarks/phone/bench-api.mjs run \
  --profiles all --replay benchmarks/phone/results/api-fixtures \
  --output benchmarks/phone/results/api-separated

# Resume with the saved phone, APK hashes, matrix, replay timing and finish policy.
node benchmarks/phone/bench-api.mjs resume benchmarks/phone/results/api-full

# Recompute statistics and report without a phone.
node benchmarks/phone/bench-api.mjs report benchmarks/phone/results/api-full
```

Use `--device <adb-serial>` if more than one device is attached. `--finish-delay-ms 1000` delays each row's finish independently after its last chunk. Compare against delay 0 in a separate result directory. The new suite has its own version/fingerprint and rejects historical sessions or builds. It also rejects altered replay content/cadence during resume. Reports are written inside the result directory and never replace the historical `benchmarks/phone/REPORT.md`.

`--diagnostic-trace` requires `--candidates downstream-controller`. The public string component owns its native session internally, so this harness cannot collect its controller-native diagnostic trace. Diagnostic runs include tracing overhead; compare them separately from normal performance runs.

Output files:

- `session.json`: selected matrix, order, input hashes, phone information and build fingerprints.
- `builds/<profile>.apk` and `.json`: APK plus revision, dirty status, diff hash, lockfile hash and package versions.
- `runs/*.json`: raw metrics, errors, deliveries, finish receipts and per-row renderer configuration.
- `summary.json` and `REPORT.md`: successful measured runs, comparisons and excluded failures.
- `traces/`: retained Perfetto evidence (`--keep-traces` keeps every trace).

For offline checks:

```sh
node --test benchmarks/phone/api.test.mjs benchmarks/phone/resume.test.mjs benchmarks/phone/tail.test.mjs
pnpm exec vitest run tests/phone-api-benchmark.test.tsx example/src/bench/apiBenchmark.test.ts
pnpm exec tsc -p example/tsconfig.json --noEmit
```

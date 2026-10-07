# Performance audit reproduction

Baseline revision: `144fd321f253dad53d0efe33eacbde6922c6dcfb`.

These are analysis-only experiments. They do not patch product sources. Run commands from the repository root. Source changes can alter results; compare on the same compiler and machine, without competing benchmarks.

## Native parser and prefix scaling

Host: Apple M5 ARM64, macOS, Apple clang 21.0.0, Node 24.15.0. The runner compiles actual C++ parser, highlighter, math, md4c, and Lexilla sources with `-O3 -DNDEBUG`. Each parser workload runs one warmup. Open fences report seven-sample medians; prose reports three-sample medians. Each append immediately parses. These totals exclude finish, scheduling, React Native, and native layout.

```sh
node benchmarks/results/2026-10-07/performance-audit/stream-scaling.mjs
c++ -std=c++20 -O3 -DNDEBUG -Isrc/cpp \
  benchmarks/results/2026-10-07/performance-audit/prefix-scaling.cpp \
  -o /tmp/downstream-prefix-audit
/tmp/downstream-prefix-audit
```

`native-scaling-results.txt` records medians and counters from the original audit runs. The prefix results use nine timed samples, one item per extension, and measure the actual `SharedList` type. This is an isolated bookkeeping workload, not an end-to-end parser timing.

## iOS tail layout

Requires Xcode and an installed iPhone simulator. Original run: iPhone 18 Pro, iOS 27.0, ARM64, Apple clang 21, `-O2 -DNDEBUG`, iPhoneSimulator27.0 SDK. The script compiles current rendering sources and the existing `DSFontsShim`, then installs and launches the repository's standalone NativeTextKitHarness simulator app. It uses that app's bundle ID and replaces its installed executable.

```sh
bash benchmarks/results/2026-10-07/performance-audit/ios-tail.sh
```

`ios-tail-results.txt` records the original output. Build/measure values are arithmetic means over 20 growing snapshots per size in one process. Layout call counts are instrumented through the real TextKit method. The width-change experiment is one measurement, not a distribution. Retained UTF-16 content is a logical character count, not resident memory.

The unmodified full `tests/ios/run-native-harness.sh` failed with eight declaration errors, preserved in `ios-native-harness-failure.txt`. This narrowed experiment omits unrelated segment-view translation units and passes against original rendering sources.

## Android reveal target counts

Requires kotlinc and Java. Original toolchain: Kotlin 2.4.20, JRE 17.0.20.1. It compiles production reveal code with the existing host stubs. The experiment models retained targets; it does not draw Android views.

```sh
kotlinc -nowarn -include-runtime -d /tmp/reveal-target-audit.jar \
  src/android/src/main/java/com/downstream/Reveal.kt \
  src/android/src/main/java/com/downstream/RevealCurve.kt \
  benchmarks/results/2026-10-07/performance-audit/Audit.kt \
  tests/android/reveal-stubs/*.kt
java -cp /tmp/reveal-target-audit.jar com.downstream.AuditKt
```

`reveal-target-results.txt` records the original output. Active roots include future scheduled units at zero alpha. Retained invalidation targets are shown before whole-batch completion clears them.

## Trace pairing defect

```sh
node benchmarks/results/2026-10-07/performance-audit/trace-pairing.mjs
```

This reports expected pairing alongside the current summarizer's output. The baseline incorrectly pairs different sequences and mispairs interleaved measurement snapshots. It deliberately does not change the summarizer or assert that the defect must persist.

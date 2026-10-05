# PR #23 validation

Baseline `0dab488b96b7a9dbb24cac23999593d5e6afd10d`; candidate is the local
uncommitted optimization diff over that revision. No commits or pushes made.

| Check | Result |
| --- | --- |
| `pnpm test` | 595 tests passed, 24 files |
| `pnpm test:native` | 329 tests passed, 15 suites |
| `pnpm typecheck` | Passed |
| `pnpm --filter react-native-downstream-example exec tsc --noEmit` | Passed |
| `pnpm lint` | Zero errors; five pre-existing bitwise warnings |
| `pnpm test:kotlin` | Passed, including sparse 8,192-entry image and host lookups |
| `pnpm typecheck:android` | Passed, including Kotlin, generated Java, and JNI sources |
| `pnpm test:fabric` | Passed, real RN generated headers on iOS and Android; rectangle lookup executed for both Float types |
| `pnpm test:ios:layout` | Passed, including widths, themed indentation, zero-height/stale hosts and 1,024 concurrent measurements |
| Native paired benchmark | All 40 workload fingerprints match across ABBA runs |
| React paired benchmark | All six workload fingerprints match across ABBA runs |
| iOS paired benchmark | Same geometry checksum across ABBA runs |
| Independent code review and focused architecture review | No actionable regression found |
| `git diff --check` | Passed |

[JS and library typecheck output](js-validation.txt),
[final lint output](lint-final.txt), and
[Fabric output](fabric-validation.txt) are retained here. The example typecheck
is silent on success; its [output file](example-typecheck.txt) is retained.
Kotlin, Android source checking, the full native suite, and the full iOS harness
were run successfully; the table is their recorded summary, not a raw log.

Native timing uses Apple M5 arm64 and Apple clang 21, `-O3 -DNDEBUG`, with the
same C++ harness and flags in both source roots. React timing uses production
React 19.2.0, Node 24.15.0, happy-dom, and fresh synthetic slot answers. iOS
timing uses the booted iPhone 18 Pro simulator, iOS 27.0, `-O2 -DNDEBUG`, and the
same current harness with each candidate's real native sources. Candidate order
is baseline, optimized, optimized, baseline. Timed runs are sequential.

These checks do not execute a shipping Android app or prove Fabric/Yoga relayout
after renderer-local height changes. No physical-device latency, UI frames,
scrolling, CPU, memory, or energy measurements are included.

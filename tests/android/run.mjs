// Compiles and runs the Kotlin mirrors' own case tables.
//
// `src/android/src/main/java/com/downstream/CodeLayout.kt` mirrors
// `src/cpp/downstream/CodeLayout.h`, and the C++ table in
// `src/cpp/tests/test_export.cpp` is the contract both copies answer.
// `src/android/src/main/java/com/downstream/Reveal.kt` mirrors the per-frame half
// of `src/cpp/downstream/Reveal.h`, and the contract is the curve table
// `src/cpp/tests/dump_reveal_curve.cpp` prints. This runs both tables against the
// Kotlin side, so a drift between them fails here rather than on a device.
//
// The reveal table compiles against `android.jar` but does not need a device:
// `PathInterpolator`'s methods are stubs on a host JVM, so the mirror's own solver
// answers there, and the check bounds it against the engine's numbers.
//
// It needs a Kotlin compiler, which `pnpm test:native` does not assume, so it is a
// separate command. Point KOTLIN_HOME at an unpacked kotlin-compiler, or put
// `kotlinc` on the PATH.
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { revealCurveCases } from '../../tests/engine.ts';

const here = dirname(fileURLToPath(import.meta.url));
const kotlin = (name) => join(here, '..', '..', 'src', 'android', 'src', 'main', 'java', 'com', 'downstream', name);

function kotlinc() {
  const home = process.env.KOTLIN_HOME;
  if (home && existsSync(join(home, 'bin', 'kotlinc'))) {
    return join(home, 'bin', 'kotlinc');
  }
  return 'kotlinc';
}

const output = mkdtempSync(join(tmpdir(), 'downstream-kotlin-'));
try {
  execFileSync(
    kotlinc(),
    [
      '-include-runtime', '-nowarn',
      '-d', join(output, 'code_layout.jar'),
      kotlin('CodeLayout.kt'),
      join(here, 'CodeLayoutCheck.kt'),
    ],
    { stdio: 'inherit' }
  );
  execFileSync('java', ['-cp', join(output, 'code_layout.jar'), 'com.downstream.CodeLayoutCheckKt'], {
    stdio: 'inherit',
  });

  // Only `RevealCurve.kt`, which is the half with no platform dependency. The
  // platform half lives in `Reveal.kt` and needs `android.jar`, and `android.jar`
  // bundles the whole `java.*` API, so a Kotlin compile against it loses the
  // stdlib's operator declarations and reports errors about destructuring and
  // comparisons that have nothing to do with this code. Gradle compiles the
  // platform half against the real android.jar; this checks the half that decides
  // what a reader sees.
  execFileSync(
    kotlinc(),
    [
      '-nowarn', '-include-runtime',
      '-d', join(output, 'reveal.jar'),
      kotlin('RevealCurve.kt'),
      join(here, 'RevealCheck.kt'),
    ],
    { stdio: 'inherit' }
  );
  // The engine's own numbers, piped in rather than read from a file, so the check
  // and the binary it checks cannot be built from different sources.
  const table = revealCurveCases()
    .map((entry) =>
      [
        entry.x1, entry.y1, entry.x2, entry.y2,
        entry.durationMs, entry.offsetMs, entry.alpha, entry.rise,
      ].join(' ')
    )
    .join('\n');
  execFileSync('java', ['-cp', join(output, 'reveal.jar'), 'com.downstream.RevealCheckKt'], {
    input: table + '\n',
    stdio: ['pipe', 'inherit', 'inherit'],
  });
} finally {
  rmSync(output, { recursive: true, force: true });
}

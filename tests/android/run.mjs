// Compiles and runs the Kotlin mirrors against their C++ tables.
//
// `CodeLayout.kt` mirrors `src/cpp/downstream/CodeLayout.h`,
// `ImageLayout.kt` mirrors `src/cpp/downstream/ImageLayout.h`, and `MathLayout.kt`
// mirrors the sizing in `src/cpp/math/MathLayout.h`. The C++ tables in
// `src/cpp/tests/test_export.cpp`, `test_images.cpp`, and `test_math.cpp` are the
// contracts those copies answer. This runs those same tables against the Kotlin
// side, so a drift between them fails here rather than on a device.
//
// It needs a Kotlin compiler, which `pnpm test:native` does not assume, so it is a
// separate command. Point KOTLIN_HOME at an unpacked kotlin-compiler, or put
// `kotlinc` on the PATH.
import { execFileSync } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const kotlin = (...names) =>
  names.map((name) => join(here, '..', '..', 'src', 'android', 'src', 'main', 'java', 'com', 'downstream', name));

/** Each mirror, its table, and the `main` that runs the table. */
const suites = [
  { jar: 'code_layout', sources: [kotlin('CodeLayout.kt'), join(here, 'CodeLayoutCheck.kt')] },
  { jar: 'image_layout', sources: [kotlin('ImageLayout.kt'), join(here, 'ImageLayoutCheck.kt')] },
  { jar: 'math_layout', sources: [kotlin('MathLayout.kt'), join(here, 'MathLayoutCheck.kt')] },
];

/** `code_layout` to `CodeLayoutCheckKt`. */
function camel(snake) {
  return snake
    .split('_')
    .map((part) => part[0].toUpperCase() + part.slice(1))
    .join('');
}

function kotlinc() {
  const home = process.env.KOTLIN_HOME;
  if (home && existsSync(join(home, 'bin', 'kotlinc'))) {
    return join(home, 'bin', 'kotlinc');
  }
  return 'kotlinc';
}

const output = mkdtempSync(join(tmpdir(), 'downstream-kotlin-'));
try {
  for (const suite of suites) {
    const jar = join(output, `${suite.jar}.jar`);
    execFileSync(kotlinc(), ['-include-runtime', '-nowarn', '-d', jar, ...suite.sources], {
      stdio: 'inherit',
    });
    execFileSync('java', ['-cp', jar, `com.downstream.${camel(suite.jar)}CheckKt`], { stdio: 'inherit' });
  }
} finally {
  rmSync(output, { recursive: true, force: true });
}

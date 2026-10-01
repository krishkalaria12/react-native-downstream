// Compiles and runs the Kotlin mirror's own case table.
//
// `src/android/src/main/java/com/downstream/CodeLayout.kt` mirrors
// `src/cpp/downstream/CodeLayout.h`, and the C++ table in
// `src/cpp/tests/test_export.cpp` is the contract both copies answer. This runs
// that same table against the Kotlin side, so a drift between them fails here
// rather than on a device.
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
const sources = [
  join(here, '..', '..', 'src', 'android', 'src', 'main', 'java', 'com', 'downstream', 'CodeLayout.kt'),
  join(here, 'CodeLayoutCheck.kt'),
];

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
    ['-include-runtime', '-nowarn', '-d', join(output, 'code_layout.jar'), ...sources],
    { stdio: 'inherit' }
  );
  execFileSync(
    'java',
    ['-cp', join(output, 'code_layout.jar'), 'com.downstream.CodeLayoutCheckKt'],
    { stdio: 'inherit' }
  );
} finally {
  rmSync(output, { recursive: true, force: true });
}

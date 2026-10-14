// Runs production text assignment and reading-order methods with host counters.
// This checks work counts and state transitions, not Android shaping or raster output.
import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(
  join(
    here,
    '../../src/android/src/main/java/com/downstream/TextSegmentView.kt'
  ),
  'utf8'
);
function method(name, optional = false) {
  const found = source.match(
    new RegExp(`^  (?:(?:private|override) )?fun ${name}\\([\\s\\S]*?^  }`, 'm')
  )?.[0];
  if (!found && !optional) throw new Error(`Missing production ${name}`);
  return found ?? '';
}
const traceText = source.match(
  /^  private inline fun traceText\([\s\S]*?^  }/m
)?.[0];
if (!traceText) throw new Error('Missing production traceText');
let fixture = readFileSync(join(here, 'TextAssignmentCheck.kt'), 'utf8');
fixture = fixture.replace('/* PRODUCTION_traceText */', traceText);
for (const name of [
  'setSegment',
  'setContent',
  'readingParts',
  'clearContent',
  'assignContent',
  'setSelectable',
  'onMeasure',
  'measureContent',
  'setTraceDocument',
  'setPreparedContent',
  'clearPrepared',
]) {
  fixture = fixture.replace(
    `/* PRODUCTION_${name} */`,
    method(name, name === 'assignContent')
  );
}
fixture = fixture.replace(
  '/* PRODUCTION_buildReadingParts */',
  method('buildReadingParts').replace(
    'fun buildReadingParts(',
    'fun productionReadingParts('
  )
);
const accessibility = readFileSync(
  join(
    here,
    '../../src/android/src/main/java/com/downstream/MarkdownAccessibility.kt'
  ),
  'utf8'
);
fixture = fixture.replace(
  '/* PRODUCTION_contentChanged */',
  accessibility.match(/^  fun contentChanged\([\s\S]*?^  }/m)[0]
);
const displayText = source.match(/^  private fun displayText\(\).*$/m)?.[0];
if (!displayText) throw new Error('Missing production displayText');
fixture = fixture.replace('/* PRODUCTION_displayText */', displayText);

const output = mkdtempSync(join(tmpdir(), 'downstream-text-assignment-'));
try {
  const check = join(output, 'TextAssignmentCheck.kt');
  const jar = join(output, 'check.jar');
  writeFileSync(check, fixture);
  const compiler = process.env.KOTLIN_HOME
    ? join(process.env.KOTLIN_HOME, 'bin/kotlinc')
    : 'kotlinc';
  execFileSync(compiler, ['-include-runtime', '-nowarn', '-d', jar, check], {
    stdio: 'inherit',
  });
  execFileSync('java', ['-cp', jar, 'com.downstream.TextAssignmentCheckKt'], {
    stdio: 'inherit',
  });
} finally {
  rmSync(output, { recursive: true, force: true });
}

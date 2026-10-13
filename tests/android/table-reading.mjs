// Runs the production table binding and reading builder with host allocation counters.
// This verifies metadata and state transitions; it does not model Android drawing.
import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(
  join(
    here,
    '../../src/android/src/main/java/com/downstream/TableSegmentView.kt'
  ),
  'utf8'
);
function method(name) {
  const found = source.match(
    new RegExp(`^  (?:(?:private|override) )?fun ${name}\\([\\s\\S]*?^  }`, 'm')
  )?.[0];
  if (!found) throw new Error(`Missing production ${name}`);
  return found;
}
const sharedBuilder = source.match(
  /^private fun buildTableReadingParts\([\s\S]*?^}/m
)?.[0];
const builder = sharedBuilder ?? method('buildReadingParts');
const builderName = sharedBuilder
  ? 'buildTableReadingParts'
  : 'buildReadingParts';
// Counting the builder entry leaves the actual row/link traversal unchanged.
const countedBuilder = builder.replace(
  '= buildList {',
  '= buildList {\n    readingBuilds++'
);
let fixture = readFileSync(join(here, 'TableReadingCheck.kt'), 'utf8')
  .replace('/* PRODUCTION_setSegment */', method('setSegment'))
  .replace(
    '/* PRODUCTION_READING_CACHE */',
    source.match(/^private class TableReadingParts\([\s\S]*?^}/m)[0]
  )
  .replace(
    '/* PRODUCTION_contentChanged */',
    readFileSync(
      join(
        here,
        '../../src/android/src/main/java/com/downstream/MarkdownAccessibility.kt'
      ),
      'utf8'
    )
      .match(/^  fun contentChanged\([\s\S]*?^  }/m)[0]
      .replace(
        'host.sendAccessibilityEvent(AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED)',
        'changes.add(resetFocus)'
      )
  )
  .replace('/* PRODUCTION_updateSegment */', method('updateSegment'))
  .replace('/* PRODUCTION_GRID_BUILDER */', sharedBuilder ? '' : countedBuilder)
  .replace(
    '/* PRODUCTION_SHARED_BUILDER */',
    sharedBuilder ? countedBuilder : ''
  )
  .replace(
    '/* PRODUCTION_READING_PART */',
    source.match(/^private class TableReadingPart\([\s\S]*?^\)/m)?.[0] ?? ''
  );
if (!fixture.includes('class TableReadingPart'))
  throw new Error('Missing production TableReadingPart');
if (!fixture.includes(`fun ${builderName}`))
  throw new Error('Missing injected reading builder');
const output = mkdtempSync(join(tmpdir(), 'downstream-table-reading-'));
try {
  const check = join(output, 'TableReadingCheck.kt');
  const jar = join(output, 'check.jar');
  writeFileSync(check, fixture);
  const compiler = process.env.KOTLIN_HOME
    ? join(process.env.KOTLIN_HOME, 'bin/kotlinc')
    : 'kotlinc';
  execFileSync(compiler, ['-include-runtime', '-nowarn', '-d', jar, check], {
    stdio: 'inherit',
  });
  execFileSync('java', ['-cp', jar, 'com.downstream.TableReadingCheckKt'], {
    stdio: 'inherit',
  });
} finally {
  rmSync(output, { recursive: true, force: true });
}

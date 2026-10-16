// Executes the production mounted-child layout method with work counters.
import { execFileSync } from 'node:child_process';
import {
  existsSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
const here = dirname(fileURLToPath(import.meta.url));
const sources = join(here, '../../src/android/src/main/java/com/downstream');
const view = readFileSync(join(sources, 'DownstreamMarkdownView.kt'), 'utf8');
const method = view.match(
  /  private fun layoutSegments\([^\n]*\) \{[\s\S]*?\n  }/
)?.[0];
if (!method) throw new Error('Missing production layout method');
const fixture = readFileSync(
  join(here, 'SegmentTraversalCheck.kt'),
  'utf8'
).replace('/* PRODUCTION_LAYOUT_METHOD */', method);
const output = mkdtempSync(join(tmpdir(), 'downstream-segment-traversal-'));
try {
  const check = join(output, 'SegmentTraversalCheck.kt');
  writeFileSync(check, fixture);
  const home = process.env.KOTLIN_HOME;
  const compiler =
    home && existsSync(join(home, 'bin/kotlinc'))
      ? join(home, 'bin/kotlinc')
      : 'kotlinc';
  const jar = join(output, 'check.jar');
  execFileSync(compiler, ['-include-runtime', '-nowarn', '-d', jar, check], {
    stdio: 'inherit',
  });
  execFileSync('java', ['-cp', jar, 'com.downstream.SegmentTraversalCheckKt'], {
    stdio: 'inherit',
  });
} finally {
  rmSync(output, { recursive: true, force: true });
}

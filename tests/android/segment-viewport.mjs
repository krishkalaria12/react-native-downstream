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
const source = readFileSync(
  join(
    here,
    '../../src/android/src/main/java/com/downstream/DownstreamMarkdownView.kt'
  ),
  'utf8'
);
const names = [
  'observeSegmentViewport',
  'refreshSegmentViewport',
  'viewportSegmentInterval',
  'checkViewportFrameOrder',
  'canDeferSegment',
  'recycleTextView',
  'syncSegmentViews',
  'layoutSegments',
];
const methods = names.map((name) => {
  const result = source.match(
    new RegExp(`  private fun ${name}\\([\\s\\S]*?\\n  }`)
  )?.[0];
  if (!result) throw new Error(`Missing production ${name}`);
  return result.replaceAll(
    'this@DownstreamMarkdownView',
    'this@MountedDocument'
  );
});
const listener = source.match(
  /  private val segmentViewportListener = ViewTreeObserver.OnPreDrawListener \{[\s\S]*?\n  }/
)?.[0];
if (!listener) throw new Error('Missing production viewport listener');
const fixture = readFileSync(
  join(here, 'SegmentViewportCheck.kt'),
  'utf8'
).replace('/* PRODUCTION_METHODS */', [listener, ...methods].join('\n'));
const directory = mkdtempSync(join(tmpdir(), 'downstream-segment-viewport-'));
try {
  const path = join(directory, 'Check.kt');
  writeFileSync(path, fixture);
  const compiler =
    process.env.KOTLIN_HOME &&
    existsSync(join(process.env.KOTLIN_HOME, 'bin/kotlinc'))
      ? join(process.env.KOTLIN_HOME, 'bin/kotlinc')
      : 'kotlinc';
  const jar = join(directory, 'check.jar');
  execFileSync(compiler, ['-nowarn', '-include-runtime', '-d', jar, path], {
    stdio: 'inherit',
  });
  execFileSync('java', ['-jar', jar], { stdio: 'inherit' });
} finally {
  rmSync(directory, { recursive: true, force: true });
}

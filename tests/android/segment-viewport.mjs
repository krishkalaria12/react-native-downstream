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
  'segmentScrollKey',
  'saveSegmentScroll',
  'restoreSegmentScroll',
  'recycleSegmentView',
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
let fixture = readFileSync(
  join(here, 'SegmentViewportCheck.kt'),
  'utf8'
).replace('/* PRODUCTION_METHODS */', [listener, ...methods].join('\n'));
for (const kind of ['Code', 'Table', 'Math']) {
  const contents = readFileSync(
    join(
      here,
      `../../src/android/src/main/java/com/downstream/${kind}SegmentView.kt`
    ),
    'utf8'
  );
  const capture = contents.match(
    /  fun captureViewportState\(\)[\s\S]*?(?=\n\n)/
  )?.[0];
  const restore = contents.match(
    /  fun restoreViewportState\([\s\S]*?\n  }/
  )?.[0];
  if (!capture || !restore)
    throw new Error(`Missing production ${kind} scroll state methods`);
  fixture = fixture.replace(
    `/* PRODUCTION_${kind.toUpperCase()}_SCROLL */`,
    `${capture}\n${restore}`
  );
  if (kind === 'Code') {
    const clear = contents.match(
      /  fun clearViewportContent\([\s\S]*?\n  }/
    )?.[0];
    if (!clear) throw new Error('Missing production Code recycling method');
    fixture = fixture.replace('/* PRODUCTION_CODE_CLEAR */', clear);
  }
}
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

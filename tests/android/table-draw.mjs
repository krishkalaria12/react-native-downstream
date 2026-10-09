// Compiles the production TableGrid draw method and clip helper with Canvas work counters.
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
const view = readFileSync(join(sources, 'TableSegmentView.kt'), 'utf8');
const grid = view.slice(view.indexOf('private class TableGridView'));
const method = grid.match(
  /  override fun onDraw\(canvas: Canvas\) \{[\s\S]*?\n  }/
)?.[0];
if (!method || !method.includes('drawWindow.build('))
  throw new Error('Missing production table draw method');
const viewportMethods = [
  'setDrawViewport',
  'refreshVisibleClip',
  'resetAncestorClip',
  'applyDrawViewport',
]
  .map((name) => {
    const method = grid.match(
      new RegExp(`  (?:private )?fun ${name}\\([\\s\\S]*?\\n  }`)
    )?.[0];
    if (!method) throw new Error(`Missing production ${name} method`);
    return method;
  })
  .join('\n');
const clipListener = view.match(
  /  private val visibleClipListener = ViewTreeObserver.OnPreDrawListener \{[\s\S]*?\n  }/
)?.[0];
const lifecycle = ['onAttachedToWindow', 'onDetachedFromWindow']
  .map((name) => {
    const method = view.match(
      new RegExp(`  override fun ${name}\\(\\) \\{[\\s\\S]*?\\n  }`)
    )?.[0];
    if (!method) throw new Error(`Missing production ${name}`);
    return method;
  })
  .join('\n');
if (!clipListener) throw new Error('Missing production ancestor clip listener');
const production = method.replace('override fun', 'fun');
const uncullled = production
  .replace('onDraw(', 'unculledDraw(')
  .replace(
    'drawClip.left, drawClip.top, drawClip.right, drawClip.bottom,',
    '-1000000, -1000000, 1000000, 1000000,'
  );
const fixture = readFileSync(join(here, 'TableDrawCheck.kt'), 'utf8')
  .replace('/* PRODUCTION_CLIP_LISTENER */', clipListener)
  .replace('/* PRODUCTION_CLIP_LIFECYCLE */', lifecycle)
  .replace('/* PRODUCTION_VIEWPORT_METHOD */', viewportMethods)
  .replace('/* PRODUCTION_DRAW_METHOD */', production)
  .replace('/* UNCULLED_DRAW_METHOD */', uncullled);
const output = mkdtempSync(join(tmpdir(), 'downstream-table-draw-'));
try {
  const check = join(output, 'TableDrawCheck.kt');
  writeFileSync(check, fixture);
  const home = process.env.KOTLIN_HOME;
  const compiler =
    home && existsSync(join(home, 'bin/kotlinc'))
      ? join(home, 'bin/kotlinc')
      : 'kotlinc';
  const jar = join(output, 'check.jar');
  execFileSync(
    compiler,
    [
      '-include-runtime',
      '-nowarn',
      '-d',
      jar,
      join(sources, 'TableDrawWindow.kt'),
      join(here, 'table-draw-stubs/Graphics.kt'),
      join(here, 'table-draw-stubs/ViewTreeObserver.kt'),
      check,
    ],
    { stdio: 'inherit' }
  );
  execFileSync('java', ['-cp', jar, 'com.downstream.TableDrawCheckKt'], {
    stdio: 'inherit',
  });
} finally {
  rmSync(output, { recursive: true, force: true });
}

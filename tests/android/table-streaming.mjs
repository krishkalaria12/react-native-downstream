// Executes the actual production table builder with host work counters.
// It checks cell reuse/geometry invalidation, not Android text rasterization.
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
const root = join(here, '../..');
const sources = join(root, 'src/android/src/main/java/com/downstream');
const path = 'src/android/src/main/java/com/downstream/RenderedDocument.kt';
const source = readFileSync(join(root, path), 'utf8');
function table(contents) {
  const matches = [
    ...contents.matchAll(/    private fun buildTable\([\s\S]*?\n    }/g),
  ];
  if (matches.length !== 1)
    throw new Error('Expected one production table method');
  return matches[0][0];
}
const declarations = source.match(
  /    private data class PreparedTableCell[\s\S]*?(?=\n    \/\*\* The text layout)/
);
if (!declarations || !declarations[0].includes('tableCellLayouts'))
  throw new Error('Missing production cell caches');
// Disable eligibility in the same method to compare against uncached work.
const baseline = table(source).replace(
  'val key = if (',
  'val key = if (false && '
);
let fixture = readFileSync(join(here, 'TableStreamingCheck.kt'), 'utf8');
for (const [marker, value] of [
  ['PRODUCTION_CACHE_DECLARATIONS', declarations[0]],
  ['PRODUCTION_TABLE_METHOD', table(source)],
  ['BASELINE_TABLE_METHOD', baseline.replace('buildTable(', 'baselineTable(')],
]) {
  if (fixture.split(`/* ${marker} */`).length !== 2)
    throw new Error(`Missing ${marker}`);
  fixture = fixture.replace(`/* ${marker} */`, value);
}
const output = mkdtempSync(join(tmpdir(), 'downstream-table-'));
try {
  const check = join(output, 'TableStreamingCheck.kt');
  writeFileSync(check, fixture);
  const home = process.env.KOTLIN_HOME;
  const compiler =
    home && existsSync(join(home, 'bin/kotlinc'))
      ? join(home, 'bin/kotlinc')
      : 'kotlinc';
  const jar = join(output, 'table.jar');
  execFileSync(
    compiler,
    [
      '-include-runtime',
      '-nowarn',
      '-d',
      jar,
      join(sources, 'RenderingCache.kt'),
      join(sources, 'TableCellKey.kt'),
      check,
    ],
    { stdio: 'inherit' }
  );
  execFileSync('java', ['-cp', jar, 'com.downstream.TableStreamingCheckKt'], {
    stdio: 'inherit',
  });
} finally {
  rmSync(output, { recursive: true, force: true });
}

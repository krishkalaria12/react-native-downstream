// Exercises the production JNI block reader against the native wire layout.
import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '../..');
const source = readFileSync(
  join(root, 'src/android/src/main/java/com/downstream/NativeDocument.kt'),
  'utf8'
);
const methods = [
  source.match(/  fun blockDirection\(block: Int\) = [^\n]+/)?.[0],
  source.match(
    /  fun blockFenceMeta\(block: Int\) = FenceMeta\([\s\S]*?\n  \)/
  )?.[0],
  source.match(
    /  private fun marksOf\(block: Int\): List<IntArray> \{[\s\S]*?\n  }/
  )?.[0],
];
if (methods.some((value) => value === undefined))
  throw new Error('Missing production block reader');
const stride = source.match(/private const val BLOCK_STRIDE = \d+/)?.[0];
const segmentSource = readFileSync(
  join(root, 'src/android/src/main/java/com/downstream/Segments.kt'),
  'utf8'
);
const gridMethod = segmentSource.match(
  /  fun visibleGridHeight\(layout: TableBlockLayout\): Int \{[\s\S]*?\n  }/
)?.[0];
if (!gridMethod) throw new Error('Missing production table grid height');
const fixture = `
private data class FenceMeta(val title: String, val startLine: Int, val noLineNumbers: Boolean, val marks: List<IntArray>)
private class Reader(val blocks: IntArray, val strings: Array<String>, val markValues: IntArray) {
  companion object { ${stride} }
${methods.join('\n')}
}
private class TableBlockLayout(val maxHeight: Float)
private const val MIN_VISIBLE_TABLE_ROWS = 3
private class TableGrid(val rowHeights: IntArray, val borderWidth: Int) {
  val tableHeight = rowHeights.sum() + borderWidth
${gridMethod}
}
fun main() {
  // DownstreamJni.cpp writes marks.size at field 16 and direction at field 17.
  // Three blocks ensure a later fence walks prior marks without reading direction.
  val blocks = IntArray(54)
  for (block in 0..2) { blocks[block * 18 + 13] = -1; blocks[block * 18 + 14] = 1 }
  blocks[16] = 0; blocks[17] = 1
  blocks[18 + 16] = 2; blocks[18 + 17] = 2
  blocks[36 + 16] = 0; blocks[36 + 17] = 1
  val reader = Reader(blocks, emptyArray(), intArrayOf(1, 3, 7, 9))
  check(reader.blockDirection(0) == 1 && reader.blockDirection(1) == 2)
  check(reader.blockFenceMeta(0).marks.isEmpty())
  check(reader.blockFenceMeta(1).marks.map { it.toList() } == listOf(listOf(1, 3), listOf(7, 9)))
  check(reader.blockFenceMeta(2).marks.isEmpty())
  check(Reader(blocks.copyOfRange(0, 18), emptyArray(), intArrayOf()).blockFenceMeta(0).marks.isEmpty())
  val table = TableGrid(IntArray(25) { 91 }, 2)
  check(table.visibleGridHeight(TableBlockLayout(0f)) == 2277)
  check(table.visibleGridHeight(TableBlockLayout(472.5f)) == 472)
  check(table.visibleGridHeight(TableBlockLayout(50f)) == 275)
  check(TableGrid(intArrayOf(91, 110), 2).visibleGridHeight(TableBlockLayout(50f)) == 203)
  println("native document JNI marks, direction and actual table row height checks passed")
}
`;
const output = mkdtempSync(join(tmpdir(), 'downstream-native-reader-'));
try {
  const kotlin = join(output, 'ReaderCheck.kt');
  const jar = join(output, 'reader.jar');
  writeFileSync(kotlin, fixture);
  const compiler = process.env.KOTLIN_HOME
    ? join(process.env.KOTLIN_HOME, 'bin/kotlinc')
    : 'kotlinc';
  execFileSync(compiler, ['-include-runtime', '-nowarn', '-d', jar, kotlin], {
    stdio: 'inherit',
  });
  execFileSync('java', ['-jar', jar], { stdio: 'inherit' });
} finally {
  rmSync(output, { recursive: true, force: true });
}

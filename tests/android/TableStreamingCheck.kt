package com.downstream

import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.min

// The production table builder runs against host text/layout counters. This
// proves reuse and invalidation, not Android glyph measurement or drawing.
private object Work {
  var copies = 0
  var widths = 0
  var layouts = 0
  fun reset() { copies = 0; widths = 0; layouts = 0 }
}
private data class Span(val start: Int, val end: Int, val value: Any)
private interface Spanned : CharSequence {
  val spans: List<Span>
  fun getSpans(start: Int, end: Int, type: Class<*>): Array<Any> =
    spans.filter { it.start < end && it.end > start }.map { it.value }.toTypedArray()
  companion object { const val SPAN_EXCLUSIVE_EXCLUSIVE = 33 }
}
private open class SpannedString(source: CharSequence) : Spanned {
  private val value = source.toString()
  override val spans: List<Span> = if (source is Spanned) source.spans.toList() else emptyList()
  override val length get() = value.length
  override fun get(index: Int) = value[index]
  override fun subSequence(startIndex: Int, endIndex: Int): CharSequence = value.substring(startIndex, endIndex)
  override fun toString() = value
}
private class SpannableStringBuilder(source: Spanned, start: Int, end: Int) : Spanned {
  private val value = source.toString().substring(start, end)
  override val spans = source.spans.filter { it.start < end && it.end > start }
    .map { Span(max(0, it.start - start), min(end - start, it.end - start), it.value) }.toMutableList()
  init { Work.copies++ }
  override val length get() = value.length
  override fun get(index: Int) = value[index]
  override fun subSequence(startIndex: Int, endIndex: Int): CharSequence = value.substring(startIndex, endIndex)
  override fun toString() = value
  fun setSpan(value: Any, start: Int, end: Int, flags: Int) { spans.add(Span(start, end, value)) }
}
private class Locales(val tags: String = "en-US") { fun toLanguageTags() = tags }
private class TextPaint(source: TextPaint? = null) {
  var color: Int = source?.color ?: 1
  val typeface: String = source?.typeface ?: "body"
  var textLocales: Locales = source?.textLocales ?: Locales()
}
private object Layout {
  enum class Alignment { ALIGN_CENTER, ALIGN_OPPOSITE, ALIGN_NORMAL }
  fun getDesiredWidth(text: CharSequence, paint: TextPaint): Float { Work.widths++; return text.length * 5f }
}
private class StaticLayout(val text: CharSequence, val width: Int, val alignment: Layout.Alignment) {
  val height = ceil(max(1, text.length) * 5.0 / width).toInt() * 10
}
private class TypefaceSpan(val face: String)
private class ForegroundColorSpan(val color: Int)
private class ContainerDecoration
private class Palette(val night: Boolean = false) { val secondary = 2 }
private class NativeTheme {
  fun slotTypeface(slot: Int, weight: Float, italic: Boolean) = "header"
  fun number(slot: Int) = 600f
  fun color(slot: Int, night: Boolean): Int? = 3
}
private object ThemeSlots {
  const val SLOT_TABLE_HEADER_FONT_FAMILY = 0
  const val SLOT_TABLE_HEADER_FONT_WEIGHT = 1
  const val SLOT_TABLE_HEADER_TEXT_COLOR = 2
}
private class Style(
  val themeId: Long = 0,
  val scale: Float = 1f,
  val density: Float = 1f,
  val colors: Palette = Palette(),
  val minColumnWidth: Float = 10f,
  val maxColumnWidth: Float = 100f,
) {
  val paint = TextPaint()
  val markerPaint = TextPaint()
  val theme = NativeTheme()
  val cellPaddingHorizontal = 2
  val cellPaddingVertical = 3
  val tableBorder = 1f
  fun px(value: Float) = value * scale * density
}
private class PlacedCaret(val kind: Int = 0) { companion object { val NONE = PlacedCaret() } }
private class NativeDocument(val rows: List<List<String>>, val header: Boolean = true, val alignment: Int = 0) {
  val blockCount get() = rows.size
  private val offsets = rows.flatten().runningFold(0) { at, cell -> at + cell.length }
  val text = rows.flatten().joinToString("")
  fun cellCount(row: Int) = rows[row].size
  private fun index(row: Int, column: Int) = rows.take(row).sumOf { it.size } + column
  fun cellStart(row: Int, column: Int) = offsets[index(row, column)]
  fun cellEnd(row: Int, column: Int) = offsets[index(row, column) + 1]
  fun isTableHeader(row: Int) = row == 0 && header
  fun cellAlign(row: Int, column: Int) = alignment
  fun blockStart(row: Int) = cellStart(row, 0)
  fun blockEnd(row: Int) = cellEnd(row, rows[row].lastIndex)
  fun blockIsRtl(row: Int) = false
  companion object { const val ALIGN_CENTER = 1; const val ALIGN_RIGHT = 2 }
}
private class TableSegment(
  val cells: List<List<StaticLayout>>,
  val hasHeader: Boolean,
  val contentLeading: Float,
  val containers: List<ContainerDecoration>,
  val columnWidths: IntArray,
  val rowHeights: IntArray,
  val paddingHorizontal: Int,
  val paddingVertical: Int,
  val markerPaint: TextPaint,
  val colors: Palette,
  val density: Float,
  val borderWidth: Int,
  val rtl: Boolean,
  val identity: Long,
  val blockIndex: Int,
  val blockCount: Int,
  val caretKind: Int,
  val gapBefore: Int,
  val ordinal: Int,
)
private object Production {
  /* PRODUCTION_CACHE_DECLARATIONS */
  fun buildLayout(text: CharSequence, paint: TextPaint, width: Int, alignment: Layout.Alignment): StaticLayout {
    Work.layouts++
    return StaticLayout(text, width, alignment)
  }
  fun startsRightToLeft(text: CharSequence, start: Int, end: Int) = false
  /* PRODUCTION_TABLE_METHOD */
  /* BASELINE_TABLE_METHOD */
  fun build(document: NativeDocument, style: Style = Style(), quoted: Boolean = false, source: Spanned = SpannedString(document.text), baseline: Boolean = false): TableSegment =
    if (baseline) baselineTable(document, source, 0, document.blockCount, style, Triple(emptyList(), 0f, quoted), 0, 1, 0, 0)
    else buildTable(document, source, 0, document.blockCount, style, Triple(emptyList(), 0f, quoted), 0, 1, 0, 0)
}
private fun assertSameGeometry(left: TableSegment, right: TableSegment) {
  check(left.columnWidths.contentEquals(right.columnWidths))
  check(left.rowHeights.contentEquals(right.rowHeights))
  left.cells.forEachIndexed { row, cells -> cells.forEachIndexed { column, cell ->
    val other = right.cells[row][column]
    check(cell.width == other.width && cell.height == other.height && cell.alignment == other.alignment)
    check(cell.text.toString() == other.text.toString())
  } }
}
fun main() {
  val snapshots = (1..100).map { count -> NativeDocument(listOf(listOf("key", "value")) +
    (1..count).map { row -> listOf("r${row.toString().padStart(3, '0')}", "v${row.toString().padStart(3, '0')}") }) }
  Work.reset()
  val baseline = snapshots.map { Production.build(it, baseline = true) }
  val oldCopies = Work.copies; val oldWidths = Work.widths; val oldLayouts = Work.layouts
  Work.reset()
  val optimized = snapshots.map { Production.build(it) }
  val copies = Work.copies; val widths = Work.widths; val layouts = Work.layouts
  check(oldLayouts == 10300 && oldWidths == 10300 && oldCopies == 10300)
  check(copies == 202 && widths == 202 && layouts == 202) { "work copies=$copies widths=$widths layouts=$layouts" }
  optimized.forEachIndexed { index, table -> assertSameGeometry(table, baseline[index]) }
  check(optimized.first().cells[0][0] === optimized.last().cells[0][0])
  check(optimized.last().cells[0][0].text is SpannedString)
  println("android_table streaming_100_rows cell_copies=$oldCopies->$copies desired_widths=$oldWidths->$widths layouts=$oldLayouts->$layouts")

  // A widening last row changes the first column's complete grid, while the
  // second column can keep its layout. Shrinking again must restore geometry.
  val before = Production.build(snapshots.last())
  val widenedDocument = NativeDocument(snapshots.last().rows + listOf(listOf("a much wider last cell", "v101")))
  Work.reset()
  val widened = Production.build(widenedDocument)
  check(widened.cells[0][0] !== before.cells[0][0])
  check(widened.cells[0][1] === before.cells[0][1])
  check(Work.layouts == 103) { "widening layouts=${Work.layouts}" }
  val shrunk = Production.build(snapshots.last())
  check(shrunk.cells[0][0].width == before.cells[0][0].width)
  assertSameGeometry(widened, Production.build(widenedDocument, baseline = true))
  assertSameGeometry(shrunk, baseline.last())

  val doc = NativeDocument(listOf(listOf("invalidate", "unique")))
  val initial = Production.build(doc)
  val variants = listOf(
    Production.build(doc, Style(themeId = 10)),
    Production.build(doc, Style(scale = 1.2f)),
    Production.build(doc, Style(density = 2f)),
    Production.build(doc, Style(colors = Palette(true))),
    Production.build(doc, quoted = true),
    Production.build(doc, Style().apply { paint.textLocales = Locales("ar") }),
    Production.build(NativeDocument(doc.rows, header = false)),
    Production.build(NativeDocument(doc.rows, alignment = NativeDocument.ALIGN_CENTER)),
  )
  variants.forEach { check(it.cells[0][0] !== initial.cells[0][0]) }
  check(variants.last().cells[0][0].alignment == Layout.Alignment.ALIGN_CENTER)
  // Reused cells must not contain a snapshot's link/math/inline span objects.
  val inline = SpannableStringBuilder(SpannedString(doc.text), 0, doc.text.length)
  inline.setSpan(Any(), 0, 1, 33)
  val firstStyled = Production.build(doc, source = inline)
  val nextStyled = Production.build(doc, source = inline)
  check(firstStyled.cells[0][0] !== nextStyled.cells[0][0])
  check(firstStyled.cells[0][1] === nextStyled.cells[0][1])
  check(firstStyled.cells[0][0].text is SpannableStringBuilder)
  println("android_table width_growth_shrink=pass appearance_header_alignment=pass inline_span_isolation=pass immutable_text=pass")
}

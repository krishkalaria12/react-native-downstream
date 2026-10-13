package com.downstream

private var readingBuilds = 0
private data class AccessibleTextPiece(val start: Int, val end: Int, val linkIndex: Int?)
private interface Spanned : CharSequence { val pieces: List<AccessibleTextPiece> }
private class LinkedText(private val value: String, override val pieces: List<AccessibleTextPiece>) : Spanned {
  override val length get() = value.length
  override fun get(index: Int) = value[index]
  override fun subSequence(startIndex: Int, endIndex: Int) = value.subSequence(startIndex, endIndex)
  override fun toString() = value
}
private fun linkPieces(text: Spanned, start: Int, end: Int) = text.pieces.also {
  check(start == 0 && end == text.length)
}
private data class Cell(val text: CharSequence)
private class Colors(val toolbarButtonColor: Int = 19)
private class TableSegment(
  val cells: List<List<Cell>>,
  val accessibilityRows: List<String>,
  val hasHeader: Boolean = true,
  val identity: Long = 1,
  val rtl: Boolean = false,
  val tableWidth: Int = 300,
  val colors: Colors = Colors(),
)
private object ColorStateList { fun valueOf(color: Int) = color }
private class Button { var imageTintList = 0 }
private class Scroll { var width = 100; var scrollX = 0 }
private class Accessibility(val entryCount: () -> Int) {
  var focused = -1
  var focusedIndex: Int
    get() = focused
    set(value) { focused = value }
  val changes = mutableListOf<Boolean>()
  /* PRODUCTION_contentChanged */
}
/* PRODUCTION_READING_PART */
/* PRODUCTION_SHARED_BUILDER */
/* PRODUCTION_READING_CACHE */
private class TableGridView {
  var headerOnly = false
  var readingParts: TableReadingParts? = null
  val parts get() = readingParts?.get() ?: emptyList()
  var segment: TableSegment? = null
  val accessibility = Accessibility { parts.size }
  var contentDescription: String? = null
  var invalidations = 0
  fun invalidate() { invalidations++ }
  /* PRODUCTION_updateSegment */
  /* PRODUCTION_GRID_BUILDER */
}
private class TableSegmentView {
  val headerGrid = TableGridView().apply { headerOnly = true }
  val grid = TableGridView()
  var chromeRowHeight = 0
  var segment: TableSegment? = null
  var sourceEpoch = -1
  val scroll = Scroll()
  var pendingLeadingDistance: Int? = null
  val copyButton = Button()
  val downloadButton = Button()
  val expandButton = Button()
  val fullscreenButton = Button()
  var layoutRequests = 0
  var invalidations = 0
  fun requestLayout() { layoutRequests++ }
  fun invalidate() { invalidations++ }
  /* PRODUCTION_setSegment */
}
private fun List<TableReadingPart>.values() = map { listOf(it.text, it.row, it.column, it.piece?.linkIndex, it.heading) }
private fun assertShared(view: TableSegmentView, expected: List<List<Any?>>) {
  check(view.headerGrid.parts === view.grid.parts) { "Header and body should share immutable reading metadata" }
  check(view.grid.parts.values() == expected) { "Reading metadata changed: ${view.grid.parts.values()}" }
  check(view.headerGrid.parts.values() == expected) { "Pinned header must retain all accessible rows" }
}
fun main() {
  val linked = LinkedText("pre 🔗 后", listOf(
    AccessibleTextPiece(0, 4, null), AccessibleTextPiece(4, 6, 3), AccessibleTextPiece(6, 8, null)))
  val linkedStart = LinkedText("link", listOf(AccessibleTextPiece(0, 4, 5)))
  val rows = listOf(listOf(Cell("名字"), Cell("status")), listOf(Cell(linked), Cell("✅")),
    emptyList(), listOf(Cell(linkedStart)), listOf(Cell(""), Cell("")))
  val expected = listOf<List<Any?>>(
    listOf("Header: 名字, status", 0, null, null, true),
    listOf("pre ", 1, null, null, false), listOf("🔗", 1, 0, 3, false),
    listOf(" 后, ✅", 1, null, null, false), listOf("", 2, null, null, false),
    listOf("link", 3, 0, 5, false), listOf(", ", 4, null, null, false))
  val view = TableSegmentView()
  val first = TableSegment(rows, listOf("名字, status", "pre 🔗 后, ✅", "", "link", ", "))
  view.setSegment(first, 7, 28)
  check(readingBuilds == 0) { "Binding must not build unused reading metadata; got $readingBuilds" }
  assertShared(view, expected)
  check(view.headerGrid.contentDescription == "名字, status")
  check(view.headerGrid.accessibility.changes == listOf(false))
  check(view.grid.accessibility.changes == listOf(true))
  check(view.pendingLeadingDistance == 0 && view.sourceEpoch == 7)
  check(view.copyButton.imageTintList == 19 && view.downloadButton.imageTintList == 19 &&
    view.expandButton.imageTintList == 19 && view.fullscreenButton.imageTintList == 19)
  view.headerGrid.accessibility.focusedIndex = 4
  view.grid.accessibility.focusedIndex = 2
  view.pendingLeadingDistance = null
  view.scroll.scrollX = 45
  val grown = TableSegment(rows + listOf(listOf(Cell("tail"))), first.accessibilityRows + "tail")
  view.setSegment(grown, 7, 28)
  check(readingBuilds == 2)
  assertShared(view, expected + listOf(listOf("tail", 5, null, null, false)))
  check(view.pendingLeadingDistance == 45)
  check(view.headerGrid.accessibility.focusedIndex == 4 && view.grid.accessibility.focusedIndex == 2)
  check(view.headerGrid.accessibility.changes == listOf(false, false))
  check(view.grid.accessibility.changes == listOf(true, false))
  val parts = view.grid.parts
  val layouts = view.layoutRequests
  val invalidations = view.invalidations
  view.setSegment(grown, 99, 30)
  check(readingBuilds == 2 && view.grid.parts === parts)
  check(view.sourceEpoch == 7 && view.layoutRequests == layouts + 1 && view.invalidations == invalidations)
  check(view.grid.accessibility.changes.size == 2 && view.headerGrid.accessibility.changes.size == 2)
  val reset = TableSegment(listOf(listOf(Cell(linkedStart)), emptyList()), listOf("link", ""))
  view.setSegment(reset, 8, 30)
  check(readingBuilds == 3)
  assertShared(view, listOf(listOf("Header: link", 0, 0, 5, true), listOf("", 1, null, null, false)))
  check(view.grid.accessibility.focusedIndex == -1 && view.headerGrid.accessibility.focusedIndex == -1)
  check(view.grid.accessibility.changes == listOf(true, false, true))
  check(view.pendingLeadingDistance == 0 && view.sourceEpoch == 8)
  // Missing headers and empty cells keep their existing reading order.
  view.setSegment(TableSegment(listOf(emptyList()), emptyList(), hasHeader = false, identity = 2), 8, 30)
  assertShared(view, listOf(listOf("", 0, null, null, false)))
  check(readingBuilds == 4 && view.headerGrid.contentDescription == null)
  // Same-table RTL preserves distance from the leading edge, with the previous width.
  val rtl = TableSegment(rows, first.accessibilityRows, rtl = true, identity = 9, tableWidth = 500)
  view.setSegment(rtl, 9, 30)
  view.pendingLeadingDistance = null
  view.scroll.scrollX = 80
  view.setSegment(TableSegment(rows, first.accessibilityRows, rtl = true, identity = 9, tableWidth = 550), 9, 30)
  check(view.pendingLeadingDistance == 320)
  check(readingBuilds == 4)
  val before = readingBuilds
  repeat(100) { revision ->
    val text = "流".repeat(revision + 1)
    view.setSegment(TableSegment(listOf(listOf(Cell(text))), listOf(text), hasHeader = false), 10, 30)
  }
  check(readingBuilds == before) { "Unused streamed revisions must build no metadata" }
  assertShared(view, listOf(listOf("流".repeat(100), 0, null, null, false)))
  check(readingBuilds == before + 1) { "Both providers should build current metadata exactly once" }
  // Resetting focused content must clear focus without reading the new revision.
  view.grid.accessibility.focusedIndex = 0
  view.setSegment(TableSegment(listOf(listOf(Cell("replacement"))), listOf("replacement"), hasHeader = false), 11, 30)
  check(view.grid.accessibility.focusedIndex == -1 && readingBuilds == before + 1)
  assertShared(view, listOf(listOf("replacement", 0, null, null, false)))
  check(readingBuilds == before + 2)
  println("Table reading checks passed: unused revisions skip builds, first query shares current metadata, focus/reset preserved")
}

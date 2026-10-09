package com.downstream

import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Rect
import android.view.ViewTreeObserver

private class Colors {
  val tableHeaderBackground = 1
  val separator = 2
  fun rowStripe(row: Int): Int? = if (row % 2 == 0) 3 else null
}
private class Cell(val width: Int, val height: Int) {
  fun draw(canvas: Canvas) = canvas.cell(width, height)
}
private class TableSegment(
  val rtl: Boolean, val cells: List<List<Cell>>, val rowHeights: IntArray,
  val columnWidths: IntArray, val hasHeader: Boolean = true, val borderWidth: Int = 1,
) {
  val markerPaint = Paint().apply { textSize = 14f }
  val colors = Colors()
  val tableWidth = columnWidths.sum() + borderWidth
  val rowTops = IntArray(rowHeights.size + 1).also { tops ->
    rowHeights.indices.forEach { tops[it + 1] = tops[it] + rowHeights[it] }
  }
  val columnLefts = IntArray(columnWidths.size).also { lefts ->
    var left = 0
    columnWidths.indices.forEach {
      lefts[it] = if (rtl) tableWidth - 1 - left - columnWidths[it] else left
      left += columnWidths[it]
    }
  }
  val paddingHorizontal = 1
  val paddingVertical = 1
  val blockIndex = 5
}
private class Host {
  var lookups = 0
  fun blockAlpha(block: Int): Float { lookups++; return if (block == 9) 0.5f else 1f }
}
private class TableGrid(val segment: TableSegment, val headerOnly: Boolean) {
  val paint = Paint()
  val drawClip = Rect()
  val drawWindow = TableDrawWindow()
  val internalViewport = Rect()
  val drawViewport = Rect()
  val nextDrawViewport = Rect()
  val ancestorClip = Rect()
  var isAttachedToWindow = false
  var isLaidOut = false
  val width = segment.tableWidth
  val height = segment.rowTops.last()
  var visibleBounds: Rect? = null
  fun getLocalVisibleRect(result: Rect): Boolean {
    val visible = visibleBounds ?: return false
    result.set(visible)
    return true
  }
  var hasDrawViewport = false
  var invalidations = 0
  fun invalidate() { invalidations++ }
  /* PRODUCTION_VIEWPORT_METHOD */
  val revealHost = Host()
  fun host() = revealHost
  /* PRODUCTION_DRAW_METHOD */
  /* UNCULLED_DRAW_METHOD */
}

private open class LifecycleBase {
  open fun onAttachedToWindow() {}
  open fun onDetachedFromWindow() {}
}
private class ClipOwner(val grid: TableGrid, val headerGrid: TableGrid) : LifecycleBase() {
  val viewTreeObserver = ViewTreeObserver()
  var clipObserver: ViewTreeObserver? = null
  val viewport = Viewport()
  class Viewport { var streaming = true }
  /* PRODUCTION_CLIP_LISTENER */
  /* PRODUCTION_CLIP_LIFECYCLE */
}

fun main() {
  for (rtl in listOf(false, true)) {
    val widths = intArrayOf(18, 31, 14, 26, 19)
    val heights = intArrayOf(11, 18, 12, 22, 19, 15)
    val cells = heights.map { h -> widths.map { w -> Cell(w - 2, h - 2) } }
    val segment = TableSegment(rtl, cells, heights, widths)
    for (header in listOf(false, true)) {
      for (left in -5..110 step 7) for (top in -5..100 step 7) {
        val clip = Rect(left, top, left + 13, top + 9)
        val culled = Canvas(clip)
        val full = Canvas(clip)
        TableGrid(segment, header).onDraw(culled)
        TableGrid(segment, header).unculledDraw(full)
        check(culled.visible.sorted() == full.visible.sorted()) {
          "culling changed clipped cells/fills/borders rtl=$rtl header=$header clip=$clip"
        }
        check(culled.layers == full.layers.filter { it.intersects(clip) }) {
          "partial revealed rows must preserve their layers"
        }
      }
    }
  }
  // Work stays bounded by the viewport, including border submission and host lookups.
  val rows = 10_000
  val columns = 64
  val segment = TableSegment(false, List(rows) { List(columns) { Cell(98, 18) } },
    IntArray(rows) { 20 }, IntArray(columns) { 100 })
  val grid = TableGrid(segment, false)
  val canvas = Canvas(Rect(303, 100_003, 597, 100_097))
  grid.onDraw(canvas)
  check(canvas.cells == 35) { "viewport plus glyph overhang should submit 7 rows x 5 columns, got ${canvas.cells}" }
  check(canvas.rectangles < 20) { "offscreen borders must not be submitted" }
  check(grid.revealHost.lookups == 7) { "reveal lookup must only visit visible rows" }
  // A hardware recording clip may be the complete grid. The scrolling table's
  // explicit viewport must still bound work, and moving it must select new rows.
  val recorded = Canvas(Rect(0, 0, segment.tableWidth, rows * 20))
  grid.setDrawViewport(303, 100_003, 597, 100_097)
  check(grid.invalidations == 1)
  grid.setDrawViewport(303, 100_003, 597, 100_097)
  check(grid.invalidations == 1) { "identical viewport must reuse its display list" }
  grid.onDraw(recorded)
  check(recorded.cells == 35)
  val scrolled = Canvas(Rect(0, 0, segment.tableWidth, rows * 20))
  grid.setDrawViewport(303, 100_403, 597, 100_497)
  check(grid.invalidations == 2) { "scroll changes must invalidate the culled display list" }
  grid.onDraw(scrolled)
  check(scrolled.cells == 35 && scrolled.visible != recorded.visible) { "scrolling must record newly visible rows" }
  // An uncapped table's internal viewport spans its entire body. The ancestor
  // chat viewport must reduce recording and restore content after scrolling.
  val uncapped = TableGrid(segment, false)
  uncapped.setDrawViewport(0, 0, segment.tableWidth, rows * 20)
  check(uncapped.drawViewport == uncapped.internalViewport) { "pre-layout clip must preserve full fallback" }
  uncapped.isAttachedToWindow = true
  uncapped.isLaidOut = true
  uncapped.visibleBounds = Rect(303, 100_003, 597, 100_097)
  uncapped.refreshVisibleClip()
  val outerClip = Canvas(Rect(0, 0, segment.tableWidth, rows * 20))
  uncapped.onDraw(outerClip)
  check(outerClip.cells == 35) { "uncapped table must honor outer chat clip" }
  val invalidations = uncapped.invalidations
  uncapped.refreshVisibleClip()
  check(uncapped.invalidations == invalidations) { "unchanged ancestor clip must retain recording" }
  uncapped.visibleBounds = null
  uncapped.refreshVisibleClip()
  val hidden = Canvas(Rect(0, 0, segment.tableWidth, rows * 20))
  uncapped.onDraw(hidden)
  check(hidden.cells == 0) { "fully offscreen tables must have empty recording" }
  uncapped.visibleBounds = Rect(303, 100_403, 597, 100_497)
  uncapped.refreshVisibleClip()
  val exposed = Canvas(Rect(0, 0, segment.tableWidth, rows * 20))
  uncapped.onDraw(exposed)
  check(exposed.cells == 35 && exposed.visible != outerClip.visible) { "exposure must invalidate and record newly visible content" }
  check(uncapped.invalidations == invalidations + 2)
  uncapped.resetAncestorClip()
  check(uncapped.drawViewport == uncapped.internalViewport) { "detach must restore fallback clip" }
  // The actual lifecycle hooks register one observer and remove it on detach.
  val lifecycle = ClipOwner(uncapped, TableGrid(segment, true))
  lifecycle.onAttachedToWindow()
  check(lifecycle.viewTreeObserver.listeners.size == 1)
  uncapped.visibleBounds = Rect(303, 100_803, 597, 100_897)
  check(lifecycle.viewTreeObserver.dispatchPreDraw())
  check(uncapped.drawViewport == uncapped.visibleBounds)
  lifecycle.onDetachedFromWindow()
  check(lifecycle.viewTreeObserver.listeners.isEmpty() && lifecycle.clipObserver == null)
  check(uncapped.drawViewport == uncapped.internalViewport)
  lifecycle.onAttachedToWindow()
  check(lifecycle.viewTreeObserver.listeners.size == 1) { "reattachment must register only one callback" }
  lifecycle.onDetachedFromWindow()
  val pinned = Canvas(Rect(303, 0, 597, 20))
  TableGrid(segment, true).onDraw(pinned)
  check(pinned.cells == 5) { "pinned header must cull columns and submit just one row" }
  println("table drawing: clipped LTR/RTL output matches full drawing; 640000 cells -> 35 submissions")
}

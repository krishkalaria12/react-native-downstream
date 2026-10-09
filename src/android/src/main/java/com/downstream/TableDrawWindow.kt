package com.downstream

/** Reused clip ranges. Columns stay in document order even when their x coordinates descend. */
internal class TableDrawWindow {
  var firstRow = 0
    private set
  var endRow = 0
    private set
  var firstColumn = 0
    private set
  var endColumn = 0
    private set
  var firstRowBorder = 0
    private set
  var endRowBorder = 0
    private set
  var firstColumnBorder = 0
    private set
  var endColumnBorder = 0
    private set

  fun build(
    rowTops: IntArray, columnLefts: IntArray, columnWidths: IntArray,
    rows: Int, rtl: Boolean, tableWidth: Int, hairline: Int, textOutset: Int,
    left: Int, top: Int, right: Int, bottom: Int,
  ) {
    // Leave room for glyph overhang outside a cell and for border strokes.
    // Android clips the actual drawing; only distant cells are skipped.
    val margin = maxOf(hairline, textOutset, 1)
    firstRow = lowerBound(rows) { rowTops[it + 1] >= top - margin }
    endRow = lowerBound(rows) { rowTops[it] > bottom + margin }
    val columns = columnLefts.size
    firstColumn = lowerBound(columns) {
      if (rtl) columnLefts[it] <= right + margin
      else columnLefts[it] + columnWidths[it] >= left - margin
    }
    endColumn = lowerBound(columns) {
      if (rtl) columnLefts[it] + columnWidths[it] < left - margin
      else columnLefts[it] > right + margin
    }
    // Horizontal border i is immediately above rowTops[i], except the first.
    firstRowBorder = lowerBound(rows + 1) { rowTops[it] >= top - margin }
    endRowBorder = lowerBound(rows + 1) { rowTops[it] > bottom + margin }
    firstColumnBorder = lowerBound(columns + 1) {
      val x = columnBorder(it, columnLefts, columnWidths, rtl, tableWidth, hairline)
      if (rtl) x <= right + margin else x >= left - margin
    }
    endColumnBorder = lowerBound(columns + 1) {
      val x = columnBorder(it, columnLefts, columnWidths, rtl, tableWidth, hairline)
      if (rtl) x < left - margin else x > right + margin
    }
  }

  fun columnBorder(index: Int, lefts: IntArray, widths: IntArray, rtl: Boolean, tableWidth: Int, hairline: Int): Int =
    if (rtl) {
      if (index == 0) tableWidth - hairline else lefts[index - 1] + 1 - hairline
    } else {
      if (index == lefts.size) {
        if (index == 0) 0 else lefts[index - 1] + widths[index - 1]
      } else lefts[index]
    }

  private inline fun lowerBound(size: Int, predicate: (Int) -> Boolean): Int {
    var low = 0
    var high = size
    while (low < high) {
      val middle = (low + high) ushr 1
      if (predicate(middle)) high = middle else low = middle + 1
    }
    return low
  }
}

package android.text

import android.graphics.Path

class Layout(val lineLength: Int = 12, val margin: Int = 2, val rtl: Boolean = false, val text: CharSequence = "x".repeat(120)) {
  val width = lineLength + margin
  var selections = 0
  fun getLineForOffset(offset: Int) = offset / lineLength
  fun getLineStart(line: Int) = line * lineLength
  fun getLineEnd(line: Int) = minOf((line + 1) * lineLength, text.length)
  fun getPrimaryHorizontal(offset: Int): Float {
    val position = offset % lineLength
    return (if (rtl) lineLength - position else margin + position).toFloat()
  }
  fun getLineTop(line: Int) = line * 10
  fun getLineBottom(line: Int) = (line + 1) * 10
  fun getParagraphDirection(line: Int) = if (rtl) DIR_RIGHT_TO_LEFT else 1
  fun getSelectionPath(start: Int, end: Int, out: Path) {
    selections++
    out.rewind()
    var from = start
    while (from < end) {
      val line = getLineForOffset(from)
      val to = minOf(end, (line + 1) * lineLength)
      val x1 = getPrimaryHorizontal(from)
      // An end exactly on the next line belongs to this line's trailing edge.
      val x2 = if (to == (line + 1) * lineLength) (if (rtl) 0 else width).toFloat() else getPrimaryHorizontal(to)
      out.addRect(minOf(x1, x2), getLineTop(line).toFloat(), maxOf(x1, x2), getLineBottom(line).toFloat(), Path.Direction.CW)
      from = to
    }
  }
  companion object { const val DIR_RIGHT_TO_LEFT = -1 }
}

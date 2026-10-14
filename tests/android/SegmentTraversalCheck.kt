package com.downstream

private open class View(var measuredWidth: Int = 0, var measuredHeight: Int = 0) {
  var isLayoutRequested = true
  var left = 0; var top = 0; var right = 0; var bottom = 0
  var visibility = VISIBLE
  var measures = 0; var layouts = 0
  fun measure(width: Int, height: Int) {
    measures++; measuredWidth = width; measuredHeight = height
    isLayoutRequested = true
  }
  fun layout(left: Int, top: Int, right: Int, bottom: Int) {
    layouts++; this.left = left; this.top = top; this.right = right; this.bottom = bottom
    isLayoutRequested = false
  }
  companion object { const val VISIBLE = 0; const val INVISIBLE = 4; const val GONE = 8 }
}
private object MeasureSpec {
  const val EXACTLY = 1
  fun makeMeasureSpec(size: Int, mode: Int) = size
}
private class Document(var frameData: IntArray) {
  fun frames(width: Int, chrome: Int, code: Int, image: Int, host: Int, table: Int, choices: Int) = frameData
}
private class MountedDocument(val rendered: Document?, val segmentViews: List<View>) {
  private class DeferredSegmentView : View()
  val viewportOnlyIndices: IntArray? = null
  var width = 200
  var paddingLeft = 3
  var paddingRight = 4
  var paddingTop = 2
  val chrome = 0; val codeLayout = 0; val imageLayout = 0
  val hostLayout = 0; val tableLayout = 0; val readerChoices = 0
  fun drive() = layoutSegments()
  /* PRODUCTION_LAYOUT_METHOD */
}

fun main() {
  val children = List(10_000) { View() }
  val frames = IntArray(children.size * 2) { if (it % 2 == 0) it / 2 * 20 else 20 }
  val document = Document(frames)
  val host = MountedDocument(document, children)
  host.drive()
  check(children.sumOf { it.measures } == 10_000 && children.sumOf { it.layouts } == 10_000)
  host.drive()
  check(children.sumOf { it.measures } == 10_000 && children.sumOf { it.layouts } == 10_000) {
    "settled geometry must skip every child measure and layout"
  }
  children.last().isLayoutRequested = true
  host.drive()
  check(children.last().measures == 2 && children.last().layouts == 2) { "requested child must update" }
  check(children.dropLast(1).all { it.measures == 1 && it.layouts == 1 })
  frames[frames.size - 2] += 10
  host.drive()
  check(children.last().measures == 2 && children.last().layouts == 3) { "moving frame only needs layout" }
  frames[frames.size - 1] += 5
  host.drive()
  check(children.last().measures == 3 && children.last().layouts == 4) { "changed height must measure and layout" }
  val hidden = children[5]
  frames[11] = 0
  host.drive()
  check(hidden.visibility == View.INVISIBLE && hidden.measuredHeight == 0)
  frames[11] = 20
  host.drive()
  check(hidden.visibility == View.VISIBLE && hidden.measuredHeight == 20)
  children[6].visibility = View.GONE
  host.drive()
  check(children[6].visibility == View.GONE) { "hosted children must stay gone" }
  host.width += 30
  host.drive()
  check(children.all { it.measuredWidth == 223 }) { "width changes must remeasure all children" }
  println("segment traversal: unchanged 10000 siblings skip measure/layout; requested and changed frames update")
}

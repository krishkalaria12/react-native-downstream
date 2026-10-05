// Host operation recorders, not Android rasterization.
package android.graphics

class RectF
class Path {
  enum class Direction { CW }
  data class Rectangle(var left: Float, var top: Float, var right: Float, var bottom: Float)
  val rectangles = mutableListOf<Rectangle>()
  init { constructions++ }
  fun rewind() { rectangles.clear() }
  fun addRect(left: Float, top: Float, right: Float, bottom: Float, direction: Direction) {
    if (left < right && top < bottom) rectangles.add(Rectangle(left, top, right, bottom))
  }
  fun addPath(path: Path) { rectangles.addAll(path.rectangles.map { it.copy() }) }
  fun offset(x: Float, y: Float) {
    for (rect in rectangles) { rect.left += x; rect.right += x; rect.top += y; rect.bottom += y }
  }
  companion object { var constructions = 0 }
}

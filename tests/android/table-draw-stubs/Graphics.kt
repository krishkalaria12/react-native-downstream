package android.graphics

class Rect(var left: Int = 0, var top: Int = 0, var right: Int = 0, var bottom: Int = 0) {
  fun set(other: Rect) = set(other.left, other.top, other.right, other.bottom)
  fun setEmpty() = set(0, 0, 0, 0)
  fun set(left: Int, top: Int, right: Int, bottom: Int) {
    this.left = left; this.top = top; this.right = right; this.bottom = bottom
  }
  fun intersect(other: Rect): Boolean {
    if (!intersects(other)) return false
    left = maxOf(left, other.left); top = maxOf(top, other.top)
    right = minOf(right, other.right); bottom = minOf(bottom, other.bottom)
    return true
  }
  fun intersects(other: Rect) = left < other.right && right > other.left && top < other.bottom && bottom > other.top
  override fun equals(other: Any?) = other is Rect && left == other.left && right == other.right && top == other.top && bottom == other.bottom
  override fun hashCode() = ((left * 31 + top) * 31 + right) * 31 + bottom
  override fun toString() = "$left,$top,$right,$bottom"
}
class RectF(val left: Float, val top: Float, val right: Float, val bottom: Float)
class Paint { var color = 0; var textSize = 0f }
class Canvas(private val clip: Rect) {
  val visible = mutableListOf<String>()
  val layers = mutableListOf<Rect>()
  var cells = 0
  var rectangles = 0
  private var x = 0f
  private var y = 0f
  private val saves = mutableListOf<Pair<Float, Float>>()
  fun getClipBounds(result: Rect): Boolean {
    result.left = clip.left; result.top = clip.top; result.right = clip.right; result.bottom = clip.bottom
    return clip.left < clip.right && clip.top < clip.bottom
  }
  fun save(): Int { saves.add(x to y); return saves.size }
  fun translate(dx: Float, dy: Float) { x += dx; y += dy }
  fun restore() { val saved = saves.removeAt(saves.lastIndex); x = saved.first; y = saved.second }
  fun restoreToCount(count: Int) { while (saves.size >= count) restore() }
  fun saveLayerAlpha(bounds: RectF, alpha: Int): Int {
    val rect = Rect(bounds.left.toInt(), bounds.top.toInt(), bounds.right.toInt(), bounds.bottom.toInt())
    if (rect.intersects(clip)) layers.add(rect)
    return save()
  }
  fun drawRect(left: Float, top: Float, right: Float, bottom: Float, paint: Paint) {
    rectangles++
    record("rect:${paint.color}", left + x, top + y, right + x, bottom + y)
  }
  fun cell(width: Int, height: Int) { cells++; record("cell", x - 4, y - 4, x + width + 4, y + height + 4) }
  private fun record(kind: String, left: Float, top: Float, right: Float, bottom: Float) {
    if (left < clip.right && right > clip.left && top < clip.bottom && bottom > clip.top) {
      visible.add("$kind:${maxOf(left, clip.left.toFloat())},${maxOf(top, clip.top.toFloat())},${minOf(right, clip.right.toFloat())},${minOf(bottom, clip.bottom.toFloat())}")
    }
  }
}

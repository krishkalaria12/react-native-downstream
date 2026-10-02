// Host-only operation recorders. These do not model Android rasterization.
package android.graphics

import android.content.res.AssetManager

object Color { const val BLACK = -16777216; const val TRANSPARENT = 0 }
class Typeface {
  companion object {
    val DEFAULT = Typeface()
    fun createFromAsset(assets: AssetManager, path: String) = DEFAULT
  }
}
class Paint(flags: Int = 0) {
  enum class Style { FILL, STROKE }
  var style = Style.FILL
  var color = 0
  var strokeWidth = 0f
  var textSize = 0f
  var typeface = Typeface.DEFAULT
  val fontSpacing get() = textSize
  class FontMetricsInt {
    var ascent = 0; var descent = 0; var top = 0; var bottom = 0; var leading = 0
  }
  val fontMetricsInt get() = FontMetricsInt()
  companion object { const val ANTI_ALIAS_FLAG = 1; const val SUBPIXEL_TEXT_FLAG = 2 }
}
class Path {
  val commands = mutableListOf<List<Float>>()
  init { constructions++ }
  fun moveTo(x: Float, y: Float) { commands.add(listOf(0f, x, y)) }
  fun lineTo(x: Float, y: Float) { commands.add(listOf(1f, x, y)) }
  fun quadTo(x1: Float, y1: Float, x: Float, y: Float) { commands.add(listOf(2f, x1, y1, x, y)) }
  fun cubicTo(x1: Float, y1: Float, x2: Float, y2: Float, x: Float, y: Float) {
    commands.add(listOf(3f, x1, y1, x2, y2, x, y))
  }
  fun close() { commands.add(listOf(4f)) }
  companion object { var constructions = 0 }
}
class Canvas {
  data class PathDraw(val path: Path, val color: Int, val strokeWidth: Float, val style: Paint.Style)
  data class TextDraw(val text: String, val x: Float, val y: Float, val size: Float, val color: Int)
  val paths = mutableListOf<PathDraw>()
  val texts = mutableListOf<TextDraw>()
  val scales = mutableListOf<Pair<Float, Float>>()
  val translations = mutableListOf<Pair<Float, Float>>()
  fun save() {}
  fun restore() {}
  fun translate(x: Float, y: Float) { translations.add(x to y) }
  fun scale(x: Float, y: Float) { scales.add(x to y) }
  fun clipRect(left: Int, top: Int, right: Int, bottom: Int) {}
  fun drawRect(left: Float, top: Float, right: Float, bottom: Float, paint: Paint) {}
  fun drawPath(path: Path, paint: Paint) { paths.add(PathDraw(path, paint.color, paint.strokeWidth, paint.style)) }
  fun drawText(text: String, x: Float, y: Float, paint: Paint) {
    texts.add(TextDraw(text, x, y, paint.textSize, paint.color))
  }
}

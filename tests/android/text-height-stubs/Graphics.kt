package android.graphics

open class Paint(flags: Int = 0) {
  var textSize = 16f
  var typeface: Typeface? = null
  val fontMetricsInt = FontMetricsInt()
  fun measureText(text: String) = text.length * 8f
  class FontMetricsInt(val ascent: Int = -12, val descent: Int = 4)
  companion object { const val ANTI_ALIAS_FLAG = 1 }
}
class Typeface

package android.text

import android.graphics.Paint

interface Spanned : CharSequence
class TextPaint(flags: Int = 0) : Paint(flags)
open class Layout {
  enum class Alignment { ALIGN_NORMAL }
  companion object {
    const val BREAK_STRATEGY_HIGH_QUALITY = 1
    const val HYPHENATION_FREQUENCY_NONE = 0
  }
}
object TextDirectionHeuristics { val FIRSTSTRONG_LTR = Any() }
class StaticLayout(
  val text: CharSequence,
  private val tops: IntArray,
  private val starts: IntArray,
) : Layout() {
  val height get() = tops.last()
  val lineCount get() = starts.size
  fun getLineTop(line: Int): Int { topReads++; return tops[line] }
  fun getLineStart(line: Int): Int { startReads++; return starts[line] }
  fun getLineWidth(line: Int) = 16f
  class Builder(private val text: CharSequence) {
    fun setIncludePad(value: Boolean) = this
    fun setBreakStrategy(value: Int) = this
    fun setHyphenationFrequency(value: Int) = this
    fun setTextDirection(value: Any) = this
    fun build() = StaticLayout(text, intArrayOf(0, 10), intArrayOf(0))
    companion object {
      fun obtain(text: CharSequence, start: Int, end: Int, paint: TextPaint, width: Int) = Builder(text)
    }
  }
  companion object {
    var topReads = 0
    var startReads = 0
  }
}

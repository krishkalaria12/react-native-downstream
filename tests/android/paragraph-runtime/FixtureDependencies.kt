package com.downstream

import android.content.Context
import android.graphics.Typeface
import android.text.Layout
import android.text.Spanned
import android.text.StaticLayout
import android.text.TextPaint
import android.text.TextDirectionHeuristics
import android.text.style.ClickableSpan
import android.view.View
import android.widget.FrameLayout

// Test-only collaborators outside the plain paragraph path. Framework layout,
// TextSegment/View, reveal, caret, accessibility and precomputed text are real.
internal sealed class Segment(val gapBefore: Int)
internal class ReaderChoices
internal class ContainerDecoration(val kind: Int = 0, val id: Int = 0, val open: Boolean = false)
internal class HiddenRange(val start: Int, val end: Int)
internal const val CONTAINER_DETAILS = 4
internal fun insideHiddenDetails(containers: List<ContainerDecoration>, choices: ReaderChoices) = false
internal fun visibleTextHeight(height: Int, lines: Int, top: (Int) -> Int, start: (Int) -> Int, hidden: List<HiddenRange>) = height
internal class DetailState(val open: Boolean)
internal fun resolveDetails(choices: ReaderChoices, id: Int, open: Boolean) = DetailState(open)
internal class AccessibleBlock(val start: Int, val end: Int, val heading: Boolean, val taskState: Int)
internal class Colors(val selectionColor: Int? = null)
internal class Style(val colors: Colors = Colors())
internal class RenderedDocument(val paint: TextPaint, val tracing: Boolean = false, val documentId: Long = 0L, val style: Style = Style()) {
  companion object {
    fun buildLayout(text: CharSequence, paint: TextPaint, width: Int, alignment: Layout.Alignment): StaticLayout =
      StaticLayout.Builder.obtain(text, 0, text.length, paint, width).setAlignment(alignment)
        .setTextDirection(TextDirectionHeuristics.FIRSTSTRONG_LTR).setIncludePad(false)
        .setBreakStrategy(Layout.BREAK_STRATEGY_HIGH_QUALITY).setHyphenationFrequency(Layout.HYPHENATION_FREQUENCY_NONE)
        .setUseLineSpacingFromFallbacks(true).setUseBoundsForWidth(false).build()
  }
}
internal object NativeDocument { const val TASK_CHECKED = 1; const val TASK_UNCHECKED = 2 }
internal object DownstreamNative { fun traceDocument(id: Long, name: String) {} }
internal object ControlDelivery { fun copyText(context: Context, label: String, text: String) {}; fun beginCopy() {} }
internal const val COPIED_LABEL = "copied"
internal const val HIT_SLOP = 10f
internal class DownstreamMarkdownView(context: Context) : FrameLayout(context) {
  var frame: RevealFrame? = null
  fun revealFrame(): RevealFrame? = frame
  fun exportMarkdown(from: Int, to: Int): String? = null
  fun pressLink(link: Int) {}
  fun pressTag(tag: Int, text: String) {}
  fun longPressLink(link: Int) {}
  fun onToggleDetails(id: Int, open: Boolean) {}
  fun onToggleTask(id: Int, checked: Boolean) {}
}
internal class LinkSpan(val index: Int, val color: Int = 0, val underlined: Boolean = true) : ClickableSpan() {
  override fun onClick(widget: View) {}
}
internal class TagSpan(val index: Int)
internal class SpoilerSpan(val color: Int)
internal class InlineCodeBackgroundSpan(val color: Int, val ascent: Int, val descent: Int)
internal class LongPress(context: Context, link: Int, callback: (Int) -> Unit) {
  fun schedule(x: Float, y: Float) {}
  fun moved(x: Float, y: Float) {}
  fun cancel() {}
}

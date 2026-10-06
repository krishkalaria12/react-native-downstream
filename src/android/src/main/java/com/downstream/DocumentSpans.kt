package com.downstream

import android.graphics.Canvas
import android.graphics.Paint
import android.text.Spanned
import android.text.TextPaint
import android.text.style.ClickableSpan
import android.text.style.LineBackgroundSpan
import android.text.style.LineHeightSpan
import android.text.style.MetricAffectingSpan
import android.text.style.UnderlineSpan
import android.view.View

/**
 * Marks a link's text. Taps are hit-tested by the views; accessibility
 * services activate links through ClickableSpan.onClick.
 */
internal class LinkSpan(val index: Int, val color: Int, val underlined: Boolean = true) : ClickableSpan() {
  override fun onClick(widget: View) {
    var parent: Any? = widget
    while (parent is View) {
      if (parent is DownstreamMarkdownView) {
        parent.pressLink(index)
        return
      }
      parent = parent.parent
    }
  }

  override fun updateDrawState(paint: TextPaint) {
    paint.color = color
    paint.isUnderlineText = underlined
  }
}

/**
 * Raises text by [pixels], or lowers it for a negative value, for footnote
 * references and scripts. Pixels rather than a share of the paint's size,
 * because the size span applied before this one has already shrunk the paint
 * and the shift is a share of the block's size, not the script's.
 */
internal class BaselineShiftSpan(private val pixels: Int) : MetricAffectingSpan() {
  // A positive baseline shift moves text down.
  override fun updateDrawState(paint: TextPaint) {
    paint.baselineShift -= pixels
  }

  override fun updateMeasureState(paint: TextPaint) {
    paint.baselineShift -= pixels
  }
}

/**
 * Holds a line to the surrounding text's metrics. Android's monospace font has
 * taller metrics than the body font, which would open a gap above a line with
 * inline code, and a raised or lowered run would open its line the same way.
 * Runs before BlockSpan, which adds block spacing.
 */
internal class HeldLineSpan(private val ascent: Int, private val descent: Int) : LineHeightSpan {
  override fun chooseHeight(
    text: CharSequence,
    lineStart: Int,
    lineEnd: Int,
    spanStartV: Int,
    lineHeight: Int,
    fm: Paint.FontMetricsInt,
  ) {
    fm.ascent = ascent
    fm.top = ascent
    fm.descent = descent
    fm.bottom = descent
  }
}

/**
 * A themed line height, as a minimum: a line the face needs more room for keeps
 * it, so no glyph is clipped, and the extra sits above the text, as iOS places
 * it. Runs after HeldLineSpan, which sets absolute metrics, and before
 * BlockSpan, which adds block spacing on top of the line.
 */
internal class BlockLineHeightSpan(private val height: Int) : LineHeightSpan {
  override fun chooseHeight(
    text: CharSequence,
    lineStart: Int,
    lineEnd: Int,
    spanStartV: Int,
    lineHeight: Int,
    fm: Paint.FontMetricsInt,
  ) {
    val extra = height - (fm.descent - fm.ascent)
    if (extra > 0) {
      fm.ascent -= extra
      fm.top = minOf(fm.top, fm.ascent)
    }
  }
}

/**
 * Inline code in a text segment, whose background TextSegmentView draws
 * between the code's ascent and descent. BackgroundColorSpan fills the whole
 * line, which on a block's first line includes the block spacing above it.
 */
internal class InlineCodeBackgroundSpan(val color: Int, val ascent: Int, val descent: Int)

/**
 * A strikethrough in a color the theme chose.
 *
 * Android's own `StrikethroughSpan` takes no color and draws the current text
 * color, so a theme that wants a strike in its own color needs a span of its
 * own. A null color means the text color, which is what the platform's span
 * does, so an unset slot is still the platform's span rather than this one.
 */
internal class ThemedStrikeSpan(val color: Int) : UnderlineSpan() {
  override fun updateDrawState(ds: TextPaint) {
    ds.flags = ds.flags or Paint.STRIKE_THRU_TEXT_FLAG
    ds.color = color
  }
}


/**
 * An underline in a color the theme chose, beside the strike span above.
 *
 * The same reason for existing: Android's own `UnderlineSpan` draws the current
 * text color, and a theme that wants its links — or its `<u>` — underlined in
 * another color needs a span of its own.
 */
internal class ThemedUnderlineSpan(val color: Int) : UnderlineSpan() {
  override fun updateDrawState(ds: TextPaint) {
    super.updateDrawState(ds)
    ds.underlineColor = color
  }
}

/**
 * A spoiler run, hidden until tapped.
 *
 * A marker span rather than a drawing span, because the hiding is done by the
 * background and foreground colors the builder already set: this only says *that*
 * the run is a spoiler, so a press on it can reveal it and a screen reader can
 * announce it as hidden.
 */
internal class SpoilerSpan(val color: Int)

/** One native block's reading order and role inside a shared text layout. */
internal class AccessibleBlock(val start: Int, val end: Int, val heading: Boolean, val taskState: Int)

/**
 * The Kotlin mirror of `downstream::theme_layout::contentLeading`: the whole
 * content leading a block's container path adds.
 *
 * One function because the column a React child hosted in a fence is measured
 * at has to be the column it is drawn in, and the shadow node reads the C++
 * version to measure it. The two summing the same indents in two files is
 * exactly the drift this avoids, which is the same argument `imageBox` makes.
 */
internal fun contentLeading(containers: List<ContainerDecoration>): Float {
  // Each decoration's leading is the sum of the levels above it and its width is
  // its own level, so the content leading of the whole path is the last level's
  // end. The levels have already been measured with the theme's own indents.
  val last = containers.lastOrNull() ?: return 0f
  return last.leading + last.width
}

internal fun startsRightToLeft(text: CharSequence, start: Int, end: Int): Boolean {
  var index = start
  while (index < end) {
    val codePoint = Character.codePointAt(text, index)
    when (Character.getDirectionality(codePoint)) {
      Character.DIRECTIONALITY_LEFT_TO_RIGHT -> return false
      Character.DIRECTIONALITY_RIGHT_TO_LEFT,
      Character.DIRECTIONALITY_RIGHT_TO_LEFT_ARABIC -> return true
    }
    index += Character.charCount(codePoint)
  }
  return false
}

/**
 * Block spacing and decoration in one paragraph span: extra space above the
 * block and around code, then code backgrounds, quote bars, list markers, and
 * rules. It reads its own span range, so it survives copying into a segment.
 * Mirrors src/ios/DSLayoutManager.mm.
 */
internal class BlockSpan(
  private val spacingBefore: Int,
  private val contentLeading: Float,
  private val decoration: Int,
  private val containers: List<ContainerDecoration>,
  private val markerPaint: TextPaint,
  private val colors: Palette,
  private val density: Float,
  private val rtl: Boolean,
  /** A rule's thickness, from the theme, rather than a density-sized hairline. */
  private val ruleHeight: Float,
) : LineHeightSpan, LineBackgroundSpan {
  private fun blockStart(text: CharSequence) = (text as Spanned).getSpanStart(this)

  // The first line's metrics before and after the block spacing. StaticLayout
  // hands the next line of the same paragraph the metrics this span left on the
  // first, so a wrapped second line would otherwise open the block's whole gap
  // above itself.
  private var naturalAscent = 0
  private var naturalTop = 0
  private var spacedAscent = 0
  private var spacedTop = 0

  override fun chooseHeight(
    text: CharSequence,
    lineStart: Int,
    lineEnd: Int,
    spanStartV: Int,
    lineHeight: Int,
    fm: Paint.FontMetricsInt,
  ) {
    if (lineStart == blockStart(text)) {
      naturalAscent = fm.ascent
      naturalTop = fm.top
      fm.ascent -= spacingBefore
      fm.top -= spacingBefore
      spacedAscent = fm.ascent
      spacedTop = fm.top
    } else if (spacingBefore != 0 && fm.ascent == spacedAscent && fm.top == spacedTop) {
      fm.ascent = naturalAscent
      fm.top = naturalTop
    }
  }

  override fun drawBackground(
    canvas: Canvas,
    paint: Paint,
    left: Int,
    right: Int,
    top: Int,
    baseline: Int,
    bottom: Int,
    text: CharSequence,
    lineStart: Int,
    lineEnd: Int,
    lineNumber: Int,
  ) {
    val first = lineStart == blockStart(text)
    val textTop = if (first) top + spacingBefore else top
    val savedColor = paint.color
    val savedStyle = paint.style
    paint.style = Paint.Style.FILL

    for (container in containers) {
      val barColor = container.barColor
      // A zero-width bar is a bar the theme removed, so the width is checked
      // before drawing rather than only the color.
      if (barColor != null && container.barWidth > 0f) {
        paint.color = barColor
        val barTop = if (first && !container.continuesPrevious) textTop else top
        val x = if (rtl) right - container.leading - 2 * density - container.barWidth
          else left + container.leading + 2 * density
        canvas.drawRect(x, barTop.toFloat(), x + container.barWidth, bottom.toFloat(), paint)
      }
      val marker = container.marker
      if (first && marker != null) {
        val x = if (rtl) right - container.leading - container.width + container.markerGap
          else left + container.leading + container.width - container.markerGap - markerPaint.measureText(marker)
        // The marker's own color where the theme gave it one, and the shared
        // marker paint otherwise, so an unset slot draws exactly as it always did.
        val paint = if (container.markerColor != null) {
          TextPaint(markerPaint).apply { color = container.markerColor }
        } else {
          markerPaint
        }
        canvas.drawText(marker, x, baseline.toFloat(), paint)
      }
    }

    // A code block's background is drawn by its card, not here: a card is a
    // view, so it can cover the padding a text layout cannot.
    if (decoration == DECORATION_RULE) {
      val x = if (rtl) left.toFloat() else left + contentLeading
      val endX = if (rtl) right - contentLeading else right.toFloat()
      // The rule's color and thickness are both the theme's, drawn centered on
      // the midline, which is where a one-point hairline sat.
      paint.color = colors.rule
      val thickness = maxOf(1f, ruleHeight)
      val y = ((textTop + bottom) / 2).toFloat()
      canvas.drawRect(x, y - (thickness - 1) / 2, endX, y - (thickness - 1) / 2 + thickness, paint)
    }
    paint.color = savedColor
    paint.style = savedStyle
  }

  companion object {
    const val DECORATION_NONE = 0
    const val DECORATION_RULE = 1
  }
}

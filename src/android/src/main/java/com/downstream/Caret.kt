package com.downstream

import android.graphics.Canvas
import android.graphics.Paint
import android.os.Build
import android.text.Layout
import android.text.Spanned
import android.text.style.ReplacementSpan
import android.view.View
import androidx.annotation.RequiresApi

/**
 * The caret, in the engine's terms, after it has been resolved against a
 * document.
 *
 * The engine owns the decision because it is the only thing that knows what the
 * last block is: a paragraph, a still-open fence, a table, a rule, or nothing at
 * all. Both platforms take the same resolved value, so the caret appears in the
 * same place for the same stream on both.
 */
internal data class PlacedCaret(
  /** 0 none, 1 block, 2 circle. */
  val kind: Int,
  /** 0 none, 1 after the text, 2 on a line of its own. */
  val place: Int,
) {
  companion object {
    /** After the last character, inside the last block. */
    const val PLACE_AFTER_TEXT = 1

    /** A line of its own, after the last block. */
    const val PLACE_OWN_LINE = 2

    val NONE = PlacedCaret(0, 0)
  }
}

/**
 * Reserves the caret's width in the measured text without drawing anything.
 *
 * A `ReplacementSpan` rather than a glyph because the shape is drawn by the
 * segment's own draw pass: a glyph would take the font's advance for U+258B,
 * which is not the width this caret wants, and would be selected, copied and read
 * aloud with the rest of the line. This span has no metrics, so it reserves
 * exactly the width it reports and contributes nothing else.
 */
internal class CaretSpan(
  private val width: Int,
  private val height: Int,
) : ReplacementSpan() {
  override fun getSize(
    paint: Paint,
    content: CharSequence,
    start: Int,
    end: Int,
    fm: Paint.FontMetricsInt?,
  ): Int {
    if (fm != null) {
      // Within the font's own ascent and descent, so the line height is whatever
      // it already was and the caret can never move a line.
      val metrics = paint.fontMetricsInt
      fm.ascent = metrics.ascent
      fm.top = metrics.top
      fm.descent = metrics.descent
      fm.bottom = metrics.bottom
    }
    return width
  }

  override fun draw(
    canvas: Canvas,
    content: CharSequence,
    start: Int,
    end: Int,
    x: Float,
    top: Int,
    y: Int,
    bottom: Int,
    paint: Paint,
  ) {
    // Nothing here. The caret is drawn at the reveal frontier, which is usually
    // before this placeholder's position, so a shape drawn from the span would be
    // in the wrong place mid-fade.
  }
}

/**
 * The text a selection should put on the clipboard.
 *
 * The caret is a reserved space, not content: it must never be copied, and a
 * block copy of a whole message goes through the engine's export, which reads the
 * document and has no caret at all. This is the one path that reads the segment's
 * own text, so the placeholder is stripped here.
 */
internal fun copyTextWithoutCaret(text: CharSequence, start: Int, end: Int): String {
  if (start < 0 || end > text.length || start >= end) {
    return ""
  }
  val builder = StringBuilder(end - start)
  for (index in start until end) {
    if (text[index] != CARET_PLACEHOLDER) {
      builder.append(text[index])
    }
  }
  return builder.toString()
}

/** The one character a caret's placeholder occupies. */
internal const val CARET_PLACEHOLDER = '\uFFFC'

/**
 * The caret's shape, drawn by a segment's draw pass.
 *
 * The proportions are U+258B and U+25CF in SF Pro, so the caret is the size a
 * reader has seen in every other chat UI. It is drawn rather than typeset for the
 * same reason the placeholder is: it has to look the same in every font, and it
 * must not change a line's height.
 */
internal object CaretShape {
  /** A bar as wide as half an em, as tall as the cap height plus half the descender. */
  fun barRect(paint: Paint, x: Float, baseline: Float, out: android.graphics.RectF, metrics: Paint.FontMetrics): android.graphics.RectF {
    paint.getFontMetrics(metrics)
    val em = paint.textSize
    val width = em * 0.5f
    val top = baseline + metrics.ascent * 0.86f
    val bottom = baseline + metrics.descent * 0.5f
    out.set(x, top, x + width, bottom)
    return out
  }

  /** A dot 0.55 em across, centered on the x-height. */
  fun dotRect(paint: Paint, x: Float, baseline: Float, out: android.graphics.RectF): android.graphics.RectF {
    val em = paint.textSize
    val radius = em * 0.275f
    val center = baseline - em * 0.28f
    out.set(x, center - radius, x + radius * 2, center + radius)
    return out
  }

  /**
   * The x a caret at [offset] sits at, or null when that offset is not on the
   * layout.
   *
   * A caret drawn at the reveal frontier can be in the middle of a layout that
   * has not been drawn yet, so a caret whose line has no position yet is simply
   * not drawn this frame.
   */
  fun xFor(layout: Layout, offset: Int): Float? {
    if (offset < 0 || offset > layout.text.length) {
      return null
    }
    val line = layout.getLineForOffset(offset)
    return if (line < 0) null else layout.getPrimaryHorizontal(offset)
  }
}

package com.downstream

import android.graphics.Canvas
import android.graphics.Rect
import android.text.Layout
import android.text.Spanned
import android.text.style.AbsoluteSizeSpan
import android.text.style.StrikethroughSpan

/** Bounds for querying decorations without assuming that their ink fits a line. */
internal class TextDecorationBounds {
  private var source: Spanned? = null
  private val clip = Rect()
  var maximumSize = 0f
    private set
  var minimumAscent = 0f
    private set
  var maximumDescent = 0f
    private set
  var hasBackgrounds = false
    private set
  var hasTextDecorations = false
    private set
  var firstLine = 0
    private set
  var lastLine = -1
    private set

  fun clear() {
    source = null
  }

  fun prepare(canvas: Canvas, text: Spanned): Boolean {
    if (text.isEmpty() || !canvas.getClipBounds(clip)) return false
    if (source !== text) {
      maximumSize = 0f
      minimumAscent = 0f
      maximumDescent = 0f
      // Snapshot metric spans do not mutate after assignment. Selection and
      // spoiler changes only affect nonmetric spans; a new assignment clears us.
      val backgrounds = text.getSpans(0, text.length, InlineCodeBackgroundSpan::class.java)
      val links = text.getSpans(0, text.length, LinkSpan::class.java)
      val strikes = text.getSpans(0, text.length, StrikethroughSpan::class.java)
      hasBackgrounds = backgrounds.isNotEmpty()
      hasTextDecorations = links.isNotEmpty() || strikes.isNotEmpty()
      if (hasTextDecorations) {
        for (span in text.getSpans(0, text.length, AbsoluteSizeSpan::class.java)) {
          maximumSize = maxOf(maximumSize, span.size.toFloat())
        }
      }
      for (span in backgrounds) {
        minimumAscent = minOf(minimumAscent, span.ascent.toFloat())
        maximumDescent = maxOf(maximumDescent, span.descent.toFloat())
      }
      source = text
    }
    return hasBackgrounds || hasTextDecorations
  }

  /** Monotonic baselines let even unusually large decorations use exact ink bounds. */
  fun lines(layout: Layout, first: Int, last: Int, top: Float, bottom: Float): Boolean {
    var low = first
    var high = last + 1
    while (low < high) {
      val middle = (low + high) ushr 1
      if (layout.getLineBaseline(middle) + bottom < clip.top - 1f) low = middle + 1 else high = middle
    }
    firstLine = low
    high = last + 1
    while (low < high) {
      val middle = (low + high) ushr 1
      if (layout.getLineBaseline(middle) + top <= clip.bottom + 1f) low = middle + 1 else high = middle
    }
    lastLine = low - 1
    return firstLine <= lastLine
  }
}

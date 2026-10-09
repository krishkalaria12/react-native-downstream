package com.downstream

import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint

internal class AccessibleBlock
internal class ImageEntry
internal class ImageMetrics
internal class ImageBox
internal fun imageBox(entry: ImageEntry?, width: Float, metrics: ImageMetrics) = ImageBox()
internal class MathDisplayList
internal class Palette
internal class TableBlockLayout(val maxHeight: Float)
internal const val MIN_VISIBLE_TABLE_ROWS = 5
internal object RenderedDocument {
  var builds = 0
  fun buildLayout(text: CharSequence, paint: TextPaint, width: Int, alignment: Layout.Alignment): StaticLayout {
    builds++
    return if (width >= 100) {
      StaticLayout(text, intArrayOf(0, 10, 30, 50, 60), intArrayOf(0, 12, 30, 46))
    } else {
      StaticLayout(text, intArrayOf(0, 10, 20, 40, 50, 70, 80), intArrayOf(0, 6, 12, 24, 30, 46))
    }
  }
}

/** Host fixture for TextSegment routing; the backend itself runs on real Android. */
internal class StreamingParagraphLayout(private val heightAtWidth: (Int) -> Int?) {
  var calls = 0
  fun frame(width: Int): StreamingParagraphFrame? {
    calls++
    return heightAtWidth(width)?.let { StreamingParagraphFrame(it) }
  }
}
internal class StreamingParagraphFrame(val height: Int)

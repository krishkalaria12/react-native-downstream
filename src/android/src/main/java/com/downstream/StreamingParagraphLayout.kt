package com.downstream

import android.os.Build
import android.text.Layout
import android.text.StaticLayout
import android.text.TextDirectionHeuristics
import android.text.TextPaint

/**
 * Opt-in live SIMPLE wrapping. Complete snapshots use the canonical renderer.
 *
 * Only ordinary ASCII is supported: appending to the last unfinished word can
 * change every line occupied by that word. We keep that whole word and one
 * preceding line mutable, rather than assuming only the final line can move.
 */
internal class StreamingParagraphLayout(
  val text: String,
  paint: TextPaint,
  previous: StreamingParagraphLayout? = null,
  private val lineHeight: Int = 0,
) : LiveTextLayout {
  private val paint = TextPaint(paint)
  private val metrics = ParagraphMetrics(paint)
  private val supported = text.isNotEmpty() && text.all { it in ' '..'~' }
  private val seeds: List<StreamingParagraphFrame> = if (
    supported && previous != null && previous.metrics == metrics && previous.lineHeight == lineHeight && text.startsWith(previous.text)
  ) previous.cachedFrames() else emptyList()
  private val frames = LinkedHashMap<Int, StreamingParagraphFrame?>()

  @Synchronized
  private fun cachedFrames(): List<StreamingParagraphFrame> = frames.values.filterNotNull()

  /** Widths are exact; null asks the caller to use canonical full layout. */
  @Synchronized
  override fun frame(width: Int): StreamingParagraphFrame? {
    val actualWidth = width.coerceAtLeast(1)
    if (frames.containsKey(actualWidth)) return frames[actualWidth]
    val seed = seeds.firstOrNull { it.layout.width == actualWidth }
    val result = if (supported) buildFrame(actualWidth, seed) else null
    if (frames.size == 2) frames.remove(frames.keys.first())
    frames[actualWidth] = result
    return result
  }

  private fun buildFrame(width: Int, seed: StreamingParagraphFrame?): StreamingParagraphFrame? {
    val groups = ArrayList<ParagraphGroup>()
    if (seed != null) groups.addAll(seed.groups)
    var suffixStart = seed?.suffixStart ?: 0
    // The seed's suffix was previously shaped. Promote only lines that are
    // strictly before its unfinished word, with an additional guard line.
    if (seed != null) {
      val oldSuffix = seed.suffix
      val oldText = oldSuffix.text
      var unfinished = oldText.length
      while (unfinished > 0 && oldText[unfinished - 1] != ' ') unfinished--
      val wordLine = oldSuffix.getLineForOffset(unfinished)
      val freezeLines = (wordLine - 1).coerceAtLeast(0)
      if (freezeLines > 0) {
        val promotedCharacters = oldSuffix.getLineStart(freezeLines)
        groups.add(ParagraphGroup(oldSuffix, suffixStart, freezeLines))
        suffixStart += promotedCharacters
      }
    }
    val suffixLength = text.length - suffixStart
    if (suffixLength > MAX_SUFFIX_CHARACTERS) return null
    val suffixText = text.substring(suffixStart)
    val suffix = simpleLayout(suffixText, width)
    return StreamingParagraphFrame(text, paint, width, groups, suffix, suffixStart, suffixLength)
  }

  private fun simpleLayout(text: String, width: Int): StaticLayout {
    val content: CharSequence = if (lineHeight > 0) android.text.SpannableString(text).apply {
      setSpan(ParagraphLineHeight(lineHeight), 0, length, android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
    } else text
    val builder = StaticLayout.Builder.obtain(content, 0, content.length, TextPaint(paint), width)
      .setAlignment(Layout.Alignment.ALIGN_NORMAL)
      .setIncludePad(false)
      .setLineSpacing(0f, 1f)
      .setBreakStrategy(Layout.BREAK_STRATEGY_SIMPLE)
      .setHyphenationFrequency(Layout.HYPHENATION_FREQUENCY_NONE)
      .setTextDirection(TextDirectionHeuristics.LTR)
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) builder.setUseLineSpacingFromFallbacks(true)
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.VANILLA_ICE_CREAM) builder.setUseBoundsForWidth(false)
    return builder.build()
  }

  companion object {
    const val MAX_SUFFIX_CHARACTERS = 8192
  }
}

/** Every property used by shaping; colors also reset the immutable paint. */
internal data class ParagraphMetrics(
  val size: Float, val scale: Float, val skew: Float, val spacing: Float,
  val flags: Int, val face: android.graphics.Typeface?, val features: String?,
  val locales: android.os.LocaleList, val density: Float, val color: Int,
  val variation: String?, val wordSpacing: Float, val hinting: Int,
  val elegantHeight: Boolean, val baselineShift: Int,
) {
  constructor(paint: TextPaint) : this(
    paint.textSize, paint.textScaleX, paint.textSkewX, paint.letterSpacing,
    paint.flags, paint.typeface, paint.fontFeatureSettings, paint.textLocales,
    paint.density, paint.color, if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) paint.fontVariationSettings else null,
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) paint.wordSpacing else 0f,
    paint.hinting, paint.isElegantTextHeight, paint.baselineShift,
  )
}

/** The same minimum line-height rule as canonical BlockLineHeightSpan. */
private class ParagraphLineHeight(private val height: Int) : android.text.style.LineHeightSpan {
  override fun chooseHeight(text: CharSequence, start: Int, end: Int, spanStartV: Int,
    lineHeight: Int, metrics: android.graphics.Paint.FontMetricsInt) {
    val extra = height - (metrics.descent - metrics.ascent)
    if (extra > 0) {
      metrics.ascent -= extra
      metrics.top = minOf(metrics.top, metrics.ascent)
    }
  }
}

/** A bounded previously shaped suffix, drawing only its stable leading lines. */
internal class ParagraphGroup(val layout: StaticLayout, val start: Int, val lines: Int) {
  val height = layout.getLineTop(lines)
}

internal class StreamingParagraphFrame(
  val source: String,
  paint: TextPaint,
  width: Int,
  internal val groups: List<ParagraphGroup>,
  internal val suffix: StaticLayout,
  internal val suffixStart: Int,
  val shapedCharacters: Int,
) : LiveTextFrame {
  val reusedLines: Int = groups.sumOf { it.lines }
  override val layout: Layout = CompositeLayout(
    source, paint, width,
    groups.map { LayoutPart(it.layout, it.start, it.lines) } + LayoutPart(suffix, suffixStart, suffix.lineCount),
  )
  override val height: Int get() = layout.height
}

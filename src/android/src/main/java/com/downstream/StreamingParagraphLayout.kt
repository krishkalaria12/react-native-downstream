package com.downstream

import android.graphics.Canvas
import android.graphics.Path
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
) {
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
  fun frame(width: Int): StreamingParagraphFrame? {
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
private data class ParagraphMetrics(
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
) {
  val reusedLines: Int = groups.sumOf { it.lines }
  val layout: Layout = ParagraphCompositeLayout(source, paint, width, groups, suffix, suffixStart)
  val height: Int get() = layout.height
}

/**
 * Global offsets and line geometry for reveal and accessibility. Platform
 * TextView never receives this live text, so it cannot shape the full paragraph
 * again during assignment or measurement.
 */
private class ParagraphCompositeLayout(
  source: String,
  paint: TextPaint,
  width: Int,
  groups: List<ParagraphGroup>,
  suffix: StaticLayout,
  suffixStart: Int,
) : Layout(source, TextPaint(paint), width, Alignment.ALIGN_NORMAL, 1f, 0f) {
  private class Part(val layout: StaticLayout, val start: Int, val lines: Int, val firstLine: Int, val top: Int)
  private val parts: List<Part>
  private val lines: Int
  private val totalHeight: Int

  init {
    var line = 0
    var top = 0
    parts = buildList {
      for (group in groups) {
        add(Part(group.layout, group.start, group.lines, line, top))
        line += group.lines
        top += group.height
      }
      add(Part(suffix, suffixStart, suffix.lineCount, line, top))
    }
    lines = line + suffix.lineCount
    totalHeight = top + suffix.height
  }

  private fun partIndexForLine(line: Int): Int {
    var low = 0
    var high = parts.lastIndex
    while (low < high) {
      val mid = (low + high + 1) ushr 1
      if (parts[mid].firstLine <= line) low = mid else high = mid - 1
    }
    return low
  }

  private fun partForLine(line: Int): Part = parts[partIndexForLine(line)]

  override fun getLineCount(): Int = lines
  override fun getHeight(): Int = totalHeight
  override fun getLineTop(line: Int): Int {
    if (line == lines) return totalHeight
    val part = partForLine(line)
    return part.top + part.layout.getLineTop(line - part.firstLine)
  }
  override fun getLineStart(line: Int): Int {
    if (line == lines) return text.length
    val part = partForLine(line)
    return part.start + part.layout.getLineStart(line - part.firstLine)
  }
  override fun getLineDescent(line: Int): Int {
    val part = partForLine(line)
    return part.layout.getLineDescent(line - part.firstLine)
  }
  override fun getParagraphDirection(line: Int): Int = DIR_LEFT_TO_RIGHT
  override fun getLineContainsTab(line: Int): Boolean = false
  override fun getLineDirections(line: Int): Directions {
    val part = partForLine(line)
    return part.layout.getLineDirections(line - part.firstLine)
  }
  override fun getTopPadding(): Int = 0
  override fun getBottomPadding(): Int = 0
  override fun getEllipsisStart(line: Int): Int = 0
  override fun getEllipsisCount(line: Int): Int = 0
  override fun getLineLeft(line: Int): Float {
    val part = partForLine(line)
    return part.layout.getLineLeft(line - part.firstLine)
  }
  override fun getLineRight(line: Int): Float {
    val part = partForLine(line)
    return part.layout.getLineRight(line - part.firstLine)
  }
  override fun getLineWidth(line: Int): Float {
    val part = partForLine(line)
    return part.layout.getLineWidth(line - part.firstLine)
  }
  override fun getLineMax(line: Int): Float {
    val part = partForLine(line)
    return part.layout.getLineMax(line - part.firstLine)
  }
  override fun getPrimaryHorizontal(offset: Int): Float {
    val part = partForLine(getLineForOffset(offset.coerceIn(0, text.length)))
    return part.layout.getPrimaryHorizontal((offset - part.start).coerceIn(0, part.layout.text.length))
  }
  override fun getSecondaryHorizontal(offset: Int): Float = getPrimaryHorizontal(offset)
  override fun getOffsetForHorizontal(line: Int, horizontal: Float): Int {
    val part = partForLine(line)
    return part.start + part.layout.getOffsetForHorizontal(line - part.firstLine, horizontal)
  }
  override fun getSelectionPath(start: Int, end: Int, destination: Path) {
    destination.rewind()
    if (start >= end) return
    val scratch = Path()
    var index = partIndexForLine(getLineForOffset(start))
    while (index < parts.size) {
      val part = parts[index++]
      if (part.start >= end) break
      val partEnd = part.start + part.layout.getLineStart(part.lines)
      val from = maxOf(start, part.start) - part.start
      val to = minOf(end, partEnd) - part.start
      if (from < to) {
        part.layout.getSelectionPath(from, to, scratch)
        scratch.offset(0f, part.top.toFloat())
        destination.addPath(scratch)
      }
    }
  }
  override fun draw(canvas: Canvas) {
    val clip = android.graphics.Rect()
    canvas.getClipBounds(clip)
    var index = partIndexForLine(getLineForVertical(clip.top.coerceAtLeast(0)))
    while (index < parts.size) {
      val part = parts[index++]
      if (part.top >= clip.bottom) break
      val bottom = part.top + part.layout.getLineTop(part.lines)
      if (bottom <= clip.top) continue
      val saved = canvas.save()
      canvas.clipRect(0, part.top, width, bottom)
      canvas.translate(0f, part.top.toFloat())
      part.layout.draw(canvas)
      canvas.restoreToCount(saved)
    }
  }
}

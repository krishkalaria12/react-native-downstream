package com.downstream

import android.content.Context
import android.content.res.Configuration
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Typeface
import android.os.Build
import android.text.Layout
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.StaticLayout
import android.text.TextDirectionHeuristics
import android.text.TextPaint
import android.text.style.AbsoluteSizeSpan
import android.text.style.BackgroundColorSpan
import android.text.style.ClickableSpan
import android.text.style.ForegroundColorSpan
import android.text.style.LeadingMarginSpan
import android.text.style.LineBackgroundSpan
import android.text.style.LineHeightSpan
import android.text.style.MetricAffectingSpan
import android.text.style.RelativeSizeSpan
import android.text.style.StrikethroughSpan
import android.text.style.StyleSpan
import android.text.style.TypefaceSpan
import android.util.LruCache
import android.view.View
import com.facebook.react.uimanager.PixelUtil
import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.min

/**
 * Marks a link's text. Taps are hit-tested by the views; accessibility
 * services activate links through ClickableSpan.onClick.
 */
internal class LinkSpan(val index: Int, private val color: Int) : ClickableSpan() {
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
    paint.isUnderlineText = true
  }
}

/** Raises text by a fraction of its size, for footnote references. */
private class BaselineShiftSpan(private val fraction: Float) : MetricAffectingSpan() {
  // A positive baseline shift moves text down.
  override fun updateDrawState(paint: TextPaint) {
    paint.baselineShift -= (paint.textSize * fraction).toInt()
  }

  override fun updateMeasureState(paint: TextPaint) {
    paint.baselineShift -= (paint.textSize * fraction).toInt()
  }
}

/**
 * Holds a line with inline code to the surrounding text's metrics. Android's
 * monospace font has taller metrics than the body font, which would open a
 * gap above the line. Runs before BlockSpan, which adds block spacing.
 */
private class InlineCodeLineSpan(private val ascent: Int, private val descent: Int) : LineHeightSpan {
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

internal class ContainerDecoration(
  val leading: Float,
  val width: Float,
  val barColor: Int?,
  val continuesPrevious: Boolean,
  val marker: String?,
)

/**
 * Block spacing and decoration in one paragraph span: extra space above the
 * block and around code, then code backgrounds, quote bars, list markers, and
 * rules. It reads its own span range, so it survives copying into a segment.
 * Mirrors ios/DSLayoutManager.mm.
 */
private class BlockSpan(
  private val spacingBefore: Int,
  private val codePadding: Int,
  private val contentLeading: Float,
  private val decoration: Int,
  private val containers: List<ContainerDecoration>,
  private val markerPaint: TextPaint,
  private val colors: Palette,
  private val density: Float,
) : LineHeightSpan, LineBackgroundSpan {
  private fun blockStart(text: CharSequence) = (text as Spanned).getSpanStart(this)

  private fun blockEnd(text: CharSequence): Int {
    val end = (text as Spanned).getSpanEnd(this)
    return if (end > 0 && text[end - 1] == '\n') end - 1 else end
  }

  override fun chooseHeight(
    text: CharSequence,
    lineStart: Int,
    lineEnd: Int,
    spanStartV: Int,
    lineHeight: Int,
    fm: Paint.FontMetricsInt,
  ) {
    if (lineStart == blockStart(text)) {
      val extra = spacingBefore + codePadding
      fm.ascent -= extra
      fm.top -= extra
    }
    if (codePadding > 0 && lineEnd >= blockEnd(text)) {
      fm.descent += codePadding
      fm.bottom += codePadding
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
      if (barColor != null) {
        paint.color = barColor
        val barTop = if (first && !container.continuesPrevious) textTop else top
        val x = left + container.leading + 2 * density
        canvas.drawRect(x, barTop.toFloat(), x + BAR_WIDTH * density, bottom.toFloat(), paint)
      }
      val marker = container.marker
      if (first && marker != null) {
        val x = left + container.leading + container.width - MARKER_GAP * density - markerPaint.measureText(marker)
        canvas.drawText(marker, x, baseline.toFloat(), markerPaint)
      }
    }

    val x = left + contentLeading
    when (decoration) {
      DECORATION_CODE -> {
        paint.color = colors.codeBackground
        canvas.drawRect(x, textTop.toFloat(), right.toFloat(), bottom.toFloat(), paint)
      }
      DECORATION_RULE -> {
        paint.color = colors.separator
        val y = ((textTop + bottom) / 2).toFloat()
        canvas.drawRect(x, y, right.toFloat(), y + density, paint)
      }
    }
    paint.color = savedColor
    paint.style = savedStyle
  }

  companion object {
    const val DECORATION_NONE = 0
    const val DECORATION_CODE = 1
    const val DECORATION_RULE = 2
    const val BAR_WIDTH = 3
    const val MARKER_GAP = 6
  }
}

internal class Palette(night: Boolean) {
  val text = if (night) 0xFFFFFFFF.toInt() else 0xFF000000.toInt()
  val secondary = if (night) 0x99EBEBF5.toInt() else 0x993C3C43.toInt()
  val link = if (night) 0xFF0A84FF.toInt() else 0xFF007AFF.toInt()
  val codeBackground = if (night) 0x3D767680 else 0x1F767680
  val separator = if (night) 0x99545458.toInt() else 0x4A3C3C43
  val quoteBar = if (night) 0xFF48484A.toInt() else 0xFFC7C7CC.toInt()

  /** GitHub alert colors; other quotes use a neutral bar. */
  fun admonition(type: String?): Int = when (type) {
    "note" -> 0xFF007AFF.toInt()
    "tip" -> 0xFF34C759.toInt()
    "important" -> 0xFFAF52DE.toInt()
    "warning" -> 0xFFFF9500.toInt()
    "caution" -> 0xFFFF3B30.toInt()
    else -> quoteBar
  }
}

internal sealed class Segment

/** A run of non-table blocks laid out as one text layout. */
internal class TextSegment(val text: Spanned) : Segment()

/**
 * One table. Column widths depend only on content, so the table is the same
 * at every view width; wide tables scroll horizontally.
 */
internal class TableSegment(
  val cells: List<List<StaticLayout>>,
  val hasHeader: Boolean,
  val contentLeading: Float,
  val containers: List<ContainerDecoration>,
  val columnWidths: IntArray,
  val rowHeights: IntArray,
  val paddingHorizontal: Int,
  val paddingVertical: Int,
  val markerPaint: TextPaint,
  val colors: Palette,
  val density: Float,
) : Segment() {
  val tableWidth = columnWidths.sum() + 1
  val tableHeight = rowHeights.sum() + 1
  val accessibilityText: String = cells.joinToString("\n") { row -> row.joinToString(", ") { it.text.toString() } }
}

/**
 * A document rendered to segments at one font scale. The same instance
 * measures and lays out, so both agree. Mirrors ios/DSRenderedDocument.mm.
 */
internal class RenderedDocument(
  val document: NativeDocument,
  val segments: List<Segment>,
  private val gaps: IntArray,
  val paint: TextPaint,
) {
  private var cachedWidth = -1
  private var cachedFrames = IntArray(0)

  fun createTextLayout(segment: TextSegment, width: Int): StaticLayout =
    buildLayout(segment.text, paint, width.coerceAtLeast(1), Layout.Alignment.ALIGN_NORMAL)

  /** Top and height of each segment at a width, as [top0, height0, top1, ...]. */
  @Synchronized
  fun frames(width: Int): IntArray {
    if (width == cachedWidth) {
      return cachedFrames
    }
    val frames = IntArray(segments.size * 2)
    var y = 0
    segments.forEachIndexed { index, segment ->
      y += gaps[index]
      val height = when (segment) {
        is TextSegment -> createTextLayout(segment, width).height
        is TableSegment -> segment.tableHeight
      }
      frames[index * 2] = y
      frames[index * 2 + 1] = height
      y += height
    }
    cachedWidth = width
    cachedFrames = frames
    return frames
  }

  fun height(width: Int): Int {
    val frames = frames(width)
    return if (frames.isEmpty()) 0 else frames[frames.size - 2] + frames[frames.size - 1]
  }

  /** Width for an unconstrained measure. */
  fun desiredWidth(): Int {
    var width = 0f
    for (segment in segments) {
      width = max(
        width,
        when (segment) {
          is TextSegment -> Layout.getDesiredWidth(segment.text, paint)
          is TableSegment -> segment.contentLeading + segment.tableWidth
        },
      )
    }
    return ceil(min(width, MAX_WIDTH)).toInt()
  }

  companion object {
    private const val MAX_WIDTH = 100_000f
    private const val BODY_SIZE = 16f
    private const val CODE_SIZE = 14f
    private val HEADING_SIZES = floatArrayOf(28f, 24f, 20f, 18f, 16f, 15f)
    private const val GAP_NORMAL = 12f
    private const val GAP_HEADING = 20f
    private const val GAP_TIGHT = 4f
    private const val QUOTE_INDENT = 16f
    private const val LIST_INDENT = 32f
    private const val CODE_PADDING = 10f
    private const val MIN_COLUMN_WIDTH = 56f
    private const val MAX_COLUMN_WIDTH = 280f
    private const val CELL_PADDING_HORIZONTAL = 12f
    private const val CELL_PADDING_VERTICAL = 8f

    private val cache = LruCache<String, RenderedDocument>(32)

    /** The text layout configuration shared by measurement and TextViews. */
    fun buildLayout(text: CharSequence, paint: TextPaint, width: Int, alignment: Layout.Alignment): StaticLayout {
      val builder = StaticLayout.Builder.obtain(text, 0, text.length, paint, width)
        .setAlignment(alignment)
        .setLineSpacing(0f, 1f)
        .setIncludePad(false)
        .setBreakStrategy(Layout.BREAK_STRATEGY_HIGH_QUALITY)
        .setHyphenationFrequency(Layout.HYPHENATION_FREQUENCY_NONE)
        .setTextDirection(TextDirectionHeuristics.FIRSTSTRONG_LTR)
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
        builder.setUseLineSpacingFromFallbacks(true)
      }
      return builder.build()
    }

    /** Returns a cached rendering, or null when the document is gone. */
    fun obtain(context: Context, documentId: Long, fontScale: Float): RenderedDocument? {
      val night = (context.resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
        Configuration.UI_MODE_NIGHT_YES
      val key = "$documentId:$fontScale:$night"
      cache.get(key)?.let { return it }
      val document = DownstreamNative.readDocument(documentId) ?: return null
      return build(document, fontScale, Palette(night)).also { cache.put(key, it) }
    }

    private fun build(document: NativeDocument, fontScale: Float, colors: Palette): RenderedDocument {
      val scale = if (fontScale > 0) fontScale else 1f
      val density = PixelUtil.toPixelFromDIP(1f)
      fun px(dp: Float) = dp * density * scale

      val paint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
        textSize = px(BODY_SIZE)
        color = colors.text
      }
      val markerPaint = TextPaint(paint).apply { color = colors.secondary }
      val text = SpannableStringBuilder(document.text)
      val flags = Spanned.SPAN_EXCLUSIVE_EXCLUSIVE

      fun baseGap(block: Int) = when (document.spacingBefore(block)) {
        NativeDocument.SPACING_NONE -> 0f
        NativeDocument.SPACING_TIGHT -> px(GAP_TIGHT)
        else -> px(if (document.blockKind(block) == NativeDocument.KIND_HEADING) GAP_HEADING else GAP_NORMAL)
      }

      fun containersOf(block: Int, previous: Int): Triple<List<ContainerDecoration>, Float, Boolean> {
        var leading = 0f
        var inQuote = false
        val containers = ArrayList<ContainerDecoration>()
        for (depth in 0 until document.containerCount(block)) {
          val kind = document.containerKind(block, depth)
          val continues = previous >= 0 && depth < document.containerCount(previous) &&
            document.containerId(previous, depth) == document.containerId(block, depth)
          if (kind == NativeDocument.CONTAINER_QUOTE || kind == NativeDocument.CONTAINER_ADMONITION) {
            inQuote = true
            val bar = if (kind == NativeDocument.CONTAINER_ADMONITION) {
              colors.admonition(document.marker(block, depth))
            } else {
              colors.quoteBar
            }
            containers.add(ContainerDecoration(leading, px(QUOTE_INDENT), bar, continues, null))
            leading += px(QUOTE_INDENT)
          } else {
            val marker = if (document.showsMarker(block, depth)) {
              when (document.taskState(block, depth)) {
                NativeDocument.TASK_UNCHECKED -> "\u2610"
                NativeDocument.TASK_CHECKED -> "\u2611"
                else -> document.marker(block, depth)
              }
            } else {
              null
            }
            containers.add(ContainerDecoration(leading, px(LIST_INDENT), null, continues, marker))
            leading += px(LIST_INDENT)
          }
        }
        return Triple(containers, leading, inQuote)
      }

      fun isAdmonitionTitle(block: Int): Boolean {
        val depth = document.containerCount(block) - 1
        if (document.blockKind(block) != NativeDocument.KIND_PARAGRAPH || depth < 0 ||
          document.containerKind(block, depth) != NativeDocument.CONTAINER_ADMONITION
        ) {
          return false
        }
        val previous = block - 1
        return previous < 0 || document.containerCount(previous) <= depth ||
          document.containerId(previous, depth) != document.containerId(block, depth)
      }

      // Style every non-table block in place; segments copy their ranges.
      val segmentStarts = ArrayList<Int>()
      for (block in 0 until document.blockCount) {
        val kind = document.blockKind(block)
        val startsSegment = block == 0 ||
          (kind == NativeDocument.KIND_TABLE_ROW) != (document.blockKind(block - 1) == NativeDocument.KIND_TABLE_ROW) ||
          (kind == NativeDocument.KIND_TABLE_ROW && document.tableId(block) != document.tableId(block - 1))
        if (startsSegment) {
          segmentStarts.add(block)
        }
        if (kind == NativeDocument.KIND_TABLE_ROW) {
          continue
        }

        val start = document.blockStart(block)
        val end = document.blockEnd(block)
        val spanEnd = if (block + 1 < document.blockCount) end + 1 else end
        val previous = if (startsSegment) -1 else block - 1
        val (containers, leading, inQuote) = containersOf(block, previous)

        var gap = 0f
        if (previous >= 0) {
          gap = baseGap(block)
          if (document.blockKind(previous) == NativeDocument.KIND_CODE) gap += px(CODE_PADDING)
        }
        val codePadding = if (kind == NativeDocument.KIND_CODE) px(CODE_PADDING) else 0f
        val decoration = when (kind) {
          NativeDocument.KIND_CODE -> BlockSpan.DECORATION_CODE
          NativeDocument.KIND_RULE -> BlockSpan.DECORATION_RULE
          else -> BlockSpan.DECORATION_NONE
        }

        var size = px(BODY_SIZE)
        when (kind) {
          NativeDocument.KIND_HEADING -> {
            size = px(HEADING_SIZES[document.headingLevel(block).coerceIn(1, 6) - 1])
            text.setSpan(StyleSpan(Typeface.BOLD), start, end, flags)
          }
          NativeDocument.KIND_CODE, NativeDocument.KIND_HTML -> {
            size = px(CODE_SIZE)
            text.setSpan(TypefaceSpan("monospace"), start, end, flags)
          }
        }
        text.setSpan(AbsoluteSizeSpan(size.toInt()), start, spanEnd, flags)
        val color = when {
          isAdmonitionTitle(block) -> colors.admonition(document.marker(block, document.containerCount(block) - 1))
          inQuote || (kind == NativeDocument.KIND_HEADING && document.headingLevel(block) == 6) -> colors.secondary
          else -> null
        }
        if (color != null) {
          text.setSpan(ForegroundColorSpan(color), start, end, flags)
        }
        val margin = (leading + codePadding).toInt()
        if (margin > 0) {
          text.setSpan(LeadingMarginSpan.Standard(margin), start, spanEnd, Spanned.SPAN_PARAGRAPH)
        }
        text.setSpan(
          BlockSpan(gap.toInt(), codePadding.toInt(), leading, decoration, containers, markerPaint, colors, density),
          start,
          spanEnd,
          Spanned.SPAN_PARAGRAPH,
        )
      }

      for (run in 0 until document.runCount) {
        val start = document.runStart(run)
        val end = document.runEnd(run)
        val styles = document.runStyles(run)
        val bold = styles and NativeDocument.STYLE_STRONG != 0
        val italic = styles and NativeDocument.STYLE_EMPHASIS != 0
        when {
          bold && italic -> text.setSpan(StyleSpan(Typeface.BOLD_ITALIC), start, end, flags)
          bold -> text.setSpan(StyleSpan(Typeface.BOLD), start, end, flags)
          italic -> text.setSpan(StyleSpan(Typeface.ITALIC), start, end, flags)
        }
        if (styles and NativeDocument.STYLE_STRIKETHROUGH != 0) {
          text.setSpan(StrikethroughSpan(), start, end, flags)
        }
        if (styles and NativeDocument.STYLE_CODE != 0) {
          val size = text.getSpans(start, end, AbsoluteSizeSpan::class.java).firstOrNull()?.size?.toFloat()
          val metrics = TextPaint(paint).apply { if (size != null) textSize = size }.fontMetricsInt
          text.setSpan(
            InlineCodeLineSpan(metrics.ascent, metrics.descent),
            start,
            end,
            flags or (1 shl Spanned.SPAN_PRIORITY_SHIFT),
          )
          text.setSpan(TypefaceSpan("monospace"), start, end, flags)
          text.setSpan(RelativeSizeSpan(CODE_SIZE / BODY_SIZE), start, end, flags)
          text.setSpan(BackgroundColorSpan(colors.codeBackground), start, end, flags)
        }
        if (styles and NativeDocument.STYLE_IMAGE_ALT != 0) {
          text.setSpan(ForegroundColorSpan(colors.secondary), start, end, flags)
        }
        if (styles and NativeDocument.STYLE_FOOTNOTE != 0) {
          text.setSpan(RelativeSizeSpan(0.75f), start, end, flags)
          text.setSpan(BaselineShiftSpan(0.45f), start, end, flags)
          text.setSpan(ForegroundColorSpan(colors.link), start, end, flags)
        }
        val link = document.runLink(run)
        if (link >= 0) {
          text.setSpan(LinkSpan(link, colors.link), start, end, flags)
        }
      }

      val segments = ArrayList<Segment>(segmentStarts.size)
      val gaps = IntArray(segmentStarts.size)
      segmentStarts.forEachIndexed { index, from ->
        val to = if (index + 1 < segmentStarts.size) segmentStarts[index + 1] else document.blockCount
        gaps[index] = if (from == 0) 0 else baseGap(from).toInt()
        if (document.blockKind(from) == NativeDocument.KIND_TABLE_ROW) {
          segments.add(buildTable(document, text, from, to, paint, markerPaint, colors, density, ::px, containersOf(from, -1)))
        } else {
          // Drop the separator after the segment's last block.
          val start = document.blockStart(from)
          val end = document.blockEnd(to - 1)
          segments.add(TextSegment(SpannableStringBuilder(text, start, end)))
        }
      }
      return RenderedDocument(document, segments, gaps, paint)
    }

    private fun buildTable(
      document: NativeDocument,
      text: Spanned,
      from: Int,
      to: Int,
      paint: TextPaint,
      markerPaint: TextPaint,
      colors: Palette,
      density: Float,
      px: (Float) -> Float,
      containers: Triple<List<ContainerDecoration>, Float, Boolean>,
    ): TableSegment {
      val cellPaint = TextPaint(paint).apply { if (containers.third) color = colors.secondary }
      val maxWidth = px(MAX_COLUMN_WIDTH)
      val columns = (from until to).maxOf { document.cellCount(it) }
      val widths = FloatArray(columns) { px(MIN_COLUMN_WIDTH) }
      val cellText = (from until to).map { row ->
        (0 until document.cellCount(row)).map { column ->
          val cell = SpannableStringBuilder(text, document.cellStart(row, column), document.cellEnd(row, column))
          if (document.isTableHeader(row)) {
            cell.setSpan(StyleSpan(Typeface.BOLD), 0, cell.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
          }
          widths[column] = max(widths[column], min(ceil(Layout.getDesiredWidth(cell, cellPaint)), maxWidth))
          cell
        }
      }

      val paddingHorizontal = px(CELL_PADDING_HORIZONTAL).toInt()
      val paddingVertical = px(CELL_PADDING_VERTICAL).toInt()
      val cells = cellText.mapIndexed { rowIndex, row ->
        row.mapIndexed { column, cell ->
          val alignment = when (document.cellAlign(from + rowIndex, column)) {
            NativeDocument.ALIGN_CENTER -> Layout.Alignment.ALIGN_CENTER
            NativeDocument.ALIGN_RIGHT -> Layout.Alignment.ALIGN_OPPOSITE
            else -> Layout.Alignment.ALIGN_NORMAL
          }
          buildLayout(cell, cellPaint, ceil(widths[column]).toInt(), alignment)
        }
      }
      val columnWidths = IntArray(columns) { ceil(widths[it]).toInt() + 2 * paddingHorizontal }
      val rowHeights = IntArray(cells.size) { row -> (cells[row].maxOfOrNull { it.height } ?: 0) + 2 * paddingVertical }
      return TableSegment(
        cells = cells,
        hasHeader = document.isTableHeader(from),
        contentLeading = containers.second,
        containers = containers.first,
        columnWidths = columnWidths,
        rowHeights = rowHeights,
        paddingHorizontal = paddingHorizontal,
        paddingVertical = paddingVertical,
        markerPaint = markerPaint,
        colors = colors,
        density = density,
      )
    }
  }
}

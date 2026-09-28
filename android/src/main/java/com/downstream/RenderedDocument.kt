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
import android.text.style.CharacterStyle
import android.text.style.ForegroundColorSpan
import android.text.style.LeadingMarginSpan
import android.text.style.LineBackgroundSpan
import android.text.style.LineHeightSpan
import android.text.style.MetricAffectingSpan
import android.text.style.RelativeSizeSpan
import android.text.style.StrikethroughSpan
import android.text.style.StyleSpan
import android.text.style.TabStopSpan
import android.text.style.TypefaceSpan
import android.util.LruCache
import com.facebook.react.uimanager.PixelUtil
import kotlin.math.ceil
import kotlin.math.min

/** Marks a link's text. Index into NativeDocument's link table. */
internal class LinkSpan(val index: Int, private val color: Int) : CharacterStyle() {
  override fun updateDrawState(paint: TextPaint) {
    paint.color = color
    paint.isUnderlineText = true
  }
}

/** Raises text by a fraction of its size, for footnote references. */
private class BaselineShiftSpan(private val fraction: Float) : MetricAffectingSpan() {
  override fun updateDrawState(paint: TextPaint) {
    paint.baselineShift += (paint.textSize * fraction).toInt()
  }

  override fun updateMeasureState(paint: TextPaint) {
    paint.baselineShift += (paint.textSize * fraction).toInt()
  }
}

private class ContainerDecoration(
  val leading: Float,
  val width: Float,
  val barColor: Int?,
  val continuesPrevious: Boolean,
  val marker: String?,
)

/**
 * Block spacing and decoration in one span: extra space above the block and
 * below code, then code backgrounds, quote bars, list markers, rules, and
 * table header lines. Mirrors ios/DSLayoutManager.mm.
 */
private class BlockSpan(
  private val start: Int,
  private val end: Int,
  private val spacingBefore: Int,
  private val codePadding: Int,
  private val contentLeading: Float,
  private val decoration: Int,
  private val containers: List<ContainerDecoration>,
  private val markerPaint: TextPaint,
  private val colors: Palette,
  private val density: Float,
) : LineHeightSpan, LineBackgroundSpan {
  override fun chooseHeight(
    text: CharSequence,
    lineStart: Int,
    lineEnd: Int,
    spanStartV: Int,
    lineHeight: Int,
    fm: Paint.FontMetricsInt,
  ) {
    if (lineStart == start) {
      val extra = spacingBefore + codePadding
      fm.ascent -= extra
      fm.top -= extra
    }
    if (codePadding > 0 && lineEnd >= end) {
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
    val first = lineStart == start
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
      DECORATION_TABLE_HEADER -> if (lineEnd >= end) {
        paint.color = colors.separator
        canvas.drawRect(x, bottom.toFloat(), right.toFloat(), bottom + 1f, paint)
      }
    }
    paint.color = savedColor
    paint.style = savedStyle
  }

  companion object {
    const val DECORATION_NONE = 0
    const val DECORATION_CODE = 1
    const val DECORATION_RULE = 2
    const val DECORATION_TABLE_HEADER = 3
    private const val BAR_WIDTH = 3
    private const val MARKER_GAP = 6
  }
}

internal class Palette(night: Boolean) {
  val text = if (night) 0xFFFFFFFF.toInt() else 0xFF000000.toInt()
  val secondary = if (night) 0x99EBEBF5.toInt() else 0x993C3C43.toInt()
  val link = if (night) 0xFF0A84FF.toInt() else 0xFF007AFF.toInt()
  val codeBackground = if (night) 0x3D767680 else 0x1F767680
  val separator = if (night) 0x99545458.toInt() else 0x4A3C3C43
  val quoteBar = if (night) 0xFF48484A.toInt() else 0xFFC7C7CC.toInt()

  fun admonition(type: String?): Int = when (type) {
    "note" -> 0xFF007AFF.toInt()
    "tip" -> 0xFF34C759.toInt()
    "important" -> 0xFFAF52DE.toInt()
    "warning" -> 0xFFFF9500.toInt()
    "caution" -> 0xFFFF3B30.toInt()
    else -> quoteBar
  }
}

/**
 * A document rendered to spans at one font scale. The same instance is used
 * to measure and to draw, so both lay out identical text.
 */
internal class RenderedDocument(
  val document: NativeDocument,
  val text: Spanned,
  private val paint: TextPaint,
  val accessibilityBlocks: List<String>,
) {
  fun createLayout(width: Int): StaticLayout {
    val builder = StaticLayout.Builder.obtain(text, 0, text.length, paint, width.coerceAtLeast(1))
      .setAlignment(Layout.Alignment.ALIGN_NORMAL)
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

  /** Width for an unconstrained measure. */
  fun desiredWidth(): Int = ceil(min(Layout.getDesiredWidth(text, paint), MAX_WIDTH)).toInt()

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
    private const val TABLE_COLUMN = 112f

    private val cache = LruCache<String, RenderedDocument>(32)

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

      val text = SpannableStringBuilder(document.text)
      val paint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
        textSize = px(BODY_SIZE)
        color = colors.text
      }
      val markerPaint = TextPaint(paint).apply { color = colors.secondary }
      val accessibility = ArrayList<String>(document.blockCount)
      val flags = Spanned.SPAN_EXCLUSIVE_EXCLUSIVE

      for (block in 0 until document.blockCount) {
        val kind = document.blockKind(block)
        val start = document.blockStart(block)
        val end = document.blockEnd(block)
        val spanEnd = if (block + 1 < document.blockCount) end + 1 else end

        var leading = 0f
        var inQuote = false
        val containers = ArrayList<ContainerDecoration>()
        val spoken = ArrayList<String>()
        for (depth in 0 until document.containerCount(block)) {
          val containerKind = document.containerKind(block, depth)
          val continues = block > 0 && depth < document.containerCount(block - 1) &&
            document.containerId(block - 1, depth) == document.containerId(block, depth)
          if (containerKind == NativeDocument.CONTAINER_QUOTE || containerKind == NativeDocument.CONTAINER_ADMONITION) {
            inQuote = true
            val bar = if (containerKind == NativeDocument.CONTAINER_ADMONITION) {
              colors.admonition(document.marker(block, depth))
            } else {
              colors.quoteBar
            }
            containers.add(ContainerDecoration(leading, px(QUOTE_INDENT), bar, continues, null))
            leading += px(QUOTE_INDENT)
          } else {
            var marker: String? = null
            if (document.showsMarker(block, depth)) {
              marker = when (document.taskState(block, depth)) {
                NativeDocument.TASK_UNCHECKED -> "\u2610".also { spoken.add("unchecked") }
                NativeDocument.TASK_CHECKED -> "\u2611".also { spoken.add("checked") }
                else -> document.marker(block, depth)?.also {
                  if (containerKind == NativeDocument.CONTAINER_FOOTNOTE || it.length > 1) spoken.add(it)
                }
              }
            }
            containers.add(ContainerDecoration(leading, px(LIST_INDENT), null, continues, marker))
            leading += px(LIST_INDENT)
          }
        }

        var gap = when (document.spacingBefore(block)) {
          NativeDocument.SPACING_NONE -> 0f
          NativeDocument.SPACING_TIGHT -> px(GAP_TIGHT)
          else -> px(if (kind == NativeDocument.KIND_HEADING) GAP_HEADING else GAP_NORMAL)
        }
        val codePadding = if (kind == NativeDocument.KIND_CODE) px(CODE_PADDING) else 0f
        val decoration = when {
          kind == NativeDocument.KIND_CODE -> BlockSpan.DECORATION_CODE
          kind == NativeDocument.KIND_RULE -> BlockSpan.DECORATION_RULE
          kind == NativeDocument.KIND_TABLE_ROW && document.isTableHeader(block) -> BlockSpan.DECORATION_TABLE_HEADER
          else -> BlockSpan.DECORATION_NONE
        }

        var size = px(BODY_SIZE)
        var monospaced = false
        when (kind) {
          NativeDocument.KIND_HEADING -> {
            val level = document.headingLevel(block).coerceIn(1, 6)
            size = px(HEADING_SIZES[level - 1])
            text.setSpan(StyleSpan(Typeface.BOLD), start, end, flags)
          }
          NativeDocument.KIND_CODE, NativeDocument.KIND_HTML -> {
            size = px(CODE_SIZE)
            monospaced = true
            text.setSpan(TypefaceSpan("monospace"), start, end, flags)
          }
        }
        text.setSpan(AbsoluteSizeSpan(size.toInt()), start, spanEnd, flags)
        val secondary = inQuote || (kind == NativeDocument.KIND_HEADING && document.headingLevel(block) == 6)
        if (secondary) {
          text.setSpan(ForegroundColorSpan(colors.secondary), start, end, flags)
        }
        val margin = (leading + codePadding).toInt()
        if (margin > 0) {
          text.setSpan(LeadingMarginSpan.Standard(margin), start, spanEnd, Spanned.SPAN_PARAGRAPH)
        }
        if (kind == NativeDocument.KIND_TABLE_ROW) {
          for (column in 1 until document.cellCount(block).coerceAtLeast(1)) {
            text.setSpan(TabStopSpan.Standard((px(TABLE_COLUMN) * column).toInt()), start, spanEnd, Spanned.SPAN_PARAGRAPH)
          }
        }
        text.setSpan(
          BlockSpan(
            start, end, gap.toInt(), codePadding.toInt(), leading, decoration, containers,
            markerPaint, colors, density,
          ),
          start, spanEnd, Spanned.SPAN_PARAGRAPH,
        )

        val blockText = document.text.substring(start, end).replace("\t", ", ").replace("\u200B", "")
        accessibility.add(if (spoken.isEmpty()) blockText else spoken.joinToString(", ") + ", " + blockText)
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

      return RenderedDocument(document, text, paint, accessibility)
    }
  }
}

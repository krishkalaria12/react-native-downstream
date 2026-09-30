package com.downstream

import android.content.Context
import android.content.res.Configuration
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Typeface
import android.os.Build
import android.text.Layout
import android.text.PrecomputedText
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
import android.widget.TextView
import androidx.annotation.RequiresApi
import com.facebook.react.uimanager.PixelUtil
import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.min

/**
 * Marks a link's text. Taps are hit-tested by the views; accessibility
 * services activate links through ClickableSpan.onClick.
 */
internal class LinkSpan(val index: Int, val color: Int) : ClickableSpan() {
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

/**
 * Inline code in a text segment, whose background TextSegmentView draws
 * between the code's ascent and descent. BackgroundColorSpan fills the whole
 * line, which on a block's first line includes the block spacing above it.
 */
internal class InlineCodeBackgroundSpan(val color: Int, val ascent: Int, val descent: Int)

internal class ContainerDecoration(
  val leading: Float,
  val width: Float,
  val barColor: Int?,
  val continuesPrevious: Boolean,
  val marker: String?,
)

/** One native block's reading order and role inside a shared text layout. */
internal class AccessibleBlock(val start: Int, val end: Int, val heading: Boolean, val taskState: Int)

private fun startsRightToLeft(text: CharSequence, start: Int, end: Int): Boolean {
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
private class BlockSpan(
  private val spacingBefore: Int,
  private val contentLeading: Float,
  private val decoration: Int,
  private val containers: List<ContainerDecoration>,
  private val markerPaint: TextPaint,
  private val colors: Palette,
  private val density: Float,
  private val rtl: Boolean,
) : LineHeightSpan, LineBackgroundSpan {
  private fun blockStart(text: CharSequence) = (text as Spanned).getSpanStart(this)

  override fun chooseHeight(
    text: CharSequence,
    lineStart: Int,
    lineEnd: Int,
    spanStartV: Int,
    lineHeight: Int,
    fm: Paint.FontMetricsInt,
  ) {
    if (lineStart == blockStart(text)) {
      fm.ascent -= spacingBefore
      fm.top -= spacingBefore
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
        val x = if (rtl) right - container.leading - (2 + BAR_WIDTH) * density
          else left + container.leading + 2 * density
        canvas.drawRect(x, barTop.toFloat(), x + BAR_WIDTH * density, bottom.toFloat(), paint)
      }
      val marker = container.marker
      if (first && marker != null) {
        val x = if (rtl) right - container.leading - container.width + MARKER_GAP * density
          else left + container.leading + container.width - MARKER_GAP * density - markerPaint.measureText(marker)
        canvas.drawText(marker, x, baseline.toFloat(), markerPaint)
      }
    }

    // A code block's background is drawn by its card, not here: a card is a
    // view, so it can cover the padding a text layout cannot.
    if (decoration == DECORATION_RULE) {
      val x = if (rtl) left.toFloat() else left + contentLeading
      val endX = if (rtl) right - contentLeading else right.toFloat()
      paint.color = colors.separator
      val y = ((textTop + bottom) / 2).toFloat()
      canvas.drawRect(x, y, endX, y + density, paint)
    }
    paint.color = savedColor
    paint.style = savedStyle
  }

  companion object {
    const val DECORATION_NONE = 0
    const val DECORATION_RULE = 1
    const val BAR_WIDTH = 3
    const val MARKER_GAP = 6
  }
}

internal class Palette(private val night: Boolean) {
  val text = if (night) 0xFFFFFFFF.toInt() else 0xFF000000.toInt()
  val secondary = if (night) 0x99EBEBF5.toInt() else 0x993C3C43.toInt()
  val link = if (night) 0xFF0A84FF.toInt() else 0xFF007AFF.toInt()
  val codeBackground = if (night) 0x3D767680 else 0x1F767680
  val separator = if (night) 0x99545458.toInt() else 0x4A3C3C43
  val quoteBar = if (night) 0xFF48484A.toInt() else 0xFFC7C7CC.toInt()

  /**
   * GitHub's Primer "prettylights" colors, the palette behind Shiki's
   * `github-light` and `github-dark` themes and the same table the engine and
   * iOS read, so a token cannot drift between platforms. Indexed by
   * NativeDocument.TOKEN_*; Plain and Operator take the code text color, as
   * punctuation does in GitHub's themes.
   */
  private val tokens = intArrayOf(
    0xFF1F2328.toInt(), 0xFF6E7781.toInt(), 0xFFCF222E.toInt(), 0xFF8250DF.toInt(), 0xFF0A3069.toInt(),
    0xFF0550AE.toInt(), 0xFF0550AE.toInt(), 0xFF116329.toInt(), 0xFF8250DF.toInt(), 0xFF0550AE.toInt(),
    0xFF1F2328.toInt(), 0xFF953800.toInt(), 0xFF116329.toInt(), 0xFF0550AE.toInt(), 0xFF116329.toInt(),
    0xFF82071E.toInt(), 0xFF0550AE.toInt(),
  )

  private val tokensNight = intArrayOf(
    0xFFF0F6FC.toInt(), 0xFF8B949E.toInt(), 0xFFFF7B72.toInt(), 0xFFD2A8FF.toInt(), 0xFFA5D6FF.toInt(),
    0xFF79C0FF.toInt(), 0xFF79C0FF.toInt(), 0xFF7EE787.toInt(), 0xFFD2A8FF.toInt(), 0xFF79C0FF.toInt(),
    0xFFF0F6FC.toInt(), 0xFFFFA657.toInt(), 0xFF7EE787.toInt(), 0xFF79C0FF.toInt(), 0xFF7EE787.toInt(),
    0xFFFFA198.toInt(), 0xFF79C0FF.toInt(),
  )

  fun token(tokenClass: Int): Int =
    (if (night) tokensNight else tokens)[tokenClass.coerceIn(0, NativeDocument.TOKEN_COUNT - 1)]

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

/** One unit of layout. [gapBefore] is the space above it in the document. */
internal sealed class Segment(val gapBefore: Int)

/**
 * A run of non-table blocks laid out as one text layout. On Android 9 and
 * later [text] is a PrecomputedText, built off the UI thread, so a TextView
 * showing it only breaks lines instead of measuring every glyph again.
 */
internal class TextSegment(val text: Spanned, val accessibleBlocks: List<AccessibleBlock>, gapBefore: Int) : Segment(gapBefore) {
  private var heightWidth = -1
  private var height = 0

  /** Layout height at a width. Committed segments are shared by snapshots, so this is cached. */
  @Synchronized
  fun height(width: Int, paint: TextPaint): Int {
    if (width != heightWidth) {
      height = RenderedDocument.buildLayout(text, paint, width.coerceAtLeast(1), Layout.Alignment.ALIGN_NORMAL).height
      heightWidth = width
    }
    return height
  }
}

/**
 * One code block: a header row above a horizontally scrolling body. The body is
 * a one-block text segment, so selection, plain-text copy, token colors, and
 * link hit testing inside the code are the ones a text segment already has.
 * Code is always laid out left to right, as in Streamdown.
 */
internal class CodeSegment(
  val text: Spanned,
  val accessibleBlocks: List<AccessibleBlock>,
  /** The fence language as written, or empty for an indented block. */
  val language: String,
  val blockIndex: Int,
  /**
   * The block's ordinal among the document's code blocks, which is what a
   * reader's wrap choice is keyed by. It is assigned where the rendering is
   * built rather than by the view, because a measurement needs it and a
   * measurement can run before any view has walked the segments.
   */
  val codeOrdinal: Int,
  val containers: List<ContainerDecoration>,
  val contentLeading: Int,
  /** The block's first line, which identifies it while a fence is still growing. */
  val firstLine: String,
  /** Padding inside the card, the same on every side. */
  val padding: Int,
  val colors: Palette,
  gapBefore: Int,
) : Segment(gapBefore) {
  private var wrappedWidth = -1
  private var wrappedHeight = 0
  private var unwrappedHeight = 0
  private var unwrappedWidth = 0

  /** The width the body lays out at: the card's inner width, or unbounded. */
  private fun layoutWidth(cardWidth: Int, wrapped: Boolean): Int =
    if (wrapped) (cardWidth - contentLeading - 2 * padding).coerceAtLeast(0) else Int.MAX_VALUE

  /**
   * Body height at a card width. Unwrapped, it is the line count times the line
   * height and does not depend on the width, which is why a rotation does not
   * lay code out again.
   */
  @Synchronized
  fun bodyHeight(paint: TextPaint, cardWidth: Int, wrapped: Boolean): Int {
    val width = layoutWidth(cardWidth, wrapped)
    if (wrapped) {
      if (width != wrappedWidth) {
        wrappedHeight = RenderedDocument.buildLayout(text, paint, width, Layout.Alignment.ALIGN_NORMAL).height
        wrappedWidth = width
      }
      return wrappedHeight
    }
    if (unwrappedHeight == 0) {
      val layout = StaticLayout.Builder.obtain(text, 0, text.length, paint, Int.MAX_VALUE)
        .setIncludePad(false)
        .setBreakStrategy(Layout.BREAK_STRATEGY_HIGH_QUALITY)
        .setHyphenationFrequency(Layout.HYPHENATION_FREQUENCY_NONE)
        .setTextDirection(TextDirectionHeuristics.FIRSTSTRONG_LTR)
        .build()
      unwrappedHeight = layout.height
      unwrappedWidth = layout.width
    }
    return unwrappedHeight
  }

  /** Body width at a card width, which is what the horizontal scroll view scrolls. */
  @Synchronized
  fun bodyWidth(paint: TextPaint, cardWidth: Int, wrapped: Boolean): Int {
    if (!wrapped) {
      bodyHeight(paint, cardWidth, false)
      return unwrappedWidth
    }
    return layoutWidth(cardWidth, true)
  }
}

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
  val rtl: Boolean,
  val identity: Long,
  /** The segment's first block, which is what a copy or a download reads. */
  val blockIndex: Int,
  val blockCount: Int,
  gapBefore: Int,
) : Segment(gapBefore) {
  val tableWidth = columnWidths.sum() + 1
  val tableHeight = rowHeights.sum() + 1
  val rowTops = IntArray(rowHeights.size + 1).also { tops ->
    for (index in rowHeights.indices) tops[index + 1] = tops[index] + rowHeights[index]
  }
  val accessibilityRows: List<String> = cells.mapIndexed { index, row ->
    val label = row.joinToString(", ") { it.text.toString() }
    if (hasHeader && index == 0) "Header: $label" else label
  }
  val accessibilityText: String = accessibilityRows.joinToString("\n")
}

/**
 * A document rendered to segments at one font scale. The same instance
 * measures and lays out, so both agree. Committed segments are shared with
 * earlier snapshots of the same stream. Mirrors src/ios/DSRenderedDocument.mm.
 */
internal class RenderedDocument(
  val documentId: Long,
  val segments: List<Segment>,
  val paint: TextPaint,
  /** Paints and sizes for one font scale and appearance, shared by the frames. */
  val style: Style,
) {
  private var cachedWidth = -1
  private var cachedChrome = -1
  private var cachedCodeBlockWrap = false
  private var cachedWrapToggles = IntArray(0)
  private var cachedFrames = IntArray(0)

  fun createTextLayout(segment: TextSegment, width: Int): StaticLayout =
    buildLayout(segment.text, paint, width.coerceAtLeast(1), Layout.Alignment.ALIGN_NORMAL)

  /**
   * The height of one segment at a view width, including its control row.
   *
   * A code block's own layout comes from the reader's wrap choices rather than
   * from `chrome`, because a document can hold wrapped and unwrapped code at
   * once and `chrome` says only whether every block wraps. The decision is the
   * one in `Controls.h`, mirrored by [codeWraps] here, so a block's measured
   * height and its drawn height cannot disagree.
   */
  private fun heightOf(
    segment: Segment,
    width: Int,
    chrome: Int,
    codeBlockWrap: Boolean,
    wrapToggles: IntArray,
  ): Int = when (segment) {
    is TextSegment -> segment.height(width, paint)
    is CodeSegment ->
      2 * segment.padding + segment.bodyHeight(paint, width, codeWraps(codeBlockWrap, wrapToggles, segment.codeOrdinal)) +
        (if (chrome and CHROME_CODE_HEADER != 0) style.chromeRowHeight else 0)
    is TableSegment ->
      segment.tableHeight + (if (chrome and CHROME_TABLE_TOOLBAR != 0) style.chromeRowHeight else 0)
  }

  /**
   * Top and height of each segment at a width, as [top0, height0, top1, ...].
   *
   * `chrome` and the reader's wrap choices are part of the key rather than the
   * rendering, so a change to either restacks frames without rebuilding any
   * spans. A measurement only holds for the choices it was made under.
   */
  @Synchronized
  fun frames(
    width: Int,
    chrome: Int = 0,
    codeBlockWrap: Boolean = false,
    wrapToggles: IntArray = IntArray(0),
  ): IntArray {
    if (width == cachedWidth && chrome == cachedChrome && codeBlockWrap == cachedCodeBlockWrap &&
      wrapToggles.contentEquals(cachedWrapToggles)
    ) {
      return cachedFrames
    }
    val frames = IntArray(segments.size * 2)
    var y = 0
    segments.forEachIndexed { index, segment ->
      y += segment.gapBefore
      frames[index * 2] = y
      frames[index * 2 + 1] = heightOf(segment, width, chrome, codeBlockWrap, wrapToggles)
      y += frames[index * 2 + 1]
    }
    cachedWidth = width
    cachedChrome = chrome
    cachedCodeBlockWrap = codeBlockWrap
    cachedWrapToggles = wrapToggles
    cachedFrames = frames
    return frames
  }

  fun height(
    width: Int,
    chrome: Int = 0,
    codeBlockWrap: Boolean = false,
    wrapToggles: IntArray = IntArray(0),
  ): Int {
    val frames = frames(width, chrome, codeBlockWrap, wrapToggles)
    return if (frames.isEmpty()) 0 else frames[frames.size - 2] + frames[frames.size - 1]
  }

  /**
   * Width for an unconstrained measure: the widest thing that does not fit a
   * view, which is a table's columns or a code line that scrolls sideways.
   */
  fun desiredWidth(chrome: Int = 0, codeBlockWrap: Boolean = false, wrapToggles: IntArray = IntArray(0)): Int {
    var width = 0f
    for (segment in segments) {
      val candidate = when (segment) {
        is TextSegment -> Layout.getDesiredWidth(segment.text, paint)
        is CodeSegment -> (
          segment.contentLeading + 2 * segment.padding +
            segment.bodyWidth(
              paint,
              UNCONSTRAINED_WIDTH,
              codeWraps(codeBlockWrap, wrapToggles, segment.codeOrdinal),
            )
          ).toFloat()
        is TableSegment -> (segment.contentLeading + segment.tableWidth).toFloat()
      }
      if (candidate > width) {
        width = candidate
      }
    }
    return ceil(min(width, MAX_WIDTH)).toInt()
  }

  companion object {
    private const val MAX_WIDTH = 100_000f
    /** The card width an unconstrained measure passes to a code segment. */
    private const val UNCONSTRAINED_WIDTH = 100_000
    /// Which control rows a document lays out with. See `Controls.h`.
    const val CHROME_CODE_HEADER = 1
    const val CHROME_TABLE_TOOLBAR = 2
    const val CHROME_CODE_WRAP = 4
    private const val BODY_SIZE = 16f
    private const val CODE_SIZE = 14f
    private val HEADING_SIZES = floatArrayOf(28f, 24f, 20f, 18f, 16f, 15f)
    private const val GAP_NORMAL = 12f
    private const val GAP_HEADING = 20f
    private const val GAP_TIGHT = 4f
    private const val QUOTE_INDENT = 16f
    private const val LIST_INDENT = 32f
    private const val CODE_PADDING = 10f
    private const val CHROME_ROW_HEIGHT_DP = 32f
    private const val MIN_COLUMN_WIDTH = 56f
    private const val MAX_COLUMN_WIDTH = 280f
    private const val CELL_PADDING_HORIZONTAL = 12f
    private const val CELL_PADDING_VERTICAL = 8f

    // Worker threads prepare every document in a commit before Fabric
    // measures them, so this holds at least a screenful of messages.
    private val cache = LruCache<String, RenderedDocument>(128)

    /**
     * The layout of the code block at [ordinal]: laid out at the card's width,
     * so long lines wrap, or at its own length, so they scroll sideways.
     *
     * [wrapToggles] holds the ordinals whose layout differs from
     * [codeBlockWrap] in ascending order, so the choice is one membership test.
     * This mirrors `downstream::codeWraps` in `Controls.h` exactly, and
     * `testWrapChoices` in `src/cpp/tests/test_export.cpp` is the table of cases
     * both sides have to agree on.
     */
    @JvmStatic
    fun codeWraps(codeBlockWrap: Boolean, wrapToggles: IntArray, ordinal: Int): Boolean =
      (wrapToggles.binarySearch(ordinal) >= 0) != codeBlockWrap

    /**
     * Committed segments by first block identity, block count, font scale,
     * and appearance. Identities are never reused, so a hit is the same
     * content. Sized in approximate bytes.
     */
    private val committedSegments = object : LruCache<String, Segment>(16 * 1024 * 1024) {
      override fun sizeOf(key: String, value: Segment) = when (value) {
        // Precomputed text also keeps a width per character.
        is TextSegment -> value.text.length * 24 + 256
        is CodeSegment -> value.text.length * 24 + 512
        is TableSegment -> value.accessibilityText.length * 32 + 1024
      }
    }

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
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.VANILLA_ICE_CREAM) {
        // The half of the advance-based breaking contract. See the note on
        // TextSegmentView for why both halves are needed.
        builder.setUseBoundsForWidth(false)
      }
      return builder.build()
    }

    /**
     * Returns a cached rendering, or null when the document is gone.
     *
     * `syntaxHighlighting` decides whether code blocks carry token colors.
     * Tokens are always computed by the engine, so this only chooses the
     * rendering and it is part of both cache keys. Measurement passes true:
     * colors do not change metrics, so a measurement is valid either way.
     */
    fun obtain(
      context: Context,
      documentId: Long,
      fontScale: Float,
      syntaxHighlighting: Boolean = true,
    ): RenderedDocument? {
      val night = (context.resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
        Configuration.UI_MODE_NIGHT_YES
      val key = "$documentId:$fontScale:$night:$syntaxHighlighting"
      cache.get(key)?.let { return it }
      val plan = DownstreamNative.readSegmentPlan(documentId) ?: return null
      val style = Style(fontScale, Palette(night))
      val segments = ArrayList<Segment>(plan.size)
      // Each code segment's ordinal is a property of the plan, so a measurement
      // and the view that draws it read the same number: the ordinal is not
      // something the view has to count for itself.
      for (planned in plan) {
        // The plan counted the code blocks, so the ordinal comes from there
        // rather than from a second walk of the plan.
        val ordinal = planned.codeOrdinal
        // Only the uncommitted tail and newly committed chunks cross JNI. The
        // ordinal is in the key because a cached segment is reused only where it
        // would be built identically.
        val segmentKey =
          "${planned.identity}:${planned.to - planned.from}:$ordinal:$fontScale:$night:$syntaxHighlighting"
        val segment = (if (planned.isCommitted) committedSegments.get(segmentKey) else null)
          ?: DownstreamNative.readDocument(documentId, planned.from, planned.to)
            ?.let {
              buildSegment(it, style, planned.from, planned.isCode, ordinal, planned.identity, syntaxHighlighting)
            }
            ?.also { if (planned.isCommitted) committedSegments.put(segmentKey, it) }
          ?: return null
        segments.add(segment)
      }
      return RenderedDocument(documentId, segments, style.paint, style).also { cache.put(key, it) }
    }

  /** Paints and metrics for one font scale and appearance. */
  internal class Style(fontScale: Float, val colors: Palette) {
    val scale = if (fontScale > 0) fontScale else 1f
    val density = PixelUtil.toPixelFromDIP(1f)
    val paint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
      textSize = px(BODY_SIZE)
      color = colors.text
    }
    val markerPaint = TextPaint(paint).apply { color = colors.secondary }
    /// The row a code block's buttons sit in, and a table's. Fixed in both
    /// directions so a block does not move when a button appears.
    val chromeRowHeight = px(CHROME_ROW_HEIGHT_DP).toInt()
    val codePadding = px(CODE_PADDING).toInt()

    fun px(dp: Float) = (dp * density * scale)
  }

    /** Builds one planned segment from its flattened blocks. */
    private fun buildSegment(
      document: NativeDocument,
      style: Style,
      from: Int,
      isCode: Boolean,
      codeOrdinal: Int,
      identity: Long,
      syntaxHighlighting: Boolean,
    ): Segment {
      val colors = style.colors
      val density = style.density
      val paint = style.paint
      val markerPaint = style.markerPaint
      fun px(dp: Float) = style.px(dp)

      val text = SpannableStringBuilder(document.text)
      val textLanguage = document.blockLanguage(0)
      val accessibleBlocks = ArrayList<AccessibleBlock>(document.blockCount)
      val isTable = document.blockKind(0) == NativeDocument.KIND_TABLE_ROW
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

      // Style every non-table block in place; a table copies its cells.
      for (block in 0 until document.blockCount) {
        val kind = document.blockKind(block)
        if (kind == NativeDocument.KIND_TABLE_ROW) {
          continue
        }

        val start = document.blockStart(block)
        val end = document.blockEnd(block)
        var taskState = 0
        for (depth in 0 until document.containerCount(block)) {
          if (document.showsMarker(block, depth) && document.taskState(block, depth) != 0) {
            taskState = document.taskState(block, depth)
          }
        }
        accessibleBlocks.add(AccessibleBlock(start, end, kind == NativeDocument.KIND_HEADING, taskState))
        val spanEnd = if (block + 1 < document.blockCount) end + 1 else end
        val previous = block - 1
        val (containers, leading, inQuote) = containersOf(block, previous)

        // A code card is its own segment and draws its own padding, so its
        // block never reserves any here, and the block that follows one starts
        // from the plain gap the document stack adds.
        val gap = if (previous >= 0 && !isCode) baseGap(block) else 0f
        val decoration = if (kind == NativeDocument.KIND_RULE) BlockSpan.DECORATION_RULE else BlockSpan.DECORATION_NONE

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
          document.isAdmonitionTitle(block) -> colors.admonition(document.marker(block, document.containerCount(block) - 1))
          inQuote || (kind == NativeDocument.KIND_HEADING && document.headingLevel(block) == 6) -> colors.secondary
          else -> null
        }
        if (color != null) {
          text.setSpan(ForegroundColorSpan(color), start, end, flags)
        }
        val margin = leading.toInt()
        if (margin > 0) {
          text.setSpan(LeadingMarginSpan.Standard(margin), start, spanEnd, Spanned.SPAN_PARAGRAPH)
        }
        text.setSpan(
          BlockSpan(gap.toInt(), leading, decoration, containers, markerPaint, colors, density,
            isCode || startsRightToLeft(text, start, end)),
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
          // Table cells have no block spacing, and their layouts draw spans.
          text.setSpan(
            if (isTable) {
              BackgroundColorSpan(colors.codeBackground)
            } else {
              InlineCodeBackgroundSpan(colors.codeBackground, metrics.ascent, metrics.descent)
            },
            start,
            end,
            flags,
          )
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

      // Tokens are offsets into the block's own text and only set a color, so
      // selection, copy, and the accessibility text are unchanged. Plain text
      // has no run, which is why the engine omits it.
      if (syntaxHighlighting) {
        for (token in 0 until document.tokenCount) {
          val tokenClass = document.tokenClass(token)
          if (tokenClass == NativeDocument.TOKEN_PLAIN) continue
          text.setSpan(
            ForegroundColorSpan(colors.token(tokenClass)),
            document.tokenStart(token),
            document.tokenEnd(token),
            flags,
          )
        }
      }

      // A text segment that a committed chunk split off gets the gap it would
      // have inside one segment, so both stack to the same height.
      val gapBefore = if (from == 0) 0 else baseGap(0).toInt()
      if (isTable) {
        return buildTable(
          document, text, 0, document.blockCount, paint, markerPaint, colors, density, ::px, containersOf(0, -1),
          gapBefore, identity, from,
        )
      }
      val displayText = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
        PrecomputedTexts.create(text, paint)
      } else {
        text
      }
      if (isCode) {
        return buildCode(
          text,
          textLanguage,
          displayText,
          accessibleBlocks,
          style,
          containersOf(0, -1),
          from,
          codeOrdinal,
          gapBefore,
        )
      }
      return TextSegment(displayText, accessibleBlocks, gapBefore)
    }

    /**
     * A code card. The block is laid out left to right however its own
     * characters read, as in Streamdown, and the card keeps the container margin
     * beside the body rather than inside it.
     *
     * `codeOrdinal` is the block's ordinal among the document's code blocks,
     * which a reader's wrap choice is keyed by. It comes from the plan, which
     * counts the code blocks as it is built, because a measurement needs it and
     * a measurement can run before any view has walked the segments.
     */
    private fun buildCode(
      text: SpannableStringBuilder,
      language: String,
      displayText: Spanned,
      accessibleBlocks: List<AccessibleBlock>,
      style: Style,
      containers: Triple<List<ContainerDecoration>, Float, Boolean>,
      blockIndex: Int,
      codeOrdinal: Int,
      gapBefore: Int,
    ): CodeSegment {
      val newline = text.indexOfFirst { it == '\n' }
      val firstLine = text.substring(0, if (newline < 0) text.length else newline).toString()
      return CodeSegment(
        text = displayText,
        accessibleBlocks = accessibleBlocks,
        language = language,
        blockIndex = blockIndex,
        codeOrdinal = codeOrdinal,
        containers = containers.first,
        contentLeading = containers.second.toInt(),
        firstLine = firstLine,
        padding = style.codePadding,
        colors = style.colors,
        gapBefore = gapBefore,
      )
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
      gapBefore: Int,
      identity: Long,
      blockIndex: Int,
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
        rtl = startsRightToLeft(text, document.blockStart(from), document.blockEnd(from)),
        identity = identity,
        blockIndex = blockIndex,
        blockCount = to - from,
        gapBefore = gapBefore,
      )
    }
  }
}

/**
 * Glyph measurement for text segments, done where the segment is built: on a
 * worker thread when the document was prepared, otherwise during Fabric
 * measurement. Parameters match [RenderedDocument.buildLayout] and
 * TextSegmentView, so both reuse it.
 */
@RequiresApi(Build.VERSION_CODES.P)
internal object PrecomputedTexts {
  fun create(text: Spanned, paint: TextPaint): Spanned = PrecomputedText.create(
    text,
    PrecomputedText.Params.Builder(paint)
      .setBreakStrategy(Layout.BREAK_STRATEGY_HIGH_QUALITY)
      .setHyphenationFrequency(Layout.HYPHENATION_FREQUENCY_NONE)
      .setTextDirection(TextDirectionHeuristics.FIRSTSTRONG_LTR)
      .build(),
  )

  /**
   * Shows precomputed text in a TextView that is not selectable. Returns
   * false when the text is not precomputed or the view's text settings do not
   * match, and the caller sets it as ordinary text.
   */
  fun show(view: TextView, text: CharSequence, color: Int): Boolean {
    if (text !is PrecomputedText || view.isTextSelectable) {
      return false
    }
    view.textMetricsParams = text.params
    view.setTextColor(color)
    return try {
      view.text = text
      true
    } catch (error: IllegalArgumentException) {
      false
    }
  }
}

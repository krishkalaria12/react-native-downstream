package com.downstream

import android.content.Context
import android.content.res.Configuration
import android.graphics.Paint
import android.graphics.Typeface
import android.os.Build
import android.text.Layout
import android.text.PrecomputedText
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.SpannedString
import android.text.StaticLayout
import android.text.TextDirectionHeuristics
import android.text.TextPaint
import android.text.style.AbsoluteSizeSpan
import android.text.style.BackgroundColorSpan
import android.text.style.ForegroundColorSpan
import android.text.style.LeadingMarginSpan
import android.text.style.RelativeSizeSpan
import android.text.style.StrikethroughSpan
import android.text.style.TypefaceSpan
import android.util.LruCache
import android.widget.TextView
import androidx.annotation.RequiresApi
import com.facebook.react.uimanager.PixelUtil
import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * A document rendered to segments at one font scale. The same instance
 * measures and lays out, so both agree. Committed segments are shared with
 * earlier snapshots of the same stream. Mirrors src/ios/DSRenderedDocument.mm.
 */
internal class RenderedDocument(
  val documentId: Long,
  val segments: List<Segment>,
  /** The immutable plan already read while building this snapshot. */
  val plan: List<PlannedSegment>,
  val paint: TextPaint,
  /** Paints and sizes for one font scale and appearance, shared by the frames. */
  val style: Style,
) {
  private var cachedWidth = -1
  private var cachedChrome = -1
  private var cachedCode: CodeLayout? = null
  private var cachedImages: ImageLayout? = null
  private var cachedHosts: HostLayout? = null
  private var cachedFrames = IntArray(0)

  fun createTextLayout(segment: TextSegment, width: Int): StaticLayout =
    buildLayout(segment.text, paint, width.coerceAtLeast(1), Layout.Alignment.ALIGN_NORMAL)

  /**
   * The height of one segment at a view width, including its control row.
   *
   * A code block's own layout comes from the whole code layout rather than from
   * `chrome`, because a document can hold wrapped and unwrapped code at once and
   * `chrome` says only whether every block wraps.
   */
  private fun heightOf(
    segment: Segment,
    width: Int,
    chrome: Int,
    code: CodeLayout,
    images: ImageLayout,
    hosts: HostLayout,
  ): Int =
    when (segment) {
      is TextSegment -> segment.height(width, paint)
      // A picture's box is the state and the theme, and nothing else: there is no
      // text to lay out and the bytes are not here. The same `imageBox` the
      // shadow node measured with, or the two disagree by the height of a
      // placeholder and the whole list jumps on the first frame.
      is ImageSegment -> {
        val metrics = style.imageMetrics
        val box = segment.box(width, images.state.find(segment.ordinal), metrics)
        ceil(box.height.toDouble()).toInt() +
          ceil(metrics.marginTop.toDouble()).toInt() + ceil(metrics.marginBottom.toDouble()).toInt()
      }
      // The engine's own `mathBlockHeight`, mirrored. An unresolved formula takes
      // one line of text, which is what its literal takes, so a formula that
      // cannot be laid out reads rather than vanishing.
      is MathSegment ->
        if (segment.resolved) {
          (mathBlockHeight(segment.height, segment.depth, style.mathFontSize, style.mathPadding) +
            style.mathMarginTop + style.mathMarginBottom).toInt()
        } else {
          // An unresolved formula takes one line, which is what its literal takes,
          // so a formula that cannot be laid out still occupies readable space.
          (style.bodySize * LINE_HEIGHT + style.mathPadding * 2 +
            style.mathMarginTop + style.mathMarginBottom).toInt()
        }
      // A hosted fence is as tall as the React child that replaced it, as on iOS:
      // the component owns its height, and the block's margins stay a code
      // block's, so the space around it does not change when a fence becomes a
      // chart. The height arrives in pixels, converted where the map is read.
      is CodeSegment -> {
        val hosted = hosts.find(segment.codeOrdinal)
        if (hosted != null) {
          ceil(hosted.height.toDouble()).toInt()
        } else {
          // The one definition: the props, the reader's choices, and this block's
          // own fence meta all meet in `resolveCodeBlock`, and `codeCardHeight` is
          // the only function that turns that into a height. The view calls the
          // same two, which is what keeps a measured card and a drawn card the same
          // size.
          val layout = resolveCodeBlock(code, segment.codeOrdinal, segment.fenceMeta)
          val gutter = segment.gutterWidth(layout)
          val body = segment.bodyHeight(
            paint, width, layout.wraps, gutter,
          )
          codeCardHeight(
            layout,
            if (chrome and CHROME_CODE_HEADER != 0) style.chromeRowHeight else 0,
            segment.padding,
            body,
            segment.lineHeight(),
          )
        }
      }
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
    code: CodeLayout = CodeLayout(),
    images: ImageLayout = ImageLayout(),
    hosts: HostLayout = HostLayout(),
  ): IntArray {
    // A measurement only holds for the layout it was made under, so the whole code
    // layout, the whole image layout, and the hosted heights are all part of the
    // key rather than the rendering. An image's height is its state and a hosted
    // fence's height is its component's, so a picture that loads or a chart that
    // changes size has to restack the frames or the text under it stays where the
    // old box was.
    if (
      width == cachedWidth && chrome == cachedChrome && code == cachedCode &&
      images == cachedImages && hosts == cachedHosts
    ) {
      return cachedFrames
    }
    val frames = IntArray(segments.size * 2)
    var y = 0
    segments.forEachIndexed { index, segment ->
      y += segment.gapBefore
      frames[index * 2] = y
      frames[index * 2 + 1] = heightOf(segment, width, chrome, code, images, hosts)
      y += frames[index * 2 + 1]
    }
    cachedWidth = width
    cachedChrome = chrome
    cachedCode = code
    cachedImages = images
    cachedHosts = hosts
    cachedFrames = frames
    return frames
  }

  fun height(
    width: Int,
    chrome: Int = 0,
    code: CodeLayout = CodeLayout(),
    images: ImageLayout = ImageLayout(),
    hosts: HostLayout = HostLayout(),
  ): Int {
    val frames = frames(width, chrome, code, images, hosts)
    return if (frames.isEmpty()) 0 else frames[frames.size - 2] + frames[frames.size - 1]
  }

  /**
   * The content rectangle of every hosted block, as `[ordinal, left, top, right,
   * bottom]` groups, in the document's own coordinates.
   *
   * Read by `DownstreamNative.hostFrames` for the measurement path, because
   * `FabricUIManager.measure` returns a size and nothing else: only `measureText`
   * reports attachment positions, and a hosted child is not text. The rectangles
   * come from the frames just stacked, so the shadow node places a child where
   * this rendering puts it.
   */
  @Synchronized
  fun hostFrames(
    width: Int,
    chrome: Int = 0,
    code: CodeLayout = CodeLayout(),
    images: ImageLayout = ImageLayout(),
    hosts: HostLayout = HostLayout(),
  ): IntArray {
    if (hosts.isEmpty()) {
      return IntArray(0)
    }
    val frames = frames(width, chrome, code, images, hosts)
    val out = IntArray(hosts.entries.size * 5)
    var count = 0
    segments.forEachIndexed { index, segment ->
      if (segment !is CodeSegment || !hosts.hosts(segment.codeOrdinal)) {
        return@forEachIndexed
      }
      val top = frames[index * 2]
      val height = frames[index * 2 + 1]
      val leading = segment.contentLeading
      val column = max(0, width - leading)
      // A right-to-left block mirrors its indent to the trailing edge, the same
      // way the container bars and the list marker are mirrored.
      val left = if (segment.rtl) width - leading - column else leading
      out[count++] = segment.codeOrdinal
      out[count++] = left
      out[count++] = top
      out[count++] = left + column
      out[count++] = top + height
    }
    // A streamed replacement can remove fences before their host state arrives.
    return if (count == out.size) out else out.copyOf(count)
  }

  /**
   * Width for an unconstrained measure: the widest thing that does not fit a
   * view, which is a table's columns or a code line that scrolls sideways.
   */
  fun desiredWidth(code: CodeLayout = CodeLayout(), images: ImageLayout = ImageLayout()): Int {
    var width = 0f
    for (segment in segments) {
      val candidate = when (segment) {
        is TextSegment -> Layout.getDesiredWidth(segment.text, paint)
        // An image is never wider than its column and never asks for one: a
        // picture that wanted 4,000 pixels of width would make every other
        // segment in the message measure against it.
        is ImageSegment -> style.imageMetrics.placeholderWidth
        is MathSegment -> segment.width * style.mathFontSize
        is CodeSegment -> {
          val layout = resolveCodeBlock(code, segment.codeOrdinal, segment.fenceMeta)
          val gutter = segment.gutterWidth(layout)
          (
            segment.contentLeading + 2 * segment.padding + gutter +
              segment.bodyWidth(paint, UNCONSTRAINED_WIDTH, layout.wraps, gutter)
            ).toFloat()
        }
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
    /**
     * The line height a one-line fallback takes, as a multiple of the font size.
     *
     * Android's own default for a body text view, which is what an unresolved
     * formula's literal is measured against so it occupies the room a line of prose
     * would. Not read from a slot: the formula's literal is body text, so it takes
     * the body's line height rather than a math-specific one.
     */
    const val LINE_HEIGHT = 1.2f

    /// Which control rows a document lays out with. See `Controls.h`.
    const val CHROME_CODE_HEADER = 1
    const val CHROME_TABLE_TOOLBAR = 2
    const val CHROME_CODE_WRAP = 4

    // Worker threads prepare every document in a commit before Fabric
    // measures them, so this holds at least a screenful of messages.
    private val cache = RenderingCache<String, RenderedDocument>(128, 8L * 1024 * 1024) { rendered ->
      // Plans, segment references and cached frames belong to each snapshot.
      // Omit shared committed payloads from this tail/metadata estimate. A
      // rendering can retain them after the separate segment cache evicts them.
      var cost = 1024L + rendered.plan.size * 128L
      rendered.plan.forEachIndexed { index, planned ->
        if (!planned.isCommitted) cost += segmentCost(rendered.segments[index])
      }
      cost
    }

    private fun segmentCost(value: Segment): Long = when (value) {
      // Precomputed text also keeps a width per character.
      is TextSegment -> value.text.length * 24L + 256
      is CodeSegment -> value.text.length * 24L + 512
      is TableSegment -> value.accessibilityText.length * 32L + 1024
      // Picture bitmaps belong to the image pipeline's cache.
      is ImageSegment -> 512L
      is MathSegment -> 512L
    }

    /**
     * Committed segments by first block identity, block count, font scale,
     * and appearance. Identities are never reused, so a hit is the same
     * content. Sized in approximate bytes.
     */
    private val committedSegments = object : LruCache<String, Segment>(16 * 1024 * 1024) {
      override fun sizeOf(key: String, value: Segment) =
        segmentCost(value).coerceAtMost(16L * 1024 * 1024 + 1).toInt()
    }

    private data class PreparedTableCell(val text: Spanned, val desiredWidth: Float)
    private data class TableCellLayoutKey(val content: TableCellKey, val width: Int)

    // A growing table rebuilds its grid but can reuse unchanged plain cells.
    // Keep the text immutable and bound both caches independently. Inline
    // spans (links, formulas, scripts, emphasis) keep the uncached path.
    private const val TABLE_CELL_CACHE_COST = 2L * 1024 * 1024
    private val tableCellTexts = RenderingCache<TableCellKey, PreparedTableCell>(1024, TABLE_CELL_CACHE_COST) {
      it.text.length * 24L + 256
    }
    private val tableCellLayouts = RenderingCache<TableCellLayoutKey, StaticLayout>(1024, TABLE_CELL_CACHE_COST) {
      it.text.length * 24L + 256
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
     *
     * `themeId` is the interned theme the caller draws with. It is in both keys
     * so a themed rendering is never handed to an unthemed view, and it resolves
     * the theme's cap on the platform font scale here rather than at every draw.
     *
     * `caret` is already resolved against the document, and it is in the keys for
     * the same reason the chrome is: its placeholder is a real character in the
     * measured text, so a rendering built without one and a rendering built with
     * one have different heights. Only the last segment changes, so a pause
     * between blocks builds one variant rather than the whole document.
     */
    fun obtain(
      context: Context,
      documentId: Long,
      fontScale: Float,
      syntaxHighlighting: Boolean = true,
      themeId: Long = 0L,
      caret: PlacedCaret = PlacedCaret.NONE,
    ): RenderedDocument? {
      val night = (context.resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
        Configuration.UI_MODE_NIGHT_YES
      val key = "$documentId:$fontScale:$night:$syntaxHighlighting:$themeId:${caret.kind}:${caret.place}"
      cache.get(key)?.let { return it }
      // Inline formulas draw from text spans, which have no context of their own.
      MathFonts.attach(context)
      val plan = DownstreamNative.readSegmentPlan(documentId) ?: return null
      // The document's pictures, read once. An image segment needs its destination,
      // alt text and policy answer, and reading those one field at a time over JNI
      // would be one crossing per field per picture; this is one for the document.
      // A document with no pictures is ordinary, so an empty list is an answer.
      val images = DownstreamNative.readImages(documentId) ?: NativeImages.EMPTY
      // A theme can cap the platform's font scale but cannot raise it, and the
      // cap is applied once here so every measurement below works with the
      // result. Resolved in C++ so both platforms read one rule.
      val scaled = DownstreamNative.fontScaleCap(themeId, fontScale)
      val theme = NativeTheme.of(themeId)
      // One style for the shared scale and the paints, which is what a
      // measurement reads; a segment builds its own for the per-block work that
      // needs the document.
      val style = Style(scaled, Palette(theme, night), theme, themeId)
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
        val isLast = planned.to == plan.last().to
        // Only the document's own last segment carries the caret, so the key only
        // varies there and every earlier segment keeps being shared between
        // snapshots.
        val caretKey = if (isLast) "${caret.kind}:${caret.place}" else "0:0"
        val segmentKey =
          "${planned.identity}:${planned.to - planned.from}:$ordinal:$scaled:$night:" +
            "$syntaxHighlighting:$themeId:$caretKey"
        val segment = (if (planned.isCommitted) committedSegments.get(segmentKey) else null)
          // The theme id goes with the read, because each block's typography is
          // resolved against it in C++; without it every block reads the default.
          ?: DownstreamNative.readDocument(documentId, planned.from, planned.to, themeId)
            ?.let {
              buildSegment(
                it, images, style, planned, ordinal, syntaxHighlighting, style.px(it.previousBottomMargin),
                if (isLast) caret else PlacedCaret.NONE,
              )
            }
            ?.also { if (planned.isCommitted) committedSegments.put(segmentKey, it) }
          ?: return null
        segments.add(segment)
      }
      return RenderedDocument(documentId, segments, plan, style.paint, style).also { cache.put(key, it) }
    }

  /**
   * Paints and metrics for one theme at one font scale in one appearance.
   *
   * Every metric is read from a slot rather than named here, so the two
   * renderers cannot disagree about what a theme means: the values come from the
   * engine's own table over one JNI crossing, and this file only scales them.
   */
  internal class Style(fontScale: Float, val colors: Palette, val theme: NativeTheme, val themeId: Long = 0L) {
    val scale = if (fontScale > 0) fontScale else 1f
    val density = PixelUtil.toPixelFromDIP(1f)

    /// The row a code block's buttons sit in, and a table's. A touch target
    /// rather than a theme setting, so it is fixed in both directions and a block
    /// does not move when a button appears.
    val chromeRowHeight = px(32f).toInt()

    /**
     * The theme's image metrics, in pixels.
     *
     * Resolved in C++ by `theme_layout::imageMetrics`, which is the same call the
     * shadow node measures with, and read here once per snapshot rather than per
     * measurement: the theme does not change under a snapshot, and a second reader
     * is how a measured image and a drawn one disagree about a margin.
     *
     * The placeholder keeps its default size rather than taking a slot: it is
     * Streamdown's `h-24 w-48` box, which is a reading affordance rather than a
     * themed style, and a theme that named no slot should not silently restyle it.
     */
    val imageMetrics = run {
      val resolved = DownstreamNative.imageMetrics(themeId)
      resolved.copy(
        maxHeight = px(resolved.maxHeight),
        borderRadius = px(resolved.borderRadius),
        marginTop = px(resolved.marginTop),
        marginBottom = px(resolved.marginBottom),
        fallbackTextHeight = px(resolved.fallbackTextHeight),
        placeholderWidth = px(kImagePlaceholderWidth),
        placeholderHeight = px(kImagePlaceholderHeight),
      )
    }

    val bodySize = px(theme.number(ThemeSlots.SLOT_PARAGRAPH_FONT_SIZE))
    val bodyWeight = theme.number(ThemeSlots.SLOT_PARAGRAPH_FONT_WEIGHT)
    val bodyItalic = theme.flag(ThemeSlots.SLOT_PARAGRAPH_FONT_STYLE)
    val bodyFamily = theme.family(ThemeSlots.SLOT_PARAGRAPH_FONT_FAMILY)

    val paint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
      textSize = bodySize
      color = colors.text
      typeface = theme.slotTypeface(
        ThemeSlots.SLOT_PARAGRAPH_FONT_FAMILY,
        bodyWeight,
        bodyItalic,
      )
    }
    /** A marker's size, from the theme's marker slot. */
    val markerSize = px(theme.number(ThemeSlots.SLOT_LIST_MARKER_FONT_SIZE))
    val markerPaint = TextPaint(paint).apply {
      color = colors.marker
      typeface = theme.slotTypeface(
        ThemeSlots.SLOT_LIST_MARKER_FONT_FAMILY,
        theme.number(ThemeSlots.SLOT_LIST_MARKER_FONT_WEIGHT),
        false,
        true,
      )
    }
    val markerGap = px(theme.number(ThemeSlots.SLOT_LIST_GAP_WIDTH))
    val barWidth = px(theme.number(ThemeSlots.SLOT_BLOCKQUOTE_BORDER_WIDTH))
    val quoteIndent = px(theme.number(ThemeSlots.SLOT_BLOCKQUOTE_GAP_WIDTH))
    val listIndent = px(theme.number(ThemeSlots.SLOT_LIST_MARGIN_LEFT))
    val codePadding = px(theme.number(ThemeSlots.SLOT_CODE_BLOCK_PADDING)).toInt()
    /** The header row's size, which the theme sets for the language label. */
    val codeHeaderSize = px(theme.number(ThemeSlots.SLOT_CODE_BLOCK_HEADER_FONT_SIZE))
    /// A formula's own color, its fill, and its error color, resolved here so the
    /// view and the measurement read one value for each.
    val mathMetrics = DownstreamNative.mathMetrics(themeId)
    val mathPadding = px(mathMetrics.padding)
    val mathTextAlign = mathMetrics.textAlign
    val mathMarginTop = px(mathMetrics.marginTop)
    val mathMarginBottom = px(mathMetrics.marginBottom)
    val mathFontSize = px(theme.number(ThemeSlots.SLOT_MATH_FONT_SIZE))
    val mathColor = colors.slotOrNull(ThemeSlots.SLOT_MATH_COLOR) ?: colors.text
    val mathBackground =
      colors.slotOrNull(ThemeSlots.SLOT_MATH_BACKGROUND_COLOR)
    val mathErrorColor = colors.slot(ThemeSlots.SLOT_MATH_ERROR_COLOR, colors.secondary, colors.secondary)
    /// The radius a formula's fill is drawn at. The code card's own, because a
    /// formula in a message should look like the message's other boxes.
    val mathRadius = px(theme.number(ThemeSlots.SLOT_CODE_BLOCK_BORDER_RADIUS))

    /// The alt text a picture shows and the label a screen reader reads, from the
    /// theme's own slot so a fallback image follows the rest of the page.
    val imageAltColor = colors.slotOrNull(ThemeSlots.SLOT_IMAGE_ALT_COLOR) ?: colors.secondary

    val codeBlockRadius = px(theme.number(ThemeSlots.SLOT_CODE_BLOCK_BORDER_RADIUS))
    val codeBlockBorder = px(theme.number(ThemeSlots.SLOT_CODE_BLOCK_BORDER_WIDTH))
    val minColumnWidth = px(theme.number(ThemeSlots.SLOT_TABLE_MIN_COLUMN_WIDTH))
    val maxColumnWidth = px(theme.number(ThemeSlots.SLOT_TABLE_MAX_COLUMN_WIDTH))
    val cellPaddingHorizontal = px(theme.number(ThemeSlots.SLOT_TABLE_CELL_PADDING_HORIZONTAL)).toInt()
    val cellPaddingVertical = px(theme.number(ThemeSlots.SLOT_TABLE_CELL_PADDING_VERTICAL)).toInt()
    val tableBorder = px(theme.number(ThemeSlots.SLOT_TABLE_BORDER_WIDTH))
    val tableRadius = px(theme.number(ThemeSlots.SLOT_TABLE_BORDER_RADIUS))
    val ruleHeight = px(theme.number(ThemeSlots.SLOT_THEMATIC_BREAK_HEIGHT))
    val footnoteScale = theme.number(ThemeSlots.SLOT_FOOTNOTE_SIZE_SCALE)

    /** A heading level's size, weight, family, slant and color, per level. */
    fun headingSize(level: Int): Float = px(theme.number(headingSlot(level, ThemeSlots.Heading.FONT_SIZE)))

    fun headingWeight(level: Int): Float = theme.number(headingSlot(level, ThemeSlots.Heading.FONT_WEIGHT))

    fun headingFamily(level: Int): String = theme.family(headingSlot(level, ThemeSlots.Heading.FONT_FAMILY))

    fun headingItalic(level: Int): Boolean = theme.flag(headingSlot(level, ThemeSlots.Heading.FONT_STYLE))

    fun headingColorSlot(level: Int): Int = headingSlot(level, ThemeSlots.Heading.COLOR)

    /**
     * Which margin slots a block draws with.
     *
     * A quote and a list own their own text, so their margins replace the
     * paragraph's rather than adding to them, which is why a block inside a
     * quote is not a paragraph with an indent.
     */
    fun marginTopSlot(document: NativeDocument, block: Int): Int = document.blockMarginTop(block)

    /** Which bottom margin a block leaves behind, resolved by ThemeLayout. */
    fun marginBottomSlot(document: NativeDocument, block: Int): Int = document.blockMarginBottom(block)

    /**
     * The gap above a block, from its own top margin and the bottom margin of
     * the block above it.
     *
     * `previousBottom` is that bottom margin in points, which is what lets the
     * rule work across a segment boundary: a segment sees only its own blocks,
     * so the document read carries the preceding margin with the slice.
     */
    fun gapAbove(document: NativeDocument, block: Int, previousBottom: Float): Float {
      val spacing = document.spacingBefore(block)
      if (spacing == NativeDocument.SPACING_NONE) {
        return 0f
      }
      if (spacing == NativeDocument.SPACING_TIGHT) {
        // Another line of the same item, so the list's own item spacing.
        return px(theme.number(ThemeSlots.SLOT_LIST_ITEM_SPACING))
      }
      // The two margins collapse, as CSS collapses them, so a theme that gives
      // every block a top margin does not also give every pair of blocks two.
      return maxOf(px(theme.number(marginTopSlot(document, block))), previousBottom)
    }

    /** As above, for a block inside this segment. */
    fun gapAbove(document: NativeDocument, block: Int, previous: Int): Float {
      val spacing = document.spacingBefore(block)
      if (spacing == NativeDocument.SPACING_NONE) {
        return 0f
      }
      if (spacing == NativeDocument.SPACING_TIGHT) {
        // Another line of the same item, so the list's own item spacing.
        return px(theme.number(ThemeSlots.SLOT_LIST_ITEM_SPACING))
      }
      // The two margins collapse, as CSS collapses them, so a theme that gives
      // every block a top margin does not also give every pair two.
      val above = if (previous >= 0) {
        px(theme.number(marginBottomSlot(document, previous)))
      } else {
        0f
      }
      return maxOf(px(theme.number(marginTopSlot(document, block))), above)
    }

    /** Level n's slot for one property, level n at index n - 1. */
    private fun headingSlot(level: Int, slots: IntArray): Int =
      slots[level.coerceIn(1, 6) - 1]

    fun px(dp: Float) = (dp * density * scale)
  }

    /** Builds one planned segment from its flattened blocks. */
    private fun buildSegment(
      document: NativeDocument,
      images: NativeImages,
      style: Style,
      planned: PlannedSegment,
      codeOrdinal: Int,
      syntaxHighlighting: Boolean,
      previousBottom: Float,
      caret: PlacedCaret = PlacedCaret.NONE,
    ): Segment {
      val from = planned.from
      val isCode = planned.kind == SEGMENT_CODE
      val identity = planned.identity
      val colors = style.colors
      val theme = style.theme
      val density = style.density
      val paint = style.paint
      val markerPaint = style.markerPaint
      fun px(dp: Float) = style.px(dp)

      // A formula or a picture is one block that the plan kept to itself, so it is
      // built here rather than falling through to the text path. Both keep their
      // literal: a formula that could not be laid out and a picture that could not
      // be fetched are both still readable as the writer's own source, which is the
      // whole of what this branch decides.
      if (planned.kind == SEGMENT_MATH) {
        return buildMath(document, style, style.gapAbove(document, 0, previousBottom).toInt())
      }
      if (planned.kind == SEGMENT_IMAGE) {
        // Null when the plan's ordinal is not in the document's picture list, in
        // which case the block's own text is drawn instead: a plan and a document
        // that disagree is worth reading, not worth crashing over.
        buildImage(images, planned.imageOrdinal, style.gapAbove(document, 0, previousBottom).toInt())
          ?.let { return it }
      }

      val text = SpannableStringBuilder(document.text)
      val textLanguage = document.blockLanguage(0)
      val accessibleBlocks = ArrayList<AccessibleBlock>(document.blockCount)
      val isTable = document.blockKind(0) == NativeDocument.KIND_TABLE_ROW
      val flags = Spanned.SPAN_EXCLUSIVE_EXCLUSIVE

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
            containers.add(
              ContainerDecoration(leading, style.quoteIndent, bar, continues, null, style.barWidth, style.markerGap),
            )
            leading += style.quoteIndent
          } else {
            val task = document.taskState(block, depth)
            val marker = if (document.showsMarker(block, depth)) {
              when (task) {
                NativeDocument.TASK_UNCHECKED -> "\u2610"
                NativeDocument.TASK_CHECKED -> "\u2611"
                else -> document.marker(block, depth)
              }
            } else {
              null
            }
            // A task's box takes its own two colors, and anything else takes the
            // bullet's slot, so a theme can make numbers quieter than bullets.
            val markerColor = when (task) {
              NativeDocument.TASK_UNCHECKED -> colors.slot(ThemeSlots.SLOT_TASK_LIST_BORDER_COLOR, colors.marker, colors.marker)
              NativeDocument.TASK_CHECKED -> colors.slot(ThemeSlots.SLOT_TASK_LIST_CHECKED_COLOR, colors.marker, colors.marker)
              else -> colors.slotOrNull(ThemeSlots.SLOT_LIST_BULLET_COLOR) ?: colors.marker
            }
            containers.add(
              ContainerDecoration(
                leading, style.listIndent, null, continues, marker, style.barWidth, style.markerGap,
                if (marker != null) markerColor else null,
              ),
            )
            leading += style.listIndent
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
        val gap = if (previous >= 0 && !isCode) style.gapAbove(document, block, previous) else 0f
        val decoration = if (kind == NativeDocument.KIND_RULE) BlockSpan.DECORATION_RULE else BlockSpan.DECORATION_NONE

        // The block's typography comes back resolved from the engine, from the
        // same rules iOS calls, so a theme means one thing rather than one thing
        // per platform. A quote or a list's own slots are already folded in.
        val level = document.headingLevel(block).coerceIn(1, 6)
        val size = px(document.blockFontSize(block))
        val weight = document.blockFontWeight(block)
        val italic = document.blockItalic(block)
        val family = theme.familyAt(document.blockFontFamily(block))
        val colorSlot = document.blockColorSlot(block)
        val monospaced = document.isCodeElement(block)
        text.setSpan(AbsoluteSizeSpan(size.toInt()), start, spanEnd, flags)
        if (weight != style.bodyWeight || italic != style.bodyItalic || family != style.bodyFamily || monospaced) {
          // A face rather than a style span, because the theme's weight is a
          // number the hundred `Typeface` constants do not cover.
          text.setSpan(
            TypefaceSpan(theme.typeface(family, weight, italic, monospaced)),
            start,
            end,
            flags,
          )
        }
        // A block inside a quote takes the quote's own color unless the theme
        // named a color for the block's element, which is what lets a themed
        // quote restyle the text inside it. An h6 reads as secondary for the
        // same reason: it is the smallest heading, and a theme that named no
        // color for it gets the muted one rather than full-strength body text.
        // The theme is asked for this appearance, so a color named for dark mode
        // only leaves light mode muted.
        val themed = theme.color(colorSlot, colors.night)
        val muted = inQuote || (kind == NativeDocument.KIND_HEADING && level == 6)
        val color = when {
          document.isAdmonitionTitle(block) ->
            colors.admonition(document.marker(block, document.containerCount(block) - 1))
          themed != null -> themed
          muted -> colors.secondary
          else -> null
        }
        if (color != null) {
          text.setSpan(ForegroundColorSpan(color), start, end, flags)
        }
        val margin = leading.toInt()
        if (margin > 0) {
          text.setSpan(LeadingMarginSpan.Standard(margin), start, spanEnd, Spanned.SPAN_PARAGRAPH)
        }
        val lineHeight = document.blockLineHeight(block)
        if (lineHeight > 0f) {
          text.setSpan(BlockLineHeightSpan(px(lineHeight).toInt()), start, spanEnd, Spanned.SPAN_PARAGRAPH)
        }
        text.setSpan(
          BlockSpan(gap.toInt(), leading, decoration, containers, markerPaint, colors, density,
            isCode || startsRightToLeft(text, start, end), style.ruleHeight),
          start,
          spanEnd,
          Spanned.SPAN_PARAGRAPH,
        )
      }

      // Blocks whose inline formula has a layout, whose box its line must hold.
      val formulaBlocks = document.formulas.filter { it.list != null }.mapTo(HashSet()) { it.block }
      // Scripts and inline code use the block's size before their own size spans.
      // Read the same integer size written above instead of searching the growing
      // span tree for every run. Table rows have no block size span.
      var metricBlock = -1
      var metricPaint = paint
      var metricAscent = 0
      var metricDescent = 0
      fun blockPaintFor(block: Int): TextPaint {
        if (metricBlock != block) {
          metricBlock = block
          metricPaint = TextPaint(paint).apply {
            if (document.blockKind(block) != NativeDocument.KIND_TABLE_ROW) {
              textSize = px(document.blockFontSize(block)).toInt().toFloat()
            }
          }
          val metrics = metricPaint.fontMetricsInt
          metricAscent = metrics.ascent
          metricDescent = metrics.descent
        }
        return metricPaint
      }
      for (run in 0 until document.runCount) {
        val styles = document.runStyles(run)
        val link = document.runLink(run)
        val start = document.runStart(run)
        val end = document.runEnd(run)
        val bold = styles and NativeDocument.STYLE_STRONG != 0
        val italic = styles and NativeDocument.STYLE_EMPHASIS != 0
        val isCode = styles and NativeDocument.STYLE_CODE != 0
        val linked = link >= 0
        // The run's face starts as the block's own and takes the inline kind's
        // slots on top, so a `strong` in a themed heading is a heavier heading
        // rather than a heading that drops back to the body face. A face rather
        // than a style span, because a theme's weight is a number the hundred
        // `Typeface` constants do not cover.
        val blockIndex = document.blockOf(start)
        val blockWeight = document.blockFontWeight(blockIndex)
        val blockItalic = document.blockItalic(blockIndex)
        val blockFamily = theme.familyAt(document.blockFontFamily(blockIndex))
        val blockMonospaced = document.isCodeElement(blockIndex) && blockFamily.isEmpty()
        var runWeight = blockWeight
        var runItalic = blockItalic
        var runFamily = blockFamily
        var runMonospaced = blockMonospaced
        if (bold) {
          runFamily = theme.family(ThemeSlots.SLOT_STRONG_FONT_FAMILY).ifEmpty { runFamily }
          // A theme that named a weight means it; one that named only a family
          // gets the block's weight raised, so bold in bold still reads.
          runWeight = if (theme.numberSet(ThemeSlots.SLOT_STRONG_FONT_WEIGHT)) {
            theme.number(ThemeSlots.SLOT_STRONG_FONT_WEIGHT)
          } else {
            minOf(900f, runWeight + if (runWeight >= 600f) 200f else 300f)
          }
        }
        if (italic) {
          runFamily = theme.family(ThemeSlots.SLOT_EM_FONT_FAMILY).ifEmpty { runFamily }
          if (theme.flag(ThemeSlots.SLOT_EM_FONT_STYLE)) {
            runItalic = true
          }
        }
        if (linked) {
          runFamily = theme.family(ThemeSlots.SLOT_LINK_FONT_FAMILY).ifEmpty { runFamily }
          runWeight = theme.number(ThemeSlots.SLOT_LINK_FONT_WEIGHT)
        }
        if (isCode) {
          runFamily = theme.family(ThemeSlots.SLOT_CODE_FONT_FAMILY)
          runMonospaced = runFamily.isEmpty()
        }
        runMonospaced = runMonospaced && runFamily.isEmpty()
        // Color-only styles inherit the block's face. Table rows skip block
        // spans, so their runs still need a face over the cell's paragraph paint.
        // Elsewhere only add a metric span when the effective face changes.
        if (isTable || runWeight != blockWeight || runItalic != blockItalic ||
          runFamily != blockFamily || runMonospaced != blockMonospaced
        ) {
          text.setSpan(
            TypefaceSpan(
              theme.typeface(runFamily, runWeight, runItalic, runMonospaced),
            ),
            start,
            end,
            flags,
          )
        }

        // A theme can recolor each inline kind on its own. The colors default to
        // "inherit", so an unset one leaves the block's color alone rather than
        // painting over it.
        val inlineColor = when {
          document.isAdmonitionTitle(blockIndex) -> null
          bold -> theme.color(ThemeSlots.SLOT_STRONG_COLOR, colors.night)
          italic -> theme.color(ThemeSlots.SLOT_EM_COLOR, colors.night)
          linked -> theme.color(ThemeSlots.SLOT_LINK_COLOR, colors.night)
          isCode -> theme.color(ThemeSlots.SLOT_CODE_COLOR, colors.night)
          else -> null
        }
        if (inlineColor != null) {
          text.setSpan(ForegroundColorSpan(inlineColor), start, end, flags)
        }
        if (styles and NativeDocument.STYLE_STRIKETHROUGH != 0) {
          val strike = theme.color(ThemeSlots.SLOT_STRIKETHROUGH_COLOR, colors.night)
          text.setSpan(
            // The platform's span when the theme named no color, so an unset
            // slot keeps drawing in the text color it always did.
            if (strike != null) ThemedStrikeSpan(strike) else StrikethroughSpan(),
            start,
            end,
            flags,
          )
        }
        if (isCode) {
          blockPaintFor(blockIndex)
          text.setSpan(
            HeldLineSpan(metricAscent, metricDescent),
            start,
            end,
            flags or (1 shl Spanned.SPAN_PRIORITY_SHIFT),
          )
          // A share of the block's own size, so inline code keeps its
          // proportions when a theme changes the body size.
          text.setSpan(
            RelativeSizeSpan(theme.inlineCodeRatio(px(theme.number(ThemeSlots.SLOT_CODE_FONT_SIZE)))),
            start,
            end,
            flags,
          )
          // Table cells have no block spacing, and their layouts draw spans.
          text.setSpan(
            if (isTable) {
              BackgroundColorSpan(colors.codeBackground)
            } else {
              InlineCodeBackgroundSpan(colors.codeBackground, metricAscent, metricDescent)
            },
            start,
            end,
            flags,
          )
        }
        if (styles and NativeDocument.STYLE_IMAGE_ALT != 0) {
          // Alt text reads as secondary by design, so the theme's own alt color
          // wins over the block's text color.
          text.setSpan(
            ForegroundColorSpan(
              theme.color(ThemeSlots.SLOT_IMAGE_ALT_COLOR, colors.night) ?: colors.secondary,
            ),
            start,
            end,
            flags,
          )
        }
        // A raised or lowered run: a <sup>, a <sub>, or a footnote reference.
        // The size and the shift are both shares of the block's own size, as
        // iOS and the web measure them, so the shift is fixed here rather than
        // read off a paint the size span has already shrunk.
        val superscript = styles and NativeDocument.STYLE_SUPERSCRIPT != 0
        val subscript = styles and NativeDocument.STYLE_SUBSCRIPT != 0
        val footnote = styles and NativeDocument.STYLE_FOOTNOTE != 0
        if (superscript || subscript || footnote) {
          val (sizeScale, raise) = when {
            footnote -> theme.number(ThemeSlots.SLOT_FOOTNOTE_SIZE_SCALE) to theme.number(ThemeSlots.SLOT_FOOTNOTE_RAISE)
            superscript -> theme.number(ThemeSlots.SLOT_SUPERSCRIPT_FONT_SCALE) to
              theme.number(ThemeSlots.SLOT_SUPERSCRIPT_BASELINE_OFFSET_SCALE)
            // A subscript lowers.
            else -> theme.number(ThemeSlots.SLOT_SUBSCRIPT_FONT_SCALE) to
              -theme.number(ThemeSlots.SLOT_SUBSCRIPT_BASELINE_OFFSET_SCALE)
          }
          text.setSpan(RelativeSizeSpan(sizeScale), start, end, flags)
          text.setSpan(BaselineShiftSpan((blockPaintFor(blockIndex).textSize * raise).roundToInt()), start, end, flags)
          // The web's `sub` and `sup` have `line-height: 0`, so a shifted run
          // never opens its line. A formula's box is what its line has to hold,
          // so a block with one keeps its natural height, as on iOS.
          if (!isCode && blockIndex !in formulaBlocks) {
            text.setSpan(
              HeldLineSpan(metricAscent, metricDescent),
              start,
              end,
              flags or (1 shl Spanned.SPAN_PRIORITY_SHIFT),
            )
          }
        }
        if (footnote) {
          text.setSpan(
            ForegroundColorSpan(
              theme.color(ThemeSlots.SLOT_FOOTNOTE_COLOR, colors.night) ?: colors.link,
            ),
            start,
            end,
            flags,
          )
        }
        if (link >= 0) {
          // A theme can turn the underline off, which is what a link that reads
          // as a link by color alone wants.
          val linkColor = theme.color(ThemeSlots.SLOT_LINK_COLOR, colors.night) ?: colors.link
          text.setSpan(
            LinkSpan(link, linkColor, theme.flag(ThemeSlots.SLOT_LINK_UNDERLINE)),
            start,
            end,
            flags,
          )
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

      // Each laid-out inline formula draws its glyphs over its own literal. A
      // formula with no layout keeps the literal, which is what a reader sees
      // while it is still arriving and when it failed.
      if (!isCode) {
        val minimumRule = density * 0.5f
        for (formula in document.formulas) {
          val list = formula.list ?: continue
          if (document.blockKind(formula.block) == NativeDocument.KIND_MATH_BLOCK) continue
          val blockStart = document.blockStart(formula.block)
          val start = blockStart + formula.start
          val end = blockStart + formula.end
          if (start < blockStart || end > text.length || end <= start) continue
          text.setSpan(MathInlineSpan(list, minimumRule), start, end, flags)
        }
      }

      // The caret's placeholder goes in before the text is precomputed, because
      // the placeholder's advance is part of what was measured and a layout built
      // without it would put the last line somewhere else.
      //
      // A breaking space first, so a full last line wraps the caret to the next
      // line the way Streamdown's `::after` does rather than overlapping the last
      // word. A caret that takes its own line gets the space too: it is a
      // paragraph of its own inside this segment, so it needs the block's own
      // spacing.
      // A table's caret is not in its text, so nothing is appended before this
      // returns. The grid view draws the shape from the kind it carries.
      if (isTable) {
        // The block above the segment is not in this slice, so the caller passes
        // its bottom margin in; zero at the very top of the document.
        return buildTable(
          document, text, 0, document.blockCount, style, containersOf(0, -1),
          style.gapAbove(document, 0, previousBottom).toInt(), identity, from, caret,
        )
      }
      val caretOffset = appendCaret(text, document, style, caret)
      // A text segment that a committed chunk split off gets the gap it would
      // have inside one segment, so both stack to the same height.
      // The block above the segment is not in this slice, so the caller passes
      // its bottom margin in; zero at the very top of the document.
      val gapBefore = style.gapAbove(document, 0, previousBottom).toInt()
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
          // A code segment is one block, so it is block 0 of the slice this
          // function was handed. `from` is its index in the whole document, which
          // is what the view needs for a download and not what these read.
          //
          // The engine parsed the info string, so the title, the start line and
          // `noLineNumbers` come back as one value rather than being rescanned here,
          // and the metrics are the block's themed ones rather than constants, so a
          // theme that resizes code resizes its gutter with it.
          document.blockFenceMeta(0),
          ceil(style.px(document.blockFontSize(0))).toInt(),
          theme.typeface(
            theme.familyAt(document.blockFontFamily(0)),
            document.blockFontWeight(0),
            false,
            true,
          ),
          caretOffset,
          caret.kind,
        )
      }
      return TextSegment(displayText, accessibleBlocks, gapBefore, caretOffset, caret.kind)
    }

    /**
     * Appends the caret's placeholder and answers the offset it sits at, or -1.
     *
     * [document] and [style] are read for the body's font metrics only; the
     * placeholder has no metrics of its own.
     *
     * The offset is carried on the segment rather than recomputed at draw time,
     * because a table's own grid view and a code card's body draw the caret and
     * neither has the text this was appended to.
     */
    private fun appendCaret(
      text: SpannableStringBuilder,
      document: NativeDocument,
      style: Style,
      caret: PlacedCaret,
    ): Int {
      if (caret.kind == 0) {
        return -1
      }
      val em = style.bodySize
      // Half an em wide, as the bar is, and no taller than the font, so the line
      // height is unchanged whatever the appearance or the text size.
      val width = ceil(em * 0.5f).toInt()
      val height = ceil(em * 1.2f).toInt()
      if (caret.place == PlacedCaret.PLACE_OWN_LINE) {
        // Its own line: two newlines open it, which is what puts the caret where
        // the next block's first line will be rather than at the end of this one.
        text.append("\n\n")
      } else if (text.isNotEmpty() && !text.endsWith("\n")) {
        text.append(' ')
      }
      val offset = text.length
      text.append('\uFFFC')
      text.setSpan(CaretSpan(width, height), offset, text.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
      return offset
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
    /**
     * One display formula, as its own segment.
     *
     * The ems come from the document's own layout rather than from a text layout's
     * line count, so `mathBlockHeight` — the same expression iOS uses — decides the
     * height and a measured formula and a drawn one cannot differ.
     *
     * `resolved` false keeps the literal: a formula the engine could not lay out is
     * not a formula that is missing, it is a formula the reader can still read, and
     * it is drawn in the theme's error colour so they can see which it is.
     */
    private fun buildMath(document: NativeDocument, style: Style, gapBefore: Int): MathSegment {
      // The literal with its delimiters, which is what `Block::text` holds and what a
      // copy produces. It is the segment's text whether or not a layout exists, so
      // the fallback needs nothing extra to be the same words.
      val literal = document.text
      val display = document.blockMathDisplay(0)
      return MathSegment(
        literal = literal,
        width = document.blockMathWidthEm(0).toFloat(),
        height = document.blockMathHeightEm(0).toFloat(),
        depth = document.blockMathDepthEm(0).toFloat(),
        resolved = document.blockMathResolved(0),
        displayList = document.formulas.firstOrNull { it.block == 0 }?.list,
        failed = document.blockMathError(0).isNotEmpty(),
        speech = document.blockMathSpeech(0).ifEmpty { literal },
        display = display,
        textAlign = style.mathTextAlign,
        gapBefore = gapBefore,
      )
    }

    /**
     * One picture, as its own segment.
     *
     * The alt text comes from the document's image list rather than from the block's
     * text, because the block's text is the whole `![…](…)` source: what a reader
     * needs when the bytes do not arrive is the alt text alone.
     *
     * `loadable` is the engine's own policy answer, resolved once on the platform
     * side by `isLoadableImageUrl` when the document was parsed. It is carried
     * rather than re-checked here, so a destination the engine refused is one the
     * view never asks for.
     */
    private fun buildImage(
      images: NativeImages,
      ordinal: Int,
      gapBefore: Int,
    ): ImageSegment? {
      // The plan counted this block's first picture in `Document::images`, so an
      // ordinal outside that list is a plan and a document that disagree. There is
      // then no picture to build, and the caller falls back to the block's text,
      // which is the writer's own source and always readable.
      if (ordinal < 0 || ordinal >= images.count) {
        return null
      }
      return ImageSegment(
        ordinal = ordinal,
        url = images.url(ordinal),
        alt = images.alt(ordinal),
        title = images.title(ordinal),
        range = images.range(ordinal),
        loadable = images.loadable(ordinal),
        link = images.link(ordinal),
        gapBefore = gapBefore,
      )
    }

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
      fenceMeta: FenceMeta = FenceMeta(),
      codeTextSize: Int,
      codeFace: Typeface,
      caretOffset: Int = -1,
      caretKind: Int = 0,
    ): CodeSegment {
      val newline = text.indexOfFirst { it == '\n' }
      val firstLine = text.substring(0, if (newline < 0) text.length else newline).toString()
      return CodeSegment(
        text = displayText,
        accessibleBlocks = accessibleBlocks,
        language = language,
        title = fenceMeta.title,
        fenceMeta = fenceMeta,
        // Found once here, on whichever thread built the rendering, so the
        // gutter's draw pass never searches the text for newlines.
        lineStarts = codeLineStarts(text),
        codeTextSize = codeTextSize,
        codeFace = codeFace,
        blockIndex = blockIndex,
        codeOrdinal = codeOrdinal,
        containers = containers.first,
        contentLeading = contentLeading(containers.first).toInt(),
        rtl = startsRightToLeft(text, 0, text.length),
        firstLine = firstLine,
        padding = style.codePadding,
        gutterGap = ceil(GUTTER_GAP * style.density).toInt(),
        colors = style.colors,
        radius = style.codeBlockRadius,
        borderWidth = style.codeBlockBorder,
        headerFace = style.theme.slotTypeface(
          ThemeSlots.SLOT_CODE_BLOCK_FONT_FAMILY,
          style.theme.number(ThemeSlots.SLOT_CODE_BLOCK_FONT_WEIGHT),
          false,
          true,
        ),
        headerSize = style.codeHeaderSize,
        markerFace = style.markerPaint.typeface,
        markerSize = style.markerSize,
        markerGap = style.markerGap,
        caretOffset = caretOffset,
        caretKind = caretKind,
        gapBefore = gapBefore,
      )
    }

    private fun buildTable(
      document: NativeDocument,
      text: Spanned,
      from: Int,
      to: Int,
      style: Style,
      containers: Triple<List<ContainerDecoration>, Float, Boolean>,
      gapBefore: Int,
      identity: Long,
      blockIndex: Int,
      caret: PlacedCaret = PlacedCaret.NONE,
    ): TableSegment {
      val paint = style.paint
      val markerPaint = style.markerPaint
      val colors = style.colors
      val theme = style.theme
      val density = style.density
      val px: (Float) -> Float = style::px
      val cellPaint = TextPaint(paint).apply { if (containers.third) color = colors.secondary }
      val maxWidth = style.maxColumnWidth
      val columns = (from until to).maxOf { document.cellCount(it) }
      val widths = FloatArray(columns) { style.minColumnWidth }
      val cellKeys = ArrayList<List<TableCellKey?>>(to - from)
      val cellText = (from until to).map { row ->
        val keys = ArrayList<TableCellKey?>(document.cellCount(row))
        cellKeys.add(keys)
        (0 until document.cellCount(row)).map { column ->
          val start = document.cellStart(row, column)
          val end = document.cellEnd(row, column)
          val header = document.isTableHeader(row)
          // The source's inline spans can carry snapshot-local link indices or
          // mutable formula drawing state. Cache only span-free source cells.
          val key = if (text.getSpans(start, end, Any::class.java).isEmpty() &&
            (end - start) * 24L + 256 <= TABLE_CELL_CACHE_COST
          ) {
            TableCellKey(
              document.text.substring(start, end), style.themeId, style.scale, density,
              colors.night, containers.third, header, document.cellAlign(row, column),
              cellPaint.textLocales.toLanguageTags(),
            )
          } else null
          keys.add(key)
          val prepared = key?.let { tableCellTexts.get(it) } ?: run {
            val cell = SpannableStringBuilder(text, start, end)
            if (header) {
              // The header's own weight and family when the theme sets them, and
              // the plain table face when it does not, so a head is never a
              // different size from the column under it.
              val face = theme.slotTypeface(
                ThemeSlots.SLOT_TABLE_HEADER_FONT_FAMILY,
                theme.number(ThemeSlots.SLOT_TABLE_HEADER_FONT_WEIGHT),
                false,
              )
              if (face != cellPaint.typeface) {
                cell.setSpan(TypefaceSpan(face), 0, cell.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
              }
              val headerColor =
                theme.color(ThemeSlots.SLOT_TABLE_HEADER_TEXT_COLOR, colors.night) ?: cellPaint.color
              cell.setSpan(
                ForegroundColorSpan(headerColor),
                0,
                cell.length,
                Spanned.SPAN_EXCLUSIVE_EXCLUSIVE,
              )
            }
            PreparedTableCell(
              if (key != null) SpannedString(cell) else cell,
              ceil(Layout.getDesiredWidth(cell, cellPaint)),
            ).also {
              if (key != null) tableCellTexts.put(key, it)
            }
          }
          widths[column] = max(widths[column], min(prepared.desiredWidth, maxWidth))
          prepared.text
        }
      }

      val paddingHorizontal = style.cellPaddingHorizontal
      val paddingVertical = style.cellPaddingVertical
      val cells = cellText.mapIndexed { rowIndex, row ->
        row.mapIndexed { column, cell ->
          val alignment = when (document.cellAlign(from + rowIndex, column)) {
            NativeDocument.ALIGN_CENTER -> Layout.Alignment.ALIGN_CENTER
            NativeDocument.ALIGN_RIGHT -> Layout.Alignment.ALIGN_OPPOSITE
            else -> Layout.Alignment.ALIGN_NORMAL
          }
          val width = ceil(widths[column]).toInt()
          val key = cellKeys[rowIndex][column]?.let { TableCellLayoutKey(it, width) }
          key?.let { tableCellLayouts.get(it) } ?: buildLayout(cell, cellPaint, width, alignment).also {
            if (key != null) tableCellLayouts.put(key, it)
          }
        }
      }
      // One border width for the closing grid line, so the last row and column
      // are not clipped by the view that draws the outer edge.
      val border = ceil(style.tableBorder).toInt()
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
        borderWidth = border,
        rtl = startsRightToLeft(text, document.blockStart(from), document.blockEnd(from)),
        identity = identity,
        blockIndex = blockIndex,
        blockCount = to - from,
        caretKind = caret.kind,
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

package com.downstream

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Rect
import android.graphics.RectF
import android.os.Build
import android.text.Layout
import android.text.PrecomputedText
import android.text.Spanned
import android.text.TextPaint
import android.text.style.AbsoluteSizeSpan
import android.text.style.ForegroundColorSpan
import android.text.style.StrikethroughSpan
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.accessibility.AccessibilityNodeInfo
import android.view.accessibility.AccessibilityNodeProvider
import android.widget.TextView

/**
 * A text segment in a TextView configured like the measuring StaticLayout
 * (RenderedDocument.buildLayout), which gives selection and text navigation.
 */
/**
 * One announced piece of a block.
 *
 * `summaryContainer` is the `<details>` container this piece's block is the
 * summary of, or null. Carried on the part rather than read from the view at
 * announce time because the reading order is built once per segment and a
 * screen reader walks it repeatedly, long after the layout that built it.
 */
private class TextReadingPart(
  val piece: AccessibleTextPiece,
  val heading: Boolean,
  val taskState: Int,
  val summaryContainer: Int? = null,
  val isSummary: Boolean = false,
)

@SuppressLint("ViewConstructor")
internal class TextSegmentView(context: Context) : TextView(context) {
  private var segment: TextSegment? = null

  /**
   * The document block this view's first block is, which is what maps a reveal
   * unit's block-and-offset pair into this view's own text.
   */
  private var blockOffset = 0
  private var pressedLink = -1

  /**
   * The `<details>` summary this view draws, if it draws one.
   *
   * A field rather than a lookup because the chevron is drawn on every frame and
   * a press is hit-tested against it: the summary is the container's first leaf,
   * so it is the same answer every time and there is one per text view at most.
   */
  private var summary: DetailsBlock? = null

  /** True while the summary's body is showing, which is which way the chevron points. */
  private var summaryOpen = false

  /** The toggle control's bit, so a document with `controls={false}` draws no chevron. */
  private var detailsToggle = false
  private val codePaint = Paint()
  private val decorationPaint = TextPaint()

  /**
   * The caret's own paint.
   *
   * A separate one from the decoration pass's, so drawing a caret between two
   * decorations cannot change what they draw.
   */
  private val caretPaint = TextPaint()
  private val caretBounds = RectF()
  private val caretMetrics = Paint.FontMetrics()
  private val revealClipBounds = Rect()
  private val codePath = Path()
  private var pressedSummary: DetailsBlock? = null
  private val chevronPath = Path()
  private val codeBounds = RectF()
  private var readingParts = emptyList<TextReadingPart>()
  private var readingBlocks = emptyList<AccessibleBlock>()
  private val accessibility = MarkdownAccessibility(this, { readingParts.size }) { accessiblePart(it) }

  init {
    includeFontPadding = false
    setPadding(0, 0, 0, 0)
    setLineSpacing(0f, 1f)
    breakStrategy = Layout.BREAK_STRATEGY_HIGH_QUALITY
    hyphenationFrequency = Layout.HYPHENATION_FREQUENCY_NONE
    textDirection = View.TEXT_DIRECTION_FIRST_STRONG_LTR
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
      isFallbackLineSpacing = true
    }
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.VANILLA_ICE_CREAM) {
      // Android 15 flips TextView#USE_BOUNDS_FOR_WIDTH on for apps targeting
      // API 35+, so a TextView breaks lines on glyph bounds where the
      // measuring StaticLayout in RenderedDocument.buildLayout breaks them on
      // glyph advances. For any font whose ink overhangs its advance, a line
      // that fits at measure time wraps at draw time, and that extra line
      // falls outside the height Yoga measured: the last word silently
      // disappears. Measurement sets setUseBoundsForWidth(false) too, so both
      // sides break lines the same way.
      //
      // The trade is Android 15's automatic reservation of overhang space at
      // the edges of a line, which is what every earlier version did not do
      // and what losing whole words is worse than.
      setUseBoundsForWidth(false)
    }
    gravity = Gravity.TOP or Gravity.START
    setBackgroundColor(0)
    highlightColor = 0x33007AFF
  }

  fun setSelectable(selectable: Boolean) {
    if (isTextSelectable != selectable) {
      setTextIsSelectable(selectable)
    }
  }

  override fun onTextContextMenuItem(id: Int): Boolean {
    // A copy that would take the caret's placeholder is written here instead,
    // because the platform's own copy reads the text verbatim and the placeholder
    // is a space the caret reserved rather than content. A selection that does not
    // touch it goes through the platform, so the common case is unchanged.
    if (id == android.R.id.copy && selectionStart != selectionEnd) {
      val start = minOf(selectionStart, selectionEnd)
      val end = maxOf(selectionStart, selectionEnd)
      val selected = text.subSequence(start, end)
      if (selected.indexOf(CARET_PLACEHOLDER) >= 0) {
        val copied = copyTextWithoutCaret(text, start, end)
        ControlDelivery.copyText(context, COPIED_LABEL, copied)
        ControlDelivery.beginCopy()
        return true
      }
    }
    val handled = super.onTextContextMenuItem(id)
    if (handled && (id == android.R.id.copy || id == android.R.id.cut)) ControlDelivery.beginCopy()
    return handled
  }

  fun setSegment(segment: TextSegment, rendered: RenderedDocument, firstBlock: Int) {
    if (segment === this.segment && firstBlock == blockOffset) {
      return
    }
    // The plan is what knows which document blocks this segment covers; the
    // segment itself only knows its own text, and a reveal unit names a document
    // block.
    blockOffset = firstBlock
    setContent(segment.text, segment.accessibleBlocks, rendered)
    // setContent also serves code bodies and clears this field. Retain the
    // identity afterwards so settled text keeps its layout and selection.
    this.segment = segment
    // The summary is the container's own first leaf, so at most one of a
    // segment's collapsible blocks is it, and it is the one a chevron belongs to.
    summary = segment.detailsBlocks.firstOrNull { it.summary }
    invalidate()
  }

  /**
   * Whether the summary's body is showing, and whether the control is on at all.
   *
   * Both in one setter because both change the chevron and nothing else: the
   * glyph points the way the body will move, and a document with the control off
   * draws no glyph and takes no touches.
   */
  fun setDetailsState(toggle: Boolean, open: Boolean) {
    if (toggle == detailsToggle && open == summaryOpen) return
    detailsToggle = toggle
    summaryOpen = open
    invalidate()
  }

  /**
   * Shows text that lives in another segment, such as a code block's body: the
   * same spans, precomputed text, and reading order, without a throwaway
   * segment object standing in for the real one.
   */
  /** The document block this view's text starts at, for a body shown with [setContent]. */
  fun setBlockOffset(firstBlock: Int) {
    bandFrame = null
    blockOffset = firstBlock
  }

  fun setContent(text: Spanned, accessibleBlocks: List<AccessibleBlock>, rendered: RenderedDocument) {
    bandFrame = null
    segment = null
    summary = null
    readingBlocks = accessibleBlocks
    val precomputed = Build.VERSION.SDK_INT >= Build.VERSION_CODES.P &&
      PrecomputedTexts.show(this, text, rendered.paint.color)
    if (!precomputed) {
      paint.typeface = rendered.paint.typeface
      setTextSize(TypedValue.COMPLEX_UNIT_PX, rendered.paint.textSize)
      setTextColor(rendered.paint.color)
      // Selection needs editable spans; plain display takes the cheaper
      // static layout.
      setText(text, if (isTextSelectable) BufferType.SPANNABLE else BufferType.NORMAL)
    }
    // A themed selection highlight. `textHighlightColor` is what a TextView
    // draws its selection in, and it is the only handle on it that has existed
    // across every supported API level.
    rendered.style.colors.selectionColor?.let { highlight ->
      highlightColor = highlight
    }
    readingParts = buildReadingParts(accessibleBlocks, segment?.detailsBlocks ?: emptyList())
    accessibility.contentChanged()
  }

  override fun getAccessibilityNodeProvider(): AccessibilityNodeProvider = accessibility

  override fun onInitializeAccessibilityNodeInfo(info: AccessibilityNodeInfo) {
    super.onInitializeAccessibilityNodeInfo(info)
    // The provider exposes text and links in source order.
    info.text = null
    info.contentDescription = null
    info.isFocusable = false
  }

  private fun buildReadingParts(
    blocks: List<AccessibleBlock>,
    details: List<DetailsBlock>,
  ): List<TextReadingPart> = buildList {
    val content = this@TextSegmentView.text as? Spanned ?: return@buildList
    for (block in blocks) {
      val start = block.start.coerceIn(0, content.length)
      val end = block.end.coerceIn(start, content.length)
      val frame = details.firstOrNull { it.start == block.start && it.end == block.end }
      val container = frame?.containers?.lastOrNull { it.kind == CONTAINER_DETAILS }?.id
      linkPieces(content, start, end).forEachIndexed { index, piece ->
        add(TextReadingPart(piece, index == 0 && block.heading,
          if (index == 0) block.taskState else 0, container, frame?.summary == true))
      }
    }
  }

  private fun accessiblePart(index: Int): MarkdownAccessibility.Entry? {
    val layout = layout ?: return null
    val part = readingParts.getOrNull(index) ?: return null
    val start = part.piece.start
    val end = part.piece.end
    val content = text ?: return null
    val visible = content.subSequence(start, end).toString()
    val label = when (part.taskState) {
      NativeDocument.TASK_CHECKED -> "Checked, $visible"
      NativeDocument.TASK_UNCHECKED -> "Unchecked, $visible"
      else -> visible
    }
    val firstLine = layout.getLineForOffset(start)
    val lastLine = layout.getLineForOffset(if (end > start) end - 1 else start)
    val selectionPath = Path()
    layout.getSelectionPath(start, end, selectionPath)
    val selectedBounds = RectF()
    selectionPath.computeBounds(selectedBounds, true)
    val bounds = Rect().also { selectedBounds.roundOut(it) }
    if (bounds.isEmpty) {
      bounds.set(0, layout.getLineTop(firstLine), width, layout.getLineBottom(lastLine))
    }
    bounds.offset(0, -scrollY)
    val link = part.piece.linkIndex
    // A summary is announced as the control it is, so a screen reader says which
    // words open a body rather than leaving the reader to guess.
    val summary = part.summaryContainer
    val toggle = if (detailsToggle && summary != null && part.isSummary) summary else null
    return MarkdownAccessibility.Entry(
      text = label,
      bounds = bounds,
      heading = part.heading,
      linkPress = if (link == null) null else ({ findHost()?.pressLink(link) }),
      togglePress = toggle?.let { id -> ({ findHost()?.toggleDetails(id) }) },
      expanded = toggle?.let { isSummaryOpen(readerChoices) },
    )
  }

  override fun onDraw(canvas: Canvas) {
    val band = findHost()?.revealFrame()?.let { bandFor(it) }
    if (band == null || band.count == 0) {
      drawContent(canvas)
      drawCaret(canvas, null)
      return
    }
    // A `TextView` draws its whole layout in one call, and the text is a shared
    // `SpannableStringBuilder` or `PrecomputedText`, so alpha cannot come from a
    // span: adding one would mean copying the tail. It comes from clipping
    // instead, by running this same draw once for the text outside the fading band
    // and once per alpha bucket inside it. Nothing shared is touched, and nothing
    // is laid out.
    //
    // The paths are in the layout's own coordinates, which sit inside the view's
    // padding, so they are offset once here rather than the canvas being
    // translated: `super.onDraw` applies the padding itself and must not see it
    // twice.
    val paddingLeft = totalPaddingLeft.toFloat()
    val paddingTop = totalPaddingTop.toFloat()
    val layout = layout ?: return
    val paths = revealPaths ?: RevealClipPaths().also { revealPaths = it }
    // Exclude only active ranges, preserving settled gaps and trailing text.
    val buckets = paths.build(layout, band, paddingLeft, paddingTop)
    val hidden = paths.hidden
    canvas.save()
    clipOut(canvas, hidden)
    drawContent(canvas)
    canvas.restore()

    // One layer per alpha bucket, so a compressed burst of forty words at forty
    // different alphas costs eight layers rather than forty. A bucket's path holds
    // every unit whose alpha rounds to it, so the passes are disjoint and nothing
    // is drawn twice.
    // `getSelectionPath` resets the path it fills, so each unit is drawn into a
    // scratch path and added to its bucket's.
    for (bucket in 1..MAX_ALPHA_BUCKETS) {
      if (buckets and (1 shl bucket) == 0) continue
      val path = paths.buckets[bucket - 1]
      val bounds = paths.bounds
      path.computeBounds(bounds, true)
      if (bounds.isEmpty()) continue
      // `saveLayerAlpha` rather than a paint alpha, because the draw below is the
      // TextView's own, which sets its paint alpha for every span it draws. The
      // layer is bounded to the bucket's own bounds, so a 40-word burst does not
      // allocate a screen-sized layer eight times.
      val checkpoint = canvas.saveLayerAlpha(bounds, (bucketAlpha(bucket) * 255f).toInt())
      canvas.clipPath(path)
      // One translation for the whole bucket, from its first unit, rather than one
      // per unit: the rise is the same fraction of four points for every unit in a
      // bucket, and a bucket is a rounding of the alpha, so the difference is below
      // a pixel.
      canvas.save()
      val rise = paths.rises[bucket - 1]
      if (rise != 0f) {
        canvas.translate(0f, rise)
      }
      drawContent(canvas)
      canvas.restore()
      canvas.restoreToCount(checkpoint)
    }
    // The caret sits at the reveal frontier, which is inside the clipped-out band,
    // so it is drawn once outside every pass rather than clipped away with the
    // text that has not appeared yet.
    drawCaret(canvas, band)
  }

  private var revealPaths: RevealClipPaths? = null

  /** Everything the segment draws: the one call the clipped passes repeat. */
  private fun drawContent(canvas: Canvas) {
    drawInlineCodeBackgrounds(canvas)
    super.onDraw(canvas)
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && text is PrecomputedText) {
      drawDecorations(canvas)
    }
    // Drawn after the text because the chevron sits in the leading margin and a
    // text layout that starts flush to the edge would otherwise paint over it.
    drawSummaryChevron(canvas)
  }

  /**
   * Removes [path] from the canvas's clip.
   *
   * `clipOutPath` is the direct way from API 26. Below that the difference has to
   * be spelled as a region, which means the clip has to be a rectangle first: the
   * current bounds are saved and re-clipped so the result is exactly the clip that
   * was already in force with [path] taken out of it.
   */
  private fun clipOut(canvas: Canvas, path: Path) {
    if (path.isEmpty) return
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
      canvas.clipOutPath(path)
      return
    }
    @Suppress("DEPRECATION")
    val bounds = revealClipBounds
    canvas.getClipBounds(bounds)
    @Suppress("DEPRECATION")
    canvas.clipRect(bounds)
    @Suppress("DEPRECATION")
    canvas.clipPath(path, android.graphics.Region.Op.DIFFERENCE)
  }

  /**
   * Draws the caret, at the reveal frontier while a fade is running.
   *
   * The frontier is the end of the last unit that has started, not the end of the
   * layout: a caret at the end of the layout would sit after words that are still
   * invisible, which reads as a gap rather than as a cursor.
   */
  private fun drawCaret(canvas: Canvas, band: RevealBand?) {
    val current = segment ?: return
    if (current.caretKind == 0) return
    val layout = layout ?: return
    // The frontier is where the first unit that has not started begins, which is
    // the end of the last one that has. With nothing fading it is the placeholder.
    // [band] is this frame's, sorted by offset, so the first unstarted unit in it
    // is the answer.
    var frontier = current.caretOffset
    if (band != null) {
      for (index in 0 until band.count) {
        if (band.alphas[index] <= 0f) {
          frontier = band.offsets[index * 2]
          break
        }
      }
    }
    if (frontier < 0) return
    val x = CaretShape.xFor(layout, frontier) ?: return
    // The placeholder's own line, so the caret draws where the space it reserved
    // is rather than where the layout's last line happens to end.
    val line = layout.getLineForOffset(frontier.coerceAtMost(layout.text.length))
    if (line < 0) return
    val baseline = layout.getLineBaseline(line) + totalPaddingTop
    val rect = when (current.caretKind) {
      2 -> CaretShape.dotRect(paint, x + totalPaddingLeft, baseline.toFloat(), caretBounds)
      else -> CaretShape.barRect(paint, x + totalPaddingLeft, baseline.toFloat(), caretBounds, caretMetrics)
    }
    val shape = caretPaint
    shape.set(paint)
    // The foreground at the last shown character, resolved for the current
    // appearance, which is what Streamdown's inherited color amounts to.
    shape.color = currentTextColor
    shape.style = Paint.Style.FILL
    if (current.caretKind == 2) {
      canvas.drawOval(rect, shape)
    } else {
      canvas.drawRoundRect(rect, rect.height() / 2f, rect.height() / 2f, shape)
    }
  }

  /**
   * The units of this view's blocks that are still fading, mapped into this
   * view's own text.
   *
   * A reveal unit names a document block and an offset inside it, and this view
   * knows where each of its blocks starts in its text, so the translation is one
   * addition per end. The answer is the text the passes need and nothing else:
   * the units that are still at full alpha are dropped, because an unaffected
   * range is drawn by the first pass exactly as it always was.
   */
  private var revealBand: RevealBand? = null
  private var bandFrame: RevealFrame? = null
  private var bandRevision = -1L
  private var cachedBand: RevealBand? = null

  /**
   * The fading characters of this view, or null when nothing is fading.
   *
   * Public within the package so the code gutter can ask for its own numbers'
   * alphas: the gutter draws beside the body rather than inside it, and it is the
   * body that knows where its blocks are.
   */
  private fun bandForReveal(): RevealBand? = findHost()?.revealFrame()?.let { bandFor(it) }

  /**
   * The alpha at one character of this view's own text, 1 for anything that is not
   * fading.
   *
   * Used where a single value is wanted rather than a clipped pass: a gutter
   * number, a card's chrome, a table row's fill.
   */
  internal fun alphaAt(offset: Int): Float {
    val band = bandForReveal() ?: return 1f
    for (index in 0 until band.count) {
      if (offset >= band.offsets[index * 2] && offset < band.offsets[index * 2 + 1]) {
        return band.alphas[index]
      }
    }
    return 1f
  }

  private fun bandFor(hostFrame: RevealFrame): RevealBand? {
    if (bandFrame === hostFrame && bandRevision == hostFrame.revision) return cachedBand
    bandFrame = hostFrame
    bandRevision = hostFrame.revision
    cachedBand = buildBand(hostFrame)
    return cachedBand
  }

  private fun buildBand(hostFrame: RevealFrame): RevealBand? {
    // The blocks this view shows, which a code body has without a text segment of
    // its own: `setContent` records them for both.
    val blocks = readingBlocks
    if (blocks.isEmpty()) return null
    val length = text?.length ?: return null
    val first = hostFrame.firstUnitForBlock(blockOffset)
    val last = hostFrame.firstUnitForBlock(blockOffset + blocks.size)
    val count = last - first
    if (count == 0) return null

    val band = revealBand ?: RevealBand(count).also { revealBand = it }
    band.begin(count)
    for (index in first until last) {
      // Past the cap a unit draws at full alpha rather than overflowing the band.
      if (band.isFull) break
      val unit = hostFrame.unitAt(index)
      if (unit.alpha >= 1f) continue
      val block = blockAt(blocks, unit.block) ?: continue
      // The caret's own placeholder is never a reveal target, and a unit cannot
      // reach past it: the placeholder is the last character and the text before
      // it is all that fades.
      val from = (block.start + unit.start).coerceIn(0, length)
      val to = (block.start + unit.end).coerceIn(from, length)
      if (from >= to) continue
      val slot = band.count++
      band.offsets[slot * 2] = from
      band.offsets[slot * 2 + 1] = to
      band.alphas[slot] = unit.alpha
      band.rises[slot] = unit.rise
    }
    if (band.count == 0) return null
    // The engine schedules in text order within a block and the blocks are in
    // order, so the mapped ranges already ascend. One insertion sort makes that a
    // guarantee rather than an assumption, and it is a no-op when it already holds.
    band.sortByOffset()
    return band
  }

  /** The accessible block for a document block, or null when this view has none. */
  private fun blockAt(blocks: List<AccessibleBlock>, documentBlock: Int): AccessibleBlock? {
    // The plan hands every segment the document blocks it covers, in order, so the
    // index within this view is the document index less the first one it covers.
    val first = blockOffset
    val index = documentBlock - first
    return blocks.getOrNull(index)
  }

  // A TextView drawing PrecomputedText leaves out underline and
  // strikethrough, so link underlines and strikethrough lines are drawn here
  // at the font's own positions, under or through each line's run.
  /**
   * The `<details>` chevron, drawn in the leading margin of the summary's line.
   *
   * In the margin rather than in the line because the line is the text: a reader
   * who selected the summary and copied it should get the words they wrote, not a
   * glyph. It is drawn here, with the list markers and the quote bars, rather than
   * as a child view because a `TextView` has no place to put one that follows its
   * own line breaking — and this one has to follow the line it belongs to.
   *
   * The glyph is a path rather than a glyph from a font, so it is the same shape
   * at every density and needs no asset: two strokes that meet, turned a quarter
   * either way.
   */
  private fun drawSummaryChevron(canvas: Canvas) {
    val summary = summary ?: return
    if (!detailsToggle) return
    val layout = layout ?: return
    val first = layout.getLineForOffset(summary.start)
    val line = layout.getLineForOffset((summary.end - 1).coerceAtLeast(summary.start))
    val baseline = layout.getLineBaseline(line).toFloat()
    // The line's own ascent and descent rather than the paragraph's, so a
    // summary line that is shorter than its neighbours gets a chevron sized to
    // itself. The text is centred in that box, which is where a reader's eye is
    // already looking.
    val ascent = layout.getLineAscent(first).toFloat()
    val descent = layout.getLineDescent(line).toFloat()
    val centerY = baseline - (ascent - descent) / 2
    val size = baseline - ascent
    val half = size * 0.22f
    val tip = if (summaryOpen) centerY + half else centerY - half
    codePaint.color = currentTextColor
    codePaint.style = Paint.Style.STROKE
    codePaint.strokeWidth = maxOf(1f, size * 0.06f)
    codePaint.strokeCap = Paint.Cap.ROUND
    codePaint.strokeJoin = Paint.Join.ROUND
    chevronPath.reset()
    // In the leading margin, on the side the text itself starts: a right-to-left
    // paragraph puts the chevron at its right, where a reader of that script
    // expects the disclosure to be. `getLineLeft` is the horizontal origin of the
    // line including its indent, which is what the margin is measured from.
    val x = layout.getLineLeft(line) - size * 0.45f
    chevronPath.moveTo(x - half, tip - (if (summaryOpen) half else -half))
    chevronPath.lineTo(x, tip)
    chevronPath.lineTo(x + half, tip - (if (summaryOpen) half else -half))
    canvas.drawPath(chevronPath, codePaint)
    codePaint.style = Paint.Style.FILL
    codePaint.strokeCap = Paint.Cap.BUTT
  }

  private fun drawDecorations(canvas: Canvas) {
    val layout = layout ?: return
    val text = layout.text as? Spanned ?: return
    val links = text.getSpans(0, text.length, LinkSpan::class.java)
    val strikes = text.getSpans(0, text.length, StrikethroughSpan::class.java)
    if (links.isEmpty() && strikes.isEmpty()) {
      return
    }
    canvas.save()
    canvas.translate(totalPaddingLeft.toFloat(), totalPaddingTop.toFloat())
    for (link in links) {
      drawDecoration(canvas, layout, text, link, link.color, strike = false)
    }
    for (strike in strikes) {
      val start = text.getSpanStart(strike)
      val color = text.getSpans(start, start + 1, ForegroundColorSpan::class.java).lastOrNull()?.foregroundColor
        ?: currentTextColor
      drawDecoration(canvas, layout, text, strike, color, strike = true)
    }
    canvas.restore()
  }

  private fun drawDecoration(canvas: Canvas, layout: Layout, text: Spanned, span: Any, color: Int, strike: Boolean) {
    val start = text.getSpanStart(span)
    val end = text.getSpanEnd(span)
    if (start >= end) return
    decorationPaint.set(paint)
    text.getSpans(start, end, AbsoluteSizeSpan::class.java).lastOrNull()?.let { decorationPaint.textSize = it.size.toFloat() }
    val size = decorationPaint.textSize
    val (position, thickness) = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
      if (strike) {
        decorationPaint.strikeThruPosition to decorationPaint.strikeThruThickness
      } else {
        decorationPaint.underlinePosition to decorationPaint.underlineThickness
      }
    } else {
      (if (strike) -size * 0.28f else size / 9f) to size / 18f
    }
    decorationPaint.color = color
    decorationPaint.style = Paint.Style.FILL
    for (line in layout.getLineForOffset(start)..layout.getLineForOffset(end - 1)) {
      val from = maxOf(start, layout.getLineStart(line))
      val to = minOf(end, layout.getLineEnd(line))
      if (from >= to) continue
      codePath.reset()
      layout.getSelectionPath(from, to, codePath)
      codePath.computeBounds(codeBounds, true)
      val y = layout.getLineBaseline(line) + position
      canvas.drawRect(codeBounds.left, y, codeBounds.right, y + maxOf(1f, thickness), decorationPaint)
    }
  }

  // Each line of a code span gets a rectangle from the selection path's
  // horizontal extent, which follows wrapping and bidirectional text, and the
  // code's own ascent and descent.
  private fun drawInlineCodeBackgrounds(canvas: Canvas) {
    val layout = layout ?: return
    val text = layout.text as? Spanned ?: return
    val spans = text.getSpans(0, text.length, InlineCodeBackgroundSpan::class.java)
    if (spans.isEmpty()) {
      return
    }
    canvas.save()
    canvas.translate(totalPaddingLeft.toFloat(), totalPaddingTop.toFloat())
    for (span in spans) {
      val start = text.getSpanStart(span)
      val end = text.getSpanEnd(span)
      if (start >= end) continue
      codePaint.color = span.color
      for (line in layout.getLineForOffset(start)..layout.getLineForOffset(end - 1)) {
        val from = maxOf(start, layout.getLineStart(line))
        val to = minOf(end, layout.getLineEnd(line))
        if (from >= to) continue
        codePath.reset()
        layout.getSelectionPath(from, to, codePath)
        codePath.computeBounds(codeBounds, true)
        val baseline = layout.getLineBaseline(line).toFloat()
        canvas.drawRect(codeBounds.left, baseline + span.ascent, codeBounds.right, baseline + span.descent, codePaint)
      }
    }
    canvas.restore()
  }

  // A touch that starts on a link belongs to the link: the TextView never
  // sees it, so selection neither highlights the link nor clicks its span.
  @SuppressLint("ClickableViewAccessibility")
  override fun onTouchEvent(event: MotionEvent): Boolean {
    val layout = layout ?: return super.onTouchEvent(event)
    val x = event.x - totalPaddingLeft + scrollX
    val y = event.y - totalPaddingTop + scrollY
    if (event.actionMasked == MotionEvent.ACTION_DOWN) {
      // The summary is hit-tested before the links under it: the whole line is the
      // control, and a link inside a summary is still a summary a reader meant to
      // open. Everything else falls through to the link, then to selection.
      val summary = this.summary
      if (detailsToggle && summary != null && onSummaryLine(layout, summary, y)) {
        pressedSummary = summary
        return true
      }
      pressedLink = linkAt(layout, x, y)
    }
    val pressed = pressedSummary
    if (pressed != null) {
      when (event.actionMasked) {
        MotionEvent.ACTION_UP -> {
          pressedSummary = null
          if (pressed === summary) {
            // The innermost frame is the block's own container, which is the one
            // whose body the press opens. A summary inside another summary opens
            // the inner one, the same way pressing it reads.
            val container = pressed.containers.last { it.kind == CONTAINER_DETAILS }
            findHost()?.toggleDetails(container.id)
          }
          return true
        }
        MotionEvent.ACTION_CANCEL -> pressedSummary = null
        else -> return true
      }
    }
    if (pressedLink < 0) {
      return super.onTouchEvent(event)
    }
    when (event.actionMasked) {
      MotionEvent.ACTION_UP -> {
        val link = pressedLink
        pressedLink = -1
        if (linkAt(layout, x, y) == link) {
          findHost()?.pressLink(link)
        }
      }
      MotionEvent.ACTION_CANCEL -> pressedLink = -1
    }
    return true
  }

  /** The summary line under `y`, or null. The whole line is the control. */
  private fun onSummaryLine(layout: Layout, block: DetailsBlock, y: Float): Boolean {
    val first = layout.getLineForOffset(block.start)
    val last = layout.getLineForOffset((block.end - 1).coerceAtLeast(block.start))
    return y >= layout.getLineTop(first) && y <= layout.getLineBottom(last)
  }

  /**
   * The reader's own set of choices, which the accessibility tree is built against.
   *
   * The announcement has to say which way the chevron points, and the chevron is
   * drawn from the same set, so the view holds it rather than re-deriving it from
   * the document at announce time.
   */
  var readerChoices = ReaderChoices()
    set(value) {
      field = value
      accessibility.contentChanged()
    }

  /** Whether this view's summary has its body showing under [choices]. */
  fun isSummaryOpen(choices: ReaderChoices): Boolean {
    val summary = summary ?: return true
    val frame = summary.containers.last { it.kind == CONTAINER_DETAILS }
    return resolveDetails(choices, frame.id, frame.open).open
  }

  private fun findHost(): DownstreamMarkdownView? {
    var parent = parent
    while (parent != null && parent !is DownstreamMarkdownView) {
      parent = parent.parent
    }
    return parent as? DownstreamMarkdownView
  }
}

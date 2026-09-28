package com.downstream

import android.annotation.SuppressLint
import android.content.Context
import android.content.res.Configuration
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Rect
import android.graphics.RectF
import android.os.Build
import android.text.Layout
import android.text.Spanned
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.TextView
import android.view.accessibility.AccessibilityNodeInfo
import android.view.accessibility.AccessibilityNodeProvider

private const val HIT_SLOP = 8f

/** Link index at a point in a laid-out text, or -1. */
private fun linkAt(layout: Layout, x: Float, y: Float): Int {
  if (y < 0 || y > layout.height) {
    return -1
  }
  val line = layout.getLineForVertical(y.toInt())
  if (x < layout.getLineLeft(line) - HIT_SLOP || x > layout.getLineRight(line) + HIT_SLOP) {
    return -1
  }
  val offset = layout.getOffsetForHorizontal(line, x)
  val text = layout.text as? Spanned ?: return -1
  return text.getSpans(offset, offset, LinkSpan::class.java).firstOrNull()?.index ?: -1
}

/**
 * Fabric host view for <DownstreamMarkdown>: stacks segment views at the
 * frames the measurement computed. Documents arrive through Fabric state; the
 * view never parses.
 */
class DownstreamMarkdownView(context: Context) : ViewGroup(context) {
  internal var onLinkPress: ((view: DownstreamMarkdownView, url: String, start: Int, end: Int) -> Unit)? = null

  private var documentId = 0L
  private var fontScale = 1f
  private var rendered: RenderedDocument? = null
  private var selectable = false
  private var generation = 0
  private var sessionId: String? = null
  private var sourceEpoch = 0

  fun setGeneration(value: Int) {
    if (value != generation) {
      generation = value
      sourceChanged()
    }
  }

  fun setSessionId(value: String?) {
    if (value != sessionId) {
      sessionId = value
      sourceChanged()
    }
  }

  private fun sourceChanged() {
    sourceEpoch++
    for (index in 0 until childCount) {
      (getChildAt(index) as? TableSegmentView)?.resetScroll()
    }
  }

  fun setDocument(documentId: Long, fontScale: Float) {
    if (documentId == this.documentId && fontScale == this.fontScale && rendered != null) {
      return
    }
    this.documentId = documentId
    this.fontScale = fontScale
    rendered = if (documentId != 0L) RenderedDocument.obtain(context, documentId, fontScale) else null
    syncSegmentViews()
  }

  fun setSelectable(selectable: Boolean) {
    this.selectable = selectable
    for (index in 0 until childCount) {
      (getChildAt(index) as? TextSegmentView)?.setSelectable(selectable)
    }
  }

  fun reset() {
    documentId = 0L
    rendered = null
    generation = 0
    sessionId = null
    sourceEpoch++
    removeAllViews()
  }

  override fun onConfigurationChanged(newConfig: Configuration) {
    super.onConfigurationChanged(newConfig)
    if (documentId != 0L) {
      RenderedDocument.obtain(context, documentId, fontScale)?.let {
        rendered = it
        syncSegmentViews()
      }
    }
  }

  internal fun pressLink(index: Int) {
    val link = DownstreamNative.readLink(documentId, index) ?: return
    onLinkPress?.invoke(this, link.url, link.start, link.end)
  }

  // Reuse views of the same kind in place, so unchanged segments keep their
  // selection and scroll position across updates.
  private fun syncSegmentViews() {
    val segments = rendered?.segments.orEmpty()
    segments.forEachIndexed { index, segment ->
      var view = if (index < childCount) getChildAt(index) else null
      val reusable = when (segment) {
        is TextSegment -> view is TextSegmentView
        is TableSegment -> view is TableSegmentView
      }
      if (!reusable) {
        if (view != null) removeViewAt(index)
        view = when (segment) {
          is TextSegment -> TextSegmentView(context).apply { setSelectable(selectable) }
          is TableSegment -> TableSegmentView(context)
        }
        addView(view, index)
      }
      when (segment) {
        is TextSegment -> (view as TextSegmentView).setSegment(segment, rendered!!)
        is TableSegment -> (view as TableSegmentView).setSegment(segment, sourceEpoch)
      }
    }
    while (childCount > segments.size) {
      removeViewAt(childCount - 1)
    }
    layoutSegments()
  }

  // React Native does not relayout children that request it, so segments are
  // laid out here whenever the document or size changes.
  private fun layoutSegments() {
    val document = rendered ?: return
    val width = width - paddingLeft - paddingRight
    if (width <= 0) {
      return
    }
    val frames = document.frames(width)
    for (index in 0 until minOf(childCount, frames.size / 2)) {
      val child = getChildAt(index)
      val top = paddingTop + frames[index * 2]
      val height = frames[index * 2 + 1]
      child.measure(
        MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY),
        MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY),
      )
      child.layout(paddingLeft, top, paddingLeft + width, top + height)
    }
  }

  override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
    setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec), MeasureSpec.getSize(heightMeasureSpec))
  }

  override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
    layoutSegments()
  }

  private var layoutPosted = false

  override fun requestLayout() {
    super.requestLayout()
    // Children changing text or scroll range need a pass the host never runs.
    // Runs during the superclass constructor too, before fields initialize.
    if (!layoutPosted) {
      layoutPosted = true
      post {
        layoutPosted = false
        layoutSegments()
      }
    }
  }
}

/**
 * A text segment in a TextView configured like the measuring StaticLayout
 * (RenderedDocument.buildLayout), which gives selection and text navigation.
 */
private class TextReadingPart(val piece: AccessibleTextPiece, val heading: Boolean, val taskState: Int)

@SuppressLint("ViewConstructor")
internal class TextSegmentView(context: Context) : TextView(context) {
  private var segment: TextSegment? = null
  private var pressedLink = -1
  private val codePaint = Paint()
  private val codePath = Path()
  private val codeBounds = RectF()
  private var readingParts = emptyList<TextReadingPart>()
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
    gravity = Gravity.TOP or Gravity.START
    setBackgroundColor(0)
    highlightColor = 0x33007AFF
  }

  fun setSelectable(selectable: Boolean) {
    if (isTextSelectable != selectable) {
      setTextIsSelectable(selectable)
    }
  }

  fun setSegment(segment: TextSegment, rendered: RenderedDocument) {
    if (segment === this.segment) {
      return
    }
    this.segment = segment
    paint.typeface = rendered.paint.typeface
    setTextSize(TypedValue.COMPLEX_UNIT_PX, rendered.paint.textSize)
    setTextColor(rendered.paint.color)
    setText(segment.text, BufferType.SPANNABLE)
    readingParts = buildReadingParts(segment)
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

  private fun buildReadingParts(segment: TextSegment): List<TextReadingPart> = buildList {
    for (block in segment.accessibleBlocks) {
      val start = block.start.coerceIn(0, segment.text.length)
      val end = block.end.coerceIn(start, segment.text.length)
      linkPieces(segment.text, start, end).forEachIndexed { index, piece ->
        add(TextReadingPart(piece, index == 0 && block.heading,
          if (index == 0) block.taskState else 0))
      }
    }
  }

  private fun accessiblePart(index: Int): MarkdownAccessibility.Entry? {
    val segment = segment ?: return null
    val layout = layout ?: return null
    val part = readingParts.getOrNull(index) ?: return null
    val start = part.piece.start
    val end = part.piece.end
    val visible = segment.text.subSequence(start, end).toString()
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
    return MarkdownAccessibility.Entry(
      text = label,
      bounds = bounds,
      heading = part.heading,
      linkPress = if (link == null) null else ({ findHost()?.pressLink(link) }),
    )
  }

  override fun onDraw(canvas: Canvas) {
    drawInlineCodeBackgrounds(canvas)
    super.onDraw(canvas)
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
      pressedLink = linkAt(layout, x, y)
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

  private fun findHost(): DownstreamMarkdownView? {
    var parent = parent
    while (parent != null && parent !is DownstreamMarkdownView) {
      parent = parent.parent
    }
    return parent as? DownstreamMarkdownView
  }
}

/**
 * One table: fixed column widths in a horizontal scroll view, with container
 * bars and the list marker drawn in the leading margin.
 */
internal class TableSegmentView(context: Context) : FrameLayout(context) {
  private val scroll = HorizontalScrollView(context).apply {
    isHorizontalScrollBarEnabled = true
    isFillViewport = false
    overScrollMode = OVER_SCROLL_NEVER
  }
  private val grid = TableGridView(context)
  private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
  private var segment: TableSegment? = null
  private var sourceEpoch = -1
  private var pendingLeadingDistance: Int? = 0

  init {
    setWillNotDraw(false)
    scroll.addView(grid, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT))
    addView(scroll)
  }

  fun setSegment(segment: TableSegment, sourceEpoch: Int) {
    if (segment === this.segment) {
      return
    }
    val previous = this.segment
    val sameTable = this.sourceEpoch == sourceEpoch && previous != null &&
      previous.rtl == segment.rtl &&
      previous.accessibilityRows.firstOrNull() == segment.accessibilityRows.firstOrNull() &&
      (previous.identity == segment.identity || previous.identity == 0L || segment.identity == 0L)
    val previousRange = (previous?.tableWidth ?: 0).minus(scroll.width).coerceAtLeast(0)
    pendingLeadingDistance = if (sameTable) {
      pendingLeadingDistance ?: if (segment.rtl) (previousRange - scroll.scrollX).coerceAtLeast(0) else scroll.scrollX
    } else {
      0
    }
    this.segment = segment
    this.sourceEpoch = sourceEpoch
    grid.updateSegment(segment, resetFocus = !sameTable)
    requestLayout()
    invalidate()
  }

  fun resetScroll() {
    sourceEpoch = -1
    pendingLeadingDistance = 0
    requestLayout()
  }

  override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
    val segment = segment ?: return
    val leading = segment.contentLeading.toInt()
    val width = (right - left - leading).coerceAtLeast(0)
    val height = bottom - top
    val oldRange = (segment.tableWidth - scroll.width).coerceAtLeast(0)
    val distance = pendingLeadingDistance ?: if (segment.rtl) {
      (oldRange - scroll.scrollX).coerceAtLeast(0)
    } else {
      scroll.scrollX
    }
    scroll.measure(
      MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY),
    )
    val scrollLeft = if (segment.rtl) 0 else leading
    scroll.layout(scrollLeft, 0, scrollLeft + width, height)
    grid.measure(
      MeasureSpec.makeMeasureSpec(segment.tableWidth, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(segment.tableHeight, MeasureSpec.EXACTLY),
    )
    grid.layout(0, 0, segment.tableWidth, segment.tableHeight)
    val newRange = (segment.tableWidth - width).coerceAtLeast(0)
    scroll.scrollTo((if (segment.rtl) newRange - distance else distance).coerceIn(0, newRange), 0)
    pendingLeadingDistance = null
  }

  override fun onDraw(canvas: Canvas) {
    super.onDraw(canvas)
    val segment = segment ?: return
    val density = segment.density
    for (container in segment.containers) {
      val barColor = container.barColor
      if (barColor != null) {
        paint.color = barColor
        val x = if (segment.rtl) width - container.leading - 5 * density
          else container.leading + 2 * density
        canvas.drawRect(x, 0f, x + 3 * density, height.toFloat(), paint)
      }
      val marker = container.marker
      if (marker != null) {
        val firstCell = segment.cells.firstOrNull()?.firstOrNull() ?: continue
        val baseline = segment.paddingVertical + firstCell.getLineBaseline(0).toFloat()
        val x = if (segment.rtl) width - container.leading - container.width + 6 * density
          else container.leading + container.width - 6 * density - segment.markerPaint.measureText(marker)
        canvas.drawText(marker, x, baseline, segment.markerPaint)
      }
    }
  }
}

private class TableReadingPart(
  val text: String,
  val row: Int,
  val column: Int?,
  val piece: AccessibleTextPiece?,
  val heading: Boolean,
)

private class TableGridView(context: Context) : View(context) {
  private var readingParts = emptyList<TableReadingPart>()
  private val accessibility = MarkdownAccessibility(this, { readingParts.size }) { accessiblePart(it) }
  private var segment: TableSegment? = null
  private val paint = Paint()
  private var pressedLink = -1

  fun updateSegment(segment: TableSegment, resetFocus: Boolean) {
    this.segment = segment
    readingParts = buildReadingParts(segment)
    invalidate()
    accessibility.contentChanged(resetFocus)
  }

  override fun getAccessibilityNodeProvider(): AccessibilityNodeProvider = accessibility

  private fun buildReadingParts(segment: TableSegment): List<TableReadingPart> = buildList {
    segment.cells.forEachIndexed { row, cells ->
      val plain = StringBuilder()
      var first = true
      fun appendPart(value: String, column: Int?, piece: AccessibleTextPiece?) {
        val heading = first && segment.hasHeader && row == 0
        add(TableReadingPart(if (heading) "Header: $value" else value, row, column, piece, heading))
        first = false
      }
      fun flushPlain() {
        if (plain.isNotEmpty()) {
          appendPart(plain.toString(), null, null)
          plain.clear()
        }
      }
      cells.forEachIndexed { column, cell ->
        if (column > 0) plain.append(", ")
        val text = cell.text
        if (text is Spanned) {
          for (piece in linkPieces(text, 0, text.length)) {
            val value = text.subSequence(piece.start, piece.end).toString()
            if (piece.linkIndex == null) {
              plain.append(value)
            } else {
              flushPlain()
              appendPart(value, column, piece)
            }
          }
        } else {
          plain.append(text)
        }
      }
      flushPlain()
      if (first) appendPart("", null, null)
    }
  }

  private fun accessiblePart(index: Int): MarkdownAccessibility.Entry? {
    val segment = segment ?: return null
    val part = readingParts.getOrNull(index) ?: return null
    val top = segment.rowTops[part.row]
    val bottom = segment.rowTops[part.row + 1]
    val bounds = Rect(0, top, segment.tableWidth, bottom)
    val column = part.column
    val piece = part.piece
    if (column != null && piece != null) {
      val cell = segment.cells[part.row][column]
      val selectionPath = Path()
      cell.getSelectionPath(piece.start, piece.end, selectionPath)
      val selectedBounds = RectF()
      selectionPath.computeBounds(selectedBounds, true)
      if (!selectedBounds.isEmpty) {
        selectedBounds.roundOut(bounds)
        val (x, y) = cellOrigin(segment, part.row, column)
        bounds.offset(x, y)
      }
    }
    val link = piece?.linkIndex
    return MarkdownAccessibility.Entry(
      text = part.text,
      bounds = bounds,
      heading = part.heading,
      linkPress = if (link == null) null else ({
        var parent = parent
        while (parent != null && parent !is DownstreamMarkdownView) parent = parent.parent
        (parent as? DownstreamMarkdownView)?.pressLink(link)
      }),
    )
  }

  private fun cellOrigin(segment: TableSegment, row: Int, column: Int): Pair<Int, Int> {
    var x = 0
    for (i in 0 until column) x += segment.columnWidths[i]
    if (segment.rtl) x = segment.tableWidth - 1 - x - segment.columnWidths[column]
    return Pair(x + segment.paddingHorizontal, segment.rowTops[row] + segment.paddingVertical)
  }

  override fun onDraw(canvas: Canvas) {
    val segment = segment ?: return
    val width = segment.tableWidth.toFloat() - 1
    if (segment.hasHeader && segment.rowHeights.isNotEmpty()) {
      paint.color = segment.colors.codeBackground
      canvas.drawRect(0f, 0f, width, segment.rowHeights[0].toFloat(), paint)
    }
    segment.cells.forEachIndexed { row, cells ->
      cells.forEachIndexed { column, cell ->
        val (x, y) = cellOrigin(segment, row, column)
        canvas.save()
        canvas.translate(x.toFloat(), y.toFloat())
        cell.draw(canvas)
        canvas.restore()
      }
    }
    paint.color = segment.colors.separator
    var y = 0f
    canvas.drawRect(0f, 0f, width, 1f, paint)
    for (height in segment.rowHeights) {
      y += height
      canvas.drawRect(0f, y, width, y + 1f, paint)
    }
    var x = if (segment.rtl) width else 0f
    canvas.drawRect(x, 0f, x + 1f, y, paint)
    for (columnWidth in segment.columnWidths) {
      x += if (segment.rtl) -columnWidth else columnWidth
      canvas.drawRect(x, 0f, x + 1f, y, paint)
    }
  }

  private fun linkAtPoint(px: Float, py: Float): Int {
    val segment = segment ?: return -1
    segment.cells.forEachIndexed { row, cells ->
      cells.forEachIndexed { column, cell ->
        val (x, y) = cellOrigin(segment, row, column)
        val right = x + segment.columnWidths[column] - 2 * segment.paddingHorizontal
        val bottom = y + segment.rowHeights[row] - 2 * segment.paddingVertical
        if (px >= x - HIT_SLOP && px <= right + HIT_SLOP && py >= y - HIT_SLOP && py <= bottom + HIT_SLOP) {
          return linkAt(cell, px - x, py - y)
        }
      }
    }
    return -1
  }

  @SuppressLint("ClickableViewAccessibility")
  override fun onTouchEvent(event: MotionEvent): Boolean {
    when (event.actionMasked) {
      MotionEvent.ACTION_DOWN -> {
        pressedLink = linkAtPoint(event.x, event.y)
        return pressedLink >= 0
      }
      MotionEvent.ACTION_UP -> {
        val link = pressedLink
        pressedLink = -1
        if (link >= 0 && linkAtPoint(event.x, event.y) == link) {
          var parent = parent
          while (parent != null && parent !is DownstreamMarkdownView) parent = parent.parent
          (parent as? DownstreamMarkdownView)?.pressLink(link)
          return true
        }
      }
      MotionEvent.ACTION_CANCEL -> pressedLink = -1
    }
    return pressedLink >= 0
  }
}

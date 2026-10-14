package com.downstream

import android.annotation.SuppressLint
import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Rect
import android.graphics.RectF
import android.text.Spanned
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewTreeObserver
import android.view.accessibility.AccessibilityNodeProvider
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.PopupMenu

/**
 * One table: a toolbar, a header row that stays put while the rows scroll under
 * it, and the grid itself scrolling on both axes inside a cap.
 *
 * The header is a sibling of the scrolling body rather than a row of it, because
 * a scroll view's own children move with the content and a header that scrolled
 * away is the whole thing this view exists to prevent: a column means nothing
 * without its name. It is drawn by a second grid reading the same segment, so the
 * strip and the row it covers cannot disagree about a column's width.
 */
internal class TableSegmentView(context: Context) : FrameLayout(context) {
  /** The horizontal scroller around the grid. Exists on every table, capped or not. */
  private val scroll = HorizontalScrollView(context).apply {
    isHorizontalScrollBarEnabled = true
    isFillViewport = false
    overScrollMode = OVER_SCROLL_NEVER
  }

  /**
   * The vertical scroller, which scrolls only while the table is past its cap.
   *
   * `CodeViewport` rather than a fresh scroll view, because the drag rule is the
   * hard part and it is already written and tested there: a vertical drag belongs
   * to this table only while it can scroll vertically, and the chat list takes it
   * over at the edge.
   */
  private val viewport = CodeViewport(context)

  /** The pinned header, in its own scroller so it follows the horizontal offset. */
  private val headerScroll = HorizontalScrollView(context).apply {
    isHorizontalScrollBarEnabled = false
    isFillViewport = false
    overScrollMode = OVER_SCROLL_NEVER
  }
  private val headerGrid = TableGridView(context).apply { headerOnly = true }
  private val grid = TableGridView(context)
  private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
  private val toolbar = LinearLayout(context).apply {
    orientation = LinearLayout.HORIZONTAL
    gravity = Gravity.END or Gravity.CENTER_VERTICAL
  }
  private val copyButton = newControlButton(context, controlDrawable("copy", R.drawable.ds_ic_copy))
  private val downloadButton = newControlButton(context, controlDrawable("download", R.drawable.ds_ic_download))
  private val expandButton = newControlButton(context, controlDrawable("expand", R.drawable.ds_ic_expand))
  private val fullscreenButton = newControlButton(context, controlDrawable("tableFullscreen", R.drawable.ds_ic_fullscreen))
  private var segment: TableSegment? = null
  private var sourceEpoch = -1
  private var pendingLeadingDistance: Int? = 0
  private var pendingVerticalOffset: Int? = null
  private var controlLabels: List<String> = DEFAULT_LABELS
  private var controls = 0
  private var controlsEnabled = false
  private var controlHost: ControlHost? = null
  /** The toolbar row the measurement reserved, from the document's style. */
  private var chromeRowHeight = 0
  private var clipObserver: ViewTreeObserver? = null
  private val visibleClipListener = ViewTreeObserver.OnPreDrawListener {
    grid.refreshVisibleClip()
    headerGrid.refreshVisibleClip()
    true
  }

  /** True while the table's rows are still arriving, which is what pins it. */
  var isStreaming = false
    set(value) {
      if (field == value) return
      field = value
      viewport.streaming = value
    }

  /**
   * This table's own cap and expand choice, as the host resolved them from the
   * whole layout. A field rather than a call because the measurement in the engine
   * decided it, and the view must agree: a table drawn taller than the box the
   * node measured is a table clipped in the wrong place.
   */
  var tableLayout = TableBlockLayout()
    set(value) {
      if (field == value) return
      field = value
      requestLayout()
    }

  init {
    setWillNotDraw(false)
    scroll.addView(grid, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT))
    viewport.addView(scroll, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))
    headerScroll.addView(headerGrid, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT))
    // One scroller reads the other's offset, so the strip cannot lag a frame
    // behind the row it covers.
    headerScroll.setOnScrollChangeListener { _, scrollX, _, _, _ ->
      if (scroll.scrollX != scrollX) scroll.scrollTo(scrollX, 0)
      updateGridClips()
    }
    scroll.setOnScrollChangeListener { _, scrollX, _, _, _ ->
      if (headerScroll.scrollX != scrollX) headerScroll.scrollTo(scrollX, 0)
      updateGridClips()
    }
    viewport.setOnScrollChangeListener { _, _, _, _, _ -> updateGridClips() }
    addView(toolbar)
    addView(viewport)
    addView(headerScroll)
    toolbar.addView(expandButton)
    toolbar.addView(fullscreenButton)
    toolbar.addView(copyButton)
    toolbar.addView(downloadButton)
    copyButton.setOnClickListener { showFormats(it, "copy") }
    downloadButton.setOnClickListener { showFormats(it, "download") }
    fullscreenButton.setOnClickListener {
      val ordinal = segment?.ordinal ?: -1
      if (ordinal >= 0) controlHost?.onTableFullscreen(ordinal)
    }
    expandButton.setOnClickListener {
      val ordinal = segment?.ordinal ?: -1
      if (ordinal >= 0) controlHost?.onToggleTableExpand(ordinal, !tableLayout.expanded)
    }
  }

  /** This table's ordinal among the document's tables, or -1. */
  val ordinal: Int get() = segment?.ordinal ?: -1

  fun setControlHost(host: ControlHost?) {
    controlHost = host
  }

  fun setControlLabels(value: List<String>) {
    controlLabels = if (value.size >= DEFAULT_LABELS.size) value else DEFAULT_LABELS
    updateButtons()
  }

  fun setControls(controls: Int) {
    if (controls == this.controls) return
    this.controls = controls
    updateButtons()
  }

  fun setControlsEnabled(enabled: Boolean) {
    if (enabled == controlsEnabled) return
    controlsEnabled = enabled
    updateButtons()
  }

  fun setToolbarVisible(visible: Boolean) {
    val next = if (visible) View.VISIBLE else View.GONE
    if (toolbar.visibility == next) return
    toolbar.visibility = next
    updateButtons()
    requestLayout()
  }

  private fun updateButtons() {
    val shown = toolbar.visibility == View.VISIBLE
    fun button(visible: Boolean) = if (shown && visible) View.VISIBLE else View.GONE
    copyButton.visibility = button(controls and CONTROL_TABLE_COPY != 0)
    downloadButton.visibility = button(controls and CONTROL_TABLE_DOWNLOAD != 0)
    fullscreenButton.visibility = button(controls and CONTROL_TABLE_FULLSCREEN != 0)
    // Expand appears only on a table past its cap, because that is the only table
    // the cap can come back on. It stays once expanded, so the reader can undo it.
    expandButton.visibility = button(controls and CONTROL_TABLE_EXPAND != 0 && (capped || tableLayout.expanded))
    for (view in listOf(copyButton, downloadButton, fullscreenButton, expandButton)) {
      view.isEnabled = controlsEnabled && view.visibility == View.VISIBLE
      view.alpha = if (view.isEnabled) 1f else 0.4f
    }
    copyButton.contentDescription = controlLabels[LABEL_COPY_TABLE]
    downloadButton.contentDescription = controlLabels[LABEL_DOWNLOAD_TABLE]
    fullscreenButton.contentDescription = controlLabels[LABEL_VIEW_FULLSCREEN]
    expandButton.setImageResource(
      if (tableLayout.expanded) controlDrawable("collapse", R.drawable.ds_ic_collapse)
      else controlDrawable("expand", R.drawable.ds_ic_expand),
    )
    expandButton.contentDescription = controlLabels[
      if (tableLayout.expanded) LABEL_COLLAPSE_TABLE else LABEL_EXPAND_TABLE,
    ]
  }

  /**
   * True while the grid is taller than the box the cap gives it, which is what
   * decides whether this table scrolls vertically and whether the expand button
   * has anything to offer.
   */
  private var capped = false

  /**
   * The formats a table can be copied or saved in. Every format also goes on
   * the clipboard as HTML, so a paste lands as a table rather than as text.
   */
  private fun showFormats(anchor: View, action: String) {
    val segment = this.segment ?: return
    val formats = if (action == "copy") {
      listOf(
        LABEL_FORMAT_MARKDOWN to "markdown",
        LABEL_FORMAT_CSV to "csv",
        LABEL_FORMAT_TSV to "tsv",
      )
    } else {
      listOf(LABEL_FORMAT_CSV to "csv", LABEL_FORMAT_MARKDOWN to "markdown")
    }
    val menu = PopupMenu(context, anchor)
    for ((label, format) in formats) {
      menu.menu.add(0, 0, menu.menu.size(), controlLabels[label]).setOnMenuItemClickListener {
        controlHost?.onControl(
          BLOCK_TABLE_ROW,
          segment.blockIndex,
          segment.blockCount,
          "",
          action,
          format,
        )
        true
      }
    }
    menu.show()
  }

  fun setSegment(segment: TableSegment, sourceEpoch: Int, chromeRowHeight: Int) {
    if (chromeRowHeight != this.chromeRowHeight) {
      this.chromeRowHeight = chromeRowHeight
      requestLayout()
    }
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
    val tint = ColorStateList.valueOf(segment.colors.toolbarButtonColor)
    copyButton.imageTintList = tint
    downloadButton.imageTintList = tint
    expandButton.imageTintList = tint
    fullscreenButton.imageTintList = tint
    // Both grids share the segment and immutable reading parts. The strip draws
    // only the header while retaining the same accessible entries as the body.
    val readingParts = TableReadingParts(segment)
    headerGrid.updateSegment(segment, readingParts, resetFocus = false)
    headerGrid.contentDescription = segment.accessibilityRows.firstOrNull()
    grid.updateSegment(segment, readingParts, resetFocus = !sameTable)
    requestLayout()
    invalidate()
  }

  fun captureViewportState(): IntArray {
    val current = segment
    val range = ((current?.tableWidth ?: 0) - scroll.width).coerceAtLeast(0)
    val leading = pendingLeadingDistance ?: if (current?.rtl == true) {
      (range - scroll.scrollX).coerceAtLeast(0)
    } else scroll.scrollX
    return intArrayOf(leading, pendingVerticalOffset ?: viewport.scrollY, viewport.capturePinState())
  }

  fun restoreViewportState(state: IntArray) {
    pendingLeadingDistance = state[0]
    pendingVerticalOffset = state[1]
    viewport.restorePinState(state[2])
    requestLayout()
  }

  fun clearViewportContent() {
    segment = null
    sourceEpoch = -1
    pendingLeadingDistance = 0
    pendingVerticalOffset = 0
    isStreaming = false
    viewport.restorePinState(1)
    grid.clearViewportContent()
    headerGrid.clearViewportContent()
    headerGrid.contentDescription = null
  }

  fun resetScroll() {
    sourceEpoch = -1
    pendingLeadingDistance = 0
    pendingVerticalOffset = 0
    viewport.restorePinState(1)
    requestLayout()
  }

  override fun onAttachedToWindow() {
    super.onAttachedToWindow()
    val observer = viewTreeObserver
    observer.addOnPreDrawListener(visibleClipListener)
    clipObserver = observer
  }

  override fun onDetachedFromWindow() {
    clipObserver?.let { if (it.isAlive) it.removeOnPreDrawListener(visibleClipListener) }
    clipObserver = null
    grid.resetAncestorClip()
    headerGrid.resetAncestorClip()
    viewport.streaming = false
    super.onDetachedFromWindow()
  }

  private fun toolbarHeight(): Int = if (toolbar.visibility == View.VISIBLE) chromeRowHeight else 0

  override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
    val segment = segment ?: return
    // In this view's own coordinates, and at the reserved row height rather than
    // the toolbar's measured one, which as a frame child is the whole table.
    val toolbarHeight = toolbarHeight()
    val leading = segment.contentLeading.toInt()
    val width = (right - left - leading).coerceAtLeast(0)
    val bodyTop = toolbarHeight
    val totalHeight = (bottom - top - toolbarHeight).coerceAtLeast(0)
    val headerHeight = if (segment.hasHeader && segment.rowHeights.isNotEmpty()) {
      segment.rowHeights[0]
    } else {
      0
    }

    // The cap decides what this table shows, so it is decided once here and the
    // layout engine measured the same number: `tableHeight` put the cap on the
    // grid box with the toolbar outside it, and this is that box.
    val boxHeight = segment.visibleGridHeight(tableLayout)
    capped = tableLayout.maxHeight > 0f && segment.tableHeight > boxHeight
    updateButtons()

    toolbar.measure(
      MeasureSpec.makeMeasureSpec(right - left, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(toolbarHeight, MeasureSpec.EXACTLY),
    )
    toolbar.layout(0, 0, right - left, toolbarHeight)
    // The header strip takes its own row above the scroller rather than floating
    // over the first row, so a reader never sees two headers.
    headerScroll.visibility = if (headerHeight > 0) View.VISIBLE else View.GONE
    val scrollLeft = if (segment.rtl) 0 else leading
    val bodyHeight = (totalHeight - headerHeight).coerceAtLeast(0)

    // The horizontal range is the same for both scrollers, so the strip is
    // restored from the same number rather than from its own previous offset.
    val oldRange = (segment.tableWidth - scroll.width).coerceAtLeast(0)
    val distance = pendingLeadingDistance ?: if (segment.rtl) {
      (oldRange - scroll.scrollX).coerceAtLeast(0)
    } else {
      scroll.scrollX
    }
    // Give the horizontal scroller its full content height before the vertical
    // viewport measures it; MATCH_PARENT otherwise measures an empty body.
    grid.layoutParams.height = segment.tableHeight
    scroll.layoutParams.height = (segment.tableHeight - headerHeight).coerceAtLeast(0)
    viewport.measure(
      MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(bodyHeight, MeasureSpec.EXACTLY),
    )
    headerScroll.measure(
      MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(headerHeight.coerceAtLeast(0), MeasureSpec.EXACTLY),
    )
    grid.measure(
      MeasureSpec.makeMeasureSpec(segment.tableWidth, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(segment.tableHeight, MeasureSpec.EXACTLY),
    )
    grid.layout(0, 0, segment.tableWidth, segment.tableHeight)
    headerGrid.measure(
      MeasureSpec.makeMeasureSpec(segment.tableWidth, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(headerHeight.coerceAtLeast(0), MeasureSpec.EXACTLY),
    )
    // Measure the scrollers before layout so their children get current bounds.
    viewport.layout(scrollLeft, bodyTop + headerHeight, scrollLeft + width, bodyTop + totalHeight)
    headerScroll.layout(scrollLeft, bodyTop, scrollLeft + width, bodyTop + headerHeight)
    headerGrid.layout(0, 0, segment.tableWidth, headerHeight)
    // The pinned strip owns the header. Move the full grid's header above the
    // body's clip, leaving only data rows inside the vertical viewport.
    grid.layout(0, -headerHeight, segment.tableWidth, segment.tableHeight - headerHeight)
    val newRange = (segment.tableWidth - width).coerceAtLeast(0)
    val restored = (if (segment.rtl) newRange - distance else distance).coerceIn(0, newRange)
    scroll.scrollTo(restored, 0)
    headerScroll.scrollTo(restored, 0)
    pendingLeadingDistance = null
    pendingVerticalOffset?.let { viewport.scrollTo(0, it) }
    pendingVerticalOffset = null

    // The vertical scroller scrolls only while the table is past its cap, which is
    // the whole difference between a capped table and an uncapped one.
    viewport.isVerticalScrollBarEnabled = capped
    if (capped && isStreaming) {
      // Follow the newest row, and not a snapshot: the pin has to land after the
      // content has its new height, which the layout above has now given it.
      viewport.followNewestLine()
    }
    updateGridClips()
  }

  /** Internal scroll clips also bound hardware display-list recording. */
  private fun updateGridClips() {
    val segment = segment ?: return
    val headerHeight = if (segment.hasHeader) segment.rowHeights.firstOrNull() ?: 0 else 0
    grid.setDrawViewport(scroll.scrollX, headerHeight + viewport.scrollY,
      scroll.scrollX + scroll.width, headerHeight + viewport.scrollY + viewport.height)
    headerGrid.setDrawViewport(headerScroll.scrollX, 0,
      headerScroll.scrollX + headerScroll.width, headerHeight)
  }

  override fun onDraw(canvas: Canvas) {
    super.onDraw(canvas)
    val segment = segment ?: return
    val density = segment.density
    val toolbarHeight = toolbarHeight()
    for (container in segment.containers) {
      val barColor = container.barColor
      if (barColor != null) {
        paint.color = barColor
        val x = if (segment.rtl) width - container.leading - 5 * density
          else container.leading + 2 * density
        canvas.drawRect(x, toolbarHeight.toFloat(), x + 3 * density, height.toFloat(), paint)
      }
      val marker = container.marker
      if (marker != null) {
        val firstCell = segment.cells.firstOrNull()?.firstOrNull() ?: continue
        val baseline = toolbarHeight + segment.paddingVertical + firstCell.getLineBaseline(0).toFloat()
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

/** One demand-built reading order shared by the body and pinned header. */
private class TableReadingParts(private val segment: TableSegment) {
  private var cached: List<TableReadingPart>? = null

  fun get(): List<TableReadingPart> = cached ?: buildTableReadingParts(segment).also { cached = it }
}

private fun buildTableReadingParts(segment: TableSegment): List<TableReadingPart> = buildList {
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

private class TableGridView(context: Context) : View(context) {
  /**
   * Draws only the header row, which is what the pinned strip is: the same grid,
   * one row, so the strip and the row it covers cannot disagree about a width.
   */
  var headerOnly = false

  /**
   * A table row is one reveal unit, and the grid draws cells rather than a text
   * layout, so the row's alpha comes from the host rather than from a band in this
   * view's own text. One lookup per row, on a table with a few dozen rows, is
   * cheaper than mapping offsets into a layout this view does not have.
   */
  private fun host(): DownstreamMarkdownView? {
    var parent = parent
    while (parent != null && parent !is DownstreamMarkdownView) {
      parent = parent.parent
    }
    return parent as? DownstreamMarkdownView
  }

  private var readingParts: TableReadingParts? = null
  private val accessibility = MarkdownAccessibility(this, { readingParts?.get()?.size ?: 0 }) { accessiblePart(it) }
  private var segment: TableSegment? = null
  private val paint = Paint()
  private var pressedLink = -1
  private val drawClip = Rect()
  private val drawWindow = TableDrawWindow()
  private val internalViewport = Rect()
  private val drawViewport = Rect()
  private val nextDrawViewport = Rect()
  private val ancestorClip = Rect()
  private var hasDrawViewport = false

  fun setDrawViewport(left: Int, top: Int, right: Int, bottom: Int) {
    internalViewport.set(left, top, right, bottom)
    hasDrawViewport = true
    refreshVisibleClip()
  }

  /** Pre-draw catches ancestor scrolling and translations before display-list recording. */
  fun refreshVisibleClip() {
    if (!hasDrawViewport) return
    nextDrawViewport.set(internalViewport)
    // Before attachment/layout, a visible-rect query cannot describe this grid.
    // Preserve full internal drawing until there is reliable ancestor geometry.
    if (isAttachedToWindow && isLaidOut && width > 0 && height > 0) {
      if (!getLocalVisibleRect(ancestorClip) || !nextDrawViewport.intersect(ancestorClip)) {
        nextDrawViewport.setEmpty()
      }
    }
    applyDrawViewport()
  }

  fun resetAncestorClip() {
    if (!hasDrawViewport) return
    nextDrawViewport.set(internalViewport)
    applyDrawViewport()
  }

  private fun applyDrawViewport() {
    if (drawViewport == nextDrawViewport) return
    drawViewport.set(nextDrawViewport)
    invalidate()
  }

  fun clearViewportContent() {
    segment = null
    readingParts = null
    pressedLink = -1
    hasDrawViewport = false
    accessibility.contentChanged()
    invalidate()
  }

  fun updateSegment(segment: TableSegment, readingParts: TableReadingParts, resetFocus: Boolean) {
    this.segment = segment
    this.readingParts = readingParts
    invalidate()
    accessibility.contentChanged(resetFocus)
  }

  override fun getAccessibilityNodeProvider(): AccessibilityNodeProvider = accessibility

  private fun accessiblePart(index: Int): MarkdownAccessibility.Entry? {
    val segment = segment ?: return null
    val part = readingParts?.get()?.getOrNull(index) ?: return null
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
    return Pair(segment.columnLefts[column] + segment.paddingHorizontal,
      segment.rowTops[row] + segment.paddingVertical)
  }

  override fun onDraw(canvas: Canvas) {
    val segment = segment ?: return
    val hairline = segment.borderWidth.coerceAtLeast(1)
    val width = segment.tableWidth.toFloat() - hairline
    val rows = if (headerOnly) minOf(1, segment.cells.size) else segment.cells.size
    if (!canvas.getClipBounds(drawClip)) return
    if (hasDrawViewport && !drawClip.intersect(drawViewport)) return
    drawWindow.build(
      segment.rowTops, segment.columnLefts, segment.columnWidths,
      rows, segment.rtl, segment.tableWidth, hairline,
      kotlin.math.ceil(segment.markerPaint.textSize).toInt(),
      drawClip.left, drawClip.top, drawClip.right, drawClip.bottom,
    )
    if (segment.hasHeader && drawWindow.firstRow == 0 && drawWindow.endRow > 0) {
      paint.color = segment.colors.tableHeaderBackground
      canvas.drawRect(0f, 0f, width, segment.rowHeights[0].toFloat(), paint)
    }
    // The same Canvas clip selects body rows and the separate pinned header.
    // Accessibility still contains every row and link, independently of drawing.
    val revealHost = host()
    for (row in drawWindow.firstRow until drawWindow.endRow) {
      val cells = segment.cells[row]
      val alpha = revealHost?.blockAlpha(segment.blockIndex + row) ?: 1f
      val top = segment.rowTops[row].toFloat()
      val bottom = segment.rowTops[row + 1].toFloat()
      val checkpoint = if (alpha < 1f) {
        canvas.saveLayerAlpha(android.graphics.RectF(0f, top, width, bottom), (alpha * 255f).toInt())
      } else {
        -1
      }
      if (row >= if (segment.hasHeader) 1 else 0) {
        val stripe = segment.colors.rowStripe(row - if (segment.hasHeader) 1 else 0)
        if (stripe != null) {
          paint.color = stripe
          canvas.drawRect(0f, top, width, bottom, paint)
        }
      }
      for (column in drawWindow.firstColumn until minOf(drawWindow.endColumn, cells.size)) {
        val x = segment.columnLefts[column] + segment.paddingHorizontal
        val y = segment.rowTops[row] + segment.paddingVertical
        canvas.save()
        canvas.translate(x.toFloat(), y.toFloat())
        cells[column].draw(canvas)
        canvas.restore()
      }
      if (checkpoint >= 0) canvas.restoreToCount(checkpoint)
    }
    paint.color = segment.colors.separator
    for (index in drawWindow.firstRowBorder until drawWindow.endRowBorder) {
      val y = segment.rowTops[index].toFloat()
      if (index == 0) canvas.drawRect(0f, 0f, width, hairline.toFloat(), paint)
      else canvas.drawRect(0f, y - hairline, width, y, paint)
    }
    val bottom = segment.rowTops[rows].toFloat()
    for (index in drawWindow.firstColumnBorder until drawWindow.endColumnBorder) {
      val x = drawWindow.columnBorder(index, segment.columnLefts, segment.columnWidths,
        segment.rtl, segment.tableWidth, hairline).toFloat()
      canvas.drawRect(x, 0f, x + hairline, bottom, paint)
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

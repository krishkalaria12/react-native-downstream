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
import android.view.accessibility.AccessibilityNodeProvider
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.PopupMenu

/**
 * One table: fixed column widths in a horizontal scroll view, a toolbar above
 * the grid, and container bars and the list marker in the leading margin.
 */
internal class TableSegmentView(context: Context) : FrameLayout(context) {
  private val scroll = HorizontalScrollView(context).apply {
    isHorizontalScrollBarEnabled = true
    isFillViewport = false
    overScrollMode = OVER_SCROLL_NEVER
  }
  private val grid = TableGridView(context)
  private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
  private val toolbar = LinearLayout(context).apply {
    orientation = LinearLayout.HORIZONTAL
    gravity = Gravity.END or Gravity.CENTER_VERTICAL
  }
  private val copyButton = newControlButton(context, R.drawable.ds_ic_copy)
  private val downloadButton = newControlButton(context, R.drawable.ds_ic_download)
  private var segment: TableSegment? = null
  private var sourceEpoch = -1
  private var pendingLeadingDistance: Int? = 0
  private var controlLabels: List<String> = DEFAULT_LABELS
  private var controls = 0
  private var controlsEnabled = false
  private var controlHost: ControlHost? = null
  /** The toolbar row the measurement reserved, from the document's style. */
  private var chromeRowHeight = 0

  init {
    setWillNotDraw(false)
    scroll.addView(grid, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT))
    addView(toolbar)
    addView(scroll)
    toolbar.addView(copyButton)
    toolbar.addView(downloadButton)
    copyButton.setOnClickListener { showFormats(it, "copy") }
    downloadButton.setOnClickListener { showFormats(it, "download") }
  }

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
    copyButton.visibility = if (shown && controls and CONTROL_TABLE_COPY != 0) View.VISIBLE else View.GONE
    downloadButton.visibility =
      if (shown && controls and CONTROL_TABLE_DOWNLOAD != 0) View.VISIBLE else View.GONE
    for (button in listOf(copyButton, downloadButton)) {
      button.isEnabled = controlsEnabled && button.visibility == View.VISIBLE
      button.alpha = if (button.isEnabled) 1f else 0.4f
    }
    copyButton.contentDescription = controlLabels[LABEL_COPY_TABLE]
    downloadButton.contentDescription = controlLabels[LABEL_DOWNLOAD_TABLE]
  }

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
    grid.updateSegment(segment, resetFocus = !sameTable)
    requestLayout()
    invalidate()
  }

  fun resetScroll() {
    sourceEpoch = -1
    pendingLeadingDistance = 0
    requestLayout()
  }

  private fun toolbarHeight(): Int = if (toolbar.visibility == View.VISIBLE) chromeRowHeight else 0

  override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
    val segment = segment ?: return
    // In this view's own coordinates, and at the reserved row height rather than
    // the toolbar's measured one, which as a frame child is the whole table.
    val toolbarHeight = toolbarHeight()
    toolbar.measure(
      MeasureSpec.makeMeasureSpec(right - left, MeasureSpec.EXACTLY),
      MeasureSpec.makeMeasureSpec(toolbarHeight, MeasureSpec.EXACTLY),
    )
    toolbar.layout(0, 0, right - left, toolbarHeight)
    val leading = segment.contentLeading.toInt()
    val width = (right - left - leading).coerceAtLeast(0)
    val height = (bottom - top - toolbarHeight).coerceAtLeast(0)
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
    // A toolbar above the grid takes layout space rather than floating over
    // the rows, so a reader never taps a button instead of a cell.
    val scrollLeft = (if (segment.rtl) 0 else leading) + 0
    val scrollTop = toolbarHeight
    scroll.layout(scrollLeft, scrollTop, scrollLeft + width, scrollTop + height)
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

private class TableGridView(context: Context) : View(context) {
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

  /** The alpha of one row, or 1 when the fade is off. */
  private fun rowAlpha(block: Int): Float = host()?.blockAlpha(block) ?: 1f
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
    return Pair(segment.columnLefts[column] + segment.paddingHorizontal,
      segment.rowTops[row] + segment.paddingVertical)
  }

  override fun onDraw(canvas: Canvas) {
    val segment = segment ?: return
    val hairline = segment.borderWidth.coerceAtLeast(1)
    val width = segment.tableWidth.toFloat() - hairline
    if (segment.hasHeader && segment.rowHeights.isNotEmpty()) {
      // The header row's own fill, which a theme can set apart from the body.
      paint.color = segment.colors.tableHeaderBackground
      canvas.drawRect(0f, 0f, width, segment.rowHeights[0].toFloat(), paint)
    }
    segment.cells.forEachIndexed { row, cells ->
      // A new row fades as one unit, so the fill, the cells and the cell text all
      // share one alpha. The layer is bounded to the row's own height, so a table
      // with fifty rows does not allocate fifty screen-sized layers.
      val alpha = rowAlpha(segment.blockIndex + row)
      val top = segment.rowTops[row].toFloat()
      val bottom = segment.rowTops[row + 1].toFloat()
      val checkpoint = if (alpha < 1f) {
        canvas.saveLayerAlpha(android.graphics.RectF(0f, top, width, bottom), (alpha * 255f).toInt())
      } else {
        -1
      }
      // The zebra fill is drawn under the cells, per row rather than per cell,
      // so an empty trailing cell in a short row still gets the row's fill.
      if (row >= if (segment.hasHeader) 1 else 0) {
        val stripe = segment.colors.rowStripe(row - if (segment.hasHeader) 1 else 0)
        if (stripe != null) {
          paint.color = stripe
          canvas.drawRect(0f, top, width, bottom, paint)
        }
      }
      cells.forEachIndexed { column, cell ->
        val (x, y) = cellOrigin(segment, row, column)
        canvas.save()
        canvas.translate(x.toFloat(), y.toFloat())
        cell.draw(canvas)
        canvas.restore()
      }
      if (checkpoint >= 0) {
        canvas.restoreToCount(checkpoint)
      }
    }
    paint.color = segment.colors.separator
    var y = 0f
    canvas.drawRect(0f, 0f, width, hairline.toFloat(), paint)
    for (height in segment.rowHeights) {
      y += height
      canvas.drawRect(0f, y - hairline, width, y, paint)
    }
    var x = if (segment.rtl) width else 0f
    canvas.drawRect(x, 0f, x + hairline, y, paint)
    for (columnWidth in segment.columnWidths) {
      x += if (segment.rtl) -columnWidth else columnWidth
      canvas.drawRect(x, 0f, x + hairline, y, paint)
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

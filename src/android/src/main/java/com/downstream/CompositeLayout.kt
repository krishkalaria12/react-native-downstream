package com.downstream

import android.graphics.Canvas
import android.graphics.Path
import android.graphics.Rect
import android.graphics.RenderNode
import android.os.Build
import android.text.Layout
import android.text.TextPaint
import androidx.annotation.RequiresApi
import kotlin.math.ceil

/**
 * One run of lines a [CompositeLayout] shows from a layout of its own.
 *
 * [base] is the composite offset of the part layout's offset zero, so a part laid
 * out over a range of a longer text, or over a copy that starts before its first
 * line, maps back by one addition. [lines] counts from the part layout's first
 * line, which lets a part show only the stable leading lines of a longer layout.
 */
internal class LayoutPart(
  val layout: Layout,
  val base: Int,
  val lines: Int,
  /**
   * The recorded drawing of a part that never changes, or null for one that is
   * drawn afresh. Owned by whatever outlives a single snapshot, so a part that is
   * reused across snapshots is recorded once rather than on every frame.
   */
  val recording: PartRecording? = null,
)

/**
 * A part's lines recorded once, then replayed.
 *
 * A text layout's draw walks every line's spans and shapes every run into display
 * list operations, and a growing code block is redrawn whenever its last line
 * changes. The lines above the last one do not change, so their operations are
 * recorded into a render node the first time and the next frames only reference
 * it. Hardware canvases only: a software canvas draws the layout directly.
 */
internal class PartRecording {
  private var node: Any? = null

  /** Whether the part holds a recording a hardware canvas can replay. */
  val isRecorded: Boolean
    get() = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && (node as? RenderNode)?.hasDisplayList() == true

  @RequiresApi(Build.VERSION_CODES.Q)
  fun draw(canvas: Canvas, layout: Layout, lines: Int) {
    var recorded = node as? RenderNode
    // The renderer drops every display list in a window's tree when it releases
    // its resources, on trim and when a window goes away, so a recording that is
    // gone is made again rather than replayed as nothing.
    if (recorded == null || !recorded.hasDisplayList()) {
      recorded = record(layout, lines)
      node = recorded
    }
    canvas.drawRenderNode(recorded)
  }

  @RequiresApi(Build.VERSION_CODES.Q)
  private fun record(layout: Layout, lines: Int): RenderNode {
    val height = layout.getLineTop(lines)
    // The ink's own extent rather than the layout's width, which for unwrapped
    // code is the largest size a view can measure. Glyph overhang past it is not
    // clipped, because the node does not clip to its bounds.
    var right = 0f
    for (line in 0 until lines) right = maxOf(right, layout.getLineRight(line))
    val width = ceil(right.toDouble()).toInt().coerceIn(1, layout.width.coerceAtLeast(1))
    return RenderNode("downstream-text").apply {
      setPosition(0, 0, width, height.coerceAtLeast(1))
      setClipToBounds(false)
      val canvas = beginRecording()
      try {
        canvas.clipRect(0, 0, layout.width, height)
        layout.draw(canvas)
      } finally {
        endRecording()
      }
    }
  }
}

/**
 * One text layout made of several, each laid out once and then reused.
 *
 * Global offsets and line geometry are answered by the part that owns the line,
 * which is what reveal, the caret and accessibility read. Platform `TextView`
 * never receives this text, so nothing shapes the whole of it again during
 * assignment, measurement or drawing.
 */
internal class CompositeLayout(
  source: CharSequence,
  paint: TextPaint,
  width: Int,
  parts: List<LayoutPart>,
) : Layout(source, TextPaint(paint), width, Alignment.ALIGN_NORMAL, 1f, 0f) {
  private class Part(
    val layout: Layout,
    val base: Int,
    val start: Int,
    val lines: Int,
    val firstLine: Int,
    val top: Int,
    val recording: PartRecording?,
  ) {
    val bottom = top + layout.getLineTop(lines)
  }
  private val parts: List<Part>
  private val lines: Int
  private val totalHeight: Int

  init {
    require(parts.isNotEmpty())
    var line = 0
    var top = 0
    this.parts = parts.map { part ->
      Part(part.layout, part.base, part.base + part.layout.getLineStart(0), part.lines, line, top, part.recording).also {
        line += part.lines
        top += part.layout.getLineTop(part.lines)
      }
    }
    lines = line
    totalHeight = top
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

  /** A composite offset in [part]'s own layout, kept inside the text it holds. */
  private fun local(part: Part, offset: Int): Int = (offset - part.base).coerceIn(0, part.layout.text.length)

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
    return part.base + part.layout.getLineStart(line - part.firstLine)
  }
  override fun getLineDescent(line: Int): Int {
    val part = partForLine(line)
    return part.layout.getLineDescent(line - part.firstLine)
  }
  override fun getParagraphDirection(line: Int): Int {
    val part = partForLine(line)
    return part.layout.getParagraphDirection(line - part.firstLine)
  }
  override fun getLineContainsTab(line: Int): Boolean {
    val part = partForLine(line)
    return part.layout.getLineContainsTab(line - part.firstLine)
  }
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
    return part.layout.getPrimaryHorizontal(local(part, offset))
  }
  override fun getSecondaryHorizontal(offset: Int): Float {
    val part = partForLine(getLineForOffset(offset.coerceIn(0, text.length)))
    return part.layout.getSecondaryHorizontal(local(part, offset))
  }
  override fun getOffsetForHorizontal(line: Int, horizontal: Float): Int {
    val part = partForLine(line)
    return part.base + part.layout.getOffsetForHorizontal(line - part.firstLine, horizontal)
  }
  /**
   * Each part selects its own lines. A range that crosses into the next part ends
   * the earlier part's selection at its last line's ink, where one layout would
   * fill that line to its width, so a selection's horizontal extent across a seam
   * can differ by a line's hanging whitespace. Ranges inside one part, which is
   * every reveal unit, select exactly as the whole text would.
   */
  override fun getSelectionPath(start: Int, end: Int, destination: Path) {
    destination.rewind()
    if (start >= end) return
    val scratch = Path()
    var index = partIndexForLine(getLineForOffset(start))
    while (index < parts.size) {
      val part = parts[index++]
      if (part.start == end && end > start) {
        // One layout ends a selection that stops at a line's start with an empty
        // rectangle on that line, which still reaches the line's height.
        val line = part.firstLine
        destination.addRect(0f, getLineTop(line).toFloat(), getLineLeft(line), getLineBottom(line).toFloat(), Path.Direction.CW)
      }
      if (part.start >= end) break
      val partEnd = part.base + part.layout.getLineStart(part.lines)
      val from = maxOf(start, part.start) - part.base
      val to = minOf(end, partEnd) - part.base
      if (from < to) {
        part.layout.getSelectionPath(from, to, scratch)
        scratch.offset(0f, part.top.toFloat())
        destination.addPath(scratch)
      }
    }
  }

  private val clip = Rect()

  override fun draw(canvas: Canvas) {
    canvas.getClipBounds(clip)
    var index = partIndexForLine(getLineForVertical(clip.top.coerceAtLeast(0)))
    val recorded = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && canvas.isHardwareAccelerated
    while (index < parts.size) {
      val part = parts[index++]
      if (part.top >= clip.bottom) break
      if (part.bottom <= clip.top) continue
      val saved = canvas.save()
      canvas.translate(0f, part.top.toFloat())
      val recording = part.recording
      if (recorded && recording != null) {
        recording.draw(canvas, part.layout, part.lines)
      } else {
        canvas.clipRect(0, 0, width, part.bottom - part.top)
        part.layout.draw(canvas)
      }
      canvas.restoreToCount(saved)
    }
  }
}

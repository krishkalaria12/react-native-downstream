package com.downstream

import android.graphics.Paint
import android.graphics.Typeface
import android.text.Layout
import android.text.Spanned
import android.text.StaticLayout
import android.text.TextDirectionHeuristics
import android.text.TextPaint
import kotlin.math.ceil
import kotlin.math.max

/** One unit of layout. [gapBefore] is the space above it in the document. */
internal sealed class Segment(val gapBefore: Int) {
  /** The planner groups blocks with one details visibility state. */
  var detailsContainers: List<ContainerDecoration> = emptyList()
}

/**
 * A picture, as its own segment.
 *
 * A picture is a View rather than a span because the bytes come from the app's own
 * image pipeline, which resolves on a background thread and then needs a
 * surface of its own to draw into. A span would have to be redrawn every frame
 * from whichever canvas TextView handed it, and it would have no way to answer
 * "how big is this" before the bytes arrive, which is the whole of the box rule.
 *
 * The alt text is kept because it is what the view shows for a destination that
 * failed, was blocked, or was never requested, and what a screen reader reads.
 */
internal class ImageSegment(
  /** The image's index in the document's image list. */
  val ordinal: Int,
  val url: String,
  val alt: String,
  val title: String,
  /** Where the `![…](…)` sits in the source, for an event that reports a press. */
  val range: IntRange,
  /** Whether the engine's own policy allows this destination. */
  val loadable: Boolean,
  /** The enclosing link's index, or -1, so a press knows whether it is a link. */
  val link: Int,
  gapBefore: Int,
) : Segment(gapBefore) {
  /** The box, which is [imageBox] under whatever state this view holds. */
  fun box(columnWidth: Int, entry: ImageEntry?, metrics: ImageMetrics): ImageBox =
    imageBox(entry, columnWidth.toFloat(), metrics)
}

/**
 * A formula on a line of its own.
 *
 * Its own segment rather than a text span because the engine lays a display
 * formula out once per block and the result is a whole box with its own padding,
 * background, and alignment. The literal stays in [literal] so a copy and a screen
 * reader both produce the writer's source rather than a picture of it.
 */
internal class MathSegment(
  /** The formula's literal, delimiters included, exactly as written. */
  val literal: String,
  /**
   * The layout's advance, height, and depth in em, from `MathLayout.h`. Zero when
   * the layout did not resolve.
   *
   * The metrics rather than the glyphs, because a height is what the frame stack
   * needs and the glyphs are what the drawing package supplies; carrying the whole
   * display list across the bridge to measure it would move a lot of numbers to
   * answer three questions.
   */
  val width: Float,
  val height: Float,
  val depth: Float,
  /** Whether the layout resolved, which is what decides the literal or an error. */
  val resolved: Boolean,
  /** The glyphs, rules and paths to draw, when the layout resolved. */
  val displayList: MathDisplayList?,
  /** True once the engine has given up on the formula, which is when it turns the error color. */
  val failed: Boolean,
  /** The spoken form from `MathSpeech.h`, or the literal when there is none. */
  val speech: String,
  /** Whether the source was a `$$` line fence, which centers by default. */
  val display: Boolean,
  /**
   * The theme's own alignment, as `math.textAlign` holds it: 0 left, 1 center, 2
   * right. Read rather than mapped so the view that centres a formula is the same
   * one that measures it.
   */
  val textAlign: Int,
  gapBefore: Int,
) : Segment(gapBefore)

/**
 * A run of non-table blocks laid out as one text layout. On Android 9 and
 * later [text] is a PrecomputedText, built off the UI thread, so a TextView
 * showing it only breaks lines instead of measuring every glyph again.
 */
internal class TextSegment(
  val text: Spanned,
  val accessibleBlocks: List<AccessibleBlock>,
  /**
   * The blocks that sit inside a `<details>`, with the character range each one
   * occupies.
   *
   * Carried here rather than read from the document at draw time because the
   * segment is the unit of layout and the document is not on the UI thread: a
   * collapsed body is a height question, and the height is asked for on every
   * measurement. Empty for a document with no collapsible block, which is the
   * common case, so the extra walk is nothing.
   */
  val detailsBlocks: List<DetailsBlock> = emptyList(),
  /**
   * The task items in this segment, with the character range each one occupies.
   *
   * Carried for the same reason the collapsible blocks are: the segment is the
   * unit of layout and the document is not on the UI thread, and a press has to
   * know which box it landed on without asking.
   */
  val taskBlocks: List<TaskBlock> = emptyList(),
  /**
   * The custom container's id when the plan grouped this segment for one, or -1.
   *
   * Set at build time from the first block's innermost Custom frame, so a
   * hosted tag is one lookup rather than a walk of the segment's blocks on
   * every measure.
   */
  val customContainer: Int = -1,
  /**
   * The content indent in pixels, from the segment's own container path.
   *
   * Carried because a hosted tag's rectangle starts here: the slot is as wide
   * as the column the text would have taken, which is what the shadow node
   * measured the child at.
   */
  val contentLeading: Int = 0,
  gapBefore: Int,
  /**
   * Where the caret's placeholder sits in [text], or -1 when this segment has
   * none. Carried here so a draw pass does not search for the span on every
   * frame.
   */
  val caretOffset: Int,
  /** The caret's shape, or 0 for none. */
  val caretKind: Int,
) : Segment(gapBefore) {
  private var heightWidth = -1
  private var fullHeight = 0
  private var lineTops = IntArray(0)
  private var lineStarts = IntArray(0)
  private var height = 0
  private var hidden = emptyList<HiddenRange>()

  /**
   * Layout height at a width, with every closed body's lines taken out.
   *
   * The layout itself is built once per width whatever the reader's choices are,
   * because the visible text has to wrap exactly as it would if the body were
   * open — opening a body is then a pure height change rather than a re-measure.
   * Only the subtraction depends on the choices.
   *
   * Cached per width *and* per choice set, because committed segments are shared
   * between snapshots and a toggle arrives as a new snapshot rather than a new
   * segment.
   */
  @Synchronized
  fun height(width: Int, paint: TextPaint, choices: ReaderChoices = ReaderChoices()): Int {
    val layoutWidth = width.coerceAtLeast(1)
    val widthChanged = layoutWidth != heightWidth
    if (widthChanged) {
      val layout = RenderedDocument.buildLayout(
        text, paint, layoutWidth, Layout.Alignment.ALIGN_NORMAL,
      )
      fullHeight = layout.height
      // Keep only line geometry for details. Retaining a full StaticLayout for
      // each committed segment would also retain its shaping and span state.
      if (detailsBlocks.isNotEmpty()) {
        lineTops = IntArray(layout.lineCount + 1) { layout.getLineTop(it) }
        lineStarts = IntArray(layout.lineCount) { layout.getLineStart(it) }
      }
      heightWidth = layoutWidth
    }
    val hiddenRanges = hiddenRanges(choices)
    if (widthChanged || hiddenRanges != hidden) {
      hidden = hiddenRanges
      height = if (hiddenRanges.isEmpty()) {
        fullHeight
      } else {
        visibleTextHeight(
          fullHeight, lineStarts.size, { lineTops[it] }, { lineStarts[it] }, hiddenRanges,
        )
      }
    }
    return height
  }

  /**
   * The character ranges this segment hides under `choices`.
   *
   * The summary is never in one: it is the visible line a reader taps to open the
   * body, so hiding it would leave nothing to press.
   */
  private fun hiddenRanges(choices: ReaderChoices): List<HiddenRange> {
    if (detailsBlocks.isEmpty()) return emptyList()
    val ranges = ArrayList<HiddenRange>(detailsBlocks.size)
    for (block in detailsBlocks) {
      if (insideHiddenDetails(block.containers, choices)) {
        ranges.add(HiddenRange(block.start, block.end))
      }
    }
    return ranges
  }
}

/**
 * One task item, as the text segment needs it to report a press on its box.
 *
 * `checked` is the state the press leaves behind, which is the opposite of the
 * state the box draws — because a checkbox is a toggle rather than a report.
 */
internal class TaskBlock(
  val start: Int,
  val end: Int,
  val containerId: Int,
  val checked: Boolean,
)

/**
 * One block inside a `<details>`, as the text segment needs it to hide the body.
 *
 * `containers` is a copy rather than a reference into the document: the segment
 * outlives the slice it was built from, and the reader's answer is resolved
 * against these frames long after that slice is gone.
 */
internal class DetailsBlock(
  val start: Int,
  val end: Int,
  /** True for the container's summary line, which is never hidden. */
  val summary: Boolean,
  val containers: List<ContainerDecoration>,
)

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
  /** The fence's title, or empty. Replaces the language in the header. */
  val title: String,
  /**
   * The meta the engine parsed from the fence's info string, and the only copy
   * this side reads: numbers, the start line, and the cap all resolve through
   * [resolveCodeBlock] against it rather than by parsing the raw string again.
   */
  val fenceMeta: FenceMeta,
  /**
   * The UTF-16 offset of each logical line, found once where the segment is
   * built so the gutter's draw pass never searches the text. Always at least one
   * entry, so an empty fence still has a line to number.
   */
  val lineStarts: IntArray,
  /** The code block's own font size in pixels, which the gutter's numbers draw at. */
  val codeTextSize: Int,
  /**
   * The code block's own face, which the gutter draws its numbers in so a digit
   * lines up with the code's own columns. Themed, so a theme that names a mono
   * family gets its numbers in it rather than in the platform default.
   */
  val codeFace: Typeface,
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
  /**
   * Whether the block's text runs right to left, which mirrors a React child
   * hosted in this block to the trailing edge, the same way the container bars
   * and the list marker are mirrored.
   */
  val rtl: Boolean,
  /** The block's first line, which identifies it while a fence is still growing. */
  val firstLine: String,
  /** Padding inside the card, the same on every side. */
  val padding: Int,
  /** [GUTTER_GAP] in pixels: the space between the line numbers and the code. */
  val gutterGap: Int,
  val colors: Palette,
  /** The card's corner radius and border width, both from the theme. */
  val radius: Float,
  val borderWidth: Float,
  /** The header row's face, for the language label. */
  val headerFace: Typeface,
  /** The language label's size, in pixels. */
  val headerSize: Float,
  /** A list marker's face and size, for the margin beside the card. */
  val markerFace: Typeface,
  val markerSize: Float,
  val markerGap: Float,
  /**
   * Where the caret's placeholder sits in the body text, or -1. A caret inside
   * an open fence belongs to the body rather than the card, so the body view is
   * what draws it.
   */
  val caretOffset: Int,
  /** The caret's shape, or 0 for none. */
  val caretKind: Int,
  gapBefore: Int,
  /**
   * This table's index among the document's tables, or -1 before it is known.
   *
   * The reader's expand choice is keyed by this rather than by the block index,
   * because a block index counts rows and moves as a table streams, while an
   * ordinal counts tables and does not.
   */
  val ordinal: Int = -1,
) : Segment(gapBefore) {
  private val digitAdvance: Float
  private val codeLineHeight: Int
  private var wrappedWidth = -1
  private var wrappedHeight = 0
  private var unwrappedHeight = 0
  private var unwrappedWidth = 0

  init {
    // Face and size are immutable for this segment. Measure these once on the
    // rendering worker instead of shaping a digit and allocating metrics in
    // every height/layout pass.
    val codePaint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
      textSize = codeTextSize.toFloat()
      typeface = codeFace
    }
    digitAdvance = codePaint.measureText("0")
    val metrics = codePaint.fontMetricsInt
    codeLineHeight = metrics.descent - metrics.ascent
  }

  /**
   * The width the body lays out at: the card's inner width, or unbounded.
   *
   * [gutterWidth] comes off the wrapped width, which is the only way line numbers
   * change a block's height. An unwrapped body is laid out at its own length
   * either way, so it still does not lay out again on a rotation.
   */
  private fun layoutWidth(cardWidth: Int, wrapped: Boolean, gutterWidth: Int = 0): Int =
    if (wrapped) {
      (cardWidth - contentLeading - 2 * padding - gutterWidth).coerceAtLeast(0)
    } else {
      Int.MAX_VALUE
    }

  /**
   * Body height at a card width. Unwrapped, it is the line count times the line
   * height and does not depend on the width, which is why a rotation does not
   * lay code out again.
   */
  @Synchronized
  fun bodyHeight(paint: TextPaint, cardWidth: Int, wrapped: Boolean, gutterWidth: Int = 0): Int {
    val width = layoutWidth(cardWidth, wrapped, gutterWidth)
    if (wrapped) {
      if (width != wrappedWidth) {
        wrappedHeight = RenderedDocument.buildLayout(text, paint, width, Layout.Alignment.ALIGN_NORMAL).height
        wrappedWidth = width
      }
      return wrappedHeight
    }
    if (unwrappedHeight == 0) {
      // A View's measure spec has 30 size bits. Use that bound only to break
      // lines, then size the scrollable body to its actual line extents.
      val maximumWidth = (1 shl 30) - 1
      val layout = StaticLayout.Builder.obtain(text, 0, text.length, paint, maximumWidth)
        .setIncludePad(false)
        .setBreakStrategy(Layout.BREAK_STRATEGY_HIGH_QUALITY)
        .setHyphenationFrequency(Layout.HYPHENATION_FREQUENCY_NONE)
        .setTextDirection(TextDirectionHeuristics.FIRSTSTRONG_LTR)
        .build()
      unwrappedHeight = layout.height
      var widestLine = 0f
      for (line in 0 until layout.lineCount) {
        // Includes trailing spaces, tabs and paragraph margins, and uses the
        // layout's own shaping and spans for either text direction.
        widestLine = max(widestLine, layout.getLineWidth(line))
      }
      unwrappedWidth = ceil(widestLine.toDouble()).coerceIn(1.0, maximumWidth.toDouble()).toInt()
    }
    return unwrappedHeight
  }

  /** Body width at a card width, which is what the horizontal scroll view scrolls. */
  @Synchronized
  fun bodyWidth(paint: TextPaint, cardWidth: Int, wrapped: Boolean, gutterWidth: Int = 0): Int {
    if (!wrapped) {
      bodyHeight(paint, cardWidth, false)
      return unwrappedWidth
    }
    return layoutWidth(cardWidth, true, gutterWidth)
  }

  /**
   * The gutter's width: its widest number's digits in the code font, plus the gap
   * before the code. Zero when this block draws no numbers.
   *
   * A digit's advance is measured in the code paint rather than assumed, so the
   * gutter is exactly as wide as its widest number and no wider.
   */
  fun gutterWidth(layout: CodeBlockLayout): Int {
    if (!layout.lineNumbers) return 0
    val digits = gutterDigits(layout.firstNumber, lineStarts.size)
    return ceil(digits * digitAdvance).toInt() + gutterGap
  }

  /** The line height the height floor needs, in the code's own face and size. */
  fun lineHeight(): Int = codeLineHeight
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
  /** One grid line's width, from the theme. */
  val borderWidth: Int,
  val rtl: Boolean,
  val identity: Long,
  /** The segment's first block, which is what a copy or a download reads. */
  val blockIndex: Int,
  val blockCount: Int,
  /**
   * The caret's shape, or 0 for none.
   *
   * A caret after a table takes a line of its own below it, and the grid view is
   * what has both the table's height and the space under it, so the shape is
   * carried here rather than in any cell's text. Its width comes from the body's
   * font, which is [RenderedDocument.Style]'s `bodySize` at draw time.
   */
  val caretKind: Int,
  gapBefore: Int,
  /**
   * This table's index among the document's tables, or -1 before it is known.
   *
   * The reader's expand choice is keyed by this rather than by the block index,
   * because a block index counts rows and moves as a table streams, while an
   * ordinal counts tables and does not.
   */
  val ordinal: Int = -1,
) : Segment(gapBefore) {
  // One border width for the closing grid line, so the outer edge the view draws
  // sits inside the size the measurement produced.
  val tableWidth = columnWidths.sum() + borderWidth
  val tableHeight = rowHeights.sum() + borderWidth
  /** Actual text row metrics, in the same pixels as the cap and native frame. */
  fun visibleGridHeight(layout: TableBlockLayout): Int {
    if (layout.maxHeight <= 0f) return tableHeight
    val minimum = rowHeights.take(MIN_VISIBLE_TABLE_ROWS).sum() + borderWidth
    return minOf(tableHeight, maxOf(layout.maxHeight.toInt(), minimum))
  }
  val rowTops = IntArray(rowHeights.size + 1).also { tops ->
    for (index in rowHeights.indices) tops[index + 1] = tops[index] + rowHeights[index]
  }
  val columnLefts = IntArray(columnWidths.size).also { lefts ->
    var left = 0
    for (column in columnWidths.indices) {
      lefts[column] = if (rtl) tableWidth - 1 - left - columnWidths[column] else left
      left += columnWidths[column]
    }
  }
  val accessibilityRows: List<String> = cells.mapIndexed { index, row ->
    val label = row.joinToString(", ") { it.text.toString() }
    if (hasHeader && index == 0) "Header: $label" else label
  }
  val accessibilityText: String = accessibilityRows.joinToString("\n")
}

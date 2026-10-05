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
import android.text.style.UnderlineSpan
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
import kotlin.math.roundToInt

/**
 * Marks a link's text. Taps are hit-tested by the views; accessibility
 * services activate links through ClickableSpan.onClick.
 */
internal class LinkSpan(val index: Int, val color: Int, val underlined: Boolean = true) : ClickableSpan() {
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
    paint.isUnderlineText = underlined
  }
}

/**
 * Raises text by [pixels], or lowers it for a negative value, for footnote
 * references and scripts. Pixels rather than a share of the paint's size,
 * because the size span applied before this one has already shrunk the paint
 * and the shift is a share of the block's size, not the script's.
 */
private class BaselineShiftSpan(private val pixels: Int) : MetricAffectingSpan() {
  // A positive baseline shift moves text down.
  override fun updateDrawState(paint: TextPaint) {
    paint.baselineShift -= pixels
  }

  override fun updateMeasureState(paint: TextPaint) {
    paint.baselineShift -= pixels
  }
}

/**
 * Holds a line to the surrounding text's metrics. Android's monospace font has
 * taller metrics than the body font, which would open a gap above a line with
 * inline code, and a raised or lowered run would open its line the same way.
 * Runs before BlockSpan, which adds block spacing.
 */
private class HeldLineSpan(private val ascent: Int, private val descent: Int) : LineHeightSpan {
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
 * A themed line height, as a minimum: a line the face needs more room for keeps
 * it, so no glyph is clipped, and the extra sits above the text, as iOS places
 * it. Runs after HeldLineSpan, which sets absolute metrics, and before
 * BlockSpan, which adds block spacing on top of the line.
 */
private class BlockLineHeightSpan(private val height: Int) : LineHeightSpan {
  override fun chooseHeight(
    text: CharSequence,
    lineStart: Int,
    lineEnd: Int,
    spanStartV: Int,
    lineHeight: Int,
    fm: Paint.FontMetricsInt,
  ) {
    val extra = height - (fm.descent - fm.ascent)
    if (extra > 0) {
      fm.ascent -= extra
      fm.top = minOf(fm.top, fm.ascent)
    }
  }
}

/**
 * Inline code in a text segment, whose background TextSegmentView draws
 * between the code's ascent and descent. BackgroundColorSpan fills the whole
 * line, which on a block's first line includes the block spacing above it.
 */
internal class InlineCodeBackgroundSpan(val color: Int, val ascent: Int, val descent: Int)

/**
 * A strikethrough in a color the theme chose.
 *
 * Android's own `StrikethroughSpan` takes no color and draws the current text
 * color, so a theme that wants a strike in its own color needs a span of its
 * own. A null color means the text color, which is what the platform's span
 * does, so an unset slot is still the platform's span rather than this one.
 */
internal class ThemedStrikeSpan(val color: Int) : UnderlineSpan() {
  override fun updateDrawState(ds: TextPaint) {
    ds.flags = ds.flags or Paint.STRIKE_THRU_TEXT_FLAG
    ds.color = color
  }
}

/**
 * One nesting level's decoration: its indent, the bar or marker it draws in that
 * indent, and the two metrics the drawing reads off it.
 *
 * The bar's width and the marker gap are here rather than in the view because
 * they are the theme's, and a view that read its own would be a second place
 * the values could differ from the measurement's.
 */
internal class ContainerDecoration(
  val leading: Float,
  val width: Float,
  val barColor: Int?,
  val continuesPrevious: Boolean,
  val marker: String?,
  val barWidth: Float = 0f,
  val markerGap: Float = 0f,
  /**
   * The marker's own color, which a theme can set apart from the list's: a
   * bullet from a number, or a finished task from an unfinished one.
   */
  val markerColor: Int? = null,
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
  /** A rule's thickness, from the theme, rather than a density-sized hairline. */
  private val ruleHeight: Float,
) : LineHeightSpan, LineBackgroundSpan {
  private fun blockStart(text: CharSequence) = (text as Spanned).getSpanStart(this)

  // The first line's metrics before and after the block spacing. StaticLayout
  // hands the next line of the same paragraph the metrics this span left on the
  // first, so a wrapped second line would otherwise open the block's whole gap
  // above itself.
  private var naturalAscent = 0
  private var naturalTop = 0
  private var spacedAscent = 0
  private var spacedTop = 0

  override fun chooseHeight(
    text: CharSequence,
    lineStart: Int,
    lineEnd: Int,
    spanStartV: Int,
    lineHeight: Int,
    fm: Paint.FontMetricsInt,
  ) {
    if (lineStart == blockStart(text)) {
      naturalAscent = fm.ascent
      naturalTop = fm.top
      fm.ascent -= spacingBefore
      fm.top -= spacingBefore
      spacedAscent = fm.ascent
      spacedTop = fm.top
    } else if (spacingBefore != 0 && fm.ascent == spacedAscent && fm.top == spacedTop) {
      fm.ascent = naturalAscent
      fm.top = naturalTop
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
      // A zero-width bar is a bar the theme removed, so the width is checked
      // before drawing rather than only the color.
      if (barColor != null && container.barWidth > 0f) {
        paint.color = barColor
        val barTop = if (first && !container.continuesPrevious) textTop else top
        val x = if (rtl) right - container.leading - 2 * density - container.barWidth
          else left + container.leading + 2 * density
        canvas.drawRect(x, barTop.toFloat(), x + container.barWidth, bottom.toFloat(), paint)
      }
      val marker = container.marker
      if (first && marker != null) {
        val x = if (rtl) right - container.leading - container.width + container.markerGap
          else left + container.leading + container.width - container.markerGap - markerPaint.measureText(marker)
        // The marker's own color where the theme gave it one, and the shared
        // marker paint otherwise, so an unset slot draws exactly as it always did.
        val paint = if (container.markerColor != null) {
          TextPaint(markerPaint).apply { color = container.markerColor }
        } else {
          markerPaint
        }
        canvas.drawText(marker, x, baseline.toFloat(), paint)
      }
    }

    // A code block's background is drawn by its card, not here: a card is a
    // view, so it can cover the padding a text layout cannot.
    if (decoration == DECORATION_RULE) {
      val x = if (rtl) left.toFloat() else left + contentLeading
      val endX = if (rtl) right - contentLeading else right.toFloat()
      // The rule's color and thickness are both the theme's, drawn centered on
      // the midline, which is where a one-point hairline sat.
      paint.color = colors.rule
      val thickness = maxOf(1f, ruleHeight)
      val y = ((textTop + bottom) / 2).toFloat()
      canvas.drawRect(x, y - (thickness - 1) / 2, endX, y - (thickness - 1) / 2 + thickness, paint)
    }
    paint.color = savedColor
    paint.style = savedStyle
  }

  companion object {
    const val DECORATION_NONE = 0
    const val DECORATION_RULE = 1
  }
}

/**
 * The colors one rendering draws with, for one theme in one appearance.
 *
 * Every entry is a slot read, with the value the renderer used before theming
 * existed as the fallback. That fallback is what makes an unthemed document
 * pixel-identical to the one this replaced, and it is why the platform's own
 * night colors sit here rather than in a theme: a theme that sets nothing should
 * still be right at two o'clock in the morning.
 */
internal class Palette(private val theme: NativeTheme, val night: Boolean) {
  /**
   * A slot's color, or one of [light] and [dark] when the theme left it unset.
   *
   * The pair is the platform's role color rather than one value, because an
   * unset slot has to follow the appearance the same way a set one does.
   */
  fun slot(id: Int, light: Int, dark: Int): Int = theme.color(id, night) ?: if (night) dark else light

  /**
   * A slot's color, or null when neither the theme nor the caller named one.
   *
   * Used where a theme's "no fill at all" has to be told from a fill that
   * happens to be transparent, such as a table's zebra striping.
   */
  fun slotOrNull(id: Int): Int? = theme.color(id, night)

  val text = slot(ThemeSlots.SLOT_PARAGRAPH_COLOR, 0xFF000000.toInt(), 0xFFFFFFFF.toInt())
  val secondary = slot(ThemeSlots.SLOT_BLOCKQUOTE_COLOR, 0x993C3C43.toInt(), 0x99EBEBF5.toInt())
  val link = slot(ThemeSlots.SLOT_LINK_COLOR, 0xFF007AFF.toInt(), 0xFF0A84FF.toInt())
  val codeBackground = slot(ThemeSlots.SLOT_CODE_BACKGROUND_COLOR, 0x1F767680, 0x3D767680)
  val codeBlockBackground =
    slot(ThemeSlots.SLOT_CODE_BLOCK_BACKGROUND_COLOR, 0x1F767680, 0x3D767680)
  val separator = slot(ThemeSlots.SLOT_TABLE_BORDER_COLOR, 0x4A3C3C43, 0x99545458.toInt())
  val rule = slot(ThemeSlots.SLOT_THEMATIC_BREAK_COLOR, 0x4A3C3C43, 0x99545458.toInt())
  val quoteBar = slot(ThemeSlots.SLOT_BLOCKQUOTE_BORDER_COLOR, 0xFFC7C7CC.toInt(), 0xFF48484A.toInt())
  val marker = slot(ThemeSlots.SLOT_LIST_MARKER_COLOR, 0x993C3C43.toInt(), 0x99EBEBF5.toInt())
  val codeText = slot(ThemeSlots.SLOT_CODE_BLOCK_COLOR, 0xFF000000.toInt(), 0xFFFFFFFF.toInt())
  val headerColor = slot(ThemeSlots.SLOT_CODE_BLOCK_HEADER_COLOR, secondary, secondary)
  val headerBackground =
    slotOrNull(ThemeSlots.SLOT_CODE_BLOCK_HEADER_BACKGROUND_COLOR)
  /**
   * The header's buttons, which a theme can tint apart from the row's text.
   *
   * A theme that named only the row's text gets buttons that match, because a
   * header and its controls read as one row.
   */
  val headerButtonColor =
    slotOrNull(ThemeSlots.SLOT_CODE_BLOCK_HEADER_BUTTON_COLOR) ?: headerColor
  /** The rule under a code card's header. Clear where a theme set none. */
  val headerBorderColor = slotOrNull(ThemeSlots.SLOT_CODE_BLOCK_HEADER_BORDER_COLOR)

  /** A table toolbar's buttons, which read as chrome rather than as content. */
  val toolbarButtonColor = slot(ThemeSlots.SLOT_TABLE_TOOLBAR_BUTTON_COLOR, secondary, secondary)

  /**
   * The selection highlight.
   *
   * The platform's own accent where a theme named none, which is what a reader
   * expects a selection to look like. `null` means "leave it alone", so a view
   * that never set one keeps whatever it inherited.
   */
  val selectionColor: Int? = theme.color(ThemeSlots.SLOT_SELECTION_COLOR, night)
  val tableHeaderBackground =
    slotOrNull(ThemeSlots.SLOT_TABLE_HEADER_BACKGROUND_COLOR) ?:
      slot(ThemeSlots.SLOT_CODE_BACKGROUND_COLOR, 0x1F767680, 0x3D767680)

  /**
   * One row's zebra fill, or null where the theme set none.
   *
   * `index` counts data rows from zero, so the header row is not counted and the
   * alternation starts on the first row of the body whichever that is.
   */
  fun rowStripe(index: Int): Int? =
    slotOrNull(
      if (index % 2 == 0) ThemeSlots.SLOT_TABLE_ROW_EVEN_BACKGROUND_COLOR
      else ThemeSlots.SLOT_TABLE_ROW_ODD_BACKGROUND_COLOR,
    )

  /** The syntax slot per engine token class, so a class picks its own color. */
  private val tokenSlots = intArrayOf(
    ThemeSlots.SLOT_SYNTAX_PLAIN,
    ThemeSlots.SLOT_SYNTAX_COMMENT,
    ThemeSlots.SLOT_SYNTAX_KEYWORD,
    ThemeSlots.SLOT_SYNTAX_TYPE,
    ThemeSlots.SLOT_SYNTAX_STRING,
    ThemeSlots.SLOT_SYNTAX_NUMBER,
    ThemeSlots.SLOT_SYNTAX_CONSTANT,
    ThemeSlots.SLOT_SYNTAX_REGEX,
    ThemeSlots.SLOT_SYNTAX_FUNCTION,
    ThemeSlots.SLOT_SYNTAX_PROPERTY,
    ThemeSlots.SLOT_SYNTAX_OPERATOR,
    ThemeSlots.SLOT_SYNTAX_PREPROCESSOR,
    ThemeSlots.SLOT_SYNTAX_TAG,
    ThemeSlots.SLOT_SYNTAX_ATTRIBUTE,
    ThemeSlots.SLOT_SYNTAX_INSERTED,
    ThemeSlots.SLOT_SYNTAX_DELETED,
    ThemeSlots.SLOT_SYNTAX_HEADING,
  )

  /**
   * One token class's color.
   *
   * An unset class draws the engine's palette, which the theme carries as the
   * unset value. Plain and Operator are the exception: unset, they take the code
   * block's own text color, as they do in GitHub's own themes.
   */
  fun token(tokenClass: Int): Int {
    val slot = tokenSlots.getOrNull(tokenClass) ?: return codeText
    theme.color(slot, night)?.let { return it }
    return if (tokenClass == NativeDocument.TOKEN_PLAIN || tokenClass == NativeDocument.TOKEN_OPERATOR) {
      codeText
    } else {
      theme.unsetColor(slot, night)
    }
  }

  /** GitHub alert colors; a type GitHub does not define gets a neutral bar. */
  fun admonition(type: String?): Int = when (type) {
    "note" -> slot(ThemeSlots.SLOT_ADMONITION_NOTE, 0xFF007AFF.toInt(), 0xFF0A84FF.toInt())
    "tip" -> slot(ThemeSlots.SLOT_ADMONITION_TIP, 0xFF34C759.toInt(), 0xFF30D158.toInt())
    "important" -> slot(ThemeSlots.SLOT_ADMONITION_IMPORTANT, 0xFFAF52DE.toInt(), 0xFFBF5AF2.toInt())
    "warning" -> slot(ThemeSlots.SLOT_ADMONITION_WARNING, 0xFFFF9500.toInt(), 0xFFFF9F0A.toInt())
    "caution" -> slot(ThemeSlots.SLOT_ADMONITION_CAUTION, 0xFFFF3B30.toInt(), 0xFFFF453A.toInt())
    else -> quoteBar
  }
}

/** One unit of layout. [gapBefore] is the space above it in the document. */
internal sealed class Segment(val gapBefore: Int)

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
) : Segment(gapBefore) {
  // One border width for the closing grid line, so the outer edge the view draws
  // sits inside the size the measurement produced.
  val tableWidth = columnWidths.sum() + borderWidth
  val tableHeight = rowHeights.sum() + borderWidth
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
  private fun heightOf(segment: Segment, width: Int, chrome: Int, code: CodeLayout, images: ImageLayout): Int =
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
      is CodeSegment -> {
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
  fun frames(width: Int, chrome: Int = 0, code: CodeLayout = CodeLayout(), images: ImageLayout = ImageLayout()): IntArray {
    // A measurement only holds for the layout it was made under, so the whole code
    // layout and the whole image layout are part of the key rather than the
    // rendering. An image's height is its state, so a picture that loads has to
    // restack the frames or the text under it stays where the placeholder was.
    if (width == cachedWidth && chrome == cachedChrome && code == cachedCode && images == cachedImages) {
      return cachedFrames
    }
    val frames = IntArray(segments.size * 2)
    var y = 0
    segments.forEachIndexed { index, segment ->
      y += segment.gapBefore
      frames[index * 2] = y
      frames[index * 2 + 1] = heightOf(segment, width, chrome, code, images)
      y += frames[index * 2 + 1]
    }
    cachedWidth = width
    cachedChrome = chrome
    cachedCode = code
    cachedImages = images
    cachedFrames = frames
    return frames
  }

  fun height(width: Int, chrome: Int = 0, code: CodeLayout = CodeLayout(), images: ImageLayout = ImageLayout()): Int {
    val frames = frames(width, chrome, code, images)
    return if (frames.isEmpty()) 0 else frames[frames.size - 2] + frames[frames.size - 1]
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
    private val cache = LruCache<String, RenderedDocument>(128)

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
        // A picture and a formula hold no text at all, so their weight is what the
        // view holds: the bitmap belongs to the pipeline's cache and is counted
        // there, and a formula's glyphs belong to whatever drew them.
        is ImageSegment -> 512
        is MathSegment -> 512
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
  internal class Style(fontScale: Float, val colors: Palette, val theme: NativeTheme, themeId: Long = 0L) {
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
        contentLeading = containers.second.toInt(),
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
      val cellText = (from until to).map { row ->
        (0 until document.cellCount(row)).map { column ->
          val cell = SpannableStringBuilder(text, document.cellStart(row, column), document.cellEnd(row, column))
          if (document.isTableHeader(row)) {
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
          widths[column] = max(widths[column], min(ceil(Layout.getDesiredWidth(cell, cellPaint)), maxWidth))
          cell
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
          buildLayout(cell, cellPaint, ceil(widths[column]).toInt(), alignment)
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

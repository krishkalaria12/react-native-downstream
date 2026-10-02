package com.downstream

import android.content.Context
import android.graphics.Color
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.TextView
import kotlin.math.ceil

/**
 * One display formula, as a view.
 *
 * A view rather than a span for the same reason a picture is: the formula's own
 * box has a background, a padding, and an alignment that an inline span cannot
 * express, and its height comes from the engine's own `mathBlockHeight` rather
 * than from a text layout's line count.
 *
 * What is drawn when there is no layout is the literal. That is the rule the whole
 * feature rests on: math that cannot be laid out stays readable as the writer's own
 * source, rather than becoming an empty box, a `[?]`, or nothing. So the view holds
 * the literal and only swaps in glyphs when the engine produced a display list for
 * them.
 */
internal class MathSegmentView(context: Context) : FrameLayout(context) {
  private companion object {
    /** `math.textAlign`, as `ThemeTextAlign` in the engine. */
    const val ALIGN_LEFT = 0
    const val ALIGN_CENTER = 1
  }

  private val background = View(context)
  private val literal = TextView(context).apply {
    includeFontPadding = false
    gravity = Gravity.CENTER_VERTICAL
  }
  private val glyphs = MathGlyphView(context)
  private val scroll = HorizontalScrollView(context).apply {
    isHorizontalScrollBarEnabled = true
    isFillViewport = false
    overScrollMode = OVER_SCROLL_NEVER
    addView(glyphs, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT))
  }

  private var segment: MathSegment? = null
  private val errorPaint = Paint(Paint.ANTI_ALIAS_FLAG)
  private var textColor = Color.BLACK
  private var fontSize = 0f
  private var padding = 0f
  private var errorColor = Color.BLACK

  /**
   * The theme's own values, resolved here rather than by the view group so the box
   * and the fill come from one read.
   */
  fun setStyle(
    fontSize: Float,
    color: Int,
    backgroundColor: Int?,
    errorColor: Int,
    padding: Float,
    radius: Float,
  ) {
    this.fontSize = fontSize
    this.padding = padding
    literal.paint.textSize = fontSize
    textColor = color
    this.errorColor = errorColor
    errorPaint.color = errorColor
    literal.setTextColor(if (segment?.failed == true) errorColor else color)
    glyphs.setPaint(color, fontSize)
    // No background is no background, rather than a fill of the default light
    // color on a dark theme: a theme that sets no math background wants the page's.
    background.visibility = if (backgroundColor == null) GONE else VISIBLE
    background.background = GradientDrawable().apply {
      cornerRadius = radius
      setColor(backgroundColor ?: Color.TRANSPARENT)
    }
    requestLayout()
  }

  fun setSegment(value: MathSegment) {
    if (segment?.literal == value.literal && segment?.resolved == value.resolved && segment?.failed == value.failed &&
      segment?.displayList === value.displayList && segment?.textAlign == value.textAlign
    ) {
      return
    }
    if (segment?.literal != value.literal) scroll.scrollTo(0, 0)
    segment = value
    // The literal always stays as the text, even when a layout resolved: it is what
    // a copy produces and what a screen reader falls back to, and a formula is
    // replaced by its own drawing rather than by its own source.
    literal.text = value.literal
    // The theme's own alignment, resolved in C++ so both platforms read one value.
    // A `$$` line fence centers by default, and the theme overrides that.
    literal.gravity = Gravity.CENTER_VERTICAL or gravityFor(value.textAlign)
    contentDescription = value.speech.ifEmpty { value.literal }
    val drawable = value.resolved && value.displayList != null
    literal.visibility = if (drawable) GONE else VISIBLE
    scroll.visibility = if (drawable) VISIBLE else GONE
    glyphs.segment = if (drawable) value else null
    // A failed formula is drawn in the theme's error color rather than the text
    // color: the reader should be able to see that this one is not rendered, without
    // the engine having to reject the document or the app logging anything. One that
    // is still arriving keeps the text color, because nothing is wrong with it yet.
    literal.setTextColor(if (value.failed) errorColor else textColor)
    requestLayout()
  }

  /** One `math.textAlign` value as a horizontal gravity. */
  private fun gravityFor(textAlign: Int): Int = when (textAlign) {
    ALIGN_LEFT -> Gravity.START
    ALIGN_CENTER -> Gravity.CENTER_HORIZONTAL
    else -> Gravity.END
  }

  override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
    val available = MeasureSpec.getSize(widthMeasureSpec)
    val current = segment ?: run {
      setMeasuredDimension(available.coerceAtLeast(0), 0)
      return
    }
    // The engine's own height, with the theme's padding: the layout's height and
    // depth in em at the scaled size, plus the padding on both sides. An unresolved
    // formula takes one line, which is what its literal takes.
    val body = if (current.resolved && current.displayList != null) {
      mathBlockHeight(current.height, current.depth, fontSize, 0f)
    } else {
      literal.paint.fontSpacing
    }
    val total = (body + padding * 2f).toInt().coerceAtLeast(0)
    setMeasuredDimension(available, total)
    val width = (available - paddingLeft - paddingRight).coerceAtLeast(0)
    val height = (total - paddingTop - paddingBottom).coerceAtLeast(0)
    // Give the scroll view the complete formula at its themed size. A fitting
    // formula keeps the column's width so the glyph view can apply textAlign.
    val natural = current.displayList?.let { ceil(it.width * fontSize).toInt() } ?: 0
    // HorizontalScrollView measures its child with an unspecified width, so the
    // view's minimum width supplies the full drawing surface for that measure.
    glyphs.minimumWidth = natural.coerceAtLeast(width)
    glyphs.layoutParams.height = height
    for (child in listOf(background, literal, scroll)) {
      child.measure(
        MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY),
        MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY),
      )
    }
  }

  override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
    val width = right - left - paddingLeft - paddingRight
    val height = bottom - top - paddingTop - paddingBottom
    for (child in listOf(background, literal, scroll)) {
      child.layout(paddingLeft, paddingTop, paddingLeft + width, paddingTop + height)
    }
  }

  override fun onDetachedFromWindow() {
    super.onDetachedFromWindow()
    glyphs.segment = null
  }

  override fun onAttachedToWindow() {
    super.onAttachedToWindow()
    // A view taken out of the window and put back, as a screen that is hidden and
    // shown again does, keeps its segment; `setSegment` returns early for the same
    // one, so the glyphs are handed back here.
    val current = segment ?: return
    if (current.resolved && current.displayList != null) {
      glyphs.segment = current
    }
  }

  init {
    addView(background)
    addView(literal)
    addView(scroll)
  }
}

/**
 * The glyphs of one formula, drawn from the engine's display list.
 *
 * The fallback lives in the parent, so a formula with no layout never enters this
 * class: [MathSegment.displayList] is null in that case and the literal is what
 * the reader sees.
 */
internal class MathGlyphView(context: Context) : View(context) {
  var segment: MathSegment? = null
    set(value) {
      field = value
      invalidate()
    }

  private val renderer = MathRenderer().also { MathFonts.attach(context) }
  private var color = Color.BLACK
  private var textSize = 0f

  /**
   * The colour and size glyphs draw at: the theme's math colour, which an
   * uncolored item takes, and the block's own scaled size in pixels per em.
   */
  fun setPaint(color: Int, textSize: Float) {
    this.color = color
    this.textSize = textSize
    invalidate()
  }

  override fun onDraw(canvas: Canvas) {
    val current = segment ?: return
    val list = current.displayList ?: return
    if (textSize <= 0f || width <= 0) {
      return
    }
    // Display math keeps its themed size. The parent gives this view its natural
    // width and lets the reader scroll when it exceeds the column.
    val size = textSize
    val drawn = list.width * size
    val x = when (current.textAlign) {
      0 -> 0f
      1 -> ((width - drawn) / 2f).coerceAtLeast(0f)
      else -> (width - drawn).coerceAtLeast(0f)
    }
    val y = ((height - (list.height + list.depth) * size) / 2f).coerceAtLeast(0f)
    canvas.save()
    canvas.clipRect(0, 0, width, height)
    canvas.translate(x, y)
    renderer.draw(canvas, list, size, color, resources.displayMetrics.density * 0.5f)
    canvas.restore()
  }
}

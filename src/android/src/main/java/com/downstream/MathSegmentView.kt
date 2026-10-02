package com.downstream

import android.content.Context
import android.graphics.Color
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.TextView

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
    literal.setTextColor(color)
    textColor = color
    this.errorColor = errorColor
    errorPaint.color = errorColor
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
    if (segment?.literal == value.literal && segment?.resolved == value.resolved) {
      return
    }
    segment = value
    // The literal always stays as the text, even when a layout resolved: it is what
    // a copy produces and what a screen reader falls back to, and a formula is
    // replaced by its own drawing rather than by its own source.
    literal.text = value.literal
    // The theme's own alignment, resolved in C++ so both platforms read one value.
    // A `$$` line fence centers by default, and the theme overrides that.
    literal.gravity = Gravity.CENTER_VERTICAL or gravityFor(value.textAlign)
    contentDescription = value.speech.ifEmpty { value.literal }
    literal.visibility = if (value.resolved) GONE else VISIBLE
    glyphs.visibility = if (value.resolved) VISIBLE else GONE
    glyphs.segment = if (value.resolved) value else null
    // An unresolved formula is drawn in the theme's error color rather than the text
    // color: the reader should be able to see that this one is not rendered, without
    // the engine having to reject the document or the app logging anything. The
    // literal is still fully readable — this is a color, not an overlay.
    literal.setTextColor(if (value.resolved) textColor else errorColor)
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
    val body = if (current.resolved) {
      mathBlockHeight(current.height, current.depth, fontSize, 0f)
    } else {
      literal.paint.fontSpacing
    }
    val total = (body + padding * 2f).toInt().coerceAtLeast(0)
    setMeasuredDimension(available, total)
    for (child in listOf(background, literal, glyphs)) {
      child.measure(
        MeasureSpec.makeMeasureSpec(available - paddingLeft - paddingRight, MeasureSpec.EXACTLY),
        MeasureSpec.makeMeasureSpec(total - paddingTop - paddingBottom, MeasureSpec.EXACTLY),
      )
    }
  }

  override fun onLayout(changed: Boolean, left: Int, top: Int, right: Int, bottom: Int) {
    val width = right - left - paddingLeft - paddingRight
    val height = bottom - top - paddingTop - paddingBottom
    for (child in listOf(background, literal, glyphs)) {
      child.layout(paddingLeft, paddingTop, paddingLeft + width, paddingTop + height)
    }
  }

  override fun onDetachedFromWindow() {
    super.onDetachedFromWindow()
    glyphs.segment = null
  }

  init {
    addView(background)
    addView(literal)
    addView(glyphs)
  }
}

/**
 * The glyphs of one formula, drawn from the engine's display list.
 *
 * A placeholder for the drawing package: it holds the layout and its paint, and
 * draws nothing until a `MathEngine` hands it a rasterized glyph set. The point of
 * keeping it a separate view is that the fallback lives in the parent, so a formula
 * with no engine never enters this class at all — [MathSegment.resolved] is false
 * in that case and the literal is what the reader sees.
 */
internal class MathGlyphView(context: Context) : View(context) {
  var segment: MathSegment? = null
  var textSize = 0f

  /**
   * The colour and size glyphs draw at.
   *
   * Set through a method rather than by assigning a whole Paint, so the view cannot
   * hand this one a paint that has not been sized and leave a formula drawn at the
   * default 12 points.
   */
  fun setPaint(color: Int, textSize: Float) {
    paint = Paint(Paint.ANTI_ALIAS_FLAG)
    paint.setColor(color)
    paint.textSize = textSize
    this.textSize = textSize
  }

  private var paint = Paint(Paint.ANTI_ALIAS_FLAG)

  /**
   * The scale one inline-sized formula draws at.
   *
   * Kept here so the drawing package has one place to read it from, and so the
   * `1.21em` that KaTeX uses is applied in exactly one place per platform.
   */
  val drawScale: Float get() = kInlineMathScale

  /** The em metrics of [segment], or null when there is nothing to draw. */
  fun emMetrics(): Triple<Float, Float, Float>? {
    val current = segment ?: return null
    if (!current.resolved) {
      return null
    }
    return Triple(current.width, current.height, current.depth)
  }

  override fun onDraw(canvas: Canvas) {
    // Nothing until an engine supplies glyphs. The parent's literal is hidden
    // precisely when this view is visible, so an engine that is not installed would
    // otherwise leave a hole — which is why `resolved` is false in that case rather
    // than true with nothing behind it.
  }
}
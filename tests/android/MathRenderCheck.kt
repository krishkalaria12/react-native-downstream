package com.downstream

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.drawable.GradientDrawable
import android.view.View
import android.widget.HorizontalScrollView
import android.widget.TextView

/** Checks redraw work and invalidation with host recorders, not device timing. */
fun main() {
  val shape = MathDisplayList.Shape(1f, 2f, false, 0f, MathDisplayList.INHERIT, floatArrayOf(
    0f, 1f, 2f, 0f, 0f, 0f, 0f,
    1f, 3f, 4f, 0f, 0f, 0f, 0f,
    2f, 5f, 6f, 7f, 8f, 0f, 0f,
    3f, 9f, 10f, 11f, 12f, 13f, 14f,
    4f, 0f, 0f, 0f, 0f, 0f, 0f,
  ))
  check(shape.path.commands == listOf(
    listOf(0f, 1f, 2f), listOf(1f, 3f, 4f), listOf(2f, 7f, 8f, 5f, 6f),
    listOf(3f, 11f, 12f, 13f, 14f, 9f, 10f), listOf(4f),
  )) { "cached path keeps command ordering and control points" }
  val glyph = MathDisplayList.Glyph(1f, 2f, 0.5f, -1, 0x1F642, MathDisplayList.INHERIT)
  val explicit = MathDisplayList.Glyph(3f, 4f, 1f, -1, 65, 123)
  val skipped = listOf(0, -1, 0x110000).map { MathDisplayList.Glyph(0f, 0f, 1f, -1, it, 0) }
  val list = MathDisplayList(2f, 1f, 0.25f, listOf(glyph, explicit) + skipped, emptyList(), listOf(shape))
  val renderer = MathRenderer()
  val canvas = Canvas()
  val pathCount = Path.constructions
  repeat(100) { renderer.draw(canvas, list, 16f, 42, 0.5f) }
  check(Path.constructions == pathCount) { "100 redraws construct no paths" }
  check(canvas.paths.all { it.path === shape.path && it.color == 42 && it.style == Paint.Style.STROKE })
  check(canvas.texts.size == 200 && canvas.texts.filterIndexed { index, _ -> index % 2 == 0 }.all {
    it.text === glyph.text && it.text == "\uD83D\uDE42" && it.size == 8f && it.color == 42
  }) { "valid supplementary glyph text is reused and invalid code points stay skipped" }
  check(canvas.texts.filterIndexed { index, _ -> index % 2 == 1 }.all { it.color == 123 })
  renderer.draw(canvas, list, 32f, 99, 0.5f)
  check(canvas.paths.last().path === shape.path && canvas.paths.last().color == 99)
  check(canvas.paths.last().strokeWidth == 0.5f / 32f && canvas.scales.last() == 32f to 32f)
  check(canvas.texts[canvas.texts.size - 2].text === glyph.text && canvas.texts[canvas.texts.size - 2].size == 16f)
  check(Path.constructions == pathCount) { "size/theme changes reuse geometry" }

  val view = MathSegmentView(Context())
  val background = view.children[0]
  val literal = view.children[1] as TextView
  val scroll = view.children[2] as HorizontalScrollView
  val glyphView = scroll.children[0] as MathGlyphView
  fun style(size: Float = 16f, color: Int = 42, fill: Int? = 10, error: Int = 91, padding: Float = 4f, radius: Float = 3f) {
    view.setStyle(size, color, fill, error, padding, radius)
  }
  fun segment(speech: String = "formula", align: Int = 1, failed: Boolean = false, resolved: Boolean = true,
    height: Float = 1f, literal: String = "\$\$x\$\$", displayList: MathDisplayList? = list) =
    MathSegment(literal, height, 0.25f, resolved, displayList, failed, speech, align)
  style()
  view.setSegment(segment())
  val drawable = background.background
  val drawables = GradientDrawable.constructions
  val layouts = view.layoutRequests
  val redraws = glyphView.invalidations
  val literalRedraws = literal.invalidations
  repeat(100) { style(); view.setSegment(segment()) }
  check(view.layoutRequests == layouts && glyphView.invalidations == redraws && literal.invalidations == literalRedraws)
  check(GradientDrawable.constructions == drawables && background.background === drawable)
  check(scroll.scrollResets == 1) { "unchanged snapshots do not reset horizontal scrolling" }
  view.setSegment(segment(speech = "updated speech"))
  check(view.contentDescription == "updated speech" && view.layoutRequests == layouts && glyphView.invalidations == redraws)
  style(color = 99, fill = 20, radius = 6f)
  check(view.layoutRequests == layouts && glyphView.invalidations == redraws + 1)
  check(background.background === drawable && drawable!!.fillColor == 20 && drawable.cornerRadius == 6f)
  check(literal.textColor == 99)
  style(size = 32f, padding = 8f)
  check(view.layoutRequests == layouts + 1)
  view.measure(View.MeasureSpec.makeMeasureSpec(100, View.MeasureSpec.EXACTLY), 0)
  check(view.measuredHeight == 56) { "size/padding changes update formula height" }
  glyphView.layout(0, 0, 100, 56)
  view.setSegment(segment(align = 2))
  val aligned = Canvas()
  glyphView.onDraw(aligned)
  check(aligned.translations.first().first == 36f) { "right alignment updates the formula origin" }
  view.onDetachedFromWindow()
  check(glyphView.segment == null)
  view.setSegment(segment(speech = "reattached speech", align = 2))
  view.onAttachedToWindow()
  check(glyphView.segment?.speech == "reattached speech") { "reattachment restores the latest segment" }
  style(fill = null)
  check(background.visibility == View.GONE && background.background === drawable)
  view.setSegment(segment(failed = true, resolved = false, displayList = null))
  check(literal.visibility == View.VISIBLE && scroll.visibility == View.GONE && literal.textColor == 91)
  style(error = 88)
  check(literal.textColor == 88)
  view.setSegment(segment(speech = "", literal = "fallback", resolved = false, displayList = null))
  check(view.contentDescription == "fallback" && literal.textColor == 42)
  val glyphRedraws = glyphView.invalidations
  glyphView.setPaint(42, 16f)
  check(glyphView.invalidations == glyphRedraws)
  view.setSegment(segment(height = 2f))
  val changedMetrics = view.layoutRequests
  view.setSegment(segment(height = 3f))
  check(view.layoutRequests == changedMetrics + 1) { "changed metrics cannot hit the snapshot guard" }
  check(GradientDrawable.constructions == drawables) { "theme updates reuse one background drawable" }
  println("math rendering: cached draw inputs, style guards, accessibility, metrics and lifecycle passed")
}

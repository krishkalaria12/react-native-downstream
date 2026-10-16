package com.downstream

import android.app.Instrumentation
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.text.Layout
import android.text.PrecomputedText
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.TextDirectionHeuristics
import android.text.TextPaint
import android.text.style.AbsoluteSizeSpan
import android.text.style.ForegroundColorSpan
import android.text.style.StrikethroughSpan
import android.text.style.StyleSpan
import android.util.Log
import android.view.View
import android.widget.FrameLayout
import org.json.JSONObject

/** Record only queries for custom drawing, excluding framework shaping queries. */
private class DecorationText(private val source: Spanned) : Spanned by source {
  val ranges = ArrayList<Pair<Int, Int>>()
  override fun <T : Any?> getSpans(start: Int, end: Int, type: Class<T>): Array<T> {
    if (type == LinkSpan::class.java || type == StrikethroughSpan::class.java || type == InlineCodeBackgroundSpan::class.java) {
      ranges.add(start to end)
    }
    return source.getSpans(start, end, type)
  }
}

/** Keep Android layout/rasterization real while counting custom selection paths. */
private class DecorationLayout(private val source: Layout, text: Spanned) :
  Layout(text, TextPaint(source.paint), source.width, Alignment.ALIGN_NORMAL, 1f, 0f) {
  var paths = 0
  override fun draw(canvas: Canvas) = source.draw(canvas)
  override fun getSelectionPath(start: Int, end: Int, path: Path) { paths++; source.getSelectionPath(start, end, path) }
  override fun getLineCount() = source.lineCount
  override fun getLineTop(line: Int) = source.getLineTop(line)
  override fun getLineDescent(line: Int) = source.getLineDescent(line)
  override fun getLineStart(line: Int) = source.getLineStart(line)
  override fun getParagraphDirection(line: Int) = source.getParagraphDirection(line)
  override fun getLineContainsTab(line: Int) = source.getLineContainsTab(line)
  override fun getLineDirections(line: Int) = source.getLineDirections(line)
  override fun getTopPadding() = source.topPadding
  override fun getBottomPadding() = source.bottomPadding
  override fun getEllipsisStart(line: Int) = source.getEllipsisStart(line)
  override fun getEllipsisCount(line: Int) = source.getEllipsisCount(line)
}

private fun decorationRaster(width: Int, height: Int, hardware: Boolean, draw: (Canvas) -> Unit): IntArray {
  if (hardware) return hardwareRaster(width, height, draw)
  val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
  try {
    val canvas = Canvas(bitmap)
    canvas.drawColor(Color.WHITE)
    draw(canvas)
    return IntArray(width * height).also { bitmap.getPixels(it, 0, width, 0, 0, width, height) }
  } finally { bitmap.recycle() }
}

private fun decorationSource(): SpannableStringBuilder {
  val source = SpannableStringBuilder()
  val flags = Spanned.SPAN_EXCLUSIVE_EXCLUSIVE
  for (row in 0 until 120) {
    val start = source.length
    source.append(if (row % 4 == 0) "مرحبا بالعالم mixed العربية text row $row, " else "Paragraph $row link words and code words, ")
    source.append("wrapping over several lines with a struck price and more text.\n")
    val end = source.length - 1
    source.setSpan(LinkSpan(row, 0xff1767a0.toInt(), true), start, end - 4, flags)
    source.setSpan(StrikethroughSpan(), start + 15, end, flags)
    source.setSpan(ForegroundColorSpan(0xff913c60.toInt()), start + 15, end, flags)
    source.setSpan(InlineCodeBackgroundSpan(0xffdee9c9.toInt(), -25, 7), start + 9, end - 9, flags)
    if (row % 5 == 0) source.setSpan(AbsoluteSizeSpan(34), start + 5, start + 31, flags)
    if (row % 3 == 0) source.setSpan(StyleSpan(android.graphics.Typeface.ITALIC), start, start + 24, flags)
  }
  // Long decorations force both span filtering and per-span visible-line bounds.
  source.setSpan(LinkSpan(1000, 0xff1e7b53.toInt(), true), source.length / 4, source.length * 3 / 4, flags)
  source.setSpan(StrikethroughSpan(), source.length / 3, source.length * 2 / 3, flags)
  source.setSpan(InlineCodeBackgroundSpan(0x22c0a0e0, -25, 7), source.length / 4, source.length * 3 / 4, flags)
  // Ink can cross the viewport even when its source line is entirely outside it.
  source.setSpan(InlineCodeBackgroundSpan(0x66efb045, -120, 80), source.length / 2 - 110, source.length / 2 - 85, flags)
  source.setSpan(LinkSpan(2000, 0xff6a268b.toInt(), true), source.length / 2 - 150, source.length / 2 + 900, flags)
  source.setSpan(AbsoluteSizeSpan(200), source.length / 2 + 897, source.length / 2 + 899, flags)
  return source
}

/** Retain the old whole-document custom passes; framework text sees the same clip. */
private fun legacyDecorations(canvas: Canvas, view: TextSegmentView) {
  val layout = view.shownLayout!!
  val text = layout.text as Spanned
  val path = Path()
  val bounds = RectF()
  val code = Paint()
  val decoration = TextPaint()
  val saved = canvas.save()
  canvas.translate(view.totalPaddingLeft.toFloat(), view.totalPaddingTop.toFloat())
  fun forSpan(span: Any, draw: (Int) -> Unit) {
    val start = text.getSpanStart(span)
    val end = text.getSpanEnd(span)
    for (line in layout.getLineForOffset(start)..layout.getLineForOffset(end - 1)) {
      val from = maxOf(start, layout.getLineStart(line))
      val to = minOf(end, layout.getLineEnd(line))
      if (from >= to) continue
      path.reset()
      layout.getSelectionPath(from, to, path)
      path.computeBounds(bounds, true)
      draw(line)
    }
  }
  for (span in text.getSpans(0, text.length, InlineCodeBackgroundSpan::class.java)) {
    code.color = span.color
    forSpan(span) { line ->
      val baseline = layout.getLineBaseline(line).toFloat()
      canvas.drawRect(bounds.left, baseline + span.ascent, bounds.right, baseline + span.descent, code)
    }
  }
  layout.draw(canvas)
  fun decorate(span: Any, color: Int, strike: Boolean) {
    decoration.set(view.paint)
    text.getSpans(text.getSpanStart(span), text.getSpanEnd(span), AbsoluteSizeSpan::class.java).lastOrNull()?.let {
      decoration.textSize = it.size.toFloat()
    }
    val position = if (strike) decoration.strikeThruPosition else decoration.underlinePosition
    val thickness = if (strike) decoration.strikeThruThickness else decoration.underlineThickness
    decoration.color = color
    decoration.style = Paint.Style.FILL
    forSpan(span) { line ->
      val y = layout.getLineBaseline(line) + position
      canvas.drawRect(bounds.left, y, bounds.right, y + maxOf(1f, thickness), decoration)
    }
  }
  for (link in text.getSpans(0, text.length, LinkSpan::class.java)) decorate(link, link.color, false)
  for (strike in text.getSpans(0, text.length, StrikethroughSpan::class.java)) {
    val start = text.getSpanStart(strike)
    val color = text.getSpans(start, start + 1, ForegroundColorSpan::class.java).lastOrNull()?.foregroundColor ?: view.currentTextColor
    decorate(strike, color, true)
  }
  canvas.restoreToCount(saved)
}

/** Actual views against the old custom passes, with bounded-work counters. */
internal fun runDecorationDrawingChecks(instrumentation: Instrumentation): JSONObject {
  var failure: Throwable? = null
  var checks = 0
  var visiblePaths = 0
  var queriedCharacters = 0
  var legacyPaths = 0
  instrumentation.runOnMainSync {
    try {
      val width = 580
      val viewportHeight = 127
      val paint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply { textSize = 27f; color = Color.BLACK }
      val source = decorationSource()
      val tracked = DecorationText(source)
      val layout = DecorationLayout(RenderedDocument.buildLayout(tracked, paint, width - 30, Layout.Alignment.ALIGN_NORMAL), tracked)
      for (mode in listOf("prepared", "precomputed", "chunked")) {
        val view = TextSegmentView(instrumentation.targetContext).apply {
          layoutParams = FrameLayout.LayoutParams(width, FrameLayout.LayoutParams.WRAP_CONTENT)
          setPadding(13, 11, 17, 9)
        }
        val rendered = RenderedDocument(paint)
        val blocks = listOf(AccessibleBlock(0, source.length, false, 0))
        if (mode == "precomputed") {
          val params = PrecomputedText.Params.Builder(paint).setBreakStrategy(Layout.BREAK_STRATEGY_HIGH_QUALITY)
            .setHyphenationFrequency(Layout.HYPHENATION_FREQUENCY_NONE)
            .setTextDirection(TextDirectionHeuristics.FIRSTSTRONG_LTR).build()
          view.setContent(PrecomputedText.create(source, params), blocks, rendered)
        } else {
          val prepared = if (mode == "chunked") ChunkedTextLayout(source, codeLineStarts(source), paint).frame(width - 30)!!.layout else layout
          view.setPreparedContent(if (mode == "chunked") source else tracked, blocks, rendered, prepared, width - 30)
        }
        view.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
          View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED))
        view.layout(0, 0, width, view.measuredHeight)
        for (hardware in listOf(false, true)) {
          for (top in listOf(0, 24, view.height / 2 + 7, view.height - viewportHeight - 3)) {
            for (inset in listOf(0, 13)) {
              tracked.ranges.clear()
              layout.paths = 0
              val pixels = decorationRaster(width, viewportHeight, hardware) {
                it.clipRect(inset, inset, width - inset, viewportHeight - inset)
                it.translate(0f, -top.toFloat())
                view.draw(it)
              }
              if (mode == "prepared" && !hardware && top == view.height / 2 + 7 && inset == 0) {
                visiblePaths = layout.paths
                queriedCharacters = tracked.ranges.maxOfOrNull { it.second - it.first } ?: 0
              }
              layout.paths = 0
              val expected = decorationRaster(width, viewportHeight, hardware) {
                it.clipRect(inset, inset, width - inset, viewportHeight - inset)
                it.translate(0f, -top.toFloat())
                legacyDecorations(it, view)
              }
              if (mode == "prepared" && !hardware && top == view.height / 2 + 7 && inset == 0) {
                legacyPaths = layout.paths
                Log.i("DecorationDrawingChecks", "sourceCharacters=${source.length} visiblePaths=$visiblePaths legacyPaths=$legacyPaths maxSpanQueryCharacters=$queriedCharacters")
                check(visiblePaths in 1..100 && queriedCharacters in 1 until source.length / 4) {
                  "decoration work must follow viewport: paths=$visiblePaths legacy=$legacyPaths maxQuery=$queriedCharacters source=${source.length}"
                }
              }
              val differences = pixels.indices.count { pixels[it] != expected[it] }
              val first = pixels.indices.firstOrNull { pixels[it] != expected[it] }
              check(differences == 0) { "$mode hardware=$hardware top=$top inset=$inset differs at $differences pixels, first=$first actual=${first?.let { pixels[it] }} expected=${first?.let { expected[it] }}" }
              checks++
            }
          }
        }
        if (mode == "prepared") {
          val changed = SpannableStringBuilder(source)
          val at = source.length / 2 - 300
          changed.setSpan(InlineCodeBackgroundSpan(0xff4369c1.toInt(), -220, 300), at, at + 18, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
          val replacement = RenderedDocument.buildLayout(changed, paint, width - 30, Layout.Alignment.ALIGN_NORMAL)
          view.setPreparedContent(changed, blocks, rendered, replacement, width - 30)
          val top = replacement.getLineBaseline(replacement.getLineForOffset(at)) + view.totalPaddingTop + 180
          for (hardware in listOf(false, true)) {
            fun cropped(legacy: Boolean) = decorationRaster(width, viewportHeight, hardware) {
              it.translate(0f, -top.toFloat())
              if (legacy) legacyDecorations(it, view) else view.draw(it)
            }
            check(cropped(false).contentEquals(cropped(true))) { "replacement background bounds hardware=$hardware" }
            checks++
            val emptyClip = decorationRaster(width, viewportHeight, hardware) {
              it.clipRect(0, 0, 0, 0)
              view.draw(it)
            }
            check(emptyClip.all { it == Color.WHITE }) { "empty clip hardware=$hardware" }
            checks++
          }
          val empty = SpannableStringBuilder()
          view.setPreparedContent(empty, emptyList(), rendered,
            RenderedDocument.buildLayout(empty, paint, width - 30, Layout.Alignment.ALIGN_NORMAL), width - 30)
          for (hardware in listOf(false, true)) {
            val pixels = decorationRaster(width, viewportHeight, hardware) { view.draw(it) }
            check(pixels.all { it == Color.WHITE }) { "empty text hardware=$hardware" }
            checks++
          }
          // Highlighted code has no custom ink; its cached draw should query none.
          val plain = SpannableStringBuilder("colored code word\n".repeat(80))
          for (start in 0 until plain.length step 18) plain.setSpan(ForegroundColorSpan(0xff205da1.toInt()),
            start, minOf(start + 7, plain.length), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
          val plainTracked = DecorationText(plain)
          view.setPreparedContent(plainTracked, listOf(AccessibleBlock(0, plain.length, false, 0)), rendered,
            RenderedDocument.buildLayout(plainTracked, paint, width - 30, Layout.Alignment.ALIGN_NORMAL), width - 30)
          decorationRaster(width, viewportHeight, false) { view.draw(it) }
          plainTracked.ranges.clear()
          val cached = decorationRaster(width, viewportHeight, false) {
            it.translate(0f, -500f)
            view.draw(it)
          }
          check(plainTracked.ranges.isEmpty()) { "cached plain text queried custom spans: ${plainTracked.ranges}" }
          val plainExpected = decorationRaster(width, viewportHeight, false) {
            it.translate(0f, -500f)
            legacyDecorations(it, view)
          }
          check(cached.contentEquals(plainExpected)) { "cached highlighted plain text pixels" }
          checks++
        }
      }
    } catch (error: Throwable) { failure = error }
  }
  failure?.let { throw it }
  return JSONObject().put("pixelChecks", checks).put("visibleSelectionPaths", visiblePaths)
    .put("legacySelectionPaths", legacyPaths).put("maxSpanQueryCharacters", queriedCharacters)
}

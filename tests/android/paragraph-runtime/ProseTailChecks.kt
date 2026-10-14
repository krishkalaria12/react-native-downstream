package com.downstream

import android.app.Instrumentation
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RenderNode
import android.graphics.Typeface
import android.os.SystemClock
import android.text.Layout
import android.text.PrecomputedText
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.TextDirectionHeuristics
import android.text.TextPaint
import android.text.style.ForegroundColorSpan
import android.text.style.LeadingMarginSpan
import android.text.style.LineHeightSpan
import android.text.style.StrikethroughSpan
import android.text.style.TypefaceSpan
import android.text.style.UnderlineSpan
import android.view.View
import org.json.JSONObject
import kotlin.random.Random

/** Block spacing on a block's first line only, as `BlockSpan` adds it. */
private class BlockGap(private val extra: Int) : LineHeightSpan, ReusableSpan {
  override val reuseKey: Any get() = extra
  override fun chooseHeight(text: CharSequence, start: Int, end: Int, spanstartv: Int, lineHeight: Int, fm: Paint.FontMetricsInt) {
    if (start == (text as Spanned).getSpanStart(this)) {
      fm.ascent -= extra
      fm.top -= extra
    }
  }
}

/** A reply's paragraphs: prose, list items, links, struck and underlined runs, Arabic and emoji. */
private fun proseParagraphs(count: Int): List<String> = List(count) { index ->
  when (index % 7) {
    0 -> "Streaming replies arrive a few words at a time, and paragraph $index wraps across several lines of the column before it ends."
    1 -> "First item of the list, with a link to the documentation and a struck price."
    2 -> "Second item, short."
    3 -> "مرحبا بالعالم — an Arabic paragraph in the middle of the reply, number $index."
    4 -> "Emoji 😀 and CJK 处理完成 keep their fallback line heights in paragraph $index."
    5 -> "A long paragraph " + "with many more words than fit on one line ".repeat(6) + "ends here."
    else -> "Underlined text and bold text close paragraph $index."
  }
}

/** One snapshot's spans over the text received so far, built new each time as a snapshot's are. */
private fun styledProse(paragraphs: List<String>, received: Int): SpannableStringBuilder {
  val whole = paragraphs.joinToString("\n")
  val text = SpannableStringBuilder(whole, 0, received)
  if (text.isEmpty()) return text
  val flags = Spanned.SPAN_EXCLUSIVE_EXCLUSIVE
  var start = 0
  for ((index, paragraph) in paragraphs.withIndex()) {
    if (start >= text.length) break
    val end = minOf(start + paragraph.length, text.length)
    val spanEnd = if (end < text.length) end + 1 else end
    text.setSpan(BlockGap(10), start, spanEnd, Spanned.SPAN_PARAGRAPH)
    if (index % 7 == 1 || index % 7 == 2) {
      text.setSpan(LeadingMarginSpan.Standard(48), start, spanEnd, Spanned.SPAN_PARAGRAPH)
    }
    fun mark(word: String, span: () -> Any) {
      val at = paragraph.indexOf(word)
      if (at >= 0 && start + at + word.length <= text.length) {
        text.setSpan(span(), start + at, start + at + word.length, flags)
      }
    }
    mark("link to the documentation") { LinkSpan(index, 0xFF0969DA.toInt()) }
    mark("struck price") { StrikethroughSpan() }
    mark("Underlined") { UnderlineSpan() }
    mark("bold text") { TypefaceSpan(Typeface.DEFAULT_BOLD) }
    mark("Streaming") { ForegroundColorSpan(0xFF8250DF.toInt()) }
    start = end + 1
  }
  return text
}

private fun proseRaster(width: Int, height: Int, top: Int, draw: (Canvas) -> Unit): IntArray {
  val bitmap = Bitmap.createBitmap(width, height.coerceAtLeast(1), Bitmap.Config.ARGB_8888)
  try {
    val canvas = Canvas(bitmap)
    canvas.translate(0f, -top.toFloat())
    draw(canvas)
    return IntArray(bitmap.width * bitmap.height).also { bitmap.getPixels(it, 0, bitmap.width, 0, 0, bitmap.width, bitmap.height) }
  } finally { bitmap.recycle() }
}

/**
 * Streams a reply's tail and holds every snapshot's chunks to the whole tail laid
 * out at once, at two column widths: line geometry, hit tests and pixels.
 */
private fun checkStreamedProse(paint: TextPaint): JSONObject {
  val paragraphs = proseParagraphs(70)
  val total = paragraphs.joinToString("\n").length
  val random = Random(3)
  var received = 0
  var previous = mutableMapOf<Int, ChunkedTextLayout>()
  var snapshots = 0
  var pixelChecks = 0
  var reused = 0
  var laidOut = 0L
  var characters = 0L
  while (received < total) {
    received = minOf(total, received + 2 + random.nextInt(14))
    val text = styledProse(paragraphs, received)
    for (width in intArrayOf(640, 1080)) {
      val body = ChunkedTextLayout(text, codeLineStarts(text), paint, previous[width])
      val frame = body.frame(width) ?: error("no frame at $received")
      // The canonical text is precomputed, and so is every chunk.
      val expected = RenderedDocument.buildLayout(PrecomputedText.create(text, shaping(paint)), paint, width,
        Layout.Alignment.ALIGN_NORMAL)
      // Positions are plain text's: precomputed text snaps an offset inside a word to its end.
      val positions = RenderedDocument.buildLayout(text, paint, width, Layout.Alignment.ALIGN_NORMAL)
      check(frame.height == expected.height) { "height at $received width=$width ${frame.height}/${expected.height}" }
      check(frame.layout.lineCount == expected.lineCount)
      for (line in 0 until expected.lineCount) {
        check(frame.layout.getLineTop(line) == expected.getLineTop(line)) { "top $line at $received" }
        check(frame.layout.getLineStart(line) == expected.getLineStart(line)) { "start $line at $received" }
        check(frame.layout.getLineLeft(line) == expected.getLineLeft(line)) { "left $line at $received" }
        check(frame.layout.getLineRight(line) == expected.getLineRight(line)) { "right $line at $received" }
        check(frame.layout.getParagraphDirection(line) == expected.getParagraphDirection(line))
        // Off a glyph's exact middle, where the two nearest offsets tie.
        val x = expected.getLineLeft(line) + (expected.getLineRight(line) - expected.getLineLeft(line)) * 0.37f
        check(frame.layout.getOffsetForHorizontal(line, x) == positions.getOffsetForHorizontal(line, x)) { "hit $line" }
      }
      for (offset in 0..text.length step maxOf(1, text.length / 61)) {
        check(frame.layout.getPrimaryHorizontal(offset) == positions.getPrimaryHorizontal(offset)) { "x at $offset" }
      }
      if (snapshots % 11 == 0 || received == total) {
        val window = minOf(expected.height, 1200)
        for (top in setOf(0, expected.height - window)) {
          check(proseRaster(width, window, top) { frame.layout.draw(it) }
            .contentEquals(proseRaster(width, window, top) { expected.draw(it) })) { "pixels at $received width=$width" }
        }
        pixelChecks++
      }
      if (width == 640 && received > total / 2) {
        reused += frame.reusedChunks
        laidOut += frame.laidOutCharacters
        characters += text.length
      }
      previous[width] = body
    }
    snapshots++
  }
  val share = laidOut.toDouble() / characters
  check(reused > 0 && share < 0.25) { "prose tail laid out $share of its text" }
  return JSONObject().put("snapshots", snapshots).put("pixelChecks", pixelChecks)
    .put("reusedChunks", reused).put("laidOutShare", share)
}

private fun shaping(paint: TextPaint) = PrecomputedText.Params.Builder(paint).setBreakStrategy(Layout.BREAK_STRATEGY_HIGH_QUALITY)
  .setHyphenationFrequency(Layout.HYPHENATION_FREQUENCY_NONE).setTextDirection(TextDirectionHeuristics.FIRSTSTRONG_LTR).build()

/** The live tail draws what the canonical `TextView` draws, link underlines and strikes included. */
private fun checkLiveTailView(instrumentation: Instrumentation, paint: TextPaint): JSONObject {
  val paragraphs = proseParagraphs(21)
  val text = styledProse(paragraphs, paragraphs.joinToString("\n").length)
  val width = 720
  val blocks = listOf(AccessibleBlock(0, text.length, false, 0))
  val params = PrecomputedText.Params.Builder(paint).setBreakStrategy(Layout.BREAK_STRATEGY_HIGH_QUALITY)
    .setHyphenationFrequency(Layout.HYPHENATION_FREQUENCY_NONE).setTextDirection(TextDirectionHeuristics.FIRSTSTRONG_LTR).build()
  val rendered = RenderedDocument(paint)
  var linkPressed = -1
  var pixelsMatch = false
  instrumentation.runOnMainSync {
    fun measured(view: TextSegmentView): TextSegmentView = view.apply {
      measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
        View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED))
      layout(0, 0, width, measuredHeight)
    }
    val live = measured(TextSegmentView(instrumentation.targetContext).apply {
      setSegment(TextSegment(text, blocks, gapBefore = 0, caretOffset = -1, caretKind = 0,
        streamingParagraph = ChunkedTextLayout(text, codeLineStarts(text), paint)), rendered, 0)
    })
    val canonical = measured(TextSegmentView(instrumentation.targetContext).apply {
      setSegment(TextSegment(PrecomputedText.create(text, params), blocks, gapBefore = 0, caretOffset = -1, caretKind = 0),
        rendered, 0)
    })
    check(live.text.isEmpty()) { "the live tail assigned platform text" }
    check(live.measuredHeight == canonical.measuredHeight) { "live ${live.measuredHeight}/${canonical.measuredHeight}" }
    val a = proseRaster(width, live.measuredHeight, 0) { live.draw(it) }
    val b = proseRaster(width, canonical.measuredHeight, 0) { canonical.draw(it) }
    pixelsMatch = a.contentEquals(b)
    if (!pixelsMatch) {
      var minX = width; var maxX = -1; var minY = Int.MAX_VALUE; var maxY = -1; var count = 0
      val rows = sortedSetOf<Int>()
      for (i in a.indices) if (a[i] != b[i]) {
        count++; val x = i % width; val y = i / width
        minX = minOf(minX, x); maxX = maxOf(maxX, x); minY = minOf(minY, y); maxY = maxOf(maxY, y); rows.add(y)
      }
      val layout = live.shownLayout!!
      val lines = rows.map { layout.getLineForVertical(it) }.toSortedSet()
      error("diff count=$count box=($minX,$minY)-($maxX,$maxY) lines=$lines texts=" +
        lines.take(6).map { text.subSequence(layout.getLineStart(it), layout.getLineEnd(it)).toString().take(40) })
    }
    // A tap on the link in the growing tail reaches the link.
    val layout = live.shownLayout!!
    val at = text.toString().indexOf("link to the documentation") + 3
    val line = layout.getLineForOffset(at)
    val x = layout.getPrimaryHorizontal(at)
    val y = (layout.getLineTop(line) + layout.getLineBottom(line)) / 2f
    linkPressed = linkAt(layout, x, y)
  }
  check(pixelsMatch) { "live tail pixels differ from the canonical TextView" }
  check(linkPressed == 1) { "link hit test in the live tail: $linkPressed" }
  return JSONObject().put("width", width).put("linkHit", true)
}

/**
 * Per-snapshot cost of a streaming multi-paragraph tail, before and after: the
 * measuring thread's work, then the UI thread's assignment, measurement and
 * display-list recording. Emulator timings: relative, not a phone's frame time.
 */
private fun benchmarkProseTail(instrumentation: Instrumentation, paint: TextPaint): JSONObject {
  val paragraphs = proseParagraphs(40)
  val total = paragraphs.joinToString("\n").length
  val ends = ArrayList<Int>()
  val random = Random(5)
  var end = 0
  while (end < total) {
    end = minOf(total, end + 3 + random.nextInt(12))
    ends.add(end)
  }
  val width = 1000
  val params = PrecomputedText.Params.Builder(paint).setBreakStrategy(Layout.BREAK_STRATEGY_HIGH_QUALITY)
    .setHyphenationFrequency(Layout.HYPHENATION_FREQUENCY_NONE).setTextDirection(TextDirectionHeuristics.FIRSTSTRONG_LTR).build()
  val rendered = RenderedDocument(paint)
  val result = JSONObject().put("snapshots", ends.size).put("finalCharacters", total).put("width", width)
  for (round in 0 until 2) {
    val canonicalMeasure = LongArray(ends.size)
    val canonicalUi = LongArray(ends.size)
    val chunkedMeasure = LongArray(ends.size)
    val chunkedUi = LongArray(ends.size)
    val views = arrayOfNulls<TextSegmentView>(2)
    val node = RenderNode("tail")
    instrumentation.runOnMainSync {
      views[0] = TextSegmentView(instrumentation.targetContext)
      views[1] = TextSegmentView(instrumentation.targetContext)
    }
    var previous: ChunkedTextLayout? = null
    for ((index, received) in ends.withIndex()) {
      val text = styledProse(paragraphs, received)
      val blocks = listOf(AccessibleBlock(0, text.length, false, 0))
      // Canonical: precomputed and measured on the measuring thread, then laid
      // out again by the TextView on the UI thread.
      var started = SystemClock.elapsedRealtimeNanos()
      val precomputed = PrecomputedText.create(text, params)
      val height = RenderedDocument.buildLayout(precomputed, paint, width, Layout.Alignment.ALIGN_NORMAL).height
      canonicalMeasure[index] = SystemClock.elapsedRealtimeNanos() - started
      val canonicalSegment = TextSegment(precomputed, blocks, gapBefore = 0, caretOffset = -1, caretKind = 0)
      // Chunked: the tail's last chunk laid out on the measuring thread.
      started = SystemClock.elapsedRealtimeNanos()
      val body = ChunkedTextLayout(text, codeLineStarts(text), paint, previous)
      check(body.frame(width)!!.height == height)
      chunkedMeasure[index] = SystemClock.elapsedRealtimeNanos() - started
      previous = body
      val chunkedSegment = TextSegment(text, blocks, gapBefore = 0, caretOffset = -1, caretKind = 0, streamingParagraph = body)
      instrumentation.runOnMainSync {
        for ((slot, segment) in listOf(canonicalSegment, chunkedSegment).withIndex()) {
          val view = views[slot]!!
          val uiStarted = SystemClock.elapsedRealtimeNanos()
          view.setSegment(segment, rendered, 0)
          view.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY))
          view.layout(0, 0, width, height)
          node.setPosition(0, 0, width, height)
          val canvas = node.beginRecording()
          view.draw(canvas)
          node.endRecording()
          val elapsed = SystemClock.elapsedRealtimeNanos() - uiStarted
          if (slot == 0) canonicalUi[index] = elapsed else chunkedUi[index] = elapsed
        }
      }
    }
    if (round == 0) continue
    fun stats(measure: LongArray, ui: LongArray): JSONObject {
      fun median(values: LongArray) = values.sorted()[values.size / 2] / 1000.0
      fun p95(values: LongArray) = values.sorted()[values.size * 95 / 100] / 1000.0
      val last = ends.size * 3 / 4 until ends.size
      return JSONObject().put("measureMedianUs", median(measure)).put("uiMedianUs", median(ui))
        .put("uiP95Us", p95(ui))
        .put("uiLastQuarterMedianUs", median(LongArray(last.count()) { ui[last.first + it] }))
        .put("measureLastQuarterMedianUs", median(LongArray(last.count()) { measure[last.first + it] }))
    }
    result.put("canonical", stats(canonicalMeasure, canonicalUi))
    result.put("chunked", stats(chunkedMeasure, chunkedUi))
  }
  return result
}

internal fun runProseTailChecks(instrumentation: Instrumentation): JSONObject {
  val paint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply { textSize = 34f; color = Color.BLACK }
  val streamed = checkStreamedProse(paint)
  android.util.Log.i("ProseTailChecks", "streamed checks passed")
  val view = checkLiveTailView(instrumentation, paint)
  android.util.Log.i("ProseTailChecks", "view checks passed")
  val benchmark = benchmarkProseTail(instrumentation, paint)
  return JSONObject().put("streamed", streamed).put("liveView", view).put("benchmark", benchmark)
}

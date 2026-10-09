package com.downstream

import android.app.Instrumentation
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Path
import android.graphics.Paint
import android.os.Debug
import android.os.SystemClock
import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint
import android.widget.TextView
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.Executors
import java.util.concurrent.Callable

private fun simpleLayout(text: String, paint: TextPaint, width: Int, lineHeight: Int = 0): StaticLayout {
  val display: CharSequence = if (lineHeight <= 0) text else android.text.SpannableString(text).apply {
    setSpan(object : android.text.style.LineHeightSpan {
      override fun chooseHeight(text: CharSequence, start: Int, end: Int, v: Int, height: Int, fm: Paint.FontMetricsInt) {
        val extra = lineHeight - (fm.descent - fm.ascent)
        if (extra > 0) { fm.ascent -= extra; fm.top = minOf(fm.top, fm.ascent) }
      }
    }, 0, length, android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
  }
  return StaticLayout.Builder.obtain(display, 0, display.length, paint, width)
    .setIncludePad(false).setBreakStrategy(Layout.BREAK_STRATEGY_SIMPLE)
    .setHyphenationFrequency(Layout.HYPHENATION_FREQUENCY_NONE)
    .setTextDirection(android.text.TextDirectionHeuristics.LTR)
    .setLineSpacing(0f, 1f).setUseLineSpacingFromFallbacks(true).setUseBoundsForWidth(false).build()
}

private fun raster(width: Int, height: Int, draw: (Canvas) -> Unit): IntArray {
  val bitmap = Bitmap.createBitmap(width, maxOf(height, 1), Bitmap.Config.ARGB_8888)
  try {
    draw(Canvas(bitmap))
    return IntArray(width * maxOf(height, 1)).also {
      bitmap.getPixels(it, 0, width, 0, 0, width, maxOf(height, 1))
    }
  } finally { bitmap.recycle() }
}

/** Verifies the Android seam property before depending on it for production snapshots. */
private fun checkSliceSeams(paint: TextPaint): Int {
  var checked = 0
  for (width in listOf(173, 280, 601)) {
    val source = ("A few words fit here, but incompletely streamed words need guarding. " +
      "More  spaces   and numbers 123456 need exact lines. ").repeat(3)
    for (end in 1..source.length step 3) {
      val text = source.substring(0, end)
      val full = simpleLayout(text, paint, width)
      if (full.lineCount < 3) continue
      val lastWord = text.indexOfLast { it == ' ' } + 1
      var seamLine = 0
      while (seamLine + 2 < full.lineCount && full.getLineEnd(seamLine + 1) <= lastWord) seamLine++
      if (seamLine == 0) continue
      val seam = full.getLineStart(seamLine)
      val prefix = simpleLayout(text.substring(0, seam), paint, width)
      val suffix = simpleLayout(text.substring(seam), paint, width)
      val top = full.getLineTop(seamLine)
      check(top + suffix.height == full.height) { "slice height mismatch width=$width end=$end seam=$seam" }
      val expected = raster(width, full.height) { full.draw(it) }
      val actual = raster(width, full.height) { canvas ->
        canvas.save(); canvas.clipRect(0, 0, width, top); prefix.draw(canvas); canvas.restore()
        canvas.save(); canvas.translate(0f, top.toFloat()); suffix.draw(canvas); canvas.restore()
      }
      check(expected.contentEquals(actual)) { "slice pixels mismatch width=$width end=$end seam=$seam" }
      checked++
    }
  }
  return checked
}

private fun assertFrame(frame: StreamingParagraphFrame, paint: TextPaint, width: Int, lineHeight: Int = 0) {
  val expected = simpleLayout(frame.source, paint, width, lineHeight)
  val actual = frame.layout
  check(frame.height == expected.height) { "height width=$width chars=${frame.source.length}: ${frame.height}/${expected.height}" }
  check(actual.lineCount == expected.lineCount) { "line count width=$width chars=${frame.source.length}" }
  for (line in 0 until expected.lineCount) {
    check(actual.getLineStart(line) == expected.getLineStart(line)) { "line start $line chars=${frame.source.length}" }
    check(actual.getLineEnd(line) == expected.getLineEnd(line)) { "line end $line chars=${frame.source.length}" }
    check(actual.getLineTop(line) == expected.getLineTop(line)) { "line top $line chars=${frame.source.length}" }
    check(actual.getLineBaseline(line) == expected.getLineBaseline(line)) { "line baseline $line chars=${frame.source.length}" }
    for (x in listOf(0f, width * 0.3f, width * 0.7f, width.toFloat())) {
      check(actual.getOffsetForHorizontal(line, x) == expected.getOffsetForHorizontal(line, x)) { "offset/horizontal line=$line x=$x chars=${frame.source.length}" }
    }
    for (offset in (expected.getLineStart(line)..expected.getLineEnd(line)).filter { it % 13 == 0 || it == expected.getLineStart(line) || it == expected.getLineEnd(line) }) {
      check(kotlin.math.abs(actual.getPrimaryHorizontal(offset) - expected.getPrimaryHorizontal(offset)) < 0.01f) { "primary horizontal offset=$offset chars=${frame.source.length}" }
      check(kotlin.math.abs(actual.getSecondaryHorizontal(offset) - expected.getSecondaryHorizontal(offset)) < 0.01f) { "secondary horizontal offset=$offset chars=${frame.source.length}" }
    }
  }
  val expectedPixels = raster(width, expected.height) { expected.draw(it) }
  val actualPixels = raster(width, actual.height) { actual.draw(it) }
  check(expectedPixels.contentEquals(actualPixels)) { "pixels width=$width chars=${frame.source.length} reused=${frame.reusedLines}" }
  // Reveal and accessibility use global selection paths across retained/suffix seams.
  for ((start, end) in listOf(0 to frame.source.length, frame.source.length / 3 to frame.source.length * 2 / 3)) {
    val ink = Paint().apply { color = Color.RED }
    fun selection(layout: Layout) = raster(width, layout.height) {
      val path = Path(); layout.getSelectionPath(start, end, path); it.drawPath(path, ink)
    }
    check(selection(expected).contentEquals(selection(actual))) { "selection $start..$end chars=${frame.source.length}" }
  }
}

private fun checkProductionFrames(paint: TextPaint): Int {
  var checked = 0
  val sources = listOf(
    "A few words fit here, but incomplete words must remain mutable. ".repeat(15),
    "Tiny a ab abc abcd abcde   many    spaces. 12345 ,.!? ".repeat(12),
    "Start with completed words. " + "w".repeat(750) + " end and more completed words. ".repeat(10),
    "A short sentence. " .repeat(35) + "trailing    ",
    "office affinities ffi ffffii efficient coffee. ".repeat(12),
  )
  for (width in listOf(173, 280, 601)) {
    for (source in sources) {
      var previous: StreamingParagraphLayout? = null
      val retained = mutableListOf<StreamingParagraphFrame>()
      for (end in 1..source.length step 7) {
        val current = StreamingParagraphLayout(source.substring(0, end), paint, previous)
        val frame = checkNotNull(current.frame(width)) { "ordinary prose unexpectedly fell back end=$end" }
        assertFrame(frame, paint, width)
        if (checked % 43 == 0) retained += frame
        previous = current
        checked++
      }
      val last = StreamingParagraphLayout(source, paint, previous)
      assertFrame(checkNotNull(last.frame(width)), paint, width)
      // Preparing further appends must not mutate layouts in previously published snapshots.
      retained.forEach { assertFrame(it, paint, width) }
    }
  }
  val source = sources.first()
  for ((size, lineHeight) in listOf(27f to 33, 33f to 65)) {
    val typography = TextPaint(paint).apply { textSize = size }
    var previous: StreamingParagraphLayout? = null
    for (end in 13..source.length step 13) {
      val current = StreamingParagraphLayout(source.substring(0, end), typography, previous, lineHeight)
      assertFrame(checkNotNull(current.frame(280)), typography, 280, lineHeight)
      previous = current
      checked++
    }
  }
  val snapshot = StreamingParagraphLayout(source, paint)
  val narrow = checkNotNull(snapshot.frame(173))
  val wide = checkNotNull(snapshot.frame(601))
  check(snapshot.frame(173) === narrow) { "two exact widths did not retain first frame" }
  assertFrame(wide, paint, 601)
  val changedPaint = TextPaint(paint).apply { textSize += 3f }
  assertFrame(checkNotNull(StreamingParagraphLayout(source + "appended", changedPaint, snapshot).frame(173)), changedPaint, 173)
  assertFrame(checkNotNull(StreamingParagraphLayout("replacement text", paint, snapshot).frame(173)), paint, 173)
  val savedPixels = raster(173, narrow.height) { narrow.layout.draw(it) }
  paint.color = Color.BLUE
  check(savedPixels.contentEquals(raster(173, narrow.height) { narrow.layout.draw(it) })) { "published frame retained mutable caller paint" }
  paint.color = Color.BLACK
  check(StreamingParagraphLayout("w".repeat(10000), paint).frame(173) == null) { "oversized unfinished word must use bounded fallback" }
  for (unsupported in listOf("Unicode café", "Arabic العربية", "emoji 😀", "tab\ttext", "line\nbreak")) {
    check(StreamingParagraphLayout(unsupported, paint).frame(173) == null) { "unsupported text bypassed canonical fallback" }
  }
  val executor = Executors.newFixedThreadPool(4)
  try {
    executor.invokeAll((0 until 24).map { index -> Callable {
      val width = if (index % 2 == 0) 173 else 601
      assertFrame(checkNotNull(snapshot.frame(width)), paint, width)
    } }).forEach { it.get() }
  } finally { executor.shutdown() }
  return checked
}

private fun checkInterleavedStreams(paint: TextPaint): Int {
  val cache = StreamingParagraphSeeds<StreamingParagraphLayout>(16, 2 * 1024 * 1024L)
  val sources = listOf("Alpha completed ordinary words share a font but preserve their own append history. ",
    "Beta independent completed words must preserve independent layout frames. ").map { it.repeat(600).take(32768) }
  val final = mutableListOf<StreamingParagraphFrame>()
  var checked = 0
  for (end in 128..32768 step 128) for ((index, source) in sources.withIndex()) {
    val text = source.substring(0, end)
    val previous = cache.matching("same typography", text)
    val current = StreamingParagraphLayout(text, paint, previous)
    val frame = checkNotNull(current.frame(601)) { "interleaved stream $index lost append seed at $end" }
    if (end > 8192) check(frame.reusedLines > 0)
    cache.put("same typography", text, current)
    if (end == 32768) final += frame
    checked++
  }
  check(cache.matching("changed typography", sources.first()) == null)
  check(cache.matching("same typography", "replacement/reset") == null)
  for (frame in final) {
    val expected = simpleLayout(frame.source, paint, 601)
    check(frame.height == expected.height)
    // Compare the entire tall paragraph in bounded bitmap strips to avoid
    // making the validation itself retain hundreds of megabytes of pixels.
    for (top in 0 until frame.height step 256) {
      val stripHeight = minOf(256, frame.height - top)
      val actual = raster(601, stripHeight) { it.translate(0f, -top.toFloat()); frame.layout.draw(it) }
      val wanted = raster(601, stripHeight) { it.translate(0f, -top.toFloat()); expected.draw(it) }
      check(actual.contentEquals(wanted)) { "interleaved final pixels differ at strip $top" }
    }
  }
  return checked
}

private fun checkProductionView(instrumentation: Instrumentation, paint: TextPaint): Int {
  var checked = 0
  var failure: Throwable? = null
  instrumentation.runOnMainSync {
    try {
    val rendered = RenderedDocument(paint)
    val view = TextSegmentView(instrumentation.targetContext)
    val host = DownstreamMarkdownView(instrumentation.targetContext)
    host.addView(view)
    fun segment(source: String, model: StreamingParagraphLayout? = null) = TextSegment(
      android.text.SpannedString(source), listOf(AccessibleBlock(0, source.length, false, 0)),
      gapBefore = 0, caretOffset = -1, caretKind = 0, streamingParagraph = model)
    fun measure(width: Int) {
      view.measure(android.view.View.MeasureSpec.makeMeasureSpec(width, android.view.View.MeasureSpec.EXACTLY),
        android.view.View.MeasureSpec.makeMeasureSpec(0, android.view.View.MeasureSpec.UNSPECIFIED))
      view.layout(0, 0, width, view.measuredHeight)
      host.layout(0, 0, width, view.measuredHeight)
    }
    val source = "Real production TextSegmentView appends should never shape full framework text. ".repeat(14)
    var previous: StreamingParagraphLayout? = null
    for (end in 17..source.length step 17) {
      val text = source.substring(0, end)
      val model = StreamingParagraphLayout(text, paint, previous)
      view.setSegment(segment(text, model), rendered, 0)
      measure(601)
      check(view.text.isEmpty()) { "live view assigned full framework text" }
      val expected = simpleLayout(text, paint, 601)
      check(view.measuredHeight == expected.height) { "view/framework height mismatch" }
      check(raster(601, view.height) { view.draw(it) }.contentEquals(raster(601, expected.height) { expected.draw(it) })) { "production view draw differs from full SIMPLE" }
      val node = checkNotNull(view.accessibilityNodeProvider.createAccessibilityNodeInfo(0)) { "missing live paragraph accessibility entry" }
      check(node.text.toString() == text) { "provider omitted live framework-empty paragraph text" }
      val bounds = android.graphics.Rect(); node.getBoundsInParent(bounds)
      check(bounds.height() > 0 && bounds.bottom <= view.height) { "incorrect provider live bounds: $bounds" }
      previous = model
      checked++
    }
    val finalText = source.substring(0, (source.length / 17) * 17)
    // Actual production reveal mapping uses the global composite offsets.
    for (rise in listOf(false, true)) {
      val reveal = RevealFrame(doubleArrayOf(0.0, 0.0, 150.0), 1f, rise)
      val start = finalText.length / 3
      val end = start + 70
      reveal.build(longArrayOf(0, start.toLong(), end.toLong()), doubleArrayOf(0.0), 75.0)
      host.frame = reveal
      val expectedLayout = simpleLayout(finalText, paint, 601)
      val band = RevealBand(1).apply {
        count = 1; offsets[0] = start; offsets[1] = end
        alphas[0] = reveal.unitAt(0).alpha; rises[0] = reveal.unitAt(0).rise
      }
      val paths = RevealClipPaths()
      val mask = paths.build(expectedLayout, band, 0f, 0f)
      val expected = raster(601, expectedLayout.height) { canvas ->
        canvas.save(); canvas.clipOutPath(paths.hidden); expectedLayout.draw(canvas); canvas.restore()
        for (bucket in 1..MAX_ALPHA_BUCKETS) {
          if (mask and (1 shl bucket) == 0) continue
          val path = paths.buckets[bucket - 1]
          val bounds = android.graphics.RectF(); path.computeBounds(bounds, true)
          val save = canvas.saveLayerAlpha(bounds, (bucketAlpha(bucket) * 255f).toInt())
          canvas.clipPath(path); canvas.save(); canvas.translate(0f, paths.rises[bucket - 1])
          expectedLayout.draw(canvas); canvas.restore(); canvas.restoreToCount(save)
        }
      }
      check(raster(601, view.height) { view.draw(it) }.contentEquals(expected)) { "actual view reveal clipping/alpha/rise differs from full SIMPLE" }
      checked++
    }
    host.frame = null
    // Width changes use the actual requested frame, never a nearby cached width.
    for (width in listOf(173, 601, 280)) {
      measure(width)
      val expected = simpleLayout(view.accessibilityNodeProvider.createAccessibilityNodeInfo(0)!!.text.toString(), paint, width)
      check(view.measuredHeight == expected.height)
      check(view.text.isEmpty())
      checked++
    }
    // Selection must switch to the mature canonical TextView implementation.
    view.setSelectable(true)
    measure(280)
    check(view.text.toString() == finalText && view.layout != null && view.isTextSelectable)
    check(view.breakStrategy == Layout.BREAK_STRATEGY_HIGH_QUALITY)
    checked++
    view.setSelectable(false)
    // Host update order: old settled selection is enabled before a new live segment arrives.
    view.setSelectable(true)
    val liveAgain = StreamingParagraphLayout(finalText, paint)
    view.setSegment(segment(finalText, liveAgain), rendered, 0)
    view.setSelectable(false)
    measure(280)
    check(view.text.isEmpty()) { "recycled selectable view failed to activate live branch" }
    check(view.measuredHeight == liveAgain.frame(280)!!.height)
    checked++
    view.setSegment(segment(finalText), rendered, 0)
    measure(280)
    check(view.text.toString() == finalText && view.layout != null)
    val settled = RenderedDocument.buildLayout(android.text.SpannedString(finalText), paint, 280, Layout.Alignment.ALIGN_NORMAL)
    check(view.measuredHeight == settled.height) { "settlement did not restore canonical HQ measurement" }
    checked++
    view.clearContent()
    check(view.text.isEmpty() && view.accessibilityNodeProvider.createAccessibilityNodeInfo(0) == null)

    // An oversized unfinished word falls back to canonical assignment at measure.
    val largeWord = "w".repeat(10000)
    view.setSegment(segment(largeWord, StreamingParagraphLayout(largeWord, paint)), rendered, 0)
    measure(280)
    check(view.text.toString() == largeWord && view.layout != null)
    checked++

    // A long live paragraph remounted with zero width must reuse its prepared real-width frame.
    val longSource = "Safe completed words should survive viewport recycling without full shaping. ".repeat(170)
    var warm: StreamingParagraphLayout? = null
    for (end in 128..longSource.length step 128) {
      warm = StreamingParagraphLayout(longSource.substring(0, end), paint, warm)
      checkNotNull(warm.frame(601))
    }
    val longText = warm!!.text
    val remount = TextSegmentView(instrumentation.targetContext)
    host.addView(remount)
    check(remount.width == 0)
    remount.setSegment(segment(longText, warm), rendered, 0)
    remount.measure(android.view.View.MeasureSpec.makeMeasureSpec(601, android.view.View.MeasureSpec.EXACTLY),
      android.view.View.MeasureSpec.makeMeasureSpec(0, android.view.View.MeasureSpec.UNSPECIFIED))
    remount.layout(0, 0, 601, remount.measuredHeight)
    check(remount.text.isEmpty()) { "zero-width remount unnecessarily restored full framework shaping" }
    check(remount.measuredHeight == warm.frame(601)!!.height)
    check(raster(601, remount.height) { remount.draw(it) }.contentEquals(raster(601, remount.height) { simpleLayout(longText, paint, 601).draw(it) }))
    checked++
    // A cold narrow width hits the bounded suffix cap; returning to the warm
    // width must reactivate the existing immutable live frame on the same view.
    remount.measure(android.view.View.MeasureSpec.makeMeasureSpec(173, android.view.View.MeasureSpec.EXACTLY),
      android.view.View.MeasureSpec.makeMeasureSpec(0, android.view.View.MeasureSpec.UNSPECIFIED))
    remount.layout(0, 0, 173, remount.measuredHeight)
    check(remount.text.toString() == longText)
    remount.measure(android.view.View.MeasureSpec.makeMeasureSpec(601, android.view.View.MeasureSpec.EXACTLY),
      android.view.View.MeasureSpec.makeMeasureSpec(0, android.view.View.MeasureSpec.UNSPECIFIED))
    remount.layout(0, 0, 601, remount.measuredHeight)
    check(remount.text.isEmpty()) { "warm width failed to reactivate after cold-width fallback" }
    check(remount.measuredHeight == warm.frame(601)!!.height)
    check(raster(601, remount.height) { remount.draw(it) }.contentEquals(raster(601, remount.height) { simpleLayout(longText, paint, 601).draw(it) }))
    checked++
    } catch (error: Throwable) { failure = error }
  }
  failure?.let { throw it }
  return checked
}

private fun paragraphBenchmarks(paint: TextPaint): JSONArray {
  val results = JSONArray()
  for (size in listOf(8192, 16384, 32768)) {
    val source = "Plain words are streamed in small pieces to reuse completed lines. ".repeat(size / 60 + 1).take(size)
    // Actual timed layout work only. Drawing/parity assertions run outside timings.
    // Three ABBA rounds after one excluded round warm up Android/font/JIT caches.
    for (round in -1..2) for ((sample, variant) in listOf("full-simple", "incremental", "incremental", "full-simple", "full-hq").withIndex()) {
      var previous: StreamingParagraphLayout? = null
      var shaped = 0L
      var reused = 0L
      val wallStart = SystemClock.elapsedRealtimeNanos()
      val cpuStart = Debug.threadCpuTimeNanos()
      for (end in 16..size step 16) {
        val text = source.substring(0, end)
        if (variant == "incremental") {
          val current = StreamingParagraphLayout(text, paint, previous)
          val frame = checkNotNull(current.frame(601))
          shaped += frame.shapedCharacters
          reused += frame.reusedLines
          previous = current
        } else {
          val layout = if (variant == "full-simple") simpleLayout(text, paint, 601)
          else StaticLayout.Builder.obtain(text, 0, text.length, paint, 601)
            .setIncludePad(false).setBreakStrategy(Layout.BREAK_STRATEGY_HIGH_QUALITY)
            .setHyphenationFrequency(Layout.HYPHENATION_FREQUENCY_NONE).setUseBoundsForWidth(false).build()
          check(layout.height > 0)
          shaped += text.length
        }
      }
      val record = JSONObject().put("sourceCharacters", size).put("chunkCharacters", 16)
        .put("width", 601).put("variant", variant).put("order", sample)
        .put("round", round)
        .put("cpuMs", (Debug.threadCpuTimeNanos() - cpuStart) / 1e6)
        .put("wallMs", (SystemClock.elapsedRealtimeNanos() - wallStart) / 1e6)
        .put("shapedCharacters", shaped).put("reusedLines", reused)
      if (round >= 0) results.put(record)
      if (variant == "incremental") check(shaped < size * 100L) { "suffix shaping work unexpectedly scaled with whole growing paragraph: $shaped" }
    }
  }
  return results
}

internal fun runParagraphChecks(instrumentation: Instrumentation): String {
  val paint = TextPaint().apply { textSize = 32f; color = Color.BLACK; isAntiAlias = true }
  val text = "A real Android paragraph layout with several wrapped words. ".repeat(20)
  val layout = StaticLayout.Builder.obtain(text, 0, text.length, paint, 280)
    .setIncludePad(false).setBreakStrategy(Layout.BREAK_STRATEGY_SIMPLE)
    .setHyphenationFrequency(Layout.HYPHENATION_FREQUENCY_NONE).build()
  check(layout.lineCount > 2)
  val bitmap = Bitmap.createBitmap(280, layout.height, Bitmap.Config.ARGB_8888)
  layout.draw(Canvas(bitmap))
  check(IntArray(bitmap.width * bitmap.height).also {
    bitmap.getPixels(it, 0, bitmap.width, 0, 0, bitmap.width, bitmap.height)
  }.any { it != 0 })
  instrumentation.runOnMainSync {
    val view = TextView(instrumentation.targetContext)
    view.text = text
    view.measure(android.view.View.MeasureSpec.makeMeasureSpec(280, android.view.View.MeasureSpec.EXACTLY),
      android.view.View.MeasureSpec.makeMeasureSpec(0, android.view.View.MeasureSpec.UNSPECIFIED))
    check(view.measuredHeight > 0)
  }
  bitmap.recycle()
  val slices = checkSliceSeams(paint)
  android.util.Log.i("ParagraphChecks", "seam checks passed")
  val frames = checkProductionFrames(paint)
  android.util.Log.i("ParagraphChecks", "core checks passed")
  val views = checkProductionView(instrumentation, paint)
  android.util.Log.i("ParagraphChecks", "view checks passed")
  val interleaved = checkInterleavedStreams(paint)
  android.util.Log.i("ParagraphChecks", "interleaved checks passed")
  val benchmarks = paragraphBenchmarks(paint)
  return JSONObject().put("sdk", android.os.Build.VERSION.SDK_INT).put("model", android.os.Build.MODEL)
    .put("abi", android.os.Build.SUPPORTED_ABIS.first()).put("greedySeamChecks", slices)
    .put("productionPrefixChecks", frames).put("productionViewChecks", views).put("interleavedStreamChecks", interleaved).put("benchmarks", benchmarks).toString(2)
}

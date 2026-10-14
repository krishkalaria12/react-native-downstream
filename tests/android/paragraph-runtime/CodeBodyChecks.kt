package com.downstream

import android.app.Instrumentation
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.ColorSpace
import android.graphics.HardwareRenderer
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PixelFormat
import android.graphics.RectF
import android.graphics.RenderNode
import android.graphics.Typeface
import android.hardware.HardwareBuffer
import android.media.ImageReader
import android.os.SystemClock
import android.text.Layout
import android.text.PrecomputedText
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.TextDirectionHeuristics
import android.text.TextPaint
import android.text.style.AbsoluteSizeSpan
import android.text.style.ForegroundColorSpan
import android.text.style.LineHeightSpan
import android.text.style.TypefaceSpan
import android.view.View
import org.json.JSONObject
import kotlin.math.ceil
import kotlin.random.Random

/**
 * Block spacing on a block's first line only, as `BlockSpan` adds it: the span
 * that makes a chunk's copy start before its first line necessary.
 */
private class FirstLineSpacing(private val extra: Int) : LineHeightSpan, ReusableSpan {
  override val reuseKey: Any get() = extra
  override fun chooseHeight(text: CharSequence, start: Int, end: Int, spanstartv: Int, lineHeight: Int, fm: Paint.FontMetricsInt) {
    if (start == (text as Spanned).getSpanStart(this)) {
      fm.ascent -= extra
      fm.top -= extra
    }
  }
}

/** A span the chunker has no value for, so a chunk under it must never be reused. */
private class OpaqueSpan : android.text.style.CharacterStyle() {
  override fun updateDrawState(paint: android.text.TextPaint) {}
}

private val KEYWORDS = Regex("\\b(const|let|function|return|if|else|import|from|export|await|async)\\b")
private val STRINGS = Regex("\"[^\"\\n]*\"?")
private val NUMBERS = Regex("\\b\\d+\\b")
private val COMMENTS = Regex("//[^\\n]*")

/** A TypeScript-like reply's code: indents, tabs, long lines, blank lines, emoji and CJK. */
private fun codeSource(lines: Int): String = buildString {
  for (line in 0 until lines) {
    when (line % 13) {
      0 -> append("import { value$line } from \"./module$line\"\n")
      1 -> append("export async function handler$line(input: Request, retries = ${line * 3}) {\n")
      2 -> append("\tconst response = await fetch(`https://example.com/api/$line`, { method: \"POST\" })\n")
      3 -> append("  if (response.status !== 200) return { ok: false, line: $line } // retry later\n")
      4 -> append("\n")
      5 -> append("  // ✅ handled 😀 — 处理完成 $line\n")
      6 -> append("  const payload = { " + (0 until 18).joinToString(", ") { "field$it: \"v$it\"" } + " }\n")
      else -> append("    return compute(${line}, \"label $line\", [${line}, ${line + 1}, ${line + 2}])\n")
    }
  }
}

/** One snapshot's spans, built new each time as a snapshot's are. */
private fun highlighted(source: String, end: Int, colors: IntArray = TOKEN_COLORS): SpannableStringBuilder {
  val text = SpannableStringBuilder(source, 0, end)
  if (text.isEmpty()) return text
  val flags = Spanned.SPAN_EXCLUSIVE_EXCLUSIVE
  text.setSpan(AbsoluteSizeSpan(28), 0, text.length, flags)
  text.setSpan(TypefaceSpan(Typeface.MONOSPACE), 0, text.length, flags)
  text.setSpan(ForegroundColorSpan(0xFF24292E.toInt()), 0, text.length, flags)
  text.setSpan(FirstLineSpacing(6), 0, text.length, Spanned.SPAN_PARAGRAPH)
  for ((kind, pattern) in listOf(KEYWORDS, STRINGS, NUMBERS, COMMENTS).withIndex()) {
    for (match in pattern.findAll(text)) {
      text.setSpan(ForegroundColorSpan(colors[kind]), match.range.first, match.range.last + 1, flags)
    }
  }
  return text
}

private val TOKEN_COLORS = intArrayOf(0xFFD73A49.toInt(), 0xFF032F62.toInt(), 0xFF005CC5.toInt(), 0xFF6A737D.toInt())

private fun raster(width: Int, height: Int, draw: (Canvas) -> Unit): IntArray {
  val bitmap = Bitmap.createBitmap(width.coerceAtLeast(1), height.coerceAtLeast(1), Bitmap.Config.ARGB_8888)
  try {
    draw(Canvas(bitmap))
    return IntArray(bitmap.width * bitmap.height).also { bitmap.getPixels(it, 0, bitmap.width, 0, 0, bitmap.width, bitmap.height) }
  } finally { bitmap.recycle() }
}

/** Renders through the GPU pipeline, where a hardware canvas replays recorded parts. */
private fun hardwareRaster(width: Int, height: Int, draw: (Canvas) -> Unit): IntArray {
  val reader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 1,
    HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE or HardwareBuffer.USAGE_GPU_COLOR_OUTPUT)
  val renderer = HardwareRenderer()
  try {
    val root = RenderNode("root").apply { setPosition(0, 0, width, height) }
    val canvas = root.beginRecording()
    check(canvas.isHardwareAccelerated)
    canvas.drawColor(Color.WHITE)
    draw(canvas)
    root.endRecording()
    renderer.setSurface(reader.surface)
    renderer.setContentRoot(root)
    renderer.createRenderRequest().setWaitForPresent(true).syncAndDraw()
    val image = reader.acquireNextImage()
    try {
      val buffer = image.hardwareBuffer ?: error("no hardware buffer")
      val wrapped = Bitmap.wrapHardwareBuffer(buffer, ColorSpace.get(ColorSpace.Named.SRGB)) ?: error("no bitmap")
      val copy = wrapped.copy(Bitmap.Config.ARGB_8888, false)
      buffer.close()
      return IntArray(width * height).also { copy.getPixels(it, 0, width, 0, 0, width, height); copy.recycle() }
    } finally { image.close() }
  } finally {
    renderer.destroy()
    reader.close()
  }
}

/**
 * Every line's geometry against the canonical layout of the whole text, and every
 * position and hit test against the whole text laid out plain: a layout over
 * precomputed text snaps an offset inside a word to the word's end, where plain
 * text, which chunks are, answers the character's own position.
 */
private fun assertSameLayout(label: String, actual: Layout, expected: Layout,
  positions: Layout = RenderedDocument.buildLayout(SpannableStringBuilder(expected.text), TextPaint(expected.paint),
    expected.width, Layout.Alignment.ALIGN_NORMAL)) {
  check(actual.lineCount == expected.lineCount) { "$label lines ${actual.lineCount}/${expected.lineCount}" }
  check(actual.height == expected.height) { "$label height ${actual.height}/${expected.height}" }
  for (line in 0 until expected.lineCount) {
    check(actual.getLineTop(line) == expected.getLineTop(line)) { "$label top $line" }
    check(actual.getLineStart(line) == expected.getLineStart(line)) { "$label start $line" }
    check(actual.getLineEnd(line) == expected.getLineEnd(line)) { "$label end $line" }
    check(actual.getLineBaseline(line) == expected.getLineBaseline(line)) { "$label baseline $line" }
    check(actual.getLineWidth(line) == expected.getLineWidth(line)) { "$label width $line" }
    check(actual.getParagraphDirection(line) == expected.getParagraphDirection(line)) { "$label direction $line" }
    check(actual.getLineContainsTab(line) == expected.getLineContainsTab(line)) { "$label tab $line" }
    val x = positions.getLineWidth(line) * 0.37f
    check(actual.getOffsetForHorizontal(line, x) == positions.getOffsetForHorizontal(line, x)) { "$label hit $line" }
  }
  val length = expected.text.length
  for (offset in (0..length step maxOf(1, length / 97)) + length) {
    check(actual.getLineForOffset(offset) == positions.getLineForOffset(offset)) { "$label line for $offset" }
    check(actual.getPrimaryHorizontal(offset) == positions.getPrimaryHorizontal(offset)) { "$label x for $offset" }
  }
  fun bounds(layout: Layout, start: Int, end: Int) = RectF().also { bounds ->
    Path().apply { layout.getSelectionPath(start, end, this) }.computeBounds(bounds, true)
  }
  // A range inside one line, and one over several lines inside the first chunk,
  // select exactly as the whole layout does.
  for (line in 0 until expected.lineCount step maxOf(1, expected.lineCount / 13)) {
    val start = positions.getLineStart(line)
    val end = expected.getLineVisibleEnd(line)
    if (end - start < 2) continue
    check(bounds(actual, start + 1, end - 1) == bounds(positions, start + 1, end - 1)) { "$label line selection $line" }
  }
  val firstChunkEnd = expected.getLineStart(minOf(expected.lineCount - 1, ChunkedTextLayout.CHUNK_LINES - 2))
  if (firstChunkEnd > 2) check(bounds(actual, 1, firstChunkEnd) == bounds(positions, 1, firstChunkEnd)) { "$label chunk selection" }
  // Across a seam each part closes its own last line at its ink rather than at the
  // layout's width, so only the vertical extent is the whole layout's.
  if (length > 2) {
    val across = bounds(actual, 1, length - 1)
    val whole = bounds(positions, 1, length - 1)
    check(across.top == whole.top && across.bottom == whole.bottom && across.left == whole.left) {
      "$label selection $across/$whole"
    }
  }
}

/**
 * The two layouts' pixels over the top, the middle and the bottom of the body,
 * where the first, an inner and the last chunk boundaries fall. Bounded windows,
 * because an unwrapped body can be thousands of pixels in both directions.
 */
private fun samePixels(actual: Layout, expected: Layout, contentWidth: Int): Boolean {
  val width = minOf(contentWidth + 16, 1400)
  val window = minOf(expected.height, 900)
  for (top in setOf(0, (expected.height - window) / 2, expected.height - window)) {
    val drawn = raster(width, window) { it.translate(0f, -top.toFloat()); actual.draw(it) }
    val reference = raster(width, window) { it.translate(0f, -top.toFloat()); expected.draw(it) }
    if (!drawn.contentEquals(reference)) return false
  }
  return true
}

/**
 * Streams a highlighted block in random deltas and holds every snapshot's chunked
 * body to the whole text laid out at once: geometry, hit tests and pixels, at an
 * unwrapped and a wrapped width.
 */
private fun checkStreamedBodies(paint: TextPaint): JSONObject {
  val source = codeSource(150)
  val random = Random(7)
  var end = 0
  var previous: ChunkedTextLayout? = null
  var snapshots = 0
  var pixelChecks = 0
  var reused = 0
  var laidOut = 0L
  var total = 0L
  while (end < source.length) {
    end = minOf(source.length, end + 1 + random.nextInt(12))
    val text = highlighted(source, end)
    val body = ChunkedTextLayout(text, codeLineStarts(text), paint, previous)
    for (width in intArrayOf(UNBOUNDED_CODE_WIDTH, 640)) {
      val frame = body.frame(width) ?: error("no frame at $end")
      val expected = canonicalLayout(text, paint, width)
      assertSameLayout("end=$end width=$width", frame.layout, expected)
      check(frame.height == expected.height)
      var widest = 0f
      for (line in 0 until expected.lineCount) widest = maxOf(widest, expected.getLineWidth(line))
      check(frame.contentWidth == ceil(widest.toDouble()).coerceAtLeast(1.0).toInt()) { "content width at $end" }
      if (snapshots % 9 == 0 || end == source.length) {
        check(samePixels(frame.layout, expected, if (width == UNBOUNDED_CODE_WIDTH) frame.contentWidth else width)) {
          "pixels end=$end width=$width"
        }
        pixelChecks++
      }
      if (width == UNBOUNDED_CODE_WIDTH && end > source.length / 2) {
        reused += frame.reusedChunks
        laidOut += frame.laidOutCharacters
        total += text.length
      }
    }
    previous = body
    snapshots++
  }
  // Late in the stream every snapshot reuses all of its settled chunks.
  check(reused > 0)
  val share = laidOut.toDouble() / total
  check(share < 0.25) { "chunks laid out $share of the streamed text" }
  return JSONObject().put("snapshots", snapshots).put("pixelChecks", pixelChecks)
    .put("reusedChunks", reused).put("laidOutShare", share)
}

/** A chunk is reused only under equal text and equal span values. */
private fun checkReuseRules(paint: TextPaint): JSONObject {
  val source = codeSource(80)
  val settled = highlighted(source, source.length)
  val seed = ChunkedTextLayout(settled, codeLineStarts(settled), paint)
  val seedFrame = seed.frame(UNBOUNDED_CODE_WIDTH)!!
  val chunks = seedFrame.settled.size
  check(chunks >= (codeLineStarts(settled).size - 1) / ChunkedTextLayout.CHUNK_LINES)

  // The same snapshot again, with new span objects: everything settled is reused.
  val again = highlighted(source, source.length)
  val same = ChunkedTextLayout(again, codeLineStarts(again), paint, seed).frame(UNBOUNDED_CODE_WIDTH)!!
  check(same.reusedChunks == chunks)

  // One recolored token in the second chunk rebuilds that chunk only.
  val recolored = highlighted(source, source.length)
  val secondChunkLine = codeLineStarts(recolored)[ChunkedTextLayout.CHUNK_LINES + 1]
  val token = recolored.getSpans(secondChunkLine, secondChunkLine + 60, ForegroundColorSpan::class.java)
    .first { recolored.getSpanStart(it) >= secondChunkLine }
  val start = recolored.getSpanStart(token)
  val tokenEnd = recolored.getSpanEnd(token)
  recolored.removeSpan(token)
  recolored.setSpan(ForegroundColorSpan(Color.MAGENTA), start, tokenEnd, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
  val recoloredFrame = ChunkedTextLayout(recolored, codeLineStarts(recolored), paint, seed).frame(UNBOUNDED_CODE_WIDTH)!!
  check(recoloredFrame.reusedChunks == chunks - 1) { "recolor reused ${recoloredFrame.reusedChunks}/$chunks" }
  val expected = canonicalLayout(recolored, paint, UNBOUNDED_CODE_WIDTH)
  assertSameLayout("recolored", recoloredFrame.layout, expected)
  check(samePixels(recoloredFrame.layout, expected, recoloredFrame.contentWidth)) { "recolored pixels" }

  // A changed character in the first chunk rebuilds it; the rest still match.
  val edited = highlighted(source.replaceFirst("value0", "valueX"), source.length)
  val editedFrame = ChunkedTextLayout(edited, codeLineStarts(edited), paint, seed).frame(UNBOUNDED_CODE_WIDTH)!!
  check(editedFrame.reusedChunks == chunks - 1)

  // A span with no comparable value is never reused.
  val opaque = highlighted(source, source.length).apply { setSpan(OpaqueSpan(), 0, 4, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE) }
  val opaqueSeed = ChunkedTextLayout(opaque, codeLineStarts(opaque), paint)
  opaqueSeed.frame(UNBOUNDED_CODE_WIDTH)!!
  val opaqueAgain = highlighted(source, source.length).apply { setSpan(OpaqueSpan(), 0, 4, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE) }
  check(ChunkedTextLayout(opaqueAgain, codeLineStarts(opaqueAgain), paint, opaqueSeed)
    .frame(UNBOUNDED_CODE_WIDTH)!!.reusedChunks == chunks - 1)

  // Another paint or another width shares nothing.
  val larger = TextPaint(paint).apply { textSize = paint.textSize + 2 }
  check(ChunkedTextLayout(again, codeLineStarts(again), larger, seed).frame(UNBOUNDED_CODE_WIDTH)!!.reusedChunks == 0)
  check(ChunkedTextLayout(again, codeLineStarts(again), paint, seed).frame(640)!!.reusedChunks == 0)

  // A right-to-left line leaves the body to the canonical TextView.
  val hebrew = highlighted("const a = 1\n\u05e9\u05dc\u05d5\u05dd world\nreturn a\n", 30)
  check(ChunkedTextLayout(hebrew, codeLineStarts(hebrew), paint).frame(UNBOUNDED_CODE_WIDTH) == null)

  // An empty body and a trailing newline lay out as the whole text does.
  for (sample in listOf("", "one line", "a\n", "a\nb\n\n")) {
    val text = highlighted(sample, sample.length)
    val frame = ChunkedTextLayout(text, codeLineStarts(text), paint).frame(UNBOUNDED_CODE_WIDTH)!!
    assertSameLayout("sample '$sample'", frame.layout,
      canonicalLayout(text, paint, UNBOUNDED_CODE_WIDTH))
  }
  return JSONObject().put("settledChunks", chunks).put("recolorRebuilt", 1).put("editRebuilt", 1)
}

/** Settled chunks replay their recording on a hardware canvas, pixel for pixel. */
private fun checkHardwareRecording(paint: TextPaint): JSONObject {
  val source = codeSource(60)
  val text = highlighted(source, source.length)
  val frame = ChunkedTextLayout(text, codeLineStarts(text), paint).frame(UNBOUNDED_CODE_WIDTH)!!
  val expected = canonicalLayout(text, paint, UNBOUNDED_CODE_WIDTH)
  val width = minOf(frame.contentWidth + 16, 1400)
  val height = minOf(expected.height, 2400)
  check(frame.settled.none { it.recording!!.isRecorded })
  val reference = hardwareRaster(width, height) { expected.draw(it) }
  val first = hardwareRaster(width, height) { frame.layout.draw(it) }
  // Destroying the renderer released every display list in its tree, as a trim
  // does in an app; the next hardware draw records the parts again.
  check(frame.settled.none { it.recording!!.isRecorded })
  val replayed = hardwareRaster(width, height) { canvas ->
    frame.layout.draw(canvas)
    check(frame.settled.all { it.recording!!.isRecorded })
  }
  check(first.contentEquals(reference)) { "recorded chunks differ from the whole layout" }
  check(replayed.contentEquals(reference)) { "replayed chunks differ from the whole layout" }
  // A software canvas never records.
  val fresh = ChunkedTextLayout(text, codeLineStarts(text), paint).frame(UNBOUNDED_CODE_WIDTH)!!
  raster(width, height) { fresh.layout.draw(it) }
  check(fresh.settled.none { it.recording!!.isRecorded })
  return JSONObject().put("recordedChunks", frame.settled.size).put("pixels", width * height)
}

/** The body view measures and draws a prepared layout without the platform's. */
private fun checkPreparedView(instrumentation: Instrumentation, paint: TextPaint): JSONObject {
  // Short lines, so the whole body rasterizes at once.
  val source = codeSource(40).lines().joinToString("\n") { it.take(60) }
  val text = highlighted(source, source.length)
  val frame = ChunkedTextLayout(text, codeLineStarts(text), paint).frame(UNBOUNDED_CODE_WIDTH)!!
  val rendered = RenderedDocument(paint)
  var pixelsMatch = false
  instrumentation.runOnMainSync {
    val prepared = TextSegmentView(instrumentation.targetContext)
    prepared.setPreparedContent(text, listOf(AccessibleBlock(0, text.length, false, 0)), rendered, frame.layout, frame.contentWidth)
    prepared.measure(View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED),
      View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED))
    check(prepared.measuredWidth == frame.contentWidth && prepared.measuredHeight == frame.height)
    check(prepared.text.isEmpty() && prepared.layout?.text?.isEmpty() != false)
    prepared.layout(0, 0, frame.contentWidth, frame.height)
    val canonical = TextSegmentView(instrumentation.targetContext)
    canonical.setContent(PrecomputedText.create(text, PrecomputedText.Params.Builder(paint)
      .setBreakStrategy(Layout.BREAK_STRATEGY_HIGH_QUALITY).setHyphenationFrequency(Layout.HYPHENATION_FREQUENCY_NONE)
      .setTextDirection(TextDirectionHeuristics.FIRSTSTRONG_LTR).build()),
      listOf(AccessibleBlock(0, text.length, false, 0)), rendered)
    canonical.measure(View.MeasureSpec.makeMeasureSpec(frame.contentWidth, View.MeasureSpec.EXACTLY),
      View.MeasureSpec.makeMeasureSpec(frame.height, View.MeasureSpec.EXACTLY))
    canonical.layout(0, 0, frame.contentWidth, frame.height)
    check(canonical.layout.height == frame.height) { "canonical ${canonical.layout.height}/${frame.height}" }
    pixelsMatch = raster(frame.contentWidth, frame.height) { prepared.draw(it) }
      .contentEquals(raster(frame.contentWidth, frame.height) { canonical.draw(it) })
  }
  check(pixelsMatch) { "prepared body pixels differ from the canonical TextView" }
  return JSONObject().put("lines", frame.layout.lineCount)
}

/** The whole text as the canonical path lays it out: precomputed, then laid out. */
private fun canonicalLayout(text: CharSequence, paint: TextPaint, width: Int): Layout =
  RenderedDocument.buildLayout(PrecomputedText.create(text, PrecomputedText.Params.Builder(paint)
    .setBreakStrategy(Layout.BREAK_STRATEGY_HIGH_QUALITY).setHyphenationFrequency(Layout.HYPHENATION_FREQUENCY_NONE)
    .setTextDirection(TextDirectionHeuristics.FIRSTSTRONG_LTR).build()), paint, width, Layout.Alignment.ALIGN_NORMAL)

private fun median(values: LongArray): Double {
  val sorted = values.sorted()
  return if (sorted.isEmpty()) 0.0 else sorted[sorted.size / 2] / 1000.0
}

/**
 * Per-snapshot cost of a streaming code body, before and after: the measuring
 * thread's layout, then the UI thread's assignment, measurement, layout and
 * display-list recording. Emulator timings: relative, not a phone's frame time.
 */
private fun benchmarkStreaming(instrumentation: Instrumentation, paint: TextPaint): JSONObject {
  val source = codeSource(200)
  val ends = ArrayList<Int>()
  val random = Random(11)
  var end = 0
  while (end < source.length) {
    end = minOf(source.length, end + 4 + random.nextInt(10))
    ends.add(end)
  }
  val rendered = RenderedDocument(paint)
  val params = PrecomputedText.Params.Builder(paint).setBreakStrategy(Layout.BREAK_STRATEGY_HIGH_QUALITY)
    .setHyphenationFrequency(Layout.HYPHENATION_FREQUENCY_NONE).setTextDirection(TextDirectionHeuristics.FIRSTSTRONG_LTR).build()
  val result = JSONObject().put("snapshots", ends.size).put("finalCharacters", source.length)
  val blocks = listOf(AccessibleBlock(0, 0, false, 0))
  for (round in 0 until 2) {
    // Canonical: the whole block precomputed and laid out on the measuring thread,
    // then assigned, measured and drawn by a TextView on the UI thread. One
    // snapshot at a time, so only the view's own text is retained.
    val canonicalMeasure = LongArray(ends.size)
    val canonicalUi = LongArray(ends.size)
    val canonicalView = arrayOfNulls<TextSegmentView>(1)
    val node = RenderNode("body")
    instrumentation.runOnMainSync { canonicalView[0] = TextSegmentView(instrumentation.targetContext) }
    for ((index, end) in ends.withIndex()) {
      val text = highlighted(source, end)
      val started = SystemClock.elapsedRealtimeNanos()
      val precomputed = PrecomputedText.create(text, params)
      val layout = RenderedDocument.buildLayout(precomputed, paint, UNBOUNDED_CODE_WIDTH, Layout.Alignment.ALIGN_NORMAL)
      var widest = 0f
      for (line in 0 until layout.lineCount) widest = maxOf(widest, layout.getLineWidth(line))
      val width = ceil(widest.toDouble()).toInt().coerceAtLeast(1)
      canonicalMeasure[index] = SystemClock.elapsedRealtimeNanos() - started
      instrumentation.runOnMainSync {
        val view = canonicalView[0]!!
        val uiStarted = SystemClock.elapsedRealtimeNanos()
        view.setContent(precomputed, blocks, rendered)
        // A code card's scroll view measures its body unconstrained first.
        view.measure(View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED),
          View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED))
        view.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
          View.MeasureSpec.makeMeasureSpec(layout.height, View.MeasureSpec.EXACTLY))
        view.layout(0, 0, view.measuredWidth, view.measuredHeight)
        node.setPosition(0, 0, view.measuredWidth, view.measuredHeight)
        val canvas = node.beginRecording()
        view.draw(canvas)
        node.endRecording()
        canonicalUi[index] = SystemClock.elapsedRealtimeNanos() - uiStarted
      }
    }
    // Chunked: the body laid out on the measuring thread from the previous
    // snapshot's chunks, then shown by the same view with no platform layout.
    val chunkedMeasure = LongArray(ends.size)
    val chunkedUi = LongArray(ends.size)
    val chunkedView = arrayOfNulls<TextSegmentView>(1)
    instrumentation.runOnMainSync { chunkedView[0] = TextSegmentView(instrumentation.targetContext) }
    var previous: ChunkedTextLayout? = null
    for ((index, end) in ends.withIndex()) {
      val text = highlighted(source, end)
      val started = SystemClock.elapsedRealtimeNanos()
      val body = ChunkedTextLayout(text, codeLineStarts(text), paint, previous)
      val frame = body.frame(UNBOUNDED_CODE_WIDTH)!!
      chunkedMeasure[index] = SystemClock.elapsedRealtimeNanos() - started
      previous = body
      instrumentation.runOnMainSync {
        val view = chunkedView[0]!!
        val uiStarted = SystemClock.elapsedRealtimeNanos()
        view.setPreparedContent(frame.text, blocks, rendered, frame.layout, frame.contentWidth)
        view.measure(View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED),
          View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED))
        view.measure(View.MeasureSpec.makeMeasureSpec(frame.contentWidth, View.MeasureSpec.EXACTLY),
          View.MeasureSpec.makeMeasureSpec(frame.height, View.MeasureSpec.EXACTLY))
        view.layout(0, 0, view.measuredWidth, view.measuredHeight)
        node.setPosition(0, 0, view.measuredWidth, view.measuredHeight)
        val canvas = node.beginRecording()
        view.draw(canvas)
        node.endRecording()
        chunkedUi[index] = SystemClock.elapsedRealtimeNanos() - uiStarted
      }
    }
    if (round == 0) continue // warm-up
    val last = ends.size * 3 / 4 until ends.size
    fun tail(values: LongArray) = LongArray(last.count()) { values[last.first + it] }
    for ((name, measure, ui) in listOf(
      Triple("canonical", canonicalMeasure, canonicalUi),
      Triple("chunked", chunkedMeasure, chunkedUi),
    )) {
      result.put(name, JSONObject()
        .put("measureMedianUs", median(measure)).put("uiMedianUs", median(ui))
        .put("measureLastQuarterMedianUs", median(tail(measure))).put("uiLastQuarterMedianUs", median(tail(ui)))
        .put("uiP95Us", LongArray(ui.size) { ui[it] }.sorted().let { it[it.size * 95 / 100] / 1000.0 }))
    }
  }
  return result
}

internal fun runCodeBodyChecks(instrumentation: Instrumentation): JSONObject {
  val paint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply { textSize = 28f; color = Color.BLACK; typeface = Typeface.MONOSPACE }
  val streamed = checkStreamedBodies(paint)
  android.util.Log.i("CodeBodyChecks", "streamed checks passed")
  val reuse = checkReuseRules(paint)
  android.util.Log.i("CodeBodyChecks", "reuse checks passed")
  val hardware = checkHardwareRecording(paint)
  android.util.Log.i("CodeBodyChecks", "hardware checks passed")
  val view = checkPreparedView(instrumentation, paint)
  android.util.Log.i("CodeBodyChecks", "view checks passed")
  val benchmark = benchmarkStreaming(instrumentation, paint)
  return JSONObject().put("streamed", streamed).put("reuse", reuse).put("hardware", hardware)
    .put("preparedView", view).put("benchmark", benchmark)
}

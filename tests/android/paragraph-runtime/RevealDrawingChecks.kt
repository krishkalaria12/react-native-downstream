package com.downstream

import android.app.Instrumentation
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RenderNode
import android.os.Debug
import android.text.Layout
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.TextPaint
import android.text.style.ReplacementSpan
import android.util.Log
import android.view.View
import android.widget.FrameLayout
import org.json.JSONObject

/** Counts source text drawing, including when a bucket clips the marker away. */
private class DrawMarker : ReplacementSpan() {
  var draws = 0
  override fun getSize(paint: Paint, text: CharSequence, start: Int, end: Int, fm: Paint.FontMetricsInt?) = 18
  override fun draw(canvas: Canvas, text: CharSequence, start: Int, end: Int, x: Float,
    top: Int, y: Int, bottom: Int, paint: Paint) {
    draws++
    canvas.drawRect(x, y - 16f, x + 14f, y.toFloat(), paint)
  }
}

private fun softwareRevealRaster(width: Int, height: Int, draw: (Canvas) -> Unit): IntArray {
  val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
  try {
    val canvas = Canvas(bitmap)
    canvas.drawColor(Color.WHITE)
    draw(canvas)
    return IntArray(width * height).also { bitmap.getPixels(it, 0, width, 0, 0, width, height) }
  } finally { bitmap.recycle() }
}

/** The former production algorithm, retained as a same-backend pixel oracle. */
private fun drawLegacyReveal(canvas: Canvas, view: TextSegmentView, band: RevealBand,
  clips: RevealClipPaths = RevealClipPaths()) {
  val mask = clips.build(view.shownLayout!!, band, view.totalPaddingLeft.toFloat(), view.totalPaddingTop.toFloat())
  val outside = canvas.save()
  canvas.clipOutPath(clips.hidden)
  view.draw(canvas)
  canvas.restoreToCount(outside)
  for (bucket in 1..MAX_ALPHA_BUCKETS) {
    if (mask and (1 shl bucket) == 0) continue
    val path = clips.buckets[bucket - 1]
    path.computeBounds(clips.bounds, true)
    if (clips.bounds.isEmpty) continue
    val layer = canvas.saveLayerAlpha(clips.bounds, (bucketAlpha(bucket) * 255f).toInt())
    canvas.clipPath(path)
    canvas.translate(0f, clips.rises[bucket - 1])
    view.draw(canvas)
    canvas.restoreToCount(layer)
  }
}

private fun assertRevealPixels(label: String, actual: IntArray, expected: IntArray) {
  var differences = 0
  var first = -1
  for (index in actual.indices) if (actual[index] != expected[index]) {
    differences++
    if (first < 0) first = index
  }
  check(differences == 0) { "$label differs at $differences pixels, first=$first actual=${actual.getOrNull(first)} expected=${expected.getOrNull(first)}" }
}

/** Return main-thread assertions through the instrumentation result bundle. */
private fun runRevealOnMain(instrumentation: Instrumentation, action: () -> Unit) {
  var failure: Throwable? = null
  instrumentation.runOnMainSync {
    try { action() } catch (error: Throwable) { failure = error }
  }
  failure?.let { throw it }
}

/** Real production views, software fallback, GPU replay and content replacement. */
internal fun runRevealDrawingChecks(instrumentation: Instrumentation): JSONObject {
  var pixelChecks = 0
  var recordedDraws = -1
  var legacyDraws = -1
  var softwareDraws = -1
  var animatedFrameCpuMs = 0.0
  var legacyFrameCpuMs = 0.0
  runRevealOnMain(instrumentation) {
    val width = 620
    val height = 150
    val paint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply { textSize = 26f; color = Color.BLACK }
    val rendered = RenderedDocument(paint)
    for (mode in listOf("prepared", "ordinary", "chunked")) {
      val prepared = mode != "ordinary"
      val host = DownstreamMarkdownView(instrumentation.targetContext)
      val actual = TextSegmentView(instrumentation.targetContext).apply {
        layoutParams = FrameLayout.LayoutParams(width, FrameLayout.LayoutParams.WRAP_CONTENT)
      }
      val reference = TextSegmentView(instrumentation.targetContext).apply {
        layoutParams = FrameLayout.LayoutParams(width, FrameLayout.LayoutParams.WRAP_CONTENT)
      }
      host.addView(actual)
      val marker = DrawMarker()
      // The same view receives changed text and color, so stale recorded text is visible.
      for (version in 0..1) {
        val source = (0 until 40).joinToString("\n") { "X alpha beta gamma delta row $it version $version" }
        val text = SpannableStringBuilder(source)
        for (start in source.indices) if (source[start] == 'X') {
          // Each span instance can only cover one range in a Spannable.
          text.setSpan(if (start == 0) marker else DrawMarker(), start, start + 1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        paint.color = if (version == 0) Color.BLACK else 0xff1849b0.toInt()
        val blocks = listOf(AccessibleBlock(0, text.length, false, 0))
        for (view in listOf(actual, reference)) {
          view.setPadding(13, 11, 17, 9)
          if (prepared) {
            val layout = if (mode == "chunked" && view === actual) {
              ChunkedTextLayout(text, codeLineStarts(text), paint).frame(width - 30)!!.layout
            } else RenderedDocument.buildLayout(text, paint, width - 30, Layout.Alignment.ALIGN_NORMAL)
            view.setPreparedContent(text, blocks, rendered, layout, width - 30)
          } else view.setContent(text, blocks, rendered)
          view.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED))
          view.layout(0, 0, width, view.measuredHeight)
        }
        if (mode == "chunked") check(actual.shownLayout is CompositeLayout) { "expected nested recorded chunks" }
        check(actual.height > height * 3) { "expected a tall segment" }
        for (top in listOf(0, 450)) {
          val line = reference.shownLayout!!.getLineForVertical(top)
          val start = reference.shownLayout!!.getLineStart(line)
          val ranges = longArrayOf(0, (start + 2).toLong(), (start + 7).toLong(),
            0, (start + 8).toLong(), (start + 12).toLong(),
            0, (start + 13).toLong(), (start + 18).toLong(),
            0, (start + 19).toLong(), (start + 24).toLong())
          val frame = RevealFrame(doubleArrayOf(), 2f, true)
          frame.build(ranges, DoubleArray(4), 50.0)
          val band = RevealBand(4)
          band.count = 4
          for (index in 0..3) {
            val alpha = floatArrayOf(0.2f, 0.45f, 0.7f, 0f)[index]
            val rise = (1f - alpha) * 8f
            frame.unitAt(index).alpha = alpha
            frame.unitAt(index).rise = rise
            band.offsets[index * 2] = ranges[index * 3 + 1].toInt()
            band.offsets[index * 2 + 1] = ranges[index * 3 + 2].toInt()
            band.alphas[index] = alpha
            band.rises[index] = rise
          }
          for (inset in listOf(0, 18)) {
            fun viewport(canvas: Canvas, draw: () -> Unit) {
              canvas.clipRect(inset, inset, width - inset, height - inset)
              canvas.translate(0f, -top.toFloat())
              draw()
            }
            val label = "mode=$mode version=$version top=$top inset=$inset"
            for (hardware in listOf(true, false)) {
              fun raster(draw: (Canvas) -> Unit): IntArray =
                if (hardware) hardwareRaster(width, height, draw) else softwareRevealRaster(width, height, draw)
              host.frame = frame
              marker.draws = 0
              val drawn = raster { canvas -> viewport(canvas) { actual.draw(canvas) } }
              val draws = marker.draws
              host.frame = null
              marker.draws = 0
              val expected = raster { canvas -> viewport(canvas) { drawLegacyReveal(canvas, reference, band) } }
              val repeated = marker.draws
              assertRevealPixels("$label hardware=$hardware", drawn, expected)
              pixelChecks++
              if (mode == "prepared" && version == 0 && top == 0 && inset == 0) {
                if (hardware) {
                  recordedDraws = draws
                  legacyDraws = repeated
                  host.frame = frame
                  val nodes = Array(2) { RenderNode("animated-frame-benchmark-$it").apply { setPosition(0, 0, width, height) } }
                  val legacyClips = RevealClipPaths()
                  val starts = DoubleArray(4)
                  fun record(optimized: Boolean) {
                    frame.build(ranges, starts, 50.0)
                    for (index in 0..3) {
                      frame.unitAt(index).alpha = band.alphas[index]
                      frame.unitAt(index).rise = band.rises[index]
                    }
                    val node = nodes[if (optimized) 0 else 1]
                    val canvas = node.beginRecording()
                    canvas.clipRect(0, 0, width, height)
                    if (optimized) actual.draw(canvas) else drawLegacyReveal(canvas, reference, band, legacyClips)
                    node.endRecording()
                  }
                  repeat(120) { record(true); record(false) }
                  val optimizedSamples = ArrayList<Double>()
                  val legacySamples = ArrayList<Double>()
                  repeat(5) {
                    for (optimized in listOf(true, false, false, true)) {
                      val began = Debug.threadCpuTimeNanos()
                      repeat(80) { record(optimized) }
                      val elapsed = (Debug.threadCpuTimeNanos() - began) / 1_000_000.0
                      (if (optimized) optimizedSamples else legacySamples).add(elapsed)
                    }
                  }
                  fun median(values: List<Double>): Double = values.sorted().let { (it[4] + it[5]) / 2.0 }
                  animatedFrameCpuMs = median(optimizedSamples)
                  legacyFrameCpuMs = median(legacySamples)
                  nodes.forEach { it.discardDisplayList() }
                  host.frame = null
                  Log.i("RevealDrawingChecks", "hardwareSourceDraws=$draws legacySourceDraws=$repeated animated80FrameCpuMs=$animatedFrameCpuMs legacy80FrameCpuMs=$legacyFrameCpuMs optimizedSamples=$optimizedSamples legacySamples=$legacySamples")
                  check(draws == 1) { "hardware source draws expected=1 actual=$draws legacy=$repeated animated80FrameCpuMs=$animatedFrameCpuMs legacy80FrameCpuMs=$legacyFrameCpuMs" }
                  check(repeated >= 4) { "legacy oracle did not exercise multiple buckets: $repeated" }
                } else {
                  softwareDraws = draws
                  check(draws == repeated && draws >= 4) { "software fallback source draws $draws/$repeated" }
                }
              }
            }
          }
          // Reusing the frame after completion must restore every formerly hidden word.
          frame.build(longArrayOf(), doubleArrayOf(), 200.0)
          host.frame = frame
          val completed = hardwareRaster(width, height) { it.translate(0f, -top.toFloat()); actual.draw(it) }
          host.frame = null
          val opaque = hardwareRaster(width, height) { it.translate(0f, -top.toFloat()); reference.draw(it) }
          assertRevealPixels("completed $mode/$version/$top", completed, opaque)
          pixelChecks++
        }
      }
    }
  }
  return JSONObject().put("pixelChecks", pixelChecks).put("hardwareSourceDraws", recordedDraws)
    .put("legacySourceDraws", legacyDraws).put("softwareSourceDraws", softwareDraws)
    .put("animated80FrameCpuMs", animatedFrameCpuMs)
    .put("legacy80FrameCpuMs", legacyFrameCpuMs)
}

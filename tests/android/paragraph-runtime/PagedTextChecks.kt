package com.downstream

import android.app.Instrumentation
import android.graphics.Bitmap
import android.graphics.Canvas
import android.text.GetChars
import android.text.SpannableStringBuilder
import android.text.SpannedString
import android.text.TextPaint
import android.view.View
import org.json.JSONObject
import java.lang.ref.WeakReference
import java.lang.ref.ReferenceQueue
import java.util.concurrent.Callable
import java.util.concurrent.Executors

internal object NativePages {
  init { System.loadLibrary("downstream_pages_test") }
  external fun create(text: String): Long
  external fun createBuilder(text: String): Long
  external fun append(id: Long, text: String): Long
  external fun read(id: Long): Array<String>
  external fun stats(): LongArray
  external fun clearCache()
  external fun reset()
}

private fun verifyPages(pages: Array<String>, expected: String): PagedText {
  val text = PagedText(pages)
  check(text.length == expected.length)
  for (at in expected.indices) check(text[at] == expected[at])
  for (start in listOf(0, 1022, 1023, 1024, 2047, expected.length)) {
    if (start > expected.length) continue
    val end = minOf(expected.length, start + 1050)
    val output = CharArray(end - start + 4) { '!' }
    text.getChars(start, end, output, 2)
    check(String(output, 2, end - start) == expected.substring(start, end))
    check(output.first() == '!' && output.last() == '!')
    val slice = text.subSequence(start, end)
    check(slice is GetChars && slice.toString() == expected.substring(start, end))
    check(slice.subSequence(0, slice.length).toString() == slice.toString())
  }
  check(SpannableStringBuilder(text).toString() == expected)
  check(text.toString() == expected)
  check(runCatching { text[-1] }.exceptionOrNull() is IndexOutOfBoundsException)
  check(runCatching { text[text.length] }.exceptionOrNull() is IndexOutOfBoundsException)
  check(runCatching { text.getChars(0, 1, CharArray(0), 0) }.isFailure)
  check(runCatching { text.getChars(0, 1, CharArray(3), Int.MAX_VALUE) }.isFailure)
  check(runCatching { text.subSequence(1, 0) }.isFailure)
  return text
}

private fun pixels(view: View): IntArray {
  val bitmap = Bitmap.createBitmap(view.width, view.height, Bitmap.Config.ARGB_8888)
  try {
    view.draw(Canvas(bitmap))
    return IntArray(view.width * view.height).also {
      bitmap.getPixels(it, 0, view.width, 0, 0, view.width, view.height)
    }
  } finally { bitmap.recycle() }
}

private fun codeModel(source: CharSequence, pages: CharSequence? = null): CodeSegment {
  val face = android.graphics.Typeface.MONOSPACE
  return CodeSegment(
    text = SpannedString(SpannableStringBuilder(source)), accessibleBlocks = emptyList(), language = "ts", title = "",
    fenceMeta = FenceMeta(), lineStarts = intArrayOf(0), codeTextSize = 16,
    codeFace = face, blockIndex = 0, codeOrdinal = 0, containers = emptyList(),
    contentLeading = 0, rtl = false, firstLine = "code", padding = 4,
    gutterGap = 4, colors = Palette(), radius = 0f, borderWidth = 0f,
    headerFace = face, headerSize = 16f, markerFace = face, markerSize = 16f,
    markerGap = 4f, caretOffset = -1, caretKind = 0, gapBefore = 0, sourcePages = pages,
  )
}

private fun checkModelPageOwnership(): Boolean {
  NativePages.clearCache()
  val id = NativePages.create("model source pages\n".repeat(200))
  val queue = ReferenceQueue<String>()
  val model = arrayOfNulls<CodeSegment>(1)
  fun own(): WeakReference<String> {
    val array = NativePages.read(id)
    val text = PagedText(array)
    model[0] = codeModel(text, text)
    return WeakReference(array[0], queue)
  }
  val weak = own()
  repeat(4) { System.gc(); Thread.sleep(10) }
  check(queue.poll() == null) { "live code model released its page source" }
  val before = NativePages.stats()[0]
  fun reread() {
    val pages = NativePages.read(id)
    check(pages[0] === weak.get()) { "live code model did not retain Java page identity" }
    check(model[0]!!.sourcePages is PagedText)
    check(model[0]!!.textLength == pages.sumOf { it.length })
  }
  reread()
  check(NativePages.stats()[0] == before) { "live model's unchanged pages crossed JNI again after GC" }
  model[0] = null
  var collected = false
  repeat(40) {
    System.gc(); Thread.sleep(10)
    if (queue.poll() === weak) collected = true
  }
  check(collected) { "released code model retained source pages" }
  NativePages.read(id)
  check(NativePages.stats()[0] > before)
  return true
}

internal fun runPagedTextChecks(instrumentation: Instrumentation): JSONObject {
  check(runCatching { PagedText(emptyArray()) }.isFailure)
  check(runCatching { PagedText(arrayOf("short", "last")) }.isFailure)
  NativePages.reset()
  val builderId = NativePages.createBuilder("mutable builder " + "x".repeat(3000))
  val builderFirst = NativePages.read(builderId)
  val builderSecond = NativePages.read(builderId)
  check(builderFirst[0] !== builderSecond[0])
  check(NativePages.stats()[1] == 0L && NativePages.stats()[2] == 0L)
  NativePages.reset()
  // A surrogate pair straddles a page; retain every snapshot to prove immutability.
  var expected = "x".repeat(1023) + "🙂 café مرحبا\n" + "code \t".repeat(500)
  var id = NativePages.create(expected)
  val first = NativePages.read(id)
  verifyPages(first, expected)
  check(NativePages.stats()[0] == expected.length.toLong())
  val second = NativePages.read(id)
  check(first.indices.all { first[it] === second[it] })
  val retained = mutableListOf(first)
  val totalBaselineUnits = mutableListOf(expected.length.toLong())
  repeat(256) {
    val previousLength = expected.length
    expected += "δ🙂 append $it\n"
    id = NativePages.append(id, "δ🙂 append $it\n")
    val pages = NativePages.read(id)
    val previous = retained.last()
    check((0 until previousLength / PagedText.PAGE_UNITS).all { pages[it] === previous[it] })
    verifyPages(pages, expected)
    retained += pages
    totalBaselineUnits += expected.length.toLong()
  }
  verifyPages(first, "x".repeat(1023) + "🙂 café مرحبا\n" + "code \t".repeat(500))
  val stats = NativePages.stats()
  check(stats[3] == 0L) { "JNI export flattened native SnapshotText" }
  check(stats[0] < totalBaselineUnits.sum() / 4) { "page transfer did not reuse prefix: ${stats.toList()}" }
  val pool = Executors.newFixedThreadPool(4)
  try {
    pool.invokeAll(List(16) { Callable { verifyPages(NativePages.read(id), expected) } }).forEach { it.get() }
  } finally { pool.shutdown() }
  val replacement = "replacement 🧪 reset\n".repeat(200)
  val replaced = NativePages.create(replacement)
  verifyPages(NativePages.read(replaced), replacement)
  verifyPages(NativePages.read(id), expected)
  NativePages.clearCache()
  val afterClear = NativePages.read(id)
  check(afterClear[0] !== retained.last()[0])
  verifyPages(afterClear, expected)
  check(NativePages.stats()[0] == expected.length.toLong())

  // Once the Java snapshot is unreachable, weak cache hits must safely become misses.
  NativePages.clearCache()
  val gcSource = "gc weak page\n".repeat(200)
  val gcId = NativePages.create(gcSource)
  val queue = ReferenceQueue<String>()
  fun weakPage(): WeakReference<String> = WeakReference(NativePages.read(gcId)[0], queue)
  val weak = weakPage()
  var collected = false
  repeat(40) {
    System.gc()
    Thread.sleep(10)
    if (queue.poll() === weak) collected = true
  }
  check(collected) { "JNI page cache retained Java string after GC" }
  val beforeGcRead = NativePages.stats()[0]
  verifyPages(NativePages.read(gcId), gcSource)
  check(NativePages.stats()[0] > beforeGcRead)
  val modelRetainsPagesAcrossGc = checkModelPageOwnership()
  val largeId = NativePages.create("z".repeat(1024 * 2050))
  val large = NativePages.read(largeId)
  check(large.size == 2050 && NativePages.stats()[2] <= 2048L)

  var viewFailure: Throwable? = null
  instrumentation.runOnMainSync {
    try {
      val paint = TextPaint().apply { textSize = 20f; color = android.graphics.Color.BLACK }
      val document = RenderedDocument(paint)
      val text = verifyPages(NativePages.read(id), expected)
      val array = NativePages.read(id)
      val protected = PagedText(array)
      array[0] = "mutated caller array"
      check(protected[0] == expected[0])
      fun view(source: CharSequence): TextSegmentView {
        val segment = TextSegment(SpannedString(SpannableStringBuilder(source)),
          listOf(AccessibleBlock(0, source.length, false, 0)), gapBefore = 0, caretOffset = -1, caretKind = 0)
        return TextSegmentView(instrumentation.targetContext).apply {
          layoutParams = android.view.ViewGroup.LayoutParams(320, android.view.ViewGroup.LayoutParams.WRAP_CONTENT)
          setSegment(segment, document, 0)
          setSelectable(true)
          measure(View.MeasureSpec.makeMeasureSpec(320, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED))
          layout(0, 0, measuredWidth, measuredHeight)
        }
      }
      val paged = view(text)
      val canonical = view(expected)
      check(paged.measuredHeight == canonical.measuredHeight)
      check(pixels(paged).contentEquals(pixels(canonical))) { "canonical pixels differ for paged code text" }
      val node = checkNotNull(paged.accessibilityNodeProvider.createAccessibilityNodeInfo(0))
      check(node.text.toString() == expected)
      android.text.Selection.setSelection(paged.text as android.text.Spannable, 1010, 1050)
      check(paged.selectionStart == 1010 && paged.selectionEnd == 1050)
      check(pixels(paged).contentEquals(pixels(canonical.apply {
        android.text.Selection.setSelection(this.text as android.text.Spannable, 1010, 1050)
      })))
    } catch (failure: Throwable) { viewFailure = failure }
  }
  viewFailure?.let { throw it }
  NativePages.reset()
  verifyPages(NativePages.read(NativePages.create("reset is independent")), "reset is independent")
  return JSONObject().put("canonicalTransferredUtf16Units", totalBaselineUnits.sum())
    .put("pagedTransferredUtf16Units", stats[0]).put("reusedPages", stats[1])
    .put("nativeFlattenedBytes", stats[3]).put("retainedSnapshots", retained.size)
    .put("gcAndConcurrency", true).put("pixelsAccessibilitySelection", true)
    .put("modelRetainsPagesAcrossGc", modelRetainsPagesAcrossGc)
}

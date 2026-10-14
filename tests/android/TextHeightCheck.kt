package com.downstream

import android.text.StaticLayout
import android.text.TextPaint
import kotlin.random.Random

private fun details(id: Int) = ContainerDecoration(
  0f, 0f, null, false, null, kind = CONTAINER_DETAILS, id = id,
)

fun main() {
  val segment = TextSegment(
    SpannedText("x".repeat(60)), emptyList(),
    detailsBlocks = listOf(
      DetailsBlock(12, 30, false, listOf(details(1))),
      DetailsBlock(46, 60, false, listOf(details(2))),
    ),
    gapBefore = 0, caretOffset = -1, caretKind = 0,
  )
  check(HiddenRange(12, 30) == HiddenRange(12, 30)) { "equal ranges must hit the visible-height cache" }
  val paint = TextPaint()
  check(segment.height(100, paint) == 30)
  val topReads = StaticLayout.topReads
  val startReads = StaticLayout.startReads
  repeat(1000) {
    check(segment.height(100, paint, ReaderChoices()) == 30)
  }
  check(RenderedDocument.builds == 1) { "same choices must reuse the width layout" }
  check(StaticLayout.topReads == topReads && StaticLayout.startReads == startReads)
  val openFirst = ReaderChoices(detailsOpen = intArrayOf(1))
  val openBoth = ReaderChoices(detailsOpen = intArrayOf(1, 2))
  repeat(1000) {
    check(segment.height(100, paint, openFirst) == 50)
    check(segment.height(100, paint, openBoth) == 60)
    check(segment.height(100, paint) == 30)
  }
  check(RenderedDocument.builds == 1) { "details toggles must not shape text" }
  check(StaticLayout.topReads == topReads && StaticLayout.startReads == startReads)
  check(segment.height(50, paint) == 40)
  check(RenderedDocument.builds == 2) { "width changes must invalidate layout" }
  check(segment.height(50, paint, openBoth) == 80)
  check(RenderedDocument.builds == 2)
  check(segment.height(0, paint) == 40)
  check(segment.height(-10, paint) == 40)
  check(RenderedDocument.builds == 3) { "equivalent clamped widths must share layout" }

  val plain = TextSegment(
    SpannedText("plain"), emptyList(), gapBefore = 0, caretOffset = -1, caretKind = 0,
  )
  val beforePlainTops = StaticLayout.topReads
  val beforePlainStarts = StaticLayout.startReads
  check(plain.height(100, paint) == 60)
  check(plain.height(100, paint, openBoth) == 60)
  check(RenderedDocument.builds == 4)
  check(StaticLayout.topReads == beforePlainTops && StaticLayout.startReads == beforePlainStarts) {
    "plain text must not allocate or copy line geometry"
  }

  val streaming = StreamingParagraphLayout { width -> if (width >= 100) 91 else null }
  val live = TextSegment(
    SpannedText("growing plain"), emptyList(), gapBefore = 0, caretOffset = -1,
    caretKind = 0, streamingParagraph = streaming,
  )
  val buildsBeforeLive = RenderedDocument.builds
  check(live.height(100, paint) == 91)
  check(live.height(100, paint, openBoth) == 91)
  check(RenderedDocument.builds == buildsBeforeLive) { "eligible live layout bypasses canonical shaping" }
  check(live.height(50, paint) == 80)
  check(RenderedDocument.builds == buildsBeforeLive + 1) { "bounded live fallback uses canonical layout" }
  check(live.height(50, paint) == 80)
  check(RenderedDocument.builds == buildsBeforeLive + 1)

  // Compare arbitrary order, overlap, empty/reversed ranges, and unequal line
  // heights against the original per-line membership rule.
  val random = Random(420)
  repeat(2000) {
    val count = random.nextInt(1, 200)
    val starts = IntArray(count)
    val tops = IntArray(count + 1)
    for (line in 1 until count) starts[line] = starts[line - 1] + random.nextInt(0, 12)
    for (line in 1..count) tops[line] = tops[line - 1] + random.nextInt(1, 30)
    val hidden = List(random.nextInt(0, 60)) {
      HiddenRange(random.nextInt(-30, starts.last() + 31), random.nextInt(-30, starts.last() + 31))
    }
    val removed = starts.indices.sumOf { line ->
      if (hidden.any { starts[line] >= it.start && starts[line] < it.end }) tops[line + 1] - tops[line] else 0
    }
    check(visibleTextHeight(tops.last(), count, { tops[it] }, { starts[it] }, hidden) == tops.last() - removed)
  }
  var startProbes = 0
  var topProbes = 0
  val count = 100_000
  val ranges = List(1000) { HiddenRange(it * 90, it * 90 + 20) }.reversed()
  check(visibleTextHeight(count * 10, count,
    { topProbes++; it * 10 }, { startProbes++; it }, ranges) == 800_000)
  check(startProbes <= 34_000 && topProbes == 2000)
  var preparations = 0
  val rawCode = SpannedText("some code")
  val face = android.graphics.Typeface()
  val code = CodeSegment(
    text = rawCode, accessibleBlocks = emptyList(), language = "ts", title = "",
    fenceMeta = FenceMeta(), lineStarts = intArrayOf(0), codeTextSize = 16,
    codeFace = face, blockIndex = 0, codeOrdinal = 0, containers = emptyList(),
    contentLeading = 0, rtl = false, firstLine = "some code", padding = 4,
    gutterGap = 4, colors = Palette(), radius = 0f, borderWidth = 0f,
    headerFace = face, headerSize = 16f, markerFace = face, markerSize = 16f,
    markerGap = 4f, caretOffset = -1, caretKind = 0, gapBefore = 0,
    prepareText = { preparations++; SpannedText("some code") },
  )
  repeat(100) { check(code.textLength == 9) }
  check(preparations == 0) { "hosted code metadata/cache accounting must not shape fallback text" }
  val threads = List(8) { Thread { repeat(100) { check(code.text.length == 9) } } }
  threads.forEach { it.start() }
  threads.forEach { it.join() }
  check(preparations == 1) { "fallback must prepare once, including simultaneous worker/view access" }
  check(rawCode.length == 9)
  println("hosted code fallback: 100 metadata reads, 0 preparations; first fallback access, 1 preparation")

  // A chunked body answers a card's size itself, so neither the measurement nor
  // the view prepares the canonical text or lays the whole block out.
  var bodyPreparations = 0
  val chunked = ChunkedTextLayout { width -> if (width == 77) null else ChunkedTextFrame(height = 120, contentWidth = 340) }
  fun chunkedCode(body: ChunkedTextLayout?) = CodeSegment(
    text = rawCode, accessibleBlocks = emptyList(), language = "ts", title = "",
    fenceMeta = FenceMeta(), lineStarts = intArrayOf(0), codeTextSize = 16,
    codeFace = face, blockIndex = 0, codeOrdinal = 0, containers = emptyList(),
    contentLeading = 0, rtl = false, firstLine = "some code", padding = 4,
    gutterGap = 4, colors = Palette(), radius = 0f, borderWidth = 0f,
    headerFace = face, headerSize = 16f, markerFace = face, markerSize = 16f,
    markerGap = 4f, caretOffset = -1, caretKind = 0, gapBefore = 0,
    prepareText = { bodyPreparations++; SpannedText("some code") },
    body = body,
  )
  val withBody = chunkedCode(chunked)
  val builds = RenderedDocument.builds
  check(withBody.bodyHeight(paint, 500, wrapped = false) == 120)
  check(withBody.bodyWidth(paint, 500, wrapped = false) == 340)
  // Wrapped, the body lays out at the card's inner width less padding and gutter.
  check(withBody.bodyHeight(paint, 500, wrapped = true, gutterWidth = 20) == 120)
  check(withBody.bodyWidth(paint, 500, wrapped = true, gutterWidth = 20) == 500 - 2 * 4 - 20)
  check(chunked.widths.take(3) == listOf(UNBOUNDED_CODE_WIDTH, UNBOUNDED_CODE_WIDTH, 472)) { "${chunked.widths}" }
  check(bodyPreparations == 0 && RenderedDocument.builds == builds) {
    "a chunked body must not prepare or lay out the canonical text"
  }
  // A width the body cannot lay out (a right-to-left line) measures canonically.
  check(withBody.bodyHeight(paint, 77 + 2 * 4, wrapped = true) > 0)
  check(bodyPreparations == 1 && RenderedDocument.builds == builds + 1)
  println("chunked code body: height and width from the body frame, 0 canonical preparations; unsupported width falls back once")
  println("text height checks passed: 3000 toggles, 1 layout; 100000 lines × 1000 ranges: $startProbes start probes, $topProbes top probes")
}

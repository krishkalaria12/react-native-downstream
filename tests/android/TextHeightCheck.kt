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
  println("text height checks passed: 3000 toggles, 1 layout; 100000 lines × 1000 ranges: $startProbes start probes, $topProbes top probes")
}

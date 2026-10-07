package com.downstream

import kotlin.system.exitProcess

/**
 * The tables in `testDetailsLayout` and `testVisibleTextHeight` from
 * `src/cpp/tests/test_export.cpp`, run against the Kotlin mirror.
 *
 * `DetailsLayout.kt` is the platform's copy of `downstream/DetailsLayout.h`, so
 * both sides have to answer the same questions the same way. Running one table
 * against both is what proves they do, rather than a comment saying they should.
 */
private var failures = 0

private fun check(condition: Boolean, label: String) {
  if (!condition) {
    println("FAIL $label")
    failures++
  }
}

/** A frame of the kind a block carries, for the container-path cases. */
private fun details(id: Int, open: Boolean, summary: Boolean = false) =
  ContainerDecoration(0f, 10f, null, false, null, 0f, 0f, kind = NativeDocument.CONTAINER_DETAILS, id = id, open = open,
    summary = summary)

private fun quote() = ContainerDecoration(0f, 10f, null, false, null, 0f, 0f, kind = 0)

fun main() {
  val none = ReaderChoices()
  check(!resolveDetails(none, 1, false).open, "a block with neither answer is closed")
  check(resolveDetails(none, 1, true).open, "the writer's open is the starting state")

  val opened = none.with(ChoiceKind.DetailsOpen, 1, true)
  check(resolveDetails(opened, 1, false).open, "a reader's choice opens a block the writer closed")
  check(!resolveDetails(opened, 1, true).open, "and closes one they did not")
  check(!resolveDetails(opened, 2, false).open, "one opened block does not open its neighbour")

  val closedByReader = none.with(ChoiceKind.DetailsOpen, 3, true)
  check(resolveDetails(closedByReader, 3, false).open, "a reader can open a block the writer left closed")
  val writerOpen = none.with(ChoiceKind.DetailsOpen, 4, false)
  check(resolveDetails(writerOpen, 4, true).open, "the writer's open stands when the reader said nothing")
  check(resolveDetails(none, 5, true).open, "with no choice at all, the writer decides")
  // A reader can also close a block the writer opened, which is what makes a
  // recorded choice a flip rather than only an open.
  val readerClosed = none.with(ChoiceKind.DetailsOpen, 6, true)
  check(!resolveDetails(readerClosed, 6, true).open, "a reader can close a block the writer opened")

  // The choice sets are separate, so a code toggle cannot open a collapsible block.
  val codeOnly = none.with(ChoiceKind.CodeExpand, 1, true)
  check(!resolveDetails(codeOnly, 1, false).open, "a code expand does not open a details block")

  // The container-path rules, which is what the platform views call.
  // A block the writer left closed is hidden, which is the default: `<details>`
  // means "ask the reader" rather than "already open".
  check(insideHiddenDetails(listOf(details(2, false)), none), "a block the writer closed is hidden")
  check(!insideHiddenDetails(listOf(details(1, true)), none), "a block the writer opened is not hidden")
  check(insideHiddenDetails(listOf(details(1, true)), none.with(ChoiceKind.DetailsOpen, 1, true)),
    "a reader who closed the writer's open block hides it")
  check(
    !insideHiddenDetails(listOf(quote(), details(1, true)), none),
    "a quote around a visible body does not hide it",
  )
  check(
    insideHiddenDetails(listOf(details(1, true), details(2, false)), none),
    "the outer closed frame hides the inner block however open the inner one is",
  )
  check(isDetailsSummary(listOf(details(1, false, summary = true))), "the marked frame is the summary")
  check(!isDetailsSummary(listOf(details(1, false))), "a frame that is not the first leaf is not the summary")
  check(
    isDetailsSummary(listOf(details(1, false), details(2, false, summary = true))),
    "a nested summary is the inner frame's own first leaf",
  )
  check(
    isDetailsSummary(listOf(details(1, false, summary = true), details(2, false, summary = true))),
    "a block that opens both is still one summary, and the outer chevron is the one drawn",
  )

  // The height rule. Four lines of ten points each, from a paragraph with a blank
  // line between every block: the line starts are the block boundaries.
  val tops = intArrayOf(0, 10, 30, 50, 60)
  val starts = intArrayOf(0, 12, 30, 46)
  fun height(vararg hidden: IntArray) =
    visibleTextHeight(60, 4, { tops[it] }, { starts[it] }, hidden.map { HiddenRange(it[0], it[1]) })

  check(height() == 60, "nothing hidden is the whole height")
  check(height(intArrayOf(12, 30)) == 40, "a collapsed block takes its own lines")
  check(height(intArrayOf(0, 12)) == 50, "a leading block goes too")
  check(height(intArrayOf(46, 1000)) == 50, "a trailing block goes too")
  check(height(intArrayOf(12, 30), intArrayOf(46, 1000)) == 30, "two collapsed blocks both go")
  check(height(intArrayOf(12, 30), intArrayOf(13, 30)) == 40, "overlapping ranges count once")
  check(height(intArrayOf(12, 30)) == 40, "a range ending on a line start leaves it")
  check(height(intArrayOf(13, 14)) == 60, "a range inside one line leaves that line")
  check(height(intArrayOf(12, 14)) == 40, "and takes it once the line starts inside")
  val oneLine = intArrayOf(0, 20)
  check(visibleTextHeight(20, 1, { oneLine[it] }, { 0 }, listOf(HiddenRange(0, 5))) == 0,
    "a one-line segment hides its only line")
  check(visibleTextHeight(20, 0, { oneLine[it] }, { 0 }, listOf(HiddenRange(0, 5))) == 20,
    "no lines is the whole height")

  if (failures > 0) {
    println("details layout: $failures failing checks")
    exitProcess(1)
  }
  println("details layout checks passed")
}

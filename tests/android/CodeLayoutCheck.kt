package com.downstream

import kotlin.system.exitProcess

/**
 * The table in `testWrapChoices`, `testCodeBlockLayout`, `testGutterDigits`, and
 * `testCodeCardHeight` from `src/cpp/tests/test_export.cpp`, run against the
 * Kotlin mirror.
 *
 * `CodeLayout.kt` is the platform's copy of `downstream/CodeLayout.h`, so both
 * sides have to answer the same questions the same way. Running one table against
 * both is what proves they do, rather than a comment saying they should.
 *
 * Compiled and run by `pnpm test:kotlin`, which needs a Kotlin compiler and
 * therefore is not part of `pnpm test:native`.
 */
private var failures = 0

private fun check(condition: Boolean, label: String) {
  if (!condition) {
    println("FAIL $label")
    failures++
  }
}

private fun checkEq(actual: Int, expected: Int, label: String) =
  check(actual == expected, "$label: expected $expected, actual $actual")

private fun checkEqF(actual: Float, expected: Float, label: String) =
  check(actual == expected, "$label: expected $expected, actual $actual")

fun main() {
  val plain = FenceMeta()
  val hidden = FenceMeta(noLineNumbers = true)
  val tenth = FenceMeta(startLine = 10)
  val layout = CodeLayout()
  val noNumbers = CodeLayout(props = CodeLayoutProps(lineNumbers = false))
  val capped = CodeLayout(props = CodeLayoutProps(maxHeight = 400f))
  val expanded = CodeLayout(props = CodeLayoutProps(maxHeight = 400f), choices = CodeChoices(expanded = intArrayOf(1)))

  fun of(l: CodeLayout, o: Int, m: FenceMeta) = resolveCodeBlock(l, o, m)

  check(of(layout, 0, plain).lineNumbers, "numbers are on by default")
  check(!of(noNumbers, 0, plain).lineNumbers, "the prop turns numbers off")
  check(!of(noNumbers, 0, plain).lineNumbers, "the prop wins over a fence that asked for numbers")
  check(!of(layout, 0, hidden).lineNumbers, "noLineNumbers hides them for one block")
  check(of(layout, 1, plain).lineNumbers, "one block's meta does not affect its neighbour")
  checkEq(of(layout, 0, tenth).firstNumber, 10, "the fence's start line reaches the gutter")
  checkEq(of(layout, 0, plain).firstNumber, 1, "no start line means line one")
  checkEqF(of(capped, 0, plain).maxHeight, 400f, "the cap prop reaches the block")
  checkEqF(of(expanded, 1, plain).maxHeight, 0f, "an expanded block has no cap")
  checkEqF(of(expanded, 0, plain).maxHeight, 400f, "one expanded block does not cap its neighbour")
  checkEqF(of(capped, 0, plain).maxHeight, 400f, "an uncapped layout leaves the cap alone")
  check(of(expanded, 1, plain).expanded, "an expanded block says so, so it can collapse again")
  check(!of(expanded, 0, plain).expanded, "its neighbour is not expanded")
  val uncapped = CodeLayout(props = CodeLayoutProps(maxHeight = 0f))
  checkEqF(of(uncapped, 0, plain).maxHeight, 0f, "a zero cap is no cap")
  checkEqF(of(uncapped, 0, plain).maxHeight, of(expanded, 1, plain).maxHeight,
    "uncapped and expanded are the same value to the card")
  val staleExpand = CodeLayout(props = CodeLayoutProps(maxHeight = 0f), choices = CodeChoices(expanded = intArrayOf(0)))
  check(!of(staleExpand, 0, plain).expanded, "a block with no cap is never expanded")

  // Wrap: the whole table from `testWrapChoices`.
  val unwrapped = CodeLayout()
  val wrapped = CodeLayout(props = CodeLayoutProps(wrap = true))
  val mixedUnwrapped = CodeLayout(choices = CodeChoices(wrapToggles = intArrayOf(2)))
  val mixedWrapped = CodeLayout(props = CodeLayoutProps(wrap = true), choices = CodeChoices(wrapToggles = intArrayOf(2)))
  check(!of(unwrapped, 0, plain).wraps, "an untoggled block does not wrap when the prop is off")
  check(of(wrapped, 0, plain).wraps, "an untoggled block wraps when the prop is on")
  check(of(mixedUnwrapped, 2, plain).wraps, "a toggled block wraps when the prop is off")
  check(!of(mixedWrapped, 2, plain).wraps, "a toggled block stops wrapping when the prop is on")
  check(!of(mixedUnwrapped, 0, plain).wraps, "an untoggled neighbour still follows the prop being off")
  check(of(mixedWrapped, 0, plain).wraps, "an untoggled neighbour still follows the prop being on")
  check(!of(mixedUnwrapped, 1, plain).wraps, "a neighbour before the toggle is unaffected")
  check(!of(mixedUnwrapped, 3, plain).wraps, "a neighbour after the toggle is unaffected")

  // Toggling keeps each choice set ascending, which `hasChoice` searches.
  fun toggledList(values: IntArray, ordinal: Int) = CodeChoices.toggled(values, ordinal).toList()
  check(toggledList(IntArray(0), 3) == listOf(3), "a first choice is added to an empty set")
  check(toggledList(intArrayOf(2, 5), 0) == listOf(0, 2, 5), "a choice before the rest goes first")
  check(toggledList(intArrayOf(2, 5), 4) == listOf(2, 4, 5), "a choice between two goes between them")
  check(toggledList(intArrayOf(2, 5), 9) == listOf(2, 5, 9), "a choice after the rest goes last")
  check(toggledList(intArrayOf(2, 4, 5), 4) == listOf(2, 5), "toggling a choice again removes it")
  check(CodeChoices().copyWithExpand(1).expanded.contentEquals(intArrayOf(1)), "expand adds to an empty set")
  check(CodeChoices().copyWith(1).wrapToggles.contentEquals(intArrayOf(1)), "wrap adds to an empty set")

  checkEq(gutterDigits(1, 1), 2, "one line is at least two digits")
  checkEq(gutterDigits(1, 9), 2, "nine lines are two digits")
  checkEq(gutterDigits(1, 10), 2, "line ten is still two digits")
  checkEq(gutterDigits(1, 99), 2, "line 99 is two digits")
  checkEq(gutterDigits(1, 100), 3, "line 100 is three digits")
  checkEq(gutterDigits(1, 1000), 4, "line 1000 is four digits")
  checkEq(gutterDigits(998, 3), 4, "a start line past 999 widens the gutter")
  checkEq(gutterDigits(998, 1), 3, "a start line of 998 on its own is three digits")
  checkEq(gutterDigits(1000000, 1), 7, "the largest start line is seven digits")
  checkEq(gutterDigits(7, 0), 2, "an empty fence numbers its one line")
  checkEq(gutterDigits(7, 0), gutterDigits(7, 1), "an empty fence is one line")

  val block = CodeBlockLayout(wraps = false, lineNumbers = true, firstNumber = 1, maxHeight = 400f)
  val header = 32
  val padding = 10
  val line = 16
  checkEq(codeCardHeight(block, header, padding, 100, line), header + 100 + 2 * padding,
    "a body under the cap is header plus body plus padding")
  checkEq(codeCardHeight(block, header, padding, 1000, line), header + 400, "a body over the cap is capped")
  checkEq(codeCardHeight(block, 0, padding, 1000, line), 400, "no header means no header")
  val none = block.copy(maxHeight = 0f)
  checkEq(codeCardHeight(none, header, padding, 1000, line), header + 1000 + 2 * padding,
    "a zero cap grows with the body")
  val small = block.copy(maxHeight = 200f)
  checkEq(codeCardHeight(small, header, padding, 1000, 40), header + 5 * 40 + 2 * padding,
    "the floor is five lines plus padding")
  checkEq(codeCardHeight(block, header, padding, 100, line), header + 100 + 2 * padding,
    "the floor never grows a card past its body")
  checkEq(codeCardHeight(small, header, padding, 40, line), header + 40 + 2 * padding,
    "the floor does not pad a short block")

  // Offsets use Android's UTF-16 units, including surrogate pairs.
  val single = codeLineStarts("a")
  check(single.size == 1 && single[0] == 0, "one line starts at zero")
  val three = codeLineStarts("a\nb\nc")
  check(three.contentEquals(intArrayOf(0, 2, 4)), "a start after each newline")
  val trailing = codeLineStarts("a\n")
  check(trailing.contentEquals(intArrayOf(0, 2)), "a trailing newline starts a last empty line")
  val empty = codeLineStarts("")
  check(empty.size == 1 && empty[0] == 0, "an empty block has one line to number")
  check(codeLineStarts("\n\n\n").contentEquals(intArrayOf(0, 1, 2, 3)),
    "consecutive empty lines grow the primitive array")
  check(codeLineStarts("\ud83d\ude00\nnext\r\n").contentEquals(intArrayOf(0, 3, 9)),
    "surrogate pairs and CRLF preserve UTF-16 line starts")

  var reads = 0
  val manyLines = object : CharSequence {
    override val length = 100_000
    override fun get(index: Int): Char {
      reads++
      return '\n'
    }
    override fun subSequence(startIndex: Int, endIndex: Int): CharSequence =
      error("line indexing must not allocate substrings")
  }
  val dense = codeLineStarts(manyLines)
  checkEq(reads, manyLines.length, "line starts scan every code unit once")
  checkEq(dense.size, manyLines.length + 1, "dense newlines survive repeated array growth")
  check(dense.indices.all { dense[it] == it }, "dense line starts have no missing or extra entries")

  val wrappedStarts = intArrayOf(0, 12, 20)
  checkEq(codeLineForOffset(wrappedStarts, -1), 0, "negative offset stays on the first line")
  checkEq(codeLineForOffset(wrappedStarts, 0), 0, "first exact start selects the first line")
  checkEq(codeLineForOffset(wrappedStarts, 6), 0, "wrapped continuation keeps its logical line")
  checkEq(codeLineForOffset(wrappedStarts, 12), 1, "exact next start selects the next line")
  checkEq(codeLineForOffset(wrappedStarts, 19), 1, "last wrapped continuation keeps its logical line")
  checkEq(codeLineForOffset(wrappedStarts, 20), 2, "trailing blank start selects its own line")
  checkEq(codeLineForOffset(wrappedStarts, Int.MAX_VALUE), 2, "past the final line stays on it")
  checkEq(codeLineForOffset(empty, 0), 0, "an empty fence selects its single logical line")
  checkEq(codeLineForOffset(single, 0), 0, "single line indexing does not need a newline")
  checkEq(codeLineForOffset(dense, 99_999), 99_999, "deep scrolling selects the exact logical line")

  if (failures > 0) {
    println("$failures check(s) failed")
    exitProcess(1)
  }
  println("code layout checks passed")
}

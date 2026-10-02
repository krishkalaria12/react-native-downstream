package com.downstream

import kotlin.math.abs
import kotlin.system.exitProcess

/**
 * The table in `src/cpp/tests/test_math.cpp`, run against the Kotlin mirror.
 *
 * `MathLayout.kt` is the platform's copy of the sizing in
 * `src/cpp/math/MathLayout.h`, and a formula's height comes from one of these
 * three functions on either side, so both have to agree. Every case and every
 * expected number here is the case and the number in the C++ table, in the same
 * order.
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

/** The same tolerance `test_math.cpp` compares with. */
private fun near(actual: Float, expected: Float): Boolean {
  val scale = abs(expected)
  val magnitude = if (scale > 1f) scale else 1f
  return abs(actual - expected) <= 1e-3f * magnitude
}

private fun checkHeight(actual: Float, expected: Float, label: String) {
  if (!near(actual, expected)) {
    println("FAIL $label: expected $expected, actual $actual")
    failures++
  }
}

private fun checkWidth(actual: Float, expected: Float, label: String) = checkHeight(actual, expected, label)

fun main() {
  // Two layouts built by hand rather than through an engine, because this is about
  // the sizing of a layout and not about producing one. Width, height above the
  // baseline, and depth below it, the last two in em.
  val block = Triple(3f, 0.9f, 0.3f)
  val inline = Triple(2.5f, 0.8f, 0.2f)
  val (_, blockHeight, blockDepth) = block
  val (inlineWidth, inlineHeight, inlineDepth) = inline

  // ------------------------------------------------------------ the constants
  checkHeight(kInlineMathScale, 1.21f, "the inline scale is KaTeX's 1.21em")
  checkHeight(kMinInlineScale, 0.5f, "the floor is half size")

  // --------------------------------------------------------- the block height
  checkHeight(mathBlockHeight(blockHeight, blockDepth, 16f, 0f), 19.2f, "a block is height plus depth at the font size")
  checkHeight(mathBlockHeight(blockHeight, blockDepth, 16f, 8f), 35.2f, "padding is added on both sides")
  checkHeight(mathBlockHeight(0f, 0f, 0f, 0f), 0f, "no font and no padding is no height")

  checkHeight(mathBlockHeight(blockHeight, blockDepth, 32f, 0f), 38.4f, "the font size is the scaled one, not re-scaled here")
  checkHeight(mathBlockHeight(blockHeight, blockDepth, 24f, 0f), 28.8f, "1.5x the body is 1.5x the height")

  checkHeight(mathBlockHeight(1f, 0f, 16f, 0f), 16f, "a symbol with no depth is its own height")
  checkHeight(mathBlockHeight(0f, 0f, 16f, 4f), 8f, "an unresolved layout is just its padding")

  // -------------------------------------------------------- the inline metrics
  checkWidth(inlineMathWidth(inlineWidth, 16f), 40f, "an inline width is the advance at the run's size")
  checkWidth(inlineMathWidth(inlineWidth, 24f), 60f, "a larger run is a wider formula")

  checkHeight(inlineMathHeight(inlineHeight, inlineDepth, 16f), 16f, "an inline height is height plus depth")
  checkHeight(inlineMathHeight(inlineHeight, inlineDepth, 0f), 0f, "a zero size is a zero height")

  checkWidth(inlineMathWidth(inlineWidth, 16f), 40f, "a deeper formula is the same width")
  checkHeight(inlineMathHeight(inlineHeight, 5f, 16f), 92.8f, "a deeper formula is taller")

  if (failures > 0) {
    println("$failures check(s) failed")
    exitProcess(1)
  }
  println("math layout mirror: every case matches the C++ table")
}
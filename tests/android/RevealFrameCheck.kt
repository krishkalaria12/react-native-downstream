package com.downstream

import android.graphics.Path
import android.text.Layout
import android.view.animation.PathInterpolator
import kotlin.math.abs

private fun timing(duration: Double = 150.0, scale: Double = 1.0) =
  doubleArrayOf(2.0, 0.0, duration, 0.0, 0.0, 0.25, 0.1, 0.25, 1.0, scale)

fun main() {
  val ranges = longArrayOf(0, 0, 2, 0, 2, 4, 1, 0, 2, 10, 0, 2)
  val starts = doubleArrayOf(0.0, 0.0, 25.0, 50.0)
  val frame = RevealFrame(timing(), 3f, true)
  PathInterpolator.evaluations = 0
  check(frame.build(ranges, starts, 75.0))
  check(PathInterpolator.evaluations == 3) { "adjacent simultaneous units must share easing" }
  check(frame.size == 4)
  for (index in 0 until frame.size) {
    val unit = frame.unitAt(index)
    check(abs(unit.rise - (1f - unit.alpha) * 12f) < 1e-6f)
  }
  check(frame.firstUnitForBlock(0) == 0)
  check(frame.firstUnitForBlock(1) == 2)
  check(frame.firstUnitForBlock(2) == 3)
  check(frame.firstUnitForBlock(11) == 4)
  val revision = frame.revision
  frame.build(ranges, starts, 175.0)
  check(frame.revision != revision)
  check(frame.size == 1 && frame.unitAt(0).block == 10) { "finished JNI ranges must leave draw frame" }
  check(!frame.build(ranges, starts, 200.0))
  check(frame.firstUnitForBlock(0) == 0)
  PathInterpolator.evaluations = 0
  frame.build(ranges, starts, -1.0)
  check(frame.size == 4 && PathInterpolator.evaluations == 0) { "future units need no interpolation" }
  check(frame.unitAt(0).alpha == 0f && frame.unitAt(0).rise == 12f)
  check(!RevealFrame(timing(scale = 0.0), 1f, true).build(ranges, starts, -1.0))
  check(!RevealFrame(timing(duration = 0.0), 1f, true).build(ranges, starts, 0.0))
  check(!frame.build(longArrayOf(0, 0), doubleArrayOf(0.0), 0.0)) { "incomplete JNI ranges are ignored" }

  // y1 and x2 had the same coefficient in the old integer-only cache key.
  val firstCurve = RevealInterpolators.forPoints(0.25f, 0.1f, 0.25f, 1f)
  val secondCurve = RevealInterpolators.forPoints(0.25f, 0.25f, 0.1f, 1f)
  check(firstCurve !== secondCurve) { "distinct control points must not share a curve" }
  check(abs(firstCurve(0.5f) - secondCurve(0.5f)) > 0.01f)
  check(firstCurve === RevealInterpolators.forPoints(0.25f, 0.1f, 0.25f, 1f))

  val targets = RevealTargets()
  val segmentEnds = intArrayOf(1, 2, 6, 11)
  targets.build(ranges, segmentEnds)
  check(targets.size == 3)
  check((0 until targets.size).map(targets::indexAt) == listOf(0, 1, 3))
  // Targets read retained ranges even when every frame unit has completed.
  frame.build(ranges, starts, 200.0)
  check(frame.size == 0 && targets.size == 3) { "completion must still invalidate its roots" }
  targets.build(longArrayOf(), segmentEnds)
  check(targets.size == 0)
  targets.build(longArrayOf(5, 0, 2, 10, 0, 2), intArrayOf())
  check(targets.size == 0)
  val largeEnds = IntArray(10_000) { it + 1 }
  targets.build(longArrayOf(9_999, 0, 2, 9_999, 2, 4), largeEnds)
  check(targets.size == 1 && targets.indexAt(0) == 9_999) { "tail fade touches one of 10000 roots" }

  val band = RevealBand(4)
  band.begin(2)
  band.count = 2
  band.offsets[0] = 2; band.offsets[1] = 4
  band.offsets[2] = 8; band.offsets[3] = 10
  band.alphas[0] = 0.5f; band.alphas[1] = 0f
  band.rises[0] = 2f
  val clips = RevealClipPaths()
  val layout = Layout()
  val allocations = Path.constructions
  val mask = clips.build(layout, band, 3f, 7f)
  check(layout.selections == 2) { "one selection path per active unit" }
  check(mask == 1 shl alphaBucket(0.5f)) { "unstarted units produce no fading pass" }
  check(clips.hidden.rectangles.map { it.left to it.right } == listOf(7f to 9f, 13f to 15f))
  val opaque = (0 until 12).filter { char ->
    val x = 3f + layout.margin + char + 0.5f
    clips.hidden.rectangles.none { x >= it.left && x < it.right }
  }
  check(opaque == listOf(0, 1, 4, 5, 6, 7, 10, 11)) { "settled gaps and suffix must remain opaque" }
  check(clips.buckets[alphaBucket(0.5f) - 1].rectangles.size == 1)
  repeat(100) { clips.build(layout, band, 3f, 7f) }
  check(Path.constructions == allocations) { "redraw must reuse every path" }
  check(clips.hidden.rectangles.size == 2) { "old clips must not accumulate" }
  band.offsets[0] = 0; band.offsets[1] = 2
  clips.build(Layout(), band, 0f, 0f)
  check(clips.hidden.rectangles.any { it.left == 0f && it.right == 2f }) { "LTR marker margin fades with first word" }
  clips.build(Layout(rtl = true), band, 0f, 0f)
  check(clips.hidden.rectangles.any { it.left == 12f && it.right == 14f }) { "RTL marker margin fades with first word" }

  // Empty rule/fence blocks use U+200B in JNI display text and [0,1) units.
  // A glyph-width selection cannot fade their full-line decoration.
  band.count = 1
  band.offsets[0] = 0; band.offsets[1] = 1
  for (text in listOf("\u200B", "\u200B\n", "\u200B\uFFFC")) {
    val emptyLayout = Layout(lineLength = text.length, text = text)
    for (alpha in listOf(0f, 0.5f)) {
      band.alphas[0] = alpha
      val emptyMask = clips.build(emptyLayout, band, 0f, 0f)
      check(clips.hidden.rectangles.any { it.left == 0f && it.right == emptyLayout.width.toFloat() }) {
        "empty rule must leave opaque pass at alpha=$alpha"
      }
      if (alpha == 0f) check(emptyMask == 0) else {
        check(clips.buckets[alphaBucket(alpha) - 1].rectangles.any { it.left == 0f && it.right == emptyLayout.width.toFloat() }) {
          "empty rule's full width must be drawn by fading layer"
        }
      }
    }
  }
  clips.build(Layout(lineLength = 2, text = "\u200Bx"), band, 0f, 0f)
  check(clips.hidden.rectangles.none { it.left == 0f && it.right == 4f }) {
    "authored zero-width character beside ordinary text must not hide settled suffix"
  }
  println("reveal frame, target invalidation and drawing operations passed")
}

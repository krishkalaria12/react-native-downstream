package com.downstream

// Runs the Kotlin mirror's reveal table against the engine's own numbers.
//
// `RevealCurve.kt` is the per-frame half of the fade that has no platform
// dependency, and it is the half a host JVM can check. The curve on a device is the
// platform's `PathInterpolator`, which solves the same cubic Bézier; this file is
// what bounds the two, because the goldens come from `dump_reveal_curve` and so a
// drift between the engine and the drawing thread fails here rather than as a fade
// that is a few percent out.
//
// The cases arrive on stdin, one per line, as
// `x1 y1 x2 y2 durationMs offsetMs alpha rise`, which is what
// `revealCurveCases()` in `tests/engine.ts` reads out of the C++ binary. An empty
// stdin is a failure rather than a pass: a table of nothing would agree with
// nothing.

import kotlin.math.abs
import kotlin.system.exitProcess

private const val TOLERANCE = 0.002f

private var checked = 0
private val failures = mutableListOf<String>()

private fun check(condition: Boolean, message: String) {
  if (!condition) {
    failures.add(message)
  }
}

private fun oneCase(line: String) {
  val parts = line.trim().split(' ').map(String::toFloat)
  if (parts.size != 8) {
    failures.add("expected eight numbers, got ${parts.size}: $line")
    return
  }
  val points = floatArrayOf(parts[0], parts[1], parts[2], parts[3])
  val duration = parts[4]
  val offset = parts[5].toDouble()
  val expectedAlpha = parts[6]
  val expectedRise = parts[7]

  val alpha = RevealMath.alpha(0.0, duration, points, offset)
  check(abs(alpha - expectedAlpha) <= TOLERANCE, "alpha for $line: Kotlin $alpha, engine $expectedAlpha")
  // The engine's rise is in points, and the mirror's is scaled by a density, so the
  // check is at a density of one: the two must agree on the fraction travelled,
  // which is the part a device and a host can be wrong about independently.
  val rise = RevealMath.rise(0.0, duration, points, 1f, offset)
  check(abs(rise - expectedRise) <= TOLERANCE, "rise for $line: Kotlin $rise, engine $expectedRise")
  checked++
}

fun main() {
  // `android.jar` stubs the file and stream classes, so the cases are read through
  // the JVM's own streams rather than through `java.io.File`.
  val input = System.`in`.bufferedReader().readText()
  val lines = input.split('\n').map(String::trim).filter { it.isNotEmpty() }
  if (lines.isEmpty()) {
    println("no reveal cases arrived on stdin")
    exitProcess(1)
  }
  for (line in lines) {
    oneCase(line)
  }

  // The clamps the drawing pass relies on, which the table above already implies
  // but which are worth stating because a frame that got them wrong would be a
  // unit that never appears or one that never finishes.
  val ease = floatArrayOf(0.25f, 0.1f, 0.25f, 1f)
  check(RevealMath.alpha(100.0, 150f, ease, 0.0) == 0f, "a unit before its start is invisible")
  check(RevealMath.alpha(100.0, 150f, ease, 1_000.0) == 1f, "a unit after its fade is opaque")
  check(RevealMath.alpha(0.0, 0f, ease, 1.0) == 1f, "a zero duration shows at once rather than never")
  check(RevealMath.solve(0f, 0.25f, 0.1f, 0.25f, 1f) == 0f, "a curve starts at its start")
  check(RevealMath.solve(1f, 0.25f, 0.1f, 0.25f, 1f) == 1f, "a curve ends at its end")
  // `linear` is the identity, and CSS defines it that way rather than as a
  // degenerate cubic, so the solver has to answer it directly. The platform's
  // `PathInterpolator` is constructed from a `Path` for the same reason.
  val linear = floatArrayOf(0f, 0f, 1f, 1f)
  for (step in 1..9) {
    val progress = step / 10f
    check(
      abs(RevealMath.alpha(0.0, 1f, linear, progress.toDouble()) - progress) <= TOLERANCE,
      "linear at $progress is the identity",
    )
  }

  // The bucketing a draw pass reads: a unit at full alpha belongs to the first
  // bucket, one that has not started to none, and anything between to one of the
  // eight fading layers. A burst of forty alphas must fit in eight layers, or the
  // cap is not a cap, and an unstarted unit must not share a layer with one that
  // is almost shown.
  check(alphaBucket(1f) == 0, "a fully shown unit is the first bucket")
  check(alphaBucket(0f) == HIDDEN_BUCKET, "an unstarted unit is drawn by no layer")
  check(alphaBucket(0f) !in 0..MAX_ALPHA_BUCKETS, "the hidden bucket is outside every layer a pass draws")
  check(alphaBucket(0.5f) > 0 && alphaBucket(0.5f) < MAX_ALPHA_BUCKETS, "a half-faded unit is in the middle")
  check(
    alphaBucket(0.999f) == MAX_ALPHA_BUCKETS,
    "an almost shown unit is the last bucket, which is the one just below opaque",
  )
  for (bucket in 1..MAX_ALPHA_BUCKETS) {
    val stand = bucketAlpha(bucket)
    check(stand > 0f && stand < 1f, "bucket $bucket stands for an alpha between nothing and everything")
    check(alphaBucket(stand) == bucket, "bucket $bucket reads back as itself")
  }
  check(sameBucket(0.5f, 0.51f), "nearby alphas share a layer")
  check(!sameBucket(0.2f, 0.8f), "distant alphas do not")

  if (failures.isNotEmpty()) {
    for (failure in failures) {
      println("FAIL: $failure")
    }
    exitProcess(1)
  }
  println("reveal mirror agrees with the engine on $checked cases")
}

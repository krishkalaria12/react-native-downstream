package com.downstream

import android.view.animation.PathInterpolator

// Times actual production frame building with a host curve-operation recorder.
// JVM CPU timings do not measure Android drawing, GPU layers or device frame rate.
fun main() {
  val count = 512
  val ranges = LongArray(count * 3) {
    when (it % 3) {
      0 -> (it / 3 / 8).toLong()
      1 -> (it / 3 % 8 * 4).toLong()
      else -> (it / 3 % 8 * 4 + 4).toLong()
    }
  }
  for ((name, stagger, rise, now) in listOf(
    Scenario("rise_burst", 0.0, true, 75.0),
    Scenario("rise_staggered", 0.1, true, 75.0),
    Scenario("fade_staggered", 0.1, false, 75.0),
    Scenario("rise_completed_prefix", 12.0, true, 6_132.0),
  )) {
    val starts = DoubleArray(count) { it * stagger }
    val timing = doubleArrayOf(2.0, 0.0, 150.0, 0.0, 0.0, 0.25, 0.1, 0.25, 1.0, 1.0)
    val frame = RevealFrame(timing, 3f, rise)
    repeat(10_000) { frame.build(ranges, starts, now) }
    val samples = LongArray(7) {
      val start = System.nanoTime()
      repeat(20_000) { frame.build(ranges, starts, now) }
      (System.nanoTime() - start) / 20_000
    }
    PathInterpolator.evaluations = 0
    frame.build(ranges, starts, now)
    println("{\"scenario\":\"$name\",\"inputUnits\":$count,\"drawUnits\":${frame.size},\"curveCalls\":${PathInterpolator.evaluations},\"medianNs\":${samples.sorted()[3]},\"samplesNs\":[${samples.joinToString(",")}]}" )
  }
}

private data class Scenario(val name: String, val stagger: Double, val rise: Boolean, val now: Double)

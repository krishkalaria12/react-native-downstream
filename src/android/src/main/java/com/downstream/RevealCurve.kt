package com.downstream

import kotlin.math.abs

/**
 * The curve and the alpha, mirrored from `src/cpp/downstream/Reveal.h`.
 *
 * Everything that decides *what* fades and *when* it starts is the engine's, and
 * the engine is where the tests are. What is here is only the arithmetic a frame
 * needs, on the drawing thread, at 60 or 120 Hz: the eased progress of a unit and
 * how far a rising one has to travel.
 *
 * This half has no platform dependency on purpose. `android.jar` bundles the whole
 * `java.*` API, and a Kotlin compile against it loses the stdlib's operator
 * declarations, so the part that has to be checkable on a build machine is the part
 * that needs no jar. `tests/android/RevealCheck.kt` runs this against the engine's
 * own numbers; `Reveal.kt` is the platform layer over it.
 */
internal object RevealMath {
  /**
   * The progress a curve gives for an input progress of [t], by the same
   * Newton-then-bisection walk `CubicBezier::solve` uses.
   *
   * On a device this is the *mirror*: the curve that actually draws is the
   * platform's `PathInterpolator`, which solves the same cubic Bézier. The shared
   * case table bounds the two at 0.002, which is far below what an eighth of an
   * alpha step can show.
   */
  fun solve(t: Float, x1: Float, y1: Float, x2: Float, y2: Float): Float {
    if (t <= 0f) return 0f
    if (t >= 1f) return 1f
    // Newton converges in a couple of steps near the answer and costs a division
    // each time; bisection is the fallback where the derivative is too small to be
    // useful, which is exactly where a curve is flat.
    var u = t
    for (step in 0 until NEWTON_STEPS) {
      val error = sampleX(u, x1, x2) - t
      if (abs(error) < NEWTON_EPSILON) break
      val slope = sampleDX(u, x1, x2)
      if (abs(slope) < NEWTON_EPSILON) break
      u -= error / slope
    }
    if (u < 0f || u > 1f) {
      var low = 0f
      var high = 1f
      u = t
      for (step in 0 until BISECTION_STEPS) {
        val x = sampleX(u, x1, x2)
        if (abs(x - t) < NEWTON_EPSILON) break
        if (x < t) low = u else high = u
        u = (low + high) / 2f
      }
    }
    return sampleY(u, y1, y2)
  }

  /** The curve's x at parameter [u], which is what the solve inverts. */
  private fun sampleX(u: Float, x1: Float, x2: Float): Float {
    val v = 1f - u
    return 3f * v * v * u * x1 + 3f * v * u * u * x2 + u * u * u
  }

  /** The curve's y at parameter [u], which is the answer once x is found. */
  private fun sampleY(u: Float, y1: Float, y2: Float): Float {
    val v = 1f - u
    return 3f * v * v * u * y1 + 3f * v * u * u * y2 + u * u * u
  }

  private fun sampleDX(u: Float, x1: Float, x2: Float): Float {
    val v = 1f - u
    return 3f * v * v * x1 + 6f * v * u * (x2 - x1) + 3f * u * u * (1f - x2)
  }

  private const val NEWTON_STEPS = 8
  private const val BISECTION_STEPS = 32
  private const val NEWTON_EPSILON = 1e-6f

  /**
   * The input progress a unit is at at [now], before any curve is applied.
   *
   * Shared by both halves so a frame cannot disagree with a check about where in
   * its fade a unit is, which would show as a fade that starts late on one path
   * and not the other.
   */
  fun progress(startAt: Double, durationMs: Float, now: Double): Float {
    if (durationMs <= 0f) return 1f
    if (now <= startAt) return 0f
    val progress = ((now - startAt) / durationMs).toFloat()
    return if (progress >= 1f) 1f else progress
  }

  /** A unit's alpha: 0 before its start, eased progress during the fade, 1 after. */
  fun alpha(
    startAt: Double,
    durationMs: Float,
    points: FloatArray,
    now: Double,
    curve: (Float) -> Float = { solve(it, points[0], points[1], points[2], points[3]) },
  ): Float = curve(progress(startAt, durationMs, now))

  /**
   * How far a rising unit has to travel at [now], in the unit [density] is
   * measured in.
   *
   * The same four points the engine travels, so a `slideUp` moves the same
   * distance on both platforms for the same font size.
   */
  fun rise(
    startAt: Double,
    durationMs: Float,
    points: FloatArray,
    density: Float,
    now: Double,
    curve: (Float) -> Float = { solve(it, points[0], points[1], points[2], points[3]) },
  ): Float = (1f - alpha(startAt, durationMs, points, now, curve)) * RISE_POINTS * density

  /**
   * How far a `Rise` unit travels as it appears, in points. A placeholder, a
   * marker and a table row all travel the same distance, so a document's parts move
   * together, and it is small enough that a line's height never changes.
   */
  const val RISE_POINTS = 4f

  /** The same CSS `ease` the engine's default is, for a curve the array did not name. */
  const val EASE_X1 = 0.25f
  const val EASE_Y1 = 0.1f
  const val EASE_X2 = 0.25f
  const val EASE_Y2 = 1f
}

/**
 * How many alpha buckets one frame may draw in.
 *
 * A compressed burst can hold dozens of units at dozens of alphas, and a layer per
 * alpha would cost a layer per word. Bucketing bounds it, and the coarseness is
 * invisible: eight steps across a 150 ms fade is a fifth of an alpha step per
 * frame at 120 Hz.
 */
internal const val MAX_ALPHA_BUCKETS = 8

/** The bucket of a unit that has not started, which no pass draws. */
internal const val HIDDEN_BUCKET = -1

/**
 * The bucket a unit's alpha falls into: 0 for fully shown, `1..MAX_ALPHA_BUCKETS`
 * while fading, and [HIDDEN_BUCKET] before it starts.
 *
 * The first bucket is the fully shown one, so a caller that skips its own opaque
 * text can skip bucket 0 rather than comparing against 1. An unstarted unit is
 * clipped out of the opaque pass and drawn by no layer, so it stays invisible
 * rather than sharing a layer with a unit that is almost shown.
 */
internal fun alphaBucket(alpha: Float): Int {
  if (alpha >= 1f) return 0
  if (alpha <= 0f) return HIDDEN_BUCKET
  // Rounded rather than truncated, so the midpoint of the range lands in the
  // middle bucket and the ends are one bucket wide instead of two.
  val scaled = (alpha * MAX_ALPHA_BUCKETS).toInt() + 1
  return if (scaled >= MAX_ALPHA_BUCKETS) MAX_ALPHA_BUCKETS else scaled
}

/** The alpha a bucket stands for, which is what a layer is asked for. */
internal fun bucketAlpha(bucket: Int): Float =
  if (bucket <= 0) 1f else (bucket - 0.5f) / MAX_ALPHA_BUCKETS

/** Whether two alphas are close enough to share a layer. */
internal fun sameBucket(one: Float, other: Float): Boolean = abs(one - other) <= 1f / (MAX_ALPHA_BUCKETS * 2f)

package com.downstream

import android.view.animation.PathInterpolator

/**
 * The platform's curve, cached by its control points.
 *
 * On a device the curve is `PathInterpolator`, the same curve CSS defines, so
 * nothing here re-solves it per frame. A host JVM answers with the mirror in
 * `RevealCurve.kt`, because `android.jar`'s methods are stubs that throw;
 * `tests/android/RevealCheck.kt` is what bounds the two.
 */
internal object RevealInterpolators {
  private val cache = HashMap<Int, (Float) -> Float>(4)

  /**
   * Whether the platform's own solver answers here.
   *
   * Resolved once by asking it a question, rather than by testing the SDK level:
   * what decides this is whether the class is real, not how new it is.
   */
  private val platformSolver: Boolean by lazy {
    try {
      PathInterpolator(0f, 0f, 1f, 1f).getInterpolation(0.5f)
      true
    } catch (error: RuntimeException) {
      false
    }
  }

  /**
   * The curve for four control points, built once per distinct curve.
   *
   * A frame is at 60 or 120 Hz and the four numbers are the same every frame, so
   * building a curve per unit per frame would be the dominant cost of a fade.
   */
  fun forPoints(x1: Float, y1: Float, x2: Float, y2: Float): (Float) -> Float {
    val key = (x1.toBits() * 31 + y1.toBits()) * 31 * 31 + (x2.toBits() * 31 + y2.toBits()) * 31 + y2.toBits()
    cache[key]?.let { return it }
    val built = if (!platformSolver) {
      mirror(x1, y1, x2, y2)
    } else {
      try {
        // `PathInterpolator` rejects a control point outside the unit square, and
        // the engine clamps x for exactly this reason. y is clamped here too, so a
        // curve that overshoots vertically, which CSS allows, resolves rather than
        // throwing on a frame.
        val platform: PathInterpolator = PathInterpolator(
          x1.coerceIn(0f, 1f), y1.coerceIn(-1f, 2f),
          x2.coerceIn(0f, 1f), y2.coerceIn(-1f, 2f),
        )
        val evaluate: (Float) -> Float = { progress -> platform.getInterpolation(progress) }
        evaluate
      } catch (error: IllegalArgumentException) {
        mirror(RevealMath.EASE_X1, RevealMath.EASE_Y1, RevealMath.EASE_X2, RevealMath.EASE_Y2)
      }
    }
    if (cache.size >= 8) {
      // A themed app can name any number of curves over a long session, and the
      // table is small enough that dropping it beats growing it.
      cache.clear()
    }
    cache[key] = built
    return built
  }

  private fun mirror(x1: Float, y1: Float, x2: Float, y2: Float): (Float) -> Float {
    val evaluate: (Float) -> Float = { progress -> RevealMath.solve(progress, x1, y1, x2, y2) }
    return evaluate
  }
}

/**
 * One unit still fading, in the coordinates the drawing thread uses.
 *
 * A plain mutable holder rather than a class per unit: a burst of forty words is
 * forty of these, and allocating them per frame is exactly the cost a fade exists
 * to avoid.
 */
internal class RevealUnit {
  /** The document block, which is what a segment maps through its own blocks. */
  var block = 0

  /** UTF-16, into the block's text. */
  var start = 0
  var end = 0

  /** On the same monotonic clock the engine scheduled with. */
  var startAt = 0.0
  var alpha = 0f
  var rise = 0f
}

/**
 * The units one view is drawing right now.
 *
 * Held by the host view and read by every segment under it. The units are pooled
 * and reused, so a tick allocates nothing.
 */
internal class RevealFrame(
  timing: DoubleArray,
  private val density: Float,
  private val rise: Boolean,
) {
  private val pool = ArrayList<RevealUnit>(64)

  /** The per-unit duration in ms. The engine applies the animator scale itself. */
  private val durationMs = if (timing.size > TIMING_DURATION) timing[TIMING_DURATION].toFloat() else 150f

  /** The four control points, in the order the engine lists them. */
  private val easing = FloatArray(4).also {
    for (index in 0 until 4) {
      it[index] = if (timing.size > TIMING_EASING + index) {
        timing[TIMING_EASING + index].toFloat()
      } else {
        DEFAULT_EASING[index]
      }
    }
  }

  private val curve = RevealInterpolators.forPoints(easing[0], easing[1], easing[2], easing[3])

  /** How many units are fading, and the units themselves, in text order. */
  var size = 0
    private set

  fun unitAt(index: Int): RevealUnit = pool[index]

  /**
   * Rebuilds the frame from the engine's flat arrays, and answers whether anything
   * is left to draw.
   *
   * [ranges] is `[block, start, end]` per unit and [starts] holds each unit's start
   * time, both in the order the engine scheduled them, so the units come out in
   * text order within a block and a drawing pass can walk them forwards.
   */
  fun build(ranges: LongArray, starts: DoubleArray, now: Double): Boolean {
    size = 0
    val count = minOf(ranges.size / 3, starts.size)
    for (index in 0 until count) {
      if (size == pool.size) {
        pool.add(RevealUnit())
      }
      val unit = pool[size++]
      unit.block = ranges[index * 3].toInt()
      unit.start = ranges[index * 3 + 1].toInt()
      unit.end = ranges[index * 3 + 2].toInt()
      unit.startAt = starts[index]
      unit.alpha = RevealMath.alpha(unit.startAt, durationMs, easing, now, curve)
      unit.rise = if (rise) {
        RevealMath.rise(unit.startAt, durationMs, easing, density, now, curve)
      } else {
        0f
      }
    }
    return size > 0
  }

  private companion object {
    /** Where the duration sits in the flat timing array. */
    const val TIMING_DURATION = 2

    /** Where the curve's first control point sits. */
    const val TIMING_EASING = 5

    /** CSS `ease`, the array's default and the engine's. */
    val DEFAULT_EASING = floatArrayOf(0.25f, 0.1f, 0.25f, 1f)
  }
}

/**
 * The fading characters of one segment, in that segment's own text.
 *
 * The arrays are allocated once and reused, because a tick that allocated them
 * would be doing the very work the fade exists to avoid.
 */
internal class RevealBand(capacity: Int) {
  private var capacity = capacity.coerceAtLeast(4)

  /** The character range of each unit, as two entries per unit. */
  var offsets = IntArray(this.capacity * 2)
    private set

  /** Each unit's alpha, and how far it still has to rise. */
  var alphas = FloatArray(this.capacity)
    private set
  var rises = FloatArray(this.capacity)
    private set

  /** How many units are in use. */
  var count = 0

  /** The first character that is not fully shown, which is where the band starts. */
  val first: Int
    get() = offsets[0]

  /**
   * Makes room for [wanted] units, so a burst larger than the first frame fits
   * rather than being clipped.
   */
  fun reserve(wanted: Int) {
    if (wanted <= capacity) return
    val grown = wanted.coerceAtMost(MAX_BAND_UNITS)
    if (grown <= capacity) return
    capacity = grown
    offsets = IntArray(grown * 2)
    alphas = FloatArray(grown)
    rises = FloatArray(grown)
    count = 0
  }

  /**
   * The rise a bucket's layer is translated by.
   *
   * A bucket stands for one alpha, so its units share one rise to within a
   * rounding. The first unit in the bucket answers, which is the one with the
   * largest alpha and therefore the smallest remaining travel.
   */
  fun riseFor(bucket: Int): Float {
    for (index in 0 until count) {
      if (alphaBucket(alphas[index]) == bucket) return rises[index]
    }
    return 0f
  }

  /**
   * Orders the units by their starting character.
   *
   * The engine already schedules in text order, so this is an insertion sort over a
   * nearly sorted run: linear in the common case, and it moves nothing.
   */
  fun sortByOffset() {
    for (index in 1 until count) {
      val from = offsets[index * 2]
      val to = offsets[index * 2 + 1]
      val alpha = alphas[index]
      val rise = rises[index]
      var at = index
      while (at > 0 && offsets[(at - 1) * 2] > from) {
        offsets[at * 2] = offsets[(at - 1) * 2]
        offsets[at * 2 + 1] = offsets[(at - 1) * 2 + 1]
        alphas[at] = alphas[at - 1]
        rises[at] = rises[at - 1]
        at--
      }
      offsets[at * 2] = from
      offsets[at * 2 + 1] = to
      alphas[at] = alpha
      rises[at] = rise
    }
  }

  private companion object {
    /**
     * The most units one segment will draw in a frame.
     *
     * A document whose last chunk is larger than this is not one a reader is
     * watching word by word, and the cap is what keeps a frame's memory bounded
     * rather than proportional to the burst.
     */
    const val MAX_BAND_UNITS = 512
  }
}

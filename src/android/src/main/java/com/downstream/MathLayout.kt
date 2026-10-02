package com.downstream

/**
 * The Kotlin mirror of the sizing half of `src/cpp/math/MathLayout.h`.
 *
 * Only the metrics are mirrored, because only the metrics are needed to lay a
 * formula out: the glyphs and paths are the drawing package's business, and
 * carrying them across the bridge to answer "how tall is this" would move a lot of
 * numbers for three answers.
 *
 * Every function here is the mirror of an `inline` in that header, and the two
 * `k` constants are KaTeX's own values, which the engine copies. The table in
 * `tests/android/MathLayoutCheck.kt` runs the same cases both sides run, so a
 * change to one that is not a change to the other is a failing test rather than a
 * formula that is a few points taller on one platform.
 *
 * What is *not* mirrored is how a formula narrower than its column is scaled down,
 * because the header deliberately leaves that to each platform's draw pass: it is
 * a drawing decision about what fits, not a layout metric. What is shared is the
 * scale's floor, [kMinInlineScale], because "too small to read" is a rule both
 * platforms have to agree on.
 */

/** KaTeX's `.katex { font-size: 1.21em }`, applied by `inlineMathFont`. */
internal const val kInlineMathScale = 1.21f

/**
 * The smallest an inline formula may be scaled to before it is clipped instead.
 *
 * Past this a formula is unreadable, and clipping at the segment's edge is more
 * honest than drawing something at a third of its size that looks like a mistake.
 */
internal const val kMinInlineScale = 0.5f

/**
 * The height of one math block, in pixels. The mirror of the `inline`
 * `mathBlockHeight`.
 *
 * [height] and [depth] are the layout's own em units above and below the
 * baseline, [fontSize] is already scaled by the platform font scale, and
 * [padding] is the theme's vertical padding applied on both sides.
 */
internal fun mathBlockHeight(height: Float, depth: Float, fontSize: Float, padding: Float): Float =
  (height + depth) * fontSize + 2 * padding

/** The width of one inline formula, in pixels. */
internal fun inlineMathWidth(width: Float, fontSize: Float): Float = width * fontSize

/** The size one inline formula draws at, in pixels, above and below the baseline. */
internal fun inlineMathHeight(height: Float, depth: Float, fontSize: Float): Float =
  (height + depth) * fontSize
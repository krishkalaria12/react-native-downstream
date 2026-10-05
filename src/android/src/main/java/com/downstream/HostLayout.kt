package com.downstream

/**
 * The Kotlin mirror of `src/cpp/downstream/HostLayout.h`: which fenced blocks a
 * React child replaced, and how tall each child measured.
 *
 * The shadow node measures with the C++ type and the view draws with this one,
 * so both sides answer "is this block drawn by a component, and how tall is it"
 * the same way. It lives beside `ImageLayout.kt` rather than in `LayoutMaps.kt`
 * because it compiles without React Native on the classpath, which is what lets
 * `pnpm test:kotlin` run its table against the C++ one with nothing but a
 * Kotlin compiler.
 */

/** One hosted block: a fence an app's component draws instead of the card. */
internal data class HostEntry(
  /** The code block's ordinal among the document's code blocks. */
  val ordinal: Int,
  /** The height the component measured at, in points as the node measures. */
  val height: Float,
)

/**
 * Every React child this view mounts into a fence.
 *
 * Sorted by ordinal, exactly as the engine writes it, so [heightFor] can binary
 * search the way the C++ `heightFor` does. Empty means no child asked to be
 * hosted, which is the case for every view that registers no renderers.
 */
internal data class HostLayout(
  val entries: List<HostEntry> = emptyList(),
) {
  fun isEmpty(): Boolean = entries.isEmpty()

  /** The height for [ordinal], or 0 when this view hosts no child there. */
  fun heightFor(ordinal: Int): Float {
    var low = 0
    var high = entries.size - 1
    while (low <= high) {
      val middle = (low + high) / 2
      val entry = entries[middle]
      when {
        entry.ordinal < ordinal -> low = middle + 1
        entry.ordinal > ordinal -> high = middle - 1
        else -> return entry.height
      }
    }
    return 0f
  }

  /**
   * Whether the block at [ordinal] is drawn by a React child rather than by the
   * native code card. Both platforms ask this before building a card.
   *
   * Presence, not height, which is what `hosts` in the C++ header means too: a
   * component that renders nothing has a height of zero and still owns its
   * fence, so the card stays away because the app said so.
   */
  fun hosts(ordinal: Int): Boolean {
    var low = 0
    var high = entries.size - 1
    while (low <= high) {
      val middle = (low + high) / 2
      val entry = entries[middle]
      when {
        entry.ordinal < ordinal -> low = middle + 1
        entry.ordinal > ordinal -> high = middle - 1
        else -> return true
      }
    }
    return false
  }
}

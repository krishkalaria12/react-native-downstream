package com.downstream

/**
 * Per-URL link styles, as the view resolved them from `markdownStyle.linkVariants`.
 *
 * One object rather than four arrays because the segment builder asks one
 * question — "what styles this URL?" — and the answer should not require it to
 * know how the question is stored. Empty when the app named no variants, which
 * is the common case and costs nothing to ask.
 */
internal class LinkVariants(
  /** Every prefix flat in one array. */
  val prefixes: List<String> = emptyList(),
  /** Where one variant's prefixes end and the next begins. */
  val counts: List<Int> = emptyList(),
  /** A processed color per variant, or -1. */
  val colors: List<Int> = emptyList(),
  /** Whether each variant underlines. */
  val underlines: List<Boolean> = emptyList(),
) {
  fun isEmpty(): Boolean = prefixes.isEmpty()

  /**
   * The variant styling [url], or null.
   *
   * First prefix wins across the whole list, which is what makes ordering
   * meaningful: specific hosts before the scheme they share.
   */
  fun variantFor(url: String): Variant? {
    if (url.isEmpty() || prefixes.isEmpty()) return null
    var at = 0
    for (variant in counts.indices) {
      val count = counts[variant]
      for (index in 0 until count) {
        if (at + index < prefixes.size && url.startsWith(prefixes[at + index])) {
          return Variant(
            color = colors.getOrNull(variant)?.takeIf { it >= 0 },
            underline = underlines.getOrNull(variant),
          )
        }
      }
      at += count
    }
    return null
  }

  internal class Variant(val color: Int?, val underline: Boolean?)

  override fun equals(other: Any?): Boolean =
    other is LinkVariants && prefixes == other.prefixes && counts == other.counts &&
      colors == other.colors && underlines == other.underlines

  override fun hashCode(): Int {
    var hash = prefixes.hashCode()
    hash = 31 * hash + counts.hashCode()
    hash = 31 * hash + colors.hashCode()
    return 31 * hash + underlines.hashCode()
  }
}

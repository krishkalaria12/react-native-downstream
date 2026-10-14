package com.downstream

/**
 * Bounded append branches, rather than one latest value per appearance. Two
 * messages with the same font can advance independently without repeatedly
 * making each other's paragraph cold. Models contain immutable frame seeds.
 */
internal class StreamingParagraphSeeds<V>(
  private val maxEntries: Int,
  private val maxCost: Long,
  /** What a value holds per character of its text, in approximate bytes. */
  private val characterCost: Long = 24L,
) {
  private data class Entry<V>(val key: String, val text: String, val value: V, val cost: Long)
  private val entries = ArrayList<Entry<V>>()
  private var cost = 0L

  @Synchronized
  fun matching(key: String, text: String): V? {
    var best = -1
    for (index in entries.indices) {
      val candidate = entries[index]
      if (candidate.key == key && text.startsWith(candidate.text) &&
        (best < 0 || candidate.text.length > entries[best].text.length)
      ) best = index
    }
    if (best < 0) return null
    val found = entries.removeAt(best)
    entries.add(found)
    return found.value
  }

  @Synchronized
  fun put(key: String, text: String, value: V) {
    val iterator = entries.iterator()
    while (iterator.hasNext()) {
      val old = iterator.next()
      if (old.key == key && text.startsWith(old.text)) {
        cost -= old.cost
        iterator.remove()
      }
    }
    // Two exact width geometries, text storage, and line metadata. Oversized
    // candidates are not retained; views still own their current snapshot.
    val nextCost = text.length * characterCost + 1024L
    if (nextCost > maxCost) return
    while (entries.isNotEmpty() && (entries.size >= maxEntries || cost > maxCost - nextCost)) {
      cost -= entries.removeAt(0).cost
    }
    entries.add(Entry(key, text, value, nextCost))
    cost += nextCost
  }
}
